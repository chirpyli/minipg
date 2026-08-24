/*-------------------------------------------------------------------------
 *
 * portalmem.c
 *	  backend portal memory management
 *
 * Portals are objects representing the execution state of a query.
 * This module provides memory management services for portals, but it
 * doesn't actually run the executor for them.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/portalmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/portalcmds.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

/*
 * Estimate of the maximum number of open portals a user would have,
 * used in initially sizing the PortalHashTable in EnablePortalManager().
 * Since the hash table can expand, there's no need to make this overly
 * generous, and keeping it small avoids unnecessary overhead in the
 * hash_seq_search() calls executed during transaction end.
 */
#define PORTALS_PER_USER	   16


/* ----------------
 *		Global state
 * ----------------
 */

#define MAX_PORTALNAME_LEN		NAMEDATALEN

typedef struct portalhashent
{
	char		portalname[MAX_PORTALNAME_LEN];
	Portal		portal;
} PortalHashEnt;

static HTAB *PortalHashTable = NULL;

#define PortalHashTableLookup(NAME, PORTAL) \
do { \
	PortalHashEnt *hentry; \
	\
	hentry = (PortalHashEnt *) hash_search(PortalHashTable, \
										   (NAME), HASH_FIND, NULL); \
	if (hentry) \
		PORTAL = hentry->portal; \
	else \
		PORTAL = NULL; \
} while(0)

#define PortalHashTableInsert(PORTAL, NAME) \
do { \
	PortalHashEnt *hentry; bool found; \
	\
	hentry = (PortalHashEnt *) hash_search(PortalHashTable, \
										   (NAME), HASH_ENTER, &found); \
	if (found) \
		elog(ERROR, "duplicate portal name"); \
	hentry->portal = PORTAL; \
	/* To avoid duplicate storage, make PORTAL->name point to htab entry */ \
	PORTAL->name = hentry->portalname; \
} while(0)

#define PortalHashTableDelete(PORTAL) \
do { \
	PortalHashEnt *hentry; \
	\
	hentry = (PortalHashEnt *) hash_search(PortalHashTable, \
										   PORTAL->name, HASH_REMOVE, NULL); \
	if (hentry == NULL) \
		elog(WARNING, "trying to delete portal name that does not exist"); \
} while(0)

static MemoryContext TopPortalContext = NULL;


/* ----------------------------------------------------------------
 *				   public portal interface functions
 * ----------------------------------------------------------------
 */

/*
 * EnablePortalManager
 *		Enables the portal management module at backend startup.
 */
void
EnablePortalManager(void)
{
	HASHCTL		ctl;

	Assert(TopPortalContext == NULL);

	TopPortalContext = AllocSetContextCreate(TopMemoryContext,
											 "TopPortalContext",
											 ALLOCSET_DEFAULT_SIZES);

	ctl.keysize = MAX_PORTALNAME_LEN;
	ctl.entrysize = sizeof(PortalHashEnt);

	/*
	 * use PORTALS_PER_USER as a guess of how many hash table entries to
	 * create, initially
	 */
	PortalHashTable = hash_create("Portal hash", PORTALS_PER_USER,
								  &ctl, HASH_ELEM | HASH_STRINGS);
}

/*
 * GetPortalByName
 *		Returns a portal given a portal name, or NULL if name not found.
 */
Portal
GetPortalByName(const char *name)
{
	Portal		portal;

	if (PointerIsValid(name))
		PortalHashTableLookup(name, portal);
	else
		portal = NULL;

	return portal;
}

/*
 * PortalGetPrimaryStmt
 *		Get the "primary" stmt within a portal, ie, the one marked canSetTag.
 *
 * Returns NULL if no such stmt.  If multiple PlannedStmt structs within the
 * portal are marked canSetTag, returns the first one.  Neither of these
 * cases should occur in present usages of this function.
 */
PlannedStmt *
PortalGetPrimaryStmt(Portal portal)
{
	ListCell   *lc;

	foreach(lc, portal->stmts)
	{
		PlannedStmt *stmt = lfirst_node(PlannedStmt, lc);

		if (stmt->canSetTag)
			return stmt;
	}
	return NULL;
}

/*
 * CreatePortal
 *		Returns a new portal given a name.
 *
 * allowDup: if true, automatically drop any pre-existing portal of the
 * same name (if false, an error is raised).
 *
 * dupSilent: if true, don't even emit a WARNING.
 */
Portal
CreatePortal(const char *name, bool allowDup, bool dupSilent)
{
	Portal		portal;

	AssertArg(PointerIsValid(name));

	portal = GetPortalByName(name);
	if (PortalIsValid(portal))
	{
		if (!allowDup)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_CURSOR),
					 errmsg("portal \"%s\" already exists", name)));
		if (!dupSilent)
			ereport(WARNING,
					(errcode(ERRCODE_DUPLICATE_CURSOR),
					 errmsg("closing existing portal \"%s\"",
							name)));
		PortalDrop(portal, false);
	}

	/* make new portal structure */
	portal = (Portal) MemoryContextAllocZero(TopPortalContext, sizeof *portal);

	/* initialize portal context; typically it won't store much */
	portal->portalContext = AllocSetContextCreate(TopPortalContext,
												  "PortalContext",
												  ALLOCSET_SMALL_SIZES);

	/* create a resource owner for the portal */
	portal->resowner = ResourceOwnerCreate(CurTransactionResourceOwner,
										   "Portal");

	/* initialize portal fields that don't start off zero */
	portal->status = PORTAL_NEW;
	portal->cleanup = PortalCleanup;
	portal->createSubid = GetCurrentSubTransactionId();
	portal->activeSubid = portal->createSubid;
	portal->createLevel = GetCurrentTransactionNestLevel();
	portal->strategy = PORTAL_MULTI_QUERY;
	portal->atStart = true;
	portal->atEnd = true;		/* disallow fetches until query is set */

	/* put portal in table (sets portal->name) */
	PortalHashTableInsert(portal, name);

	/* for named portals reuse portal->name copy */
	MemoryContextSetIdentifier(portal->portalContext, portal->name[0] ? portal->name : "<unnamed>");

	return portal;
}

/*
 * CreateNewPortal
 *		Create a new portal, assigning it a random nonconflicting name.
 */
Portal
CreateNewPortal(void)
{
	static unsigned int unnamed_portal_count = 0;

	char		portalname[MAX_PORTALNAME_LEN];

	/* Select a nonconflicting name */
	for (;;)
	{
		unnamed_portal_count++;
		sprintf(portalname, "<unnamed portal %u>", unnamed_portal_count);
		if (GetPortalByName(portalname) == NULL)
			break;
	}

	return CreatePortal(portalname, false, false);
}

/*
 * PortalDefineQuery
 *		A simple subroutine to establish a portal's query.
 *
 * Notes: as of PG 8.4, caller MUST supply a sourceText string; it is not
 * allowed anymore to pass NULL.  (If you really don't have source text,
 * you can pass a constant string, perhaps "(query not available)".)
 *
 * commandTag shall be NULL if and only if the original query string
 * (before rewriting) was an empty string.  Also, the passed commandTag must
 * be a pointer to a constant string, since it is not copied.
 *
 * If cplan is provided, then it is a cached plan containing the stmts, and
 * the caller must have done GetCachedPlan(), causing a refcount increment.
 * The refcount will be released when the portal is destroyed.
 *
 * If cplan is NULL, then it is the caller's responsibility to ensure that
 * the passed plan trees have adequate lifetime.  Typically this is done by
 * copying them into the portal's context.
 *
 * The caller is also responsible for ensuring that the passed prepStmtName
 * (if not NULL) and sourceText have adequate lifetime.
 *
 * NB: this function mustn't do much beyond storing the passed values; in
 * particular don't do anything that risks elog(ERROR).  If that were to
 * happen here before storing the cplan reference, we'd leak the plancache
 * refcount that the caller is trying to hand off to us.
 */
void
PortalDefineQuery(Portal portal,
				  const char *prepStmtName,
				  const char *sourceText,
				  CommandTag commandTag,
				  List *stmts,
				  CachedPlan *cplan)
{
	AssertArg(PortalIsValid(portal));
	AssertState(portal->status == PORTAL_NEW);

	AssertArg(sourceText != NULL);
	AssertArg(commandTag != CMDTAG_UNKNOWN || stmts == NIL);

	portal->prepStmtName = prepStmtName;
	portal->sourceText = sourceText;
	portal->qc.commandTag = commandTag;
	portal->qc.nprocessed = 0;
	portal->commandTag = commandTag;
	portal->stmts = stmts;
	portal->cplan = cplan;
	portal->status = PORTAL_DEFINED;
}

/*
 * PortalReleaseCachedPlan
 *		Release a portal's reference to its cached plan, if any.
 */
static void
PortalReleaseCachedPlan(Portal portal)
{
	if (portal->cplan)
	{
		ReleaseCachedPlan(portal->cplan, NULL);
		portal->cplan = NULL;

		/*
		 * We must also clear portal->stmts which is now a dangling reference
		 * to the cached plan's plan list.  This protects any code that might
		 * try to examine the Portal later.
		 */
		portal->stmts = NIL;
	}
}

/*
 * PortalCreateHoldStore
 *		Create the tuplestore for a portal.
 */
void
PortalCreateHoldStore(Portal portal)
{
	MemoryContext oldcxt;

	Assert(portal->holdContext == NULL);
	Assert(portal->holdStore == NULL);
	Assert(portal->holdSnapshot == NULL);

	/*
	 * Create the memory context that is used for storage of the tuple set.
	 * Note this is NOT a child of the portal's portalContext.
	 */
	portal->holdContext =
		AllocSetContextCreate(TopPortalContext,
							  "PortalHoldContext",
							  ALLOCSET_DEFAULT_SIZES);

	/*
	 * Create the tuple store, selecting cross-transaction temp files.
	 *
	 * XXX: Should maintenance_work_mem be used for the portal size?
	 */
	oldcxt = MemoryContextSwitchTo(portal->holdContext);

	portal->holdStore =
		tuplestore_begin_heap(false,
							  true, work_mem);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * MarkPortalActive
 *		Transition a portal from READY to ACTIVE state.
 *
 * NOTE: never set portal->status = PORTAL_ACTIVE directly; call this instead.
 */
void
MarkPortalActive(Portal portal)
{
	/* For safety, this is a runtime test not just an Assert */
	if (portal->status != PORTAL_READY)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("portal \"%s\" cannot be run", portal->name)));
	/* Perform the state transition */
	portal->status = PORTAL_ACTIVE;
	portal->activeSubid = GetCurrentSubTransactionId();
}

/*
 * MarkPortalDone
 *		Transition a portal from ACTIVE to DONE state.
 *
 * NOTE: never set portal->status = PORTAL_DONE directly; call this instead.
 */
void
MarkPortalDone(Portal portal)
{
	/* Perform the state transition */
	Assert(portal->status == PORTAL_ACTIVE);
	portal->status = PORTAL_DONE;

	/*
	 * Allow portalcmds.c to clean up the state it knows about.  We might as
	 * well do that now, since the portal can't be executed any more.
	 *
	 * In some cases involving execution of a ROLLBACK command in an already
	 * aborted transaction, this is necessary, or we'd reach AtCleanup_Portals
	 * with the cleanup hook still unexecuted.
	 */
	if (PointerIsValid(portal->cleanup))
	{
		portal->cleanup(portal);
		portal->cleanup = NULL;
	}
}

/*
 * MarkPortalFailed
 *		Transition a portal into FAILED state.
 *
 * NOTE: never set portal->status = PORTAL_FAILED directly; call this instead.
 */
void
MarkPortalFailed(Portal portal)
{
	/* Perform the state transition */
	Assert(portal->status != PORTAL_DONE);
	portal->status = PORTAL_FAILED;

	/*
	 * Allow portalcmds.c to clean up the state it knows about.  We might as
	 * well do that now, since the portal can't be executed any more.
	 *
	 * In some cases involving cleanup of an already aborted transaction, this
	 * is necessary, or we'd reach AtCleanup_Portals with the cleanup hook
	 * still unexecuted.
	 */
	if (PointerIsValid(portal->cleanup))
	{
		portal->cleanup(portal);
		portal->cleanup = NULL;
	}
}

/*
 * PortalDrop
 *		Destroy the portal.
 */
void
PortalDrop(Portal portal, bool isTopCommit)
{
	AssertArg(PortalIsValid(portal));

	/*
	 * Not sure if the PORTAL_ACTIVE case can validly happen or not...
	 */
	if (portal->status == PORTAL_ACTIVE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cannot drop active portal \"%s\"", portal->name)));

	/*
	 * Allow portalcmds.c to clean up the state it knows about, in particular
	 * shutting down the executor if still active.  This step potentially runs
	 * user-defined code so failure has to be expected.  It's the cleanup
	 * hook's responsibility to not try to do that more than once, in the case
	 * that failure occurs and then we come back to drop the portal again
	 * during transaction abort.
	 *
	 * Note: in most paths of control, this will have been done already in
	 * MarkPortalDone or MarkPortalFailed.  We're just making sure.
	 */
	if (PointerIsValid(portal->cleanup))
	{
		portal->cleanup(portal);
		portal->cleanup = NULL;
	}

	/* There shouldn't be an active snapshot anymore, except after error */
	Assert(portal->portalSnapshot == NULL || !isTopCommit);

	/*
	 * Remove portal from hash table.  Because we do this here, we will not
	 * come back to try to remove the portal again if there's any error in the
	 * subsequent steps.  Better to leak a little memory than to get into an
	 * infinite error-recovery loop.
	 */
	PortalHashTableDelete(portal);

	/* drop cached plan reference, if any */
	PortalReleaseCachedPlan(portal);

	/*
	 * If portal has a snapshot protecting its data, release that.  This needs
	 * a little care since the registration will be attached to the portal's
	 * resowner; if the portal failed, we will already have released the
	 * resowner (and the snapshot) during transaction abort.
	 */
	if (portal->holdSnapshot)
	{
		if (portal->resowner)
			UnregisterSnapshotFromOwner(portal->holdSnapshot,
										portal->resowner);
		portal->holdSnapshot = NULL;
	}

	/*
	 * Release any resources still attached to the portal.  There are several
	 * cases being covered here:
	 *
	 * Top transaction commit (indicated by isTopCommit): normally we should
	 * do nothing here and let the regular end-of-transaction resource
	 * releasing mechanism handle these resources too.  However, if we have a
	 * FAILED portal (eg, one that got an error), we'd better clean up
	 * its resources to avoid resource-leakage warning messages.
	 *
	 * Sub transaction commit: never comes here at all, since we don't kill
	 * any portals in AtSubCommit_Portals().
	 *
	 * Main or sub transaction abort: we will do nothing here because
	 * portal->resowner was already set NULL; the resources were already
	 * cleaned up in transaction abort.
	 *
	 * Ordinary portal drop: must release resources.  However, if the portal
	 * is not FAILED then we do not release its locks.  The locks become the
	 * responsibility of the transaction's ResourceOwner (since it is the
	 * parent of the portal's owner) and will be released when the transaction
	 * eventually ends.
	 */
	if (portal->resowner &&
		(!isTopCommit || portal->status == PORTAL_FAILED))
	{
		bool		isCommit = (portal->status != PORTAL_FAILED);

		ResourceOwnerRelease(portal->resowner,
							 RESOURCE_RELEASE_BEFORE_LOCKS,
							 isCommit, false);
		ResourceOwnerRelease(portal->resowner,
							 RESOURCE_RELEASE_LOCKS,
							 isCommit, false);
		ResourceOwnerRelease(portal->resowner,
							 RESOURCE_RELEASE_AFTER_LOCKS,
							 isCommit, false);
		ResourceOwnerDelete(portal->resowner);
	}
	portal->resowner = NULL;

	/*
	 * Delete tuplestore if present.  We should do this even under error
	 * conditions; since the tuplestore would have been using cross-
	 * transaction storage, its temp files need to be explicitly deleted.
	 */
	if (portal->holdStore)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(portal->holdContext);
		tuplestore_end(portal->holdStore);
		MemoryContextSwitchTo(oldcontext);
		portal->holdStore = NULL;
	}

	/* delete tuplestore storage, if any */
	if (portal->holdContext)
		MemoryContextDelete(portal->holdContext);

	/* release subsidiary storage */
	MemoryContextDelete(portal->portalContext);

	/* release portal struct (it's in TopPortalContext) */
	pfree(portal);
}

/*
 * Delete all non-active portals.
 *
 * Used by command: DISCARD ALL
 */
void
PortalHashTableDeleteAll(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	if (PortalHashTable == NULL)
		return;

	hash_seq_init(&status, PortalHashTable);
	while ((hentry = hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/* Can't close the active portal (the one running the command) */
		if (portal->status == PORTAL_ACTIVE)
			continue;

		PortalDrop(portal, false);

		/* Restart the iteration in case that led to other drops */
		hash_seq_term(&status);
		hash_seq_init(&status, PortalHashTable);
	}
}

/*
 * Pre-commit processing for portals.
 *
 * Portals created in this transaction are simply removed, since we are
 * going to close down the executor and release locks.
 *
 * Returns true if any portals changed state (possibly causing user-defined
 * code to be run), false if not.
 */
bool
PreCommit_Portals(bool isPrepare)
{
	bool		result = false;
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/*
		 * Do not touch active portals --- this can only happen in the case of
		 * a multi-transaction utility command, such as VACUUM, or a commit in
		 * a procedure.
		 *
		 * Note however that any resource owner attached to such a portal is
		 * still going to go away, so don't leave a dangling pointer.  Also
		 * unregister any snapshots held by the portal, mainly to avoid
		 * snapshot leak warnings from ResourceOwnerRelease().
		 */
		if (portal->status == PORTAL_ACTIVE)
		{
			if (portal->holdSnapshot)
			{
				if (portal->resowner)
					UnregisterSnapshotFromOwner(portal->holdSnapshot,
												portal->resowner);
				portal->holdSnapshot = NULL;
			}
			portal->resowner = NULL;
			/* Clear portalSnapshot too, for cleanliness */
			portal->portalSnapshot = NULL;
			continue;
		}

		/*
		 * Zap all portals created in this transaction.
		 */
		PortalDrop(portal, true);

		/* Report we changed state */
		result = true;

		/*
		 * After dropping a portal, we have to restart the iteration, because
		 * we could have invoked user-defined code that caused a drop of the
		 * next portal in the hash chain.
		 */
		hash_seq_term(&status);
		hash_seq_init(&status, PortalHashTable);
	}

	return result;
}

/*
 * Abort processing for portals.
 *
 * At this point we run the cleanup hook if present, but we can't release the
 * portal's memory until the cleanup call.
 */
void
AtAbort_Portals(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/*
		 * When elog(FATAL) is progress, we need to set the active portal to
		 * failed, so that PortalCleanup() doesn't run the executor shutdown.
		 */
		if (portal->status == PORTAL_ACTIVE && shmem_exit_inprogress)
			MarkPortalFailed(portal);

		/*
		 * If it was created in the current transaction, we can't do normal
		 * shutdown on a READY portal either; it might refer to objects
		 * created in the failed transaction.  See comments in
		 * AtSubAbort_Portals.
		 */
		if (portal->status == PORTAL_READY)
			MarkPortalFailed(portal);

		/*
		 * Allow portalcmds.c to clean up the state it knows about, if we
		 * haven't already.
		 */
		if (PointerIsValid(portal->cleanup))
		{
			portal->cleanup(portal);
			portal->cleanup = NULL;
		}

		/* drop cached plan reference, if any */
		PortalReleaseCachedPlan(portal);

		/*
		 * Any resources belonging to the portal will be released in the
		 * upcoming transaction-wide cleanup; they will be gone before we run
		 * PortalDrop.
		 */
		portal->resowner = NULL;

		/*
		 * Although we can't delete the portal data structure proper, we can
		 * release any memory in subsidiary contexts, such as executor state.
		 * The cleanup hook was the last thing that might have needed data
		 * there.  But leave active portals alone.
		 */
		if (portal->status != PORTAL_ACTIVE)
			MemoryContextDeleteChildren(portal->portalContext);
	}
}

/*
 * Post-abort cleanup for portals.
 */
void
AtCleanup_Portals(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/*
		 * Do not touch active portals --- this can only happen in the case of
		 * a multi-transaction command.
		 */
		if (portal->status == PORTAL_ACTIVE)
			continue;

		/*
		 * We had better not call any user-defined code during cleanup, so if
		 * the cleanup hook hasn't been run yet, too bad; we'll just skip it.
		 */
		if (PointerIsValid(portal->cleanup))
		{
			elog(WARNING, "skipping cleanup for portal \"%s\"", portal->name);
			portal->cleanup = NULL;
		}

		/* Zap it. */
		PortalDrop(portal, false);
	}
}

/*
 * Pre-subcommit processing for portals.
 *
 * Reassign portals created or used in the current subtransaction to the
 * parent subtransaction.
 */
void
AtSubCommit_Portals(SubTransactionId mySubid,
					SubTransactionId parentSubid,
					int parentLevel,
					ResourceOwner parentXactOwner)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		if (portal->createSubid == mySubid)
		{
			portal->createSubid = parentSubid;
			portal->createLevel = parentLevel;
			if (portal->resowner)
				ResourceOwnerNewParent(portal->resowner, parentXactOwner);
		}
		if (portal->activeSubid == mySubid)
			portal->activeSubid = parentSubid;
	}
}

/*
 * Subtransaction abort handling for portals.
 *
 * Deactivate portals created or used during the failed subtransaction.
 * Note that per AtSubCommit_Portals, this will catch portals created/used
 * in descendants of the subtransaction too.
 *
 * We don't destroy any portals here; that's done in AtSubCleanup_Portals.
 */
void
AtSubAbort_Portals(SubTransactionId mySubid,
				   SubTransactionId parentSubid,
				   ResourceOwner myXactOwner,
				   ResourceOwner parentXactOwner)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/* Was it created in this subtransaction? */
		if (portal->createSubid != mySubid)
		{
			/* No, but maybe it was used in this subtransaction? */
			if (portal->activeSubid == mySubid)
			{
				/* Maintain activeSubid until the portal is removed */
				portal->activeSubid = parentSubid;

				/*
				 * A MarkPortalActive() caller ran an upper-level portal in
				 * this subtransaction and left the portal ACTIVE.  This can't
				 * happen, but force the portal into FAILED state for the same
				 * reasons discussed below.
				 *
				 * We assume we can get away without forcing upper-level READY
				 * portals to fail, even if they were run and then suspended.
				 * In theory a suspended upper-level portal could have
				 * acquired some references to objects that are about to be
				 * destroyed, but there should be sufficient defenses against
				 * such cases: the portal's original query cannot contain such
				 * references, and any references within, say, cached plans of
				 * PL/pgSQL functions are not from active queries and should
				 * be protected by revalidation logic.
				 */
				if (portal->status == PORTAL_ACTIVE)
					MarkPortalFailed(portal);

				/*
				 * Also, if we failed it during the current subtransaction
				 * (either just above, or earlier), reattach its resource
				 * owner to the current subtransaction's resource owner, so
				 * that any resources it still holds will be released while
				 * cleaning up this subtransaction.  This prevents some corner
				 * cases wherein we might get Asserts or worse while cleaning
				 * up objects created during the current subtransaction
				 * (because they're still referenced within this portal).
				 */
				if (portal->status == PORTAL_FAILED && portal->resowner)
				{
					ResourceOwnerNewParent(portal->resowner, myXactOwner);
					portal->resowner = NULL;
				}
			}
			/* Done if it wasn't created in this subtransaction */
			continue;
		}

		/*
		 * Force any live portals of my own subtransaction into FAILED state.
		 * We have to do this because they might refer to objects created or
		 * changed in the failed subtransaction, leading to crashes within
		 * ExecutorEnd when portalcmds.c tries to close down the portal.
		 * Currently, every MarkPortalActive() caller ensures it updates the
		 * portal status again before relinquishing control, so ACTIVE can't
		 * happen here.  If it does happen, dispose the portal like existing
		 * MarkPortalActive() callers would.
		 */
		if (portal->status == PORTAL_READY ||
			portal->status == PORTAL_ACTIVE)
			MarkPortalFailed(portal);

		/*
		 * Allow portalcmds.c to clean up the state it knows about, if we
		 * haven't already.
		 */
		if (PointerIsValid(portal->cleanup))
		{
			portal->cleanup(portal);
			portal->cleanup = NULL;
		}

		/* drop cached plan reference, if any */
		PortalReleaseCachedPlan(portal);

		/*
		 * Any resources belonging to the portal will be released in the
		 * upcoming transaction-wide cleanup; they will be gone before we run
		 * PortalDrop.
		 */
		portal->resowner = NULL;

		/*
		 * Although we can't delete the portal data structure proper, we can
		 * release any memory in subsidiary contexts, such as executor state.
		 * The cleanup hook was the last thing that might have needed data
		 * there.
		 */
		MemoryContextDeleteChildren(portal->portalContext);
	}
}

/*
 * Post-subabort cleanup for portals.
 *
 * Drop all portals created in the failed subtransaction (but note that
 * we will not drop any that were reassigned to the parent above).
 */
void
AtSubCleanup_Portals(SubTransactionId mySubid)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		if (portal->createSubid != mySubid)
			continue;

		/*
		 * We had better not call any user-defined code during cleanup, so if
		 * the cleanup hook hasn't been run yet, too bad; we'll just skip it.
		 */
		if (PointerIsValid(portal->cleanup))
		{
			elog(WARNING, "skipping cleanup for portal \"%s\"", portal->name);
			portal->cleanup = NULL;
		}

		/* Zap it. */
		PortalDrop(portal, false);
	}
}

/*
 * Drop the outer active snapshots for all portals, so that no snapshots
 * remain active.
 *
 * This must be called when initiating a COMMIT or ROLLBACK inside a
 * procedure.  It should not be run until we're done with steps that are
 * likely to fail.
 *
 * It's tempting to fold this into PreCommit_Portals, but to do so, we'd
 * need to clean up snapshot management in VACUUM and perhaps other places.
 */
void
ForgetPortalSnapshots(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;
	int			numPortalSnaps = 0;
	int			numActiveSnaps = 0;

	/* First, scan PortalHashTable and clear portalSnapshot fields */
	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		if (portal->portalSnapshot != NULL)
		{
			portal->portalSnapshot = NULL;
			numPortalSnaps++;
		}
		/* portal->holdSnapshot will be cleaned up in PreCommit_Portals */
	}

	/*
	 * Now, pop all the active snapshots, which should be just those that were
	 * portal snapshots.  Ideally we'd drive this directly off the portal
	 * scan, but there's no good way to visit the portals in the correct
	 * order.  So just cross-check after the fact.
	 */
	while (ActiveSnapshotSet())
	{
		PopActiveSnapshot();
		numActiveSnaps++;
	}

	if (numPortalSnaps != numActiveSnaps)
		elog(ERROR, "portal snapshots (%d) did not account for all active snapshots (%d)",
			 numPortalSnaps, numActiveSnaps);
}
