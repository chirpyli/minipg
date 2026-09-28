/*-------------------------------------------------------------------------
 *
 * lwlock.h
 *	  Lightweight lock manager
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/lwlock.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LWLOCK_H
#define LWLOCK_H

#ifdef FRONTEND
#error "lwlock.h may not be included from frontend code"
#endif

#include "port/atomics.h"
#include "storage/proclist_types.h"

struct PGPROC;

/* what state of the wait process is a backend in */
typedef enum LWLockWaitState
{
	LW_WS_NOT_WAITING, /* not currently waiting / woken up */
	LW_WS_WAITING, /* currently waiting */
	LW_WS_PENDING_WAKEUP /* removed from waitlist, but not yet signalled */
} LWLockWaitState;

/*
 * lwlock.c 之外的代码不应直接操作本结构体的内容，但我们必须在这里声明它，
 * 以便把 LWLock 嵌入到其他数据结构中。
 */
typedef struct LWLock
{
	uint16		tranche;		/* tranche ID,  16位锁族（tranche）编号, 相当于身份标签，是LWLock对外可观测性的关键字段  */  
	pg_atomic_uint32 state;		/* state of exclusive/nonexclusive lockers */
	proclist_head waiters;		/* list of waiting PGPROCs */
#ifdef LOCK_DEBUG
	pg_atomic_uint32 nwaiters;	/* number of waiters */
	struct PGPROC *owner;		/* last exclusive owner of the lock */
#endif
} LWLock;

/*
 * 大多数情况下，最好让每一组（tranche）LWLock 按缓存行边界对齐，并让数组
 * 的步长（stride）为 2 的幂。这能在索引计算时省下几个周期，但更重要的是
 * 保证单个 LWLock 不会跨越缓存行边界。这样可以减少缓存争用问题，
 * 在 AMD Opteron 平台上尤其明显。某些情况下，加入更多填充、让每个 LWLock
 * 独占一整条缓存行是有用的；例如在主 LWLock 数组（MainLWLockArray）中，
 * 锁的总数很少，但其中一些锁争用非常激烈，此时这种做法就很有价值。
 */
#define LWLOCK_PADDED_SIZE	PG_CACHE_LINE_SIZE

/* LWLock, padded to a full cache line size */
typedef union LWLockPadded
{
	LWLock		lock;
	char		pad[LWLOCK_PADDED_SIZE];
} LWLockPadded;

extern PGDLLIMPORT LWLockPadded *MainLWLockArray;

/* Names for fixed lwlocks */
#include "storage/lwlocknames.h"

/*
 * 把 NUM_BUFFER_PARTITIONS 和 NUM_LOCK_PARTITIONS 声明在这里有点奇怪，
 * 但我们需要它们来计算 MainLWLockArray 中的偏移量，而让本文件包含
 * lock.h 或 bufmgr.h 则会造成依赖倒置。
 */

/* Number of partitions of the shared buffer mapping hashtable */
#define NUM_BUFFER_PARTITIONS  128

/* Number of partitions the shared lock tables are divided into */
#define LOG2_NUM_LOCK_PARTITIONS  4
#define NUM_LOCK_PARTITIONS  (1 << LOG2_NUM_LOCK_PARTITIONS)

/* Number of partitions the shared predicate lock tables are divided into */
#define LOG2_NUM_PREDICATELOCK_PARTITIONS  4
#define NUM_PREDICATELOCK_PARTITIONS  (1 << LOG2_NUM_PREDICATELOCK_PARTITIONS)

/* Offsets for various chunks of preallocated lwlocks. */
#define BUFFER_MAPPING_LWLOCK_OFFSET	NUM_INDIVIDUAL_LWLOCKS
#define LOCK_MANAGER_LWLOCK_OFFSET		\
	(BUFFER_MAPPING_LWLOCK_OFFSET + NUM_BUFFER_PARTITIONS)
#define PREDICATELOCK_MANAGER_LWLOCK_OFFSET \
	(LOCK_MANAGER_LWLOCK_OFFSET + NUM_LOCK_PARTITIONS)
#define NUM_FIXED_LWLOCKS \
	(PREDICATELOCK_MANAGER_LWLOCK_OFFSET + NUM_PREDICATELOCK_PARTITIONS)

typedef enum LWLockMode
{
	LW_EXCLUSIVE,
	LW_SHARED,
	LW_WAIT_UNTIL_FREE			/* A special mode used in PGPROC->lwWaitMode,
								 * when waiting for lock to become free. Not
								 * to be used as LWLockAcquire argument */
} LWLockMode;


#ifdef LOCK_DEBUG
extern bool Trace_lwlocks;
#endif

extern bool LWLockAcquire(LWLock *lock, LWLockMode mode);
extern bool LWLockConditionalAcquire(LWLock *lock, LWLockMode mode);
extern bool LWLockAcquireOrWait(LWLock *lock, LWLockMode mode);
extern void LWLockRelease(LWLock *lock);
extern void LWLockReleaseClearVar(LWLock *lock, uint64 *valptr, uint64 val);
extern void LWLockReleaseAll(void);
extern void ForEachLWLockHeldByMe(void (*callback) (LWLock *, LWLockMode, void *),
								  void *context);
extern bool LWLockHeldByMe(LWLock *lock);
extern bool LWLockHeldByMeInMode(LWLock *lock, LWLockMode mode);

extern bool LWLockWaitForVar(LWLock *lock, uint64 *valptr, uint64 oldval, uint64 *newval);
extern void LWLockUpdateVar(LWLock *lock, uint64 *valptr, uint64 value);

extern Size LWLockShmemSize(void);
extern void CreateLWLocks(void);
extern void InitLWLockAccess(void);

extern const char *GetLWLockIdentifier(uint32 classId, uint16 eventId);

extern void LWLockInitialize(LWLock *lock, int tranche_id);

/*
 * 所有小于 NUM_INDIVIDUAL_LWLOCKS 的 tranche ID 都是保留的；此外，
 * 对于那些不在单个 LWLock 集合中的内置 tranche，我们还额外保留了一些
 * tranche ID。
 */
typedef enum BuiltinTrancheIds
{
	LWTRANCHE_XACT_BUFFER = NUM_INDIVIDUAL_LWLOCKS,
	LWTRANCHE_SUBTRANS_BUFFER,
	LWTRANCHE_MULTIXACTOFFSET_BUFFER,
	LWTRANCHE_MULTIXACTMEMBER_BUFFER,
	LWTRANCHE_NOTIFY_BUFFER,
	LWTRANCHE_SERIAL_BUFFER,
	LWTRANCHE_WAL_INSERT,
	LWTRANCHE_BUFFER_CONTENT,
	LWTRANCHE_REPLICATION_ORIGIN_STATE,
	LWTRANCHE_LOCK_FASTPATH,
	LWTRANCHE_BUFFER_MAPPING,
	LWTRANCHE_LOCK_MANAGER,
	LWTRANCHE_PREDICATE_LOCK_MANAGER,
	LWTRANCHE_PARALLEL_HASH_JOIN,
	LWTRANCHE_PARALLEL_QUERY_DSA,
	LWTRANCHE_PER_SESSION_DSA,
	LWTRANCHE_PER_SESSION_RECORD_TYPE,
	LWTRANCHE_PER_SESSION_RECORD_TYPMOD,
	LWTRANCHE_SHARED_TUPLESTORE,
	LWTRANCHE_SHARED_TIDBITMAP,
	LWTRANCHE_PARALLEL_APPEND,
	LWTRANCHE_PER_XACT_PREDICATE_LIST,
	LWTRANCHE_FIRST_USER_DEFINED
}			BuiltinTrancheIds;

/*
 * 在 PostgreSQL 9.4 之前，我们使用名为 LWLockId 的枚举类型来引用 LWLock。
 * 新代码应当改用 LWLock *。不过，为了方便第三方代码，我们仍提供下面这个
 * typedef。
 */
typedef LWLock *LWLockId;

#endif							/* LWLOCK_H */
