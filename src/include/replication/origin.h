/*-------------------------------------------------------------------------
 * origin.h
 *	   Replication origin session progress tracking types.
 *
 * minipg has removed logical replication, but the basic RepOriginId type
 * and well-known origin id constants are still referenced by transaction
 * machinery (e.g. commit timestamp tracking), so they are kept here.
 *
 * Copyright (c) 2013-2021, PostgreSQL Global Development Group
 *
 * src/include/replication/origin.h
 *-------------------------------------------------------------------------
 */
#ifndef PG_ORIGIN_H
#define PG_ORIGIN_H

#include "access/xlogdefs.h"

#define InvalidRepOriginId 0
#define DoNotReplicateId PG_UINT16_MAX

#endif							/* PG_ORIGIN_H */
