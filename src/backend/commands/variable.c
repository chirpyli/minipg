/*-------------------------------------------------------------------------
 *
 * variable.c
 *		Routines for handling specialized SET variables.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/variable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "commands/variable.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp_core.h"
#include "utils/varlena.h"


/*
 * TIMEZONE
 */

/*
 * check_timezone: GUC check_hook for timezone
 */
bool
check_timezone(char **newval, void **extra, GucSource source)
{
	pg_tz	   *new_tz;
	long		gmtoffset;
	char	   *endptr;
	double		hours;

	{
		/*
		 * Try it as a numeric number of hours (possibly fractional).
		 */
		hours = strtod(*newval, &endptr);
		if (endptr != *newval && *endptr == '\0')
		{
			/* Here we change from SQL to Unix sign convention */
			gmtoffset = -hours * SECS_PER_HOUR;
			new_tz = pg_tzset_offset(gmtoffset);
		}
		else
		{
			/*
			 * Otherwise assume it is a timezone name, and try to load it.
			 */
			new_tz = pg_tzset(*newval);

			if (!new_tz)
			{
				/* Doesn't seem to be any great value in errdetail here */
				return false;
			}

			if (!pg_tz_acceptable(new_tz))
			{
				GUC_check_errmsg("time zone \"%s\" appears to use leap seconds",
								 *newval);
				GUC_check_errdetail("PostgreSQL does not support leap seconds.");
				return false;
			}
		}
	}

	/* Test for failure in pg_tzset_offset, which we assume is out-of-range */
	if (!new_tz)
	{
		GUC_check_errdetail("UTC timezone offset is out of range.");
		return false;
	}

	/*
	 * Pass back data for assign_timezone to use
	 */
	*extra = malloc(sizeof(pg_tz *));
	if (!*extra)
		return false;
	*((pg_tz **) *extra) = new_tz;

	return true;
}

/*
 * assign_timezone: GUC assign_hook for timezone
 */
void
assign_timezone(const char *newval, void *extra)
{
	session_timezone = *((pg_tz **) extra);
}

/*
 * show_timezone: GUC show_hook for timezone
 */
const char *
show_timezone(void)
{
	const char *tzn;

	/* Always show the zone's canonical name */
	tzn = pg_get_timezone_name(session_timezone);

	if (tzn != NULL)
		return tzn;

	return "unknown";
}


/*
 * LOG_TIMEZONE
 *
 * For log_timezone, we don't support the interval-based methods of setting a
 * zone, which are only there for SQL spec compliance not because they're
 * actually useful.
 */

/*
 * check_log_timezone: GUC check_hook for log_timezone
 */
bool
check_log_timezone(char **newval, void **extra, GucSource source)
{
	pg_tz	   *new_tz;

	/*
	 * Assume it is a timezone name, and try to load it.
	 */
	new_tz = pg_tzset(*newval);

	if (!new_tz)
	{
		/* Doesn't seem to be any great value in errdetail here */
		return false;
	}

	if (!pg_tz_acceptable(new_tz))
	{
		GUC_check_errmsg("time zone \"%s\" appears to use leap seconds",
						 *newval);
		GUC_check_errdetail("PostgreSQL does not support leap seconds.");
		return false;
	}

	/*
	 * Pass back data for assign_log_timezone to use
	 */
	*extra = malloc(sizeof(pg_tz *));
	if (!*extra)
		return false;
	*((pg_tz **) *extra) = new_tz;

	return true;
}

/*
 * assign_log_timezone: GUC assign_hook for log_timezone
 */
void
assign_log_timezone(const char *newval, void *extra)
{
	log_timezone = *((pg_tz **) extra);
}

/*
 * show_log_timezone: GUC show_hook for log_timezone
 */
const char *
show_log_timezone(void)
{
	const char *tzn;

	/* Always show the zone's canonical name */
	tzn = pg_get_timezone_name(log_timezone);

	if (tzn != NULL)
		return tzn;

	return "unknown";
}


/*
 * SET TRANSACTION READ ONLY and SET TRANSACTION READ WRITE
 *
 * We allow idempotent changes (r/w -> r/w and r/o -> r/o) at any time, and
 * we also always allow changes from read-write to read-only.  However,
 * read-only may be changed to read-write only when in a top-level transaction
 * that has not yet taken an initial snapshot.  Can't do it in a hot standby,
 * either.
 *
 * If we are not in a transaction at all, just allow the change; it means
 * nothing since XactReadOnly will be reset by the next StartTransaction().
 * The IsTransactionState() test protects us against trying to check
 * RecoveryInProgress() in contexts where shared memory is not accessible.
 */
bool
check_transaction_read_only(bool *newval, void **extra, GucSource source)
{
	if (*newval == false && XactReadOnly && IsTransactionState())
	{
		/* Can't go to r/w mode inside a r/o transaction */
		if (IsSubTransaction())
		{
			GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
			GUC_check_errmsg("cannot set transaction read-write mode inside a read-only transaction");
			return false;
		}
		/* Top level transaction can't change to r/w after first snapshot. */
		if (FirstSnapshotSet)
		{
			GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
			GUC_check_errmsg("transaction read-write mode must be set before any query");
			return false;
		}
		/* Can't go to r/w mode while recovery is still active */
		if (RecoveryInProgress())
		{
			GUC_check_errcode(ERRCODE_FEATURE_NOT_SUPPORTED);
			GUC_check_errmsg("cannot set transaction read-write mode during recovery");
			return false;
		}
	}

	return true;
}

/*
 * SET TRANSACTION ISOLATION LEVEL
 *
 * We allow idempotent changes at any time, but otherwise this can only be
 * changed in a toplevel transaction that has not yet taken a snapshot.
 *
 * As in check_transaction_read_only, allow it if not inside a transaction.
 */
bool
check_XactIsoLevel(int *newval, void **extra, GucSource source)
{
	int			newXactIsoLevel = *newval;

	if (newXactIsoLevel != XactIsoLevel && IsTransactionState())
	{
		if (FirstSnapshotSet)
		{
			GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
			GUC_check_errmsg("SET TRANSACTION ISOLATION LEVEL must be called before any query");
			return false;
		}
		/* We ignore a subtransaction setting it to the existing value. */
		if (IsSubTransaction())
		{
			GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
			GUC_check_errmsg("SET TRANSACTION ISOLATION LEVEL must not be called in a subtransaction");
			return false;
		}
		/* Can't go to serializable mode while recovery is still active */
		if (newXactIsoLevel == XACT_SERIALIZABLE && RecoveryInProgress())
		{
			GUC_check_errcode(ERRCODE_FEATURE_NOT_SUPPORTED);
			GUC_check_errmsg("cannot use serializable mode in a hot standby");
			GUC_check_errhint("You can use REPEATABLE READ instead.");
			return false;
		}
	}

	return true;
}

/*
 * SET TRANSACTION [NOT] DEFERRABLE
 */

bool
check_transaction_deferrable(bool *newval, void **extra, GucSource source)
{
	if (IsSubTransaction())
	{
		GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
		GUC_check_errmsg("SET TRANSACTION [NOT] DEFERRABLE cannot be called within a subtransaction");
		return false;
	}
	if (FirstSnapshotSet)
	{
		GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
		GUC_check_errmsg("SET TRANSACTION [NOT] DEFERRABLE must be called before any query");
		return false;
	}

	return true;
}

/*
 * Random number seed
 *
 * We can't roll back the random sequence on error, and we don't want
 * config file reloads to affect it, so we only want interactive SET SEED
 * commands to set it.  We use the "extra" storage to ensure that rollbacks
 * don't try to do the operation again.
 */

bool
check_random_seed(double *newval, void **extra, GucSource source)
{
	*extra = malloc(sizeof(int));
	if (!*extra)
		return false;
	/* Arm the assign only if source of value is an interactive SET */
	*((int *) *extra) = (source >= PGC_S_INTERACTIVE);

	return true;
}

void
assign_random_seed(double newval, void *extra)
{
	/* We'll do this at most once for any setting of the GUC variable */
	if (*((int *) extra))
		DirectFunctionCall1(setseed, Float8GetDatum(newval));
	*((int *) extra) = 0;
}

const char *
show_random_seed(void)
{
	return "unavailable";
}


/*
 * SET CLIENT_ENCODING
 */

bool
check_client_encoding(char **newval, void **extra, GucSource source)
{
	int			encoding;
	const char *canonical_name;

	/* Look up the encoding by name */
	encoding = pg_valid_client_encoding(*newval);
	if (encoding < 0)
		return false;

	/* Get the canonical name (no aliases, uniform case) */
	canonical_name = pg_encoding_to_char(encoding);

	/*
	 * If we are not within a transaction then PrepareClientEncoding will not
	 * be able to look up the necessary conversion procs.  If we are still
	 * starting up, it will return "OK" anyway, and InitializeClientEncoding
	 * will fix things once initialization is far enough along.  After
	 * startup, we'll fail.  This would only happen if someone tries to change
	 * client_encoding in postgresql.conf and then SIGHUP existing sessions.
	 * It seems like a bad idea for client_encoding to change that way anyhow,
	 * so we don't go out of our way to support it.
	 *
	 * Note: in the postmaster, or any other process that never calls
	 * InitializeClientEncoding, PrepareClientEncoding will always succeed,
	 * and so will SetClientEncoding; but they won't do anything, which is OK.
	 */
	if (PrepareClientEncoding(encoding) < 0)
	{
		if (IsTransactionState())
		{
			/* Must be a genuine no-such-conversion problem */
			GUC_check_errcode(ERRCODE_FEATURE_NOT_SUPPORTED);
			GUC_check_errdetail("Conversion between %s and %s is not supported.",
								canonical_name,
								GetDatabaseEncodingName());
		}
		else
		{
			/* Provide a useful complaint */
			GUC_check_errdetail("Cannot change \"client_encoding\" now.");
		}
		return false;
	}

	/*
	 * Replace the user-supplied string with the encoding's canonical name.
	 * This gets rid of aliases and case-folding variations.
	 *
	 * XXX Although canonicalizing seems like a good idea in the abstract, it
	 * breaks pre-9.1 JDBC drivers, which expect that if they send "UNICODE"
	 * as the client_encoding setting then it will read back the same way. As
	 * a workaround, don't replace the string if it's "UNICODE".  Remove that
	 * hack when pre-9.1 JDBC drivers are no longer in use.
	 */
	if (strcmp(*newval, canonical_name) != 0 &&
		strcmp(*newval, "UNICODE") != 0)
	{
		free(*newval);
		*newval = strdup(canonical_name);
		if (!*newval)
			return false;
	}

	/*
	 * Save the encoding's ID in *extra, for use by assign_client_encoding.
	 */
	*extra = malloc(sizeof(int));
	if (!*extra)
		return false;
	*((int *) *extra) = encoding;

	return true;
}

void
assign_client_encoding(const char *newval, void *extra)
{
	int			encoding = *((int *) extra);

	/* We do not expect an error if PrepareClientEncoding succeeded */
	if (SetClientEncoding(encoding) < 0)
		elog(LOG, "SetClientEncoding(%d) failed", encoding);
}








