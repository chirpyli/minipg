/*-------------------------------------------------------------------------
 *
 * pgtz.c
 *	  Timezone Library Integration Functions
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/timezone/pgtz.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "datatype/timestamp.h"
#include "pgtz.h"
#include "utils/hsearch.h"


/* Current session timezone (controlled by TimeZone GUC) */
pg_tz	   *session_timezone = NULL;

/* Current log timezone (controlled by log_timezone GUC) */
pg_tz	   *log_timezone = NULL;


/*
 * minipg: built-in fixed-offset timezones.
 *
 * We do not ship the IANA timezone database, so there are no timezone files
 * to load.  Besides POSIX-style specifications, the only timezones available
 * are the fixed-offset zones: UTC and its aliases, plus the historical "Etc"
 * area, all of which are pure UTC offsets.
 *
 * Each entry maps a zone name to an equivalent POSIX TZ specification.  The
 * quoted abbreviation in that specification is the one the IANA timezone
 * compiler generates for the corresponding "Etc" zone, so tzparse() builds a
 * definition that is indistinguishable from the compiled one.  Note that the
 * specification must contain an offset: tzparse() rejects a bare abbreviation
 * such as "UTC".
 *
 * Names are matched case-insensitively (the lookup key is the upper-cased
 * input), and "name" is the canonical spelling reported for the zone.
 */
static const struct
{
	const char *name;			/* canonical zone name */
	const char *posixspec;		/* equivalent POSIX TZ specification */
}	builtin_fixed_zones[] =
{
	/* UTC and its historical aliases */
	{"UTC", "UTC0"},
	{"Etc/UTC", "UTC0"},
	{"Etc/UCT", "UTC0"},
	{"Etc/Universal", "UTC0"},
	{"Etc/Zulu", "UTC0"},

	/* GMT and its historical aliases */
	{"GMT", "GMT0"},
	{"Etc/GMT", "GMT0"},
	{"Etc/GMT+0", "GMT0"},
	{"Etc/GMT-0", "GMT0"},
	{"Etc/GMT0", "GMT0"},
	{"Etc/Greenwich", "GMT0"},
	{"GMT+0", "GMT0"},
	{"GMT-0", "GMT0"},
	{"GMT0", "GMT0"},
	{"Greenwich", "GMT0"},

	/* Etc/GMT-N: N hours east of Greenwich */
	{"Etc/GMT-1", "<+01>-1"},
	{"Etc/GMT-2", "<+02>-2"},
	{"Etc/GMT-3", "<+03>-3"},
	{"Etc/GMT-4", "<+04>-4"},
	{"Etc/GMT-5", "<+05>-5"},
	{"Etc/GMT-6", "<+06>-6"},
	{"Etc/GMT-7", "<+07>-7"},
	{"Etc/GMT-8", "<+08>-8"},
	{"Etc/GMT-9", "<+09>-9"},
	{"Etc/GMT-10", "<+10>-10"},
	{"Etc/GMT-11", "<+11>-11"},
	{"Etc/GMT-12", "<+12>-12"},
	{"Etc/GMT-13", "<+13>-13"},
	{"Etc/GMT-14", "<+14>-14"},

	/* Etc/GMT+N: N hours west of Greenwich */
	{"Etc/GMT+1", "<-01>1"},
	{"Etc/GMT+2", "<-02>2"},
	{"Etc/GMT+3", "<-03>3"},
	{"Etc/GMT+4", "<-04>4"},
	{"Etc/GMT+5", "<-05>5"},
	{"Etc/GMT+6", "<-06>6"},
	{"Etc/GMT+7", "<-07>7"},
	{"Etc/GMT+8", "<-08>8"},
	{"Etc/GMT+9", "<-09>9"},
	{"Etc/GMT+10", "<-10>10"},
	{"Etc/GMT+11", "<-11>11"},
	{"Etc/GMT+12", "<-12>12"},
};


/*
 * We keep loaded timezones in a hashtable so we don't have to
 * load and parse the TZ definition file every time one is selected.
 * Because we want timezone names to be found case-insensitively,
 * the hash key is the uppercased name of the zone.
 */
typedef struct
{
	/* tznameupper contains the all-upper-case name of the timezone */
	char		tznameupper[TZ_STRLEN_MAX + 1];
	pg_tz		tz;
} pg_tz_cache;

static HTAB *timezone_cache = NULL;


static bool
init_timezone_hashtable(void)
{
	HASHCTL		hash_ctl;

	hash_ctl.keysize = TZ_STRLEN_MAX + 1;
	hash_ctl.entrysize = sizeof(pg_tz_cache);

	timezone_cache = hash_create("Timezones",
								 4,
								 &hash_ctl,
								 HASH_ELEM | HASH_STRINGS);
	if (!timezone_cache)
		return false;

	return true;
}

/*
 * Load a timezone from cache, from the built-in fixed-offset table, or from a
 * POSIX-style specification.
 * Does not verify that the timezone is acceptable!
 *
 * "GMT" and the other built-in zones are handled without any reference to the
 * filesystem.  This has a number of benefits:
 * 1. "GMT" is guaranteed to succeed, so we don't have the failure mode wherein
 * the bootstrap default timezone setting doesn't work.
 * 2. Because we aren't accessing the filesystem, we can safely initialize
 * the "GMT" zone definition before my_exec_path is known.
 * 3. It's quick enough that we don't waste much time when the bootstrap
 * default timezone setting is later overridden from postgresql.conf.
 */
pg_tz *
pg_tzset(const char *name)
{
	pg_tz_cache *tzp;
	struct state tzstate;
	char		uppername[TZ_STRLEN_MAX + 1];
	const char *canonname = NULL;
	const char *posixspec = NULL;
	char	   *p;
	int			i;

	if (strlen(name) > TZ_STRLEN_MAX)
		return NULL;			/* not going to fit */

	if (!timezone_cache)
		if (!init_timezone_hashtable())
			return NULL;

	/*
	 * Upcase the given name to perform a case-insensitive hashtable search.
	 * (We could alternatively downcase it, but we prefer upcase so that we
	 * can get consistently upcased results from tzparse() in case the name is
	 * a POSIX-style timezone spec.)
	 */
	p = uppername;
	while (*name)
		*p++ = pg_toupper((unsigned char) *name++);
	*p = '\0';

	tzp = (pg_tz_cache *) hash_search(timezone_cache,
									  uppername,
									  HASH_FIND,
									  NULL);
	if (tzp)
	{
		/* Timezone found in cache, nothing more to do */
		return &tzp->tz;
	}

	/* Look for a built-in fixed-offset zone, case-insensitively. */
	for (i = 0; i < lengthof(builtin_fixed_zones); i++)
	{
		if (pg_strcasecmp(uppername, builtin_fixed_zones[i].name) == 0)
		{
			canonname = builtin_fixed_zones[i].name;
			posixspec = builtin_fixed_zones[i].posixspec;
			break;
		}
	}

	/*
	 * Otherwise, interpret the name as a POSIX-style timezone specification
	 * ("UTC-2", "<+08>-8", ...), for which the upper-cased name is canonical.
	 * A leading ":" requests a timezone file, which we cannot provide.
	 */
	if (posixspec == NULL)
	{
		if (uppername[0] == ':')
			return NULL;

		canonname = uppername;
		posixspec = uppername;
	}

	if (!tzparse(posixspec, &tzstate, false))
	{
		/* Unknown timezone. Fail our call instead of loading GMT! */
		return NULL;
	}

	/* Save timezone in the cache */
	tzp = (pg_tz_cache *) hash_search(timezone_cache,
									  uppername,
									  HASH_ENTER,
									  NULL);

	/* hash_search already copied uppername into the hash key */
	strcpy(tzp->tz.TZname, canonname);
	memcpy(&tzp->tz.state, &tzstate, sizeof(tzstate));

	return &tzp->tz;
}

/*
 * Load a fixed-GMT-offset timezone.
 * This is used for SQL-spec SET TIME ZONE INTERVAL 'foo' cases.
 * It's otherwise equivalent to pg_tzset().
 *
 * The GMT offset is specified in seconds, positive values meaning west of
 * Greenwich (ie, POSIX not ISO sign convention).  However, we use ISO
 * sign convention in the displayable abbreviation for the zone.
 *
 * Caution: this can fail (return NULL) if the specified offset is outside
 * the range allowed by tzparse().
 */
pg_tz *
pg_tzset_offset(long gmtoffset)
{
	long		absoffset = (gmtoffset < 0) ? -gmtoffset : gmtoffset;
	char		offsetstr[64];
	char		tzname[128];

	snprintf(offsetstr, sizeof(offsetstr),
			 "%02ld", absoffset / SECS_PER_HOUR);
	absoffset %= SECS_PER_HOUR;
	if (absoffset != 0)
	{
		snprintf(offsetstr + strlen(offsetstr),
				 sizeof(offsetstr) - strlen(offsetstr),
				 ":%02ld", absoffset / SECS_PER_MINUTE);
		absoffset %= SECS_PER_MINUTE;
		if (absoffset != 0)
			snprintf(offsetstr + strlen(offsetstr),
					 sizeof(offsetstr) - strlen(offsetstr),
					 ":%02ld", absoffset);
	}
	if (gmtoffset > 0)
		snprintf(tzname, sizeof(tzname), "<-%s>+%s",
				 offsetstr, offsetstr);
	else
		snprintf(tzname, sizeof(tzname), "<+%s>-%s",
				 offsetstr, offsetstr);

	return pg_tzset(tzname);
}


/*
 * Initialize timezone library
 *
 * This is called before GUC variable initialization begins.  Its purpose
 * is to ensure that log_timezone has a valid value before any logging GUC
 * variables could become set to values that require elog.c to provide
 * timestamps (e.g., log_line_prefix).  We may as well initialize
 * session_timezone to something valid, too.
 */
void
pg_timezone_initialize(void)
{
	/*
	 * We may not yet know where PGSHAREDIR is.  So use "GMT", which pg_tzset
	 * resolves from its built-in table without reference to the filesystem.
	 * This corresponds to the bootstrap default for these variables in
	 * guc.c, although in principle it could be different.
	 */
	session_timezone = pg_tzset("GMT");
	log_timezone = session_timezone;
}
