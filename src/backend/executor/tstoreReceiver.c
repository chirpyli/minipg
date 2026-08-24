/*-------------------------------------------------------------------------
 *
 * tstoreReceiver.c
 *	  An implementation of DestReceiver that stores the result tuples in
 *	  a Tuplestore.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/tstoreReceiver.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/tstoreReceiver.h"


typedef struct
{
	DestReceiver pub;
	/* parameters: */
	Tuplestorestate *tstore;	/* where to put the data */
} TStoreState;


/*
 * Prepare to receive tuples from executor.
 */
static void
tstoreStartupReceiver(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	/* nothing to do */
}

/*
 * Receive a tuple from the executor and store it in the tuplestore.
 */
static bool
tstoreReceiveSlot(TupleTableSlot *slot, DestReceiver *self)
{
	TStoreState *myState = (TStoreState *) self;

	tuplestore_puttupleslot(myState->tstore, slot);

	return true;
}

/*
 * Clean up at end of an executor run
 */
static void
tstoreShutdownReceiver(DestReceiver *self)
{
	/* nothing to do */
}

/*
 * Destroy receiver when done with it
 */
static void
tstoreDestroyReceiver(DestReceiver *self)
{
	pfree(self);
}

/*
 * Initially create a DestReceiver object.
 */
DestReceiver *
CreateTuplestoreDestReceiver(void)
{
	TStoreState *self = (TStoreState *) palloc0(sizeof(TStoreState));

	self->pub.receiveSlot = tstoreReceiveSlot;
	self->pub.rStartup = tstoreStartupReceiver;
	self->pub.rShutdown = tstoreShutdownReceiver;
	self->pub.rDestroy = tstoreDestroyReceiver;
	self->pub.mydest = DestTuplestore;

	/* private fields will be set by SetTuplestoreDestReceiverParams */

	return (DestReceiver *) self;
}

/*
 * Set parameters for a TuplestoreDestReceiver
 *
 * tStore: where to store the tuples
 */
void
SetTuplestoreDestReceiverParams(DestReceiver *self,
								Tuplestorestate *tStore)
{
	TStoreState *myState = (TStoreState *) self;

	Assert(myState->pub.mydest == DestTuplestore);
	myState->tstore = tStore;
}
