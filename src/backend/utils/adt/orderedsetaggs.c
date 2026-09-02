/*-------------------------------------------------------------------------
 *
 * orderedsetaggs.c
 *		Ordered-set aggregate functions.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/orderedsetaggs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"


/*
 * Generic support for ordered-set aggregates
 *
 * The state for an ordered-set aggregate is divided into a per-group struct
 * (which is the internal-type transition state datum returned to nodeAgg.c)
 * and a per-query struct, which contains data and sub-objects that we can
 * create just once per query because they will not change across groups.
 * The per-query struct and subsidiary data live in the executor's per-query
 * memory context, and go away implicitly at ExecutorEnd().
 *
 * These structs are set up during the first call of the transition function.
 * Because we allow nodeAgg.c to merge ordered-set aggregates (but not
 * hypothetical aggregates) with identical inputs and transition functions,
 * this info must not depend on the particular aggregate (ie, particular
 * final-function), nor on the direct argument(s) of the aggregate.
 */

typedef struct OSAPerQueryState
{
	/* Representative Aggref for this aggregate: */
	Aggref	   *aggref;
	/* Memory context containing this struct and other per-query data: */
	MemoryContext qcontext;
	/* Context for expression evaluation */
	ExprContext *econtext;
	/* Do we expect multiple final-function calls within one group? */
	bool		rescan_needed;

	/* These fields are used only when accumulating tuples: */

	/* Tuple descriptor for tuples inserted into sortstate: */
	TupleDesc	tupdesc;
	/* Tuple slot we can use for inserting/extracting tuples: */
	TupleTableSlot *tupslot;
	/* Per-sort-column sorting information */
	int			numSortCols;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *eqOperators;
	Oid		   *sortCollations;
	bool	   *sortNullsFirsts;
	/* Equality operator call info, created only if needed: */
	ExprState  *compareTuple;

	/* These fields are used only when accumulating datums: */

	/* Info about datatype of datums being sorted: */
	Oid			sortColType;
	int16		typLen;
	bool		typByVal;
	char		typAlign;
	/* Info about sort ordering: */
	Oid			sortOperator;
	Oid			eqOperator;
	Oid			sortCollation;
	bool		sortNullsFirst;
	/* Equality operator call info, created only if needed: */
	FmgrInfo	equalfn;
} OSAPerQueryState;

typedef struct OSAPerGroupState
{
	/* Link to the per-query state for this aggregate: */
	OSAPerQueryState *qstate;
	/* Memory context containing per-group data: */
	MemoryContext gcontext;
	/* Sort object we're accumulating data in: */
	Tuplesortstate *sortstate;
	/* Number of normal rows inserted into sortstate: */
	int64		number_of_rows;
	/* Have we already done tuplesort_performsort? */
	bool		sort_done;
} OSAPerGroupState;

static void ordered_set_shutdown(Datum arg);


/*
 * Set up working state for an ordered-set aggregate
 */
static OSAPerGroupState *
ordered_set_startup(FunctionCallInfo fcinfo, bool use_tuples)
{
	OSAPerGroupState *osastate;
	OSAPerQueryState *qstate;
	MemoryContext gcontext;
	MemoryContext oldcontext;

	/*
	 * Check we're called as aggregate (and not a window function), and get
	 * the Agg node's group-lifespan context (which might change from group to
	 * group, so we shouldn't cache it in the per-query state).
	 */
	if (AggCheckCallContext(fcinfo, &gcontext) != AGG_CONTEXT_AGGREGATE)
		elog(ERROR, "ordered-set aggregate called in non-aggregate context");

	/*
	 * We keep a link to the per-query state in fn_extra; if it's not there,
	 * create it, and do the per-query setup we need.
	 */
	qstate = (OSAPerQueryState *) fcinfo->flinfo->fn_extra;
	if (qstate == NULL)
	{
		Aggref	   *aggref;
		MemoryContext qcontext;
		List	   *sortlist;
		int			numSortCols;

		/* Get the Aggref so we can examine aggregate's arguments */
		aggref = AggGetAggref(fcinfo);
		if (!aggref)
			elog(ERROR, "ordered-set aggregate called in non-aggregate context");
		if (!AGGKIND_IS_ORDERED_SET(aggref->aggkind))
			elog(ERROR, "ordered-set aggregate support function called for non-ordered-set aggregate");

		/*
		 * Prepare per-query structures in the fn_mcxt, which we assume is the
		 * executor's per-query context; in any case it's the right place to
		 * keep anything found via fn_extra.
		 */
		qcontext = fcinfo->flinfo->fn_mcxt;
		oldcontext = MemoryContextSwitchTo(qcontext);

		qstate = (OSAPerQueryState *) palloc0(sizeof(OSAPerQueryState));
		qstate->aggref = aggref;
		qstate->qcontext = qcontext;

		/* We need to support rescans if the trans state is shared */
		qstate->rescan_needed = AggStateIsShared(fcinfo);

		/* Extract the sort information */
		sortlist = aggref->aggorder;
		numSortCols = list_length(sortlist);

		if (use_tuples)
		{
			bool		ishypothetical = (aggref->aggkind == AGGKIND_HYPOTHETICAL);
			ListCell   *lc;
			int			i;

			if (ishypothetical)
				numSortCols++;	/* make space for flag column */
			qstate->numSortCols = numSortCols;
			qstate->sortColIdx = (AttrNumber *) palloc(numSortCols * sizeof(AttrNumber));
			qstate->sortOperators = (Oid *) palloc(numSortCols * sizeof(Oid));
			qstate->eqOperators = (Oid *) palloc(numSortCols * sizeof(Oid));
			qstate->sortCollations = (Oid *) palloc(numSortCols * sizeof(Oid));
			qstate->sortNullsFirsts = (bool *) palloc(numSortCols * sizeof(bool));

			i = 0;
			foreach(lc, sortlist)
			{
				SortGroupClause *sortcl = (SortGroupClause *) lfirst(lc);
				TargetEntry *tle = get_sortgroupclause_tle(sortcl,
														   aggref->args);

				/* the parser should have made sure of this */
				Assert(OidIsValid(sortcl->sortop));

				qstate->sortColIdx[i] = tle->resno;
				qstate->sortOperators[i] = sortcl->sortop;
				qstate->eqOperators[i] = sortcl->eqop;
				qstate->sortCollations[i] = exprCollation((Node *) tle->expr);
				qstate->sortNullsFirsts[i] = sortcl->nulls_first;
				i++;
			}

			if (ishypothetical)
			{
				/* Add an integer flag column as the last sort column */
				qstate->sortColIdx[i] = list_length(aggref->args) + 1;
				qstate->sortOperators[i] = Int4LessOperator;
				qstate->eqOperators[i] = Int4EqualOperator;
				qstate->sortCollations[i] = InvalidOid;
				qstate->sortNullsFirsts[i] = false;
				i++;
			}

			Assert(i == numSortCols);

			/*
			 * Get a tupledesc corresponding to the aggregated inputs
			 * (including sort expressions) of the agg.
			 */
			qstate->tupdesc = ExecTypeFromTL(aggref->args);

			/* If we need a flag column, hack the tupledesc to include that */
			if (ishypothetical)
			{
				TupleDesc	newdesc;
				int			natts = qstate->tupdesc->natts;

				newdesc = CreateTemplateTupleDesc(natts + 1);
				for (i = 1; i <= natts; i++)
					TupleDescCopyEntry(newdesc, i, qstate->tupdesc, i);

				TupleDescInitEntry(newdesc,
								   (AttrNumber) ++natts,
								   "flag",
								   INT4OID,
								   -1,
								   0);

				FreeTupleDesc(qstate->tupdesc);
				qstate->tupdesc = newdesc;
			}

			/* Create slot we'll use to store/retrieve rows */
			qstate->tupslot = MakeSingleTupleTableSlot(qstate->tupdesc,
													   &TTSOpsMinimalTuple);
		}
		else
		{
			/* Sort single datums */
			SortGroupClause *sortcl;
			TargetEntry *tle;

			if (numSortCols != 1 || aggref->aggkind == AGGKIND_HYPOTHETICAL)
				elog(ERROR, "ordered-set aggregate support function does not support multiple aggregated columns");

			sortcl = (SortGroupClause *) linitial(sortlist);
			tle = get_sortgroupclause_tle(sortcl, aggref->args);

			/* the parser should have made sure of this */
			Assert(OidIsValid(sortcl->sortop));

			/* Save sort ordering info */
			qstate->sortColType = exprType((Node *) tle->expr);
			qstate->sortOperator = sortcl->sortop;
			qstate->eqOperator = sortcl->eqop;
			qstate->sortCollation = exprCollation((Node *) tle->expr);
			qstate->sortNullsFirst = sortcl->nulls_first;

			/* Save datatype info */
			get_typlenbyvalalign(qstate->sortColType,
								 &qstate->typLen,
								 &qstate->typByVal,
								 &qstate->typAlign);
		}

		fcinfo->flinfo->fn_extra = (void *) qstate;

		MemoryContextSwitchTo(oldcontext);
	}

	/* Now build the stuff we need in group-lifespan context */
	oldcontext = MemoryContextSwitchTo(gcontext);

	osastate = (OSAPerGroupState *) palloc(sizeof(OSAPerGroupState));
	osastate->qstate = qstate;
	osastate->gcontext = gcontext;

	/*
	 * Initialize tuplesort object.
	 */
	if (use_tuples)
		osastate->sortstate = tuplesort_begin_heap(qstate->tupdesc,
												   qstate->numSortCols,
												   qstate->sortColIdx,
												   qstate->sortOperators,
												   qstate->sortCollations,
												   qstate->sortNullsFirsts,
												   work_mem,
												   NULL,
												   qstate->rescan_needed);
	else
		osastate->sortstate = tuplesort_begin_datum(qstate->sortColType,
													qstate->sortOperator,
													qstate->sortCollation,
													qstate->sortNullsFirst,
													work_mem,
													NULL,
													qstate->rescan_needed);

	osastate->number_of_rows = 0;
	osastate->sort_done = false;

	/* Now register a shutdown callback to clean things up at end of group */
	AggRegisterCallback(fcinfo,
						ordered_set_shutdown,
						PointerGetDatum(osastate));

	MemoryContextSwitchTo(oldcontext);

	return osastate;
}

/*
 * Clean up when evaluation of an ordered-set aggregate is complete.
 *
 * We don't need to bother freeing objects in the per-group memory context,
 * since that will get reset anyway by nodeAgg.c; nor should we free anything
 * in the per-query context, which will get cleared (if this was the last
 * group) by ExecutorEnd.  But we must take care to release any potential
 * non-memory resources.
 *
 * In the case where we're not expecting multiple finalfn calls, we could
 * arguably rely on the finalfn to clean up; but it's easier and more testable
 * if we just do it the same way in either case.
 */
static void
ordered_set_shutdown(Datum arg)
{
	OSAPerGroupState *osastate = (OSAPerGroupState *) DatumGetPointer(arg);

	/* Tuplesort object might have temp files. */
	if (osastate->sortstate)
		tuplesort_end(osastate->sortstate);
	osastate->sortstate = NULL;
	/* The tupleslot probably can't be holding a pin, but let's be safe. */
	if (osastate->qstate->tupslot)
		ExecClearTuple(osastate->qstate->tupslot);
}
/*
 * Generic transition function for ordered-set aggregates
 * with (potentially) multiple aggregated input columns
 */
Datum
ordered_set_transition_multi(PG_FUNCTION_ARGS)
{
	OSAPerGroupState *osastate;
	TupleTableSlot *slot;
	int			nargs;
	int			i;

	/* If first call, create the transition state workspace */
	if (PG_ARGISNULL(0))
		osastate = ordered_set_startup(fcinfo, true);
	else
		osastate = (OSAPerGroupState *) PG_GETARG_POINTER(0);

	/* Form a tuple from all the other inputs besides the transition value */
	slot = osastate->qstate->tupslot;
	ExecClearTuple(slot);
	nargs = PG_NARGS() - 1;
	for (i = 0; i < nargs; i++)
	{
		slot->tts_values[i] = PG_GETARG_DATUM(i + 1);
		slot->tts_isnull[i] = PG_ARGISNULL(i + 1);
	}
	if (osastate->qstate->aggref->aggkind == AGGKIND_HYPOTHETICAL)
	{
		/* Add a zero flag value to mark this row as a normal input row */
		slot->tts_values[i] = Int32GetDatum(0);
		slot->tts_isnull[i] = false;
		i++;
	}
	Assert(i == slot->tts_tupleDescriptor->natts);
	ExecStoreVirtualTuple(slot);

	/* Load the row into the tuplesort object */
	tuplesort_puttupleslot(osastate->sortstate, slot);
	osastate->number_of_rows++;

	PG_RETURN_POINTER(osastate);
}

/*
 * Common code to sanity-check args for hypothetical-set functions. No need
 * for friendly errors, these can only happen if someone's messing up the
 * aggregate definitions. The checks are needed for security, however.
 */
static void
hypothetical_check_argtypes(FunctionCallInfo fcinfo, int nargs,
							TupleDesc tupdesc)
{
	int			i;

	/* check that we have an int4 flag column */
	if (!tupdesc ||
		(nargs + 1) != tupdesc->natts ||
		TupleDescAttr(tupdesc, nargs)->atttypid != INT4OID)
		elog(ERROR, "type mismatch in hypothetical-set function");

	/* check that direct args match in type with aggregated args */
	for (i = 0; i < nargs; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		if (get_fn_expr_argtype(fcinfo->flinfo, i + 1) != attr->atttypid)
			elog(ERROR, "type mismatch in hypothetical-set function");
	}
}

/*
 * compute rank of hypothetical row
 *
 * flag should be -1 to sort hypothetical row ahead of its peers, or +1
 * to sort behind.
 * total number of regular rows is returned into *number_of_rows.
 */
static int64
hypothetical_rank_common(FunctionCallInfo fcinfo, int flag,
						 int64 *number_of_rows)
{
	int			nargs = PG_NARGS() - 1;
	int64		rank = 1;
	OSAPerGroupState *osastate;
	TupleTableSlot *slot;
	int			i;

	Assert(AggCheckCallContext(fcinfo, NULL) == AGG_CONTEXT_AGGREGATE);

	/* If there were no regular rows, the rank is always 1 */
	if (PG_ARGISNULL(0))
	{
		*number_of_rows = 0;
		return 1;
	}

	osastate = (OSAPerGroupState *) PG_GETARG_POINTER(0);
	*number_of_rows = osastate->number_of_rows;

	/* Adjust nargs to be the number of direct (or aggregated) args */
	if (nargs % 2 != 0)
		elog(ERROR, "wrong number of arguments in hypothetical-set function");
	nargs /= 2;

	hypothetical_check_argtypes(fcinfo, nargs, osastate->qstate->tupdesc);

	/* because we need a hypothetical row, we can't share transition state */
	Assert(!osastate->sort_done);

	/* insert the hypothetical row into the sort */
	slot = osastate->qstate->tupslot;
	ExecClearTuple(slot);
	for (i = 0; i < nargs; i++)
	{
		slot->tts_values[i] = PG_GETARG_DATUM(i + 1);
		slot->tts_isnull[i] = PG_ARGISNULL(i + 1);
	}
	slot->tts_values[i] = Int32GetDatum(flag);
	slot->tts_isnull[i] = false;
	ExecStoreVirtualTuple(slot);

	tuplesort_puttupleslot(osastate->sortstate, slot);

	/* finish the sort */
	tuplesort_performsort(osastate->sortstate);
	osastate->sort_done = true;

	/* iterate till we find the hypothetical row */
	while (tuplesort_gettupleslot(osastate->sortstate, true, true, slot, NULL))
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, nargs + 1, &isnull);

		if (!isnull && DatumGetInt32(d) != 0)
			break;

		rank++;

		CHECK_FOR_INTERRUPTS();
	}

	ExecClearTuple(slot);

	return rank;
}


/*
 * rank()  - rank of hypothetical row
 */
Datum
hypothetical_rank_final(PG_FUNCTION_ARGS)
{
	int64		rank;
	int64		rowcount;

	rank = hypothetical_rank_common(fcinfo, -1, &rowcount);

	PG_RETURN_INT64(rank);
}

/*
 * percent_rank()	- percentile rank of hypothetical row
 */
Datum
hypothetical_percent_rank_final(PG_FUNCTION_ARGS)
{
	int64		rank;
	int64		rowcount;
	double		result_val;

	rank = hypothetical_rank_common(fcinfo, -1, &rowcount);

	if (rowcount == 0)
		PG_RETURN_FLOAT8(0);

	result_val = (double) (rank - 1) / (double) (rowcount);

	PG_RETURN_FLOAT8(result_val);
}

/*
 * cume_dist()	- cumulative distribution of hypothetical row
 */
Datum
hypothetical_cume_dist_final(PG_FUNCTION_ARGS)
{
	int64		rank;
	int64		rowcount;
	double		result_val;

	rank = hypothetical_rank_common(fcinfo, 1, &rowcount);

	result_val = (double) (rank) / (double) (rowcount + 1);

	PG_RETURN_FLOAT8(result_val);
}

/*
 * dense_rank() - rank of hypothetical row without gaps in ranking
 */
Datum
hypothetical_dense_rank_final(PG_FUNCTION_ARGS)
{
	ExprContext *econtext;
	ExprState  *compareTuple;
	int			nargs = PG_NARGS() - 1;
	int64		rank = 1;
	int64		duplicate_count = 0;
	OSAPerGroupState *osastate;
	int			numDistinctCols;
	Datum		abbrevVal = (Datum) 0;
	Datum		abbrevOld = (Datum) 0;
	TupleTableSlot *slot;
	TupleTableSlot *extraslot;
	TupleTableSlot *slot2;
	int			i;

	Assert(AggCheckCallContext(fcinfo, NULL) == AGG_CONTEXT_AGGREGATE);

	/* If there were no regular rows, the rank is always 1 */
	if (PG_ARGISNULL(0))
		PG_RETURN_INT64(rank);

	osastate = (OSAPerGroupState *) PG_GETARG_POINTER(0);
	econtext = osastate->qstate->econtext;
	if (!econtext)
	{
		MemoryContext oldcontext;

		/* Make sure to we create econtext under correct parent context. */
		oldcontext = MemoryContextSwitchTo(osastate->qstate->qcontext);
		osastate->qstate->econtext = CreateStandaloneExprContext();
		econtext = osastate->qstate->econtext;
		MemoryContextSwitchTo(oldcontext);
	}

	/* Adjust nargs to be the number of direct (or aggregated) args */
	if (nargs % 2 != 0)
		elog(ERROR, "wrong number of arguments in hypothetical-set function");
	nargs /= 2;

	hypothetical_check_argtypes(fcinfo, nargs, osastate->qstate->tupdesc);

	/*
	 * When comparing tuples, we can omit the flag column since we will only
	 * compare rows with flag == 0.
	 */
	numDistinctCols = osastate->qstate->numSortCols - 1;

	/* Build tuple comparator, if we didn't already */
	compareTuple = osastate->qstate->compareTuple;
	if (compareTuple == NULL)
	{
		AttrNumber *sortColIdx = osastate->qstate->sortColIdx;
		MemoryContext oldContext;

		oldContext = MemoryContextSwitchTo(osastate->qstate->qcontext);
		compareTuple = execTuplesMatchPrepare(osastate->qstate->tupdesc,
											  numDistinctCols,
											  sortColIdx,
											  osastate->qstate->eqOperators,
											  osastate->qstate->sortCollations,
											  NULL);
		MemoryContextSwitchTo(oldContext);
		osastate->qstate->compareTuple = compareTuple;
	}

	/* because we need a hypothetical row, we can't share transition state */
	Assert(!osastate->sort_done);

	/* insert the hypothetical row into the sort */
	slot = osastate->qstate->tupslot;
	ExecClearTuple(slot);
	for (i = 0; i < nargs; i++)
	{
		slot->tts_values[i] = PG_GETARG_DATUM(i + 1);
		slot->tts_isnull[i] = PG_ARGISNULL(i + 1);
	}
	slot->tts_values[i] = Int32GetDatum(-1);
	slot->tts_isnull[i] = false;
	ExecStoreVirtualTuple(slot);

	tuplesort_puttupleslot(osastate->sortstate, slot);

	/* finish the sort */
	tuplesort_performsort(osastate->sortstate);
	osastate->sort_done = true;

	/*
	 * We alternate fetching into tupslot and extraslot so that we have the
	 * previous row available for comparisons.  This is accomplished by
	 * swapping the slot pointer variables after each row.
	 */
	extraslot = MakeSingleTupleTableSlot(osastate->qstate->tupdesc,
										 &TTSOpsMinimalTuple);
	slot2 = extraslot;

	/* iterate till we find the hypothetical row */
	while (tuplesort_gettupleslot(osastate->sortstate, true, true, slot,
								  &abbrevVal))
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, nargs + 1, &isnull);
		TupleTableSlot *tmpslot;

		if (!isnull && DatumGetInt32(d) != 0)
			break;

		/* count non-distinct tuples */
		econtext->ecxt_outertuple = slot;
		econtext->ecxt_innertuple = slot2;

		if (!TupIsNull(slot2) &&
			abbrevVal == abbrevOld &&
			ExecQualAndReset(compareTuple, econtext))
			duplicate_count++;

		tmpslot = slot2;
		slot2 = slot;
		slot = tmpslot;
		/* avoid ExecQual() calls by reusing abbreviated keys */
		abbrevOld = abbrevVal;

		rank++;

		CHECK_FOR_INTERRUPTS();
	}

	ExecClearTuple(slot);
	ExecClearTuple(slot2);

	ExecDropSingleTupleTableSlot(extraslot);

	rank = rank - duplicate_count;

	PG_RETURN_INT64(rank);
}
