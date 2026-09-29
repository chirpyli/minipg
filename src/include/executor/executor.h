/*-------------------------------------------------------------------------
 *
 * executor.h
 *	  support for the POSTGRES executor module
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/executor.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "executor/execdesc.h"
#include "fmgr.h"
#include "nodes/lockoptions.h"
#include "nodes/parsenodes.h"
#include "utils/memutils.h"


/*
 * 传给 ExecutorStart 以及各 ExecInitNode 例程的 "eflags" 参数，是下列标志位
 * 的按位或，它们告诉被调用的计划节点应该预期什么。注意这些标志在沿计划树
 * 向下传递的过程中会被修改，因为上层节点可能要求其子节点具备整个计划并未
 * 要求的功能（例如：MergeJoin 要求其内侧输入具备 mark/restore 能力），
 * 或者上层节点可能替其输入屏蔽掉某些功能要求
 *（例如：Materialize 使其输入无需支持反向扫描）。
 *
 * EXPLAIN_ONLY 表示计划树被初始化只是为了便于 EXPLAIN 打印输出，而不会
 * 真正被执行。因此，启动阶段不应产生任何副作用。不过，错误检查
 *（例如权限检查）仍然应当执行。
 *
 * REWIND 表示计划节点应尽量高效地支持不改变参数的重扫描（rescan）。
 *（节点在任何情况下都必须支持 ExecReScan 调用；但如果没有给出这个标志，
 * 它们可以通过完全重新计算来实现。注意，参数变化在任何情况下都会强制
 * 完全重新计算。）
 *
 * BACKWARD 表示计划节点必须遵循 es_direction 标志。
 * 如果不传这个标志，计划节点只会向前运行。
 *
 * MARK 表示计划节点必须支持 Mark/Restore 调用。
 * 如果不传这个标志，就不会发生任何 Mark/Restore。
 */
#define EXEC_FLAG_EXPLAIN_ONLY	0x0001	/* EXPLAIN, no ANALYZE */
#define EXEC_FLAG_REWIND		0x0002	/* need efficient rescan */
#define EXEC_FLAG_BACKWARD		0x0004	/* need backward scan */
#define EXEC_FLAG_MARK			0x0008	/* need mark/restore */
#define EXEC_FLAG_WITH_NO_DATA	0x0020	/* rel scannability doesn't matter */


/*
 * prototypes from functions in execAmi.c
 */
struct Path;					/* avoid including pathnodes.h here */

extern void ExecReScan(PlanState *node);
extern void ExecMarkPos(PlanState *node);
extern void ExecRestrPos(PlanState *node);
extern bool ExecSupportsMarkRestore(struct Path *pathnode);
extern bool ExecSupportsBackwardScan(Plan *node);
extern bool ExecMaterializesOutput(NodeTag plantype);

/*
 * prototypes from functions in execGrouping.c
 */
extern ExprState *execTuplesMatchPrepare(TupleDesc desc,
										 int numCols,
										 const AttrNumber *keyColIdx,
										 const Oid *eqOperators,
										 const Oid *collations,
										 PlanState *parent);
extern void execTuplesHashPrepare(int numCols,
								  const Oid *eqOperators,
								  Oid **eqFuncOids,
								  FmgrInfo **hashFunctions);
extern TupleHashTable BuildTupleHashTableExt(PlanState *parent,
											 TupleDesc inputDesc,
											 int numCols, AttrNumber *keyColIdx,
											 const Oid *eqfuncoids,
											 FmgrInfo *hashfunctions,
											 Oid *collations,
											 long nbuckets, Size additionalsize,
											 MemoryContext metacxt,
											 MemoryContext tablecxt,
											 MemoryContext tempcxt);
extern TupleHashEntry LookupTupleHashEntry(TupleHashTable hashtable,
										   TupleTableSlot *slot,
										   bool *isnew, uint32 *hash);
extern TupleHashEntry FindTupleHashEntry(TupleHashTable hashtable,
										 TupleTableSlot *slot,
										 ExprState *eqcomp,
										 FmgrInfo *hashfunctions);
extern void ResetTupleHashTable(TupleHashTable hashtable);

/*
 * prototypes from functions in execJunk.c
 */
extern JunkFilter *ExecInitJunkFilter(List *targetList,
									  TupleTableSlot *slot);
extern JunkFilter *ExecInitJunkFilterConversion(List *targetList,
												TupleDesc cleanTupType,
												TupleTableSlot *slot);
extern AttrNumber ExecFindJunkAttribute(JunkFilter *junkfilter,
										const char *attrName);
extern AttrNumber ExecFindJunkAttributeInTlist(List *targetlist,
											   const char *attrName);
extern TupleTableSlot *ExecFilterJunk(JunkFilter *junkfilter,
									  TupleTableSlot *slot);

/*
 * ExecGetJunkAttribute
 *
 * Given a junk filter's input tuple (slot) and a junk attribute's number
 * previously found by ExecFindJunkAttribute, extract & return the value and
 * isNull flag of the attribute.
 */
#ifndef FRONTEND
static inline Datum
ExecGetJunkAttribute(TupleTableSlot *slot, AttrNumber attno, bool *isNull)
{
	Assert(attno > 0);
	return slot_getattr(slot, attno, isNull);
}
#endif

/*
 * prototypes from functions in execMain.c
 */
extern void ExecutorStart(QueryDesc *queryDesc, int eflags);
extern void standard_ExecutorStart(QueryDesc *queryDesc, int eflags);
extern void ExecutorRun(QueryDesc *queryDesc,
						ScanDirection direction, uint64 count, bool execute_once);
extern void standard_ExecutorRun(QueryDesc *queryDesc,
								 ScanDirection direction, uint64 count, bool execute_once);
extern void ExecutorFinish(QueryDesc *queryDesc);
extern void standard_ExecutorFinish(QueryDesc *queryDesc);
extern void ExecutorEnd(QueryDesc *queryDesc);
extern void standard_ExecutorEnd(QueryDesc *queryDesc);
extern void ExecutorRewind(QueryDesc *queryDesc);
extern void CheckValidResultRelNew(ResultRelInfo *resultRelInfo, CmdType operation);
extern void CheckValidResultRel(ResultRelInfo *resultRelInfo, CmdType operation);
extern void InitResultRelInfo(ResultRelInfo *resultRelInfo,
							  Relation resultRelationDesc,
							  Index resultRelationIndex);
extern LockTupleMode ExecUpdateLockMode(EState *estate, ResultRelInfo *relinfo);
extern ExecRowMark *ExecFindRowMark(EState *estate, Index rti);
extern ExecAuxRowMark *ExecBuildAuxRowMark(ExecRowMark *erm, List *targetlist);
extern TupleTableSlot *EvalPlanQual(EPQState *epqstate, Relation relation,
									Index rti, TupleTableSlot *testslot);
extern void EvalPlanQualInit(EPQState *epqstate, EState *parentestate,
							 Plan *subplan, List *auxrowmarks, int epqParam);
extern void EvalPlanQualInitExt(EPQState *epqstate, EState *parentestate,
								Plan *subplan, List *auxrowmarks,
								int epqParam, List *resultRelations);
extern void EvalPlanQualSetPlan(EPQState *epqstate,
								Plan *subplan, List *auxrowmarks);
extern TupleTableSlot *EvalPlanQualSlot(EPQState *epqstate,
										Relation relation, Index rti);

#define EvalPlanQualSetSlot(epqstate, slot)  ((epqstate)->origslot = (slot))
extern bool EvalPlanQualFetchRowMark(EPQState *epqstate, Index rti, TupleTableSlot *slot);
extern TupleTableSlot *EvalPlanQualNext(EPQState *epqstate);
extern void EvalPlanQualBegin(EPQState *epqstate);
extern void EvalPlanQualEnd(EPQState *epqstate);

/*
 * functions in execProcnode.c
 */
extern PlanState *ExecInitNode(Plan *node, EState *estate, int eflags);
extern void ExecSetExecProcNode(PlanState *node, ExecProcNodeMtd function);
extern Node *MultiExecProcNode(PlanState *node);
extern void ExecEndNode(PlanState *node);



/* ----------------------------------------------------------------
 *		ExecProcNode
 *
 *		Execute the given node to return a(nother) tuple.
 * ----------------------------------------------------------------
 */
#ifndef FRONTEND
static inline TupleTableSlot *
ExecProcNode(PlanState *node)
{
	if (node->chgParam != NULL) /* something changed? */
		ExecReScan(node);		/* let ReScan handle this */

	return node->ExecProcNode(node);
}
#endif

/*
 * prototypes from functions in execExpr.c
 */
extern ExprState *ExecInitExpr(Expr *node, PlanState *parent);
extern ExprState *ExecInitExprWithParams(Expr *node, ParamListInfo ext_params);
extern ExprState *ExecInitQual(List *qual, PlanState *parent);
extern ExprState *ExecInitCheck(List *qual, PlanState *parent);
extern List *ExecInitExprList(List *nodes, PlanState *parent);
extern ExprState *ExecBuildGroupingEqual(TupleDesc ldesc, TupleDesc rdesc,
										 const TupleTableSlotOps *lops, const TupleTableSlotOps *rops,
										 int numCols,
										 const AttrNumber *keyColIdx,
										 const Oid *eqfunctions,
										 const Oid *collations,
										 PlanState *parent);
extern ExprState *ExecBuildParamSetEqual(TupleDesc desc,
										 const TupleTableSlotOps *lops,
										 const TupleTableSlotOps *rops,
										 const Oid *eqfunctions,
										 const Oid *collations,
										 const List *param_exprs,
										 PlanState *parent);
extern ProjectionInfo *ExecBuildProjectionInfo(List *targetList,
											   ExprContext *econtext,
											   TupleTableSlot *slot,
											   PlanState *parent,
											   TupleDesc inputDesc);
extern ProjectionInfo *ExecBuildUpdateProjection(List *targetList,
												 bool evalTargetList,
												 List *targetColnos,
												 TupleDesc relDesc,
												 ExprContext *econtext,
												 TupleTableSlot *slot,
												 PlanState *parent);
extern ExprState *ExecPrepareExpr(Expr *node, EState *estate);
extern ExprState *ExecPrepareQual(List *qual, EState *estate);
extern ExprState *ExecPrepareCheck(List *qual, EState *estate);
extern List *ExecPrepareExprList(List *nodes, EState *estate);

/*
 * ExecEvalExpr
 *
 * Evaluate expression identified by "state" in the execution context
 * given by "econtext".  *isNull is set to the is-null flag for the result,
 * and the Datum value is the function result.
 *
 * The caller should already have switched into the temporary memory
 * context econtext->ecxt_per_tuple_memory.  The convenience entry point
 * ExecEvalExprSwitchContext() is provided for callers who don't prefer to
 * do the switch in an outer loop.
 */
#ifndef FRONTEND
static inline Datum
ExecEvalExpr(ExprState *state,
			 ExprContext *econtext,
			 bool *isNull)
{
	return state->evalfunc(state, econtext, isNull);
}
#endif

/*
 * ExecEvalExprSwitchContext
 *
 * Same as ExecEvalExpr, but get into the right allocation context explicitly.
 */
#ifndef FRONTEND
static inline Datum
ExecEvalExprSwitchContext(ExprState *state,
						  ExprContext *econtext,
						  bool *isNull)
{
	Datum		retDatum;
	MemoryContext oldContext;

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	retDatum = state->evalfunc(state, econtext, isNull);
	MemoryContextSwitchTo(oldContext);
	return retDatum;
}
#endif

/*
 * ExecProject
 *
 * Projects a tuple based on projection info and stores it in the slot passed
 * to ExecBuildProjectionInfo().
 *
 * Note: the result is always a virtual tuple; therefore it may reference
 * the contents of the exprContext's scan tuples and/or temporary results
 * constructed in the exprContext.  If the caller wishes the result to be
 * valid longer than that data will be valid, he must call ExecMaterializeSlot
 * on the result slot.
 */
#ifndef FRONTEND
static inline TupleTableSlot *
ExecProject(ProjectionInfo *projInfo)
{
	ExprContext *econtext = projInfo->pi_exprContext;
	ExprState  *state = &projInfo->pi_state;
	TupleTableSlot *slot = state->resultslot;
	bool		isnull;

	/*
	 * Clear any former contents of the result slot.  This makes it safe for
	 * us to use the slot's Datum/isnull arrays as workspace.
	 */
	ExecClearTuple(slot);

	/* Run the expression, discarding scalar result from the last column. */
	(void) ExecEvalExprSwitchContext(state, econtext, &isnull);

	/*
	 * Successfully formed a result row.  Mark the result slot as containing a
	 * valid virtual tuple (inlined version of ExecStoreVirtualTuple()).
	 */
	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	slot->tts_nvalid = slot->tts_tupleDescriptor->natts;

	return slot;
}
#endif

/*
 * ExecQual - evaluate a qual prepared with ExecInitQual (possibly via
 * ExecPrepareQual).  Returns true if qual is satisfied, else false.
 *
 * Note: ExecQual used to have a third argument "resultForNull".  The
 * behavior of this function now corresponds to resultForNull == false.
 * If you want the resultForNull == true behavior, see ExecCheck.
 */
#ifndef FRONTEND
static inline bool
ExecQual(ExprState *state, ExprContext *econtext)
{
	Datum		ret;
	bool		isnull;

	/* short-circuit (here and in ExecInitQual) for empty restriction list */
	if (state == NULL)
		return true;

	/* verify that expression was compiled using ExecInitQual */
	Assert(state->flags & EEO_FLAG_IS_QUAL);

	ret = ExecEvalExprSwitchContext(state, econtext, &isnull);

	/* EEOP_QUAL should never return NULL */
	Assert(!isnull);

	return DatumGetBool(ret);
}
#endif

/*
 * ExecQualAndReset() - evaluate qual with ExecQual() and reset expression
 * context.
 */
#ifndef FRONTEND
static inline bool
ExecQualAndReset(ExprState *state, ExprContext *econtext)
{
	bool		ret = ExecQual(state, econtext);

	/* inline ResetExprContext, to avoid ordering issue in this file */
	MemoryContextReset(econtext->ecxt_per_tuple_memory);
	return ret;
}
#endif

extern bool ExecCheck(ExprState *state, ExprContext *context);

/*
 * prototypes from functions in execSRF.c
 */
extern SetExprState *ExecInitFunctionResultSet(Expr *expr,
											   ExprContext *econtext, PlanState *parent);
extern Datum ExecMakeFunctionResultSet(SetExprState *fcache,
									   ExprContext *econtext,
									   MemoryContext argContext,
									   bool *isNull,
									   ExprDoneCond *isDone);

/*
 * prototypes from functions in execScan.c
 */
typedef TupleTableSlot *(*ExecScanAccessMtd) (ScanState *node);
typedef bool (*ExecScanRecheckMtd) (ScanState *node, TupleTableSlot *slot);

extern TupleTableSlot *ExecScan(ScanState *node, ExecScanAccessMtd accessMtd,
								ExecScanRecheckMtd recheckMtd);
extern void ExecAssignScanProjectionInfo(ScanState *node);
extern void ExecAssignScanProjectionInfoWithVarno(ScanState *node, Index varno);
extern void ExecScanReScan(ScanState *node);

/*
 * prototypes from functions in execTuples.c
 */
extern void ExecInitResultTypeTL(PlanState *planstate);
extern void ExecInitResultSlot(PlanState *planstate,
							   const TupleTableSlotOps *tts_ops);
extern void ExecInitResultTupleSlotTL(PlanState *planstate,
									  const TupleTableSlotOps *tts_ops);
extern void ExecInitScanTupleSlot(EState *estate, ScanState *scanstate,
								  TupleDesc tupleDesc,
								  const TupleTableSlotOps *tts_ops);
extern TupleTableSlot *ExecInitExtraTupleSlot(EState *estate,
											  TupleDesc tupledesc,
											  const TupleTableSlotOps *tts_ops);
extern TupleTableSlot *ExecInitNullTupleSlot(EState *estate, TupleDesc tupType,
											 const TupleTableSlotOps *tts_ops);
extern TupleDesc ExecTypeFromTL(List *targetList);
extern TupleDesc ExecCleanTypeFromTL(List *targetList);
extern TupleDesc ExecTypeFromExprList(List *exprList);
extern void ExecTypeSetColNames(TupleDesc typeInfo, List *namesList);
extern void UpdateChangedParamSet(PlanState *node, Bitmapset *newchg);

typedef struct TupOutputState
{
	TupleTableSlot *slot;
	DestReceiver *dest;
} TupOutputState;

extern TupOutputState *begin_tup_output_tupdesc(DestReceiver *dest,
												TupleDesc tupdesc,
												const TupleTableSlotOps *tts_ops);
extern void do_tup_output(TupOutputState *tstate, Datum *values, bool *isnull);
extern void do_text_output_multiline(TupOutputState *tstate, const char *txt);
extern void end_tup_output(TupOutputState *tstate);

/*
 * Write a single line of text given as a C string.
 *
 * Should only be used with a single-TEXT-attribute tupdesc.
 */
#define do_text_output_oneline(tstate, str_to_emit) \
	do { \
		Datum	values_[1]; \
		bool	isnull_[1]; \
		values_[0] = PointerGetDatum(cstring_to_text(str_to_emit)); \
		isnull_[0] = false; \
		do_tup_output(tstate, values_, isnull_); \
		pfree(DatumGetPointer(values_[0])); \
	} while (0)


/*
 * prototypes from functions in execUtils.c
 */
extern EState *CreateExecutorState(void);
extern void FreeExecutorState(EState *estate);
extern ExprContext *CreateExprContext(EState *estate);
extern ExprContext *CreateWorkExprContext(EState *estate);
extern ExprContext *CreateStandaloneExprContext(void);
extern void FreeExprContext(ExprContext *econtext, bool isCommit);
extern void ReScanExprContext(ExprContext *econtext);

#define ResetExprContext(econtext) \
	MemoryContextReset((econtext)->ecxt_per_tuple_memory)

extern ExprContext *MakePerTupleExprContext(EState *estate);

/* Get an EState's per-output-tuple exprcontext, making it if first use */
#define GetPerTupleExprContext(estate) \
	((estate)->es_per_tuple_exprcontext ? \
	 (estate)->es_per_tuple_exprcontext : \
	 MakePerTupleExprContext(estate))

#define GetPerTupleMemoryContext(estate) \
	(GetPerTupleExprContext(estate)->ecxt_per_tuple_memory)

/* Reset an EState's per-output-tuple exprcontext, if one's been created */
#define ResetPerTupleExprContext(estate) \
	do { \
		if ((estate)->es_per_tuple_exprcontext) \
			ResetExprContext((estate)->es_per_tuple_exprcontext); \
	} while (0)

extern void ExecAssignExprContext(EState *estate, PlanState *planstate);
extern TupleDesc ExecGetResultType(PlanState *planstate);
extern const TupleTableSlotOps *ExecGetResultSlotOps(PlanState *planstate,
													 bool *isfixed);
extern void ExecAssignProjectionInfo(PlanState *planstate,
									 TupleDesc inputDesc);
extern void ExecConditionalAssignProjectionInfo(PlanState *planstate,
												TupleDesc inputDesc, Index varno);
extern void ExecFreeExprContext(PlanState *planstate);
extern void ExecAssignScanType(ScanState *scanstate, TupleDesc tupDesc);
extern void ExecCreateScanSlotFromOuterPlan(EState *estate,
											ScanState *scanstate,
											const TupleTableSlotOps *tts_ops);

extern Relation ExecOpenScanRelation(EState *estate, Index scanrelid, int eflags);

extern void ExecInitRangeTable(EState *estate, List *rangeTable);
extern void ExecCloseRangeTableRelations(EState *estate);
extern void ExecCloseResultRelations(EState *estate);

static inline RangeTblEntry *
exec_rt_fetch(Index rti, EState *estate)
{
	return (RangeTblEntry *) list_nth(estate->es_range_table, rti - 1);
}

extern Relation ExecGetRangeTableRelation(EState *estate, Index rti);
extern void ExecInitResultRelation(EState *estate, ResultRelInfo *resultRelInfo,
								   Index rti);

extern int	executor_errposition(EState *estate, int location);

extern void RegisterExprContextCallback(ExprContext *econtext,
										ExprContextCallbackFunction function,
										Datum arg);
extern void UnregisterExprContextCallback(ExprContext *econtext,
										  ExprContextCallbackFunction function,
										  Datum arg);

extern Datum GetAttributeByName(HeapTupleHeader tuple, const char *attname,
								bool *isNull);
extern Datum GetAttributeByNum(HeapTupleHeader tuple, AttrNumber attrno,
							   bool *isNull);

extern int	ExecTargetListLength(List *targetlist);
extern int	ExecCleanTargetListLength(List *targetlist);

extern Bitmapset *ExecGetUpdatedCols(ResultRelInfo *relinfo, EState *estate);
extern Bitmapset *ExecGetAllUpdatedCols(ResultRelInfo *relinfo, EState *estate);

/*
 * prototypes from functions in execIndexing.c
 */
extern void ExecOpenIndices(ResultRelInfo *resultRelInfo);
extern void ExecCloseIndices(ResultRelInfo *resultRelInfo);
extern List *ExecInsertIndexTuples(ResultRelInfo *resultRelInfo,
								   TupleTableSlot *slot, EState *estate,
								   bool update);

/*
 * prototypes from functions in nodeModifyTable.c
 */
extern TupleTableSlot *ExecGetUpdateNewTuple(ResultRelInfo *relinfo,
											 TupleTableSlot *planSlot,
											 TupleTableSlot *oldSlot);
extern ResultRelInfo *ExecLookupResultRelByOid(ModifyTableState *node,
											   Oid resultoid,
											   bool missing_ok,
											   bool update_cache);

#endif							/* EXECUTOR_H  */
