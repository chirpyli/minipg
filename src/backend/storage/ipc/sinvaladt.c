/*-------------------------------------------------------------------------
 *
 * sinvaladt.c
 *	  POSTGRES shared cache invalidation data manager.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/sinvaladt.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/transam.h"
#include "miscadmin.h"
#include "storage/backendid.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "storage/sinvaladt.h"
#include "storage/spin.h"

/*
 * 从概念上讲，共享的缓存失效消息存储在一个无限长的数组中：maxMsgNum 是
 * 存放下一条提交消息的数组下标，minMsgNum 是"尚未被所有后端读到"的消息
 * 所占据的最小数组下标，且始终有 maxMsgNum >= minMsgNum。（当没有待处理
 * 消息时二者相等。）每个活跃后端都有一个 nextMsgNum 指针，指示它下一条
 * 需要读取的消息；对每个后端都有 maxMsgNum >= nextMsgNum >= minMsgNum。
 *
 * （在当前实现中，minMsgNum 是各进程 nextMsgNum 值的下界，但并没有严格
 * 保持等于最小的 nextMsgNum --- 它可能滞后。我们只在调用 SICleanupQueue
 * 时更新它，并且尽量不频繁地调用该函数。）
 *
 * 实际上，这些消息存放在一个含 MAXNUMMESSAGES 个条目的环形缓冲区中。
 * 我们通过计算 MsgNum % MAXNUMMESSAGES 把 MsgNum 值翻译成环形缓冲区下标
 * （只要 MAXNUMMESSAGES 是常量且为 2 的幂，这个计算就应当很快）。只要
 * maxMsgNum 超出 minMsgNum 的量不超过 MAXNUMMESSAGES，缓冲区空间就够用。
 * 如果缓冲区确实溢出，我们就把落后太多的每个后端都置上 "reset" 标志，
 * 以此恢复。处于 "reset" 状态的后端在确定 minMsgNum 时会被忽略。当它
 * 最终尝试接收失效消息时，必须丢弃它所有可失效的状态，因为它不知道自己
 * 错过了什么。
 *
 * 为了降低需要 reset 的概率，我们会向任何看起来落后得离谱的后端发送
 * "catchup"（追赶）中断。通常的行为是：同时至多只有一个这样的中断在途；
 * 当某个后端处理完一个 catchup 中断时，它会执行 SICleanupQueue，后者会
 * 在需要时向下一个最落后的后端发信号。这避免了多个后端同时追赶所造成的
 * 过度争抢。不过，最落后的那个后端可能卡在某种无法追赶的状态中。它最终
 * 会被 reset，因此除了它自己，不会再给其他人添麻烦。但我们不愿意看到
 * 一批其他后端现在也逼近 reset 阈值、来不及挽救。所以 SICleanupQueue
 * 被设计成：随着队列变满，偶尔会向那些落后很远、但还没收到过中断的后端
 * 额外发送 catchup 中断。只要"卡住"的后端不多，我们就不需要太多额外
 * 中断，因为没卡住的后端会把中断传递给下一个进程。
 *
 * 如果 MsgNum 的值溢出了整数范围，就会出问题，所以每当 minMsgNum 超过
 * MSGNUMWRAPAROUND 时，我们就同时从所有 MsgNum 变量中减去
 * MSGNUMWRAPAROUND。MSGNUMWRAPAROUND 可以取得很大，这样就不必频繁做这
 * 件事。它必须是 MAXNUMMESSAGES 的整数倍，这样在做这种调整时，已有的
 * 环形缓冲区条目不需要移动。
 *
 * 对共享 sinval 数组的访问由两把锁保护：SInvalReadLock 和
 * SInvalWriteLock。读者以共享模式获取 SInvalReadLock；这授权它们修改
 * 自己的 ProcState，但不得修改、甚至不得查看他人的 ProcState。当需要做
 * 数组范围的更新时（例如 SICleanupQueue 中），我们以排他模式获取
 * SInvalReadLock，把所有读者挡在外面。写者获取 SInvalWriteLock（总是
 * 排他模式）来串行化向队列中添加消息的操作。注意，写者可以与一个或多个
 * 读者并行工作，因为写者无需触碰任何人的 ProcState，除非在少数需要
 * SICleanupQueue 的情况下。唯一的重叠点是：写者想修改 maxMsgNum，而
 * 读者需要读取它。我们的处理办法是使用一把自旋锁，读者必须持有它刚好
 * 足够读取 maxMsgNum 的时间，写者持有它刚好足够写入 maxMsgNum 的时间。
 * （确切的规则是：如果你没有持有 SInvalWriteLock，读取 maxMsgNum 时
 * 需要持有该自旋锁；除非你同时持有两把锁，否则写入 maxMsgNum 时也需要
 * 持有该自旋锁。）
 *
 * 注意：既然 maxMsgNum 是 int，因而大概可以原子地读写，那么这把自旋锁
 * 似乎没有必要。之所以需要它，是为了提供内存屏障：我们必须确保写入数组
 * 的消息在 maxMsgNum 增加之前确实已经就位，并且读者在取到 maxMsgNum
 * 之后能看到那些数据。内存序保证较弱的多处理器，如果没有自旋锁序列中
 * 包含的内存屏障指令，就可能出错。
 */


/*
 * Configurable parameters.
 *
 * MAXNUMMESSAGES: max number of shared-inval messages we can buffer.
 * Must be a power of 2 for speed.
 *
 * MSGNUMWRAPAROUND: how often to reduce MsgNum variables to avoid overflow.
 * Must be a multiple of MAXNUMMESSAGES.  Should be large.
 *
 * CLEANUP_MIN: the minimum number of messages that must be in the buffer
 * before we bother to call SICleanupQueue.
 *
 * CLEANUP_QUANTUM: how often (in messages) to call SICleanupQueue once
 * we exceed CLEANUP_MIN.  Should be a power of 2 for speed.
 *
 * SIG_THRESHOLD: the minimum number of messages a backend must have fallen
 * behind before we'll send it PROCSIG_CATCHUP_INTERRUPT.
 *
 * WRITE_QUANTUM: the max number of messages to push into the buffer per
 * iteration of SIInsertDataEntries.  Noncritical but should be less than
 * CLEANUP_QUANTUM, because we only consider calling SICleanupQueue once
 * per iteration.
 */

#define MAXNUMMESSAGES 4096
#define MSGNUMWRAPAROUND (MAXNUMMESSAGES * 262144)
#define CLEANUP_MIN (MAXNUMMESSAGES / 2)
#define CLEANUP_QUANTUM (MAXNUMMESSAGES / 16)
#define SIG_THRESHOLD (MAXNUMMESSAGES / 2)
#define WRITE_QUANTUM 64

/* Per-backend state in shared invalidation structure */
typedef struct ProcState
{
	/* procPid is zero in an inactive ProcState array entry. */
	pid_t		procPid;		/* PID of backend, for signaling */
	PGPROC	   *proc;			/* PGPROC of backend */
	/* nextMsgNum is meaningless if procPid == 0 or resetState is true. */
	int			nextMsgNum;		/* next message number to read */
	bool		resetState;		/* backend needs to reset its state */
	bool		signaled;		/* backend has been sent catchup signal */
	bool		hasMessages;	/* backend has unread messages */

	/*
	 * Backend only sends invalidations, never receives them. This only makes
	 * sense for Startup process during recovery because it doesn't maintain a
	 * relcache, yet it fires inval messages to allow query backends to see
	 * schema changes.
	 */
	bool		sendOnly;		/* backend only sends, never receives */

	/*
	 * Next LocalTransactionId to use for each idle backend slot.  We keep
	 * this here because it is indexed by BackendId and it is convenient to
	 * copy the value to and from local memory when MyBackendId is set. It's
	 * meaningless in an active ProcState entry.
	 */
	LocalTransactionId nextLXID;
} ProcState;

/* Shared cache invalidation memory segment */
typedef struct SISeg
{
	/*
	 * General state information
	 */
	int			minMsgNum;		/* oldest message still needed */
	int			maxMsgNum;		/* next message number to be assigned */
	int			nextThreshold;	/* # of messages to call SICleanupQueue */
	int			lastBackend;	/* index of last active procState entry, +1 */
	int			maxBackends;	/* size of procState array */

	slock_t		msgnumLock;		/* spinlock protecting maxMsgNum */

	/*
	 * Circular buffer holding shared-inval messages
	 */
	SharedInvalidationMessage buffer[MAXNUMMESSAGES];

	/*
	 * Per-backend invalidation state info (has MaxBackends entries).
	 */
	ProcState	procState[FLEXIBLE_ARRAY_MEMBER];
} SISeg;

static SISeg *shmInvalBuffer;	/* pointer to the shared inval buffer */


static LocalTransactionId nextLocalTransactionId;

static void CleanupInvalidationState(int status, Datum arg);


/*
 * SInvalShmemSize --- return shared-memory space needed
 */
Size
SInvalShmemSize(void)
{
	Size		size;

	size = offsetof(SISeg, procState);
	size = add_size(size, mul_size(sizeof(ProcState), MaxBackends));

	return size;
}

/*
 * CreateSharedInvalidationState
 *		Create and initialize the SI message buffer
 */
void
CreateSharedInvalidationState(void)
{
	int			i;
	bool		found;

	/* Allocate space in shared memory */
	shmInvalBuffer = (SISeg *)
		ShmemInitStruct("shmInvalBuffer", SInvalShmemSize(), &found);
	if (found)
		return;

	/* Clear message counters, save size of procState array, init spinlock */
	shmInvalBuffer->minMsgNum = 0;
	shmInvalBuffer->maxMsgNum = 0;
	shmInvalBuffer->nextThreshold = CLEANUP_MIN;
	shmInvalBuffer->lastBackend = 0;
	shmInvalBuffer->maxBackends = MaxBackends;
	SpinLockInit(&shmInvalBuffer->msgnumLock);

	/* The buffer[] array is initially all unused, so we need not fill it */

	/* Mark all backends inactive, and initialize nextLXID */
	for (i = 0; i < shmInvalBuffer->maxBackends; i++)
	{
		shmInvalBuffer->procState[i].procPid = 0;	/* inactive */
		shmInvalBuffer->procState[i].proc = NULL;
		shmInvalBuffer->procState[i].nextMsgNum = 0;	/* meaningless */
		shmInvalBuffer->procState[i].resetState = false;
		shmInvalBuffer->procState[i].signaled = false;
		shmInvalBuffer->procState[i].hasMessages = false;
		shmInvalBuffer->procState[i].nextLXID = InvalidLocalTransactionId;
	}
}

/*
 * SharedInvalBackendInit
 *		Initialize a new backend to operate on the sinval buffer
 */
void
SharedInvalBackendInit(bool sendOnly)
{
	int			index;
	ProcState  *stateP = NULL;
	SISeg	   *segP = shmInvalBuffer;

	/*
	 * This can run in parallel with read operations, but not with write
	 * operations, since SIInsertDataEntries relies on lastBackend to set
	 * hasMessages appropriately.
	 */
	LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);

	/* Look for a free entry in the procState array */
	for (index = 0; index < segP->lastBackend; index++)
	{
		if (segP->procState[index].procPid == 0)	/* inactive slot? */
		{
			stateP = &segP->procState[index];
			break;
		}
	}

	if (stateP == NULL)
	{
		if (segP->lastBackend < segP->maxBackends)
		{
			stateP = &segP->procState[segP->lastBackend];
			Assert(stateP->procPid == 0);
			segP->lastBackend++;
		}
		else
		{
			/*
			 * out of procState slots: MaxBackends exceeded -- report normally
			 */
			MyBackendId = InvalidBackendId;
			LWLockRelease(SInvalWriteLock);
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("sorry, too many clients already")));
		}
	}

	MyBackendId = (stateP - &segP->procState[0]) + 1;

	/* Advertise assigned backend ID in MyProc */
	MyProc->backendId = MyBackendId;

	/* Fetch next local transaction ID into local memory */
	nextLocalTransactionId = stateP->nextLXID;

	/* mark myself active, with all extant messages already read */
	stateP->procPid = MyProcPid;
	stateP->proc = MyProc;
	stateP->nextMsgNum = segP->maxMsgNum;
	stateP->resetState = false;
	stateP->signaled = false;
	stateP->hasMessages = false;
	stateP->sendOnly = sendOnly;

	LWLockRelease(SInvalWriteLock);

	/* register exit routine to mark my entry inactive at exit */
	on_shmem_exit(CleanupInvalidationState, PointerGetDatum(segP));

	elog(DEBUG4, "my backend ID is %d", MyBackendId);
}

/*
 * CleanupInvalidationState
 *		Mark the current backend as no longer active.
 *
 * This function is called via on_shmem_exit() during backend shutdown.
 *
 * arg is really of type "SISeg*".
 */
static void
CleanupInvalidationState(int status, Datum arg)
{
	SISeg	   *segP = (SISeg *) DatumGetPointer(arg);
	ProcState  *stateP;
	int			i;

	Assert(PointerIsValid(segP));

	LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);

	stateP = &segP->procState[MyBackendId - 1];

	/* Update next local transaction ID for next holder of this backendID */
	stateP->nextLXID = nextLocalTransactionId;

	/* Mark myself inactive */
	stateP->procPid = 0;
	stateP->proc = NULL;
	stateP->nextMsgNum = 0;
	stateP->resetState = false;
	stateP->signaled = false;

	/* Recompute index of last active backend */
	for (i = segP->lastBackend; i > 0; i--)
	{
		if (segP->procState[i - 1].procPid != 0)
			break;
	}
	segP->lastBackend = i;

	LWLockRelease(SInvalWriteLock);
}

/*
 * BackendIdGetProc
 *		Get the PGPROC structure for a backend, given the backend ID.
 *		The result may be out of date arbitrarily quickly, so the caller
 *		must be careful about how this information is used.  NULL is
 *		returned if the backend is not active.
 */
PGPROC *
BackendIdGetProc(int backendID)
{
	PGPROC	   *result = NULL;
	SISeg	   *segP = shmInvalBuffer;

	/* Need to lock out additions/removals of backends */
	LWLockAcquire(SInvalWriteLock, LW_SHARED);

	if (backendID > 0 && backendID <= segP->lastBackend)
	{
		ProcState  *stateP = &segP->procState[backendID - 1];

		result = stateP->proc;
	}

	LWLockRelease(SInvalWriteLock);

	return result;
}

/*
 * BackendIdGetTransactionIds
 *		Get the xid and xmin of the backend. The result may be out of date
 *		arbitrarily quickly, so the caller must be careful about how this
 *		information is used.
 */
void
BackendIdGetTransactionIds(int backendID, TransactionId *xid, TransactionId *xmin)
{
	SISeg	   *segP = shmInvalBuffer;

	*xid = InvalidTransactionId;
	*xmin = InvalidTransactionId;

	/* Need to lock out additions/removals of backends */
	LWLockAcquire(SInvalWriteLock, LW_SHARED);

	if (backendID > 0 && backendID <= segP->lastBackend)
	{
		ProcState  *stateP = &segP->procState[backendID - 1];
		PGPROC	   *proc = stateP->proc;

		if (proc != NULL)
		{
			*xid = proc->xid;
			*xmin = proc->xmin;
		}
	}

	LWLockRelease(SInvalWriteLock);
}

/*
 * SIInsertDataEntries
 *		Add new invalidation message(s) to the buffer.
 */
void
SIInsertDataEntries(const SharedInvalidationMessage *data, int n)
{
	SISeg	   *segP = shmInvalBuffer;

	/*
	 * N can be arbitrarily large.  We divide the work into groups of no more
	 * than WRITE_QUANTUM messages, to be sure that we don't hold the lock for
	 * an unreasonably long time.  (This is not so much because we care about
	 * letting in other writers, as that some just-caught-up backend might be
	 * trying to do SICleanupQueue to pass on its signal, and we don't want it
	 * to have to wait a long time.)  Also, we need to consider calling
	 * SICleanupQueue every so often.
	 */
	while (n > 0)
	{
		int			nthistime = Min(n, WRITE_QUANTUM);
		int			numMsgs;
		int			max;
		int			i;

		n -= nthistime;

		LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);

		/*
		 * If the buffer is full, we *must* acquire some space.  Clean the
		 * queue and reset anyone who is preventing space from being freed.
		 * Otherwise, clean the queue only when it's exceeded the next
		 * fullness threshold.  We have to loop and recheck the buffer state
		 * after any call of SICleanupQueue.
		 */
		for (;;)
		{
			numMsgs = segP->maxMsgNum - segP->minMsgNum;
			if (numMsgs + nthistime > MAXNUMMESSAGES ||
				numMsgs >= segP->nextThreshold)
				SICleanupQueue(true, nthistime);
			else
				break;
		}

		/*
		 * Insert new message(s) into proper slot of circular buffer
		 */
		max = segP->maxMsgNum;
		while (nthistime-- > 0)
		{
			segP->buffer[max % MAXNUMMESSAGES] = *data++;
			max++;
		}

		/* Update current value of maxMsgNum using spinlock */
		SpinLockAcquire(&segP->msgnumLock);
		segP->maxMsgNum = max;
		SpinLockRelease(&segP->msgnumLock);

		/*
		 * Now that the maxMsgNum change is globally visible, we give everyone
		 * a swift kick to make sure they read the newly added messages.
		 * Releasing SInvalWriteLock will enforce a full memory barrier, so
		 * these (unlocked) changes will be committed to memory before we exit
		 * the function.
		 */
		for (i = 0; i < segP->lastBackend; i++)
		{
			ProcState  *stateP = &segP->procState[i];

			stateP->hasMessages = true;
		}

		LWLockRelease(SInvalWriteLock);
	}
}

/*
 * SIGetDataEntries
 *		get next SI message(s) for current backend, if there are any
 *
 * Possible return values:
 *	0:	 no SI message available
 *	n>0: next n SI messages have been extracted into data[]
 * -1:	 SI reset message extracted
 *
 * If the return value is less than the array size "datasize", the caller
 * can assume that there are no more SI messages after the one(s) returned.
 * Otherwise, another call is needed to collect more messages.
 *
 * NB: this can run in parallel with other instances of SIGetDataEntries
 * executing on behalf of other backends, since each instance will modify only
 * fields of its own backend's ProcState, and no instance will look at fields
 * of other backends' ProcStates.  We express this by grabbing SInvalReadLock
 * in shared mode.  Note that this is not exactly the normal (read-only)
 * interpretation of a shared lock! Look closely at the interactions before
 * allowing SInvalReadLock to be grabbed in shared mode for any other reason!
 *
 * NB: this can also run in parallel with SIInsertDataEntries.  It is not
 * guaranteed that we will return any messages added after the routine is
 * entered.
 *
 * Note: we assume that "datasize" is not so large that it might be important
 * to break our hold on SInvalReadLock into segments.
 */
int
SIGetDataEntries(SharedInvalidationMessage *data, int datasize)
{
	SISeg	   *segP;
	ProcState  *stateP;
	int			max;
	int			n;

	segP = shmInvalBuffer;
	stateP = &segP->procState[MyBackendId - 1];

	/*
	 * Before starting to take locks, do a quick, unlocked test to see whether
	 * there can possibly be anything to read.  On a multiprocessor system,
	 * it's possible that this load could migrate backwards and occur before
	 * we actually enter this function, so we might miss a sinval message that
	 * was just added by some other processor.  But they can't migrate
	 * backwards over a preceding lock acquisition, so it should be OK.  If we
	 * haven't acquired a lock preventing against further relevant
	 * invalidations, any such occurrence is not much different than if the
	 * invalidation had arrived slightly later in the first place.
	 */
	if (!stateP->hasMessages)
		return 0;

	LWLockAcquire(SInvalReadLock, LW_SHARED);

	/*
	 * We must reset hasMessages before determining how many messages we're
	 * going to read.  That way, if new messages arrive after we have
	 * determined how many we're reading, the flag will get reset and we'll
	 * notice those messages part-way through.
	 *
	 * Note that, if we don't end up reading all of the messages, we had
	 * better be certain to reset this flag before exiting!
	 */
	stateP->hasMessages = false;

	/* Fetch current value of maxMsgNum using spinlock */
	SpinLockAcquire(&segP->msgnumLock);
	max = segP->maxMsgNum;
	SpinLockRelease(&segP->msgnumLock);

	if (stateP->resetState)
	{
		/*
		 * Force reset.  We can say we have dealt with any messages added
		 * since the reset, as well; and that means we should clear the
		 * signaled flag, too.
		 */
		stateP->nextMsgNum = max;
		stateP->resetState = false;
		stateP->signaled = false;
		LWLockRelease(SInvalReadLock);
		return -1;
	}

	/*
	 * Retrieve messages and advance backend's counter, until data array is
	 * full or there are no more messages.
	 *
	 * There may be other backends that haven't read the message(s), so we
	 * cannot delete them here.  SICleanupQueue() will eventually remove them
	 * from the queue.
	 */
	n = 0;
	while (n < datasize && stateP->nextMsgNum < max)
	{
		data[n++] = segP->buffer[stateP->nextMsgNum % MAXNUMMESSAGES];
		stateP->nextMsgNum++;
	}

	/*
	 * If we have caught up completely, reset our "signaled" flag so that
	 * we'll get another signal if we fall behind again.
	 *
	 * If we haven't caught up completely, reset the hasMessages flag so that
	 * we see the remaining messages next time.
	 */
	if (stateP->nextMsgNum >= max)
		stateP->signaled = false;
	else
		stateP->hasMessages = true;

	LWLockRelease(SInvalReadLock);
	return n;
}

/*
 * SICleanupQueue
 *		Remove messages that have been consumed by all active backends
 *
 * callerHasWriteLock is true if caller is holding SInvalWriteLock.
 * minFree is the minimum number of message slots to make free.
 *
 * Possible side effects of this routine include marking one or more
 * backends as "reset" in the array, and sending PROCSIG_CATCHUP_INTERRUPT
 * to some backend that seems to be getting too far behind.  We signal at
 * most one backend at a time, for reasons explained at the top of the file.
 *
 * Caution: because we transiently release write lock when we have to signal
 * some other backend, it is NOT guaranteed that there are still minFree
 * free message slots at exit.  Caller must recheck and perhaps retry.
 */
void
SICleanupQueue(bool callerHasWriteLock, int minFree)
{
	SISeg	   *segP = shmInvalBuffer;
	int			min,
				minsig,
				lowbound,
				numMsgs,
				i;
	ProcState  *needSig = NULL;

	/* Lock out all writers and readers */
	if (!callerHasWriteLock)
		LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);
	LWLockAcquire(SInvalReadLock, LW_EXCLUSIVE);

	/*
	 * Recompute minMsgNum = minimum of all backends' nextMsgNum, identify the
	 * furthest-back backend that needs signaling (if any), and reset any
	 * backends that are too far back.  Note that because we ignore sendOnly
	 * backends here it is possible for them to keep sending messages without
	 * a problem even when they are the only active backend.
	 */
	min = segP->maxMsgNum;
	minsig = min - SIG_THRESHOLD;
	lowbound = min - MAXNUMMESSAGES + minFree;

	for (i = 0; i < segP->lastBackend; i++)
	{
		ProcState  *stateP = &segP->procState[i];
		int			n = stateP->nextMsgNum;

		/* Ignore if inactive or already in reset state */
		if (stateP->procPid == 0 || stateP->resetState || stateP->sendOnly)
			continue;

		/*
		 * If we must free some space and this backend is preventing it, force
		 * him into reset state and then ignore until he catches up.
		 */
		if (n < lowbound)
		{
			stateP->resetState = true;
			/* no point in signaling him ... */
			continue;
		}

		/* Track the global minimum nextMsgNum */
		if (n < min)
			min = n;

		/* Also see who's furthest back of the unsignaled backends */
		if (n < minsig && !stateP->signaled)
		{
			minsig = n;
			needSig = stateP;
		}
	}
	segP->minMsgNum = min;

	/*
	 * When minMsgNum gets really large, decrement all message counters so as
	 * to forestall overflow of the counters.  This happens seldom enough that
	 * folding it into the previous loop would be a loser.
	 */
	if (min >= MSGNUMWRAPAROUND)
	{
		segP->minMsgNum -= MSGNUMWRAPAROUND;
		segP->maxMsgNum -= MSGNUMWRAPAROUND;
		for (i = 0; i < segP->lastBackend; i++)
		{
			/* we don't bother skipping inactive entries here */
			segP->procState[i].nextMsgNum -= MSGNUMWRAPAROUND;
		}
	}

	/*
	 * Determine how many messages are still in the queue, and set the
	 * threshold at which we should repeat SICleanupQueue().
	 */
	numMsgs = segP->maxMsgNum - segP->minMsgNum;
	if (numMsgs < CLEANUP_MIN)
		segP->nextThreshold = CLEANUP_MIN;
	else
		segP->nextThreshold = (numMsgs / CLEANUP_QUANTUM + 1) * CLEANUP_QUANTUM;

	/*
	 * Lastly, signal anyone who needs a catchup interrupt.  Since
	 * SendProcSignal() might not be fast, we don't want to hold locks while
	 * executing it.
	 */
	if (needSig)
	{
		pid_t		his_pid = needSig->procPid;
		BackendId	his_backendId = (needSig - &segP->procState[0]) + 1;

		needSig->signaled = true;
		LWLockRelease(SInvalReadLock);
		LWLockRelease(SInvalWriteLock);
		elog(DEBUG4, "sending sinval catchup signal to PID %d", (int) his_pid);
		SendProcSignal(his_pid, PROCSIG_CATCHUP_INTERRUPT, his_backendId);
		if (callerHasWriteLock)
			LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);
	}
	else
	{
		LWLockRelease(SInvalReadLock);
		if (!callerHasWriteLock)
			LWLockRelease(SInvalWriteLock);
	}
}

/*
 * SIResetAll
 *		Mark all active backends as "reset"
 *
 * Use this when we don't know what needs to be invalidated.  It's a
 * cluster-wide InvalidateSystemCaches().  This was a back-branch-only remedy
 * to avoid a WAL format change.
 *
 * The implementation is like SICleanupQueue(false, MAXNUMMESSAGES + 1), with
 * one addition.  SICleanupQueue() assumes minFree << MAXNUMMESSAGES, so it
 * assumes hasMessages==true for any backend it resets.  We're resetting even
 * fully-caught-up backends, so we set hasMessages.
 */
void
SIResetAll(void)
{
	SISeg	   *segP = shmInvalBuffer;
	int			i;

	LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);
	LWLockAcquire(SInvalReadLock, LW_EXCLUSIVE);

	for (i = 0; i < segP->lastBackend; i++)
	{
		ProcState  *stateP = &segP->procState[i];

		if (stateP->procPid == 0 || stateP->sendOnly)
			continue;

		/* Consuming the reset will update "nextMsgNum" and "signaled". */
		stateP->resetState = true;
		stateP->hasMessages = true;
	}

	segP->minMsgNum = segP->maxMsgNum;
	segP->nextThreshold = CLEANUP_MIN;

	LWLockRelease(SInvalReadLock);
	LWLockRelease(SInvalWriteLock);
}


/*
 * GetNextLocalTransactionId --- 分配一个新的 LocalTransactionId
 *
 * 我们把 VirtualTransactionId 拆分为两部分，这样就可以在不受共享内存
 * 竞争影响的情况下分配新的 ID，代价只是后端启动/关闭期间的一点额外
 * 开销。VirtualTransactionId 的高位部分是 BackendId，低位部分是
 * LocalTransactionId，后者由一个本地计数器分配。为了避免
 * VirtualTransactionId 在短时间间隔内被重复使用，先后占用同一个
 * backend ID 槽位的进程应使用连续的一段 local ID，其实现方式就是
 * 如上文那样把 nextLocalTransactionId 一并复制过去。
 */
LocalTransactionId
GetNextLocalTransactionId(void)
{
	LocalTransactionId result;

	/* loop to avoid returning InvalidLocalTransactionId at wraparound */
	do
	{
		result = nextLocalTransactionId++;
	} while (!LocalTransactionIdIsValid(result));

	return result;
}
