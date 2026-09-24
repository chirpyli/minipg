/*-------------------------------------------------------------------------
 *
 * transam.h
 *	  postgres transaction access method support code
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/transam.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TRANSAM_H
#define TRANSAM_H

#include "access/xlogdefs.h"


/* ----------------
 *		特殊事务 ID 取值
 *
 * BootstrapTransactionId 是用于 "bootstrap" 操作的 XID，
 * FrozenTransactionId 用于非常老的元组。两者都应始终被视为有效。
 *
 * FirstNormalTransactionId 是第一个"普通"（normal）事务 ID。
 * 注意：如果需要修改它，必须同时修改 pg_class.h。
 * ----------------
 */
#define InvalidTransactionId		((TransactionId) 0)
#define BootstrapTransactionId		((TransactionId) 1)
#define FrozenTransactionId			((TransactionId) 2)
#define FirstNormalTransactionId	((TransactionId) 3)
#define MaxTransactionId			((TransactionId) 0xFFFFFFFF)   // 采用32位事务ID

/* ----------------
 *		transaction ID manipulation macros
 * ----------------
 */
#define TransactionIdIsValid(xid)		((xid) != InvalidTransactionId)
#define TransactionIdIsNormal(xid)		((xid) >= FirstNormalTransactionId)
#define TransactionIdEquals(id1, id2)	((id1) == (id2))
#define TransactionIdStore(xid, dest)	(*(dest) = (xid))
#define StoreInvalidTransactionId(dest) (*(dest) = InvalidTransactionId)

#define EpochFromFullTransactionId(x)	((uint32) ((x).value >> 32))
#define XidFromFullTransactionId(x)		((uint32) (x).value)
#define U64FromFullTransactionId(x)		((x).value)
#define FullTransactionIdEquals(a, b)	((a).value == (b).value)
#define FullTransactionIdPrecedes(a, b)	((a).value < (b).value)
#define FullTransactionIdPrecedesOrEquals(a, b) ((a).value <= (b).value)
#define FullTransactionIdFollows(a, b) ((a).value > (b).value)
#define FullTransactionIdFollowsOrEquals(a, b) ((a).value >= (b).value)
#define FullTransactionIdIsValid(x)		TransactionIdIsValid(XidFromFullTransactionId(x))
#define InvalidFullTransactionId		FullTransactionIdFromEpochAndXid(0, InvalidTransactionId)
#define FirstNormalFullTransactionId	FullTransactionIdFromEpochAndXid(0, FirstNormalTransactionId)
#define FullTransactionIdIsNormal(x)	FullTransactionIdFollowsOrEquals(x, FirstNormalFullTransactionId)

/*
 * 一个包含 epoch 和 TransactionId 的 64 位值。它被封装在结构体中，
 * 以防止与 TransactionId 之间发生隐式转换。
 * 并非所有取值都表示有效的普通 XID。
 */
typedef struct FullTransactionId
{
	uint64		value;
} FullTransactionId;

static inline FullTransactionId
FullTransactionIdFromEpochAndXid(uint32 epoch, TransactionId xid)
{
	FullTransactionId result;

	result.value = ((uint64) epoch) << 32 | xid;

	return result;
}

static inline FullTransactionId
FullTransactionIdFromU64(uint64 value)
{
	FullTransactionId result;

	result.value = value;

	return result;
}

/* advance a transaction ID variable, handling wraparound correctly */
#define TransactionIdAdvance(dest)	\
	do { \
		(dest)++; \
		if ((dest) < FirstNormalTransactionId) \
			(dest) = FirstNormalTransactionId; \
	} while(0)

/*
 * Retreat a FullTransactionId variable, stepping over xids that would appear
 * to be special only when viewed as 32bit XIDs.
 */
static inline void
FullTransactionIdRetreat(FullTransactionId *dest)
{
	dest->value--;

	/*
	 * In contrast to 32bit XIDs don't step over the "actual" special xids.
	 * For 64bit xids these can't be reached as part of a wraparound as they
	 * can in the 32bit case.
	 */
	if (FullTransactionIdPrecedes(*dest, FirstNormalFullTransactionId))
		return;

	/*
	 * But we do need to step over XIDs that'd appear special only for 32bit
	 * XIDs.
	 */
	while (XidFromFullTransactionId(*dest) < FirstNormalTransactionId)
		dest->value--;
}

/*
 * Advance a FullTransactionId variable, stepping over xids that would appear
 * to be special only when viewed as 32bit XIDs.
 */
static inline void
FullTransactionIdAdvance(FullTransactionId *dest)
{
	dest->value++;

	/* see FullTransactionIdAdvance() */
	if (FullTransactionIdPrecedes(*dest, FirstNormalFullTransactionId))
		return;

	while (XidFromFullTransactionId(*dest) < FirstNormalTransactionId)
		dest->value++;
}

/* back up a transaction ID variable, handling wraparound correctly */
#define TransactionIdRetreat(dest)	\
	do { \
		(dest)--; \
	} while ((dest) < FirstNormalTransactionId)

/* compare two XIDs already known to be normal; this is a macro for speed */
#define NormalTransactionIdPrecedes(id1, id2) \
	(AssertMacro(TransactionIdIsNormal(id1) && TransactionIdIsNormal(id2)), \
	(int32) ((id1) - (id2)) < 0)

/* compare two XIDs already known to be normal; this is a macro for speed */
#define NormalTransactionIdFollows(id1, id2) \
	(AssertMacro(TransactionIdIsNormal(id1) && TransactionIdIsNormal(id2)), \
	(int32) ((id1) - (id2)) > 0)

/* ----------
 *		对象 ID（OID）取值为零时是 InvalidOid。
 *
 *		OID 1-9999 预留给手工分配（参见 src/include/catalog/ 下的 .dat
 *		文件）。其中 8000-9999 预留给开发用途（例如正在开发的补丁和
 *		分支）；它们不应该出现在正式发布的版本中。
 *
 *		OID 10000-11999 预留给 genbki.pl 分配，用于 src/include/catalog/
 *		下的 .dat 文件没有为某个需要 OID 的系统表条目指定 OID 的情况。
 *		注意 genbki.pl 是在每个系统表中独立分配这些 OID 的，因此不能
 *		保证它们全局唯一。
 *
 *		OID 12000-16383 预留给 initdb 期间使用 OID 生成器进行分配。
 *		（我们把生成器的起点设为 12000。）
 *
 *		从 16384 开始的 OID 在正常的多用户运行期间由 OID 生成器分配。
 *		（一旦进入正常运行状态，我们就把生成器强制推进到 16384。）
 *
 * 8000、10000 和 12000 这几个分界值完全是任意选取的，如果在某个类别中
 * OID 即将用尽，可以调整它们。为此只需修改下面的宏，并更新相关文档
 * （参见 bki.sgml 和 RELEASE_CHANGES）即可。不过，移动 initdb 分配的
 * OID 与用户自定义对象之间 16384 这个分界会麻烦得多，因为某些用户自定义
 * OID 会出现在磁盘数据中；这样的改动很可能会导致 pg_upgrade 失败。
 *
 * 注意：如果 OID 生成器发生回绕，我们会跳过 OID 0-16383，从 16384 继续
 * 分配。这样可以避免重新分配那些可能在 initdb 期间已被分配的 OID，
 * 从而把 OID 冲突的可能性降到最低。
 * ----------
 */
#define FirstGenbkiObjectId		10000
#define FirstBootstrapObjectId	12000
#define FirstNormalObjectId		16384

/*
 * VariableCache 是共享内存中的一种数据结构，用于跟踪 OID 和 XID 的分配状态。
 * 主要出于历史原因，这里只用了一个结构体，其不同字段由不同的 LWLock 保护。
 *
 * 注意：xidWrapLimit 和 oldestXidDB 并不是"活跃"的取值，它们只是用来在
 * xidWarnLimit 或 xidStopLimit 被超过时生成有用的提示信息。
 */
typedef struct VariableCacheData
{
	/*
	 * These fields are protected by OidGenLock.
	 */
	Oid			nextOid;		/* next OID to assign */
	uint32		oidCount;		/* OIDs available before must do XLOG work */

	/*
	 * These fields are protected by XidGenLock.
	 */
	FullTransactionId nextXid;	/* next XID to assign */

	TransactionId oldestXid;	/* cluster-wide minimum datfrozenxid */
	TransactionId xidVacLimit;	/* start forcing autovacuums here */
	TransactionId xidWarnLimit; /* start complaining here */
	TransactionId xidStopLimit; /* refuse to advance nextXid beyond here */
	TransactionId xidWrapLimit; /* where the world ends */
	Oid			oldestXidDB;	/* database with minimum datfrozenxid */

	/*
	 * These fields are protected by ProcArrayLock.
	 */
	FullTransactionId latestCompletedXid;	/* newest full XID that has
											 * committed or aborted */

	/*
	 * Number of top-level transactions with xids (i.e. which may have
	 * modified the database) that completed in some form since the start of
	 * the server. This currently is solely used to check whether
	 * GetSnapshotData() needs to recompute the contents of the snapshot, or
	 * not. There are likely other users of this.  Always above 1.
	 */
	uint64		xactCompletionCount;

	/*
	 * These fields are protected by XactTruncationLock
	 */
	TransactionId oldestClogXid;	/* oldest it's safe to look up in clog */

} VariableCacheData;

typedef VariableCacheData *VariableCache;


/* ----------------
 *		extern declarations
 * ----------------
 */

/* in transam/xact.c */
extern bool TransactionStartedDuringRecovery(void);

/* in transam/varsup.c */
extern PGDLLIMPORT VariableCache ShmemVariableCache;

/*
 * prototypes for functions in transam/transam.c
 */
extern bool TransactionIdDidCommit(TransactionId transactionId);
extern bool TransactionIdDidAbort(TransactionId transactionId);
extern bool TransactionIdIsKnownCompleted(TransactionId transactionId);
extern void TransactionIdCommitTree(TransactionId xid, int nxids, TransactionId *xids);
extern void TransactionIdAsyncCommitTree(TransactionId xid, int nxids, TransactionId *xids, XLogRecPtr lsn);
extern void TransactionIdAbortTree(TransactionId xid, int nxids, TransactionId *xids);
extern bool TransactionIdPrecedes(TransactionId id1, TransactionId id2);
extern bool TransactionIdPrecedesOrEquals(TransactionId id1, TransactionId id2);
extern bool TransactionIdFollows(TransactionId id1, TransactionId id2);
extern bool TransactionIdFollowsOrEquals(TransactionId id1, TransactionId id2);
extern TransactionId TransactionIdLatest(TransactionId mainxid,
										 int nxids, const TransactionId *xids);
extern XLogRecPtr TransactionIdGetCommitLSN(TransactionId xid);

/* in commands/vacuum.c */
extern int	autovacuum_freeze_max_age;
extern int	autovacuum_multixact_freeze_max_age;

/* in transam/varsup.c */
extern FullTransactionId GetNewTransactionId(bool isSubXact);
extern void AdvanceNextFullTransactionIdPastXid(TransactionId xid);
extern FullTransactionId ReadNextFullTransactionId(void);
extern void SetTransactionIdLimit(TransactionId oldest_datfrozenxid,
								  Oid oldest_datoid);
extern void AdvanceOldestClogXid(TransactionId oldest_datfrozenxid);
extern bool ForceTransactionIdLimitUpdate(void);
extern Oid	GetNewObjectId(void);

#ifdef USE_ASSERT_CHECKING
extern void AssertTransactionIdInAllowableRange(TransactionId xid);
#else
#define AssertTransactionIdInAllowableRange(xid) ((void)true)
#endif

/*
 * Some frontend programs include this header.  For compilers that emit static
 * inline functions even when they're unused, that leads to unsatisfied
 * external references; hence hide them with #ifndef FRONTEND.
 */
#ifndef FRONTEND

/*
 * For callers that just need the XID part of the next transaction ID.
 */
static inline TransactionId
ReadNextTransactionId(void)
{
	return XidFromFullTransactionId(ReadNextFullTransactionId());
}

/* return transaction ID backed up by amount, handling wraparound correctly */
static inline TransactionId
TransactionIdRetreatedBy(TransactionId xid, uint32 amount)
{
	xid -= amount;

	while (xid < FirstNormalTransactionId)
		xid--;

	return xid;
}

/* return the older of the two IDs */
static inline TransactionId
TransactionIdOlder(TransactionId a, TransactionId b)
{
	if (!TransactionIdIsValid(a))
		return b;

	if (!TransactionIdIsValid(b))
		return a;

	if (TransactionIdPrecedes(a, b))
		return a;
	return b;
}

/* return the older of the two IDs, assuming they're both normal */
static inline TransactionId
NormalTransactionIdOlder(TransactionId a, TransactionId b)
{
	Assert(TransactionIdIsNormal(a));
	Assert(TransactionIdIsNormal(b));
	if (NormalTransactionIdPrecedes(a, b))
		return a;
	return b;
}

/* return the newer of the two IDs */
static inline FullTransactionId
FullTransactionIdNewer(FullTransactionId a, FullTransactionId b)
{
	if (!FullTransactionIdIsValid(a))
		return b;

	if (!FullTransactionIdIsValid(b))
		return a;

	if (FullTransactionIdFollows(a, b))
		return a;
	return b;
}

#endif							/* FRONTEND */

#endif							/* TRANSAM_H */
