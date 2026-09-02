/*-------------------------------------------------------------------------
 *
 * timestamp.c
 *	  Functions for the built-in SQL types "timestamp" and "interval".
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/timestamp.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <math.h>
#include <limits.h>
#include <sys/time.h>

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "common/int.h"

#include "funcapi.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "parser/scansup.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/float.h"

/*
 * gcc's -ffast-math switch breaks routines that expect exact results from
 * expressions like timeval / SECS_PER_HOUR, where timeval is double.
 */
#ifdef __FAST_MATH__
#error -ffast-math is known to break this code
#endif

#define SAMESIGN(a,b)	(((a) < 0) == ((b) < 0))

/* Set at postmaster start */
TimestampTz PgStartTime;

/* Set at configuration reload */
TimestampTz PgReloadTime;

static TimeOffset time2t(const int hour, const int min, const int sec, const fsec_t fsec);
static Timestamp dt2local(Timestamp dt, int timezone);
static TimestampTz timestamp2timestamptz(Timestamp timestamp);
static Timestamp timestamptz2timestamp(TimestampTz timestamp);


/* common code for timestamptypmodin and timestamptztypmodin */
static int32
anytimestamp_typmodin(bool istz, ArrayType *ta)
{
	int32	   *tl;
	int			n;

	tl = ArrayGetIntegerTypmods(ta, &n);

	/*
	 * we're not too tense about good error message here because grammar
	 * shouldn't allow wrong number of modifiers for TIMESTAMP
	 */
	if (n != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid type modifier")));

	return anytimestamp_typmod_check(istz, tl[0]);
}

/* exported so parse_expr.c can use it */
int32
anytimestamp_typmod_check(bool istz, int32 typmod)
{
	if (typmod < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("TIMESTAMP(%d)%s precision must not be negative",
						typmod, (istz ? " WITH TIME ZONE" : ""))));
	if (typmod > MAX_TIMESTAMP_PRECISION)
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("TIMESTAMP(%d)%s precision reduced to maximum allowed, %d",
						typmod, (istz ? " WITH TIME ZONE" : ""),
						MAX_TIMESTAMP_PRECISION)));
		typmod = MAX_TIMESTAMP_PRECISION;
	}

	return typmod;
}

/* common code for timestamptypmodout and timestamptztypmodout */
static char *
anytimestamp_typmodout(bool istz, int32 typmod)
{
	const char *tz = istz ? " with time zone" : " without time zone";

	if (typmod >= 0)
		return psprintf("(%d)%s", (int) typmod, tz);
	else
		return psprintf("%s", tz);
}


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/* timestamp_in()
 * Convert a string to internal form.
 */
Datum
timestamp_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	Timestamp	result;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;
	int			tz;
	int			dtype;
	int			nf;
	int			dterr;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		workbuf[MAXDATELEN + MAXDATEFIELDS];

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf),
						  field, ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, &tz);
	if (dterr != 0)
		DateTimeParseError(dterr, str, "timestamp");

	switch (dtype)
	{
		case DTK_DATE:
			if (tm2timestamp(tm, fsec, NULL, &result) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range: \"%s\"", str)));
			break;

		case DTK_EPOCH:
			result = SetEpochTimestamp();
			break;

		case DTK_LATE:
			TIMESTAMP_NOEND(result);
			break;

		case DTK_EARLY:
			TIMESTAMP_NOBEGIN(result);
			break;

		default:
			elog(ERROR, "unexpected dtype %d while parsing timestamp \"%s\"",
				 dtype, str);
			TIMESTAMP_NOEND(result);
	}

	AdjustTimestampForTypmod(&result, typmod);

	PG_RETURN_TIMESTAMP(result);
}

/* timestamp_out()
 * Convert a timestamp to external form.
 */
Datum
timestamp_out(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	char	   *result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	char		buf[MAXDATELEN + 1];

	if (TIMESTAMP_NOT_FINITE(timestamp))
		EncodeSpecialTimestamp(timestamp, buf);
	else if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) == 0)
		EncodeDateTime(tm, fsec, false, 0, NULL, DateStyle, buf);
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/*
 *		timestamp_recv			- converts external binary format to timestamp
 */
Datum
timestamp_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	Timestamp	timestamp;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;

	timestamp = (Timestamp) pq_getmsgint64(buf);

	/* range check: see if timestamp_out would like it */
	if (TIMESTAMP_NOT_FINITE(timestamp))
		 /* ok */ ;
	else if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0 ||
			 !IS_VALID_TIMESTAMP(timestamp))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	AdjustTimestampForTypmod(&timestamp, typmod);

	PG_RETURN_TIMESTAMP(timestamp);
}

/*
 *		timestamp_send			- converts timestamp to binary format
 */
Datum
timestamp_send(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, timestamp);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
timestamptypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_INT32(anytimestamp_typmodin(false, ta));
}

Datum
timestamptypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);

	PG_RETURN_CSTRING(anytimestamp_typmodout(false, typmod));
}


/*
 * timestamp_support()
 *
 * Planner support function for the timestamp_scale() and timestamptz_scale()
 * length coercion functions (we need not distinguish them here).
 */
Datum
timestamp_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;

		ret = TemporalSimplify(MAX_TIMESTAMP_PRECISION, (Node *) req->fcall);
	}

	PG_RETURN_POINTER(ret);
}

/* timestamp_scale()
 * Adjust time type for specified scale factor.
 * Used by PostgreSQL type system to stuff columns.
 */
Datum
timestamp_scale(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	int32		typmod = PG_GETARG_INT32(1);
	Timestamp	result;

	result = timestamp;

	AdjustTimestampForTypmod(&result, typmod);

	PG_RETURN_TIMESTAMP(result);
}

/*
 * AdjustTimestampForTypmodError --- round off a timestamp to suit given typmod
 * Works for either timestamp or timestamptz.
 */
bool
AdjustTimestampForTypmodError(Timestamp *time, int32 typmod, bool *error)
{
	static const int64 TimestampScales[MAX_TIMESTAMP_PRECISION + 1] = {
		INT64CONST(1000000),
		INT64CONST(100000),
		INT64CONST(10000),
		INT64CONST(1000),
		INT64CONST(100),
		INT64CONST(10),
		INT64CONST(1)
	};

	static const int64 TimestampOffsets[MAX_TIMESTAMP_PRECISION + 1] = {
		INT64CONST(500000),
		INT64CONST(50000),
		INT64CONST(5000),
		INT64CONST(500),
		INT64CONST(50),
		INT64CONST(5),
		INT64CONST(0)
	};

	if (!TIMESTAMP_NOT_FINITE(*time)
		&& (typmod != -1) && (typmod != MAX_TIMESTAMP_PRECISION))
	{
		if (typmod < 0 || typmod > MAX_TIMESTAMP_PRECISION)
		{
			if (error)
			{
				*error = true;
				return false;
			}

			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("timestamp(%d) precision must be between %d and %d",
							typmod, 0, MAX_TIMESTAMP_PRECISION)));
		}

		if (*time >= INT64CONST(0))
		{
			*time = ((*time + TimestampOffsets[typmod]) / TimestampScales[typmod]) *
				TimestampScales[typmod];
		}
		else
		{
			*time = -((((-*time) + TimestampOffsets[typmod]) / TimestampScales[typmod])
					  * TimestampScales[typmod]);
		}
	}

	return true;
}

void
AdjustTimestampForTypmod(Timestamp *time, int32 typmod)
{
	(void) AdjustTimestampForTypmodError(time, typmod, NULL);
}

/* timestamptz_in()
 * Convert a string to internal form.
 */
Datum
timestamptz_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	TimestampTz result;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;
	int			tz;
	int			dtype;
	int			nf;
	int			dterr;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		workbuf[MAXDATELEN + MAXDATEFIELDS];

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf),
						  field, ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, &tz);
	if (dterr != 0)
		DateTimeParseError(dterr, str, "timestamp with time zone");

	switch (dtype)
	{
		case DTK_DATE:
			if (tm2timestamp(tm, fsec, &tz, &result) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range: \"%s\"", str)));
			break;

		case DTK_EPOCH:
			result = SetEpochTimestamp();
			break;

		case DTK_LATE:
			TIMESTAMP_NOEND(result);
			break;

		case DTK_EARLY:
			TIMESTAMP_NOBEGIN(result);
			break;

		default:
			elog(ERROR, "unexpected dtype %d while parsing timestamptz \"%s\"",
				 dtype, str);
			TIMESTAMP_NOEND(result);
	}

	AdjustTimestampForTypmod(&result, typmod);

	PG_RETURN_TIMESTAMPTZ(result);
}

/*
 * Try to parse a timezone specification, and return its timezone offset value
 * if it's acceptable.  Otherwise, an error is thrown.
 *
 * Note: some code paths update tm->tm_isdst, and some don't; current callers
 * don't care, so we don't bother being consistent.
 */
static int
parse_sane_timezone(struct pg_tm *tm, text *zone)
{
	char		tzname[TZ_STRLEN_MAX + 1];
	int			rt;
	int			tz;

	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	/*
	 * Look up the requested timezone.  First we try to interpret it as a
	 * numeric timezone specification; if DecodeTimezone decides it doesn't
	 * like the format, we look in the timezone abbreviation table (to handle
	 * cases like "EST"), and if that also fails, we look in the timezone
	 * database (to handle cases like "America/New_York").  (This matches the
	 * order in which timestamp input checks the cases; it's important because
	 * the timezone database unwisely uses a few zone names that are identical
	 * to offset abbreviations.)
	 *
	 * Note pg_tzset happily parses numeric input that DecodeTimezone would
	 * reject.  To avoid having it accept input that would otherwise be seen
	 * as invalid, it's enough to disallow having a digit in the first
	 * position of our input string.
	 */
	if (isdigit((unsigned char) *tzname))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"numeric time zone", tzname),
				 errhint("Numeric time zones must have \"-\" or \"+\" as first character.")));

	rt = DecodeTimezone(tzname, &tz);
	if (rt != 0)
	{
		char	   *lowzone;
		int			type,
					val;
		pg_tz	   *tzp;

		if (rt == DTERR_TZDISP_OVERFLOW)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("numeric time zone \"%s\" out of range", tzname)));
		else if (rt != DTERR_BAD_FORMAT)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("time zone \"%s\" not recognized", tzname)));

		/* DecodeTimezoneAbbrev requires lowercase input */
		lowzone = downcase_truncate_identifier(tzname,
											   strlen(tzname),
											   false);
		type = DecodeTimezoneAbbrev(0, lowzone, &val, &tzp);

		if (type == TZ || type == DTZ)
		{
			/* fixed-offset abbreviation */
			tz = -val;
		}
		else if (type == DYNTZ)
		{
			/* dynamic-offset abbreviation, resolve using specified time */
			tz = DetermineTimeZoneAbbrevOffset(tm, tzname, tzp);
		}
		else
		{
			/* try it as a full zone name */
			tzp = pg_tzset(tzname);
			if (tzp)
				tz = DetermineTimeZoneOffset(tm, tzp);
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("time zone \"%s\" not recognized", tzname)));
		}
	}

	return tz;
}

/*
 * make_timestamp_internal
 *		workhorse for make_timestamp and make_timestamptz
 */
static Timestamp
make_timestamp_internal(int year, int month, int day,
						int hour, int min, double sec)
{
	struct pg_tm tm;
	TimeOffset	date;
	TimeOffset	time;
	int			dterr;
	bool		bc = false;
	Timestamp	result;

	tm.tm_year = year;
	tm.tm_mon = month;
	tm.tm_mday = day;

	/* Handle negative years as BC */
	if (tm.tm_year < 0)
	{
		bc = true;
		tm.tm_year = -tm.tm_year;
	}

	dterr = ValidateDate(DTK_DATE_M, false, false, bc, &tm);

	if (dterr != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
				 errmsg("date field value out of range: %d-%02d-%02d",
						year, month, day)));

	if (!IS_VALID_JULIAN(tm.tm_year, tm.tm_mon, tm.tm_mday))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("date out of range: %d-%02d-%02d",
						year, month, day)));

	date = date2j(tm.tm_year, tm.tm_mon, tm.tm_mday) - POSTGRES_EPOCH_JDATE;

	/* Check for time overflow */
	if (float_time_overflows(hour, min, sec))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
				 errmsg("time field value out of range: %d:%02d:%02g",
						hour, min, sec)));

	/* This should match tm2time */
	time = (((hour * MINS_PER_HOUR + min) * SECS_PER_MINUTE)
			* USECS_PER_SEC) + (int64) rint(sec * USECS_PER_SEC);

	result = date * USECS_PER_DAY + time;
	/* check for major overflow */
	if ((result - time) / USECS_PER_DAY != date)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range: %d-%02d-%02d %d:%02d:%02g",
						year, month, day,
						hour, min, sec)));

	/* check for just-barely overflow (okay except time-of-day wraps) */
	/* caution: we want to allow 1999-12-31 24:00:00 */
	if ((result < 0 && date > 0) ||
		(result > 0 && date < -1))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range: %d-%02d-%02d %d:%02d:%02g",
						year, month, day,
						hour, min, sec)));

	/* final range check catches just-out-of-range timestamps */
	if (!IS_VALID_TIMESTAMP(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range: %d-%02d-%02d %d:%02d:%02g",
						year, month, day,
						hour, min, sec)));

	return result;
}

/*
 * make_timestamp() - timestamp constructor
 */
Datum
make_timestamp(PG_FUNCTION_ARGS)
{
	int32		year = PG_GETARG_INT32(0);
	int32		month = PG_GETARG_INT32(1);
	int32		mday = PG_GETARG_INT32(2);
	int32		hour = PG_GETARG_INT32(3);
	int32		min = PG_GETARG_INT32(4);
	float8		sec = PG_GETARG_FLOAT8(5);
	Timestamp	result;

	result = make_timestamp_internal(year, month, mday,
									 hour, min, sec);

	PG_RETURN_TIMESTAMP(result);
}

/*
 * make_timestamptz() - timestamp with time zone constructor
 */
Datum
make_timestamptz(PG_FUNCTION_ARGS)
{
	int32		year = PG_GETARG_INT32(0);
	int32		month = PG_GETARG_INT32(1);
	int32		mday = PG_GETARG_INT32(2);
	int32		hour = PG_GETARG_INT32(3);
	int32		min = PG_GETARG_INT32(4);
	float8		sec = PG_GETARG_FLOAT8(5);
	Timestamp	result;

	result = make_timestamp_internal(year, month, mday,
									 hour, min, sec);

	PG_RETURN_TIMESTAMPTZ(timestamp2timestamptz(result));
}

/*
 * Construct a timestamp with time zone.
 *		As above, but the time zone is specified as seventh argument.
 */
Datum
make_timestamptz_at_timezone(PG_FUNCTION_ARGS)
{
	int32		year = PG_GETARG_INT32(0);
	int32		month = PG_GETARG_INT32(1);
	int32		mday = PG_GETARG_INT32(2);
	int32		hour = PG_GETARG_INT32(3);
	int32		min = PG_GETARG_INT32(4);
	float8		sec = PG_GETARG_FLOAT8(5);
	text	   *zone = PG_GETARG_TEXT_PP(6);
	TimestampTz result;
	Timestamp	timestamp;
	struct pg_tm tt;
	int			tz;
	fsec_t		fsec;

	timestamp = make_timestamp_internal(year, month, mday,
										hour, min, sec);

	if (timestamp2tm(timestamp, NULL, &tt, &fsec, NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	tz = parse_sane_timezone(&tt, zone);

	result = dt2local(timestamp, -tz);

	if (!IS_VALID_TIMESTAMP(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_TIMESTAMPTZ(result);
}

/*
 * to_timestamp(double precision)
 * Convert UNIX epoch to timestamptz.
 */
Datum
float8_timestamptz(PG_FUNCTION_ARGS)
{
	float8		seconds = PG_GETARG_FLOAT8(0);
	TimestampTz result;

	/* Deal with NaN and infinite inputs ... */
	if (isnan(seconds))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp cannot be NaN")));

	if (isinf(seconds))
	{
		if (seconds < 0)
			TIMESTAMP_NOBEGIN(result);
		else
			TIMESTAMP_NOEND(result);
	}
	else
	{
		/* Out of range? */
		if (seconds <
			(float8) SECS_PER_DAY * (DATETIME_MIN_JULIAN - UNIX_EPOCH_JDATE)
			|| seconds >=
			(float8) SECS_PER_DAY * (TIMESTAMP_END_JULIAN - UNIX_EPOCH_JDATE))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range: \"%g\"", seconds)));

		/* Convert UNIX epoch to Postgres epoch */
		seconds -= ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);

		seconds = rint(seconds * USECS_PER_SEC);
		result = (int64) seconds;

		/* Recheck in case roundoff produces something just out of range */
		if (!IS_VALID_TIMESTAMP(result))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range: \"%g\"",
							PG_GETARG_FLOAT8(0))));
	}

	PG_RETURN_TIMESTAMP(result);
}

/* timestamptz_out()
 * Convert a timestamp to external form.
 */
Datum
timestamptz_out(PG_FUNCTION_ARGS)
{
	TimestampTz dt = PG_GETARG_TIMESTAMPTZ(0);
	char	   *result;
	int			tz;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	const char *tzn;
	char		buf[MAXDATELEN + 1];

	if (TIMESTAMP_NOT_FINITE(dt))
		EncodeSpecialTimestamp(dt, buf);
	else if (timestamp2tm(dt, &tz, tm, &fsec, &tzn, NULL) == 0)
		EncodeDateTime(tm, fsec, true, tz, tzn, DateStyle, buf);
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/*
 *		timestamptz_recv			- converts external binary format to timestamptz
 */
Datum
timestamptz_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	TimestampTz timestamp;
	int			tz;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;

	timestamp = (TimestampTz) pq_getmsgint64(buf);

	/* range check: see if timestamptz_out would like it */
	if (TIMESTAMP_NOT_FINITE(timestamp))
		 /* ok */ ;
	else if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0 ||
			 !IS_VALID_TIMESTAMP(timestamp))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	AdjustTimestampForTypmod(&timestamp, typmod);

	PG_RETURN_TIMESTAMPTZ(timestamp);
}

/*
 *		timestamptz_send			- converts timestamptz to binary format
 */
Datum
timestamptz_send(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, timestamp);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
timestamptztypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_INT32(anytimestamp_typmodin(true, ta));
}

Datum
timestamptztypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);

	PG_RETURN_CSTRING(anytimestamp_typmodout(true, typmod));
}


/* timestamptz_scale()
 * Adjust time type for specified scale factor.
 * Used by PostgreSQL type system to stuff columns.
 */
Datum
timestamptz_scale(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	int32		typmod = PG_GETARG_INT32(1);
	TimestampTz result;

	result = timestamp;

	AdjustTimestampForTypmod(&result, typmod);

	PG_RETURN_TIMESTAMPTZ(result);
}


/* EncodeSpecialTimestamp()
 * Convert reserved timestamp data type to string.
 */
void
EncodeSpecialTimestamp(Timestamp dt, char *str)
{
	if (TIMESTAMP_IS_NOBEGIN(dt))
		strcpy(str, EARLY);
	else if (TIMESTAMP_IS_NOEND(dt))
		strcpy(str, LATE);
	else						/* shouldn't happen */
		elog(ERROR, "invalid argument for EncodeSpecialTimestamp");
}

Datum
now(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(GetCurrentTransactionStartTimestamp());
}

Datum
statement_timestamp(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(GetCurrentStatementStartTimestamp());
}

Datum
clock_timestamp(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(GetCurrentTimestamp());
}

Datum
pg_postmaster_start_time(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(PgStartTime);
}

Datum
pg_conf_load_time(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(PgReloadTime);
}

/*
 * GetCurrentTimestamp -- get the current operating system time
 *
 * Result is in the form of a TimestampTz value, and is expressed to the
 * full precision of the gettimeofday() syscall
 */
TimestampTz
GetCurrentTimestamp(void)
{
	TimestampTz result;
	struct timeval tp;

	gettimeofday(&tp, NULL);

	result = (TimestampTz) tp.tv_sec -
		((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
	result = (result * USECS_PER_SEC) + tp.tv_usec;

	return result;
}

/*
 * GetSQLCurrentTimestamp -- implements CURRENT_TIMESTAMP, CURRENT_TIMESTAMP(n)
 */
TimestampTz
GetSQLCurrentTimestamp(int32 typmod)
{
	TimestampTz ts;

	ts = GetCurrentTransactionStartTimestamp();
	if (typmod >= 0)
		AdjustTimestampForTypmod(&ts, typmod);
	return ts;
}

/*
 * GetSQLLocalTimestamp -- implements LOCALTIMESTAMP, LOCALTIMESTAMP(n)
 */
Timestamp
GetSQLLocalTimestamp(int32 typmod)
{
	Timestamp	ts;

	ts = timestamptz2timestamp(GetCurrentTransactionStartTimestamp());
	if (typmod >= 0)
		AdjustTimestampForTypmod(&ts, typmod);
	return ts;
}

/*
 * timeofday(*) -- returns the current time as a text.
 */
Datum
timeofday(PG_FUNCTION_ARGS)
{
	struct timeval tp;
	pg_time_t	tt;
	struct pg_tm *tm;
	char		part1[128];
	char		part2[128];
	char		buf[128 + 128 + 10];

	gettimeofday(&tp, NULL);
	tt = (pg_time_t) tp.tv_sec;
	tm = pg_localtime(&tt, session_timezone);

	pg_strftime(part1, sizeof(part1), "%a %b %d %H:%M:%S", tm);
	pg_strftime(part2, sizeof(part2), "%Y %Z", tm);
	snprintf(buf, sizeof(buf), "%s.%06d %s", part1, (int) tp.tv_usec, part2);

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/*
 * TimestampDifference -- convert the difference between two timestamps
 *		into integer seconds and microseconds
 *
 * This is typically used to calculate a wait timeout for select(2),
 * which explains the otherwise-odd choice of output format.
 *
 * Both inputs must be ordinary finite timestamps (in current usage,
 * they'll be results from GetCurrentTimestamp()).
 *
 * We expect start_time <= stop_time.  If not, we return zeros,
 * since then we're already past the previously determined stop_time.
 */
void
TimestampDifference(TimestampTz start_time, TimestampTz stop_time,
					long *secs, int *microsecs)
{
	TimestampTz diff = stop_time - start_time;

	if (diff <= 0)
	{
		*secs = 0;
		*microsecs = 0;
	}
	else
	{
		*secs = (long) (diff / USECS_PER_SEC);
		*microsecs = (int) (diff % USECS_PER_SEC);
	}
}

/*
 * TimestampDifferenceMilliseconds -- convert the difference between two
 * 		timestamps into integer milliseconds
 *
 * This is typically used to calculate a wait timeout for WaitLatch()
 * or a related function.  The choice of "long" as the result type
 * is to harmonize with that.  It is caller's responsibility that the
 * input timestamps not be so far apart as to risk overflow of "long"
 * (which'd happen at about 25 days on machines with 32-bit "long").
 *
 * Both inputs must be ordinary finite timestamps (in current usage,
 * they'll be results from GetCurrentTimestamp()).
 *
 * We expect start_time <= stop_time.  If not, we return zero,
 * since then we're already past the previously determined stop_time.
 *
 * Note we round up any fractional millisecond, since waiting for just
 * less than the intended timeout is undesirable.
 */
long
TimestampDifferenceMilliseconds(TimestampTz start_time, TimestampTz stop_time)
{
	TimestampTz diff = stop_time - start_time;

	if (diff <= 0)
		return 0;
	else
		return (long) ((diff + 999) / 1000);
}

/*
 * TimestampDifferenceExceeds -- report whether the difference between two
 *		timestamps is >= a threshold (expressed in milliseconds)
 *
 * Both inputs must be ordinary finite timestamps (in current usage,
 * they'll be results from GetCurrentTimestamp()).
 */
bool
TimestampDifferenceExceeds(TimestampTz start_time,
						   TimestampTz stop_time,
						   int msec)
{
	TimestampTz diff = stop_time - start_time;

	return (diff >= msec * INT64CONST(1000));
}

/*
 * Convert a time_t to TimestampTz.
 *
 * We do not use time_t internally in Postgres, but this is provided for use
 * by functions that need to interpret, say, a stat(2) result.
 *
 * To avoid having the function's ABI vary depending on the width of time_t,
 * we declare the argument as pg_time_t, which is cast-compatible with
 * time_t but always 64 bits wide (unless the platform has no 64-bit type).
 * This detail should be invisible to callers, at least at source code level.
 */
TimestampTz
time_t_to_timestamptz(pg_time_t tm)
{
	TimestampTz result;

	result = (TimestampTz) tm -
		((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
	result *= USECS_PER_SEC;

	return result;
}

/*
 * Convert a TimestampTz to time_t.
 *
 * This too is just marginally useful, but some places need it.
 *
 * To avoid having the function's ABI vary depending on the width of time_t,
 * we declare the result as pg_time_t, which is cast-compatible with
 * time_t but always 64 bits wide (unless the platform has no 64-bit type).
 * This detail should be invisible to callers, at least at source code level.
 */
pg_time_t
timestamptz_to_time_t(TimestampTz t)
{
	pg_time_t	result;

	result = (pg_time_t) (t / USECS_PER_SEC +
						  ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY));

	return result;
}

/*
 * Produce a C-string representation of a TimestampTz.
 *
 * This is mostly for use in emitting messages.  The primary difference
 * from timestamptz_out is that we force the output format to ISO.  Note
 * also that the result is in a static buffer, not pstrdup'd.
 *
 * See also pg_strftime.
 */
const char *
timestamptz_to_str(TimestampTz t)
{
	static char buf[MAXDATELEN + 1];
	int			tz;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	const char *tzn;

	if (TIMESTAMP_NOT_FINITE(t))
		EncodeSpecialTimestamp(t, buf);
	else if (timestamp2tm(t, &tz, tm, &fsec, &tzn, NULL) == 0)
		EncodeDateTime(tm, fsec, true, tz, tzn, USE_ISO_DATES, buf);
	else
		strlcpy(buf, "(timestamp out of range)", sizeof(buf));

	return buf;
}


void
dt2time(Timestamp jd, int *hour, int *min, int *sec, fsec_t *fsec)
{
	TimeOffset	time;

	time = jd;

	*hour = time / USECS_PER_HOUR;
	time -= (*hour) * USECS_PER_HOUR;
	*min = time / USECS_PER_MINUTE;
	time -= (*min) * USECS_PER_MINUTE;
	*sec = time / USECS_PER_SEC;
	*fsec = time - (*sec * USECS_PER_SEC);
}								/* dt2time() */


/*
 * timestamp2tm() - Convert timestamp data type to POSIX time structure.
 *
 * Note that year is _not_ 1900-based, but is an explicit full value.
 * Also, month is one-based, _not_ zero-based.
 * Returns:
 *	 0 on success
 *	-1 on out of range
 *
 * If attimezone is NULL, the global timezone setting will be used.
 */
int
timestamp2tm(Timestamp dt, int *tzp, struct pg_tm *tm, fsec_t *fsec, const char **tzn, pg_tz *attimezone)
{
	Timestamp	date;
	Timestamp	time;
	pg_time_t	utime;

	/* Use session timezone if caller asks for default */
	if (attimezone == NULL)
		attimezone = session_timezone;

	time = dt;
	TMODULO(time, date, USECS_PER_DAY);

	if (time < INT64CONST(0))
	{
		time += USECS_PER_DAY;
		date -= 1;
	}

	/* add offset to go from J2000 back to standard Julian date */
	date += POSTGRES_EPOCH_JDATE;

	/* Julian day routine does not work for negative Julian days */
	if (date < 0 || date > (Timestamp) INT_MAX)
		return -1;

	j2date((int) date, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
	dt2time(time, &tm->tm_hour, &tm->tm_min, &tm->tm_sec, fsec);

	/* Done if no TZ conversion wanted */
	if (tzp == NULL)
	{
		tm->tm_isdst = -1;
		tm->tm_gmtoff = 0;
		tm->tm_zone = NULL;
		if (tzn != NULL)
			*tzn = NULL;
		return 0;
	}

	/*
	 * If the time falls within the range of pg_time_t, use pg_localtime() to
	 * rotate to the local time zone.
	 *
	 * First, convert to an integral timestamp, avoiding possibly
	 * platform-specific roundoff-in-wrong-direction errors, and adjust to
	 * Unix epoch.  Then see if we can convert to pg_time_t without loss. This
	 * coding avoids hardwiring any assumptions about the width of pg_time_t,
	 * so it should behave sanely on machines without int64.
	 */
	dt = (dt - *fsec) / USECS_PER_SEC +
		(POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY;
	utime = (pg_time_t) dt;
	if ((Timestamp) utime == dt)
	{
		struct pg_tm *tx = pg_localtime(&utime, attimezone);

		tm->tm_year = tx->tm_year + 1900;
		tm->tm_mon = tx->tm_mon + 1;
		tm->tm_mday = tx->tm_mday;
		tm->tm_hour = tx->tm_hour;
		tm->tm_min = tx->tm_min;
		tm->tm_sec = tx->tm_sec;
		tm->tm_isdst = tx->tm_isdst;
		tm->tm_gmtoff = tx->tm_gmtoff;
		tm->tm_zone = tx->tm_zone;
		*tzp = -tm->tm_gmtoff;
		if (tzn != NULL)
			*tzn = tm->tm_zone;
	}
	else
	{
		/*
		 * When out of range of pg_time_t, treat as GMT
		 */
		*tzp = 0;
		/* Mark this as *no* time zone available */
		tm->tm_isdst = -1;
		tm->tm_gmtoff = 0;
		tm->tm_zone = NULL;
		if (tzn != NULL)
			*tzn = NULL;
	}

	return 0;
}


/* tm2timestamp()
 * Convert a tm structure to a timestamp data type.
 * Note that year is _not_ 1900-based, but is an explicit full value.
 * Also, month is one-based, _not_ zero-based.
 *
 * Returns -1 on failure (value out of range).
 */
int
tm2timestamp(struct pg_tm *tm, fsec_t fsec, int *tzp, Timestamp *result)
{
	TimeOffset	date;
	TimeOffset	time;

	/* Prevent overflow in Julian-day routines */
	if (!IS_VALID_JULIAN(tm->tm_year, tm->tm_mon, tm->tm_mday))
	{
		*result = 0;			/* keep compiler quiet */
		return -1;
	}

	date = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - POSTGRES_EPOCH_JDATE;
	time = time2t(tm->tm_hour, tm->tm_min, tm->tm_sec, fsec);

	*result = date * USECS_PER_DAY + time;
	/* check for major overflow */
	if ((*result - time) / USECS_PER_DAY != date)
	{
		*result = 0;			/* keep compiler quiet */
		return -1;
	}
	/* check for just-barely overflow (okay except time-of-day wraps) */
	/* caution: we want to allow 1999-12-31 24:00:00 */
	if ((*result < 0 && date > 0) ||
		(*result > 0 && date < -1))
	{
		*result = 0;			/* keep compiler quiet */
		return -1;
	}
	if (tzp != NULL)
		*result = dt2local(*result, -(*tzp));

	/* final range check catches just-out-of-range timestamps */
	if (!IS_VALID_TIMESTAMP(*result))
	{
		*result = 0;			/* keep compiler quiet */
		return -1;
	}

	return 0;
}


static TimeOffset
time2t(const int hour, const int min, const int sec, const fsec_t fsec)
{
	return (((((hour * MINS_PER_HOUR) + min) * SECS_PER_MINUTE) + sec) * USECS_PER_SEC) + fsec;
}

static Timestamp
dt2local(Timestamp dt, int tz)
{
	dt -= (tz * USECS_PER_SEC);
	return dt;
}


/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/


Datum
timestamp_finite(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);

	PG_RETURN_BOOL(!TIMESTAMP_NOT_FINITE(timestamp));
}


/*----------------------------------------------------------
 *	Relational operators for timestamp.
 *---------------------------------------------------------*/

void
GetEpochTime(struct pg_tm *tm)
{
	struct pg_tm *t0;
	pg_time_t	epoch = 0;

	t0 = pg_gmtime(&epoch);

	if (t0 == NULL)
		elog(ERROR, "could not convert epoch to timestamp: %m");

	tm->tm_year = t0->tm_year;
	tm->tm_mon = t0->tm_mon;
	tm->tm_mday = t0->tm_mday;
	tm->tm_hour = t0->tm_hour;
	tm->tm_min = t0->tm_min;
	tm->tm_sec = t0->tm_sec;

	tm->tm_year += 1900;
	tm->tm_mon++;
}

Timestamp
SetEpochTimestamp(void)
{
	Timestamp	dt;
	struct pg_tm tt,
			   *tm = &tt;

	GetEpochTime(tm);
	/* we don't bother to test for failure ... */
	tm2timestamp(tm, 0, NULL, &dt);

	return dt;
}								/* SetEpochTimestamp() */

/*
 * We are currently sharing some code between timestamp and timestamptz.
 * The comparison functions are among them. - thomas 2001-09-25
 *
 *		timestamp_relop - is timestamp1 relop timestamp2
 */
int
timestamp_cmp_internal(Timestamp dt1, Timestamp dt2)
{
	return (dt1 < dt2) ? -1 : ((dt1 > dt2) ? 1 : 0);
}

Datum
timestamp_eq(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) == 0);
}

Datum
timestamp_ne(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) != 0);
}

Datum
timestamp_lt(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) < 0);
}

Datum
timestamp_gt(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) > 0);
}

Datum
timestamp_le(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) <= 0);
}

Datum
timestamp_ge(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) >= 0);
}

Datum
timestamp_cmp(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_INT32(timestamp_cmp_internal(dt1, dt2));
}

/* note: this is used for timestamptz also */
static int
timestamp_fastcmp(Datum x, Datum y, SortSupport ssup)
{
	Timestamp	a = DatumGetTimestamp(x);
	Timestamp	b = DatumGetTimestamp(y);

	return timestamp_cmp_internal(a, b);
}

Datum
timestamp_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = timestamp_fastcmp;
	PG_RETURN_VOID();
}

Datum
timestamp_hash(PG_FUNCTION_ARGS)
{
	return hashint8(fcinfo);
}

Datum
timestamp_hash_extended(PG_FUNCTION_ARGS)
{
	return hashint8extended(fcinfo);
}

/*
 * Cross-type comparison functions for timestamp vs timestamptz
 */

int32
timestamp_cmp_timestamptz_internal(Timestamp timestampVal, TimestampTz dt2)
{
	TimestampTz dt1;
	int			overflow;

	dt1 = timestamp2timestamptz_opt_overflow(timestampVal, &overflow);
	if (overflow > 0)
	{
		/* dt1 is larger than any finite timestamp, but less than infinity */
		return TIMESTAMP_IS_NOEND(dt2) ? -1 : +1;
	}
	if (overflow < 0)
	{
		/* dt1 is less than any finite timestamp, but more than -infinity */
		return TIMESTAMP_IS_NOBEGIN(dt2) ? +1 : -1;
	}

	return timestamptz_cmp_internal(dt1, dt2);
}

Datum
timestamp_eq_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) == 0);
}

Datum
timestamp_ne_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) != 0);
}

Datum
timestamp_lt_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) < 0);
}

Datum
timestamp_gt_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) > 0);
}

Datum
timestamp_le_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) <= 0);
}

Datum
timestamp_ge_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) >= 0);
}

Datum
timestamp_cmp_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_INT32(timestamp_cmp_timestamptz_internal(timestampVal, dt2));
}

Datum
timestamptz_eq_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) == 0);
}

Datum
timestamptz_ne_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) != 0);
}

Datum
timestamptz_lt_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) > 0);
}

Datum
timestamptz_gt_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) < 0);
}

Datum
timestamptz_le_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) >= 0);
}

Datum
timestamptz_ge_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) <= 0);
}

Datum
timestamptz_cmp_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_INT32(-timestamp_cmp_timestamptz_internal(timestampVal, dt1));
}


/*
 *		interval_relop	- is interval1 relop interval2
 *
 * Interval comparison is based on converting interval values to a linear
 * representation expressed in the units of the time field (microseconds,
 * in the case of integer timestamps) with days assumed to be always 24 hours
 * and months assumed to be always 30 days.  To avoid overflow, we need a
 * wider-than-int64 datatype for the linear representation, so use INT128.
 */

/*
 * Hashing for intervals
 *
 * We must produce equal hashvals for values that interval_cmp_internal()
 * considers equal.  So, compute the net span the same way it does,
 * and then hash that.
 */
/* overlaps_timestamp() --- implements the SQL OVERLAPS operator.
 *
 * Algorithm is per SQL spec.  This is much harder than you'd think
 * because the spec requires us to deliver a non-null answer in some cases
 * where some of the inputs are null.
 */
Datum
overlaps_timestamp(PG_FUNCTION_ARGS)
{
	/*
	 * The arguments are Timestamps, but we leave them as generic Datums to
	 * avoid unnecessary conversions between value and reference forms --- not
	 * to mention possible dereferences of null pointers.
	 */
	Datum		ts1 = PG_GETARG_DATUM(0);
	Datum		te1 = PG_GETARG_DATUM(1);
	Datum		ts2 = PG_GETARG_DATUM(2);
	Datum		te2 = PG_GETARG_DATUM(3);
	bool		ts1IsNull = PG_ARGISNULL(0);
	bool		te1IsNull = PG_ARGISNULL(1);
	bool		ts2IsNull = PG_ARGISNULL(2);
	bool		te2IsNull = PG_ARGISNULL(3);

#define TIMESTAMP_GT(t1,t2) \
	DatumGetBool(DirectFunctionCall2(timestamp_gt,t1,t2))
#define TIMESTAMP_LT(t1,t2) \
	DatumGetBool(DirectFunctionCall2(timestamp_lt,t1,t2))

	/*
	 * If both endpoints of interval 1 are null, the result is null (unknown).
	 * If just one endpoint is null, take ts1 as the non-null one. Otherwise,
	 * take ts1 as the lesser endpoint.
	 */
	if (ts1IsNull)
	{
		if (te1IsNull)
			PG_RETURN_NULL();
		/* swap null for non-null */
		ts1 = te1;
		te1IsNull = true;
	}
	else if (!te1IsNull)
	{
		if (TIMESTAMP_GT(ts1, te1))
		{
			Datum		tt = ts1;

			ts1 = te1;
			te1 = tt;
		}
	}

	/* Likewise for interval 2. */
	if (ts2IsNull)
	{
		if (te2IsNull)
			PG_RETURN_NULL();
		/* swap null for non-null */
		ts2 = te2;
		te2IsNull = true;
	}
	else if (!te2IsNull)
	{
		if (TIMESTAMP_GT(ts2, te2))
		{
			Datum		tt = ts2;

			ts2 = te2;
			te2 = tt;
		}
	}

	/*
	 * At this point neither ts1 nor ts2 is null, so we can consider three
	 * cases: ts1 > ts2, ts1 < ts2, ts1 = ts2
	 */
	if (TIMESTAMP_GT(ts1, ts2))
	{
		/*
		 * This case is ts1 < te2 OR te1 < te2, which may look redundant but
		 * in the presence of nulls it's not quite completely so.
		 */
		if (te2IsNull)
			PG_RETURN_NULL();
		if (TIMESTAMP_LT(ts1, te2))
			PG_RETURN_BOOL(true);
		if (te1IsNull)
			PG_RETURN_NULL();

		/*
		 * If te1 is not null then we had ts1 <= te1 above, and we just found
		 * ts1 >= te2, hence te1 >= te2.
		 */
		PG_RETURN_BOOL(false);
	}
	else if (TIMESTAMP_LT(ts1, ts2))
	{
		/* This case is ts2 < te1 OR te2 < te1 */
		if (te1IsNull)
			PG_RETURN_NULL();
		if (TIMESTAMP_LT(ts2, te1))
			PG_RETURN_BOOL(true);
		if (te2IsNull)
			PG_RETURN_NULL();

		/*
		 * If te2 is not null then we had ts2 <= te2 above, and we just found
		 * ts2 >= te1, hence te2 >= te1.
		 */
		PG_RETURN_BOOL(false);
	}
	else
	{
		/*
		 * For ts1 = ts2 the spec says te1 <> te2 OR te1 = te2, which is a
		 * rather silly way of saying "true if both are non-null, else null".
		 */
		if (te1IsNull || te2IsNull)
			PG_RETURN_NULL();
		PG_RETURN_BOOL(true);
	}

#undef TIMESTAMP_GT
#undef TIMESTAMP_LT
}


/*----------------------------------------------------------
 *	"Arithmetic" operators on date/times.
 *---------------------------------------------------------*/

Datum
timestamp_smaller(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);
	Timestamp	result;

	/* use timestamp_cmp_internal to be sure this agrees with comparisons */
	if (timestamp_cmp_internal(dt1, dt2) < 0)
		result = dt1;
	else
		result = dt2;
	PG_RETURN_TIMESTAMP(result);
}

Datum
timestamp_larger(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);
	Timestamp	result;

	if (timestamp_cmp_internal(dt1, dt2) > 0)
		result = dt1;
	else
		result = dt2;
	PG_RETURN_TIMESTAMP(result);
}


/*
 *	interval_justify_interval()
 *
 *	Adjust interval so 'month', 'day', and 'time' portions are within
 *	customary bounds.  Specifically:
 *
 *		0 <= abs(time) < 24 hours
 *		0 <= abs(day)  < 30 days
 *
 *	Also, the sign bit on all three fields is made equal, so either
 *	all three fields are negative or all are positive.
 */
/*
 *	interval_justify_hours()
 *
 *	Adjust interval so 'time' contains less than a whole day, adding
 *	the excess to 'day'.  This is useful for
 *	situations (such as non-TZ) where '1 day' = '24 hours' is valid,
 *	e.g. interval subtraction and division.
 */
/*
 *	interval_justify_days()
 *
 *	Adjust interval so 'day' contains less than 30 days, adding
 *	the excess to 'month'.
 */
/* timestamp_pl_interval()
 * Add an interval to a timestamp data type.
 * Note that interval has provisions for qualitative year/month and day
 *	units, so try to do the right thing with them.
 * To add a month, increment the month, and use the same day of month.
 * Then, if the next month has fewer days, set the day of month
 *	to the last day of month.
 * To add a day, increment the mday, and use the same time of day.
 * Lastly, add in the "quantitative time".
 */

/* timestamptz_pl_interval()
 * Add an interval to a timestamp with time zone data type.
 * Note that interval has provisions for qualitative year/month
 *	units, so try to do the right thing with them.
 * To add a month, increment the month, and use the same day of month.
 * Then, if the next month has fewer days, set the day of month
 *	to the last day of month.
 * Lastly, add in the "quantitative time".
 */


/*
 *	There is no interval_abs():  it is unclear what value to return:
 *	  http://archives.postgresql.org/pgsql-general/2009-10/msg01031.php
 *	  http://archives.postgresql.org/pgsql-general/2009-11/msg00041.php
 */


/*
 * in_range support functions for timestamps and intervals.
 *
 * Per SQL spec, we support these with interval as the offset type.
 * The spec's restriction that the offset not be negative is a bit hard to
 * decipher for intervals, but we choose to interpret it the same as our
 * interval comparison operators would.
 */


/*
 * interval_accum, interval_accum_inv, and interval_avg implement the
 * AVG(interval) aggregate.
 *
 * The transition datatype for this aggregate is a 2-element array of
 * intervals, where the first is the running sum and the second contains
 * the number of values so far in its 'time' field.  This is a bit ugly
 * but it beats inventing a specialized datatype for the purpose.
 */


/* timestamp_age()
 * Calculate time difference while retaining year/month fields.
 * Note that this does not result in an accurate absolute time span
 *	since year and month are out of context once the arithmetic
 *	is done.
 */

/* timestamptz_age()
 * Calculate time difference while retaining year/month fields.
 * Note that this does not result in an accurate absolute time span
 *	since year and month are out of context once the arithmetic
 *	is done.
 */

/*----------------------------------------------------------
 *	Conversion operators.
 *---------------------------------------------------------*/


/* timestamp_bin()
 * Bin timestamp into specified interval.
 */
/* timestamp_trunc()
 * Truncate timestamp to specified units.
 */
Datum
timestamp_trunc(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	Timestamp	result;
	int			type,
				val;
	char	   *lowunits;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMP(timestamp);

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_WEEK:
				{
					int			woy;

					woy = date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);

					/*
					 * If it is week 52/53 and the month is January, then the
					 * week must belong to the previous year. Also, some
					 * December dates belong to the next year.
					 */
					if (woy >= 52 && tm->tm_mon == 1)
						--tm->tm_year;
					if (woy <= 1 && tm->tm_mon == MONTHS_PER_YEAR)
						++tm->tm_year;
					isoweek2date(woy, &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
					tm->tm_hour = 0;
					tm->tm_min = 0;
					tm->tm_sec = 0;
					fsec = 0;
					break;
				}
			case DTK_MILLENNIUM:
				/* see comments in timestamptz_trunc */
				if (tm->tm_year > 0)
					tm->tm_year = ((tm->tm_year + 999) / 1000) * 1000 - 999;
				else
					tm->tm_year = -((999 - (tm->tm_year - 1)) / 1000) * 1000 + 1;
				/* FALL THRU */
			case DTK_CENTURY:
				/* see comments in timestamptz_trunc */
				if (tm->tm_year > 0)
					tm->tm_year = ((tm->tm_year + 99) / 100) * 100 - 99;
				else
					tm->tm_year = -((99 - (tm->tm_year - 1)) / 100) * 100 + 1;
				/* FALL THRU */
			case DTK_DECADE:
				/* see comments in timestamptz_trunc */
				if (val != DTK_MILLENNIUM && val != DTK_CENTURY)
				{
					if (tm->tm_year > 0)
						tm->tm_year = (tm->tm_year / 10) * 10;
					else
						tm->tm_year = -((8 - (tm->tm_year - 1)) / 10) * 10;
				}
				/* FALL THRU */
			case DTK_YEAR:
				tm->tm_mon = 1;
				/* FALL THRU */
			case DTK_QUARTER:
				tm->tm_mon = (3 * ((tm->tm_mon - 1) / 3)) + 1;
				/* FALL THRU */
			case DTK_MONTH:
				tm->tm_mday = 1;
				/* FALL THRU */
			case DTK_DAY:
				tm->tm_hour = 0;
				/* FALL THRU */
			case DTK_HOUR:
				tm->tm_min = 0;
				/* FALL THRU */
			case DTK_MINUTE:
				tm->tm_sec = 0;
				/* FALL THRU */
			case DTK_SECOND:
				fsec = 0;
				break;

			case DTK_MILLISEC:
				fsec = (fsec / 1000) * 1000;
				break;

			case DTK_MICROSEC:
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp units \"%s\" not supported",
								lowunits)));
				result = 0;
		}

		if (tm2timestamp(tm, fsec, NULL, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("timestamp units \"%s\" not recognized",
						lowunits)));
		result = 0;
	}

	PG_RETURN_TIMESTAMP(result);
}

/* timestamptz_bin()
 * Bin timestamptz into specified interval using specified origin.
 */
/*
 * Common code for timestamptz_trunc() and timestamptz_trunc_zone().
 *
 * tzp identifies the zone to truncate with respect to.  We assume
 * infinite timestamps have already been rejected.
 */
static TimestampTz
timestamptz_trunc_internal(text *units, TimestampTz timestamp, pg_tz *tzp)
{
	TimestampTz result;
	int			tz;
	int			type,
				val;
	bool		redotz = false;
	char	   *lowunits;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, tzp) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_WEEK:
				{
					int			woy;

					woy = date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);

					/*
					 * If it is week 52/53 and the month is January, then the
					 * week must belong to the previous year. Also, some
					 * December dates belong to the next year.
					 */
					if (woy >= 52 && tm->tm_mon == 1)
						--tm->tm_year;
					if (woy <= 1 && tm->tm_mon == MONTHS_PER_YEAR)
						++tm->tm_year;
					isoweek2date(woy, &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
					tm->tm_hour = 0;
					tm->tm_min = 0;
					tm->tm_sec = 0;
					fsec = 0;
					redotz = true;
					break;
				}
				/* one may consider DTK_THOUSAND and DTK_HUNDRED... */
			case DTK_MILLENNIUM:

				/*
				 * truncating to the millennium? what is this supposed to
				 * mean? let us put the first year of the millennium... i.e.
				 * -1000, 1, 1001, 2001...
				 */
				if (tm->tm_year > 0)
					tm->tm_year = ((tm->tm_year + 999) / 1000) * 1000 - 999;
				else
					tm->tm_year = -((999 - (tm->tm_year - 1)) / 1000) * 1000 + 1;
				/* FALL THRU */
			case DTK_CENTURY:
				/* truncating to the century? as above: -100, 1, 101... */
				if (tm->tm_year > 0)
					tm->tm_year = ((tm->tm_year + 99) / 100) * 100 - 99;
				else
					tm->tm_year = -((99 - (tm->tm_year - 1)) / 100) * 100 + 1;
				/* FALL THRU */
			case DTK_DECADE:

				/*
				 * truncating to the decade? first year of the decade. must
				 * not be applied if year was truncated before!
				 */
				if (val != DTK_MILLENNIUM && val != DTK_CENTURY)
				{
					if (tm->tm_year > 0)
						tm->tm_year = (tm->tm_year / 10) * 10;
					else
						tm->tm_year = -((8 - (tm->tm_year - 1)) / 10) * 10;
				}
				/* FALL THRU */
			case DTK_YEAR:
				tm->tm_mon = 1;
				/* FALL THRU */
			case DTK_QUARTER:
				tm->tm_mon = (3 * ((tm->tm_mon - 1) / 3)) + 1;
				/* FALL THRU */
			case DTK_MONTH:
				tm->tm_mday = 1;
				/* FALL THRU */
			case DTK_DAY:
				tm->tm_hour = 0;
				redotz = true;	/* for all cases >= DAY */
				/* FALL THRU */
			case DTK_HOUR:
				tm->tm_min = 0;
				/* FALL THRU */
			case DTK_MINUTE:
				tm->tm_sec = 0;
				/* FALL THRU */
			case DTK_SECOND:
				fsec = 0;
				break;
			case DTK_MILLISEC:
				fsec = (fsec / 1000) * 1000;
				break;
			case DTK_MICROSEC:
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp with time zone units \"%s\" not "
								"supported", lowunits)));
				result = 0;
		}

		if (redotz)
			tz = DetermineTimeZoneOffset(tm, tzp);

		if (tm2timestamp(tm, fsec, &tz, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("timestamp with time zone units \"%s\" not recognized",
						lowunits)));
		result = 0;
	}

	return result;
}

/* timestamptz_trunc()
 * Truncate timestamptz to specified units in session timezone.
 */
Datum
timestamptz_trunc(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	TimestampTz result;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMPTZ(timestamp);

	result = timestamptz_trunc_internal(units, timestamp, session_timezone);

	PG_RETURN_TIMESTAMPTZ(result);
}

/* timestamptz_trunc_zone()
 * Truncate timestamptz to specified units in specified timezone.
 */
Datum
timestamptz_trunc_zone(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	text	   *zone = PG_GETARG_TEXT_PP(2);
	TimestampTz result;
	char		tzname[TZ_STRLEN_MAX + 1];
	char	   *lowzone;
	int			type,
				val;
	pg_tz	   *tzp;

	/*
	 * timestamptz_zone() doesn't look up the zone for infinite inputs, so we
	 * don't do so here either.
	 */
	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMP(timestamp);

	/*
	 * Look up the requested timezone (see notes in timestamptz_zone()).
	 */
	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	/* DecodeTimezoneAbbrev requires lowercase input */
	lowzone = downcase_truncate_identifier(tzname,
										   strlen(tzname),
										   false);

	type = DecodeTimezoneAbbrev(0, lowzone, &val, &tzp);

	if (type == TZ || type == DTZ)
	{
		/* fixed-offset abbreviation, get a pg_tz descriptor for that */
		tzp = pg_tzset_offset(-val);
	}
	else if (type == DYNTZ)
	{
		/* dynamic-offset abbreviation, use its referenced timezone */
	}
	else
	{
		/* try it as a full zone name */
		tzp = pg_tzset(tzname);
		if (!tzp)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("time zone \"%s\" not recognized", tzname)));
	}

	result = timestamptz_trunc_internal(units, timestamp, tzp);

	PG_RETURN_TIMESTAMPTZ(result);
}

/* interval_trunc()
 * Extract specified field from interval.
 */
/* isoweek2j()
 *
 *	Return the Julian day which corresponds to the first day (Monday) of the given ISO 8601 year and week.
 *	Julian days are used to convert between ISO week dates and Gregorian dates.
 */
int
isoweek2j(int year, int week)
{
	int			day0,
				day4;

	/* fourth day of current year */
	day4 = date2j(year, 1, 4);

	/* day0 == offset to first day of week (Monday) */
	day0 = j2day(day4 - 1);

	return ((week - 1) * 7) + (day4 - day0);
}

/* isoweek2date()
 * Convert ISO week of year number to date.
 * The year field must be specified with the ISO year!
 * karel 2000/08/07
 */
void
isoweek2date(int woy, int *year, int *mon, int *mday)
{
	j2date(isoweek2j(*year, woy), year, mon, mday);
}

/* isoweekdate2date()
 *
 *	Convert an ISO 8601 week date (ISO year, ISO week) into a Gregorian date.
 *	Gregorian day of week sent so weekday strings can be supplied.
 *	Populates year, mon, and mday with the correct Gregorian values.
 *	year must be passed in as the ISO year.
 */
void
isoweekdate2date(int isoweek, int wday, int *year, int *mon, int *mday)
{
	int			jday;

	jday = isoweek2j(*year, isoweek);
	/* convert Gregorian week start (Sunday=1) to ISO week start (Monday=1) */
	if (wday > 1)
		jday += wday - 2;
	else
		jday += 6;
	j2date(jday, year, mon, mday);
}

/* date2isoweek()
 *
 *	Returns ISO week number of year.
 */
int
date2isoweek(int year, int mon, int mday)
{
	float8		result;
	int			day0,
				day4,
				dayn;

	/* current day */
	dayn = date2j(year, mon, mday);

	/* fourth day of current year */
	day4 = date2j(year, 1, 4);

	/* day0 == offset to first day of week (Monday) */
	day0 = j2day(day4 - 1);

	/*
	 * We need the first week containing a Thursday, otherwise this day falls
	 * into the previous year for purposes of counting weeks
	 */
	if (dayn < day4 - day0)
	{
		day4 = date2j(year - 1, 1, 4);

		/* day0 == offset to first day of week (Monday) */
		day0 = j2day(day4 - 1);
	}

	result = (dayn - (day4 - day0)) / 7 + 1;

	/*
	 * Sometimes the last few days in a year will fall into the first week of
	 * the next year, so check for this.
	 */
	if (result >= 52)
	{
		day4 = date2j(year + 1, 1, 4);

		/* day0 == offset to first day of week (Monday) */
		day0 = j2day(day4 - 1);

		if (dayn >= day4 - day0)
			result = (dayn - (day4 - day0)) / 7 + 1;
	}

	return (int) result;
}


/* date2isoyear()
 *
 *	Returns ISO 8601 year number.
 *	Note: zero or negative results follow the year-zero-exists convention.
 */
int
date2isoyear(int year, int mon, int mday)
{
	float8		result;
	int			day0,
				day4,
				dayn;

	/* current day */
	dayn = date2j(year, mon, mday);

	/* fourth day of current year */
	day4 = date2j(year, 1, 4);

	/* day0 == offset to first day of week (Monday) */
	day0 = j2day(day4 - 1);

	/*
	 * We need the first week containing a Thursday, otherwise this day falls
	 * into the previous year for purposes of counting weeks
	 */
	if (dayn < day4 - day0)
	{
		day4 = date2j(year - 1, 1, 4);

		/* day0 == offset to first day of week (Monday) */
		day0 = j2day(day4 - 1);

		year--;
	}

	result = (dayn - (day4 - day0)) / 7 + 1;

	/*
	 * Sometimes the last few days in a year will fall into the first week of
	 * the next year, so check for this.
	 */
	if (result >= 52)
	{
		day4 = date2j(year + 1, 1, 4);

		/* day0 == offset to first day of week (Monday) */
		day0 = j2day(day4 - 1);

		if (dayn >= day4 - day0)
			year++;
	}

	return year;
}


/* date2isoyearday()
 *
 *	Returns the ISO 8601 day-of-year, given a Gregorian year, month and day.
 *	Possible return values are 1 through 371 (364 in non-leap years).
 */
int
date2isoyearday(int year, int mon, int mday)
{
	return date2j(year, mon, mday) - isoweek2j(date2isoyear(year, mon, mday), 1) + 1;
}

/*
 * NonFiniteTimestampTzPart
 *
 *	Used by timestamp_part and timestamptz_part when extracting from infinite
 *	timestamp[tz].  Returns +/-Infinity if that is the appropriate result,
 *	otherwise returns zero (which should be taken as meaning to return NULL).
 *
 *	Errors thrown here for invalid units should exactly match those that
 *	would be thrown in the calling functions, else there will be unexpected
 *	discrepancies between finite- and infinite-input cases.
 */
static float8
NonFiniteTimestampTzPart(int type, int unit, char *lowunits,
						 bool isNegative, bool isTz)
{
	if ((type != UNITS) && (type != RESERV))
	{
		if (isTz)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("timestamp with time zone units \"%s\" not recognized",
							lowunits)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("timestamp units \"%s\" not recognized",
							lowunits)));
	}

	switch (unit)
	{
			/* Oscillating units */
		case DTK_MICROSEC:
		case DTK_MILLISEC:
		case DTK_SECOND:
		case DTK_MINUTE:
		case DTK_HOUR:
		case DTK_DAY:
		case DTK_MONTH:
		case DTK_QUARTER:
		case DTK_WEEK:
		case DTK_DOW:
		case DTK_ISODOW:
		case DTK_DOY:
		case DTK_TZ:
		case DTK_TZ_MINUTE:
		case DTK_TZ_HOUR:
			return 0.0;

			/* Monotonically-increasing units */
		case DTK_YEAR:
		case DTK_DECADE:
		case DTK_CENTURY:
		case DTK_MILLENNIUM:
		case DTK_JULIAN:
		case DTK_ISOYEAR:
		case DTK_EPOCH:
			if (isNegative)
				return -get_float8_infinity();
			else
				return get_float8_infinity();

		default:
			if (isTz)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp with time zone units \"%s\" not supported",
								lowunits)));
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp units \"%s\" not supported",
								lowunits)));
			return 0.0;			/* keep compiler quiet */
	}
}

/* timestamp_part() and extract_timestamp()
 * Extract specified field from timestamp.
 */
static Datum
timestamp_part_common(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	int64		intresult;
	Timestamp	epoch;
	int			type,
				val;
	char	   *lowunits;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (TIMESTAMP_NOT_FINITE(timestamp))
	{
		double		r = NonFiniteTimestampTzPart(type, val, lowunits,
												 TIMESTAMP_IS_NOBEGIN(timestamp),
												 false);

		if (r)
		{
			PG_RETURN_FLOAT8(r);
		}
		else
			PG_RETURN_NULL();
	}

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_MICROSEC:
				intresult = tm->tm_sec * INT64CONST(1000000) + fsec;
				break;

			case DTK_MILLISEC:
				PG_RETURN_FLOAT8(tm->tm_sec * 1000.0 + fsec / 1000.0);
				break;

			case DTK_SECOND:
				PG_RETURN_FLOAT8(tm->tm_sec + fsec / 1000000.0);
				break;

			case DTK_MINUTE:
				intresult = tm->tm_min;
				break;

			case DTK_HOUR:
				intresult = tm->tm_hour;
				break;

			case DTK_DAY:
				intresult = tm->tm_mday;
				break;

			case DTK_MONTH:
				intresult = tm->tm_mon;
				break;

			case DTK_QUARTER:
				intresult = (tm->tm_mon - 1) / 3 + 1;
				break;

			case DTK_WEEK:
				intresult = date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);
				break;

			case DTK_YEAR:
				if (tm->tm_year > 0)
					intresult = tm->tm_year;
				else
					/* there is no year 0, just 1 BC and 1 AD */
					intresult = tm->tm_year - 1;
				break;

			case DTK_DECADE:

				/*
				 * what is a decade wrt dates? let us assume that decade 199
				 * is 1990 thru 1999... decade 0 starts on year 1 BC, and -1
				 * is 11 BC thru 2 BC...
				 */
				if (tm->tm_year >= 0)
					intresult = tm->tm_year / 10;
				else
					intresult = -((8 - (tm->tm_year - 1)) / 10);
				break;

			case DTK_CENTURY:

				/* ----
				 * centuries AD, c>0: year in [ (c-1)* 100 + 1 : c*100 ]
				 * centuries BC, c<0: year in [ c*100 : (c+1) * 100 - 1]
				 * there is no number 0 century.
				 * ----
				 */
				if (tm->tm_year > 0)
					intresult = (tm->tm_year + 99) / 100;
				else
					/* caution: C division may have negative remainder */
					intresult = -((99 - (tm->tm_year - 1)) / 100);
				break;

			case DTK_MILLENNIUM:
				/* see comments above. */
				if (tm->tm_year > 0)
					intresult = (tm->tm_year + 999) / 1000;
				else
					intresult = -((999 - (tm->tm_year - 1)) / 1000);
				break;

			case DTK_JULIAN:
				PG_RETURN_FLOAT8(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) +
								 ((((tm->tm_hour * MINS_PER_HOUR) + tm->tm_min) * SECS_PER_MINUTE) +
								  tm->tm_sec + (fsec / 1000000.0)) / (double) SECS_PER_DAY);
				break;

			case DTK_ISOYEAR:
				intresult = date2isoyear(tm->tm_year, tm->tm_mon, tm->tm_mday);
				/* Adjust BC years */
				if (intresult <= 0)
					intresult -= 1;
				break;

			case DTK_DOW:
			case DTK_ISODOW:
				intresult = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (val == DTK_ISODOW && intresult == 0)
					intresult = 7;
				break;

			case DTK_DOY:
				intresult = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)
							 - date2j(tm->tm_year, 1, 1) + 1);
				break;

			case DTK_TZ:
			case DTK_TZ_MINUTE:
			case DTK_TZ_HOUR:
			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp units \"%s\" not supported",
								lowunits)));
				intresult = 0;
		}
	}
	else if (type == RESERV)
	{
		switch (val)
		{
			case DTK_EPOCH:
				epoch = SetEpochTimestamp();
				/* (timestamp - epoch) / 1000000 */
				{
					float8		result;

					/* try to avoid precision loss in subtraction */
					if (timestamp < (PG_INT64_MAX + epoch))
						result = (timestamp - epoch) / 1000000.0;
					else
						result = ((float8) timestamp - epoch) / 1000000.0;
					PG_RETURN_FLOAT8(result);
				}
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp units \"%s\" not supported",
								lowunits)));
				intresult = 0;
		}

	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("timestamp units \"%s\" not recognized", lowunits)));
		intresult = 0;
	}

	PG_RETURN_FLOAT8(intresult);
}

Datum
timestamp_part(PG_FUNCTION_ARGS)
{
	return timestamp_part_common(fcinfo);
}

Datum
extract_timestamp(PG_FUNCTION_ARGS)
{
	return timestamp_part_common(fcinfo);
}

/* timestamptz_part() and extract_timestamptz()
 * Extract specified field from timestamp with time zone.
 */
static Datum
timestamptz_part_common(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	int64		intresult;
	Timestamp	epoch;
	int			tz;
	int			type,
				val;
	char	   *lowunits;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (TIMESTAMP_NOT_FINITE(timestamp))
	{
		double		r = NonFiniteTimestampTzPart(type, val, lowunits,
												 TIMESTAMP_IS_NOBEGIN(timestamp),
												 true);

		if (r)
		{
			PG_RETURN_FLOAT8(r);
		}
		else
			PG_RETURN_NULL();
	}

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_TZ:
				intresult = -tz;
				break;

			case DTK_TZ_MINUTE:
				intresult = (-tz / SECS_PER_MINUTE) % MINS_PER_HOUR;
				break;

			case DTK_TZ_HOUR:
				intresult = -tz / SECS_PER_HOUR;
				break;

			case DTK_MICROSEC:
				intresult = tm->tm_sec * INT64CONST(1000000) + fsec;
				break;

			case DTK_MILLISEC:
				PG_RETURN_FLOAT8(tm->tm_sec * 1000.0 + fsec / 1000.0);
				break;

			case DTK_SECOND:
				PG_RETURN_FLOAT8(tm->tm_sec + fsec / 1000000.0);
				break;

			case DTK_MINUTE:
				intresult = tm->tm_min;
				break;

			case DTK_HOUR:
				intresult = tm->tm_hour;
				break;

			case DTK_DAY:
				intresult = tm->tm_mday;
				break;

			case DTK_MONTH:
				intresult = tm->tm_mon;
				break;

			case DTK_QUARTER:
				intresult = (tm->tm_mon - 1) / 3 + 1;
				break;

			case DTK_WEEK:
				intresult = date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);
				break;

			case DTK_YEAR:
				if (tm->tm_year > 0)
					intresult = tm->tm_year;
				else
					/* there is no year 0, just 1 BC and 1 AD */
					intresult = tm->tm_year - 1;
				break;

			case DTK_DECADE:
				/* see comments in timestamp_part */
				if (tm->tm_year > 0)
					intresult = tm->tm_year / 10;
				else
					intresult = -((8 - (tm->tm_year - 1)) / 10);
				break;

			case DTK_CENTURY:
				/* see comments in timestamp_part */
				if (tm->tm_year > 0)
					intresult = (tm->tm_year + 99) / 100;
				else
					intresult = -((99 - (tm->tm_year - 1)) / 100);
				break;

			case DTK_MILLENNIUM:
				/* see comments in timestamp_part */
				if (tm->tm_year > 0)
					intresult = (tm->tm_year + 999) / 1000;
				else
					intresult = -((999 - (tm->tm_year - 1)) / 1000);
				break;

			case DTK_JULIAN:
				PG_RETURN_FLOAT8(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) +
								 ((((tm->tm_hour * MINS_PER_HOUR) + tm->tm_min) * SECS_PER_MINUTE) +
								  tm->tm_sec + (fsec / 1000000.0)) / (double) SECS_PER_DAY);
				break;

			case DTK_ISOYEAR:
				intresult = date2isoyear(tm->tm_year, tm->tm_mon, tm->tm_mday);
				/* Adjust BC years */
				if (intresult <= 0)
					intresult -= 1;
				break;

			case DTK_DOW:
			case DTK_ISODOW:
				intresult = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (val == DTK_ISODOW && intresult == 0)
					intresult = 7;
				break;

			case DTK_DOY:
				intresult = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)
							 - date2j(tm->tm_year, 1, 1) + 1);
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp with time zone units \"%s\" not supported",
								lowunits)));
				intresult = 0;
		}

	}
	else if (type == RESERV)
	{
		switch (val)
		{
			case DTK_EPOCH:
				epoch = SetEpochTimestamp();
				/* (timestamp - epoch) / 1000000 */
				{
					float8		result;

					/* try to avoid precision loss in subtraction */
					if (timestamp < (PG_INT64_MAX + epoch))
						result = (timestamp - epoch) / 1000000.0;
					else
						result = ((float8) timestamp - epoch) / 1000000.0;
					PG_RETURN_FLOAT8(result);
				}
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp with time zone units \"%s\" not supported",
								lowunits)));
				intresult = 0;
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("timestamp with time zone units \"%s\" not recognized",
						lowunits)));

		intresult = 0;
	}

	PG_RETURN_FLOAT8(intresult);
}

Datum
timestamptz_part(PG_FUNCTION_ARGS)
{
	return timestamptz_part_common(fcinfo);
}

Datum
extract_timestamptz(PG_FUNCTION_ARGS)
{
	return timestamptz_part_common(fcinfo);
}



/*	timestamp_zone()
 *	Encode timestamp type with specified time zone.
 *	This function is just timestamp2timestamptz() except instead of
 *	shifting to the global timezone, we shift to the specified timezone.
 *	This is different from the other AT TIME ZONE cases because instead
 *	of shifting _to_ a new time zone, it sets the time to _be_ the
 *	specified timezone.
 */
Datum
timestamp_zone(PG_FUNCTION_ARGS)
{
	text	   *zone = PG_GETARG_TEXT_PP(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	TimestampTz result;
	int			tz;
	char		tzname[TZ_STRLEN_MAX + 1];
	char	   *lowzone;
	int			type,
				val;
	pg_tz	   *tzp;
	struct pg_tm tm;
	fsec_t		fsec;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMPTZ(timestamp);

	/*
	 * Look up the requested timezone.  First we look in the timezone
	 * abbreviation table (to handle cases like "EST"), and if that fails, we
	 * look in the timezone database (to handle cases like
	 * "America/New_York").  (This matches the order in which timestamp input
	 * checks the cases; it's important because the timezone database unwisely
	 * uses a few zone names that are identical to offset abbreviations.)
	 */
	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	/* DecodeTimezoneAbbrev requires lowercase input */
	lowzone = downcase_truncate_identifier(tzname,
										   strlen(tzname),
										   false);

	type = DecodeTimezoneAbbrev(0, lowzone, &val, &tzp);

	if (type == TZ || type == DTZ)
	{
		/* fixed-offset abbreviation */
		tz = val;
		result = dt2local(timestamp, tz);
	}
	else if (type == DYNTZ)
	{
		/* dynamic-offset abbreviation, resolve using specified time */
		if (timestamp2tm(timestamp, NULL, &tm, &fsec, NULL, tzp) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		tz = -DetermineTimeZoneAbbrevOffset(&tm, tzname, tzp);
		result = dt2local(timestamp, tz);
	}
	else
	{
		/* try it as a full zone name */
		tzp = pg_tzset(tzname);
		if (tzp)
		{
			/* Apply the timezone change */
			if (timestamp2tm(timestamp, NULL, &tm, &fsec, NULL, tzp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
			tz = DetermineTimeZoneOffset(&tm, tzp);
			if (tm2timestamp(&tm, fsec, &tz, &result) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("time zone \"%s\" not recognized", tzname)));
			result = 0;			/* keep compiler quiet */
		}
	}

	if (!IS_VALID_TIMESTAMP(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_TIMESTAMPTZ(result);
}

/* timestamp_izone()
 * Encode timestamp type with specified time interval as time zone.
 */
/* TimestampTimestampTzRequiresRewrite()
 *
 * Returns false if the TimeZone GUC setting causes timestamp_timestamptz and
 * timestamptz_timestamp to be no-ops, where the return value has the same
 * bits as the argument.  Since project convention is to assume a GUC changes
 * no more often than STABLE functions change, the answer is valid that long.
 */
bool
TimestampTimestampTzRequiresRewrite(void)
{
	long		offset;

	if (pg_get_timezone_offset(session_timezone, &offset) && offset == 0)
		return false;
	return true;
}

/* timestamp_timestamptz()
 * Convert local timestamp to timestamp at GMT
 */
Datum
timestamp_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);

	PG_RETURN_TIMESTAMPTZ(timestamp2timestamptz(timestamp));
}

/*
 * Convert timestamp to timestamp with time zone.
 *
 * On successful conversion, *overflow is set to zero if it's not NULL.
 *
 * If the timestamp is finite but out of the valid range for timestamptz, then:
 * if overflow is NULL, we throw an out-of-range error.
 * if overflow is not NULL, we store +1 or -1 there to indicate the sign
 * of the overflow, and return the appropriate timestamptz infinity.
 */
TimestampTz
timestamp2timestamptz_opt_overflow(Timestamp timestamp, int *overflow)
{
	TimestampTz result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;

	if (overflow)
		*overflow = 0;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		return timestamp;

	/* We don't expect this to fail, but check it pro forma */
	if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) == 0)
	{
		tz = DetermineTimeZoneOffset(tm, session_timezone);

		result = dt2local(timestamp, -tz);

		if (IS_VALID_TIMESTAMP(result))
		{
			return result;
		}
		else if (overflow)
		{
			if (result < MIN_TIMESTAMP)
			{
				*overflow = -1;
				TIMESTAMP_NOBEGIN(result);
			}
			else
			{
				*overflow = 1;
				TIMESTAMP_NOEND(result);
			}
			return result;
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
			 errmsg("timestamp out of range")));

	return 0;
}

/*
 * Promote timestamp to timestamptz, throwing error for overflow.
 */
static TimestampTz
timestamp2timestamptz(Timestamp timestamp)
{
	return timestamp2timestamptz_opt_overflow(timestamp, NULL);
}

/* timestamptz_timestamp()
 * Convert timestamp at GMT to local timestamp
 */
Datum
timestamptz_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);

	PG_RETURN_TIMESTAMP(timestamptz2timestamp(timestamp));
}

static Timestamp
timestamptz2timestamp(TimestampTz timestamp)
{
	Timestamp	result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		result = timestamp;
	else
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		if (tm2timestamp(tm, fsec, NULL, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}
	return result;
}

/* timestamptz_zone()
 * Evaluate timestamp with time zone type at the specified time zone.
 * Returns a timestamp without time zone.
 */
Datum
timestamptz_zone(PG_FUNCTION_ARGS)
{
	text	   *zone = PG_GETARG_TEXT_PP(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	Timestamp	result;
	int			tz;
	char		tzname[TZ_STRLEN_MAX + 1];
	char	   *lowzone;
	int			type,
				val;
	pg_tz	   *tzp;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMP(timestamp);

	/*
	 * Look up the requested timezone.  First we look in the timezone
	 * abbreviation table (to handle cases like "EST"), and if that fails, we
	 * look in the timezone database (to handle cases like
	 * "America/New_York").  (This matches the order in which timestamp input
	 * checks the cases; it's important because the timezone database unwisely
	 * uses a few zone names that are identical to offset abbreviations.)
	 */
	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	/* DecodeTimezoneAbbrev requires lowercase input */
	lowzone = downcase_truncate_identifier(tzname,
										   strlen(tzname),
										   false);

	type = DecodeTimezoneAbbrev(0, lowzone, &val, &tzp);

	if (type == TZ || type == DTZ)
	{
		/* fixed-offset abbreviation */
		tz = -val;
		result = dt2local(timestamp, tz);
	}
	else if (type == DYNTZ)
	{
		/* dynamic-offset abbreviation, resolve using specified time */
		int			isdst;

		tz = DetermineTimeZoneAbbrevOffsetTS(timestamp, tzname, tzp, &isdst);
		result = dt2local(timestamp, tz);
	}
	else
	{
		/* try it as a full zone name */
		tzp = pg_tzset(tzname);
		if (tzp)
		{
			/* Apply the timezone change */
			struct pg_tm tm;
			fsec_t		fsec;

			if (timestamp2tm(timestamp, &tz, &tm, &fsec, NULL, tzp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
			if (tm2timestamp(&tm, fsec, NULL, &result) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("time zone \"%s\" not recognized", tzname)));
			result = 0;			/* keep compiler quiet */
		}
	}

	if (!IS_VALID_TIMESTAMP(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_TIMESTAMP(result);
}

/* timestamptz_izone()
 * Encode timestamp with time zone type with specified time interval as time zone.
 * Returns a timestamp without time zone.
 */
/* generate_series_timestamp()
 * Generate the set of timestamps from start to finish by step
 */
/* generate_series_timestamptz()
 * Generate the set of timestamps from start to finish by step
 */