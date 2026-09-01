/*-------------------------------------------------------------------------
 * oracle_compat.c
 *	Oracle compatible functions.
 *
 * Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 *	Author: Edmund Mergl <E.Mergl@bawue.de>
 *	Multibyte enhancement: Tatsuo Ishii <ishii@postgresql.org>
 *
 *
 * IDENTIFICATION
 *	src/backend/utils/adt/oracle_compat.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

/*
 * towlower() and friends should be in <wctype.h>, but some pre-C99 systems
 * declare them in <wchar.h>, so include that too.
 */
#include <limits.h>
#include <wchar.h>
#ifdef HAVE_WCTYPE_H
#include <wctype.h>
#endif

#include "catalog/pg_collation.h"
#include "common/int.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/pg_locale.h"

static text *dotrim(const char *string, int stringlen,
					const char *set, int setlen,
					bool doltrim, bool dortrim);
static bytea *dobyteatrim(bytea *string, bytea *set,
						  bool doltrim, bool dortrim);
static char *str_tolower(const char *buff, size_t nbytes, Oid collid);
static char *str_toupper(const char *buff, size_t nbytes, Oid collid);
static char *str_initcap(const char *buff, size_t nbytes, Oid collid);
static char *asc_tolower(const char *buff, size_t nbytes);
static char *asc_toupper(const char *buff, size_t nbytes);
static char *asc_initcap(const char *buff, size_t nbytes);


/********************************************************************
 *
 * lower
 *
 * Syntax:
 *
 *	 text lower(text string)
 *
 * Purpose:
 *
 *	 Returns string, with all letters forced to lowercase.
 *
 ********************************************************************/

Datum
lower(PG_FUNCTION_ARGS)
{
	text	   *in_string = PG_GETARG_TEXT_PP(0);
	char	   *out_string;
	text	   *result;

	out_string = str_tolower(VARDATA_ANY(in_string),
							 VARSIZE_ANY_EXHDR(in_string),
							 PG_GET_COLLATION());
	result = cstring_to_text(out_string);
	pfree(out_string);

	PG_RETURN_TEXT_P(result);
}


/********************************************************************
 *
 * upper
 *
 * Syntax:
 *
 *	 text upper(text string)
 *
 * Purpose:
 *
 *	 Returns string, with all letters forced to uppercase.
 *
 ********************************************************************/

Datum
upper(PG_FUNCTION_ARGS)
{
	text	   *in_string = PG_GETARG_TEXT_PP(0);
	char	   *out_string;
	text	   *result;

	out_string = str_toupper(VARDATA_ANY(in_string),
							 VARSIZE_ANY_EXHDR(in_string),
							 PG_GET_COLLATION());
	result = cstring_to_text(out_string);
	pfree(out_string);

	PG_RETURN_TEXT_P(result);
}


/********************************************************************
 *
 * initcap
 *
 * Syntax:
 *
 *	 text initcap(text string)
 *
 * Purpose:
 *
 *	 Returns string, with first letter of each word in uppercase, all
 *	 other letters in lowercase. A word is defined as a sequence of
 *	 alphanumeric characters, delimited by non-alphanumeric
 *	 characters.
 *
 ********************************************************************/

Datum
initcap(PG_FUNCTION_ARGS)
{
	text	   *in_string = PG_GETARG_TEXT_PP(0);
	char	   *out_string;
	text	   *result;

	out_string = str_initcap(VARDATA_ANY(in_string),
							 VARSIZE_ANY_EXHDR(in_string),
							 PG_GET_COLLATION());
	result = cstring_to_text(out_string);
	pfree(out_string);

	PG_RETURN_TEXT_P(result);
}




/*****************************************************************************
 *			Case conversion helpers for upper/lower/initcap
 *
 * If the system provides the needed functions for wide-character manipulation
 * (which are all standardized by C99), then we implement upper/lower/initcap
 * using wide-character functions, if necessary.  Otherwise we use the
 * traditional <ctype.h> functions, which of course will not work as desired
 * in multibyte character sets.  Note that in either case we are effectively
 * assuming that the database character encoding matches the encoding implied
 * by LC_CTYPE.
 *
 * If the system provides locale_t and associated functions (which are
 * standardized by Open Group's XBD), we can support collations that are
 * neither default nor C.  The code is written to handle both combinations
 * of have-wide-characters and have-locale_t, though it's rather unlikely
 * a platform would have the latter without the former.
 *****************************************************************************/

/*
 * collation-aware, wide-character-aware lower function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
static char *
str_tolower(const char *buff, size_t nbytes, Oid collid)
{
	char	   *result;

	if (!buff)
		return NULL;

	/* C/POSIX collations use this path regardless of database encoding */
	if (lc_ctype_is_c(collid))
	{
		result = asc_tolower(buff, nbytes);
	}
	else
	{
		pg_locale_t mylocale = 0;

		if (collid != DEFAULT_COLLATION_OID)
		{
			if (!OidIsValid(collid))
			{
				/*
				 * This typically means that the parser could not resolve a
				 * conflict of implicit collations, so report it that way.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_COLLATION),
						 errmsg("could not determine which collation to use for %s function",
								"lower()"),
						 errhint("Use the COLLATE clause to set the collation explicitly.")));
			}
			mylocale = pg_newlocale_from_collation(collid);
		}

		{
			if (pg_database_encoding_max_length() > 1)
			{
				wchar_t    *workspace;
				size_t		curr_char;
				size_t		result_size;

				/* Overflow paranoia */
				if ((nbytes + 1) > (INT_MAX / sizeof(wchar_t)))
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("out of memory")));

				/* Output workspace cannot have more codes than input bytes */
				workspace = (wchar_t *) palloc((nbytes + 1) * sizeof(wchar_t));

				char2wchar(workspace, nbytes + 1, buff, nbytes, mylocale);

				for (curr_char = 0; workspace[curr_char] != 0; curr_char++)
				{
#ifdef HAVE_LOCALE_T
					if (mylocale)
						workspace[curr_char] = towlower_l(workspace[curr_char], mylocale->info.lt);
					else
#endif
						workspace[curr_char] = towlower(workspace[curr_char]);
				}

				/*
				 * Make result large enough; case change might change number
				 * of bytes
				 */
				result_size = curr_char * pg_database_encoding_max_length() + 1;
				result = palloc(result_size);

				wchar2char(result, workspace, result_size, mylocale);
				pfree(workspace);
			}
			else
			{
				char	   *p;

				result = pnstrdup(buff, nbytes);

				/*
				 * Note: we assume that tolower_l() will not be so broken as
				 * to need an isupper_l() guard test.  When using the default
				 * collation, we apply the traditional Postgres behavior that
				 * forces ASCII-style treatment of I/i, but in non-default
				 * collations you get exactly what the collation says.
				 */
				for (p = result; *p; p++)
				{
#ifdef HAVE_LOCALE_T
					if (mylocale)
						*p = tolower_l((unsigned char) *p, mylocale->info.lt);
					else
#endif
						*p = pg_tolower((unsigned char) *p);
				}
			}
		}
	}

	return result;
}

/*
 * collation-aware, wide-character-aware upper function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
static char *
str_toupper(const char *buff, size_t nbytes, Oid collid)
{
	char	   *result;

	if (!buff)
		return NULL;

	/* C/POSIX collations use this path regardless of database encoding */
	if (lc_ctype_is_c(collid))
	{
		result = asc_toupper(buff, nbytes);
	}
	else
	{
		pg_locale_t mylocale = 0;

		if (collid != DEFAULT_COLLATION_OID)
		{
			if (!OidIsValid(collid))
			{
				/*
				 * This typically means that the parser could not resolve a
				 * conflict of implicit collations, so report it that way.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_COLLATION),
						 errmsg("could not determine which collation to use for %s function",
								"upper()"),
						 errhint("Use the COLLATE clause to set the collation explicitly.")));
			}
			mylocale = pg_newlocale_from_collation(collid);
		}

		{
			if (pg_database_encoding_max_length() > 1)
			{
				wchar_t    *workspace;
				size_t		curr_char;
				size_t		result_size;

				/* Overflow paranoia */
				if ((nbytes + 1) > (INT_MAX / sizeof(wchar_t)))
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("out of memory")));

				/* Output workspace cannot have more codes than input bytes */
				workspace = (wchar_t *) palloc((nbytes + 1) * sizeof(wchar_t));

				char2wchar(workspace, nbytes + 1, buff, nbytes, mylocale);

				for (curr_char = 0; workspace[curr_char] != 0; curr_char++)
				{
#ifdef HAVE_LOCALE_T
					if (mylocale)
						workspace[curr_char] = towupper_l(workspace[curr_char], mylocale->info.lt);
					else
#endif
						workspace[curr_char] = towupper(workspace[curr_char]);
				}

				/*
				 * Make result large enough; case change might change number
				 * of bytes
				 */
				result_size = curr_char * pg_database_encoding_max_length() + 1;
				result = palloc(result_size);

				wchar2char(result, workspace, result_size, mylocale);
				pfree(workspace);
			}
			else
			{
				char	   *p;

				result = pnstrdup(buff, nbytes);

				/*
				 * Note: we assume that toupper_l() will not be so broken as
				 * to need an islower_l() guard test.  When using the default
				 * collation, we apply the traditional Postgres behavior that
				 * forces ASCII-style treatment of I/i, but in non-default
				 * collations you get exactly what the collation says.
				 */
				for (p = result; *p; p++)
				{
#ifdef HAVE_LOCALE_T
					if (mylocale)
						*p = toupper_l((unsigned char) *p, mylocale->info.lt);
					else
#endif
						*p = pg_toupper((unsigned char) *p);
				}
			}
		}
	}

	return result;
}

/*
 * collation-aware, wide-character-aware initcap function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
static char *
str_initcap(const char *buff, size_t nbytes, Oid collid)
{
	char	   *result;
	int			wasalnum = false;

	if (!buff)
		return NULL;

	/* C/POSIX collations use this path regardless of database encoding */
	if (lc_ctype_is_c(collid))
	{
		result = asc_initcap(buff, nbytes);
	}
	else
	{
		pg_locale_t mylocale = 0;

		if (collid != DEFAULT_COLLATION_OID)
		{
			if (!OidIsValid(collid))
			{
				/*
				 * This typically means that the parser could not resolve a
				 * conflict of implicit collations, so report it that way.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_COLLATION),
						 errmsg("could not determine which collation to use for %s function",
								"initcap()"),
						 errhint("Use the COLLATE clause to set the collation explicitly.")));
			}
			mylocale = pg_newlocale_from_collation(collid);
		}

		{
			if (pg_database_encoding_max_length() > 1)
			{
				wchar_t    *workspace;
				size_t		curr_char;
				size_t		result_size;

				/* Overflow paranoia */
				if ((nbytes + 1) > (INT_MAX / sizeof(wchar_t)))
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("out of memory")));

				/* Output workspace cannot have more codes than input bytes */
				workspace = (wchar_t *) palloc((nbytes + 1) * sizeof(wchar_t));

				char2wchar(workspace, nbytes + 1, buff, nbytes, mylocale);

				for (curr_char = 0; workspace[curr_char] != 0; curr_char++)
				{
#ifdef HAVE_LOCALE_T
					if (mylocale)
					{
						if (wasalnum)
							workspace[curr_char] = towlower_l(workspace[curr_char], mylocale->info.lt);
						else
							workspace[curr_char] = towupper_l(workspace[curr_char], mylocale->info.lt);
						wasalnum = iswalnum_l(workspace[curr_char], mylocale->info.lt);
					}
					else
#endif
					{
						if (wasalnum)
							workspace[curr_char] = towlower(workspace[curr_char]);
						else
							workspace[curr_char] = towupper(workspace[curr_char]);
						wasalnum = iswalnum(workspace[curr_char]);
					}
				}

				/*
				 * Make result large enough; case change might change number
				 * of bytes
				 */
				result_size = curr_char * pg_database_encoding_max_length() + 1;
				result = palloc(result_size);

				wchar2char(result, workspace, result_size, mylocale);
				pfree(workspace);
			}
			else
			{
				char	   *p;

				result = pnstrdup(buff, nbytes);

				/*
				 * Note: we assume that toupper_l()/tolower_l() will not be so
				 * broken as to need guard tests.  When using the default
				 * collation, we apply the traditional Postgres behavior that
				 * forces ASCII-style treatment of I/i, but in non-default
				 * collations you get exactly what the collation says.
				 */
				for (p = result; *p; p++)
				{
#ifdef HAVE_LOCALE_T
					if (mylocale)
					{
						if (wasalnum)
							*p = tolower_l((unsigned char) *p, mylocale->info.lt);
						else
							*p = toupper_l((unsigned char) *p, mylocale->info.lt);
						wasalnum = isalnum_l((unsigned char) *p, mylocale->info.lt);
					}
					else
#endif
					{
						if (wasalnum)
							*p = pg_tolower((unsigned char) *p);
						else
							*p = pg_toupper((unsigned char) *p);
						wasalnum = isalnum((unsigned char) *p);
					}
				}
			}
		}
	}

	return result;
}

/*
 * ASCII-only lower function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
static char *
asc_tolower(const char *buff, size_t nbytes)
{
	char	   *result;
	char	   *p;

	if (!buff)
		return NULL;

	result = pnstrdup(buff, nbytes);

	for (p = result; *p; p++)
		*p = pg_ascii_tolower((unsigned char) *p);

	return result;
}

/*
 * ASCII-only upper function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
static char *
asc_toupper(const char *buff, size_t nbytes)
{
	char	   *result;
	char	   *p;

	if (!buff)
		return NULL;

	result = pnstrdup(buff, nbytes);

	for (p = result; *p; p++)
		*p = pg_ascii_toupper((unsigned char) *p);

	return result;
}

/*
 * ASCII-only initcap function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
static char *
asc_initcap(const char *buff, size_t nbytes)
{
	char	   *result;
	char	   *p;
	int			wasalnum = false;

	if (!buff)
		return NULL;

	result = pnstrdup(buff, nbytes);

	for (p = result; *p; p++)
	{
		char		c;

		if (wasalnum)
			*p = c = pg_ascii_tolower((unsigned char) *p);
		else
			*p = c = pg_ascii_toupper((unsigned char) *p);
		/* we don't trust isalnum() here */
		wasalnum = ((c >= 'A' && c <= 'Z') ||
					(c >= 'a' && c <= 'z') ||
					(c >= '0' && c <= '9'));
	}

	return result;
}


/********************************************************************
 *
 * btrim
 *
 * Syntax:
 *
 *	 text btrim(text string, text set)
 *
 * Purpose:
 *
 *	 Returns string with characters removed from the front and back
 *	 up to the first character not in set.
 *
 ********************************************************************/

Datum
btrim(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	text	   *set = PG_GETARG_TEXT_PP(1);
	text	   *ret;

	ret = dotrim(VARDATA_ANY(string), VARSIZE_ANY_EXHDR(string),
				 VARDATA_ANY(set), VARSIZE_ANY_EXHDR(set),
				 true, true);

	PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * btrim1 --- btrim with set fixed as ' '
 *
 ********************************************************************/

Datum
btrim1(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	text	   *ret;

	ret = dotrim(VARDATA_ANY(string), VARSIZE_ANY_EXHDR(string),
				 " ", 1,
				 true, true);

	PG_RETURN_TEXT_P(ret);
}

/*
 * Common implementation for btrim, ltrim, rtrim
 */
static text *
dotrim(const char *string, int stringlen,
	   const char *set, int setlen,
	   bool doltrim, bool dortrim)
{
	int			i;

	/* Nothing to do if either string or set is empty */
	if (stringlen > 0 && setlen > 0)
	{
		if (pg_database_encoding_max_length() > 1)
		{
			/*
			 * In the multibyte-encoding case, build arrays of pointers to
			 * character starts, so that we can avoid inefficient checks in
			 * the inner loops.
			 */
			const char **stringchars;
			const char **setchars;
			const char *setend;
			int		   *stringmblen;
			int		   *setmblen;
			int			stringnchars;
			int			setnchars;
			int			resultndx;
			int			resultnchars;
			const char *p;
			const char *pend;
			int			len;
			int			mblen;
			const char *str_pos;
			int			str_len;

			stringchars = (const char **) palloc(stringlen * sizeof(char *));
			stringmblen = (int *) palloc(stringlen * sizeof(int));
			stringnchars = 0;
			p = string;
			len = stringlen;
			pend = p + len;
			while (len > 0)
			{
				stringchars[stringnchars] = p;
				stringmblen[stringnchars] = mblen = pg_mblen_range(p, pend);
				stringnchars++;
				p += mblen;
				len -= mblen;
			}

			setchars = (const char **) palloc(setlen * sizeof(char *));
			setmblen = (int *) palloc(setlen * sizeof(int));
			setnchars = 0;
			p = set;
			len = setlen;
			setend = set + setlen;
			while (len > 0)
			{
				setchars[setnchars] = p;
				setmblen[setnchars] = mblen = pg_mblen_range(p, setend);
				setnchars++;
				p += mblen;
				len -= mblen;
			}

			resultndx = 0;		/* index in stringchars[] */
			resultnchars = stringnchars;

			if (doltrim)
			{
				while (resultnchars > 0)
				{
					str_pos = stringchars[resultndx];
					str_len = stringmblen[resultndx];
					for (i = 0; i < setnchars; i++)
					{
						if (str_len == setmblen[i] &&
							memcmp(str_pos, setchars[i], str_len) == 0)
							break;
					}
					if (i >= setnchars)
						break;	/* no match here */
					string += str_len;
					stringlen -= str_len;
					resultndx++;
					resultnchars--;
				}
			}

			if (dortrim)
			{
				while (resultnchars > 0)
				{
					str_pos = stringchars[resultndx + resultnchars - 1];
					str_len = stringmblen[resultndx + resultnchars - 1];
					for (i = 0; i < setnchars; i++)
					{
						if (str_len == setmblen[i] &&
							memcmp(str_pos, setchars[i], str_len) == 0)
							break;
					}
					if (i >= setnchars)
						break;	/* no match here */
					stringlen -= str_len;
					resultnchars--;
				}
			}

			pfree(stringchars);
			pfree(stringmblen);
			pfree(setchars);
			pfree(setmblen);
		}
		else
		{
			/*
			 * In the single-byte-encoding case, we don't need such overhead.
			 */
			if (doltrim)
			{
				while (stringlen > 0)
				{
					char		str_ch = *string;

					for (i = 0; i < setlen; i++)
					{
						if (str_ch == set[i])
							break;
					}
					if (i >= setlen)
						break;	/* no match here */
					string++;
					stringlen--;
				}
			}

			if (dortrim)
			{
				while (stringlen > 0)
				{
					char		str_ch = string[stringlen - 1];

					for (i = 0; i < setlen; i++)
					{
						if (str_ch == set[i])
							break;
					}
					if (i >= setlen)
						break;	/* no match here */
					stringlen--;
				}
			}
		}
	}

	/* Return selected portion of string */
	return cstring_to_text_with_len(string, stringlen);
}

/*
 * Common implementation for bytea versions of btrim, ltrim, rtrim
 */
bytea *
dobyteatrim(bytea *string, bytea *set, bool doltrim, bool dortrim)
{
	bytea	   *ret;
	char	   *ptr,
			   *end,
			   *ptr2,
			   *ptr2start,
			   *end2;
	int			m,
				stringlen,
				setlen;

	stringlen = VARSIZE_ANY_EXHDR(string);
	setlen = VARSIZE_ANY_EXHDR(set);

	if (stringlen <= 0 || setlen <= 0)
		return string;

	m = stringlen;
	ptr = VARDATA_ANY(string);
	end = ptr + stringlen - 1;
	ptr2start = VARDATA_ANY(set);
	end2 = ptr2start + setlen - 1;

	if (doltrim)
	{
		while (m > 0)
		{
			ptr2 = ptr2start;
			while (ptr2 <= end2)
			{
				if (*ptr == *ptr2)
					break;
				++ptr2;
			}
			if (ptr2 > end2)
				break;
			ptr++;
			m--;
		}
	}

	if (dortrim)
	{
		while (m > 0)
		{
			ptr2 = ptr2start;
			while (ptr2 <= end2)
			{
				if (*end == *ptr2)
					break;
				++ptr2;
			}
			if (ptr2 > end2)
				break;
			end--;
			m--;
		}
	}

	ret = (bytea *) palloc(VARHDRSZ + m);
	SET_VARSIZE(ret, VARHDRSZ + m);
	memcpy(VARDATA(ret), ptr, m);
	return ret;
}

/********************************************************************
 *
 * byteatrim
 *
 * Syntax:
 *
 *	 bytea byteatrim(bytea string, bytea set)
 *
 * Purpose:
 *
 *	 Returns string with characters removed from the front and back
 *	 up to the first character not in set.
 *
 * Cloned from btrim and modified as required.
 ********************************************************************/

Datum
byteatrim(PG_FUNCTION_ARGS)
{
	bytea	   *string = PG_GETARG_BYTEA_PP(0);
	bytea	   *set = PG_GETARG_BYTEA_PP(1);
	bytea	   *ret;

	ret = dobyteatrim(string, set, true, true);

	PG_RETURN_BYTEA_P(ret);
}

/********************************************************************
 *
 * bytealtrim
 *
 * Syntax:
 *
 *	 bytea bytealtrim(bytea string, bytea set)
 *
 * Purpose:
 *
 *	 Returns string with initial characters removed up to the first
 *	 character not in set.
 *
 ********************************************************************/

Datum
bytealtrim(PG_FUNCTION_ARGS)
{
	bytea	   *string = PG_GETARG_BYTEA_PP(0);
	bytea	   *set = PG_GETARG_BYTEA_PP(1);
	bytea	   *ret;

	ret = dobyteatrim(string, set, true, false);

	PG_RETURN_BYTEA_P(ret);
}

/********************************************************************
 *
 * byteartrim
 *
 * Syntax:
 *
 *	 bytea byteartrim(bytea string, bytea set)
 *
 * Purpose:
 *
 *	 Returns string with final characters removed after the last
 *	 character not in set.
 *
 ********************************************************************/

Datum
byteartrim(PG_FUNCTION_ARGS)
{
	bytea	   *string = PG_GETARG_BYTEA_PP(0);
	bytea	   *set = PG_GETARG_BYTEA_PP(1);
	bytea	   *ret;

	ret = dobyteatrim(string, set, false, true);

	PG_RETURN_BYTEA_P(ret);
}

/********************************************************************
 *
 * ltrim
 *
 * Syntax:
 *
 *	 text ltrim(text string, text set)
 *
 * Purpose:
 *
 *	 Returns string with initial characters removed up to the first
 *	 character not in set.
 *
 ********************************************************************/

Datum
ltrim(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	text	   *set = PG_GETARG_TEXT_PP(1);
	text	   *ret;

	ret = dotrim(VARDATA_ANY(string), VARSIZE_ANY_EXHDR(string),
				 VARDATA_ANY(set), VARSIZE_ANY_EXHDR(set),
				 true, false);

	PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * ltrim1 --- ltrim with set fixed as ' '
 *
 ********************************************************************/

Datum
ltrim1(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	text	   *ret;

	ret = dotrim(VARDATA_ANY(string), VARSIZE_ANY_EXHDR(string),
				 " ", 1,
				 true, false);

	PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * rtrim
 *
 * Syntax:
 *
 *	 text rtrim(text string, text set)
 *
 * Purpose:
 *
 *	 Returns string with final characters removed after the last
 *	 character not in set.
 *
 ********************************************************************/

Datum
rtrim(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	text	   *set = PG_GETARG_TEXT_PP(1);
	text	   *ret;

	ret = dotrim(VARDATA_ANY(string), VARSIZE_ANY_EXHDR(string),
				 VARDATA_ANY(set), VARSIZE_ANY_EXHDR(set),
				 false, true);

	PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * rtrim1 --- rtrim with set fixed as ' '
 *
 ********************************************************************/

Datum
rtrim1(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	text	   *ret;

	ret = dotrim(VARDATA_ANY(string), VARSIZE_ANY_EXHDR(string),
				 " ", 1,
				 false, true);

	PG_RETURN_TEXT_P(ret);
}


/********************************************************************
 *
 * translate
 *
 * Syntax:
 *
 *	 text translate(text string, text from, text to)
 *
 * Purpose:
 *
 *	 Returns string after replacing all occurrences of characters in from
 *	 with the corresponding character in to.  If from is longer than to,
 *	 occurrences of the extra characters in from are deleted.
 *	 Improved by Edwin Ramirez <ramirez@doc.mssm.edu>.
 *
 ********************************************************************/

Datum
translate(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	text	   *from = PG_GETARG_TEXT_PP(1);
	text	   *to = PG_GETARG_TEXT_PP(2);
	text	   *result;
	char	   *from_ptr,
			   *to_ptr,
			   *to_end;
	char	   *source,
			   *target;
	const char *source_end;
	const char *from_end;
	int			m,
				fromlen,
				tolen,
				retlen,
				i;
	int			worst_len;
	int			len;
	int			source_len;
	int			from_index;

	m = VARSIZE_ANY_EXHDR(string);
	if (m <= 0)
		PG_RETURN_TEXT_P(string);
	source = VARDATA_ANY(string);
	source_end = source + m;

	fromlen = VARSIZE_ANY_EXHDR(from);
	from_ptr = VARDATA_ANY(from);
	from_end = from_ptr + fromlen;
	tolen = VARSIZE_ANY_EXHDR(to);
	to_ptr = VARDATA_ANY(to);
	to_end = to_ptr + tolen;

	/*
	 * The worst-case expansion is to substitute a max-length character for a
	 * single-byte character at each position of the string.
	 */
	worst_len = pg_database_encoding_max_length() * m;

	/* check for integer overflow */
	if (worst_len / pg_database_encoding_max_length() != m)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested length too large")));

	result = (text *) palloc(worst_len + VARHDRSZ);
	target = VARDATA(result);
	retlen = 0;

	while (m > 0)
	{
		source_len = pg_mblen_range(source, source_end);
		from_index = 0;

		for (i = 0; i < fromlen; i += len)
		{
			len = pg_mblen_range(&from_ptr[i], from_end);
			if (len == source_len &&
				memcmp(source, &from_ptr[i], len) == 0)
				break;

			from_index++;
		}
		if (i < fromlen)
		{
			/* substitute, or delete if no corresponding "to" character */
			char	   *p = to_ptr;

			for (i = 0; i < from_index; i++)
			{
				if (p >= to_end)
					break;
				p += pg_mblen_range(p, to_end);
			}
			if (p < to_end)
			{
				len = pg_mblen_range(p, to_end);
				memcpy(target, p, len);
				target += len;
				retlen += len;
			}

		}
		else
		{
			/* no match, so copy */
			memcpy(target, source, source_len);
			target += source_len;
			retlen += source_len;
		}

		source += source_len;
		m -= source_len;
	}

	SET_VARSIZE(result, retlen + VARHDRSZ);

	/*
	 * The function result is probably much bigger than needed, if we're using
	 * a multibyte encoding, but it's not worth reallocating it; the result
	 * probably won't live long anyway.
	 */

	PG_RETURN_TEXT_P(result);
}

/********************************************************************
 *
 * ascii
 *
 * Syntax:
 *
 *	 int ascii(text string)
 *
 * Purpose:
 *
 *	 Returns the decimal representation of the first character from
 *	 string.
 *	 If the string is empty we return 0.
 *	 If the database encoding is UTF8, we return the Unicode codepoint.
 *	 If the database encoding is any other multi-byte encoding, we
 *	 return the value of the first byte if it is an ASCII character
 *	 (range 1 .. 127), or raise an error.
 *	 For all other encodings we return the value of the first byte,
 *	 (range 1..255).
 *
 ********************************************************************/

Datum
ascii(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	int			encoding = GetDatabaseEncoding();
	unsigned char *data;

	if (VARSIZE_ANY_EXHDR(string) <= 0)
		PG_RETURN_INT32(0);

	data = (unsigned char *) VARDATA_ANY(string);

	if (encoding == PG_UTF8 && *data > 127)
	{
		/* return the code point for Unicode */

		int			result = 0,
					tbytes = 0,
					i;

		if (*data >= 0xF0)
		{
			result = *data & 0x07;
			tbytes = 3;
		}
		else if (*data >= 0xE0)
		{
			result = *data & 0x0F;
			tbytes = 2;
		}
		else
		{
			Assert(*data > 0xC0);
			result = *data & 0x1f;
			tbytes = 1;
		}

		Assert(tbytes > 0);

		for (i = 1; i <= tbytes; i++)
		{
			Assert((data[i] & 0xC0) == 0x80);
			result = (result << 6) + (data[i] & 0x3f);
		}

		PG_RETURN_INT32(result);
	}
	else
	{
		if (pg_encoding_max_length(encoding) > 1 && *data > 127)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("requested character too large")));


		PG_RETURN_INT32((int32) *data);
	}
}

/********************************************************************
 *
 * chr
 *
 * Syntax:
 *
 *	 text chr(int val)
 *
 * Purpose:
 *
 *	Returns the character having the binary equivalent to val.
 *
 * For UTF8 we treat the argument as a Unicode code point.
 * For other multi-byte encodings we raise an error for arguments
 * outside the strict ASCII range (1..127).
 *
 * It's important that we don't ever return a value that is not valid
 * in the database encoding, so that this doesn't become a way for
 * invalid data to enter the database.
 *
 ********************************************************************/

Datum
chr			(PG_FUNCTION_ARGS)
{
	uint32		cvalue = PG_GETARG_UINT32(0);
	text	   *result;
	int			encoding = GetDatabaseEncoding();

	if (encoding == PG_UTF8 && cvalue > 127)
	{
		/* for Unicode we treat the argument as a code point */
		int			bytes;
		unsigned char *wch;

		/*
		 * We only allow valid Unicode code points; per RFC3629 that stops at
		 * U+10FFFF, even though 4-byte UTF8 sequences can hold values up to
		 * U+1FFFFF.
		 */
		if (cvalue > 0x0010ffff)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("requested character too large for encoding: %d",
							cvalue)));

		if (cvalue > 0xffff)
			bytes = 4;
		else if (cvalue > 0x07ff)
			bytes = 3;
		else
			bytes = 2;

		result = (text *) palloc(VARHDRSZ + bytes);
		SET_VARSIZE(result, VARHDRSZ + bytes);
		wch = (unsigned char *) VARDATA(result);

		if (bytes == 2)
		{
			wch[0] = 0xC0 | ((cvalue >> 6) & 0x1F);
			wch[1] = 0x80 | (cvalue & 0x3F);
		}
		else if (bytes == 3)
		{
			wch[0] = 0xE0 | ((cvalue >> 12) & 0x0F);
			wch[1] = 0x80 | ((cvalue >> 6) & 0x3F);
			wch[2] = 0x80 | (cvalue & 0x3F);
		}
		else
		{
			wch[0] = 0xF0 | ((cvalue >> 18) & 0x07);
			wch[1] = 0x80 | ((cvalue >> 12) & 0x3F);
			wch[2] = 0x80 | ((cvalue >> 6) & 0x3F);
			wch[3] = 0x80 | (cvalue & 0x3F);
		}

		/*
		 * The preceding range check isn't sufficient, because UTF8 excludes
		 * Unicode "surrogate pair" codes.  Make sure what we created is valid
		 * UTF8.
		 */
		if (!pg_utf8_islegal(wch, bytes))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("requested character not valid for encoding: %d",
							cvalue)));
	}
	else
	{
		bool		is_mb;

		/*
		 * Error out on arguments that make no sense or that we can't validly
		 * represent in the encoding.
		 */
		if (cvalue == 0)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("null character not permitted")));

		is_mb = pg_encoding_max_length(encoding) > 1;

		if ((is_mb && (cvalue > 127)) || (!is_mb && (cvalue > 255)))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("requested character too large for encoding: %d",
							cvalue)));

		result = (text *) palloc(VARHDRSZ + 1);
		SET_VARSIZE(result, VARHDRSZ + 1);
		*VARDATA(result) = (char) cvalue;
	}

	PG_RETURN_TEXT_P(result);
}

/********************************************************************
 *
 * repeat
 *
 * Syntax:
 *
 *	 text repeat(text string, int val)
 *
 * Purpose:
 *
 *	Repeat string by val.
 *
 ********************************************************************/

Datum
repeat(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	int32		count = PG_GETARG_INT32(1);
	text	   *result;
	int			slen,
				tlen;
	int			i;
	char	   *cp,
			   *sp;

	if (count < 0)
		count = 0;

	slen = VARSIZE_ANY_EXHDR(string);

	if (unlikely(pg_mul_s32_overflow(count, slen, &tlen)) ||
		unlikely(pg_add_s32_overflow(tlen, VARHDRSZ, &tlen)))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested length too large")));

	result = (text *) palloc(tlen);

	SET_VARSIZE(result, tlen);
	cp = VARDATA(result);
	sp = VARDATA_ANY(string);
	for (i = 0; i < count; i++)
	{
		memcpy(cp, sp, slen);
		cp += slen;
		CHECK_FOR_INTERRUPTS();
	}

	PG_RETURN_TEXT_P(result);
}
