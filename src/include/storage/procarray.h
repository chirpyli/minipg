/*-------------------------------------------------------------------------
 *
 * procarray.h
 *	  POSTGRES process array definitions.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/procarray.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROCARRAY_H
#define PROCARRAY_H

#include "storage/lock.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"


extern Size ProcArrayShmemSize(void);
extern void CreateSharedProcArray(void);
extern void ProcArrayAdd(PGPROC *proc);
extern void ProcArrayRemove(PGPROC *proc, TransactionId latestXid);

extern void ProcArrayEndTransaction(PGPROC *proc, TransactionId latestXid);
extern void ProcArrayClearTransaction(PGPROC *proc);

/*
 * Data structure for GetRunningTransactionData(). Similar to Snapshots, but
 * not quite. This has nothing at all to do with visibility on this server,
 * so this is completely separate from snapmgr.c and snapmgr.h.
 */
typedef enum
{
	SUBXIDS_IN_ARRAY,			/* xids array includes all running subxids */
	SUBXIDS_MISSING,			/* snapshot overflowed, subxids are missing */
	SUBXIDS_IN_SUBTRANS,		/* subxids are not included in 'xids', but
								 * pg_subtrans is fully up-to-date */
} subxids_array_status;

typedef struct RunningTransactionsData
{
	int			xcnt;			/* # of xact ids in xids[] */
	int			subxcnt;		/* # of subxact ids in xids[] */
	subxids_array_status subxid_status;
	TransactionId nextXid;		/* xid from ShmemVariableCache->nextXid */
	TransactionId oldestRunningXid; /* *not* oldestXmin */
	TransactionId latestCompletedXid;	/* so we can set xmax */

	TransactionId *xids;		/* array of (sub)xids still running */
} RunningTransactionsData;

typedef RunningTransactionsData *RunningTransactions;

extern int	GetMaxSnapshotXidCount(void);
extern int	GetMaxSnapshotSubxidCount(void);

extern Snapshot GetSnapshotData(Snapshot snapshot);

extern bool ProcArrayInstallImportedXmin(TransactionId xmin,
										 VirtualTransactionId *sourcevxid);
extern bool ProcArrayInstallRestoredXmin(TransactionId xmin, PGPROC *proc);

extern RunningTransactions GetRunningTransactionData(void);

extern bool TransactionIdIsInProgress(TransactionId xid);
extern bool TransactionIdIsActive(TransactionId xid);
extern TransactionId GetOldestNonRemovableTransactionId(Relation rel);
extern TransactionId GetOldestTransactionIdConsideredRunning(void);
extern TransactionId GetOldestActiveTransactionId(void);


extern VirtualTransactionId *GetVirtualXIDsDelayingChkpt(int *nvxids);
extern VirtualTransactionId *GetVirtualXIDsDelayingChkptEnd(int *nvxids);
extern bool HaveVirtualXIDsDelayingChkpt(VirtualTransactionId *vxids,
										 int nvxids);
extern bool HaveVirtualXIDsDelayingChkptEnd(VirtualTransactionId *vxids,
											int nvxids);

extern PGPROC *BackendPidGetProc(int pid);
extern PGPROC *BackendPidGetProcWithLock(int pid);
extern int	BackendXidGetPid(TransactionId xid);
extern bool IsBackendPid(int pid);

extern VirtualTransactionId *GetCurrentVirtualXIDs(TransactionId limitXmin,
												   bool excludeXmin0, bool allDbs, int excludeVacuum,
												   int *nvxids);
extern VirtualTransactionId *GetConflictingVirtualXIDs(TransactionId limitXmin, Oid dbOid);

extern bool MinimumActiveBackends(int min);
extern int	CountDBBackends(Oid databaseid);
extern int	CountDBConnections(Oid databaseid);
extern int	CountUserBackends(Oid roleid);
extern bool CountOtherDBBackends(Oid databaseId,
								 int *nbackends, int *nprepared);
extern void TerminateOtherDBBackends(Oid databaseId);

extern void XidCacheRemoveRunningXids(TransactionId xid,
									  int nxids, const TransactionId *xids,
									  TransactionId latestXid);

#endif							/* PROCARRAY_H */
