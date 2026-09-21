/*-------------------------------------------------------------------------
 *
 * execIndexing.c
 *	  routines for inserting index tuples and enforcing unique and
 *	  exclusion constraints.
 *
 * ExecInsertIndexTuples() is the main entry point.  It's called after
 * inserting a tuple to the heap, and it inserts corresponding index tuples
 * into all indexes.  At the same time, it enforces any unique and
 * exclusion constraints:
 *
 * Unique Indexes
 * --------------
 *
 * Enforcing a unique constraint is straightforward.  When the index AM
 * inserts the tuple to the index, it also checks that there are no
 * conflicting tuples in the index already.  It does so atomically, so that
 * even if two backends try to insert the same key concurrently, only one
 * of them will succeed.  All the logic to ensure atomicity, and to wait
 * for in-progress transactions to finish, is handled by the index AM.
 *
 * If a unique constraint is deferred, we request the index AM to not
 * throw an error if a conflict is found.  Instead, we make note that there
 * was a conflict and return the list of indexes with conflicts to the
 * caller.  The caller must re-check them later, by calling index_insert()
 * with the UNIQUE_CHECK_EXISTING option.
 *
 * Exclusion Constraints
 * ---------------------
 *
 * Exclusion constraints are different from unique indexes in that when the
 * tuple is inserted to the index, the index AM does not check for
 * duplicate keys at the same time.  After the insertion, we perform a
 * separate scan on the index to check for conflicting tuples, and if one
 * is found, we throw an error and the transaction is aborted.  If the
 * conflicting tuple's inserter or deleter is in-progress, we wait for it
 * to finish first.
 *
 * There is a chance of deadlock, if two backends insert a tuple at the
 * same time, and then perform the scan to check for conflicts.  They will
 * find each other's tuple, and both try to wait for each other.  The
 * deadlock detector will detect that, and abort one of the transactions.
 * That's fairly harmless, as one of them was bound to abort with a
 * "duplicate key error" anyway, although you get a different error
 * message.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execIndexing.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "catalog/pg_collation.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"

/* ----------------------------------------------------------------
 *		ExecOpenIndices
 *
 *		Find the indices associated with a result relation, open them,
 *		and save information about them in the result ResultRelInfo.
 *
 *		At entry, caller has already opened and locked
 *		resultRelInfo->ri_RelationDesc.
 * ----------------------------------------------------------------
 */
void
ExecOpenIndices(ResultRelInfo *resultRelInfo)
{
	Relation	resultRelation = resultRelInfo->ri_RelationDesc;
	List	   *indexoidlist;
	ListCell   *l;
	int			len,
				i;
	RelationPtr relationDescs;
	IndexInfo **indexInfoArray;

	resultRelInfo->ri_NumIndices = 0;

	/* fast path if no indexes */
	if (!RelationGetForm(resultRelation)->relhasindex)
		return;

	/*
	 * Get cached list of index OIDs
	 */
	indexoidlist = RelationGetIndexList(resultRelation);
	len = list_length(indexoidlist);
	if (len == 0)
		return;

	/*
	 * allocate space for result arrays
	 */
	relationDescs = (RelationPtr) palloc(len * sizeof(Relation));
	indexInfoArray = (IndexInfo **) palloc(len * sizeof(IndexInfo *));

	resultRelInfo->ri_NumIndices = len;
	resultRelInfo->ri_IndexRelationDescs = relationDescs;
	resultRelInfo->ri_IndexRelationInfo = indexInfoArray;

	/*
	 * For each index, open the index relation and save pg_index info. We
	 * acquire RowExclusiveLock, signifying we will update the index.
	 *
	 * Note: we do this even if the index is not indisready; it's not worth
	 * the trouble to optimize for the case where it isn't.
	 */
	i = 0;
	foreach(l, indexoidlist)
	{
		Oid			indexOid = lfirst_oid(l);
		Relation	indexDesc;
		IndexInfo  *ii;

		indexDesc = index_open(indexOid, RowExclusiveLock);

		/* extract index key information from the index's pg_index info */
		ii = BuildIndexInfo(indexDesc);

		relationDescs[i] = indexDesc;
		indexInfoArray[i] = ii;
		i++;
	}

	list_free(indexoidlist);
}

/* ----------------------------------------------------------------
 *		ExecCloseIndices
 *
 *		Close the index relations stored in resultRelInfo
 * ----------------------------------------------------------------
 */
void
ExecCloseIndices(ResultRelInfo *resultRelInfo)
{
	int			i;
	int			numIndices;
	RelationPtr indexDescs;

	numIndices = resultRelInfo->ri_NumIndices;
	indexDescs = resultRelInfo->ri_IndexRelationDescs;

	for (i = 0; i < numIndices; i++)
	{
		if (indexDescs[i] == NULL)
			continue;			/* shouldn't happen? */

		/* Drop lock acquired by ExecOpenIndices */
		index_close(indexDescs[i], RowExclusiveLock);
	}

	/*
	 * XXX should free indexInfo array here too?  Currently we assume that
	 * such stuff will be cleaned up automatically in FreeExecutorState.
	 */
}

/* ----------------------------------------------------------------
 *		ExecInsertIndexTuples
 *
 *		This routine takes care of inserting index tuples
 *		into all the relations indexing the result relation
 *		when a heap tuple is inserted into the result relation.
 *
 *		When 'update' is true, executor is performing an UPDATE
 *		that could not use an optimization like heapam's HOT (in
 *		more general terms a call to table_tuple_update() took
 *		place and set 'update_indexes' to true).  Receiving this
 *		hint makes us consider if we should pass down the
 *		'indexUnchanged' hint in turn.  That's something that we
 *		figure out for each index_insert() call iff 'update' is
 *		true.  (When 'update' is false we already know not to pass
 *		the hint to any index.)
 *
 *		Unique and exclusion constraints are enforced at the same
 *		time.  This returns a list of index OIDs for any unique or
 *		exclusion constraints that are deferred and that had
 *		potential (unconfirmed) conflicts.
 *
 * ----------------------------------------------------------------
 */
List *
ExecInsertIndexTuples(ResultRelInfo *resultRelInfo,
					  TupleTableSlot *slot,
					  EState *estate,
					  bool update)
{
	ItemPointer tupleid = &slot->tts_tid;
	List	   *result = NIL;
	int			i;
	int			numIndices;
	RelationPtr relationDescs;
	Relation	heapRelation;
	IndexInfo **indexInfoArray;
	ExprContext *econtext;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	Assert(ItemPointerIsValid(tupleid));

	/*
	 * Get information from the result relation info structure.
	 */
	numIndices = resultRelInfo->ri_NumIndices;
	relationDescs = resultRelInfo->ri_IndexRelationDescs;
	indexInfoArray = resultRelInfo->ri_IndexRelationInfo;
	heapRelation = resultRelInfo->ri_RelationDesc;

	/* Sanity check: slot must belong to the same rel as the resultRelInfo. */
	Assert(slot->tts_tableOid == RelationGetRelid(heapRelation));

	/*
	 * We will use the EState's per-tuple context for evaluating predicates
	 * and index expressions (creating it if it's not already there).
	 */
	econtext = GetPerTupleExprContext(estate);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/*
	 * for each index, form and insert the index tuple
	 */
	for (i = 0; i < numIndices; i++)
	{
		Relation	indexRelation = relationDescs[i];
		IndexInfo  *indexInfo;
		IndexUniqueCheck checkUnique;
		bool		indexUnchanged;

		if (indexRelation == NULL)
			continue;

		indexInfo = indexInfoArray[i];

		/* If the index is marked as read-only, ignore it */
		if (!indexInfo->ii_ReadyForInserts)
			continue;

		/* Check for partial index */
		if (indexInfo->ii_Predicate != NIL)
		{
			ExprState  *predicate;

			/*
			 * If predicate state not set up yet, create it (in the estate's
			 * per-query context)
			 */
			predicate = indexInfo->ii_PredicateState;
			if (predicate == NULL)
			{
				predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);
				indexInfo->ii_PredicateState = predicate;
			}

			/* Skip this index-update if the predicate isn't satisfied */
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * FormIndexDatum fills in its values and isnull parameters with the
		 * appropriate values for the column(s) of the index.
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		/*
		 * The index AM does the actual insertion, plus uniqueness checking.
		 *
		 * For an immediate-mode unique index, we just tell the index AM to
		 * throw error if not unique.
		 */
		if (!indexRelation->rd_index->indisunique)
			checkUnique = UNIQUE_CHECK_NO;
		else
			checkUnique = UNIQUE_CHECK_YES;

		/*
		 * There's definitely going to be an index_insert() call for this
		 * index.  If we're being called as part of an UPDATE statement,
		 * consider if the 'indexUnchanged' = true hint should be passed.
		 *
		 * XXX We always assume that the hint should be passed for an UPDATE.
		 * This is a workaround for a bug in PostgreSQL 14.  In practice this
		 * won't make much difference for current users of the hint.
		 */
		indexUnchanged = update;

		(void) index_insert(indexRelation,	/* index relation */
							values, /* array of index Datums */
							isnull, /* null flags */
							tupleid,	/* tid of heap tuple */
							heapRelation,	/* heap relation */
							checkUnique,	/* type of uniqueness check to do */
							indexUnchanged, /* UPDATE without logical change? */
							indexInfo); /* index AM may need this */
	}

	return result;
}

