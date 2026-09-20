/*-------------------------------------------------------------------------
 *
 * nodeBitmapHeapscan.c
 *	  Routines to support bitmapped scans of relations
 *
 * NOTE: it is critical that this plan type only be used with MVCC-compliant
 * snapshots (ie, regular snapshots, not SnapshotAny or one of the other
 * special snapshots).  The reason is that since index and heap scans are
 * decoupled, there can be no assurance that the index tuple prompting a
 * visit to a particular heap TID still exists when the visit is made.
 * Therefore the tuple might not exist anymore either (which is OK because
 * heap_fetch will cope) --- but worse, the tuple slot could have been
 * re-used for a newer tuple.  With an MVCC snapshot the newer tuple is
 * certain to fail the time qual and so it will not be mistakenly returned,
 * but with anything else we might return a tuple that doesn't meet the
 * required index qual conditions.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeBitmapHeapscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecBitmapHeapScan			scans a relation using bitmap info
 *		ExecBitmapHeapNext			workhorse for above
 *		ExecInitBitmapHeapScan		creates and initializes state info.
 *		ExecReScanBitmapHeapScan	prepares to rescan the plan.
 *		ExecEndBitmapHeapScan		releases all storage.
 */
#include "postgres.h"

#include <math.h>

#include "access/relscan.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/visibilitymap.h"
#include "executor/execdebug.h"
#include "executor/nodeBitmapHeapscan.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

static TupleTableSlot *BitmapHeapNext(BitmapHeapScanState *node);
static inline void BitmapAdjustPrefetchIterator(BitmapHeapScanState *node,
												TBMIterateResult *tbmres);
static inline void BitmapAdjustPrefetchTarget(BitmapHeapScanState *node);
static inline void BitmapPrefetch(BitmapHeapScanState *node,
								  TableScanDesc scan);


/* ----------------------------------------------------------------
 *		BitmapHeapNext
 *
 *		Retrieve next tuple from the BitmapHeapScan node's currentRelation
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
BitmapHeapNext(BitmapHeapScanState *node)
{
	ExprContext *econtext;
	TableScanDesc scan;
	TIDBitmap  *tbm;
	TBMIterator *tbmiterator = NULL;
	TBMIterateResult *tbmres;
	TupleTableSlot *slot;

	/*
	 * extract necessary information from index scan node
	 */
	econtext = node->ss.ps.ps_ExprContext;
	slot = node->ss.ss_ScanTupleSlot;
	scan = node->ss.ss_currentScanDesc;
	tbm = node->tbm;
	tbmiterator = node->tbmiterator;
	tbmres = node->tbmres;

	/*
	 * If we haven't yet performed the underlying index scan, do it, and begin
	 * the iteration over the bitmap.
	 *
	 * For prefetching, we use *two* iterators, one for the pages we are
	 * actually scanning and another that runs ahead of the first for
	 * prefetching.  node->prefetch_pages tracks exactly how many pages ahead
	 * the prefetch iterator is.  Also, node->prefetch_target tracks the
	 * desired prefetch distance, which starts small and increases up to the
	 * node->prefetch_maximum.  This is to avoid doing a lot of prefetching in
	 * a scan that stops after a few tuples because of a LIMIT.
	 */
	if (!node->initialized)
	{
		tbm = (TIDBitmap *) MultiExecProcNode(outerPlanState(node));

		if (!tbm || !IsA(tbm, TIDBitmap))
			elog(ERROR, "unrecognized result from subplan");

		node->tbm = tbm;
		node->tbmiterator = tbmiterator = tbm_begin_iterate(tbm);
		node->tbmres = tbmres = NULL;

#ifdef USE_PREFETCH
		if (node->prefetch_maximum > 0)
		{
			node->prefetch_iterator = tbm_begin_iterate(tbm);
			node->prefetch_pages = 0;
			node->prefetch_target = -1;
		}
#endif							/* USE_PREFETCH */

		node->initialized = true;
	}

	for (;;)
	{
		bool		skip_fetch;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Get next page of results if needed
		 */
		if (tbmres == NULL)
		{
			node->tbmres = tbmres = tbm_iterate(tbmiterator);
			if (tbmres == NULL)
			{
				/* no more entries in the bitmap */
				break;
			}

			BitmapAdjustPrefetchIterator(node, tbmres);

			if (tbmres->ntuples >= 0)
				node->exact_pages++;
			else
				node->lossy_pages++;

			/*
			 * We can skip fetching the heap page if we don't need any fields
			 * from the heap, and the bitmap entries don't need rechecking,
			 * and all tuples on the page are visible to our transaction.
			 *
			 * XXX: It's a layering violation that we do these checks above
			 * tableam, they should probably moved below it at some point.
			 */
			skip_fetch = (node->can_skip_fetch &&
						  !tbmres->recheck &&
						  VM_ALL_VISIBLE(node->ss.ss_currentRelation,
										 tbmres->blockno,
										 &node->vmbuffer));

			if (skip_fetch)
			{
				/* can't be lossy in the skip_fetch case */
				Assert(tbmres->ntuples >= 0);

				/*
				 * The number of tuples on this page is put into
				 * node->return_empty_tuples.
				 */
				node->return_empty_tuples = tbmres->ntuples;
			}
			else if (!table_scan_bitmap_next_block(scan, tbmres))
			{
				/* AM doesn't think this block is valid, skip */
				continue;
			}

			/* Adjust the prefetch target */
			BitmapAdjustPrefetchTarget(node);
		}
		else
		{
			/*
			 * Continuing in previously obtained page.
			 */

#ifdef USE_PREFETCH

			/*
			 * Try to prefetch at least a few pages even before we get to the
			 * second page if we don't stop reading after the first tuple.
			 */
			if (node->prefetch_target < node->prefetch_maximum)
				node->prefetch_target++;
#endif							/* USE_PREFETCH */
		}

		/*
		 * We issue prefetch requests *after* fetching the current page to try
		 * to avoid having prefetching interfere with the main I/O. Also, this
		 * should happen only when we have determined there is still something
		 * to do on the current page, else we may uselessly prefetch the same
		 * page we are just about to request for real.
		 *
		 * XXX: It's a layering violation that we do these checks above
		 * tableam, they should probably moved below it at some point.
		 */
		BitmapPrefetch(node, scan);

		if (node->return_empty_tuples > 0)
		{
			/*
			 * If we don't have to fetch the tuple, just return nulls.
			 */
			ExecStoreAllNullTuple(slot);

			if (--node->return_empty_tuples == 0)
			{
				/* no more tuples to return in the next round */
				node->tbmres = tbmres = NULL;
			}
		}
		else
		{
			/*
			 * Attempt to fetch tuple from AM.
			 */
			if (!table_scan_bitmap_next_tuple(scan, tbmres, slot))
			{
				/* nothing more to look at on this page */
				node->tbmres = tbmres = NULL;
				continue;
			}

			/*
			 * If we are using lossy info, we have to recheck the qual
			 * conditions at every tuple.
			 */
			if (tbmres->recheck)
			{
				econtext->ecxt_scantuple = slot;
				if (!ExecQualAndReset(node->bitmapqualorig, econtext))
				{
					/* Fails recheck, so drop it and loop back for another */
					InstrCountFiltered2(node, 1);
					ExecClearTuple(slot);
					continue;
				}
			}
		}

		/* OK to return this tuple */
		return slot;
	}

	/*
	 * if we get here it means we are at the end of the scan..
	 */
	return ExecClearTuple(slot);
}

/*
 *	BitmapAdjustPrefetchIterator - Adjust the prefetch iterator
 */
static inline void
BitmapAdjustPrefetchIterator(BitmapHeapScanState *node,
							 TBMIterateResult *tbmres)
{
#ifdef USE_PREFETCH
	TBMIterator *prefetch_iterator = node->prefetch_iterator;

	if (node->prefetch_pages > 0)
	{
		/* The main iterator has closed the distance by one page */
		node->prefetch_pages--;
	}
	else if (prefetch_iterator)
	{
		/* Do not let the prefetch iterator get behind the main one */
		TBMIterateResult *tbmpre = tbm_iterate(prefetch_iterator);

		if (tbmpre == NULL || tbmpre->blockno != tbmres->blockno)
			elog(ERROR, "prefetch and main iterators are out of sync");
	}
#endif							/* USE_PREFETCH */
}

/*
 * BitmapAdjustPrefetchTarget - Adjust the prefetch target
 *
 * Increase prefetch target if it's not yet at the max.  Note that
 * we will increase it to zero after fetching the very first
 * page/tuple, then to one after the second tuple is fetched, then
 * it doubles as later pages are fetched.
 */
static inline void
BitmapAdjustPrefetchTarget(BitmapHeapScanState *node)
{
#ifdef USE_PREFETCH
	if (node->prefetch_target >= node->prefetch_maximum)
		 /* don't increase any further */ ;
	else if (node->prefetch_target >= node->prefetch_maximum / 2)
		node->prefetch_target = node->prefetch_maximum;
	else if (node->prefetch_target > 0)
		node->prefetch_target *= 2;
	else
		node->prefetch_target++;
#endif							/* USE_PREFETCH */
}

/*
 * BitmapPrefetch - Prefetch, if prefetch_pages are behind prefetch_target
 */
static inline void
BitmapPrefetch(BitmapHeapScanState *node, TableScanDesc scan)
{
#ifdef USE_PREFETCH
	TBMIterator *prefetch_iterator = node->prefetch_iterator;

	if (prefetch_iterator)
	{
		while (node->prefetch_pages < node->prefetch_target)
		{
			TBMIterateResult *tbmpre = tbm_iterate(prefetch_iterator);
			bool		skip_fetch;

			if (tbmpre == NULL)
			{
				/* No more pages to prefetch */
				tbm_end_iterate(prefetch_iterator);
				node->prefetch_iterator = NULL;
				break;
			}
			node->prefetch_pages++;

			/*
			 * If we expect not to have to actually read this heap page,
			 * skip this prefetch call, but continue to run the prefetch
			 * logic normally.  (Would it be better not to increment
			 * prefetch_pages?)
			 *
			 * This depends on the assumption that the index AM will
			 * report the same recheck flag for this future heap page as
			 * it did for the current heap page; which is not a certainty
			 * but is true in many cases.
			 */
			skip_fetch = (node->can_skip_fetch &&
						  (node->tbmres ? !node->tbmres->recheck : false) &&
						  VM_ALL_VISIBLE(node->ss.ss_currentRelation,
										 tbmpre->blockno,
										 &node->pvmbuffer));

			if (!skip_fetch)
				PrefetchBuffer(scan->rs_rd, MAIN_FORKNUM, tbmpre->blockno);
		}
	}
#endif							/* USE_PREFETCH */
}

/*
 * BitmapHeapRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
BitmapHeapRecheck(BitmapHeapScanState *node, TupleTableSlot *slot)
{
	ExprContext *econtext;

	/*
	 * extract necessary information from index scan node
	 */
	econtext = node->ss.ps.ps_ExprContext;

	/* Does the tuple meet the original qual conditions? */
	econtext->ecxt_scantuple = slot;
	return ExecQualAndReset(node->bitmapqualorig, econtext);
}

/* ----------------------------------------------------------------
 *		ExecBitmapHeapScan(node)
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecBitmapHeapScan(PlanState *pstate)
{
	BitmapHeapScanState *node = castNode(BitmapHeapScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) BitmapHeapNext,
					(ExecScanRecheckMtd) BitmapHeapRecheck);
}

/* ----------------------------------------------------------------
 *		ExecReScanBitmapHeapScan(node)
 * ----------------------------------------------------------------
 */
void
ExecReScanBitmapHeapScan(BitmapHeapScanState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/* rescan to release any page pin */
	table_rescan(node->ss.ss_currentScanDesc, NULL);

	/* release bitmaps and buffers if any */
	if (node->tbmiterator)
		tbm_end_iterate(node->tbmiterator);
	if (node->prefetch_iterator)
		tbm_end_iterate(node->prefetch_iterator);
	if (node->tbm)
		tbm_free(node->tbm);
	if (node->vmbuffer != InvalidBuffer)
		ReleaseBuffer(node->vmbuffer);
	if (node->pvmbuffer != InvalidBuffer)
		ReleaseBuffer(node->pvmbuffer);
	node->tbm = NULL;
	node->tbmiterator = NULL;
	node->tbmres = NULL;
	node->prefetch_iterator = NULL;
	node->initialized = false;
	node->vmbuffer = InvalidBuffer;
	node->pvmbuffer = InvalidBuffer;

	ExecScanReScan(&node->ss);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}

/* ----------------------------------------------------------------
 *		ExecEndBitmapHeapScan
 * ----------------------------------------------------------------
 */
void
ExecEndBitmapHeapScan(BitmapHeapScanState *node)
{
	TableScanDesc scanDesc;

	/*
	 * extract information from the node
	 */
	scanDesc = node->ss.ss_currentScanDesc;

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * clear out tuple table slots
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * close down subplans
	 */
	ExecEndNode(outerPlanState(node));

	/*
	 * release bitmaps and buffers if any
	 */
	if (node->tbmiterator)
		tbm_end_iterate(node->tbmiterator);
	if (node->prefetch_iterator)
		tbm_end_iterate(node->prefetch_iterator);
	if (node->tbm)
		tbm_free(node->tbm);
	if (node->vmbuffer != InvalidBuffer)
		ReleaseBuffer(node->vmbuffer);
	if (node->pvmbuffer != InvalidBuffer)
		ReleaseBuffer(node->pvmbuffer);

	/*
	 * close heap scan
	 */
	table_endscan(scanDesc);
}

/* ----------------------------------------------------------------
 *		ExecInitBitmapHeapScan
 *
 *		Initializes the scan's state information.
 * ----------------------------------------------------------------
 */
BitmapHeapScanState *
ExecInitBitmapHeapScan(BitmapHeapScan *node, EState *estate, int eflags)
{
	BitmapHeapScanState *scanstate;
	Relation	currentRelation;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * Assert caller didn't ask for an unsafe snapshot --- see comments at
	 * head of file.
	 */
	Assert(IsMVCCSnapshot(estate->es_snapshot));

	/*
	 * create state structure
	 */
	scanstate = makeNode(BitmapHeapScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecBitmapHeapScan;

	scanstate->tbm = NULL;
	scanstate->tbmiterator = NULL;
	scanstate->tbmres = NULL;
	scanstate->return_empty_tuples = 0;
	scanstate->vmbuffer = InvalidBuffer;
	scanstate->pvmbuffer = InvalidBuffer;
	scanstate->exact_pages = 0;
	scanstate->lossy_pages = 0;
	scanstate->prefetch_iterator = NULL;
	scanstate->prefetch_pages = 0;
	scanstate->prefetch_target = 0;
	scanstate->initialized = false;

	/*
	 * Unfortunately it turns out that the below optimization does not
	 * take the removal of TIDs by a concurrent vacuum into
	 * account. The concurrent vacuum can remove dead TIDs and make
	 * pages ALL_VISIBLE while those dead TIDs are referenced in the
	 * bitmap. This would lead to a !need_tuples scan returning too
	 * many tuples.
	 *
	 * In the back-branches, we therefore simply disable the
	 * optimization. Removing all the relevant code would be too
	 * invasive (and a major backpatching pain).
	 */
	scanstate->can_skip_fetch = false;
#ifdef NOT_ANYMORE
	/*
	 * We can potentially skip fetching heap pages if we do not need any
	 * columns of the table, either for checking non-indexable quals or for
	 * returning data.  This test is a bit simplistic, as it checks the
	 * stronger condition that there's no qual or return tlist at all.  But in
	 * most cases it's probably not worth working harder than that.
	 */
	scanstate->can_skip_fetch = (node->scan.plan.qual == NIL &&
								 node->scan.plan.targetlist == NIL);
#endif

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * open the scan relation
	 */
	currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid, eflags);

	/*
	 * initialize child nodes
	 */
	outerPlanState(scanstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * get the scan type from the relation descriptor.
	 */
	ExecInitScanTupleSlot(estate, &scanstate->ss,
						  RelationGetDescr(currentRelation),
						  table_slot_callbacks(currentRelation));

	/*
	 * Initialize result type and projection.
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);
	scanstate->bitmapqualorig =
		ExecInitQual(node->bitmapqualorig, (PlanState *) scanstate);

	/*
	 * Maximum number of prefetches, the current value of the
	 * effective_io_concurrency GUC.
	 */
	scanstate->prefetch_maximum = effective_io_concurrency;

	scanstate->ss.ss_currentRelation = currentRelation;

	scanstate->ss.ss_currentScanDesc = table_beginscan_bm(currentRelation,
														  estate->es_snapshot,
														  0,
														  NULL);

	/*
	 * all done.
	 */
	return scanstate;
}
