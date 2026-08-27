/*-------------------------------------------------------------------------
 *
 * md5_common.c
 *	  Routines shared between all MD5 implementations used for encrypted
 *	  passwords.
 *
 * Sverre H. Huseby <sverrehu@online.no>
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/md5_common.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/cryptohash.h"
#include "common/md5.h"

static void
bytesToHex(uint8 b[16], char *s)
{
	static const char *hex = "0123456789abcdef";
	int			q,
				w;

	for (q = 0, w = 0; q < 16; q++)
	{
		s[w++] = hex[(b[q] >> 4) & 0x0F];
		s[w++] = hex[b[q] & 0x0F];
	}
	s[w] = '\0';
}

/*
 *	pg_md5_hash
 *
 *	Calculates the MD5 sum of the bytes in a buffer.
 *
 *	SYNOPSIS	  #include "md5.h"
 *				  int pg_md5_hash(const void *buff, size_t len, char *hexsum)
 *
 *	INPUT		  buff	  the buffer containing the bytes that you want
 *						  the MD5 sum of.
 *				  len	  number of bytes in the buffer.
 *
 *	OUTPUT		  hexsum  the MD5 sum as a '\0'-terminated string of
 *						  hexadecimal digits.  an MD5 sum is 16 bytes long.
 *						  each byte is represented by two hexadecimal
 *						  characters.  you thus need to provide an array
 *						  of 33 characters, including the trailing '\0'.
 *
 *	RETURNS		  false on failure (out of memory for internal buffers
 *				  or MD5 computation failure) or true on success.
 *
 *	STANDARDS	  MD5 is described in RFC 1321.
 *
 *	AUTHOR		  Sverre H. Huseby <sverrehu@online.no>
 *
 */

bool
pg_md5_hash(const void *buff, size_t len, char *hexsum)
{
	uint8		sum[MD5_DIGEST_LENGTH];
	pg_cryptohash_ctx *ctx;

	ctx = pg_cryptohash_create(PG_MD5);
	if (ctx == NULL)
		return false;

	if (pg_cryptohash_init(ctx) < 0 ||
		pg_cryptohash_update(ctx, buff, len) < 0 ||
		pg_cryptohash_final(ctx, sum, sizeof(sum)) < 0)
	{
		pg_cryptohash_free(ctx);
		return false;
	}

	bytesToHex(sum, hexsum);
	pg_cryptohash_free(ctx);
	return true;
}
