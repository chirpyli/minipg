/*-------------------------------------------------------------------------
 *
 * standbydefs.h
 *	   Frontend exposed definitions for invalidation message descriptors.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/standbydefs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STANDBYDEFS_H
#define STANDBYDEFS_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/lockdefs.h"
#include "storage/sinval.h"

/* Descriptor routine shared by xact_desc and XLOG_XACT_INVALIDATIONS */
extern void standby_desc_invalidations(StringInfo buf,
									   int nmsgs, SharedInvalidationMessage *msgs,
									   Oid dbId, Oid tsId,
									   bool relcacheInitFileInval);

#endif							/* STANDBYDEFS_H */
