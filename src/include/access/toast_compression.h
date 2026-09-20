/*-------------------------------------------------------------------------
 *
 * toast_compression.h
 *	  Functions for toast compression.
 *
 * Copyright (c) 2021, PostgreSQL Global Development Group
 *
 * src/include/access/toast_compression.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TOAST_COMPRESSION_H
#define TOAST_COMPRESSION_H

/*
 * Built-in compression method ID.  The toast compression header will store
 * this in the first 2 bits of the raw length.  These built-in compression
 * method IDs are directly mapped to the built-in compression methods.
 *
 * Don't use these values for anything other than understanding the meaning
 * of the raw bits from a varlena; in particular, if the goal is to identify
 * a compression method, use the constants TOAST_PGLZ_COMPRESSION, etc.
 * below. We might someday support more than 4 compression methods, but
 * we can never have more than 4 values in this enum, because there are
 * only 2 bits available in the places where this is stored.
 */
typedef enum ToastCompressionId
{
	TOAST_PGLZ_COMPRESSION_ID = 0,
	TOAST_INVALID_COMPRESSION_ID = 2
} ToastCompressionId;

/*
 * Built-in compression method.  minipg only supports pglz, so all toast
 * values are compressed with pglz.
 */
#define TOAST_PGLZ_COMPRESSION			'p'
#define InvalidCompressionMethod		'\0'

#define CompressionMethodIsValid(cm)  ((cm) != InvalidCompressionMethod)


/* pglz compression/decompression routines */
extern struct varlena *pglz_compress_datum(const struct varlena *value);
extern struct varlena *pglz_decompress_datum(const struct varlena *value);
extern struct varlena *pglz_decompress_datum_slice(const struct varlena *value,
												   int32 slicelength);

#endif							/* TOAST_COMPRESSION_H */
