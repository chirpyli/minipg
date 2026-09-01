/*-------------------------------------------------------------------------
 *
 * encnames.c
 *	  Encoding names and routines for working with them.
 *
 * Portions Copyright (c) 2001-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/encnames.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <ctype.h>
#include <unistd.h>

#include "mb/pg_wchar.h"


/*
 * All encoding names, sorted:		 *** A L P H A B E T I C ***
 *
 * All names must be without irrelevant chars, search routines use
 * isalnum() chars only. It means ISO-8859-1, iso_8859-1 and Iso8859_1
 * are always converted to 'iso88591'. All must be lower case.
 *
 * minipg supports only three encodings: SQL_ASCII, UTF8 and LATIN1
 * (ISO-8859-1).  All other encodings were removed to slim down the
 * character set machinery.
 */
typedef struct pg_encname
{
	const char *name;
	pg_enc		encoding;
} pg_encname;

static const pg_encname pg_encname_tbl[] =
{
	{
		"iso88591", PG_LATIN1
	},							/* ISO-8859-1; RFC1345,KXS2 */
	{
		"latin1", PG_LATIN1
	},							/* alias for ISO-8859-1 */
	{
		"sqlascii", PG_SQL_ASCII
	},
	{
		"unicode", PG_UTF8
	},							/* alias for UTF8 */
	{
		"utf8", PG_UTF8
	}							/* alias for UTF8 */
};


/*
 * These are "official" encoding names.
 * XXX must be sorted by the same order as enum pg_enc (in mb/pg_wchar.h)
 */
const pg_enc2name pg_enc2name_tbl[] =
{
	{"SQL_ASCII", PG_SQL_ASCII},
	{"UTF8", PG_UTF8},
	{"LATIN1", PG_LATIN1}
};



/*
 * Encoding checks, for error returns -1 else encoding id
 */
int
pg_valid_client_encoding(const char *name)
{
	int			enc;

	if ((enc = pg_char_to_encoding(name)) < 0)
		return -1;

	if (!PG_VALID_FE_ENCODING(enc))
		return -1;

	return enc;
}

int
pg_valid_server_encoding(const char *name)
{
	int			enc;

	if ((enc = pg_char_to_encoding(name)) < 0)
		return -1;

	if (!PG_VALID_BE_ENCODING(enc))
		return -1;

	return enc;
}

int
pg_valid_server_encoding_id(int encoding)
{
	return PG_VALID_BE_ENCODING(encoding);
}

/*
 * Remove irrelevant chars from encoding name, store at *newkey
 *
 * (Caller's responsibility to provide a large enough buffer)
 */
static char *
clean_encoding_name(const char *key, char *newkey)
{
	const char *p;
	char	   *np;

	for (p = key, np = newkey; *p != '\0'; p++)
	{
		if (isalnum((unsigned char) *p))
		{
			if (*p >= 'A' && *p <= 'Z')
				*np++ = *p + 'a' - 'A';
			else
				*np++ = *p;
		}
	}
	*np = '\0';
	return newkey;
}

/*
 * Search encoding by encoding name
 *
 * Returns encoding ID, or -1 if not recognized
 */
int
pg_char_to_encoding(const char *name)
{
	unsigned int nel = lengthof(pg_encname_tbl);
	const pg_encname *base = pg_encname_tbl,
			   *last = base + nel - 1,
			   *position;
	int			result;
	char		buff[NAMEDATALEN],
			   *key;

	if (name == NULL || *name == '\0')
		return -1;

	if (strlen(name) >= NAMEDATALEN)
		return -1;				/* it's certainly not in the table */

	key = clean_encoding_name(name, buff);

	while (last >= base)
	{
		position = base + ((last - base) >> 1);
		result = key[0] - position->name[0];

		if (result == 0)
		{
			result = strcmp(key, position->name);
			if (result == 0)
				return position->encoding;
		}
		if (result < 0)
			last = position - 1;
		else
			base = position + 1;
	}
	return -1;
}

const char *
pg_encoding_to_char(int encoding)
{
	if (PG_VALID_ENCODING(encoding))
	{
		const pg_enc2name *p = &pg_enc2name_tbl[encoding];

		Assert(encoding == p->encoding);
		return p->name;
	}
	return "";
}
