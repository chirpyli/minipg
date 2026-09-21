/*-------------------------------------------------------------------------
 *
 * parallel.c
 *	  Support for parallel execution.
 *
 * minipg 已彻底裁剪并行执行框架（ParallelContext、动态共享内存、共享消息
 * 队列、后台 worker 启动等）。此文件仅保留与"并行模式/并行 worker 环境"
 * 相关的最小符号存根：事务状态机（xact.c）、SSI（predicate.c）、信号处理
 * （procsignal.c）与消息处理（postgres.c）仍会引用这些钩子。
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/parallel.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "miscadmin.h"

/*
 * State for the current parallel worker, if any.  With the framework removed
 * these are always in their default (non-worker) state.
 */
volatile bool ParallelMessagePending = false;
int			ParallelWorkerNumber = -1;
bool		InitializingParallelWorker = false;

/*
 * Is a parallel context currently active?  Never, without the framework.
 */
bool
ParallelContextActive(void)
{
	return false;
}

/*
 * The remaining entry points are retained as no-ops so that the callers that
 * belong to the parallel-mode state machine keep working unchanged.
 */
void
HandleParallelMessageInterrupt(void)
{
}

void
HandleParallelMessages(void)
{
}

void
AtEOXact_Parallel(bool isCommit)
{
}

void
AtEOSubXact_Parallel(bool isCommit, SubTransactionId mySubId)
{
}

void
ParallelWorkerReportLastRecEnd(XLogRecPtr last_xlog_end)
{
}
