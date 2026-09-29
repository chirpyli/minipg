/*-------------------------------------------------------------------------
 *
 * dest.h
 *	  通信目的地（communication destinations）支持
 *
 * 每当后端执行一个返回元组的查询时，结果都必须送往某个地方。例如：
 *
 *	  - 只有当我们运行的是独立后端（standalone backend，没有 postmaster），
 *		并且把结果返回给交互式用户时，stdout 才是目的地。
 *
 *	  - 当我们运行的是带前端的后端，并且前端执行了 PQexec() 时，
 *		目的地是一个远程进程。在这种情况下，结果通过 backend/libpq 中的
 *		函数发送给前端。
 *
 *	  - 当系统在内部执行查询时，目的地是 DestNone。结果被丢弃。
 *
 * dest.c 定义了三个实现目的地管理的函数：
 *
 * BeginCommand：在命令开始时初始化目的地。
 * CreateDestReceiver：返回一个指向特定目的地的接收器（receiver）函数结构体的
 * 指针。
 * EndCommand：在命令结束时清理目的地。
 *
 * BeginCommand/EndCommand 每收到一条 SQL 查询就执行一次。
 *
 * CreateDestReceiver 返回一个适合指定目的地的接收器对象。执行器以及能够返回
 * 元组的工具语句（utility statement）会收到由此产生的 DestReceiver* 指针。
 * 每次执行器运行或工具执行都会先调用接收器的 rStartup 方法，然后调用 receiveSlot
 * 方法（零次或多次），最后调用 rShutdown 方法。同一个接收器对象可以被多次重用；
 * 最终通过调用它的 rDestroy 方法销毁。
 *
 * 在某些情况下，接收器对象需要额外的参数，这些参数必须在调用 CreateDestReceiver
 * 之后传递给它们。由于参数集合因接收器类型而异，这一点不由本模块处理，而是由调用
 * 代码直接调用接收器类型特有的函数来完成。
 *
 * CreateDestReceiver 返回的 DestReceiver 对象可以是一个静态分配的对象（用于那些
 * 不需要本地状态的目的地类型），此时 rDestroy 是一个空操作。它也可以是一个
 * palloc 分配的对象，以 DestReceiver 作为其第一个字段，并含有额外字段
 * （示例见 printtup.c）。这些额外字段随后可以通过把传给 DestReceiver 函数的
 * DestReceiver* 指针进行强制转换来访问。该 palloc 分配的对象由 rDestroy 方法
 * 用 pfree 释放。注意，CreateDestReceiver 的调用者应当确保在一个足够长寿的内存
 * 上下文中进行分配，以便接收器对象在仍被需要时不会消失。
 *
 * 特殊安排：None_Receiver 是供 DestNone 目的地使用的、永久可用的接收器对象。
 * 这避免了在 portal 和游标（cursor）操作中进行无用的创建/销毁调用。
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/tcop/dest.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DEST_H
#define DEST_H

#include "executor/tuptable.h"
#include "tcop/cmdtag.h"


/* buffer size to use for command completion tags */
#define COMPLETION_TAG_BUFSIZE	64


/* ----------------
 *		CommandDest 是一种用于标识目标端的简化手段。将来这一天或许
 *		需要对其进行改进。
 *
 * 注意：对于全局变量 whereToSendOutput，只有 DestNone、DestDebug、
 * DestRemote 这几个取值是合法的。其余取值可作为单条命令的目标端使用。
 * ----------------
 */
typedef enum
{
	DestNone,					/* results are discarded */
	DestDebug,					/* results go to debugging output */
	DestRemote,					/* results sent to frontend process */
	DestRemoteSimple,			/* sent to frontend, w/no catalog access */
	DestTuplestore,				/* results sent to Tuplestore */
	DestSQLFunction				/* results sent to SQL-language func mgr */
} CommandDest;

/* ----------------
 *		DestReceiver 是特定目的地局部状态的基类型。
 *		在最简单的情况下，没有状态信息，只有执行器必须调用的函数指针。
 *
 * 注意：传给 receiveSlot 例程的 slot 所包含的 TupleDesc 必须与传给 rStartup
 * 例程的 TupleDesc 完全相同。它返回 bool，其中"true"值表示"继续处理"，而
 * "false"值表示"提前停止，就像我们已经到达扫描末尾一样"。
 * ----------------
 */
typedef struct _DestReceiver DestReceiver;

struct _DestReceiver
{
	/* Called for each tuple to be output: */
	bool		(*receiveSlot) (TupleTableSlot *slot,
								DestReceiver *self);
	/* Per-executor-run initialization and shutdown: */
	void		(*rStartup) (DestReceiver *self,
							 int operation,
							 TupleDesc typeinfo);
	void		(*rShutdown) (DestReceiver *self);
	/* Destroy the receiver object itself (if dynamically allocated) */
	void		(*rDestroy) (DestReceiver *self);
	/* CommandDest code for this receiver */
	CommandDest mydest;
	/* Private fields might appear beyond this point... */
};

extern PGDLLIMPORT DestReceiver *None_Receiver; /* permanent receiver for
												 * DestNone */

/* The primary destination management functions */

extern void BeginCommand(CommandTag commandTag, CommandDest dest);
extern DestReceiver *CreateDestReceiver(CommandDest dest);
extern void EndCommand(const QueryCompletion *qc, CommandDest dest,
					   bool force_undecorated_output);
extern void EndReplicationCommand(const char *commandTag);

/* Additional functions that go with destination management, more or less. */

extern void NullCommand(CommandDest dest);
extern void ReadyForQuery(CommandDest dest);

#endif							/* DEST_H */
