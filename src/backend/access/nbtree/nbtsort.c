/*-------------------------------------------------------------------------
 *
 * nbtsort.c
 *		Build a btree from sorted input by loading leaf pages sequentially.
 *
 * NOTES
 *
 * We use tuplesort.c to sort the given index tuples into order.
 * Then we scan the index tuples in order and build the btree pages
 * for each level.  We load source tuples into leaf-level pages.
 * Whenever we fill a page at one level, we add a link to it to its
 * parent level (starting a new parent level if necessary).  When
 * done, we write out each final page on each level, adding it to
 * its parent level.  When we have only one page on a level, it must be
 * the root -- it can be attached to the btree metapage and we are done.
 *
 * It is not wise to pack the pages entirely full, since then *any*
 * insertion would cause a split (and not only of the leaf page; the need
 * for a split would cascade right up the tree).  The steady-state load
 * factor for btrees is usually estimated at 70%.  We choose to pack leaf
 * pages to the user-controllable fill factor (default 90%) while upper pages
 * are always packed to 70%.  This gives us reasonable density (there aren't
 * many upper pages if the keys are reasonable-size) without risking a lot of
 * cascading splits during early insertions.
 *
 * Formerly the index pages being built were kept in shared buffers, but
 * that is of no value (since other backends have no interest in them yet)
 * and it created locking problems for CHECKPOINT, because the upper-level
 * pages were held exclusive-locked for long periods.  Now we just build
 * the pages in local memory and smgrwrite or smgrextend them as we finish
 * them.  They will need to be re-read into shared buffers on first use after
 * the build finishes.
 *
 * This code isn't concerned about the FSM at all. The caller is responsible
 * for initializing that.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtsort.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/nbtree.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "commands/progress.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"		/* pgrminclude ignore */
#include "utils/rel.h"
#include "utils/sortsupport.h"
#include "utils/tuplesort.h"




/*
 * Status record for spooling/sorting phase.  (Note we may have two of
 * these due to the special requirements for uniqueness-checking with
 * dead tuples.)
 */
typedef struct BTSpool
{
	Tuplesortstate *sortstate;	/* state data for tuplesort.c */
	Relation	heap;
	Relation	index;
	bool		isunique;
} BTSpool;

/*
 * Working state for btbuild and its callback.
 */
typedef struct BTBuildState
{
	bool		isunique;
	bool		havedead;
	Relation	heap;
	BTSpool    *spool;

	/*
	 * spool2 is needed only when the index is a unique index. Dead tuples are
	 * put into spool2 instead of spool in order to avoid uniqueness check.
	 */
	BTSpool    *spool2;
	double		indtuples;
} BTBuildState;

/*
 * Status record for a btree page being built.  We have one of these
 * for each active tree level.
 */
typedef struct BTPageState
{
	Page		btps_page;		/* workspace for page building */
	BlockNumber btps_blkno;		/* block # to write this page at */
	IndexTuple	btps_lowkey;	/* page's strict lower bound pivot tuple */
	OffsetNumber btps_lastoff;	/* last item offset loaded */
	Size		btps_lastextra; /* last item's extra posting list space */
	uint32		btps_level;		/* tree level (0 = leaf) */
	Size		btps_full;		/* "full" if less than this much free space */
	struct BTPageState *btps_next;	/* link to parent level, if any */
} BTPageState;

/*
 * Overall status record for index writing phase.
 */
typedef struct BTWriteState
{
	Relation	heap;
	Relation	index;
	BTScanInsert inskey;		/* generic insertion scankey */
	bool		btws_use_wal;	/* dump pages to WAL? */
	BlockNumber btws_pages_alloced; /* # pages allocated */
	BlockNumber btws_pages_written; /* # pages written out */
	Page		btws_zeropage;	/* workspace for filling zeroes */
} BTWriteState;


static double _bt_spools_heapscan(Relation heap, Relation index,
								  BTBuildState *buildstate, IndexInfo *indexInfo);
static void _bt_spooldestroy(BTSpool *btspool);
static void _bt_spool(BTSpool *btspool, ItemPointer self,
					  Datum *values, bool *isnull);
static void _bt_leafbuild(BTSpool *btspool, BTSpool *btspool2);
static void _bt_build_callback(Relation index, ItemPointer tid, Datum *values,
							   bool *isnull, bool tupleIsAlive, void *state);
static Page _bt_blnewpage(uint32 level);
static BTPageState *_bt_pagestate(BTWriteState *wstate, uint32 level);
static void _bt_slideleft(Page rightmostpage);
static void _bt_sortaddtup(Page page, Size itemsize,
						   IndexTuple itup, OffsetNumber itup_off,
						   bool newfirstdataitem);
static void _bt_buildadd(BTWriteState *wstate, BTPageState *state,
						 IndexTuple itup, Size truncextra);
static void _bt_sort_dedup_finish_pending(BTWriteState *wstate,
										  BTPageState *state,
										  BTDedupState dstate);
static void _bt_uppershutdown(BTWriteState *wstate, BTPageState *state);
static void _bt_load(BTWriteState *wstate,
					 BTSpool *btspool, BTSpool *btspool2);

/*
 *	btbuild() -- build a new btree index.
 */
IndexBuildResult *
btbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	BTBuildState buildstate;
	double		reltuples;


	buildstate.isunique = indexInfo->ii_Unique;
	buildstate.havedead = false;
	buildstate.heap = heap;
	buildstate.spool = NULL;
	buildstate.spool2 = NULL;
	buildstate.indtuples = 0;

	/*
	 * We expect to be called exactly once for any index relation. If that's
	 * not the case, big trouble's what we have.
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	reltuples = _bt_spools_heapscan(heap, index, &buildstate, indexInfo);

	/*
	 * Finish the build by (1) completing the sort of the spool file, (2)
	 * inserting the sorted tuples into btree pages and (3) building the upper
	 * levels.  Finally, it may also be necessary to end use of parallelism.
	 */
	_bt_leafbuild(buildstate.spool, buildstate.spool2);
	_bt_spooldestroy(buildstate.spool);
	if (buildstate.spool2)
		_bt_spooldestroy(buildstate.spool2);

	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;


	return result;
}

/*
 * Create and initialize one or two spool structures, and save them in caller's
 * buildstate argument.  May also fill-in fields within indexInfo used by index
 * builds.
 *
 * Scans the heap, possibly in parallel, filling spools with IndexTuples.  This
 * routine encapsulates all aspects of managing parallelism.  Caller need only
 * call _bt_end_parallel() in parallel case after it is done with spool/spool2.
 *
 * Returns the total number of heap tuples scanned.
 */
static double
_bt_spools_heapscan(Relation heap, Relation index, BTBuildState *buildstate,
					IndexInfo *indexInfo)
{
	BTSpool    *btspool = (BTSpool *) palloc0(sizeof(BTSpool));
	double		reltuples = 0;

	/*
	 * We size the sort area as maintenance_work_mem rather than work_mem to
	 * speed index creation.  This should be OK since a single backend can't
	 * run multiple index creations in parallel (see also: notes on
	 * parallelism and maintenance_work_mem below).
	 */
	btspool->heap = heap;
	btspool->index = index;
	btspool->isunique = indexInfo->ii_Unique;

	/* Save as primary spool */
	buildstate->spool = btspool;

	/* Report table scan phase started */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
								 PROGRESS_BTREE_PHASE_INDEXBUILD_TABLESCAN);

	/*
	 * Begin serial/leader tuplesort.
	 *
	 * In cases where parallelism is involved, the leader receives the same
	 * share of maintenance_work_mem as a serial sort (it is generally treated
	 * in the same way as a serial sort once we return).  Parallel worker
	 * Tuplesortstates will have received only a fraction of
	 * maintenance_work_mem, though.
	 *
	 * We rely on the lifetime of the Leader Tuplesortstate almost not
	 * overlapping with any worker Tuplesortstate's lifetime.  There may be
	 * some small overlap, but that's okay because we rely on leader
	 * Tuplesortstate only allocating a small, fixed amount of memory here.
	 * When its tuplesort_performsort() is called (by our caller), and
	 * significant amounts of memory are likely to be used, all workers must
	 * have already freed almost all memory held by their Tuplesortstates
	 * (they are about to go away completely, too).  The overall effect is
	 * that maintenance_work_mem always represents an absolute high watermark
	 * on the amount of memory used by a CREATE INDEX operation, regardless of
	 * the use of parallelism or any other factor.
	 */
	buildstate->spool->sortstate =
		tuplesort_begin_index_btree(heap, index, buildstate->isunique,
									maintenance_work_mem,
									false);

	/*
	 * If building a unique index, put dead tuples in a second spool to keep
	 * them out of the uniqueness check.  We expect that the second spool (for
	 * dead tuples) won't get very full, so we give it only work_mem.
	 */
	if (indexInfo->ii_Unique)
	{
		BTSpool    *btspool2 = (BTSpool *) palloc0(sizeof(BTSpool));

		/* Initialize secondary spool */
		btspool2->heap = heap;
		btspool2->index = index;
		btspool2->isunique = false;
		/* Save as secondary spool */
		buildstate->spool2 = btspool2;

		/*
		 * We expect that the second one (for dead tuples) won't get very
		 * full, so we give it only work_mem
		 */
		buildstate->spool2->sortstate =
			tuplesort_begin_index_btree(heap, index, false, work_mem,
										false);
	}

	/* Fill spool using a serial heap scan */
	reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
									   _bt_build_callback, (void *) buildstate,
									   NULL);

	/*
	 * Set the progress target for the next phase.  Reset the block number
	 * values set by table_index_build_scan
	 */
	{
		const int	progress_index[] = {
			PROGRESS_CREATEIDX_TUPLES_TOTAL,
			PROGRESS_SCAN_BLOCKS_TOTAL,
			PROGRESS_SCAN_BLOCKS_DONE
		};
		const int64 progress_vals[] = {
			buildstate->indtuples,
			0, 0
		};

		pgstat_progress_update_multi_param(3, progress_index, progress_vals);
	}

	/* okay, all heap tuples are spooled */
	if (buildstate->spool2 && !buildstate->havedead)
	{
		/* spool2 turns out to be unnecessary */
		_bt_spooldestroy(buildstate->spool2);
		buildstate->spool2 = NULL;
	}

	return reltuples;
}

/*
 * clean up a spool structure and its substructures.
 */
static void
_bt_spooldestroy(BTSpool *btspool)
{
	tuplesort_end(btspool->sortstate);
	pfree(btspool);
}

/*
 * spool an index entry into the sort file.
 */
static void
_bt_spool(BTSpool *btspool, ItemPointer self, Datum *values, bool *isnull)
{
	tuplesort_putindextuplevalues(btspool->sortstate, btspool->index,
								  self, values, isnull);
}

/*
 * given a spool loaded by successive calls to _bt_spool,
 * create an entire btree.
 */
static void
_bt_leafbuild(BTSpool *btspool, BTSpool *btspool2)
{
	BTWriteState wstate;


	/* Execute the sort */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
								 PROGRESS_BTREE_PHASE_PERFORMSORT_1);
	tuplesort_performsort(btspool->sortstate);
	if (btspool2)
	{
		pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
									 PROGRESS_BTREE_PHASE_PERFORMSORT_2);
		tuplesort_performsort(btspool2->sortstate);
	}

	wstate.heap = btspool->heap;
	wstate.index = btspool->index;
	wstate.inskey = _bt_mkscankey(wstate.index, NULL);
	/* _bt_mkscankey() won't set allequalimage without metapage */
	wstate.inskey->allequalimage = _bt_allequalimage(wstate.index, true);
	wstate.btws_use_wal = RelationNeedsWAL(wstate.index);

	/* reserve the metapage */
	wstate.btws_pages_alloced = BTREE_METAPAGE + 1;
	wstate.btws_pages_written = 0;
	wstate.btws_zeropage = NULL;	/* until needed */

	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
								 PROGRESS_BTREE_PHASE_LEAF_LOAD);
	_bt_load(&wstate, btspool, btspool2);
}

/*
 * Per-tuple callback for table_index_build_scan
 */
static void
_bt_build_callback(Relation index,
				   ItemPointer tid,
				   Datum *values,
				   bool *isnull,
				   bool tupleIsAlive,
				   void *state)
{
	BTBuildState *buildstate = (BTBuildState *) state;

	/*
	 * insert the index tuple into the appropriate spool file for subsequent
	 * processing
	 */
	if (tupleIsAlive || buildstate->spool2 == NULL)
		_bt_spool(buildstate->spool, tid, values, isnull);
	else
	{
		/* dead tuples are put into spool2 */
		buildstate->havedead = true;
		_bt_spool(buildstate->spool2, tid, values, isnull);
	}

	buildstate->indtuples += 1;
}

/*
 * allocate workspace for a new, clean btree page, not linked to any siblings.
 */
static Page
_bt_blnewpage(uint32 level)
{
	Page		page;
	BTPageOpaque opaque;

	page = (Page) palloc(BLCKSZ);

	/* Zero the page and set up standard page header info */
	_bt_pageinit(page, BLCKSZ);

	/* Initialize BT opaque state */
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	opaque->btpo_prev = opaque->btpo_next = P_NONE;
	opaque->btpo_level = level;
	opaque->btpo_flags = (level > 0) ? 0 : BTP_LEAF;
	opaque->btpo_cycleid = 0;

	/* Make the P_HIKEY line pointer appear allocated */
	((PageHeader) page)->pd_lower += sizeof(ItemIdData);

	return page;
}

/*
 * emit a completed btree page, and release the working storage.
 */
static void
_bt_blwritepage(BTWriteState *wstate, Page page, BlockNumber blkno)
{
	/* XLOG stuff */
	if (wstate->btws_use_wal)
	{
		/* We use the XLOG_FPI record type for this */
		log_newpage(&wstate->index->rd_node, MAIN_FORKNUM, blkno, page, true);
	}

	/*
	 * If we have to write pages nonsequentially, fill in the space with
	 * zeroes until we come back and overwrite.  This is not logically
	 * necessary on standard Unix filesystems (unwritten space will read as
	 * zeroes anyway), but it should help to avoid fragmentation. The dummy
	 * pages aren't WAL-logged though.
	 */
	while (blkno > wstate->btws_pages_written)
	{
		if (!wstate->btws_zeropage)
			wstate->btws_zeropage = (Page) palloc0(BLCKSZ);
		/* don't set checksum for all-zero page */
		smgrextend(RelationGetSmgr(wstate->index), MAIN_FORKNUM,
				   wstate->btws_pages_written++,
				   (char *) wstate->btws_zeropage,
				   true);
	}

	/*
	 * Now write the page.  There's no need for smgr to schedule an fsync for
	 * this write; we'll do it ourselves before ending the build.
	 */
	if (blkno == wstate->btws_pages_written)
	{
		/* extending the file... */
		smgrextend(RelationGetSmgr(wstate->index), MAIN_FORKNUM, blkno,
				   (char *) page, true);
		wstate->btws_pages_written++;
	}
	else
	{
		/* overwriting a block we zero-filled before */
		smgrwrite(RelationGetSmgr(wstate->index), MAIN_FORKNUM, blkno,
				  (char *) page, true);
	}

	pfree(page);
}

/*
 * allocate and initialize a new BTPageState.  the returned structure
 * is suitable for immediate use by _bt_buildadd.
 */
static BTPageState *
_bt_pagestate(BTWriteState *wstate, uint32 level)
{
	BTPageState *state = (BTPageState *) palloc0(sizeof(BTPageState));

	/* create initial page for level */
	state->btps_page = _bt_blnewpage(level);

	/* and assign it a page position */
	state->btps_blkno = wstate->btws_pages_alloced++;

	state->btps_lowkey = NULL;
	/* initialize lastoff so first item goes into P_FIRSTKEY */
	state->btps_lastoff = P_HIKEY;
	state->btps_lastextra = 0;
	state->btps_level = level;
	/* set "full" threshold based on level.  See notes at head of file. */
	if (level > 0)
		state->btps_full = (BLCKSZ * (100 - BTREE_NONLEAF_FILLFACTOR) / 100);
	else
		state->btps_full = BLCKSZ * (100 - BTREE_DEFAULT_FILLFACTOR) / 100;

	/* no parent level, yet */
	state->btps_next = NULL;

	return state;
}

/*
 * Slide the array of ItemIds from the page back one slot (from P_FIRSTKEY to
 * P_HIKEY, overwriting P_HIKEY).
 *
 * _bt_blnewpage() makes the P_HIKEY line pointer appear allocated, but the
 * rightmost page on its level is not supposed to get a high key.  Now that
 * it's clear that this page is a rightmost page, remove the unneeded empty
 * P_HIKEY line pointer space.
 */
static void
_bt_slideleft(Page rightmostpage)
{
	OffsetNumber off;
	OffsetNumber maxoff;
	ItemId		previi;

	maxoff = PageGetMaxOffsetNumber(rightmostpage);
	Assert(maxoff >= P_FIRSTKEY);
	previi = PageGetItemId(rightmostpage, P_HIKEY);
	for (off = P_FIRSTKEY; off <= maxoff; off = OffsetNumberNext(off))
	{
		ItemId		thisii = PageGetItemId(rightmostpage, off);

		*previi = *thisii;
		previi = thisii;
	}
	((PageHeader) rightmostpage)->pd_lower -= sizeof(ItemIdData);
}

/*
 * Add an item to a page being built.
 *
 * This is very similar to nbtinsert.c's _bt_pgaddtup(), but this variant
 * raises an error directly.
 *
 * Note that our nbtsort.c caller does not know yet if the page will be
 * rightmost.  Offset P_FIRSTKEY is always assumed to be the first data key by
 * caller.  Page that turns out to be the rightmost on its level is fixed by
 * calling _bt_slideleft().
 */
static void
_bt_sortaddtup(Page page,
			   Size itemsize,
			   IndexTuple itup,
			   OffsetNumber itup_off,
			   bool newfirstdataitem)
{
	IndexTupleData trunctuple;

	if (newfirstdataitem)
	{
		trunctuple = *itup;
		trunctuple.t_info = sizeof(IndexTupleData);
		BTreeTupleSetNAtts(&trunctuple, 0, false);
		itup = &trunctuple;
		itemsize = sizeof(IndexTupleData);
	}

	if (PageAddItem(page, (Item) itup, itemsize, itup_off,
					false, false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add item to the index page");
}

/*----------
 * Add an item to a disk page from the sort output (or add a posting list
 * item formed from the sort output).
 *
 * We must be careful to observe the page layout conventions of nbtsearch.c:
 * - rightmost pages start data items at P_HIKEY instead of at P_FIRSTKEY.
 * - on non-leaf pages, the key portion of the first item need not be
 *	 stored, we should store only the link.
 *
 * A leaf page being built looks like:
 *
 * +----------------+---------------------------------+
 * | PageHeaderData | linp0 linp1 linp2 ...           |
 * +-----------+----+---------------------------------+
 * | ... linpN |									  |
 * +-----------+--------------------------------------+
 * |	 ^ last										  |
 * |												  |
 * +-------------+------------------------------------+
 * |			 | itemN ...                          |
 * +-------------+------------------+-----------------+
 * |		  ... item3 item2 item1 | "special space" |
 * +--------------------------------+-----------------+
 *
 * Contrast this with the diagram in bufpage.h; note the mismatch
 * between linps and items.  This is because we reserve linp0 as a
 * placeholder for the pointer to the "high key" item; when we have
 * filled up the page, we will set linp0 to point to itemN and clear
 * linpN.  On the other hand, if we find this is the last (rightmost)
 * page, we leave the items alone and slide the linp array over.  If
 * the high key is to be truncated, offset 1 is deleted, and we insert
 * the truncated high key at offset 1.
 *
 * 'last' pointer indicates the last offset added to the page.
 *
 * 'truncextra' is the size of the posting list in itup, if any.  This
 * information is stashed for the next call here, when we may benefit
 * from considering the impact of truncating away the posting list on
 * the page before deciding to finish the page off.  Posting lists are
 * often relatively large, so it is worth going to the trouble of
 * accounting for the saving from truncating away the posting list of
 * the tuple that becomes the high key (that may be the only way to
 * get close to target free space on the page).  Note that this is
 * only used for the soft fillfactor-wise limit, not the critical hard
 * limit.
 *----------
 */
static void
_bt_buildadd(BTWriteState *wstate, BTPageState *state, IndexTuple itup,
			 Size truncextra)
{
	Page		npage;
	BlockNumber nblkno;
	OffsetNumber last_off;
	Size		last_truncextra;
	Size		pgspc;
	Size		itupsz;
	bool		isleaf;

	/*
	 * This is a handy place to check for cancel interrupts during the btree
	 * load phase of index creation.
	 */
	CHECK_FOR_INTERRUPTS();

	npage = state->btps_page;
	nblkno = state->btps_blkno;
	last_off = state->btps_lastoff;
	last_truncextra = state->btps_lastextra;
	state->btps_lastextra = truncextra;

	pgspc = PageGetFreeSpace(npage);
	itupsz = IndexTupleSize(itup);
	itupsz = MAXALIGN(itupsz);
	/* Leaf case has slightly different rules due to suffix truncation */
	isleaf = (state->btps_level == 0);

	/*
	 * Check whether the new item can fit on a btree page on current level at
	 * all.
	 *
	 * Every newly built index will treat heap TID as part of the keyspace,
	 * which imposes the requirement that new high keys must occasionally have
	 * a heap TID appended within _bt_truncate().  That may leave a new pivot
	 * tuple one or two MAXALIGN() quantums larger than the original
	 * firstright tuple it's derived from.  v4 deals with the problem by
	 * decreasing the limit on the size of tuples inserted on the leaf level
	 * by the same small amount.  Enforce the new v4+ limit on the leaf level,
	 * and the old limit on internal levels, since pivot tuples may need to
	 * make use of the reserved space.  This should never fail on internal
	 * pages.
	 */
	if (unlikely(itupsz > BTMaxItemSize(npage)))
		_bt_check_third_page(wstate->index, wstate->heap, isleaf, npage,
							 itup);

	/*
	 * Check to see if current page will fit new item, with space left over to
	 * append a heap TID during suffix truncation when page is a leaf page.
	 *
	 * It is guaranteed that we can fit at least 2 non-pivot tuples plus a
	 * high key with heap TID when finishing off a leaf page, since we rely on
	 * _bt_check_third_page() rejecting oversized non-pivot tuples.  On
	 * internal pages we can always fit 3 pivot tuples with larger internal
	 * page tuple limit (includes page high key).
	 *
	 * Most of the time, a page is only "full" in the sense that the soft
	 * fillfactor-wise limit has been exceeded.  However, we must always leave
	 * at least two items plus a high key on each page before starting a new
	 * page.  Disregard fillfactor and insert on "full" current page if we
	 * don't have the minimum number of items yet.  (Note that we deliberately
	 * assume that suffix truncation neither enlarges nor shrinks new high key
	 * when applying soft limit, except when last tuple has a posting list.)
	 */
	Assert(last_truncextra == 0 || isleaf);
	if (pgspc < itupsz + (isleaf ? MAXALIGN(sizeof(ItemPointerData)) : 0) ||
		(pgspc + last_truncextra < state->btps_full && last_off > P_FIRSTKEY))
	{
		/*
		 * Finish off the page and write it out.
		 */
		Page		opage = npage;
		BlockNumber oblkno = nblkno;
		ItemId		ii;
		ItemId		hii;
		IndexTuple	oitup;

		/* Create new page of same level */
		npage = _bt_blnewpage(state->btps_level);

		/* and assign it a page position */
		nblkno = wstate->btws_pages_alloced++;

		/*
		 * We copy the last item on the page into the new page, and then
		 * rearrange the old page so that the 'last item' becomes its high key
		 * rather than a true data item.  There had better be at least two
		 * items on the page already, else the page would be empty of useful
		 * data.
		 */
		Assert(last_off > P_FIRSTKEY);
		ii = PageGetItemId(opage, last_off);
		oitup = (IndexTuple) PageGetItem(opage, ii);
		_bt_sortaddtup(npage, ItemIdGetLength(ii), oitup, P_FIRSTKEY,
					   !isleaf);

		/*
		 * Move 'last' into the high key position on opage.  _bt_blnewpage()
		 * allocated empty space for a line pointer when opage was first
		 * created, so this is a matter of rearranging already-allocated space
		 * on page, and initializing high key line pointer. (Actually, leaf
		 * pages must also swap oitup with a truncated version of oitup, which
		 * is sometimes larger than oitup, though never by more than the space
		 * needed to append a heap TID.)
		 */
		hii = PageGetItemId(opage, P_HIKEY);
		*hii = *ii;
		ItemIdSetUnused(ii);	/* redundant */
		((PageHeader) opage)->pd_lower -= sizeof(ItemIdData);

		if (isleaf)
		{
			IndexTuple	lastleft;
			IndexTuple	truncated;

			/*
			 * Truncate away any unneeded attributes from high key on leaf
			 * level.  This is only done at the leaf level because downlinks
			 * in internal pages are either negative infinity items, or get
			 * their contents from copying from one level down.  See also:
			 * _bt_split().
			 *
			 * We don't try to bias our choice of split point to make it more
			 * likely that _bt_truncate() can truncate away more attributes,
			 * whereas the split point used within _bt_split() is chosen much
			 * more delicately.  Even still, the lastleft and firstright
			 * tuples passed to _bt_truncate() here are at least not fully
			 * equal to each other when deduplication is used, unless there is
			 * a large group of duplicates (also, unique index builds usually
			 * have few or no spool2 duplicates).  When the split point is
			 * between two unequal tuples, _bt_truncate() will avoid including
			 * a heap TID in the new high key, which is the most important
			 * benefit of suffix truncation.
			 *
			 * Overwrite the old item with new truncated high key directly.
			 * oitup is already located at the physical beginning of tuple
			 * space, so this should directly reuse the existing tuple space.
			 */
			ii = PageGetItemId(opage, OffsetNumberPrev(last_off));
			lastleft = (IndexTuple) PageGetItem(opage, ii);

			Assert(IndexTupleSize(oitup) > last_truncextra);
			truncated = _bt_truncate(wstate->index, lastleft, oitup,
									 wstate->inskey);
			if (!PageIndexTupleOverwrite(opage, P_HIKEY, (Item) truncated,
										 IndexTupleSize(truncated)))
				elog(ERROR, "failed to add high key to the index page");
			pfree(truncated);

			/* oitup should continue to point to the page's high key */
			hii = PageGetItemId(opage, P_HIKEY);
			oitup = (IndexTuple) PageGetItem(opage, hii);
		}

		/*
		 * Link the old page into its parent, using its low key.  If we don't
		 * have a parent, we have to create one; this adds a new btree level.
		 */
		if (state->btps_next == NULL)
			state->btps_next = _bt_pagestate(wstate, state->btps_level + 1);

		Assert((BTreeTupleGetNAtts(state->btps_lowkey, wstate->index) <=
				IndexRelationGetNumberOfKeyAttributes(wstate->index) &&
				BTreeTupleGetNAtts(state->btps_lowkey, wstate->index) > 0) ||
			   P_LEFTMOST((BTPageOpaque) PageGetSpecialPointer(opage)));
		Assert(BTreeTupleGetNAtts(state->btps_lowkey, wstate->index) == 0 ||
			   !P_LEFTMOST((BTPageOpaque) PageGetSpecialPointer(opage)));
		BTreeTupleSetDownLink(state->btps_lowkey, oblkno);
		_bt_buildadd(wstate, state->btps_next, state->btps_lowkey, 0);
		pfree(state->btps_lowkey);

		/*
		 * Save a copy of the high key from the old page.  It is also the low
		 * key for the new page.
		 */
		state->btps_lowkey = CopyIndexTuple(oitup);

		/*
		 * Set the sibling links for both pages.
		 */
		{
			BTPageOpaque oopaque = (BTPageOpaque) PageGetSpecialPointer(opage);
			BTPageOpaque nopaque = (BTPageOpaque) PageGetSpecialPointer(npage);

			oopaque->btpo_next = nblkno;
			nopaque->btpo_prev = oblkno;
			nopaque->btpo_next = P_NONE;	/* redundant */
		}

		/*
		 * Write out the old page.  We never need to touch it again, so we can
		 * free the opage workspace too.
		 */
		_bt_blwritepage(wstate, opage, oblkno);

		/*
		 * Reset last_off to point to new page
		 */
		last_off = P_FIRSTKEY;
	}

	/*
	 * By here, either original page is still the current page, or a new page
	 * was created that became the current page.  Either way, the current page
	 * definitely has space for new item.
	 *
	 * If the new item is the first for its page, it must also be the first
	 * item on its entire level.  On later same-level pages, a low key for a
	 * page will be copied from the prior page in the code above.  Generate a
	 * minus infinity low key here instead.
	 */
	if (last_off == P_HIKEY)
	{
		Assert(state->btps_lowkey == NULL);
		state->btps_lowkey = palloc0(sizeof(IndexTupleData));
		state->btps_lowkey->t_info = sizeof(IndexTupleData);
		BTreeTupleSetNAtts(state->btps_lowkey, 0, false);
	}

	/*
	 * Add the new item into the current page.
	 */
	last_off = OffsetNumberNext(last_off);
	_bt_sortaddtup(npage, itupsz, itup, last_off,
				   !isleaf && last_off == P_FIRSTKEY);

	state->btps_page = npage;
	state->btps_blkno = nblkno;
	state->btps_lastoff = last_off;
}

/*
 * Finalize pending posting list tuple, and add it to the index.  Final tuple
 * is based on saved base tuple, and saved list of heap TIDs.
 *
 * This is almost like _bt_dedup_finish_pending(), but it adds a new tuple
 * using _bt_buildadd().
 */
static void
_bt_sort_dedup_finish_pending(BTWriteState *wstate, BTPageState *state,
							  BTDedupState dstate)
{
	Assert(dstate->nitems > 0);

	if (dstate->nitems == 1)
		_bt_buildadd(wstate, state, dstate->base, 0);
	else
	{
		IndexTuple	postingtuple;
		Size		truncextra;

		/* form a tuple with a posting list */
		postingtuple = _bt_form_posting(dstate->base,
										dstate->htids,
										dstate->nhtids);
		/* Calculate posting list overhead */
		truncextra = IndexTupleSize(postingtuple) -
			BTreeTupleGetPostingOffset(postingtuple);

		_bt_buildadd(wstate, state, postingtuple, truncextra);
		pfree(postingtuple);
	}

	dstate->nmaxitems = 0;
	dstate->nhtids = 0;
	dstate->nitems = 0;
	dstate->phystupsize = 0;
}

/*
 * Finish writing out the completed btree.
 */
static void
_bt_uppershutdown(BTWriteState *wstate, BTPageState *state)
{
	BTPageState *s;
	BlockNumber rootblkno = P_NONE;
	uint32		rootlevel = 0;
	Page		metapage;

	/*
	 * Each iteration of this loop completes one more level of the tree.
	 */
	for (s = state; s != NULL; s = s->btps_next)
	{
		BlockNumber blkno;
		BTPageOpaque opaque;

		blkno = s->btps_blkno;
		opaque = (BTPageOpaque) PageGetSpecialPointer(s->btps_page);

		/*
		 * We have to link the last page on this level to somewhere.
		 *
		 * If we're at the top, it's the root, so attach it to the metapage.
		 * Otherwise, add an entry for it to its parent using its low key.
		 * This may cause the last page of the parent level to split, but
		 * that's not a problem -- we haven't gotten to it yet.
		 */
		if (s->btps_next == NULL)
		{
			opaque->btpo_flags |= BTP_ROOT;
			rootblkno = blkno;
			rootlevel = s->btps_level;
		}
		else
		{
			Assert((BTreeTupleGetNAtts(s->btps_lowkey, wstate->index) <=
					IndexRelationGetNumberOfKeyAttributes(wstate->index) &&
					BTreeTupleGetNAtts(s->btps_lowkey, wstate->index) > 0) ||
				   P_LEFTMOST(opaque));
			Assert(BTreeTupleGetNAtts(s->btps_lowkey, wstate->index) == 0 ||
				   !P_LEFTMOST(opaque));
			BTreeTupleSetDownLink(s->btps_lowkey, blkno);
			_bt_buildadd(wstate, s->btps_next, s->btps_lowkey, 0);
			pfree(s->btps_lowkey);
			s->btps_lowkey = NULL;
		}

		/*
		 * This is the rightmost page, so the ItemId array needs to be slid
		 * back one slot.  Then we can dump out the page.
		 */
		_bt_slideleft(s->btps_page);
		_bt_blwritepage(wstate, s->btps_page, s->btps_blkno);
		s->btps_page = NULL;	/* writepage freed the workspace */
	}

	/*
	 * As the last step in the process, construct the metapage and make it
	 * point to the new root (unless we had no data at all, in which case it's
	 * set to point to "P_NONE").  This changes the index to the "valid" state
	 * by filling in a valid magic number in the metapage.
	 */
	metapage = (Page) palloc(BLCKSZ);
	_bt_initmetapage(metapage, rootblkno, rootlevel,
					 wstate->inskey->allequalimage);
	_bt_blwritepage(wstate, metapage, BTREE_METAPAGE);
}

/*
 * Read tuples in correct sort order from tuplesort, and load them into
 * btree leaves.
 */
static void
_bt_load(BTWriteState *wstate, BTSpool *btspool, BTSpool *btspool2)
{
	BTPageState *state = NULL;
	bool		merge = (btspool2 != NULL);
	IndexTuple	itup,
				itup2 = NULL;
	bool		load1;
	TupleDesc	tupdes = RelationGetDescr(wstate->index);
	int			i,
				keysz = IndexRelationGetNumberOfKeyAttributes(wstate->index);
	SortSupport sortKeys;
	int64		tuples_done = 0;
	bool		deduplicate;

	deduplicate = wstate->inskey->allequalimage && !btspool->isunique;

	if (merge)
	{
		/*
		 * Another BTSpool for dead tuples exists. Now we have to merge
		 * btspool and btspool2.
		 */

		/* the preparation of merge */
		itup = tuplesort_getindextuple(btspool->sortstate, true);
		itup2 = tuplesort_getindextuple(btspool2->sortstate, true);

		/* Prepare SortSupport data for each column */
		sortKeys = (SortSupport) palloc0(keysz * sizeof(SortSupportData));

		for (i = 0; i < keysz; i++)
		{
			SortSupport sortKey = sortKeys + i;
			ScanKey		scanKey = wstate->inskey->scankeys + i;
			int16		strategy;

			sortKey->ssup_cxt = CurrentMemoryContext;
			sortKey->ssup_collation = scanKey->sk_collation;
			sortKey->ssup_nulls_first =
				(scanKey->sk_flags & SK_BT_NULLS_FIRST) != 0;
			sortKey->ssup_attno = scanKey->sk_attno;
			/* Abbreviation is not supported here */
			sortKey->abbreviate = false;

			AssertState(sortKey->ssup_attno != 0);

			strategy = (scanKey->sk_flags & SK_BT_DESC) != 0 ?
				BTGreaterStrategyNumber : BTLessStrategyNumber;

			PrepareSortSupportFromIndexRel(wstate->index, strategy, sortKey);
		}

		for (;;)
		{
			load1 = true;		/* load BTSpool next ? */
			if (itup2 == NULL)
			{
				if (itup == NULL)
					break;
			}
			else if (itup != NULL)
			{
				int32		compare = 0;

				for (i = 1; i <= keysz; i++)
				{
					SortSupport entry;
					Datum		attrDatum1,
								attrDatum2;
					bool		isNull1,
								isNull2;

					entry = sortKeys + i - 1;
					attrDatum1 = index_getattr(itup, i, tupdes, &isNull1);
					attrDatum2 = index_getattr(itup2, i, tupdes, &isNull2);

					compare = ApplySortComparator(attrDatum1, isNull1,
												  attrDatum2, isNull2,
												  entry);
					if (compare > 0)
					{
						load1 = false;
						break;
					}
					else if (compare < 0)
						break;
				}

				/*
				 * If key values are equal, we sort on ItemPointer.  This is
				 * required for btree indexes, since heap TID is treated as an
				 * implicit last key attribute in order to ensure that all
				 * keys in the index are physically unique.
				 */
				if (compare == 0)
				{
					compare = ItemPointerCompare(&itup->t_tid, &itup2->t_tid);
					Assert(compare != 0);
					if (compare > 0)
						load1 = false;
				}
			}
			else
				load1 = false;

			/* When we see first tuple, create first index page */
			if (state == NULL)
				state = _bt_pagestate(wstate, 0);

			if (load1)
			{
				_bt_buildadd(wstate, state, itup, 0);
				itup = tuplesort_getindextuple(btspool->sortstate, true);
			}
			else
			{
				_bt_buildadd(wstate, state, itup2, 0);
				itup2 = tuplesort_getindextuple(btspool2->sortstate, true);
			}

			/* Report progress */
			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
										 ++tuples_done);
		}
		pfree(sortKeys);
	}
	else if (deduplicate)
	{
		/* merge is unnecessary, deduplicate into posting lists */
		BTDedupState dstate;

		dstate = (BTDedupState) palloc(sizeof(BTDedupStateData));
		dstate->deduplicate = true; /* unused */
		dstate->nmaxitems = 0;	/* unused */
		dstate->maxpostingsize = 0; /* set later */
		/* Metadata about base tuple of current pending posting list */
		dstate->base = NULL;
		dstate->baseoff = InvalidOffsetNumber;	/* unused */
		dstate->basetupsize = 0;
		/* Metadata about current pending posting list TIDs */
		dstate->htids = NULL;
		dstate->nhtids = 0;
		dstate->nitems = 0;
		dstate->phystupsize = 0;	/* unused */
		dstate->nintervals = 0; /* unused */

		while ((itup = tuplesort_getindextuple(btspool->sortstate,
											   true)) != NULL)
		{
			/* When we see first tuple, create first index page */
			if (state == NULL)
			{
				state = _bt_pagestate(wstate, 0);

				/*
				 * Limit size of posting list tuples to 1/10 space we want to
				 * leave behind on the page, plus space for final item's line
				 * pointer.  This is equal to the space that we'd like to
				 * leave behind on each leaf page when fillfactor is 90,
				 * allowing us to get close to fillfactor% space utilization
				 * when there happen to be a great many duplicates.  (This
				 * makes higher leaf fillfactor settings ineffective when
				 * building indexes that have many duplicates, but packing
				 * leaf pages full with few very large tuples doesn't seem
				 * like a useful goal.)
				 */
				dstate->maxpostingsize = MAXALIGN_DOWN((BLCKSZ * 10 / 100)) -
					sizeof(ItemIdData);
				Assert(dstate->maxpostingsize <= BTMaxItemSize(state->btps_page) &&
					   dstate->maxpostingsize <= INDEX_SIZE_MASK);
				dstate->htids = palloc(dstate->maxpostingsize);

				/* start new pending posting list with itup copy */
				_bt_dedup_start_pending(dstate, CopyIndexTuple(itup),
										InvalidOffsetNumber);
			}
			else if (_bt_keep_natts_fast(wstate->index, dstate->base,
										 itup) > keysz &&
					 _bt_dedup_save_htid(dstate, itup))
			{
				/*
				 * Tuple is equal to base tuple of pending posting list.  Heap
				 * TID from itup has been saved in state.
				 */
			}
			else
			{
				/*
				 * Tuple is not equal to pending posting list tuple, or
				 * _bt_dedup_save_htid() opted to not merge current item into
				 * pending posting list.
				 */
				_bt_sort_dedup_finish_pending(wstate, state, dstate);
				pfree(dstate->base);

				/* start new pending posting list with itup copy */
				_bt_dedup_start_pending(dstate, CopyIndexTuple(itup),
										InvalidOffsetNumber);
			}

			/* Report progress */
			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
										 ++tuples_done);
		}

		if (state)
		{
			/*
			 * Handle the last item (there must be a last item when the
			 * tuplesort returned one or more tuples)
			 */
			_bt_sort_dedup_finish_pending(wstate, state, dstate);
			pfree(dstate->base);
			pfree(dstate->htids);
		}

		pfree(dstate);
	}
	else
	{
		/* merging and deduplication are both unnecessary */
		while ((itup = tuplesort_getindextuple(btspool->sortstate,
											   true)) != NULL)
		{
			/* When we see first tuple, create first index page */
			if (state == NULL)
				state = _bt_pagestate(wstate, 0);

			_bt_buildadd(wstate, state, itup, 0);

			/* Report progress */
			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
										 ++tuples_done);
		}
	}

	/* Close down final pages and write the metapage */
	_bt_uppershutdown(wstate, state);

	/*
	 * When we WAL-logged index pages, we must nonetheless fsync index files.
	 * Since we're building outside shared buffers, a CHECKPOINT occurring
	 * during the build has no way to flush the previously written data to
	 * disk (indeed it won't know the index even exists).  A crash later on
	 * would replay WAL from the checkpoint, therefore it wouldn't replay our
	 * earlier WAL entries. If we do not fsync those pages here, they might
	 * still not be on disk when the crash occurs.
	 */
	if (wstate->btws_use_wal)
		smgrimmedsync(RelationGetSmgr(wstate->index), MAIN_FORKNUM);
}







