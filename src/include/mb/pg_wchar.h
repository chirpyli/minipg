/*-------------------------------------------------------------------------
 *
 * pg_wchar.h
 *	  multibyte-character support
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/mb/pg_wchar.h
 *
 *	NOTES
 *		This is used both by the backend and by frontends, but should not be
 *		included by libpq client programs.  In particular, a libpq client
 *		should not assume that the encoding IDs used by the version of libpq
 *		it's linked to match up with the IDs declared here.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_WCHAR_H
#define PG_WCHAR_H

/*
 * The pg_wchar type
 */
typedef unsigned int pg_wchar;

/*
 * Maximum byte length of multibyte characters in any backend encoding
 */
#define MAX_MULTIBYTE_CHAR_LEN	4

/*
 * PostgreSQL encoding identifiers
 *
 * WARNING: the order of this enum must be same as order of entries
 *			in the pg_enc2name_tbl[] array (in src/common/encnames.c), and
 *			in the pg_wchar_table[] array (in src/common/wchar.c)!
 *
 * PG_SQL_ASCII is default encoding and must be = 0.
 *
 * minipg supports only three encodings: SQL_ASCII, UTF8 and LATIN1
 * (ISO-8859-1).  All other encodings were removed to slim down the
 * character set machinery.
 */
typedef enum pg_enc
{
	PG_SQL_ASCII = 0,			/* SQL/ASCII */
	PG_UTF8,					/* Unicode UTF8 */
	PG_LATIN1,					/* ISO-8859-1 Latin 1 */
	_PG_LAST_ENCODING_			/* mark only */

} pg_enc;

#define PG_ENCODING_BE_LAST PG_LATIN1

/*
 * Please use these tests before access to pg_enc2name_tbl[]
 * or to other places...
 */
#define PG_VALID_BE_ENCODING(_enc) \
		((_enc) >= 0 && (_enc) <= PG_ENCODING_BE_LAST)

#define PG_ENCODING_IS_CLIENT_ONLY(_enc) \
		((_enc) > PG_ENCODING_BE_LAST && (_enc) < _PG_LAST_ENCODING_)

#define PG_VALID_ENCODING(_enc) \
		((_enc) >= 0 && (_enc) < _PG_LAST_ENCODING_)

/* On FE are possible all encodings */
#define PG_VALID_FE_ENCODING(_enc)	PG_VALID_ENCODING(_enc)

/*
 * When converting strings between different encodings, we assume that space
 * for converted result is 4-to-1 growth in the worst case.
 */
#define MAX_CONVERSION_GROWTH  4

/*
 * Maximum number of input bytes that can be handled in one call to a
 * conversion function.  The output buffer must be at least
 * MAX_CONVERSION_GROWTH * MAX_CONVERSION_INPUT_LENGTH bytes long.
 */
#define MAX_CONVERSION_INPUT_LENGTH	16

/*
 * Maximum byte length of the string equivalent to any one Unicode code point,
 * in any backend encoding.  The current value assumes that a 4-byte UTF-8
 * character might expand by MAX_CONVERSION_GROWTH, which is a huge
 * overestimate.  But in current usage we don't allocate large multiples of
 * this, so there's little point in being stingy.
 */
#define MAX_UNICODE_EQUIVALENT_STRING	16

/*
 * Table for mapping an encoding number to official encoding name and
 * possibly other subsidiary data.  Be careful to check encoding number
 * before accessing a table entry!
 */
typedef struct pg_enc2name
{
	const char *name;
	pg_enc		encoding;
} pg_enc2name;

extern PGDLLIMPORT const pg_enc2name pg_enc2name_tbl[];

/*
 * pg_wchar stuff
 */
typedef int (*mb2wchar_with_len_converter) (const unsigned char *from,
											pg_wchar *to,
											int len);

typedef int (*wchar2mb_with_len_converter) (const pg_wchar *from,
											unsigned char *to,
											int len);

typedef int (*mblen_converter) (const unsigned char *mbstr);

typedef int (*mbdisplaylen_converter) (const unsigned char *mbstr);

typedef bool (*mbcharacter_incrementer) (unsigned char *mbstr, int len);

typedef int (*mbchar_verifier) (const unsigned char *mbstr, int len);

typedef int (*mbstr_verifier) (const unsigned char *mbstr, int len);

typedef struct
{
	mb2wchar_with_len_converter mb2wchar_with_len;	/* convert a multibyte
													 * string to a wchar */
	wchar2mb_with_len_converter wchar2mb_with_len;	/* convert a wchar string
													 * to a multibyte */
	mblen_converter mblen;		/* get byte length of a char */
	mbdisplaylen_converter dsplen;	/* get display width of a char */
	mbchar_verifier mbverifychar;	/* verify multibyte character */
	mbstr_verifier mbverifystr; /* verify multibyte string */
	int			maxmblen;		/* max bytes for a char in this encoding */
} pg_wchar_tbl;

extern const pg_wchar_tbl pg_wchar_table[];

/*
 * Support macro for encoding conversion functions to validate their
 * arguments.
 */
#define CHECK_ENCODING_CONVERSION_ARGS(srcencoding,destencoding) \
	check_encoding_conversion_args(PG_GETARG_INT32(0), \
								   PG_GETARG_INT32(1), \
								   PG_GETARG_INT32(4), \
								   (srcencoding), \
								   (destencoding))


/*
 * Some handy functions for Unicode-specific tests.
 */
static inline bool
is_valid_unicode_codepoint(pg_wchar c)
{
	return (c > 0 && c <= 0x10FFFF);
}

static inline bool
is_utf16_surrogate_first(pg_wchar c)
{
	return (c >= 0xD800 && c <= 0xDBFF);
}

static inline bool
is_utf16_surrogate_second(pg_wchar c)
{
	return (c >= 0xDC00 && c <= 0xDFFF);
}

static inline pg_wchar
surrogate_pair_to_codepoint(pg_wchar first, pg_wchar second)
{
	return ((first & 0x3FF) << 10) + 0x10000 + (second & 0x3FF);
}


/*
 * These functions are considered part of libpq's exported API and
 * are also declared in libpq-fe.h.
 */
extern int	pg_char_to_encoding(const char *name);
extern const char *pg_encoding_to_char(int encoding);
extern int	pg_valid_server_encoding_id(int encoding);

/*
 * These functions are available to frontend code that links with libpgcommon
 * (in addition to the ones just above).  The constant tables declared
 * earlier in this file are also available from libpgcommon.
 */
extern void pg_encoding_set_invalid(int encoding, char *dst);
extern int	pg_encoding_mblen(int encoding, const char *mbstr);
extern int	pg_encoding_mblen_or_incomplete(int encoding, const char *mbstr,
											size_t remaining);
extern int	pg_encoding_mblen_bounded(int encoding, const char *mbstr);
extern int	pg_encoding_dsplen(int encoding, const char *mbstr);
extern int	pg_encoding_verifymbchar(int encoding, const char *mbstr, int len);
extern int	pg_encoding_verifymbstr(int encoding, const char *mbstr, int len);
extern int	pg_encoding_max_length(int encoding);
extern int	pg_valid_client_encoding(const char *name);
extern int	pg_valid_server_encoding(const char *name);

extern unsigned char *unicode_to_utf8(pg_wchar c, unsigned char *utf8string);
extern pg_wchar utf8_to_unicode(const unsigned char *c);
extern bool pg_utf8_islegal(const unsigned char *source, int length);
extern int	pg_utf_mblen(const unsigned char *s);

/*
 * The remaining functions are backend-only.
 */
extern int	pg_mb2wchar(const char *from, pg_wchar *to);
extern int	pg_mb2wchar_with_len(const char *from, pg_wchar *to, int len);
extern int	pg_encoding_mb2wchar_with_len(int encoding,
										  const char *from, pg_wchar *to, int len);
extern int	pg_wchar2mb(const pg_wchar *from, char *to);
extern int	pg_wchar2mb_with_len(const pg_wchar *from, char *to, int len);
extern int	pg_encoding_wchar2mb_with_len(int encoding,
										  const pg_wchar *from, char *to, int len);
extern int	pg_char_and_wchar_strcmp(const char *s1, const pg_wchar *s2);
extern int	pg_wchar_strncmp(const pg_wchar *s1, const pg_wchar *s2, size_t n);
extern int	pg_char_and_wchar_strncmp(const char *s1, const pg_wchar *s2, size_t n);
extern size_t pg_wchar_strlen(const pg_wchar *wstr);
extern int	pg_mblen_cstr(const char *mbstr);
extern int	pg_mblen_range(const char *mbstr, const char *end);
extern int	pg_mblen_with_len(const char *mbstr, int limit);
extern int	pg_mblen_unbounded(const char *mbstr);

/* deprecated */
extern int	pg_mblen(const char *mbstr);

extern int	pg_dsplen(const char *mbstr);
extern int	pg_mbstrlen(const char *mbstr);
extern int	pg_mbstrlen_with_len(const char *mbstr, int len);
extern int	pg_mbcliplen(const char *mbstr, int len, int limit);
extern int	pg_encoding_mbcliplen(int encoding, const char *mbstr,
								  int len, int limit);
extern int	pg_mbcharcliplen(const char *mbstr, int len, int limit);
extern int	pg_database_encoding_max_length(void);
extern mbcharacter_incrementer pg_database_encoding_character_incrementer(void);

extern int	PrepareClientEncoding(int encoding);
extern int	SetClientEncoding(int encoding);
extern void InitializeClientEncoding(void);
extern int	pg_get_client_encoding(void);
extern const char *pg_get_client_encoding_name(void);

extern void SetDatabaseEncoding(int encoding);
extern int	GetDatabaseEncoding(void);
extern const char *GetDatabaseEncodingName(void);
extern void SetMessageEncoding(int encoding);
extern int	GetMessageEncoding(void);

extern unsigned char *pg_do_encoding_conversion(unsigned char *src, int len,
												int src_encoding,
												int dest_encoding);
extern int	pg_do_encoding_conversion_buf(Oid proc,
										  int src_encoding,
										  int dest_encoding,
										  unsigned char *src, int srclen,
										  unsigned char *dst, int dstlen,
										  bool noError);

extern char *pg_client_to_server(const char *s, int len);
extern char *pg_server_to_client(const char *s, int len);
extern char *pg_any_to_server(const char *s, int len, int encoding);
extern char *pg_server_to_any(const char *s, int len, int encoding);

extern void pg_unicode_to_server(pg_wchar c, unsigned char *s);

extern bool pg_verifymbstr(const char *mbstr, int len, bool noError);
extern bool pg_verify_mbstr(int encoding, const char *mbstr, int len,
							bool noError);
extern int	pg_verify_mbstr_len(int encoding, const char *mbstr, int len,
								bool noError);

extern void check_encoding_conversion_args(int src_encoding,
										   int dest_encoding,
										   int len,
										   int expected_src_encoding,
										   int expected_dest_encoding);

extern void report_invalid_encoding(int encoding, const char *mbstr, int len) pg_attribute_noreturn();
extern void report_untranslatable_char(int src_encoding, int dest_encoding,
									   const char *mbstr, int len) pg_attribute_noreturn();

#endif							/* PG_WCHAR_H */
