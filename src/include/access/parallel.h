/*-------------------------------------------------------------------------
 *
 * parallel.h
 *	  Infrastructure for launching parallel workers
 *
 * 在 minipg 中，并行框架（ParallelContext/DSM/共享消息队列等）已被裁剪。
 * 这里仅保留"并行模式/并行 worker 环境"所需的最小符号：并行模式的状态
 * 机（xact.c/SSI）与若干钩子仍会引用它们。
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/parallel.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARALLEL_H
#define PARALLEL_H

#include "access/xlogdefs.h"

extern volatile bool ParallelMessagePending;
extern PGDLLIMPORT int ParallelWorkerNumber;
extern PGDLLIMPORT bool InitializingParallelWorker;

#define		IsParallelWorker()		(ParallelWorkerNumber >= 0)

/*
 * 并行上下文框架已裁剪，因此永远不会有活动的并行上下文。
 */
extern bool ParallelContextActive(void);

/* 并行模式钩子：框架裁剪后为空操作，仅保留调用点 */
extern void HandleParallelMessageInterrupt(void);
extern void HandleParallelMessages(void);
extern void AtEOXact_Parallel(bool isCommit);
extern void AtEOSubXact_Parallel(bool isCommit, SubTransactionId mySubId);
extern void ParallelWorkerReportLastRecEnd(XLogRecPtr last_xlog_end);

#endif							/* PARALLEL_H */
