/*-------------------------------------------------------------------------
 *
 * latch.h
 *	  进程间 latch（闩锁）相关例程
 *
 * latch 是一个布尔变量，并附带一组操作，使进程可以休眠直到它被置位。
 * latch 可以由另一个进程置位，也可以由同一进程内的信号处理函数置位。
 *
 * latch 接口是对以下常见模式的可靠替代：用 pg_usleep() 或 select() 等待
 * 信号到来，由信号处理函数设置一个标志变量。因为：在某些平台上，到来的
 * 信号不会打断 sleep；即使在会打断 sleep 的平台上，如果信号恰好在进入
 * sleep 之前到达，也存在竞态条件。因此，上述常见模式必须定期醒来并轮询
 * 标志变量。pselect() 系统调用正是为解决此问题而发明的，但它的可移植性
 * 不够好。latch 的设计目的就是克服这些限制：让你无需轮询即可休眠，同时
 * 保证对其他进程发来的信号快速响应。
 *
 * latch 分两种：本地 latch 和共享 latch。本地 latch 由 InitLatch 初始化，
 * 只能由同一进程置位。在信号处理函数中调用 SetLatch，即可用本地 latch
 * 等待信号到来。共享 latch 位于共享内存中，必须在 postmaster 启动时由
 * InitSharedLatch 初始化。共享 latch 在被等待之前，必须先通过 OwnLatch
 * 与某个进程关联。只有拥有该 latch 的进程才能等待它，但任何进程都可以
 * 置位它。
 *
 * latch 有三种基本操作：
 *
 * SetLatch		- 置位 latch
 * ResetLatch	- 清除 latch，使其可以再次被置位
 * WaitLatch	- 等待 latch 被置位
 *
 * WaitLatch 提供了超时机制（应尽量避免使用，因为会带来额外开销），还提供
 * 了让 postmaster 子进程在 postmaster 死亡时立即醒来的机制。导出函数的
 * 详细规格说明见 latch.c。
 *
 * 等待事件的正确模式是：
 *
 * for (;;)
 * {
 *	   ResetLatch();
 *	   if (work to do)
 *		   Do Stuff();
 *	   WaitLatch();
 * }
 *
 * 关键是要在检查"是否有工作要做" *之前* 重置 latch。否则，如果有人在检查
 * 与 ResetLatch 调用之间置位了 latch，你就会漏掉它，Wait 将会错误地阻塞。
 *
 * 另一种有效的编码模式如下：
 *
 * for (;;)
 * {
 *	   if (work to do)
 *		   Do Stuff(); // 特别地，如果某个条件满足就退出循环
 *	   WaitLatch();
 *	   ResetLatch();
 * }
 *
 * 如果预期循环的终止条件经常在第一次迭代就得到满足，这种写法有助于减少
 * latch 的交互流量；代价是条件不满足时会多绕一轮循环才进入阻塞。必须避免
 * 的是：把任何异步事件检查放在 WaitLatch 之后、ResetLatch 之前，因为那会
 * 造成竞态条件。
 *
 * 要唤醒等待者，必须首先设置一个全局标志、或其他会被等待循环在
 * "if (work to do)" 部分检查的东西，然后 *再* 调用 SetLatch。如果 latch
 * 已经被置位，SetLatch 被设计为快速返回。
 *
 * 在某些平台上，信号本身不会打断 latch 等待原语。因此，任何旨在终止
 * WaitLatch 等待的信号处理函数都必须调用 SetLatch，这一点至关重要。
 *
 * 注意：在向辅助进程发信号时，使用进程 latch（PGPROC.procLatch）通常比
 * 临时自建的共享 latch 更好。因为通用信号处理函数只会对进程 latch 调用
 * SetLatch，所以使用进程 latch 之外的任何 latch，实际上就等于无法使用
 * 任何通用处理函数。
 *
 *
 * WaitEventSet 允许同时等待 latch 被置位以及一些附加事件——目前包括
 * postmaster 死亡和多个套接字的就绪状态。在许多平台上，使用长期存在的
 * 事件集比使用 WaitLatch 或 WaitLatchOrSocket 更高效。
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/latch.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LATCH_H
#define LATCH_H

#include <signal.h>

/*
 * Latch 结构体应视为不透明的，只能通过公共函数访问。之所以在这里给出
 * 定义，是为了允许把 Latch 作为成员内嵌到更大的结构体中。
 */
typedef struct Latch
{
	sig_atomic_t is_set;
	sig_atomic_t maybe_sleeping;
	bool		is_shared;
	int			owner_pid;
} Latch;

/*
 * 可能唤醒 WaitLatch()、WaitLatchOrSocket() 或 WaitEventSetWait() 的
 * 事件的位掩码。
 */
#define WL_LATCH_SET		 (1 << 0)
#define WL_SOCKET_READABLE	 (1 << 1)
#define WL_SOCKET_WRITEABLE  (1 << 2)
#define WL_TIMEOUT			 (1 << 3)	/* 不适用于 WaitEventSetWait() */
#define WL_POSTMASTER_DEATH  (1 << 4)
#define WL_EXIT_ON_PM_DEATH	 (1 << 5)
/* 避免在不要求该语义的平台上做特殊处理 */
#define WL_SOCKET_CONNECTED  WL_SOCKET_WRITEABLE

#define WL_SOCKET_MASK		(WL_SOCKET_READABLE | \
							 WL_SOCKET_WRITEABLE | \
							 WL_SOCKET_CONNECTED)

typedef struct WaitEvent
{
	int			pos;			/* 在事件数据结构中的位置 */
	uint32		events;			/* 被触发的事件 */
	pgsocket	fd;				/* 与该事件关联的套接字 fd */
	void	   *user_data;		/* 在 AddWaitEventToSet 中提供的指针 */
} WaitEvent;

/* 前向声明，以免暴露 latch.c 的实现细节 */
typedef struct WaitEventSet WaitEventSet;

/*
 * latch.c 中函数的原型声明
 */
extern void InitializeLatchSupport(void);
extern void InitLatch(Latch *latch);
extern void InitSharedLatch(Latch *latch);
extern void OwnLatch(Latch *latch);
extern void DisownLatch(Latch *latch);
extern void SetLatch(Latch *latch);
extern void ResetLatch(Latch *latch);
extern void ShutdownLatchSupport(void);

extern WaitEventSet *CreateWaitEventSet(MemoryContext context, int nevents);
extern void FreeWaitEventSet(WaitEventSet *set);
extern int	AddWaitEventToSet(WaitEventSet *set, uint32 events, pgsocket fd,
							  Latch *latch, void *user_data);
extern void ModifyWaitEvent(WaitEventSet *set, int pos, uint32 events, Latch *latch);

extern int	WaitEventSetWait(WaitEventSet *set, long timeout,
							 WaitEvent *occurred_events, int nevents,
							 uint32 wait_event_info);
extern int	WaitLatch(Latch *latch, int wakeEvents, long timeout,
					  uint32 wait_event_info);
extern int	WaitLatchOrSocket(Latch *latch, int wakeEvents,
							  pgsocket sock, long timeout, uint32 wait_event_info);
extern void InitializeLatchWaitSet(void);

#endif							/* LATCH_H */
