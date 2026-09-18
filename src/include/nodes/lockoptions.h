/*-------------------------------------------------------------------------
 *
 * lockoptions.h
 *	  Common header for some locking-related declarations.
 *
 *
 * Copyright (c) 2014-2021, PostgreSQL Global Development Group
 *
 * src/include/nodes/lockoptions.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOCKOPTIONS_H
#define LOCKOPTIONS_H

/*
 * This enum represents the different strengths of row locking.  Only LCS_NONE
 * is used now that the FOR UPDATE/SHARE clauses have been trimmed, but the
 * remaining values are retained as part of the row-marking infrastructure.
 */
typedef enum LockClauseStrength
{
	LCS_NONE,					/* no such clause - only used in PlanRowMark */
	LCS_FORKEYSHARE,			/* FOR KEY SHARE */
	LCS_FORSHARE,				/* FOR SHARE */
	LCS_FORNOKEYUPDATE,			/* FOR NO KEY UPDATE */
	LCS_FORUPDATE				/* FOR UPDATE */
} LockClauseStrength;

/*
 * This enum controls how to deal with rows being locked (i.e., it represents
 * the NOWAIT and SKIP LOCKED options).
 */
typedef enum LockWaitPolicy
{
	/* Wait for the lock to become available (default behavior) */
	LockWaitBlock,
	/* Skip rows that can't be locked (SKIP LOCKED) */
	LockWaitSkip,
	/* Raise an error if a row cannot be locked (NOWAIT) */
	LockWaitError
} LockWaitPolicy;

/*
 * Possible lock modes for a tuple.
 */
typedef enum LockTupleMode
{
	/* SELECT FOR KEY SHARE */
	LockTupleKeyShare,
	/* SELECT FOR SHARE */
	LockTupleShare,
	/* SELECT FOR NO KEY UPDATE, and UPDATEs that don't modify key columns */
	LockTupleNoKeyExclusive,
	/* SELECT FOR UPDATE, UPDATEs that modify key columns, and DELETE */
	LockTupleExclusive
} LockTupleMode;

#endif							/* LOCKOPTIONS_H */
