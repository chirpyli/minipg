/*-------------------------------------------------------------------------
 *
 * wchar.c
 *	  Functions for working with multibyte characters in various encodings.
 *
 * Portions Copyright (c) 1998-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/wchar.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <limits.h>

#include "mb/pg_wchar.h"


/*
 * In today's multibyte encodings other than UTF8, this two-byte sequence
 * ensures pg_encoding_mblen() == 2 && pg_encoding_verifymbstr() == 0.
 */
#define NONUTF8_INVALID_BYTE0 (0x8d)
#define NONUTF8_INVALID_BYTE1 (' ')


/*
 * Operations on multi-byte encodings are driven by a table of helper
 * functions.
 *
 * To add an encoding support, define mblen(), dsplen(), verifychar() and
 * verifystr() for the encoding.  For server-encodings, also define mb2wchar()
 * and wchar2mb() conversion functions.
 *
 * minipg supports only three encodings: SQL_ASCII, UTF8 and LATIN1
 * (ISO-8859-1).  All other encodings were removed to slim down the character
 * set machinery.
 */
#define MB2CHAR_NEED_AT_LEAST(len, need) if ((len) < (need)) break

/*
 * SQL/ASCII
 */
static int
pg_ascii2wchar_with_len(const unsigned char *from, pg_wchar *to, int len)
{
	int			cnt = 0;

	while (len > 0 && *from)
	{
		*to++ = *from++;
		len--;
		cnt++;
	}
	*to = 0;
	return cnt;
}

static int
pg_ascii_mblen(const unsigned char *s)
{
	return 1;
}

static int
pg_ascii_dsplen(const unsigned char *s)
{
	if (*s == '\0')
		return 0;
	if (*s < 0x20 || *s == 0x7f)
		return -1;

	return 1;
}

/*
 * convert UTF8 string to pg_wchar (UCS-4)
 * caller must allocate enough space for "to", including a trailing zero!
 * len: length of from.
 * "from" not necessarily null terminated.
 */
static int
pg_utf2wchar_with_len(const unsigned char *from, pg_wchar *to, int len)
{
	int			cnt = 0;
	uint32		c1,
				c2,
				c3,
				c4;

	while (len > 0 && *from)
	{
		if ((*from & 0x80) == 0)
		{
			*to = *from++;
			len--;
		}
		else if ((*from & 0xe0) == 0xc0)
		{
			MB2CHAR_NEED_AT_LEAST(len, 2);
			c1 = *from++ & 0x1f;
			c2 = *from++ & 0x3f;
			*to = (c1 << 6) | c2;
			len -= 2;
		}
		else if ((*from & 0xf0) == 0xe0)
		{
			MB2CHAR_NEED_AT_LEAST(len, 3);
			c1 = *from++ & 0x0f;
			c2 = *from++ & 0x3f;
			c3 = *from++ & 0x3f;
			*to = (c1 << 12) | (c2 << 6) | c3;
			len -= 3;
		}
		else if ((*from & 0xf8) == 0xf0)
		{
			MB2CHAR_NEED_AT_LEAST(len, 4);
			c1 = *from++ & 0x07;
			c2 = *from++ & 0x3f;
			c3 = *from++ & 0x3f;
			c4 = *from++ & 0x3f;
			*to = (c1 << 18) | (c2 << 12) | (c3 << 6) | c4;
			len -= 4;
		}
		else
		{
			/* treat a bogus char as length 1; not ours to raise error */
			*to = *from++;
			len--;
		}
		to++;
		cnt++;
	}
	*to = 0;
	return cnt;
}


/*
 * Map a Unicode code point to UTF-8.  utf8string must have 4 bytes of
 * space allocated.
 */
unsigned char *
unicode_to_utf8(pg_wchar c, unsigned char *utf8string)
{
	if (c <= 0x7F)
	{
		utf8string[0] = c;
	}
	else if (c <= 0x7FF)
	{
		utf8string[0] = 0xC0 | ((c >> 6) & 0x1F);
		utf8string[1] = 0x80 | (c & 0x3F);
	}
	else if (c <= 0xFFFF)
	{
		utf8string[0] = 0xE0 | ((c >> 12) & 0x0F);
		utf8string[1] = 0x80 | ((c >> 6) & 0x3F);
		utf8string[2] = 0x80 | (c & 0x3F);
	}
	else
	{
		utf8string[0] = 0xF0 | ((c >> 18) & 0x07);
		utf8string[1] = 0x80 | ((c >> 12) & 0x3F);
		utf8string[2] = 0x80 | ((c >> 6) & 0x3F);
		utf8string[3] = 0x80 | (c & 0x3F);
	}

	return utf8string;
}

/*
 * Trivial conversion from pg_wchar to UTF-8.
 * caller should allocate enough space for "to"
 * len: length of from.
 * "from" not necessarily null terminated.
 */
static int
pg_wchar2utf_with_len(const pg_wchar *from, unsigned char *to, int len)
{
	int			cnt = 0;

	while (len > 0 && *from)
	{
		int			char_len;

		unicode_to_utf8(*from, to);
		char_len = pg_utf_mblen(to);
		cnt += char_len;
		to += char_len;
		from++;
		len--;
	}
	*to = 0;
	return cnt;
}

/*
 * Return the byte length of a UTF8 character pointed to by s
 *
 * Note: in the current implementation we do not support UTF8 sequences
 * of more than 4 bytes; hence do NOT return a value larger than 4.
 * We return "1" for any leading byte that is either flat-out illegal or
 * indicates a length larger than we support.
 *
 * pg_utf2wchar_with_len(), utf8_to_unicode(), pg_utf8_islegal(), and perhaps
 * other places would need to be fixed to change this.
 */
int
pg_utf_mblen(const unsigned char *s)
{
	int			len;

	if ((*s & 0x80) == 0)
		len = 1;
	else if ((*s & 0xe0) == 0xc0)
		len = 2;
	else if ((*s & 0xf0) == 0xe0)
		len = 3;
	else if ((*s & 0xf8) == 0xf0)
		len = 4;
#ifdef NOT_USED
	else if ((*s & 0xfc) == 0xf8)
		len = 5;
	else if ((*s & 0xfe) == 0xfc)
		len = 6;
#endif
	else
		len = 1;
	return len;
}

/*
 * This is an implementation of wcwidth() and wcswidth() as defined in
 * "The Single UNIX Specification, Version 2, The Open Group, 1997"
 * <http://www.unix.org/online.html>
 *
 * Markus Kuhn -- 2001-09-08 -- public domain
 *
 * customised for PostgreSQL
 *
 * original available at : http://www.cl.cam.ac.uk/~mgk25/ucs/wcwidth.c
 */

/*
 * The following functions define the column width of an ISO 10646
 * character as follows:
 *
 *	  - The null character (U+0000) has a column width of 0.
 *
 *	  - Other C0/C1 control characters and DEL will lead to a return
 *		value of -1.
 *
 *	  - Spacing characters in the East Asian Wide (W) or East Asian
 *		FullWidth (F) category as defined in Unicode Technical
 *		Report #11 have a column width of 2.
 *
 *	  - All remaining characters (including non-spacing combining
 *		characters, Unicode format characters, all printable
 *		ISO 8859-1 and WGL4 characters, Unicode control characters,
 *		etc.) have a column width of 1.
 *
 * This implementation assumes that wchar_t characters are encoded
 * in ISO 10646.
 */

static int
ucs_wcwidth(pg_wchar ucs)
{
	/* test for 8-bit control characters */
	if (ucs == 0)
		return 0;

	if (ucs < 0x20 || (ucs >= 0x7f && ucs < 0xa0) || ucs > 0x0010ffff)
		return -1;

	/*
	 * if we arrive here, ucs is not a C0/C1 control character
	 */

	return 1 +
		(ucs >= 0x1100 &&
		 (ucs <= 0x115f ||		/* Hangul Jamo init. consonants */
		  (ucs >= 0x2e80 && ucs <= 0xa4cf && (ucs & ~0x0011) != 0x300a &&
		   ucs != 0x303f) ||	/* CJK ... Yi */
		  (ucs >= 0xac00 && ucs <= 0xd7a3) ||	/* Hangul Syllables */
		  (ucs >= 0xf900 && ucs <= 0xfaff) ||	/* CJK Compatibility
												 * Ideographs */
		  (ucs >= 0xfe30 && ucs <= 0xfe6f) ||	/* CJK Compatibility Forms */
		  (ucs >= 0xff00 && ucs <= 0xff5f) ||	/* Fullwidth Forms */
		  (ucs >= 0xffe0 && ucs <= 0xffe6) ||
		  (ucs >= 0x20000 && ucs <= 0x2ffff)));
}

/*
 * Convert a UTF-8 character to a Unicode code point.
 * This is a one-character version of pg_utf2wchar_with_len.
 *
 * No error checks here, c must point to a long-enough string.
 */
pg_wchar
utf8_to_unicode(const unsigned char *c)
{
	if ((*c & 0x80) == 0)
		return (pg_wchar) c[0];
	else if ((*c & 0xe0) == 0xc0)
		return (pg_wchar) (((c[0] & 0x1f) << 6) |
						   (c[1] & 0x3f));
	else if ((*c & 0xf0) == 0xe0)
		return (pg_wchar) (((c[0] & 0x0f) << 12) |
						   ((c[1] & 0x3f) << 6) |
						   (c[2] & 0x3f));
	else if ((*c & 0xf8) == 0xf0)
		return (pg_wchar) (((c[0] & 0x07) << 18) |
						   ((c[1] & 0x3f) << 12) |
						   ((c[2] & 0x3f) << 6) |
						   (c[3] & 0x3f));
	else
		/* that is an invalid code on purpose */
		return 0xffffffff;
}

static int
pg_utf_dsplen(const unsigned char *s)
{
	return ucs_wcwidth(utf8_to_unicode(s));
}

/*
 * ISO8859-1
 */
static int
pg_latin12wchar_with_len(const unsigned char *from, pg_wchar *to, int len)
{
	int			cnt = 0;

	while (len > 0 && *from)
	{
		*to++ = *from++;
		len--;
		cnt++;
	}
	*to = 0;
	return cnt;
}

/*
 * Trivial conversion from pg_wchar to single byte encoding. Just ignores
 * high bits.
 * caller should allocate enough space for "to"
 * len: length of from.
 * "from" not necessarily null terminated.
 */
static int
pg_wchar2single_with_len(const pg_wchar *from, unsigned char *to, int len)
{
	int			cnt = 0;

	while (len > 0 && *from)
	{
		*to++ = *from++;
		len--;
		cnt++;
	}
	*to = 0;
	return cnt;
}

static int
pg_latin1_mblen(const unsigned char *s)
{
	return 1;
}

static int
pg_latin1_dsplen(const unsigned char *s)
{
	return pg_ascii_dsplen(s);
}

/*
 *-------------------------------------------------------------------
 * multibyte sequence validators
 *
 * The verifychar functions accept "s", a pointer to the first byte of a
 * string, and "len", the remaining length of the string.  If there is a
 * validly encoded character beginning at *s, return its length in bytes;
 * else return -1.
 *
 * The verifystr functions also accept "s", a pointer to a string and "len",
 * the length of the string.  They verify the whole string, and return the
 * number of input bytes (<= len) that are valid.  In other words, if the
 * whole string is valid, verifystr returns "len", otherwise it returns the
 * byte offset of the first invalid character.  The verifystr functions must
 * test for and reject zeroes in the input.
 *
 * The verifychar functions can assume that len > 0 and that *s != '\0', but
 * they must test for and reject zeroes in any additional bytes of a
 * multibyte character.  Note that this definition allows the function for a
 * single-byte encoding to be just "return 1".
 *-------------------------------------------------------------------
 */
static int
pg_ascii_verifychar(const unsigned char *s, int len)
{
	return 1;
}

static int
pg_ascii_verifystr(const unsigned char *s, int len)
{
	const unsigned char *nullpos = memchr(s, 0, len);

	if (nullpos == NULL)
		return len;
	else
		return nullpos - s;
}

static int
pg_latin1_verifychar(const unsigned char *s, int len)
{
	return 1;
}

static int
pg_latin1_verifystr(const unsigned char *s, int len)
{
	const unsigned char *nullpos = memchr(s, 0, len);

	if (nullpos == NULL)
		return len;
	else
		return nullpos - s;
}

static int
pg_utf8_verifychar(const unsigned char *s, int len)
{
	int			l;

	if ((*s & 0x80) == 0)
	{
		if (*s == '\0')
			return -1;
		return 1;
	}
	else if ((*s & 0xe0) == 0xc0)
		l = 2;
	else if ((*s & 0xf0) == 0xe0)
		l = 3;
	else if ((*s & 0xf8) == 0xf0)
		l = 4;
	else
		l = 1;

	if (l > len)
		return -1;

	if (!pg_utf8_islegal(s, l))
		return -1;

	return l;
}

static int
pg_utf8_verifystr(const unsigned char *s, int len)
{
	const unsigned char *start = s;

	while (len > 0)
	{
		int			l;

		/* fast path for ASCII-subset characters */
		if (!IS_HIGHBIT_SET(*s))
		{
			if (*s == '\0')
				break;
			l = 1;
		}
		else
		{
			l = pg_utf8_verifychar(s, len);
			if (l == -1)
				break;
		}
		s += l;
		len -= l;
	}

	return s - start;
}

/*
 * Check for validity of a single UTF-8 encoded character
 *
 * This directly implements the rules in RFC3629.  The bizarre-looking
 * restrictions on the second byte are meant to ensure that there isn't
 * more than one encoding of a given Unicode character point; that is,
 * you may not use a longer-than-necessary byte sequence with high order
 * zero bits to represent a character that would fit in fewer bytes.
 * To do otherwise is to create security hazards (eg, create an apparent
 * non-ASCII character that decodes to plain ASCII).
 *
 * length is assumed to have been obtained by pg_utf_mblen(), and the
 * caller must have checked that that many bytes are present in the buffer.
 */
bool
pg_utf8_islegal(const unsigned char *source, int length)
{
	unsigned char a;

	switch (length)
	{
		default:
			/* reject lengths 5 and 6 for now */
			return false;
		case 4:
			a = source[3];
			if (a < 0x80 || a > 0xBF)
				return false;
			/* FALL THRU */
		case 3:
			a = source[2];
			if (a < 0x80 || a > 0xBF)
				return false;
			/* FALL THRU */
		case 2:
			a = source[1];
			switch (*source)
			{
				case 0xE0:
					if (a < 0xA0 || a > 0xBF)
						return false;
					break;
				case 0xED:
					if (a < 0x80 || a > 0x9F)
						return false;
					break;
				case 0xF0:
					if (a < 0x90 || a > 0xBF)
						return false;
					break;
				case 0xF4:
					if (a < 0x80 || a > 0x8F)
						return false;
					break;
				default:
					if (a < 0x80 || a > 0xBF)
						return false;
					break;
			}
			/* FALL THRU */
		case 1:
			a = *source;
			if (a >= 0x80 && a < 0xC2)
				return false;
			if (a > 0xF4)
				return false;
			break;
	}
	return true;
}


/*
 * Fills the provided buffer with two bytes such that:
 *   pg_encoding_mblen(dst) == 2 && pg_encoding_verifymbstr(dst) == 0
 */
void
pg_encoding_set_invalid(int encoding, char *dst)
{
	Assert(pg_encoding_max_length(encoding) > 1);

	dst[0] = (encoding == PG_UTF8 ? 0xc0 : NONUTF8_INVALID_BYTE0);
	dst[1] = NONUTF8_INVALID_BYTE1;
}

/*
 *-------------------------------------------------------------------
 * encoding info table
 * XXX must be sorted by the same order as enum pg_enc (in mb/pg_wchar.h)
 *-------------------------------------------------------------------
 */
const pg_wchar_tbl pg_wchar_table[] = {
	{pg_ascii2wchar_with_len, pg_wchar2single_with_len, pg_ascii_mblen, pg_ascii_dsplen, pg_ascii_verifychar, pg_ascii_verifystr, 1},	/* PG_SQL_ASCII */
	{pg_utf2wchar_with_len, pg_wchar2utf_with_len, pg_utf_mblen, pg_utf_dsplen, pg_utf8_verifychar, pg_utf8_verifystr, 4},	/* PG_UTF8 */
	{pg_latin12wchar_with_len, pg_wchar2single_with_len, pg_latin1_mblen, pg_latin1_dsplen, pg_latin1_verifychar, pg_latin1_verifystr, 1},	/* PG_LATIN1 */
};

/*
 * Returns the byte length of a multibyte character.
 *
 * pg_encoding_mblen() can be used when ANY of these conditions are met:
 *
 * - The input string is zero-terminated
 *
 * - The input string is known to be valid in the encoding
 *
 * - The encoding is not one that requires more than one byte of lookahead
 *   to determine character length (none of the supported encodings do).
 */
int
pg_encoding_mblen(int encoding, const char *mbstr)
{
	return (PG_VALID_ENCODING(encoding) ?
			pg_wchar_table[encoding].mblen((const unsigned char *) mbstr) :
			pg_wchar_table[PG_SQL_ASCII].mblen((const unsigned char *) mbstr));
}

/*
 * Returns the byte length of a multibyte character (possibly not
 * zero-terminated), or INT_MAX if too few bytes remain to determine a length.
 */
int
pg_encoding_mblen_or_incomplete(int encoding, const char *mbstr,
								size_t remaining)
{
	if (remaining < 1)
		return INT_MAX;
	return pg_encoding_mblen(encoding, mbstr);
}

/*
 * Returns the byte length of a multibyte character; but not more than the
 * distance to the terminating zero byte.
 */
int
pg_encoding_mblen_bounded(int encoding, const char *mbstr)
{
	return strnlen(mbstr, pg_encoding_mblen(encoding, mbstr));
}

/*
 * Returns the display length of a multibyte character.
 */
int
pg_encoding_dsplen(int encoding, const char *mbstr)
{
	return (PG_VALID_ENCODING(encoding) ?
			pg_wchar_table[encoding].dsplen((const unsigned char *) mbstr) :
			pg_wchar_table[PG_SQL_ASCII].dsplen((const unsigned char *) mbstr));
}

/*
 * Verify the first multibyte character of the given string.
 * Return its byte length if good, -1 if bad.
 */
int
pg_encoding_verifymbchar(int encoding, const char *mbstr, int len)
{
	return (PG_VALID_ENCODING(encoding) ?
			pg_wchar_table[encoding].mbverifychar((const unsigned char *) mbstr, len) :
			pg_wchar_table[PG_SQL_ASCII].mbverifychar((const unsigned char *) mbstr, len));
}

/*
 * Verify that a string is valid for the given encoding.
 * Returns the number of input bytes (<= len) that form a valid string.
 */
int
pg_encoding_verifymbstr(int encoding, const char *mbstr, int len)
{
	return (PG_VALID_ENCODING(encoding) ?
			pg_wchar_table[encoding].mbverifystr((const unsigned char *) mbstr, len) :
			pg_wchar_table[PG_SQL_ASCII].mbverifystr((const unsigned char *) mbstr, len));
}

/*
 * fetch maximum length of a given encoding
 */
int
pg_encoding_max_length(int encoding)
{
	Assert(PG_VALID_ENCODING(encoding));

	/*
	 * Check for the encoding despite the assert, due to some mingw versions
	 * otherwise issuing bogus warnings.
	 */
	return PG_VALID_ENCODING(encoding) ?
		pg_wchar_table[encoding].maxmblen :
		pg_wchar_table[PG_SQL_ASCII].maxmblen;
}
