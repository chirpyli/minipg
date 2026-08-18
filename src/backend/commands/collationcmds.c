/*-------------------------------------------------------------------------
 *
 * collationcmds.c
 *	  collation-related commands support code
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/collationcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_collation.h"
#include "commands/alter.h"
#include "commands/collationcmds.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "common/string.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/syscache.h"


typedef struct
{
	char	   *localename;		/* name of locale, as per "locale -a" */
	char	   *alias;			/* shortened alias for same */
	int			enc;			/* encoding */
} CollAliasData;


/*
 * Subroutine for ALTER COLLATION SET SCHEMA and RENAME
 *
 * Is there a collation with the same name of the given collation already in
 * the given namespace?  If so, raise an appropriate error message.
 */
void
IsThereCollationInNamespace(const char *collname, Oid nspOid)
{
	/* make sure the name doesn't already exist in new schema */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  CStringGetDatum(collname),
							  Int32GetDatum(GetDatabaseEncoding()),
							  ObjectIdGetDatum(nspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" for encoding \"%s\" already exists in schema \"%s\"",
						collname, GetDatabaseEncodingName(),
						get_namespace_name(nspOid))));

	/* mustn't match an any-encoding entry, either */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  CStringGetDatum(collname),
							  Int32GetDatum(-1),
							  ObjectIdGetDatum(nspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" already exists in schema \"%s\"",
						collname, get_namespace_name(nspOid))));
}


Datum
pg_collation_actual_version(PG_FUNCTION_ARGS)
{
	Oid			collid = PG_GETARG_OID(0);
	HeapTuple	tp;
	char	   *collcollate;
	char		collprovider;
	char	   *version;

	tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
	if (!HeapTupleIsValid(tp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("collation with OID %u does not exist", collid)));

	collcollate = pstrdup(NameStr(((Form_pg_collation) GETSTRUCT(tp))->collcollate));
	collprovider = ((Form_pg_collation) GETSTRUCT(tp))->collprovider;

	ReleaseSysCache(tp);

	version = get_collation_actual_version(collprovider, collcollate);

	if (version)
		PG_RETURN_TEXT_P(cstring_to_text(version));
	else
		PG_RETURN_NULL();
}


/* will we use "locale -a" in pg_import_system_collations? */
#ifdef HAVE_LOCALE_T
#define READ_LOCALE_A_OUTPUT
#endif

#ifdef READ_LOCALE_A_OUTPUT
/*
 * "Normalize" a libc locale name, stripping off encoding tags such as
 * ".utf8" (e.g., "en_US.utf8" -> "en_US", but "br_FR.iso885915@euro"
 * -> "br_FR@euro").  Return true if a new, different name was
 * generated.
 */
static bool
normalize_libc_locale_name(char *new, const char *old)
{
	char	   *n = new;
	const char *o = old;
	bool		changed = false;

	while (*o)
	{
		if (*o == '.')
		{
			/* skip over encoding tag such as ".utf8" or ".UTF-8" */
			o++;
			while ((*o >= 'A' && *o <= 'Z')
				   || (*o >= 'a' && *o <= 'z')
				   || (*o >= '0' && *o <= '9')
				   || (*o == '-'))
				o++;
			changed = true;
		}
		else
			*n++ = *o++;
	}
	*n = '\0';

	return changed;
}

/*
 * qsort comparator for CollAliasData items
 */
static int
cmpaliases(const void *a, const void *b)
{
	const CollAliasData *ca = (const CollAliasData *) a;
	const CollAliasData *cb = (const CollAliasData *) b;

	/* comparing localename is enough because other fields are derived */
	return strcmp(ca->localename, cb->localename);
}
#endif							/* READ_LOCALE_A_OUTPUT */




/*
 * pg_import_system_collations: add known system collations to pg_collation
 */
Datum
pg_import_system_collations(PG_FUNCTION_ARGS)
{
	Oid			nspid = PG_GETARG_OID(0);
	int			ncreated = 0;


	if (!SearchSysCacheExists1(NAMESPACEOID, ObjectIdGetDatum(nspid)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema with OID %u does not exist", nspid)));

	/* Load collations known to libc, using "locale -a" to enumerate them */
#ifdef READ_LOCALE_A_OUTPUT
	{
		FILE	   *locale_a_handle;
		char		localebuf[NAMEDATALEN]; /* we assume ASCII so this is fine */
		int			nvalid = 0;
		Oid			collid;
		CollAliasData *aliases;
		int			naliases,
					maxaliases,
					i;

		/* expansible array of aliases */
		maxaliases = 100;
		aliases = (CollAliasData *) palloc(maxaliases * sizeof(CollAliasData));
		naliases = 0;

		locale_a_handle = OpenPipeStream("locale -a", "r");
		if (locale_a_handle == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not execute command \"%s\": %m",
							"locale -a")));

		while (fgets(localebuf, sizeof(localebuf), locale_a_handle))
		{
			size_t		len;
			int			enc;
			char		alias[NAMEDATALEN];

			len = strlen(localebuf);

			if (len == 0 || localebuf[len - 1] != '\n')
			{
				elog(DEBUG1, "locale name too long, skipped: \"%s\"", localebuf);
				continue;
			}
			localebuf[len - 1] = '\0';

			/*
			 * Some systems have locale names that don't consist entirely of
			 * ASCII letters (such as "bokm&aring;l" or "fran&ccedil;ais").
			 * This is pretty silly, since we need the locale itself to
			 * interpret the non-ASCII characters. We can't do much with
			 * those, so we filter them out.
			 */
			if (!pg_is_ascii(localebuf))
			{
				elog(DEBUG1, "locale name has non-ASCII characters, skipped: \"%s\"", localebuf);
				continue;
			}

			enc = pg_get_encoding_from_locale(localebuf, false);
			if (enc < 0)
			{
				/* error message printed by pg_get_encoding_from_locale() */
				continue;
			}
			if (!PG_VALID_BE_ENCODING(enc))
				continue;		/* ignore locales for client-only encodings */
			if (enc == PG_SQL_ASCII)
				continue;		/* C/POSIX are already in the catalog */

			/* count valid locales found in operating system */
			nvalid++;

			/*
			 * Create a collation named the same as the locale, but quietly
			 * doing nothing if it already exists.  This is the behavior we
			 * need even at initdb time, because some versions of "locale -a"
			 * can report the same locale name more than once.  And it's
			 * convenient for later import runs, too, since you just about
			 * always want to add on new locales without a lot of chatter
			 * about existing ones.
			 */
			collid = CollationCreate(localebuf, nspid,
									 COLLPROVIDER_LIBC, true, enc,
									 localebuf, localebuf,
									 get_collation_actual_version(COLLPROVIDER_LIBC, localebuf),
									 true, true);
			if (OidIsValid(collid))
			{
				ncreated++;

				/* Must do CCI between inserts to handle duplicates correctly */
				CommandCounterIncrement();
			}

			/*
			 * Generate aliases such as "en_US" in addition to "en_US.utf8"
			 * for ease of use.  Note that collation names are unique per
			 * encoding only, so this doesn't clash with "en_US" for LATIN1,
			 * say.
			 *
			 * However, it might conflict with a name we'll see later in the
			 * "locale -a" output.  So save up the aliases and try to add them
			 * after we've read all the output.
			 */
			if (normalize_libc_locale_name(alias, localebuf))
			{
				if (naliases >= maxaliases)
				{
					maxaliases *= 2;
					aliases = (CollAliasData *)
						repalloc(aliases, maxaliases * sizeof(CollAliasData));
				}
				aliases[naliases].localename = pstrdup(localebuf);
				aliases[naliases].alias = pstrdup(alias);
				aliases[naliases].enc = enc;
				naliases++;
			}
		}

		ClosePipeStream(locale_a_handle);

		/*
		 * Before processing the aliases, sort them by locale name.  The point
		 * here is that if "locale -a" gives us multiple locale names with the
		 * same encoding and base name, say "en_US.utf8" and "en_US.utf-8", we
		 * want to pick a deterministic one of them.  First in ASCII sort
		 * order is a good enough rule.  (Before PG 10, the code corresponding
		 * to this logic in initdb.c had an additional ordering rule, to
		 * prefer the locale name exactly matching the alias, if any.  We
		 * don't need to consider that here, because we would have already
		 * created such a pg_collation entry above, and that one will win.)
		 */
		if (naliases > 1)
			qsort((void *) aliases, naliases, sizeof(CollAliasData), cmpaliases);

		/* Now add aliases, ignoring any that match pre-existing entries */
		for (i = 0; i < naliases; i++)
		{
			char	   *locale = aliases[i].localename;
			char	   *alias = aliases[i].alias;
			int			enc = aliases[i].enc;

			collid = CollationCreate(alias, nspid,
									 COLLPROVIDER_LIBC, true, enc,
									 locale, locale,
									 get_collation_actual_version(COLLPROVIDER_LIBC, locale),
									 true, true);
			if (OidIsValid(collid))
			{
				ncreated++;

				CommandCounterIncrement();
			}
		}

		/* Give a warning if "locale -a" seems to be malfunctioning */
		if (nvalid == 0)
			ereport(WARNING,
					(errmsg("no usable system locales were found")));
	}
#endif							/* READ_LOCALE_A_OUTPUT */


	PG_RETURN_INT32(ncreated);
}
