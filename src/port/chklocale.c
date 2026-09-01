/*-------------------------------------------------------------------------
 *
 * chklocale.c
 *		Functions for handling locale-related info
 *
 *
 * Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/port/chklocale.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#ifdef HAVE_LANGINFO_H
#include <langinfo.h>
#endif

#include "mb/pg_wchar.h"


/*
 * This table needs to recognize all the CODESET spellings for supported
 * backend encodings, as well as frontend-only encodings where possible
 * (the latter case is currently only needed for initdb to recognize
 * error situations).  On Windows, we rely on entries for codepage
 * numbers (CPnnn).
 *
 * Note that we search the table with pg_strcasecmp(), so variant
 * capitalizations don't need their own entries.
 */
struct encoding_match
{
	enum pg_enc pg_enc_code;
	const char *system_enc_name;
};

static const struct encoding_match encoding_match_list[] = {
	{PG_UTF8, "UTF-8"},
	{PG_UTF8, "utf8"},
	{PG_UTF8, "CP65001"},

	{PG_LATIN1, "ISO-8859-1"},
	{PG_LATIN1, "ISO8859-1"},
	{PG_LATIN1, "iso88591"},
	{PG_LATIN1, "CP28591"},

	{PG_SQL_ASCII, "US-ASCII"},

	{PG_SQL_ASCII, NULL}		/* end marker */
};


#if (defined(HAVE_LANGINFO_H) && defined(CODESET))

/*
 * Given a setting for LC_CTYPE, return the Postgres ID of the associated
 * encoding, if we can determine it.  Return -1 if we can't determine it.
 *
 * Pass in NULL to get the encoding for the current locale setting.
 * Pass "" to get the encoding selected by the server's environment.
 *
 * If the result is PG_SQL_ASCII, callers should treat it as being compatible
 * with any desired encoding.
 *
 * If running in the backend and write_message is false, this function must
 * cope with the possibility that elog() and palloc() are not yet usable.
 */
int
pg_get_encoding_from_locale(const char *ctype, bool write_message)
{
	char	   *sys;
	int			i;

	/* Get the CODESET property, and also LC_CTYPE if not passed in */
	if (ctype)
	{
		char	   *save;
		char	   *name;

		/* If locale is C or POSIX, we can allow all encodings */
		if (pg_strcasecmp(ctype, "C") == 0 ||
			pg_strcasecmp(ctype, "POSIX") == 0)
			return PG_SQL_ASCII;

		save = setlocale(LC_CTYPE, NULL);
		if (!save)
			return -1;			/* setlocale() broken? */
		/* must copy result, or it might change after setlocale */
		save = strdup(save);
		if (!save)
			return -1;			/* out of memory; unlikely */

		name = setlocale(LC_CTYPE, ctype);
		if (!name)
		{
			free(save);
			return -1;			/* bogus ctype passed in? */
		}

		sys = nl_langinfo(CODESET);
		if (sys)
			sys = strdup(sys);

		setlocale(LC_CTYPE, save);
		free(save);
	}
	else
	{
		/* much easier... */
		ctype = setlocale(LC_CTYPE, NULL);
		if (!ctype)
			return -1;			/* setlocale() broken? */

		/* If locale is C or POSIX, we can allow all encodings */
		if (pg_strcasecmp(ctype, "C") == 0 ||
			pg_strcasecmp(ctype, "POSIX") == 0)
			return PG_SQL_ASCII;

		sys = nl_langinfo(CODESET);
		if (sys)
			sys = strdup(sys);
	}

	if (!sys)
		return -1;				/* out of memory; unlikely */

	/* Check the table */
	for (i = 0; encoding_match_list[i].system_enc_name; i++)
	{
		if (pg_strcasecmp(sys, encoding_match_list[i].system_enc_name) == 0)
		{
			free(sys);
			return encoding_match_list[i].pg_enc_code;
		}
	}

	/* Special-case kluges for particular platforms go here */

	/*
	 * We print a warning if we got a CODESET string but couldn't recognize
	 * it.  This means we need another entry in the table.
	 */
	if (write_message)
	{
#ifdef FRONTEND
		fprintf(stderr, _("could not determine encoding for locale \"%s\": codeset is \"%s\""),
				ctype, sys);
		/* keep newline separate so there's only one translatable string */
		fputc('\n', stderr);
#else
		ereport(WARNING,
				(errmsg("could not determine encoding for locale \"%s\": codeset is \"%s\"",
						ctype, sys)));
#endif
	}

	free(sys);
	return -1;
}
#else							/* (HAVE_LANGINFO_H && CODESET) || WIN32 */

/*
 * stub if no multi-language platform support
 *
 * Note: we could return -1 here, but that would have the effect of
 * forcing users to specify an encoding to initdb on such platforms.
 * It seems better to silently default to SQL_ASCII.
 */
int
pg_get_encoding_from_locale(const char *ctype, bool write_message)
{
	return PG_SQL_ASCII;
}

#endif							/* (HAVE_LANGINFO_H && CODESET) || WIN32 */
