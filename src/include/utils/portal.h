/*-------------------------------------------------------------------------
 *
 * portal.h
 *	  POSTGRES portal definitions.
 *
 * A portal is an abstraction which represents the execution state of
 * a running or runnable query.  Portals support protocol-level portals.
 *
 * Scrolling (nonsequential access) and suspension of execution are allowed
 * only for portals that contain a single SELECT-type query.  We do not want
 * to let the client suspend an update-type query partway through!	Because
 * the query rewriter does not allow arbitrary ON SELECT rewrite rules,
 * only queries that were originally update-type could produce multiple
 * plan trees; so the restriction to a single query is not a problem
 * in practice.
 *
 * Protocol-level portals have no nonsequential-fetch API and so the
 * distinction doesn't matter for them.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/portal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORTAL_H
#define PORTAL_H

#include "datatype/timestamp.h"
#include "executor/execdesc.h"
#include "tcop/cmdtag.h"
#include "utils/resowner.h"

/*
 * Portal 有多种执行策略，具体取决于要执行的是哪条（或哪些）查询。
 * （注意：在任何情况下，一个 Portal 都只执行单条源 SQL 查询，因此从用户的
 * 视角来看只产生单一结果。不过，规则重写器可能把这条源查询展开为
 * 零条或多条实际查询。）
 *
 * PORTAL_ONE_SELECT：Portal 中包含一条 SELECT 查询。我们按结果被请求
 * 的节奏增量式地运行执行器。
 *
 * PORTAL_ONE_RETURNING：Portal 中包含一条带 RETURNING 子句的
 * INSERT/UPDATE/DELETE 查询（可能还有规则重写附加的辅助查询）。首次执行时，
 * 我们将该 Portal 完整运行，并把主查询的结果转储进 Portal 的 tuplestore；
 * 随后按客户端请求返回这些结果。（我们无法支持在查询执行到一半时挂起，
 * 因为 AFTER 触发器代码无法处理这种情况，同时也是因为我们不想冒险漏执行
 * 任何辅助查询。）
 *
 * PORTAL_ONE_MOD_WITH：Portal 中包含一条 SELECT 查询，但它带有
 * 数据修改型 CTE。由于可能需要触发触发器，目前将其与 PORTAL_ONE_RETURNING
 * 情形同等对待。未来其行为可能会更接近 PORTAL_ONE_SELECT。
 *
 * PORTAL_UTIL_SELECT：Portal 中包含一条返回类 SELECT 结果的实用语句
 * （例如 EXPLAIN 或 SHOW）。首次执行时，我们运行该语句并将其结果转储进
 * Portal 的 tuplestore；随后按客户端请求返回这些结果。
 *
 * PORTAL_MULTI_QUERY：所有其他情形。在此情形下，我们不支持部分执行：
 * Portal 的查询会在首次调用时完整运行至结束。
 */
typedef enum PortalStrategy
{
	PORTAL_ONE_SELECT,
	PORTAL_ONE_RETURNING,
	PORTAL_ONE_MOD_WITH,
	PORTAL_UTIL_SELECT,
	PORTAL_MULTI_QUERY
} PortalStrategy;

/*
 * A portal is always in one of these states.  It is possible to transit
 * from ACTIVE back to READY if the query is not run to completion;
 * otherwise we never back up in status.
 */
typedef enum PortalStatus
{
	PORTAL_NEW,					/* freshly created */
	PORTAL_DEFINED,				/* PortalDefineQuery done */
	PORTAL_READY,				/* PortalStart complete, can run it */
	PORTAL_ACTIVE,				/* portal is running (can't delete it) */
	PORTAL_DONE,				/* portal is finished (don't re-run it) */
	PORTAL_FAILED				/* portal got error (can't re-run it) */
} PortalStatus;

typedef struct PortalData *Portal;

typedef struct PortalData
{
	/* Bookkeeping data */
	const char *name;			/* portal's name */
	const char *prepStmtName;	/* source prepared statement (NULL if none) */
	MemoryContext portalContext;	/* subsidiary memory for portal */
	ResourceOwner resowner;		/* resources owned by portal */
	void		(*cleanup) (Portal portal); /* cleanup hook */

	/*
	 * State data for remembering which subtransaction(s) the portal was
	 * created or used in: createSubid is the creating subxact and
	 * activeSubid is the last subxact in which we ran the portal.
	 */
	SubTransactionId createSubid;	/* the creating subxact */
	SubTransactionId activeSubid;	/* the last subxact with activity */

	/* The query or queries the portal will execute */
	const char *sourceText;		/* text of query (as of 8.4, never NULL) */
	CommandTag	commandTag;		/* command tag for original query */
	QueryCompletion qc;			/* command completion data for executed query */
	List	   *stmts;			/* list of PlannedStmts */

	ParamListInfo portalParams; /* params to pass to query */
	QueryEnvironment *queryEnv; /* environment for query */

	/* Features/options */
	PortalStrategy strategy;	/* see above */

	/* Status data */
	PortalStatus status;		/* see above */

	/* If not NULL, Executor is active; call ExecutorEnd eventually: */
	QueryDesc  *queryDesc;		/* info needed for executor invocation */

	/* If portal returns tuples, this is their tupdesc: */
	TupleDesc	tupDesc;		/* descriptor for result tuples */
	/* and these are the format codes to use for the columns: */
	int16	   *formats;		/* a format code for each column */

	/*
	 * Outermost ActiveSnapshot for execution of the portal's queries.  For
	 * all but a few utility commands, we require such a snapshot to exist.
	 * This ensures that TOAST references in query results can be detoasted,
	 * and helps to reduce thrashing of the process's exposed xmin.
	 */
	Snapshot	portalSnapshot; /* active snapshot, or NULL if none */

	/*
	 * Where we store tuples for a PORTAL_ONE_RETURNING,
	 * PORTAL_ONE_MOD_WITH, or PORTAL_UTIL_SELECT query.
	 */
	Tuplestorestate *holdStore; /* store for stashed query results */
	MemoryContext holdContext;	/* memory containing holdStore */

	/*
	 * Snapshot under which tuples in the holdStore were read.  We must keep a
	 * reference to this snapshot if there is any possibility that the tuples
	 * contain TOAST references, because releasing the snapshot could allow
	 * recently-dead rows to be vacuumed away, along with any toast data
	 * belonging to them.
	 */
	Snapshot	holdSnapshot;	/* registered snapshot, or NULL if none */

	/*
 * atStart, atEnd and portalPos indicate the current portal position.
 * portalPos is zero before the first row, N after fetching N'th row of
 * query.  After we run off the end, portalPos = # of rows in query, and
 * atEnd is true.  Note that atStart implies portalPos == 0, but not the
 * reverse: we might have backed up only as far as the first row, not to
 * the start.  Also note that various code inspects atStart and atEnd, but
 * only the portal run routines should touch portalPos.
 */
	bool		atStart;
	bool		atEnd;
	uint64		portalPos;

	/* Stuff added at the end to avoid ABI break in stable branches: */
	int			createLevel;	/* creating subxact's nesting level */
}			PortalData;

/*
 * PortalIsValid
 *		True iff portal is valid.
 */
#define PortalIsValid(p) PointerIsValid(p)


/* Prototypes for functions in utils/mmgr/portalmem.c */
extern void EnablePortalManager(void);
extern bool PreCommit_Portals(bool isPrepare);
extern void AtAbort_Portals(void);
extern void AtCleanup_Portals(void);
extern void AtSubCommit_Portals(SubTransactionId mySubid,
								SubTransactionId parentSubid,
								int parentLevel,
								ResourceOwner parentXactOwner);
extern void AtSubAbort_Portals(SubTransactionId mySubid,
							   SubTransactionId parentSubid,
							   ResourceOwner myXactOwner,
							   ResourceOwner parentXactOwner);
extern void AtSubCleanup_Portals(SubTransactionId mySubid);
extern Portal CreatePortal(const char *name, bool allowDup, bool dupSilent);
extern Portal CreateNewPortal(void);
extern void MarkPortalActive(Portal portal);
extern void MarkPortalDone(Portal portal);
extern void MarkPortalFailed(Portal portal);
extern void PortalDrop(Portal portal, bool isTopCommit);
extern Portal GetPortalByName(const char *name);
extern void PortalDefineQuery(Portal portal,
							  const char *prepStmtName,
							  const char *sourceText,
							  CommandTag commandTag,
							  List *stmts);
extern PlannedStmt *PortalGetPrimaryStmt(Portal portal);
extern void PortalCreateHoldStore(Portal portal);
extern void PortalHashTableDeleteAll(void);
extern void ForgetPortalSnapshots(void);

#endif							/* PORTAL_H */
