/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities
 *
 * Portions Copyright (c) 2002-2021, PostgreSQL Global Development Group
 *
 * src/backend/utils/adt/pg_locale.c
 *
 *-----------------------------------------------------------------------
 */

/*----------
 * Here is how the locale stuff is handled: LC_COLLATE and LC_CTYPE
 * are fixed at CREATE DATABASE time, stored in pg_database, and cannot
 * be changed. Thus, the effects of strcoll(), strxfrm(), isupper(),
 * toupper(), etc. are always in the same fixed locale.
 *
 * LC_MESSAGES is settable at run time and will take effect
 * immediately.
 *
 * The other categories, LC_MONETARY and LC_NUMERIC, are also
 * settable at run-time.  However, we don't actually set those locale
 * categories permanently.  This would have bizarre effects like no
 * longer accepting standard floating-point literals in some locales.
 * Instead, we only set these locale categories briefly when needed,
 * cache the required information obtained from localeconv(), and then
 * set the locale categories back to "C".
 * The cached information is only used by the money type.  For the
 * user, this should all be transparent.
 *
 * !!! NOW HEAR THIS !!!
 *
 * We've been bitten repeatedly by this bug, so let's try to keep it in
 * mind in future: on some platforms, the locale functions return pointers
 * to static data that will be overwritten by any later locale function.
 * Thus, for example, the obvious-looking sequence
 *			save = setlocale(category, NULL);
 *			if (!setlocale(category, value))
 *				fail = true;
 *			setlocale(category, save);
 * DOES NOT WORK RELIABLY: on some platforms the second setlocale() call
 * will change the memory save is pointing at.  To do this sort of thing
 * safely, you *must* pstrdup what setlocale returns the first time.
 *
 * The POSIX locale standard is available here:
 *
 *	http://www.opengroup.org/onlinepubs/009695399/basedefs/xbd_chap07.html
 *----------
 */


#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_control.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif


/* GUC settings */
char	   *locale_messages;
char	   *locale_monetary;
char	   *locale_numeric;

/* indicates whether locale information cache is valid */
static bool CurrentLocaleConvValid = false;

/* Cache for collation-related knowledge */

typedef struct
{
	Oid			collid;			/* hash key: pg_collation OID */
	bool		collate_is_c;	/* is collation's LC_COLLATE C? */
	bool		ctype_is_c;		/* is collation's LC_CTYPE C? */
	bool		flags_valid;	/* true if above flags are valid */
	pg_locale_t locale;			/* locale_t struct, or 0 if not valid */
} collation_cache_entry;

static HTAB *collation_cache = NULL;



/*
 * pg_perm_setlocale
 *
 * This wraps the libc function setlocale(), with two additions.  First, when
 * changing LC_CTYPE, update gettext's encoding for the current message
 * domain.  GNU gettext automatically tracks LC_CTYPE on most platforms, but
 * not on Windows.  Second, if the operation is successful, the corresponding
 * LC_XXX environment variable is set to match.  By setting the environment
 * variable, we ensure that any subsequent use of setlocale(..., "") will
 * preserve the settings made through this routine.  Of course, LC_ALL must
 * also be unset to fully ensure that, but that has to be done elsewhere after
 * all the individual LC_XXX variables have been set correctly.  (Thank you
 * Perl for making this kluge necessary.)
 */
char *
pg_perm_setlocale(int category, const char *locale)
{
	char	   *result;
	const char *envvar;

	result = setlocale(category, locale);

	if (result == NULL)
		return result;			/* fall out immediately on failure */

	/*
	 * minipg 已移除 Native Language Support（ENABLE_NLS）翻译子系统，消息
	 * 格式串为 ASCII，但数据库编码的字符串可能通过 %s 进入消息。这使整体
	 * 消息编码等于数据库编码。
	 */
	if (category == LC_CTYPE)
	{
		static char save_lc_ctype[NAMEDATALEN + 20];

		/* copy setlocale() return value before callee invokes it again */
		strlcpy(save_lc_ctype, result, sizeof(save_lc_ctype));
		result = save_lc_ctype;

		SetMessageEncoding(GetDatabaseEncoding());
	}

	switch (category)
	{
		case LC_COLLATE:
			envvar = "LC_COLLATE";
			break;
		case LC_CTYPE:
			envvar = "LC_CTYPE";
			break;
#ifdef LC_MESSAGES
		case LC_MESSAGES:
			envvar = "LC_MESSAGES";
			break;
#endif							/* LC_MESSAGES */
		case LC_MONETARY:
			envvar = "LC_MONETARY";
			break;
		case LC_NUMERIC:
			envvar = "LC_NUMERIC";
			break;
		default:
			elog(FATAL, "unrecognized LC category: %d", category);
			return NULL;		/* keep compiler quiet */
	}

	if (setenv(envvar, result, 1) != 0)
		return NULL;

	return result;
}


/*
 * Is the locale name valid for the locale category?
 *
 * If successful, and canonname isn't NULL, a palloc'd copy of the locale's
 * canonical name is stored there.  This is especially useful for figuring out
 * what locale name "" means (ie, the server environment value).  (Actually,
 * it seems that on most implementations that's the only thing it's good for;
 * we could wish that setlocale gave back a canonically spelled version of
 * the locale name, but typically it doesn't.)
 */
bool
check_locale(int category, const char *locale, char **canonname)
{
	char	   *save;
	char	   *res;

	if (canonname)
		*canonname = NULL;		/* in case of failure */

	save = setlocale(category, NULL);
	if (!save)
		return false;			/* won't happen, we hope */

	/* save may be pointing at a modifiable scratch variable, see above. */
	save = pstrdup(save);

	/* set the locale with setlocale, to see if it accepts it. */
	res = setlocale(category, locale);

	/* save canonical name if requested. */
	if (res && canonname)
		*canonname = pstrdup(res);

	/* restore old value. */
	if (!setlocale(category, save))
		elog(WARNING, "failed to restore old locale \"%s\"", save);
	pfree(save);

	return (res != NULL);
}


/*
 * GUC check/assign hooks
 *
 * For most locale categories, the assign hook doesn't actually set the locale
 * permanently, just reset flags so that the next use will cache the
 * appropriate values.  (See explanation at the top of this file.)
 *
 * Note: we accept value = "" as selecting the postmaster's environment
 * value, whatever it was (so long as the environment setting is legal).
 * This will have been locked down by an earlier call to pg_perm_setlocale.
 */
bool
check_locale_monetary(char **newval, void **extra, GucSource source)
{
	return check_locale(LC_MONETARY, *newval, NULL);
}

void
assign_locale_monetary(const char *newval, void *extra)
{
	CurrentLocaleConvValid = false;
}

bool
check_locale_numeric(char **newval, void **extra, GucSource source)
{
	return check_locale(LC_NUMERIC, *newval, NULL);
}

void
assign_locale_numeric(const char *newval, void *extra)
{
	CurrentLocaleConvValid = false;
}

/*
 * We allow LC_MESSAGES to actually be set globally.
 *
 * Note: we normally disallow value = "" because it wouldn't have consistent
 * semantics (it'd effectively just use the previous value).  However, this
 * is the value passed for PGC_S_DEFAULT, so don't complain in that case,
 * not even if the attempted setting fails due to invalid environment value.
 * The idea there is just to accept the environment setting *if possible*
 * during startup, until we can read the proper value from postgresql.conf.
 */
bool
check_locale_messages(char **newval, void **extra, GucSource source)
{
	if (**newval == '\0')
	{
		if (source == PGC_S_DEFAULT)
			return true;
		else
			return false;
	}

	/*
	 * LC_MESSAGES category does not exist everywhere, but accept it anyway
	 */
#ifdef LC_MESSAGES
	return check_locale(LC_MESSAGES, *newval, NULL);
#else
	return true;
#endif
}

void
assign_locale_messages(const char *newval, void *extra)
{
	/*
	 * LC_MESSAGES category does not exist everywhere, but accept it anyway.
	 * We ignore failure, as per comment above.
	 */
#ifdef LC_MESSAGES
	(void) pg_perm_setlocale(LC_MESSAGES, newval);
#endif
}


/*
 * Frees the malloced content of a struct lconv.  (But not the struct
 * itself.)  It's important that this not throw elog(ERROR).
 */
static void
free_struct_lconv(struct lconv *s)
{
	if (s->decimal_point)
		free(s->decimal_point);
	if (s->thousands_sep)
		free(s->thousands_sep);
	if (s->grouping)
		free(s->grouping);
	if (s->int_curr_symbol)
		free(s->int_curr_symbol);
	if (s->currency_symbol)
		free(s->currency_symbol);
	if (s->mon_decimal_point)
		free(s->mon_decimal_point);
	if (s->mon_thousands_sep)
		free(s->mon_thousands_sep);
	if (s->mon_grouping)
		free(s->mon_grouping);
	if (s->positive_sign)
		free(s->positive_sign);
	if (s->negative_sign)
		free(s->negative_sign);
}

/*
 * Check that all fields of a struct lconv (or at least, the ones we care
 * about) are non-NULL.  The field list must match free_struct_lconv().
 */
static bool
struct_lconv_is_valid(struct lconv *s)
{
	if (s->decimal_point == NULL)
		return false;
	if (s->thousands_sep == NULL)
		return false;
	if (s->grouping == NULL)
		return false;
	if (s->int_curr_symbol == NULL)
		return false;
	if (s->currency_symbol == NULL)
		return false;
	if (s->mon_decimal_point == NULL)
		return false;
	if (s->mon_thousands_sep == NULL)
		return false;
	if (s->mon_grouping == NULL)
		return false;
	if (s->positive_sign == NULL)
		return false;
	if (s->negative_sign == NULL)
		return false;
	return true;
}


/*
 * Convert the strdup'd string at *str from the specified encoding to the
 * database encoding.
 */
static void
db_encoding_convert(int encoding, char **str)
{
	char	   *pstr;
	char	   *mstr;

	/* convert the string to the database encoding */
	pstr = pg_any_to_server(*str, strlen(*str), encoding);
	if (pstr == *str)
		return;					/* no conversion happened */

	/* need it malloc'd not palloc'd */
	mstr = strdup(pstr);
	if (mstr == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/* replace old string */
	free(*str);
	*str = mstr;

	pfree(pstr);
}


/*
 * Return the POSIX lconv struct (contains number/money formatting
 * information) with locale information for all categories.
 */
struct lconv *
PGLC_localeconv(void)
{
	static struct lconv CurrentLocaleConv;
	static bool CurrentLocaleConvAllocated = false;
	struct lconv *extlconv;
	struct lconv worklconv;
	char	   *save_lc_monetary;
	char	   *save_lc_numeric;

	/* Did we do it already? */
	if (CurrentLocaleConvValid)
		return &CurrentLocaleConv;

	/* Free any already-allocated storage */
	if (CurrentLocaleConvAllocated)
	{
		free_struct_lconv(&CurrentLocaleConv);
		CurrentLocaleConvAllocated = false;
	}

	/*
	 * This is tricky because we really don't want to risk throwing error
	 * while the locale is set to other than our usual settings.  Therefore,
	 * the process is: collect the usual settings, set locale to special
	 * setting, copy relevant data into worklconv using strdup(), restore
	 * normal settings, convert data to desired encoding, and finally stash
	 * the collected data in CurrentLocaleConv.  This makes it safe if we
	 * throw an error during encoding conversion or run out of memory anywhere
	 * in the process.  All data pointed to by struct lconv members is
	 * allocated with strdup, to avoid premature elog(ERROR) and to allow
	 * using a single cleanup routine.
	 */
	memset(&worklconv, 0, sizeof(worklconv));

	/* Save prevailing values of monetary and numeric locales */
	save_lc_monetary = setlocale(LC_MONETARY, NULL);
	if (!save_lc_monetary)
		elog(ERROR, "setlocale(NULL) failed");
	save_lc_monetary = pstrdup(save_lc_monetary);

	save_lc_numeric = setlocale(LC_NUMERIC, NULL);
	if (!save_lc_numeric)
		elog(ERROR, "setlocale(NULL) failed");
	save_lc_numeric = pstrdup(save_lc_numeric);

	/*
	 * The POSIX standard explicitly says that it is undefined what happens if
	 * LC_MONETARY or LC_NUMERIC imply an encoding (codeset) different from
	 * that implied by LC_CTYPE.  In practice, all Unix-ish platforms seem to
	 * believe that localeconv() should return strings that are encoded in the
	 * codeset implied by the LC_MONETARY or LC_NUMERIC locale name.  Hence,
	 * once we have successfully collected the localeconv() results, we will
	 * convert them from that codeset to the desired server encoding.
	 */

	/* Here begins the critical section where we must not throw error */

	/* Get formatting information for numeric */
	setlocale(LC_NUMERIC, locale_numeric);
	extlconv = localeconv();

	/* Must copy data now in case setlocale() overwrites it */
	worklconv.decimal_point = strdup(extlconv->decimal_point);
	worklconv.thousands_sep = strdup(extlconv->thousands_sep);
	worklconv.grouping = strdup(extlconv->grouping);

	/* Get formatting information for monetary */
	setlocale(LC_MONETARY, locale_monetary);
	extlconv = localeconv();

	/* Must copy data now in case setlocale() overwrites it */
	worklconv.int_curr_symbol = strdup(extlconv->int_curr_symbol);
	worklconv.currency_symbol = strdup(extlconv->currency_symbol);
	worklconv.mon_decimal_point = strdup(extlconv->mon_decimal_point);
	worklconv.mon_thousands_sep = strdup(extlconv->mon_thousands_sep);
	worklconv.mon_grouping = strdup(extlconv->mon_grouping);
	worklconv.positive_sign = strdup(extlconv->positive_sign);
	worklconv.negative_sign = strdup(extlconv->negative_sign);
	/* Copy scalar fields as well */
	worklconv.int_frac_digits = extlconv->int_frac_digits;
	worklconv.frac_digits = extlconv->frac_digits;
	worklconv.p_cs_precedes = extlconv->p_cs_precedes;
	worklconv.p_sep_by_space = extlconv->p_sep_by_space;
	worklconv.n_cs_precedes = extlconv->n_cs_precedes;
	worklconv.n_sep_by_space = extlconv->n_sep_by_space;
	worklconv.p_sign_posn = extlconv->p_sign_posn;
	worklconv.n_sign_posn = extlconv->n_sign_posn;

	/*
	 * Restore the prevailing locale settings; failure to do so is fatal.
	 * Considering that the prevailing LC_MONETARY and LC_NUMERIC are almost
	 * certainly "C", there's really no reason that restoring those should
	 * fail.
	 */
	if (!setlocale(LC_MONETARY, save_lc_monetary))
		elog(FATAL, "failed to restore LC_MONETARY to \"%s\"", save_lc_monetary);
	if (!setlocale(LC_NUMERIC, save_lc_numeric))
		elog(FATAL, "failed to restore LC_NUMERIC to \"%s\"", save_lc_numeric);

	/*
	 * At this point we've done our best to clean up, and can call functions
	 * that might possibly throw errors with a clean conscience.  But let's
	 * make sure we don't leak any already-strdup'd fields in worklconv.
	 */
	PG_TRY();
	{
		int			encoding;

		/* Release the pstrdup'd locale names */
		pfree(save_lc_monetary);
		pfree(save_lc_numeric);

		/* If any of the preceding strdup calls failed, complain now. */
		if (!struct_lconv_is_valid(&worklconv))
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));

		/*
		 * Now we must perform encoding conversion from whatever's associated
		 * with the locales into the database encoding.  If we can't identify
		 * the encoding implied by LC_NUMERIC or LC_MONETARY (ie we get -1),
		 * use PG_SQL_ASCII, which will result in just validating that the
		 * strings are OK in the database encoding.
		 */
		encoding = pg_get_encoding_from_locale(locale_numeric, true);
		if (encoding < 0)
			encoding = PG_SQL_ASCII;

		db_encoding_convert(encoding, &worklconv.decimal_point);
		db_encoding_convert(encoding, &worklconv.thousands_sep);
		/* grouping is not text and does not require conversion */

		encoding = pg_get_encoding_from_locale(locale_monetary, true);
		if (encoding < 0)
			encoding = PG_SQL_ASCII;

		db_encoding_convert(encoding, &worklconv.int_curr_symbol);
		db_encoding_convert(encoding, &worklconv.currency_symbol);
		db_encoding_convert(encoding, &worklconv.mon_decimal_point);
		db_encoding_convert(encoding, &worklconv.mon_thousands_sep);
		/* mon_grouping is not text and does not require conversion */
		db_encoding_convert(encoding, &worklconv.positive_sign);
		db_encoding_convert(encoding, &worklconv.negative_sign);
	}
	PG_CATCH();
	{
		free_struct_lconv(&worklconv);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/*
	 * Everything is good, so save the results.
	 */
	CurrentLocaleConv = worklconv;
	CurrentLocaleConvAllocated = true;
	CurrentLocaleConvValid = true;
	return &CurrentLocaleConv;
}


/*
 * Detect aging strxfrm() implementations that, in a subset of locales, write
 * past the specified buffer length.  Affected users must update OS packages
 * before using PostgreSQL 9.5 or later.
 *
 * Assume that the bug can come and go from one postmaster startup to another
 * due to physical replication among diverse machines.  Assume that the bug's
 * presence will not change during the life of a particular postmaster.  Given
 * those assumptions, call this no less than once per postmaster startup per
 * LC_COLLATE setting used.  No known-affected system offers strxfrm_l(), so
 * there is no need to consider pg_collation locales.
 */
void
check_strxfrm_bug(void)
{
	char		buf[32];
	const int	canary = 0x7F;
	bool		ok = true;

	/*
	 * Given a two-byte ASCII string and length limit 7, 8 or 9, Solaris 10
	 * 05/08 returns 18 and modifies 10 bytes.  It respects limits above or
	 * below that range.
	 *
	 * The bug is present in Solaris 8 as well; it is absent in Solaris 10
	 * 01/13 and Solaris 11.2.  Affected locales include is_IS.ISO8859-1,
	 * en_US.UTF-8, en_US.ISO8859-1, and ru_RU.KOI8-R.  Unaffected locales
	 * include de_DE.UTF-8, de_DE.ISO8859-1, zh_TW.UTF-8, and C.
	 */
	buf[7] = canary;
	(void) strxfrm(buf, "ab", 7);
	if (buf[7] != canary)
		ok = false;

	/*
	 * illumos bug #1594 was present in the source tree from 2010-10-11 to
	 * 2012-02-01.  Given an ASCII string of any length and length limit 1,
	 * affected systems ignore the length limit and modify a number of bytes
	 * one less than the return value.  The problem inputs for this bug do not
	 * overlap those for the Solaris bug, hence a distinct test.
	 *
	 * Affected systems include smartos-20110926T021612Z.  Affected locales
	 * include en_US.ISO8859-1 and en_US.UTF-8.  Unaffected locales include C.
	 */
	buf[1] = canary;
	(void) strxfrm(buf, "a", 1);
	if (buf[1] != canary)
		ok = false;

	if (!ok)
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg_internal("strxfrm(), in locale \"%s\", writes past the specified array length",
								 setlocale(LC_COLLATE, NULL)),
				 errhint("Apply system library package updates.")));
}


/*
 * Cache mechanism for collation information.
 *
 * We cache two flags: whether the collation's LC_COLLATE or LC_CTYPE is C
 * (or POSIX), so we can optimize a few code paths in various places.
 * For the built-in C and POSIX collations, we can know that without even
 * doing a cache lookup, but we want to support aliases for C/POSIX too.
 * For the "default" collation, there are separate static cache variables,
 * since consulting the pg_collation catalog doesn't tell us what we need.
 *
 * Also, if a pg_locale_t has been requested for a collation, we cache that
 * for the life of a backend.
 *
 * Note that some code relies on the flags not reporting false negatives
 * (that is, saying it's not C when it is).  For example, char2wchar()
 * could fail if the locale is C, so the upper/lower/initcap functions
 * shouldn't call it in that case.
 *
 * Note that we currently lack any way to flush the cache.  Since we don't
 * support ALTER COLLATION, this is OK.  The worst case is that someone
 * drops a collation, and a useless cache entry hangs around in existing
 * backends.
 */

static collation_cache_entry *
lookup_collation_cache(Oid collation, bool set_flags)
{
	collation_cache_entry *cache_entry;
	bool		found;

	Assert(OidIsValid(collation));
	Assert(collation != DEFAULT_COLLATION_OID);

	AssertCouldGetRelation();

	if (collation_cache == NULL)
	{
		/* First time through, initialize the hash table */
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(collation_cache_entry);
		collation_cache = hash_create("Collation cache", 100, &ctl,
									  HASH_ELEM | HASH_BLOBS);
	}

	cache_entry = hash_search(collation_cache, &collation, HASH_ENTER, &found);
	if (!found)
	{
		/*
		 * Make sure cache entry is marked invalid, in case we fail before
		 * setting things.
		 */
		cache_entry->flags_valid = false;
		cache_entry->locale = 0;
	}

	if (set_flags && !cache_entry->flags_valid)
	{
		/* Attempt to set the flags */
		HeapTuple	tp;
		Form_pg_collation collform;
		const char *collcollate;
		const char *collctype;

		tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collation));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for collation %u", collation);
		collform = (Form_pg_collation) GETSTRUCT(tp);

		collcollate = NameStr(collform->collcollate);
		collctype = NameStr(collform->collctype);

		cache_entry->collate_is_c = ((strcmp(collcollate, "C") == 0) ||
									 (strcmp(collcollate, "POSIX") == 0));
		cache_entry->ctype_is_c = ((strcmp(collctype, "C") == 0) ||
								   (strcmp(collctype, "POSIX") == 0));

		cache_entry->flags_valid = true;

		ReleaseSysCache(tp);
	}

	return cache_entry;
}


/*
 * Detect whether collation's LC_COLLATE property is C
 */
bool
lc_collate_is_c(Oid collation)
{
	/*
	 * If we're asked about "collation 0", return false, so that the code will
	 * go into the non-C path and report that the collation is bogus.
	 */
	if (!OidIsValid(collation))
		return false;

	/*
	 * If we're asked about the default collation, the answer is always C:
	 * minipg removed per-object collations, and the default collation is
	 * pinned to C everywhere (bootstrap, single-user and normal backends
	 * alike).  This keeps index builds deterministic regardless of the
	 * LC_COLLATE environment the process happens to inherit.
	 */
	if (collation == DEFAULT_COLLATION_OID)
		return true;

	/*
	 * If we're asked about the built-in C/POSIX collations, we know that.
	 */
	if (collation == C_COLLATION_OID ||
		collation == POSIX_COLLATION_OID)
		return true;

	/*
	 * Otherwise, we have to consult pg_collation, but we cache that.
	 */
	return (lookup_collation_cache(collation, true))->collate_is_c;
}

/*
 * Detect whether collation's LC_CTYPE property is C
 */
bool
lc_ctype_is_c(Oid collation)
{
	/*
	 * If we're asked about "collation 0", return false, so that the code will
	 * go into the non-C path and report that the collation is bogus.
	 */
	if (!OidIsValid(collation))
		return false;

	/*
	 * If we're asked about the default collation, we have to inquire of the C
	 * library.  Cache the result so we only have to compute it once.
	 */
	if (collation == DEFAULT_COLLATION_OID)
	{
		static int	result = -1;
		char	   *localeptr;

		if (result >= 0)
			return (bool) result;
		localeptr = setlocale(LC_CTYPE, NULL);
		if (!localeptr)
			elog(ERROR, "invalid LC_CTYPE setting");

		if (strcmp(localeptr, "C") == 0)
			result = true;
		else if (strcmp(localeptr, "POSIX") == 0)
			result = true;
		else
			result = false;
		return (bool) result;
	}

	/*
	 * If we're asked about the built-in C/POSIX collations, we know that.
	 */
	if (collation == C_COLLATION_OID ||
		collation == POSIX_COLLATION_OID)
		return true;

	/*
	 * Otherwise, we have to consult pg_collation, but we cache that.
	 */
	return (lookup_collation_cache(collation, true))->ctype_is_c;
}


/* simple subroutine for reporting errors from newlocale() */
#ifdef HAVE_LOCALE_T
static void
report_newlocale_failure(const char *localename)
{
	int			save_errno;

	/*
	 * Windows doesn't provide any useful error indication from
	 * _create_locale(), and BSD-derived platforms don't seem to feel they
	 * need to set errno either (even though POSIX is pretty clear that
	 * newlocale should do so).  So, if errno hasn't been set, assume ENOENT
	 * is what to report.
	 */
	if (errno == 0)
		errno = ENOENT;

	/*
	 * ENOENT means "no such locale", not "no such file", so clarify that
	 * errno with an errdetail message.
	 */
	save_errno = errno;			/* auxiliary funcs might change errno */
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("could not create locale \"%s\": %m",
					localename),
			 (save_errno == ENOENT ?
			  errdetail("The operating system could not find any locale data for the locale name \"%s\".",
						localename) : 0)));
}
#endif							/* HAVE_LOCALE_T */


/*
 * Create a locale_t from a collation OID.  Results are cached for the
 * lifetime of the backend.  Thus, do not free the result with freelocale().
 *
 * As a special optimization, the default/database collation returns 0.
 * Callers should then revert to the non-locale_t-enabled code path.
 * In fact, they shouldn't call this function at all when they are dealing
 * with the default locale.  That can save quite a bit in hotspots.
 * Also, callers should avoid calling this before going down a C/POSIX
 * fastpath, because such a fastpath should work even on platforms without
 * locale_t support in the C library.
 *
 * For simplicity, we always generate COLLATE + CTYPE even though we
 * might only need one of them.  Since this is called only once per session,
 * it shouldn't cost much.
 */
pg_locale_t
pg_newlocale_from_collation(Oid collid)
{
	collation_cache_entry *cache_entry;

	/* Callers must pass a valid OID */
	Assert(OidIsValid(collid));

	/* Return 0 for "default" collation, just in case caller forgets */
	if (collid == DEFAULT_COLLATION_OID)
		return (pg_locale_t) 0;

	cache_entry = lookup_collation_cache(collid, false);

	if (cache_entry->locale == 0)
	{
		/* We haven't computed this yet in this session, so do it */
		HeapTuple	tp;
		Form_pg_collation collform;
		const char *collcollate;
		const char *collctype pg_attribute_unused();
		struct pg_locale_struct result;
		pg_locale_t resultp;
		Datum		collversion;
		bool		isnull;

		tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for collation %u", collid);
		collform = (Form_pg_collation) GETSTRUCT(tp);

		collcollate = NameStr(collform->collcollate);
		collctype = NameStr(collform->collctype);

		/* We'll fill in the result struct locally before allocating memory */
		memset(&result, 0, sizeof(result));
		result.provider = collform->collprovider;
		result.deterministic = collform->collisdeterministic;

		if (collform->collprovider == COLLPROVIDER_LIBC)
		{
#ifdef HAVE_LOCALE_T
			locale_t	loc;

			if (strcmp(collcollate, collctype) == 0)
			{
				/* Normal case where they're the same */
				errno = 0;
				loc = newlocale(LC_COLLATE_MASK | LC_CTYPE_MASK, collcollate,
								NULL);
				if (!loc)
					report_newlocale_failure(collcollate);
			}
			else
			{
				/* We need two newlocale() steps */
				locale_t	loc1;

				errno = 0;
				loc1 = newlocale(LC_COLLATE_MASK, collcollate, NULL);
				if (!loc1)
					report_newlocale_failure(collcollate);
				errno = 0;
				loc = newlocale(LC_CTYPE_MASK, collctype, loc1);
				if (!loc)
					report_newlocale_failure(collctype);
			}

			result.info.lt = loc;
#else							/* not HAVE_LOCALE_T */
			/* platform that doesn't support locale_t */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("collation provider LIBC is not supported on this platform")));
#endif							/* not HAVE_LOCALE_T */
		}

		collversion = SysCacheGetAttr(COLLOID, tp, Anum_pg_collation_collversion,
									  &isnull);
		if (!isnull)
		{
			char	   *actual_versionstr;
			char	   *collversionstr;

			actual_versionstr = get_collation_actual_version(collform->collprovider, collcollate);
			if (!actual_versionstr)
			{
				/*
				 * This could happen when specifying a version in CREATE
				 * COLLATION for a libc locale, or manually creating a mess in
				 * the catalogs.
				 */
				ereport(ERROR,
						(errmsg("collation \"%s\" has no actual version, but a version was specified",
								NameStr(collform->collname))));
			}
			collversionstr = TextDatumGetCString(collversion);

			if (strcmp(actual_versionstr, collversionstr) != 0)
				ereport(WARNING,
						(errmsg("collation \"%s\" has version mismatch",
								NameStr(collform->collname)),
						 errdetail("The collation in the database was created using version %s, "
								   "but the operating system provides version %s.",
								   collversionstr, actual_versionstr),
						 errhint("Rebuild all objects affected by this collation and run "
								 "ALTER COLLATION %s REFRESH VERSION, "
								 "or build PostgreSQL with the right library version.",
								 quote_qualified_identifier(get_namespace_name(collform->collnamespace),
															NameStr(collform->collname)))));
		}

		ReleaseSysCache(tp);

		/* We'll keep the pg_locale_t structures in TopMemoryContext */
		resultp = MemoryContextAlloc(TopMemoryContext, sizeof(*resultp));
		*resultp = result;

		cache_entry->locale = resultp;
	}

	return cache_entry->locale;
}

/*
 * Get provider-specific collation version string for the given collation from
 * the operating system/library.
 */
char *
get_collation_actual_version(char collprovider, const char *collcollate)
{
	char	   *collversion = NULL;

	if (collprovider == COLLPROVIDER_LIBC &&
		pg_strcasecmp("C", collcollate) != 0 &&
		pg_strncasecmp("C.", collcollate, 2) != 0 &&
		pg_strcasecmp("POSIX", collcollate) != 0)
	{
#if defined(__GLIBC__)
		/* Use the glibc version because we don't have anything better. */
		collversion = pstrdup(gnu_get_libc_version());
#elif defined(LC_VERSION_MASK)
		locale_t	loc;

		/* Look up FreeBSD collation version. */
		loc = newlocale(LC_COLLATE_MASK, collcollate, NULL);
		if (loc)
		{
			collversion =
				pstrdup(querylocale(LC_COLLATE_MASK | LC_VERSION_MASK, loc));
			freelocale(loc);
		}
		else
			ereport(ERROR,
					(errmsg("could not load locale \"%s\"", collcollate)));
#endif
	}

	return collversion;
}



/*
 * These functions convert from/to libc's wchar_t, *not* pg_wchar_t.
 * Therefore we keep them here rather than with the mbutils code.
 */

/*
 * wchar2char --- convert wide characters to multibyte format
 *
 * This has the same API as the standard wcstombs_l() function; in particular,
 * tolen is the maximum number of bytes to store at *to, and *from must be
 * zero-terminated.  The output will be zero-terminated iff there is room.
 */
size_t
wchar2char(char *to, const wchar_t *from, size_t tolen, pg_locale_t locale)
{
	size_t		result;

	Assert(!locale || locale->provider == COLLPROVIDER_LIBC);

	if (tolen == 0)
		return 0;

	if (locale == (pg_locale_t) 0)
	{
		/* Use wcstombs directly for the default locale */
		result = wcstombs(to, from, tolen);
	}
	else
	{
#ifdef HAVE_LOCALE_T
#ifdef HAVE_WCSTOMBS_L
		/* Use wcstombs_l for nondefault locales */
		result = wcstombs_l(to, from, tolen, locale->info.lt);
#else							/* !HAVE_WCSTOMBS_L */
		/* We have to temporarily set the locale as current ... ugh */
		locale_t	save_locale = uselocale(locale->info.lt);

		result = wcstombs(to, from, tolen);

		uselocale(save_locale);
#endif							/* HAVE_WCSTOMBS_L */
#else							/* !HAVE_LOCALE_T */
		/* Can't have locale != 0 without HAVE_LOCALE_T */
		elog(ERROR, "wcstombs_l is not available");
		result = 0;				/* keep compiler quiet */
#endif							/* HAVE_LOCALE_T */
	}

	return result;
}

/*
 * char2wchar --- convert multibyte characters to wide characters
 *
 * This has almost the API of mbstowcs_l(), except that *from need not be
 * null-terminated; instead, the number of input bytes is specified as
 * fromlen.  Also, we ereport() rather than returning -1 for invalid
 * input encoding.  tolen is the maximum number of wchar_t's to store at *to.
 * The output will be zero-terminated iff there is room.
 */
size_t
char2wchar(wchar_t *to, size_t tolen, const char *from, size_t fromlen,
		   pg_locale_t locale)
{
	size_t		result;

	Assert(!locale || locale->provider == COLLPROVIDER_LIBC);

	if (tolen == 0)
		return 0;

	{
		/* mbstowcs requires ending '\0' */
		char	   *str = pnstrdup(from, fromlen);

		if (locale == (pg_locale_t) 0)
		{
			/* Use mbstowcs directly for the default locale */
			result = mbstowcs(to, str, tolen);
		}
		else
		{
#ifdef HAVE_LOCALE_T
#ifdef HAVE_MBSTOWCS_L
			/* Use mbstowcs_l for nondefault locales */
			result = mbstowcs_l(to, str, tolen, locale->info.lt);
#else							/* !HAVE_MBSTOWCS_L */
			/* We have to temporarily set the locale as current ... ugh */
			locale_t	save_locale = uselocale(locale->info.lt);

			result = mbstowcs(to, str, tolen);

			uselocale(save_locale);
#endif							/* HAVE_MBSTOWCS_L */
#else							/* !HAVE_LOCALE_T */
			/* Can't have locale != 0 without HAVE_LOCALE_T */
			elog(ERROR, "mbstowcs_l is not available");
			result = 0;			/* keep compiler quiet */
#endif							/* HAVE_LOCALE_T */
		}

		pfree(str);
	}

	if (result == -1)
	{
		/*
		 * Invalid multibyte character encountered.  We try to give a useful
		 * error message by letting pg_verifymbstr check the string.  But it's
		 * possible that the string is OK to us, and not OK to mbstowcs ---
		 * this suggests that the LC_CTYPE locale is different from the
		 * database encoding.  Give a generic error message if pg_verifymbstr
		 * can't find anything wrong.
		 */
		pg_verifymbstr(from, fromlen, false);	/* might not return */
		/* but if it does ... */
		ereport(ERROR,
				(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
				 errmsg("invalid multibyte character for locale"),
				 errhint("The server's LC_CTYPE locale is probably incompatible with the database encoding.")));
	}

	return result;
}
