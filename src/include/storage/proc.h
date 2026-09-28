/*-------------------------------------------------------------------------
 *
 * proc.h
 *	  per-process shared memory data structures
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/proc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PROC_H_
#define _PROC_H_

#include "access/clog.h"
#include "access/xlogdefs.h"
#include "lib/ilist.h"
#include "storage/latch.h"
#include "storage/lock.h"
#include "storage/pg_sema.h"
#include "storage/proclist_types.h"

/*
 * Each backend advertises up to PGPROC_MAX_CACHED_SUBXIDS TransactionIds
 * for non-aborted subtransactions of its current top transaction.  These
 * have to be treated as running XIDs by other backends.
 *
 * We also keep track of whether the cache overflowed (ie, the transaction has
 * generated at least one subtransaction that didn't fit in the cache).
 * If none of the caches have overflowed, we can assume that an XID that's not
 * listed anywhere in the PGPROC array is not a running transaction.  Else we
 * have to look at pg_subtrans.
 */
#define PGPROC_MAX_CACHED_SUBXIDS 64	/* XXX guessed-at value */

typedef struct XidCacheStatus
{
	/* number of cached subxids, never more than PGPROC_MAX_CACHED_SUBXIDS */
	uint8		count;
	/* has PGPROC->subxids overflowed */
	bool		overflowed;
} XidCacheStatus;

struct XidCache
{
	TransactionId xids[PGPROC_MAX_CACHED_SUBXIDS];
};

/*
 * Flags for PGPROC->statusFlags and PROC_HDR->statusFlags[]
 */
#define		PROC_IN_VACUUM		0x02	/* currently running lazy vacuum */
#define		PROC_IN_SAFE_IC		0x04	/* currently running CREATE INDEX
										 * CONCURRENTLY or REINDEX
										 * CONCURRENTLY on non-expressional,
										 * non-partial index */
#define		PROC_IN_LOGICAL_DECODING	0x10	/* currently doing logical
												 * decoding outside xact */

/* flags reset at EOXact */
#define		PROC_VACUUM_STATE_MASK \
	(PROC_IN_VACUUM | PROC_IN_SAFE_IC)

/*
 * Xmin-related flags. Make sure any flags that affect how the process' Xmin
 * value is interpreted by VACUUM are included here.
 */
#define		PROC_XMIN_FLAGS (PROC_IN_VACUUM | PROC_IN_SAFE_IC)

/*
 * We allow a small number of "weak" relation locks (AccessShareLock,
 * RowShareLock, RowExclusiveLock) to be recorded in the PGPROC structure
 * rather than the main lock table.  This eases contention on the lock
 * manager LWLocks.  See storage/lmgr/README for additional details.
 */
#define		FP_LOCK_SLOTS_PER_BACKEND 16

/*
 * An invalid pgprocno.  Must be larger than the maximum number of PGPROC
 * structures we could possibly have.  See comments for MAX_BACKENDS.
 */
#define INVALID_PGPROCNO		PG_INT32_MAX

/*
 * Flags used only for type of internal functions
 * GetVirtualXIDsDelayingChkptGuts and HaveVirtualXIDsDelayingChkptGuts.
 */
#define DELAY_CHKPT_START		(1<<0)
#define DELAY_CHKPT_COMPLETE	(1<<1)

typedef enum
{
	PROC_WAIT_STATUS_OK,
	PROC_WAIT_STATUS_WAITING,
	PROC_WAIT_STATUS_ERROR,
} ProcWaitStatus;

/*
 * 每个后端（backend）在共享内存中都有一个 PGPROC 结构体。此外还有一个当前
 * 未使用的 PGPROC 结构体链表，这些结构体将被重新分配给新的后端。
 *
 * links：PGPROC 所在任意链表中的链接。当等待某个锁时，该 PGPROC 会被链接到
 * 该锁的 waitProcs 等待队列中。一个被回收的 PGPROC 会被链接到 ProcGlobal 的
 * freeProcs 空闲链表中。
 *
 * 我们允许在没有锁的情况下访问该结构体的许多字段，例如 delayChkpt。
 * 但是请注意，写入那些被镜像的字段（见下文）需要至少以
 * 共享模式持有 ProcArrayLock 或 XidGenLock，以免 pgxactoff 被并发地修改。
 *
 * 镜像字段（Mirrored fields）：
 *
 * PGPROC 中的某些字段（见 "mirrored in ..." 注释）会被镜像到打包得更紧凑的
 * ProcGlobal 数组的某个元素中。这些数组以 PGPROC->pgxactoff 为索引。两份副本
 * 需要保持一致性。
 *
 * 注意（NB）：以 pgxactoff 为索引的值*绝不*可以在未持有锁的情况下被访问。
 *
 * 详见 PROC_HDR。
 */
struct PGPROC
{
	/* proc->links MUST BE FIRST IN STRUCT (see ProcSleep,ProcWakeup,etc) */
	SHM_QUEUE	links;			/* list link if process is in a list */
	PGPROC	  **procgloballist; /* procglobal list that owns this PGPROC */

	PGSemaphore sem;			/* ONE semaphore to sleep on */
	ProcWaitStatus waitStatus;

	Latch		procLatch;		/* generic latch for process */


	TransactionId xid;			/* 顶层事务的 id，该事务当前正在
								 * 由本进程执行，若正在运行且 XID
								 * 已分配；否则为 InvalidTransactionId。
								 * 镜像保存在 ProcGlobal->xids[pgxactoff] 中 */

	TransactionId xmin;			/* 当我们启动事务时的最小运行 XID：
								 * 不含 LAZY VACUUM；
								 * vacuum 不得移除由 xid >= xmin 的事务
								 * 所删除的元组 ! */

	LocalTransactionId lxid;	/* 顶层事务的本地 id，该事务当前正在
								 * 由本进程执行，若正在运行；
								 * 否则为 InvalidLocalTransactionId */
	int			pid;			/* 后端进程 ID */

	int			pgxactoff;		/* offset into various ProcGlobal->arrays with
								 * data mirrored from this PGPROC */
	int			pgprocno;

	/* These fields are zero while a backend is still starting up: */
	BackendId	backendId;		/* This backend's backend ID (if assigned) */
	Oid			databaseId;		/* OID of database this backend is using */
	Oid			roleId;			/* OID of role using this backend */

	/* Info about LWLock the process is currently waiting for, if any. */
	uint8		lwWaiting;		/* see LWLockWaitState */
	uint8		lwWaitMode;		/* lwlock mode being waited for */
	proclist_node lwWaitLink;	/* position in LW lock wait list */

	/* Support for condition variables. */
	proclist_node cvWaitLink;	/* position in CV wait list */

	/* Info about lock the process is currently waiting for, if any. */
	/* waitLock and waitProcLock are NULL if not currently waiting. */
	LOCK	   *waitLock;		/* Lock object we're sleeping on ... */
	PROCLOCK   *waitProcLock;	/* Per-holder info for awaited lock */
	LOCKMODE	waitLockMode;	/* type of lock we're waiting for */
	LOCKMASK	heldLocks;		/* bitmask for lock types already held on this
								 * lock object by this backend */
	pg_atomic_uint64 waitStart; /* time at which wait for lock acquisition
								 * started */

	bool		delayChkpt;		/* true if this proc delays checkpoint start */

	uint8		statusFlags;	/* this backend's status flags, see PROC_*
								 * above. mirrored in
								 * ProcGlobal->statusFlags[pgxactoff] */
	bool		delayChkptEnd;	/* true if this proc delays checkpoint end */

	/*
	 * All PROCLOCK objects for locks held or awaited by this backend are
	 * linked into one of these lists, according to the partition number of
	 * their lock.
	 */
	SHM_QUEUE	myProcLocks[NUM_LOCK_PARTITIONS];

	XidCacheStatus subxidStatus;	/* mirrored with
									 * ProcGlobal->subxidStates[i] */
	struct XidCache subxids;	/* cache for subtransaction XIDs */

	/* Support for group XID clearing. */
	/* true, if member of ProcArray group waiting for XID clear */
	bool		procArrayGroupMember;
	/* next ProcArray group member waiting for XID clear */
	pg_atomic_uint32 procArrayGroupNext;

	/*
	 * latest transaction id among the transaction's main XID and
	 * subtransactions
	 */
	TransactionId procArrayGroupMemberXid;

	uint32		wait_event_info;	/* proc's wait information */

	/* Support for group transaction status update. */
	bool		clogGroupMember;	/* true, if member of clog group */
	pg_atomic_uint32 clogGroupNext; /* next clog group member */
	TransactionId clogGroupMemberXid;	/* transaction id of clog group member */
	XidStatus	clogGroupMemberXidStatus;	/* transaction status of clog
											 * group member */
	int			clogGroupMemberPage;	/* clog page corresponding to
										 * transaction id of clog group member */
	XLogRecPtr	clogGroupMemberLsn; /* WAL location of commit record for clog
									 * group member */

	/* Lock manager data, recording fast-path locks taken by this backend. */
	LWLock		fpInfoLock;		/* protects per-backend fast-path state */
	uint64		fpLockBits;		/* lock modes held for each fast-path slot */
	Oid			fpRelId[FP_LOCK_SLOTS_PER_BACKEND]; /* slots for rel oids */
	bool		fpVXIDLock;		/* are we holding a fast-path VXID lock? */
	LocalTransactionId fpLocalTransactionId;	/* lxid for fast-path VXID
												 * lock */

	/*
	 * Support for lock groups.  Use LockHashPartitionLockByProc on the group
	 * leader to get the LWLock protecting these fields.
	 */
	PGPROC	   *lockGroupLeader;	/* lock group leader, if I'm a member */
	dlist_head	lockGroupMembers;	/* list of members, if I'm a leader */
	dlist_node	lockGroupLink;	/* my member link, if I'm a member */
};

/* NOTE: "typedef struct PGPROC PGPROC" appears in storage/lock.h. */


extern PGDLLIMPORT PGPROC *MyProc;

/*
 * 整个数据库集群只有一个 ProcGlobal 结构体。
 *
 * 向 procarray 中添加/移除条目需要以排他模式同时（*both*）持有
 * ProcArrayLock 和 XidGenLock（按此顺序）。之所以两者都需要，是因为
 * 稠密数组（dense array，见下文）会被 GetNewTransactionId() 和
 * GetSnapshotData() 访问，而我们不希望让这两个函数使用同一把锁而进一步
 * 加剧争用。添加/移除 procarray 条目的频率则低得多。
 *
 * PGPROC 中的某些字段会被镜像到打包得更紧密的数组中（例如 xids），
 * 每个后端对应一个条目。这些数组只包含那些已经用 ProcArrayAdd() 加入
 * 共享数组的 PGPROC 的条目（这与 PGPROC 数组不同，后者中间还夹杂着
 * 未被使用的 PGPROC）。
 *
 * 稠密数组用 PGPROC->pgxactoff 作为索引。任何并发的 ProcArrayAdd() /
 * ProcArrayRemove() 都可能导致某个 procarray 成员的 pgxactoff 发生变化。
 * 因此，只有在持有 ProcArrayLock 或 XidGenLock 时，使用
 * PGPROC->pgxactoff 访问稠密数组才是安全的。
 *
 * 只要某个 PGPROC 还在 procarray 中，镜像到两处的值就必须以一致的
 * 方式加以维护。
 *
 * 使用这些更紧凑的独立数组主要有三个好处：第一，可以让访问数据的循环
 * 尽可能紧凑。第二，可以避免对频繁变化数据（例如 xmin）的更新使得同时
 * 包含较少变化数据（例如 xid、statusFlags）的缓存行失效。第三，把频繁
 * 访问的数据压缩到尽可能少的缓存行中。
 *
 * 让数据在这些稠密数组与 PGPROC 之间保持镜像，主要有两个原因。第一，
 * 如上所述，PGPROC 的数组条目只能在持有 ProcArrayLock 或 XidGenLock 时
 * 访问，而 PGPROC 中的条目则没有这一要求（显然，围绕各个字段本身可能
 * 仍有加锁要求，这与这里讨论的问题无关）。这一点对后端高效地检查自己的
 * 取值尤为重要，因为它通常可以在不加锁的情况下安全地这样做。第二，
 * PGPROC 中的字段可以避免对稠密数组进行不必要的访问和修改。后端自己的
 * PGPROC 更可能位于本地缓存中，而稠密数组的缓存行则会被其他后端修改
 * （因而常常会从其他核心/插槽的缓存中被逐出）。在提交/回滚时，检查
 * PGPROC 中的取值就可以避免访问/弄脏稠密数组中对应的值。
 *
 * 基本上，当检查单个后端的数据时，访问 PGPROC 中的变量是合理的做法，
 * 特别是在已经出于其他原因查看该 PGPROC 的情况下。如果我们需要查看
 * 许多/大部分条目，则查看"稠密"数组是合理的，因为这样可以从更少的
 * 间接跳转以及更好的跨进程缓存友好性中获益。
 *
 * 当通过 ProcArrayAdd() 为 2PC 事务加入一个 PGPROC 时，稠密数组中的
 * 数据是在已持有 ProcArrayLock 的情况下从 PGPROC 初始化的。
 */
typedef struct PROC_HDR
{
	/* Array of PGPROC structures */
	PGPROC	   *allProcs;

	/* Array mirroring PGPROC.xid for each PGPROC currently in the procarray */
	TransactionId *xids;

	/*
	 * Array mirroring PGPROC.subxidStatus for each PGPROC currently in the
	 * procarray.
	 */
	XidCacheStatus *subxidStates;

	/*
	 * Array mirroring PGPROC.statusFlags for each PGPROC currently in the
	 * procarray.
	 */
	uint8	   *statusFlags;

	/* Length of allProcs array */
	uint32		allProcCount;
	/* Head of list of free PGPROC structures */
	PGPROC	   *freeProcs;
	/* First pgproc waiting for group XID clear */
	pg_atomic_uint32 procArrayGroupFirst;
	/* First pgproc waiting for group transaction status update */
	pg_atomic_uint32 clogGroupFirst;
	/* WALWriter process's latch */
	Latch	   *walwriterLatch;
	/* Checkpointer process's latch */
	Latch	   *checkpointerLatch;
	/* Current shared estimate of appropriate spins_per_delay value */
	int			spins_per_delay;
	/* The proc of the Startup process, since not in ProcArray */
	PGPROC	   *startupProc;
	int			startupProcPid;
} PROC_HDR;

extern PGDLLIMPORT PROC_HDR *ProcGlobal;

/* Accessor for PGPROC given a pgprocno. */
#define GetPGProcByNumber(n) (&ProcGlobal->allProcs[(n)])

/*
 * We set aside some extra PGPROC structures for auxiliary processes,
 * ie things that aren't full-fledged backends but need shmem access.
 *
 * Background writer, checkpointer, WAL writer and archiver run during normal
 * operation.  Startup process and WAL receiver also consume 2 slots, but WAL
 * writer is launched only after startup has exited, so we only need 5 slots.
 */
#define NUM_AUXILIARY_PROCS		5

/* configurable options */
extern PGDLLIMPORT int DeadlockTimeout;
extern PGDLLIMPORT int LockTimeout;
extern PGDLLIMPORT int IdleInTransactionSessionTimeout;
extern bool log_lock_waits;


/*
 * Function Prototypes
 */
extern int	ProcGlobalSemas(void);
extern Size ProcGlobalShmemSize(void);
extern void InitProcGlobal(void);
extern void InitProcess(void);
extern void InitProcessPhase2(void);
extern void InitAuxiliaryProcess(void);

extern void PublishStartupProcessInformation(void);

extern void ProcReleaseLocks(bool isCommit);

extern void ProcQueueInit(PROC_QUEUE *queue);
extern ProcWaitStatus ProcSleep(LOCALLOCK *locallock, LockMethod lockMethodTable);
extern PGPROC *ProcWakeup(PGPROC *proc, ProcWaitStatus waitStatus);
extern void ProcLockWakeup(LockMethod lockMethodTable, LOCK *lock);
extern void CheckDeadLockAlert(void);
extern void LockErrorCleanup(void);

extern void ProcWaitForSignal(uint32 wait_event_info);
extern void ProcSendSignal(int pid);

extern PGPROC *AuxiliaryPidGetProc(int pid);

extern void BecomeLockGroupLeader(void);
extern bool BecomeLockGroupMember(PGPROC *leader, int pid);

#endif							/* _PROC_H_ */
