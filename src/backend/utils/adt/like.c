/*-------------------------------------------------------------------------
 *
 * like.c
 *	  like expression handling code.
 *
 *	 NOTES
 *		A big hack of the regexp.c code!! Contributed by
 *		Keith Parks <emkxp01@mtcc.demon.co.uk> (7/95).
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	src/backend/utils/adt/like.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "catalog/pg_collation.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/pg_locale.h"


#define LIKE_TRUE						1
#define LIKE_FALSE						0
#define LIKE_ABORT						(-1)


static int	SB_MatchText(const char *t, int tlen, const char *p, int plen,
						 pg_locale_t locale, bool locale_is_c);

static int	MB_MatchText(const char *t, int tlen, const char *p, int plen,
						 pg_locale_t locale, bool locale_is_c);

static int	UTF8_MatchText(const char *t, int tlen, const char *p, int plen,
						   pg_locale_t locale, bool locale_is_c);

static int	GenericMatchText(const char *s, int slen, const char *p, int plen, Oid collation);

/*--------------------
 * Support routine for MatchText. Compares given multibyte streams
 * as wide characters. If they match, returns 1 otherwise returns 0.
 *--------------------
 */
static inline int
wchareq(const char *p1, int p1len, const char *p2, int p2len)
{
	int			p1clen;

	/* Optimization:  quickly compare the first byte. */
	if (*p1 != *p2)
		return 0;

	p1clen = pg_mblen_with_len(p1, p1len);
	if (pg_mblen_with_len(p2, p2len) != p1clen)
		return 0;

	/* They are the same length */
	while (p1clen--)
	{
		if (*p1++ != *p2++)
			return 0;
	}
	return 1;
}

/*
 * Note: the case-insensitive variants of LIKE (ILIKE and the ~~* operators)
 * have been trimmed from this project, so only case-sensitive matching is
 * compiled here.  If case-insensitive matching is ever wanted again, the
 * multibyte case needs care: tolower() has a single-byte API, so the strings
 * have to be downcased up front rather than folding characters on the fly.
 */


#define NextByte(p, plen)	((p)++, (plen)--)

/* Set up to compile like_match.c for multibyte characters */
#define CHAREQ(p1, p1len, p2, p2len) wchareq((p1), (p1len), (p2), (p2len))
#define NextChar(p, plen) \
	do { int __l = pg_mblen_with_len((p), (plen)); (p) +=__l; (plen) -=__l; } while (0)
#define CopyAdvChar(dst, src, srclen) \
	do { int __l = pg_mblen_with_len((src), (srclen)); \
		 (srclen) -= __l; \
		 while (__l-- > 0) \
			 *(dst)++ = *(src)++; \
	   } while (0)

#define MatchText	MB_MatchText

#include "like_match.c"

/* Set up to compile like_match.c for single-byte characters */
#define CHAREQ(p1, p1len, p2, p2len) (*(p1) == *(p2))
#define NextChar(p, plen) NextByte((p), (plen))
#define CopyAdvChar(dst, src, srclen) (*(dst)++ = *(src)++, (srclen)--)

#define MatchText	SB_MatchText

#include "like_match.c"

/* setup to compile like_match.c for UTF8 encoding, using fast NextChar */

#define NextChar(p, plen) \
	do { (p)++; (plen)--; } while ((plen) > 0 && (*(p) & 0xC0) == 0x80 )
#define MatchText	UTF8_MatchText

#include "like_match.c"

/* Generic for all cases not requiring inline case-folding */
static inline int
GenericMatchText(const char *s, int slen, const char *p, int plen, Oid collation)
{
	if (collation && !lc_ctype_is_c(collation) && collation != DEFAULT_COLLATION_OID)
	{
		pg_locale_t locale = pg_newlocale_from_collation(collation);

		if (locale && !locale->deterministic)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("nondeterministic collations are not supported for LIKE")));
	}

	if (pg_database_encoding_max_length() == 1)
		return SB_MatchText(s, slen, p, plen, 0, true);
	else if (GetDatabaseEncoding() == PG_UTF8)
		return UTF8_MatchText(s, slen, p, plen, 0, true);
	else
		return MB_MatchText(s, slen, p, plen, 0, true);
}

/*
 *	interface routines called by the function manager
 */

Datum
namelike(PG_FUNCTION_ARGS)
{
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_PP(1);
	bool		result;
	char	   *s,
			   *p;
	int			slen,
				plen;

	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA_ANY(pat);
	plen = VARSIZE_ANY_EXHDR(pat);

	result = (GenericMatchText(s, slen, p, plen, PG_GET_COLLATION()) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
namenlike(PG_FUNCTION_ARGS)
{
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_PP(1);
	bool		result;
	char	   *s,
			   *p;
	int			slen,
				plen;

	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA_ANY(pat);
	plen = VARSIZE_ANY_EXHDR(pat);

	result = (GenericMatchText(s, slen, p, plen, PG_GET_COLLATION()) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
textlike(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_PP(0);
	text	   *pat = PG_GETARG_TEXT_PP(1);
	bool		result;
	char	   *s,
			   *p;
	int			slen,
				plen;

	s = VARDATA_ANY(str);
	slen = VARSIZE_ANY_EXHDR(str);
	p = VARDATA_ANY(pat);
	plen = VARSIZE_ANY_EXHDR(pat);

	result = (GenericMatchText(s, slen, p, plen, PG_GET_COLLATION()) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
textnlike(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_PP(0);
	text	   *pat = PG_GETARG_TEXT_PP(1);
	bool		result;
	char	   *s,
			   *p;
	int			slen,
				plen;

	s = VARDATA_ANY(str);
	slen = VARSIZE_ANY_EXHDR(str);
	p = VARDATA_ANY(pat);
	plen = VARSIZE_ANY_EXHDR(pat);

	result = (GenericMatchText(s, slen, p, plen, PG_GET_COLLATION()) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
bytealike(PG_FUNCTION_ARGS)
{
	bytea	   *str = PG_GETARG_BYTEA_PP(0);
	bytea	   *pat = PG_GETARG_BYTEA_PP(1);
	bool		result;
	char	   *s,
			   *p;
	int			slen,
				plen;

	s = VARDATA_ANY(str);
	slen = VARSIZE_ANY_EXHDR(str);
	p = VARDATA_ANY(pat);
	plen = VARSIZE_ANY_EXHDR(pat);

	result = (SB_MatchText(s, slen, p, plen, 0, true) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
byteanlike(PG_FUNCTION_ARGS)
{
	bytea	   *str = PG_GETARG_BYTEA_PP(0);
	bytea	   *pat = PG_GETARG_BYTEA_PP(1);
	bool		result;
	char	   *s,
			   *p;
	int			slen,
				plen;

	s = VARDATA_ANY(str);
	slen = VARSIZE_ANY_EXHDR(str);
	p = VARDATA_ANY(pat);
	plen = VARSIZE_ANY_EXHDR(pat);

	result = (SB_MatchText(s, slen, p, plen, 0, true) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

/*
 * Case-insensitive versions of LIKE (nameiclike, nameicnlike, texticlike,
 * texticnlike) and the obsolete ESCAPE helpers (like_escape,
 * like_escape_bytea) have been trimmed from this project.
 */
