/*-------------------------------------------------------------------------
 *
 * slotfuncs.c
 *	   Support functions for replication slots
 *
 * Copyright (c) 2012-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/slotfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xlog_internal.h"
#include "access/xlogutils.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/slot.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/pg_lsn.h"
#include "utils/resowner.h"

static void
check_permissions(void)
{
	if (!superuser() && !has_rolreplication(GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser or replication role to use replication slots")));
}

/*
 * Helper function for creating a new physical replication slot with
 * given arguments. Note that this function doesn't release the created
 * slot.
 *
 * If restart_lsn is a valid value, we use it without WAL reservation
 * routine. So the caller must guarantee that WAL is available.
 */
static void
create_physical_replication_slot(char *name, bool immediately_reserve,
								 bool temporary, XLogRecPtr restart_lsn)
{
	Assert(!MyReplicationSlot);

	/* acquire replication slot, this will check for conflicting names */
	ReplicationSlotCreate(name, false,
						  temporary ? RS_TEMPORARY : RS_PERSISTENT, false);

	if (immediately_reserve)
	{
		/* Reserve WAL as the user asked for it */
		if (XLogRecPtrIsInvalid(restart_lsn))
			ReplicationSlotReserveWal();
		else
			MyReplicationSlot->data.restart_lsn = restart_lsn;

		/* Write this slot to disk */
		ReplicationSlotMarkDirty();
		ReplicationSlotSave();
	}
}

/*
 * SQL function for creating a new physical (streaming replication)
 * replication slot.
 */
Datum
pg_create_physical_replication_slot(PG_FUNCTION_ARGS)
{
	Name		name = PG_GETARG_NAME(0);
	bool		immediately_reserve = PG_GETARG_BOOL(1);
	bool		temporary = PG_GETARG_BOOL(2);
	Datum		values[2];
	bool		nulls[2];
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	Datum		result;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	check_permissions();

	CheckSlotRequirements();

	create_physical_replication_slot(NameStr(*name),
									 immediately_reserve,
									 temporary,
									 InvalidXLogRecPtr);

	values[0] = NameGetDatum(&MyReplicationSlot->data.name);
	nulls[0] = false;

	if (immediately_reserve)
	{
		values[1] = LSNGetDatum(MyReplicationSlot->data.restart_lsn);
		nulls[1] = false;
	}
	else
		nulls[1] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	ReplicationSlotRelease();

	PG_RETURN_DATUM(result);
}


/*
 * SQL function for dropping a replication slot.
 */
Datum
pg_drop_replication_slot(PG_FUNCTION_ARGS)
{
	Name		name = PG_GETARG_NAME(0);

	check_permissions();

	CheckSlotRequirements();

	ReplicationSlotDrop(NameStr(*name), true);

	PG_RETURN_VOID();
}

/*
 * pg_get_replication_slots - SQL SRF showing active replication slots.
 */
Datum
pg_get_replication_slots(PG_FUNCTION_ARGS)
{
#define PG_GET_REPLICATION_SLOTS_COLS 14
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	XLogRecPtr	currlsn;
	int			slotno;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/*
	 * We don't require any special permission to see this function's data
	 * because nothing should be sensitive. The most critical being the slot
	 * name, which shouldn't contain anything particularly sensitive.
	 */

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	currlsn = GetXLogWriteRecPtr();

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (slotno = 0; slotno < max_replication_slots; slotno++)
	{
		ReplicationSlot *slot = &ReplicationSlotCtl->replication_slots[slotno];
		ReplicationSlot slot_contents;
		Datum		values[PG_GET_REPLICATION_SLOTS_COLS];
		bool		nulls[PG_GET_REPLICATION_SLOTS_COLS];
		WALAvailability walstate;
		int			i;

		if (!slot->in_use)
			continue;

		/* Copy slot contents while holding spinlock, then examine at leisure */
		SpinLockAcquire(&slot->mutex);
		slot_contents = *slot;
		SpinLockRelease(&slot->mutex);

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		i = 0;
		values[i++] = NameGetDatum(&slot_contents.data.name);

		if (slot_contents.data.database == InvalidOid)
			nulls[i++] = true;
		else
			values[i++] = NameGetDatum(&slot_contents.data.plugin);

		if (slot_contents.data.database == InvalidOid)
			values[i++] = CStringGetTextDatum("physical");
		else
			values[i++] = CStringGetTextDatum("logical");

		if (slot_contents.data.database == InvalidOid)
			nulls[i++] = true;
		else
			values[i++] = ObjectIdGetDatum(slot_contents.data.database);

		values[i++] = BoolGetDatum(slot_contents.data.persistency == RS_TEMPORARY);
		values[i++] = BoolGetDatum(slot_contents.active_pid != 0);

		if (slot_contents.active_pid != 0)
			values[i++] = Int32GetDatum(slot_contents.active_pid);
		else
			nulls[i++] = true;

		if (slot_contents.data.xmin != InvalidTransactionId)
			values[i++] = TransactionIdGetDatum(slot_contents.data.xmin);
		else
			nulls[i++] = true;

		if (slot_contents.data.catalog_xmin != InvalidTransactionId)
			values[i++] = TransactionIdGetDatum(slot_contents.data.catalog_xmin);
		else
			nulls[i++] = true;

		if (slot_contents.data.restart_lsn != InvalidXLogRecPtr)
			values[i++] = LSNGetDatum(slot_contents.data.restart_lsn);
		else
			nulls[i++] = true;

		if (slot_contents.data.confirmed_flush != InvalidXLogRecPtr)
			values[i++] = LSNGetDatum(slot_contents.data.confirmed_flush);
		else
			nulls[i++] = true;

		/*
		 * If invalidated_at is valid and restart_lsn is invalid, we know for
		 * certain that the slot has been invalidated.  Otherwise, test
		 * availability from restart_lsn.
		 */
		if (XLogRecPtrIsInvalid(slot_contents.data.restart_lsn) &&
			!XLogRecPtrIsInvalid(slot_contents.data.invalidated_at))
			walstate = WALAVAIL_REMOVED;
		else
			walstate = GetWALAvailability(slot_contents.data.restart_lsn);

		switch (walstate)
		{
			case WALAVAIL_INVALID_LSN:
				nulls[i++] = true;
				break;

			case WALAVAIL_RESERVED:
				values[i++] = CStringGetTextDatum("reserved");
				break;

			case WALAVAIL_EXTENDED:
				values[i++] = CStringGetTextDatum("extended");
				break;

			case WALAVAIL_UNRESERVED:
				values[i++] = CStringGetTextDatum("unreserved");
				break;

			case WALAVAIL_REMOVED:

				/*
				 * If we read the restart_lsn long enough ago, maybe that file
				 * has been removed by now.  However, the walsender could have
				 * moved forward enough that it jumped to another file after
				 * we looked.  If checkpointer signalled the process to
				 * termination, then it's definitely lost; but if a process is
				 * still alive, then "unreserved" seems more appropriate.
				 *
				 * If we do change it, save the state for safe_wal_size below.
				 */
				if (!XLogRecPtrIsInvalid(slot_contents.data.restart_lsn))
				{
					int			pid;

					SpinLockAcquire(&slot->mutex);
					pid = slot->active_pid;
					slot_contents.data.restart_lsn = slot->data.restart_lsn;
					SpinLockRelease(&slot->mutex);
					if (pid != 0)
					{
						values[i++] = CStringGetTextDatum("unreserved");
						walstate = WALAVAIL_UNRESERVED;
						break;
					}
				}
				values[i++] = CStringGetTextDatum("lost");
				break;
		}

		/*
		 * safe_wal_size is only computed for slots that have not been lost,
		 * and only if there's a configured maximum size.
		 */
		if (walstate == WALAVAIL_REMOVED || max_slot_wal_keep_size_mb < 0)
			nulls[i++] = true;
		else
		{
			XLogSegNo	targetSeg;
			uint64		slotKeepSegs;
			uint64		keepSegs;
			XLogSegNo	failSeg;
			XLogRecPtr	failLSN;

			XLByteToSeg(slot_contents.data.restart_lsn, targetSeg, wal_segment_size);

			/* determine how many segments slots can be kept by slots */
			slotKeepSegs = XLogMBVarToSegs(max_slot_wal_keep_size_mb, wal_segment_size);
			/* ditto for wal_keep_size */
			keepSegs = XLogMBVarToSegs(wal_keep_size_mb, wal_segment_size);

			/* if currpos reaches failLSN, we lose our segment */
			failSeg = targetSeg + Max(slotKeepSegs, keepSegs) + 1;
			XLogSegNoOffsetToRecPtr(failSeg, 0, wal_segment_size, failLSN);

			values[i++] = Int64GetDatum(failLSN - currlsn);
		}

		values[i++] = BoolGetDatum(slot_contents.data.two_phase);

		Assert(i == PG_GET_REPLICATION_SLOTS_COLS);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	LWLockRelease(ReplicationSlotControlLock);

	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 * Helper function for advancing our physical replication slot forward.
 *
 * The LSN position to move to is compared simply to the slot's restart_lsn,
 * knowing that any position older than that would be removed by successive
 * checkpoints.
 */
static XLogRecPtr
pg_physical_replication_slot_advance(XLogRecPtr moveto)
{
	XLogRecPtr	startlsn = MyReplicationSlot->data.restart_lsn;
	XLogRecPtr	retlsn = startlsn;

	Assert(moveto != InvalidXLogRecPtr);

	if (startlsn < moveto)
	{
		SpinLockAcquire(&MyReplicationSlot->mutex);
		MyReplicationSlot->data.restart_lsn = moveto;
		SpinLockRelease(&MyReplicationSlot->mutex);
		retlsn = moveto;

		/*
		 * Dirty the slot so as it is written out at the next checkpoint. Note
		 * that the LSN position advanced may still be lost in the event of a
		 * crash, but this makes the data consistent after a clean shutdown.
		 */
		ReplicationSlotMarkDirty();
	}

	return retlsn;
}

/*
 * SQL function for moving the position in a replication slot.
 */
Datum
pg_replication_slot_advance(PG_FUNCTION_ARGS)
{
	Name		slotname = PG_GETARG_NAME(0);
	XLogRecPtr	moveto = PG_GETARG_LSN(1);
	XLogRecPtr	endlsn;
	XLogRecPtr	minlsn;
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2];
	HeapTuple	tuple;
	Datum		result;

	Assert(!MyReplicationSlot);

	check_permissions();

	if (XLogRecPtrIsInvalid(moveto))
		ereport(ERROR,
				(errmsg("invalid target WAL LSN")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/*
	 * We can't move slot past what's been flushed/replayed so clamp the
	 * target position accordingly.
	 */
	if (!RecoveryInProgress())
		moveto = Min(moveto, GetFlushRecPtr());
	else
		moveto = Min(moveto, GetXLogReplayRecPtr(&ThisTimeLineID));

	/* Acquire the slot so we "own" it */
	ReplicationSlotAcquire(NameStr(*slotname), true);

	/* A slot whose restart_lsn has never been reserved cannot be advanced */
	if (XLogRecPtrIsInvalid(MyReplicationSlot->data.restart_lsn))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("replication slot \"%s\" cannot be advanced",
						NameStr(*slotname)),
				 errdetail("This slot has never previously reserved WAL, or it has been invalidated.")));

	/*
	 * Check if the slot is not moving backwards.  Physical slots rely simply
	 * on restart_lsn as a minimum point, meaning that data older than that is
	 * not available anymore.
	 */
	minlsn = MyReplicationSlot->data.restart_lsn;

	if (moveto < minlsn)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot advance replication slot to %X/%X, minimum is %X/%X",
						LSN_FORMAT_ARGS(moveto), LSN_FORMAT_ARGS(minlsn))));

	/* Do the actual slot update */
	endlsn = pg_physical_replication_slot_advance(moveto);

	values[0] = NameGetDatum(&MyReplicationSlot->data.name);
	nulls[0] = false;

	/*
	 * Recompute the minimum LSN and xmin across all slots to adjust with the
	 * advancing potentially done.
	 */
	ReplicationSlotsComputeRequiredXmin(false);
	ReplicationSlotsComputeRequiredLSN();

	ReplicationSlotRelease();

	/* Return the reached position. */
	values[1] = LSNGetDatum(endlsn);
	nulls[1] = false;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}

/*
 * Helper function of copying a replication slot.
 */
static Datum
copy_replication_slot(FunctionCallInfo fcinfo)
{
	Name		src_name = PG_GETARG_NAME(0);
	Name		dst_name = PG_GETARG_NAME(1);
	ReplicationSlot *src = NULL;
	ReplicationSlot first_slot_contents;
	ReplicationSlot second_slot_contents;
	XLogRecPtr	src_restart_lsn;
	bool		temporary;
	Datum		values[2];
	bool		nulls[2];
	Datum		result;
	TupleDesc	tupdesc;
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	check_permissions();

	CheckSlotRequirements();

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	/*
	 * We need to prevent the source slot's reserved WAL from being removed,
	 * but we don't want to lock that slot for very long, and it can advance
	 * in the meantime.  So obtain the source slot's data, and create a new
	 * slot using its restart_lsn.  Afterwards we lock the source slot again
	 * and verify that the data we copied (name, type) has not changed
	 * incompatibly.  No inconvenient WAL removal can occur once the new slot
	 * is created -- but since WAL removal could have occurred before we
	 * managed to create the new slot, we advance the new slot's restart_lsn
	 * to the source slot's updated restart_lsn the second time we lock it.
	 */
	for (int i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (s->in_use && strcmp(NameStr(s->data.name), NameStr(*src_name)) == 0)
		{
			/* Copy the slot contents while holding spinlock */
			SpinLockAcquire(&s->mutex);
			first_slot_contents = *s;
			SpinLockRelease(&s->mutex);
			src = s;
			break;
		}
	}

	LWLockRelease(ReplicationSlotControlLock);

	if (src == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("replication slot \"%s\" does not exist", NameStr(*src_name))));

	src_restart_lsn = first_slot_contents.data.restart_lsn;
	temporary = (first_slot_contents.data.persistency == RS_TEMPORARY);

	/* Copying non-reserved slot doesn't make sense */
	if (XLogRecPtrIsInvalid(src_restart_lsn))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot copy a replication slot that doesn't reserve WAL")));

	/* Overwrite params from optional arguments */
	if (PG_NARGS() >= 3)
		temporary = PG_GETARG_BOOL(2);

	/* Create new physical slot and acquire it */
	create_physical_replication_slot(NameStr(*dst_name),
									 true,
									 temporary,
									 src_restart_lsn);

	/*
	 * Update the destination slot to current values of the source slot;
	 * recheck that the source slot is still the one we saw previously.
	 */
	{
		TransactionId copy_effective_xmin;
		TransactionId copy_effective_catalog_xmin;
		TransactionId copy_xmin;
		TransactionId copy_catalog_xmin;
		XLogRecPtr	copy_restart_lsn;
		XLogRecPtr	copy_confirmed_flush;
		char	   *copy_name;

		/* Copy data of source slot again */
		SpinLockAcquire(&src->mutex);
		second_slot_contents = *src;
		SpinLockRelease(&src->mutex);

		copy_effective_xmin = second_slot_contents.effective_xmin;
		copy_effective_catalog_xmin = second_slot_contents.effective_catalog_xmin;

		copy_xmin = second_slot_contents.data.xmin;
		copy_catalog_xmin = second_slot_contents.data.catalog_xmin;
		copy_restart_lsn = second_slot_contents.data.restart_lsn;
		copy_confirmed_flush = second_slot_contents.data.confirmed_flush;

		/* for existence check */
		copy_name = NameStr(second_slot_contents.data.name);

		/*
		 * Check if the source slot still exists and is valid. We regard it as
		 * invalid if the name has been changed, or the restart_lsn either is
		 * invalid or has gone backward.
		 */
		if (copy_restart_lsn < src_restart_lsn ||
			strcmp(copy_name, NameStr(*src_name)) != 0)
			ereport(ERROR,
					(errmsg("could not copy replication slot \"%s\"",
							NameStr(*src_name)),
					 errdetail("The source replication slot was modified incompatibly during the copy operation.")));

		/* Install copied values again */
		SpinLockAcquire(&MyReplicationSlot->mutex);
		MyReplicationSlot->effective_xmin = copy_effective_xmin;
		MyReplicationSlot->effective_catalog_xmin = copy_effective_catalog_xmin;

		MyReplicationSlot->data.xmin = copy_xmin;
		MyReplicationSlot->data.catalog_xmin = copy_catalog_xmin;
		MyReplicationSlot->data.restart_lsn = copy_restart_lsn;
		MyReplicationSlot->data.confirmed_flush = copy_confirmed_flush;
		SpinLockRelease(&MyReplicationSlot->mutex);

		ReplicationSlotMarkDirty();
		ReplicationSlotsComputeRequiredXmin(false);
		ReplicationSlotsComputeRequiredLSN();
		ReplicationSlotSave();

#ifdef USE_ASSERT_CHECKING
		/* Check that the restart_lsn is available */
		{
			XLogSegNo	segno;

			XLByteToSeg(copy_restart_lsn, segno, wal_segment_size);
			Assert(XLogGetLastRemovedSegno() < segno);
		}
#endif
	}

	/* All done.  Set up the return values */
	values[0] = NameGetDatum(dst_name);
	nulls[0] = false;
	if (!XLogRecPtrIsInvalid(MyReplicationSlot->data.confirmed_flush))
	{
		values[1] = LSNGetDatum(MyReplicationSlot->data.confirmed_flush);
		nulls[1] = false;
	}
	else
		nulls[1] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	ReplicationSlotRelease();

	PG_RETURN_DATUM(result);
}

Datum
pg_copy_physical_replication_slot_a(PG_FUNCTION_ARGS)
{
	return copy_replication_slot(fcinfo);
}

Datum
pg_copy_physical_replication_slot_b(PG_FUNCTION_ARGS)
{
	return copy_replication_slot(fcinfo);
}
