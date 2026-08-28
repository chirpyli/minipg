/*-------------------------------------------------------------------------
 *
 * spi.c
 *				Server Programming Interface
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/spi.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/printtup.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/heap.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/spi_priv.h"
#include "miscadmin.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/*
 * These global variables are part of the API for various SPI functions
 * (a horrible API choice, but it's too late now).  To reduce the risk of
 * interference between different SPI callers, we save and restore them
 * when entering/exiting a SPI nesting level.
 */
uint64		SPI_processed = 0;
SPITupleTable *SPI_tuptable = NULL;
int			SPI_result = 0;

static _SPI_connection *_SPI_stack = NULL;
static _SPI_connection *_SPI_current = NULL;
static int	_SPI_stack_depth = 0;	/* allocated size of _SPI_stack */
static int	_SPI_connected = -1;	/* current stack index */

typedef struct SPICallbackArg
{
	const char *query;
	RawParseMode mode;
} SPICallbackArg;

static void _SPI_prepare_plan(const char *src, SPIPlanPtr plan);

static int	_SPI_execute_plan(SPIPlanPtr plan, const SPIExecuteOptions *options,
							  Snapshot snapshot, Snapshot crosscheck_snapshot,
							  bool fire_triggers);

static ParamListInfo _SPI_convert_params(int nargs, Oid *argtypes,
										 Datum *Values, const char *Nulls);

static int	_SPI_pquery(QueryDesc *queryDesc, bool fire_triggers, uint64 tcount);

static void _SPI_error_callback(void *arg);

static SPIPlanPtr _SPI_make_plan_non_temp(SPIPlanPtr plan);
static SPIPlanPtr _SPI_save_plan(SPIPlanPtr plan);

static int	_SPI_begin_call(bool use_exec);
static int	_SPI_end_call(bool use_exec);
static MemoryContext _SPI_execmem(void);
static MemoryContext _SPI_procmem(void);
static bool _SPI_checktuples(void);


/* =================== interface functions =================== */

int
SPI_connect(void)
{
	return SPI_connect_ext(0);
}

int
SPI_connect_ext(int options)
{
	int			newdepth;

	/* Enlarge stack if necessary */
	if (_SPI_stack == NULL)
	{
		if (_SPI_connected != -1 || _SPI_stack_depth != 0)
			elog(ERROR, "SPI stack corrupted");
		newdepth = 16;
		_SPI_stack = (_SPI_connection *)
			MemoryContextAlloc(TopMemoryContext,
							   newdepth * sizeof(_SPI_connection));
		_SPI_stack_depth = newdepth;
	}
	else
	{
		if (_SPI_stack_depth <= 0 || _SPI_stack_depth <= _SPI_connected)
			elog(ERROR, "SPI stack corrupted");
		if (_SPI_stack_depth == _SPI_connected + 1)
		{
			newdepth = _SPI_stack_depth * 2;
			_SPI_stack = (_SPI_connection *)
				repalloc(_SPI_stack,
						 newdepth * sizeof(_SPI_connection));
			_SPI_stack_depth = newdepth;
		}
	}

	/* Enter new stack level */
	_SPI_connected++;
	Assert(_SPI_connected >= 0 && _SPI_connected < _SPI_stack_depth);

	_SPI_current = &(_SPI_stack[_SPI_connected]);
	_SPI_current->processed = 0;
	_SPI_current->tuptable = NULL;
	_SPI_current->execSubid = InvalidSubTransactionId;
	slist_init(&_SPI_current->tuptables);
	_SPI_current->procCxt = NULL;	/* in case we fail to create 'em */
	_SPI_current->execCxt = NULL;
	_SPI_current->connectSubid = GetCurrentSubTransactionId();
	_SPI_current->queryEnv = NULL;
	_SPI_current->atomic = (options & SPI_OPT_NONATOMIC ? false : true);
	_SPI_current->internal_xact = false;
	_SPI_current->outer_processed = SPI_processed;
	_SPI_current->outer_tuptable = SPI_tuptable;
	_SPI_current->outer_result = SPI_result;

	/*
	 * Create memory contexts for this procedure
	 *
	 * In atomic contexts (the normal case), we use TopTransactionContext,
	 * otherwise PortalContext, so that it lives across transaction
	 * boundaries.
	 *
	 * XXX It could be better to use PortalContext as the parent context in
	 * all cases, but we may not be inside a portal (consider deferred-trigger
	 * execution).  Perhaps CurTransactionContext could be an option?  For now
	 * it doesn't matter because we clean up explicitly in AtEOSubXact_SPI();
	 * but see also AtEOXact_SPI().
	 */
	_SPI_current->procCxt = AllocSetContextCreate(_SPI_current->atomic ? TopTransactionContext : PortalContext,
												  "SPI Proc",
												  ALLOCSET_DEFAULT_SIZES);
	_SPI_current->execCxt = AllocSetContextCreate(_SPI_current->atomic ? TopTransactionContext : _SPI_current->procCxt,
												  "SPI Exec",
												  ALLOCSET_DEFAULT_SIZES);
	/* ... and switch to procedure's context */
	_SPI_current->savedcxt = MemoryContextSwitchTo(_SPI_current->procCxt);

	/*
	 * Reset API global variables so that current caller cannot accidentally
	 * depend on state of an outer caller.
	 */
	SPI_processed = 0;
	SPI_tuptable = NULL;
	SPI_result = 0;

	return SPI_OK_CONNECT;
}

int
SPI_finish(void)
{
	int			res;

	res = _SPI_begin_call(false);	/* just check we're connected */
	if (res < 0)
		return res;

	/* Restore memory context as it was before procedure call */
	MemoryContextSwitchTo(_SPI_current->savedcxt);

	/* Release memory used in procedure call (including tuptables) */
	MemoryContextDelete(_SPI_current->execCxt);
	_SPI_current->execCxt = NULL;
	MemoryContextDelete(_SPI_current->procCxt);
	_SPI_current->procCxt = NULL;

	/*
	 * Restore outer API variables, especially SPI_tuptable which is probably
	 * pointing at a just-deleted tuptable
	 */
	SPI_processed = _SPI_current->outer_processed;
	SPI_tuptable = _SPI_current->outer_tuptable;
	SPI_result = _SPI_current->outer_result;

	/* Exit stack level */
	_SPI_connected--;
	if (_SPI_connected < 0)
		_SPI_current = NULL;
	else
		_SPI_current = &(_SPI_stack[_SPI_connected]);

	return SPI_OK_FINISH;
}

/*
 * SPI_start_transaction is a no-op, kept for backwards compatibility.
 * SPI callers are *always* inside a transaction.
 */
void
SPI_start_transaction(void)
{
}

static void
_SPI_commit(bool chain)
{
	MemoryContext oldcontext = CurrentMemoryContext;

	/*
	 * Complain if we are in a context that doesn't permit transaction
	 * termination.  (Note: here and _SPI_rollback should be the only places
	 * that throw ERRCODE_INVALID_TRANSACTION_TERMINATION, so that callers can
	 * test for that with security that they know what happened.)
	 */
	if (_SPI_current->atomic)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_TERMINATION),
				 errmsg("invalid transaction termination")));

	/*
	 * This restriction is required by PLs implemented on top of SPI.  They
	 * use subtransactions to establish exception blocks that are supposed to
	 * be rolled back together if there is an error.  Terminating the
	 * top-level transaction in such a block violates that idea.  A future PL
	 * implementation might have different ideas about this, in which case
	 * this restriction would have to be refined or the check possibly be
	 * moved out of SPI into the PLs.  Note however that the code below relies
	 * on not being within a subtransaction.
	 */
	if (IsSubTransaction())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_TERMINATION),
				 errmsg("cannot commit while a subtransaction is active")));

	/* XXX this ain't re-entrant enough for my taste */
	if (chain)
		SaveTransactionCharacteristics();

	/* Catch any error occurring during the COMMIT */
	PG_TRY();
	{
		/* Protect current SPI stack entry against deletion */
		_SPI_current->internal_xact = true;

		/* Release snapshots associated with portals */
		ForgetPortalSnapshots();

		/* Do the deed */
		CommitTransactionCommand();

		/* Immediately start a new transaction */
		StartTransactionCommand();
		if (chain)
			RestoreTransactionCharacteristics();

		MemoryContextSwitchTo(oldcontext);

		_SPI_current->internal_xact = false;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* Save error info in caller's context */
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		/*
		 * Abort the failed transaction.  If this fails too, we'll just
		 * propagate the error out ... there's not that much we can do.
		 */
		AbortCurrentTransaction();

		/* ... and start a new one */
		StartTransactionCommand();
		if (chain)
			RestoreTransactionCharacteristics();

		MemoryContextSwitchTo(oldcontext);

		_SPI_current->internal_xact = false;

		/* Now that we've cleaned up the transaction, re-throw the error */
		ReThrowError(edata);
	}
	PG_END_TRY();
}

void
SPI_commit(void)
{
	_SPI_commit(false);
}

void
SPI_commit_and_chain(void)
{
	_SPI_commit(true);
}

static void
_SPI_rollback(bool chain)
{
	MemoryContext oldcontext = CurrentMemoryContext;

	/* see comments in _SPI_commit() */
	if (_SPI_current->atomic)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_TERMINATION),
				 errmsg("invalid transaction termination")));

	/* see comments in _SPI_commit() */
	if (IsSubTransaction())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_TERMINATION),
				 errmsg("cannot roll back while a subtransaction is active")));

	/* XXX this ain't re-entrant enough for my taste */
	if (chain)
		SaveTransactionCharacteristics();

	/* Catch any error occurring during the ROLLBACK */
	PG_TRY();
	{
		/* Protect current SPI stack entry against deletion */
		_SPI_current->internal_xact = true;

		/* Release snapshots associated with portals */
		ForgetPortalSnapshots();

		/* Do the deed */
		AbortCurrentTransaction();

		/* Immediately start a new transaction */
		StartTransactionCommand();
		if (chain)
			RestoreTransactionCharacteristics();

		MemoryContextSwitchTo(oldcontext);

		_SPI_current->internal_xact = false;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* Save error info in caller's context */
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		/*
		 * Try again to abort the failed transaction.  If this fails too,
		 * we'll just propagate the error out ... there's not that much we can
		 * do.
		 */
		AbortCurrentTransaction();

		/* ... and start a new one */
		StartTransactionCommand();
		if (chain)
			RestoreTransactionCharacteristics();

		MemoryContextSwitchTo(oldcontext);

		_SPI_current->internal_xact = false;

		/* Now that we've cleaned up the transaction, re-throw the error */
		ReThrowError(edata);
	}
	PG_END_TRY();
}

void
SPI_rollback(void)
{
	_SPI_rollback(false);
}

void
SPI_rollback_and_chain(void)
{
	_SPI_rollback(true);
}

/*
 * SPICleanup is a no-op, kept for backwards compatibility. We rely on
 * AtEOXact_SPI to cleanup. Extensions should not (need to) fiddle with the
 * internal SPI state directly.
 */
void
SPICleanup(void)
{
}

/*
 * Clean up SPI state at transaction commit or abort.
 */
void
AtEOXact_SPI(bool isCommit)
{
	bool		found = false;

	/*
	 * Pop stack entries, stopping if we find one marked internal_xact (that
	 * one belongs to the caller of SPI_commit or SPI_abort).
	 */
	while (_SPI_connected >= 0)
	{
		_SPI_connection *connection = &(_SPI_stack[_SPI_connected]);

		if (connection->internal_xact)
			break;

		found = true;

		/*
		 * We need not release the procedure's memory contexts explicitly, as
		 * they'll go away automatically when their parent context does; see
		 * notes in SPI_connect_ext.
		 */

		/*
		 * Restore outer global variables and pop the stack entry.  Unlike
		 * SPI_finish(), we don't risk switching to memory contexts that might
		 * be already gone.
		 */
		SPI_processed = connection->outer_processed;
		SPI_tuptable = connection->outer_tuptable;
		SPI_result = connection->outer_result;

		_SPI_connected--;
		if (_SPI_connected < 0)
			_SPI_current = NULL;
		else
			_SPI_current = &(_SPI_stack[_SPI_connected]);
	}

	/* We should only find entries to pop during an ABORT. */
	if (found && isCommit)
		ereport(WARNING,
				(errcode(ERRCODE_WARNING),
				 errmsg("transaction left non-empty SPI stack"),
				 errhint("Check for missing \"SPI_finish\" calls.")));
}

/*
 * Clean up SPI state at subtransaction commit or abort.
 *
 * During commit, there shouldn't be any unclosed entries remaining from
 * the current subtransaction; we emit a warning if any are found.
 */
void
AtEOSubXact_SPI(bool isCommit, SubTransactionId mySubid)
{
	bool		found = false;

	while (_SPI_connected >= 0)
	{
		_SPI_connection *connection = &(_SPI_stack[_SPI_connected]);

		if (connection->connectSubid != mySubid)
			break;				/* couldn't be any underneath it either */

		if (connection->internal_xact)
			break;

		found = true;

		/*
		 * Release procedure memory explicitly (see note in SPI_connect)
		 */
		if (connection->execCxt)
		{
			MemoryContextDelete(connection->execCxt);
			connection->execCxt = NULL;
		}
		if (connection->procCxt)
		{
			MemoryContextDelete(connection->procCxt);
			connection->procCxt = NULL;
		}

		/*
		 * Restore outer global variables and pop the stack entry.  Unlike
		 * SPI_finish(), we don't risk switching to memory contexts that might
		 * be already gone.
		 */
		SPI_processed = connection->outer_processed;
		SPI_tuptable = connection->outer_tuptable;
		SPI_result = connection->outer_result;

		_SPI_connected--;
		if (_SPI_connected < 0)
			_SPI_current = NULL;
		else
			_SPI_current = &(_SPI_stack[_SPI_connected]);
	}

	if (found && isCommit)
		ereport(WARNING,
				(errcode(ERRCODE_WARNING),
				 errmsg("subtransaction left non-empty SPI stack"),
				 errhint("Check for missing \"SPI_finish\" calls.")));

	/*
	 * If we are aborting a subtransaction and there is an open SPI context
	 * surrounding the subxact, clean up to prevent memory leakage.
	 */
	if (_SPI_current && !isCommit)
	{
		slist_mutable_iter siter;

		/*
		 * Throw away executor state if current executor operation was started
		 * within current subxact (essentially, force a _SPI_end_call(true)).
		 */
		if (_SPI_current->execSubid >= mySubid)
		{
			_SPI_current->execSubid = InvalidSubTransactionId;
			MemoryContextResetAndDeleteChildren(_SPI_current->execCxt);
		}

		/* throw away any tuple tables created within current subxact */
		slist_foreach_modify(siter, &_SPI_current->tuptables)
		{
			SPITupleTable *tuptable;

			tuptable = slist_container(SPITupleTable, next, siter.cur);
			if (tuptable->subid >= mySubid)
			{
				/*
				 * If we used SPI_freetuptable() here, its internal search of
				 * the tuptables list would make this operation O(N^2).
				 * Instead, just free the tuptable manually.  This should
				 * match what SPI_freetuptable() does.
				 */
				slist_delete_current(&siter);
				if (tuptable == _SPI_current->tuptable)
					_SPI_current->tuptable = NULL;
				if (tuptable == SPI_tuptable)
					SPI_tuptable = NULL;
				MemoryContextDelete(tuptable->tuptabcxt);
			}
		}
	}
}

/*
 * Are we executing inside a procedure (that is, a nonatomic SPI context)?
 */
bool
SPI_inside_nonatomic_context(void)
{
	if (_SPI_current == NULL)
		return false;			/* not in any SPI context at all */
	/* these tests must match _SPI_commit's opinion of what's atomic: */
	if (_SPI_current->atomic)
		return false;			/* it's atomic (ie function not procedure) */
	if (IsSubTransaction())
		return false;			/* if within subtransaction, it's atomic */
	return true;
}


/* Parse, plan, and execute a query string */
int
SPI_execute(const char *src, bool read_only, long tcount)
{
	_SPI_plan	plan;
	SPIExecuteOptions options;
	int			res;

	if (src == NULL || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	memset(&plan, 0, sizeof(_SPI_plan));
	plan.magic = _SPI_PLAN_MAGIC;
	plan.parse_mode = RAW_PARSE_DEFAULT;
	plan.cursor_options = CURSOR_OPT_PARALLEL_OK;

	_SPI_prepare_plan(src, &plan);

	memset(&options, 0, sizeof(options));
	options.read_only = read_only;
	options.tcount = tcount;

	res = _SPI_execute_plan(&plan, &options,
							InvalidSnapshot, InvalidSnapshot,
							true);

	_SPI_end_call(true);
	return res;
}

/* Obsolete version of SPI_execute */
int
SPI_exec(const char *src, long tcount)
{
	return SPI_execute(src, false, tcount);
}

/* Parse, plan, and execute a query string, with extensible options */
int
SPI_execute_extended(const char *src,
					 const SPIExecuteOptions *options)
{
	int			res;
	_SPI_plan	plan;

	if (src == NULL || options == NULL)
		return SPI_ERROR_ARGUMENT;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	memset(&plan, 0, sizeof(_SPI_plan));
	plan.magic = _SPI_PLAN_MAGIC;
	plan.parse_mode = RAW_PARSE_DEFAULT;
	plan.cursor_options = CURSOR_OPT_PARALLEL_OK;
	if (options->params)
	{
		plan.parserSetup = options->params->parserSetup;
		plan.parserSetupArg = options->params->parserSetupArg;
	}

	_SPI_prepare_plan(src, &plan);

	res = _SPI_execute_plan(&plan, options,
							InvalidSnapshot, InvalidSnapshot,
							true);

	_SPI_end_call(true);
	return res;
}

/* Execute a previously prepared plan */
int
SPI_execute_plan(SPIPlanPtr plan, Datum *Values, const char *Nulls,
				 bool read_only, long tcount)
{
	SPIExecuteOptions options;
	int			res;

	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	if (plan->nargs > 0 && Values == NULL)
		return SPI_ERROR_PARAM;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	memset(&options, 0, sizeof(options));
	options.params = _SPI_convert_params(plan->nargs, plan->argtypes,
										 Values, Nulls);
	options.read_only = read_only;
	options.tcount = tcount;

	res = _SPI_execute_plan(plan, &options,
							InvalidSnapshot, InvalidSnapshot,
							true);

	_SPI_end_call(true);
	return res;
}

/* Obsolete version of SPI_execute_plan */
int
SPI_execp(SPIPlanPtr plan, Datum *Values, const char *Nulls, long tcount)
{
	return SPI_execute_plan(plan, Values, Nulls, false, tcount);
}

/* Execute a previously prepared plan */
int
SPI_execute_plan_extended(SPIPlanPtr plan,
						  const SPIExecuteOptions *options)
{
	int			res;

	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC || options == NULL)
		return SPI_ERROR_ARGUMENT;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	res = _SPI_execute_plan(plan, options,
							InvalidSnapshot, InvalidSnapshot,
							true);

	_SPI_end_call(true);
	return res;
}

/* Execute a previously prepared plan */
int
SPI_execute_plan_with_paramlist(SPIPlanPtr plan, ParamListInfo params,
								bool read_only, long tcount)
{
	SPIExecuteOptions options;
	int			res;

	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	memset(&options, 0, sizeof(options));
	options.params = params;
	options.read_only = read_only;
	options.tcount = tcount;

	res = _SPI_execute_plan(plan, &options,
							InvalidSnapshot, InvalidSnapshot,
							true);

	_SPI_end_call(true);
	return res;
}

/*
 * SPI_execute_snapshot -- identical to SPI_execute_plan, except that we allow
 * the caller to specify exactly which snapshots to use, which will be
 * registered here.  Also, the caller may specify that AFTER triggers should be
 * queued as part of the outer query rather than being fired immediately at the
 * end of the command.
 *
 * This is currently not documented in spi.sgml because it is only intended
 * for use by RI triggers.
 *
 * Passing snapshot == InvalidSnapshot will select the normal behavior of
 * fetching a new snapshot for each query.
 */
int
SPI_execute_snapshot(SPIPlanPtr plan,
					 Datum *Values, const char *Nulls,
					 Snapshot snapshot, Snapshot crosscheck_snapshot,
					 bool read_only, bool fire_triggers, long tcount)
{
	SPIExecuteOptions options;
	int			res;

	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	if (plan->nargs > 0 && Values == NULL)
		return SPI_ERROR_PARAM;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	memset(&options, 0, sizeof(options));
	options.params = _SPI_convert_params(plan->nargs, plan->argtypes,
										 Values, Nulls);
	options.read_only = read_only;
	options.tcount = tcount;

	res = _SPI_execute_plan(plan, &options,
							snapshot, crosscheck_snapshot,
							fire_triggers);

	_SPI_end_call(true);
	return res;
}

/*
 * SPI_execute_with_args -- plan and execute a query with supplied arguments
 *
 * This is functionally equivalent to SPI_prepare followed by
 * SPI_execute_plan.
 */
int
SPI_execute_with_args(const char *src,
					  int nargs, Oid *argtypes,
					  Datum *Values, const char *Nulls,
					  bool read_only, long tcount)
{
	int			res;
	_SPI_plan	plan;
	ParamListInfo paramLI;
	SPIExecuteOptions options;

	if (src == NULL || nargs < 0 || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	if (nargs > 0 && (argtypes == NULL || Values == NULL))
		return SPI_ERROR_PARAM;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	memset(&plan, 0, sizeof(_SPI_plan));
	plan.magic = _SPI_PLAN_MAGIC;
	plan.parse_mode = RAW_PARSE_DEFAULT;
	plan.cursor_options = CURSOR_OPT_PARALLEL_OK;
	plan.nargs = nargs;
	plan.argtypes = argtypes;
	plan.parserSetup = NULL;
	plan.parserSetupArg = NULL;

	paramLI = _SPI_convert_params(nargs, argtypes,
								  Values, Nulls);

	_SPI_prepare_plan(src, &plan);

	memset(&options, 0, sizeof(options));
	options.params = paramLI;
	options.read_only = read_only;
	options.tcount = tcount;

	res = _SPI_execute_plan(&plan, &options,
							InvalidSnapshot, InvalidSnapshot,
							true);

	_SPI_end_call(true);
	return res;
}

SPIPlanPtr
SPI_prepare(const char *src, int nargs, Oid *argtypes)
{
	_SPI_plan	plan;
	SPIPlanPtr	result;

	if (src == NULL || nargs < 0 || (nargs > 0 && argtypes == NULL))
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	SPI_result = _SPI_begin_call(true);
	if (SPI_result < 0)
		return NULL;

	memset(&plan, 0, sizeof(_SPI_plan));
	plan.magic = _SPI_PLAN_MAGIC;
	plan.parse_mode = RAW_PARSE_DEFAULT;
	plan.cursor_options = 0;
	plan.nargs = nargs;
	plan.argtypes = argtypes;
	plan.parserSetup = NULL;
	plan.parserSetupArg = NULL;

	_SPI_prepare_plan(src, &plan);

	/* copy plan to procedure context */
	result = _SPI_make_plan_non_temp(&plan);

	_SPI_end_call(true);

	return result;
}

SPIPlanPtr
SPI_prepare_extended(const char *src,
					 const SPIPrepareOptions *options)
{
	_SPI_plan	plan;
	SPIPlanPtr	result;

	if (src == NULL || options == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	SPI_result = _SPI_begin_call(true);
	if (SPI_result < 0)
		return NULL;

	memset(&plan, 0, sizeof(_SPI_plan));
	plan.magic = _SPI_PLAN_MAGIC;
	plan.parse_mode = options->parseMode;
	plan.cursor_options = options->cursorOptions;
	plan.nargs = 0;
	plan.argtypes = NULL;
	plan.parserSetup = options->parserSetup;
	plan.parserSetupArg = options->parserSetupArg;

	_SPI_prepare_plan(src, &plan);

	/* copy plan to procedure context */
	result = _SPI_make_plan_non_temp(&plan);

	_SPI_end_call(true);

	return result;
}

SPIPlanPtr
SPI_prepare_params(const char *src,
				   ParserSetupHook parserSetup,
				   void *parserSetupArg,
				   int cursorOptions)
{
	_SPI_plan	plan;
	SPIPlanPtr	result;

	if (src == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	SPI_result = _SPI_begin_call(true);
	if (SPI_result < 0)
		return NULL;

	memset(&plan, 0, sizeof(_SPI_plan));
	plan.magic = _SPI_PLAN_MAGIC;
	plan.parse_mode = RAW_PARSE_DEFAULT;
	plan.cursor_options = cursorOptions;
	plan.nargs = 0;
	plan.argtypes = NULL;
	plan.parserSetup = parserSetup;
	plan.parserSetupArg = parserSetupArg;

	_SPI_prepare_plan(src, &plan);

	/* copy plan to procedure context */
	result = _SPI_make_plan_non_temp(&plan);

	_SPI_end_call(true);

	return result;
}

/*
 * SPI_keepplan --- mark a plan as long-lived
 *
 * Since the plan cache has been removed, all SPI plans are simply stored
 * as query text.  This function is kept for backwards compatibility but
 * now just verifies the plan is valid.  The plan's memory context is
 * already under the procedure context, so it will survive.
 */
int
SPI_keepplan(SPIPlanPtr plan)
{
	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC)
		return SPI_ERROR_ARGUMENT;

	return 0;
}

SPIPlanPtr
SPI_saveplan(SPIPlanPtr plan)
{
	SPIPlanPtr	newplan;

	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	SPI_result = _SPI_begin_call(false);	/* don't change context */
	if (SPI_result < 0)
		return NULL;

	newplan = _SPI_save_plan(plan);

	SPI_result = _SPI_end_call(false);

	return newplan;
}

int
SPI_freeplan(SPIPlanPtr plan)
{
	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC)
		return SPI_ERROR_ARGUMENT;

	/* Delete the plan's memory context and everything in it */
	if (plan->plancxt)
		MemoryContextDelete(plan->plancxt);

	return 0;
}

HeapTuple
SPI_copytuple(HeapTuple tuple)
{
	MemoryContext oldcxt;
	HeapTuple	ctuple;

	if (tuple == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	if (_SPI_current == NULL)
	{
		SPI_result = SPI_ERROR_UNCONNECTED;
		return NULL;
	}

	oldcxt = MemoryContextSwitchTo(_SPI_current->savedcxt);

	ctuple = heap_copytuple(tuple);

	MemoryContextSwitchTo(oldcxt);

	return ctuple;
}

HeapTupleHeader
SPI_returntuple(HeapTuple tuple, TupleDesc tupdesc)
{
	MemoryContext oldcxt;
	HeapTupleHeader dtup;

	if (tuple == NULL || tupdesc == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	if (_SPI_current == NULL)
	{
		SPI_result = SPI_ERROR_UNCONNECTED;
		return NULL;
	}

	/* For RECORD results, make sure a typmod has been assigned */
	if (tupdesc->tdtypeid == RECORDOID &&
		tupdesc->tdtypmod < 0)
		assign_record_type_typmod(tupdesc);

	oldcxt = MemoryContextSwitchTo(_SPI_current->savedcxt);

	dtup = DatumGetHeapTupleHeader(heap_copy_tuple_as_datum(tuple, tupdesc));

	MemoryContextSwitchTo(oldcxt);

	return dtup;
}

HeapTuple
SPI_modifytuple(Relation rel, HeapTuple tuple, int natts, int *attnum,
				Datum *Values, const char *Nulls)
{
	MemoryContext oldcxt;
	HeapTuple	mtuple;
	int			numberOfAttributes;
	Datum	   *v;
	bool	   *n;
	int			i;

	if (rel == NULL || tuple == NULL || natts < 0 || attnum == NULL || Values == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	if (_SPI_current == NULL)
	{
		SPI_result = SPI_ERROR_UNCONNECTED;
		return NULL;
	}

	oldcxt = MemoryContextSwitchTo(_SPI_current->savedcxt);

	SPI_result = 0;

	numberOfAttributes = rel->rd_att->natts;
	v = (Datum *) palloc(numberOfAttributes * sizeof(Datum));
	n = (bool *) palloc(numberOfAttributes * sizeof(bool));

	/* fetch old values and nulls */
	heap_deform_tuple(tuple, rel->rd_att, v, n);

	/* replace values and nulls */
	for (i = 0; i < natts; i++)
	{
		if (attnum[i] <= 0 || attnum[i] > numberOfAttributes)
			break;
		v[attnum[i] - 1] = Values[i];
		n[attnum[i] - 1] = (Nulls && Nulls[i] == 'n') ? true : false;
	}

	if (i == natts)				/* no errors in *attnum */
	{
		mtuple = heap_form_tuple(rel->rd_att, v, n);

		/*
		 * copy the identification info of the old tuple: t_ctid, t_self, and
		 * OID (if any)
		 */
		mtuple->t_data->t_ctid = tuple->t_data->t_ctid;
		mtuple->t_self = tuple->t_self;
		mtuple->t_tableOid = tuple->t_tableOid;
	}
	else
	{
		mtuple = NULL;
		SPI_result = SPI_ERROR_NOATTRIBUTE;
	}

	pfree(v);
	pfree(n);

	MemoryContextSwitchTo(oldcxt);

	return mtuple;
}

int
SPI_fnumber(TupleDesc tupdesc, const char *fname)
{
	int			res;
	const FormData_pg_attribute *sysatt;

	for (res = 0; res < tupdesc->natts; res++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, res);

		if (namestrcmp(&attr->attname, fname) == 0 &&
			!attr->attisdropped)
			return res + 1;
	}

	sysatt = SystemAttributeByName(fname);
	if (sysatt != NULL)
		return sysatt->attnum;

	/* SPI_ERROR_NOATTRIBUTE is different from all sys column numbers */
	return SPI_ERROR_NOATTRIBUTE;
}

char *
SPI_fname(TupleDesc tupdesc, int fnumber)
{
	const FormData_pg_attribute *att;

	SPI_result = 0;

	if (fnumber > tupdesc->natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return NULL;
	}

	if (fnumber > 0)
		att = TupleDescAttr(tupdesc, fnumber - 1);
	else
		att = SystemAttributeDefinition(fnumber);

	return pstrdup(NameStr(att->attname));
}

char *
SPI_getvalue(HeapTuple tuple, TupleDesc tupdesc, int fnumber)
{
	Datum		val;
	bool		isnull;
	Oid			typoid,
				foutoid;
	bool		typisvarlena;

	SPI_result = 0;

	if (fnumber > tupdesc->natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return NULL;
	}

	val = heap_getattr(tuple, fnumber, tupdesc, &isnull);
	if (isnull)
		return NULL;

	if (fnumber > 0)
		typoid = TupleDescAttr(tupdesc, fnumber - 1)->atttypid;
	else
		typoid = (SystemAttributeDefinition(fnumber))->atttypid;

	getTypeOutputInfo(typoid, &foutoid, &typisvarlena);

	return OidOutputFunctionCall(foutoid, val);
}

Datum
SPI_getbinval(HeapTuple tuple, TupleDesc tupdesc, int fnumber, bool *isnull)
{
	SPI_result = 0;

	if (fnumber > tupdesc->natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		*isnull = true;
		return (Datum) NULL;
	}

	return heap_getattr(tuple, fnumber, tupdesc, isnull);
}

char *
SPI_gettype(TupleDesc tupdesc, int fnumber)
{
	Oid			typoid;
	HeapTuple	typeTuple;
	char	   *result;

	SPI_result = 0;

	if (fnumber > tupdesc->natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return NULL;
	}

	if (fnumber > 0)
		typoid = TupleDescAttr(tupdesc, fnumber - 1)->atttypid;
	else
		typoid = (SystemAttributeDefinition(fnumber))->atttypid;

	typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typoid));

	if (!HeapTupleIsValid(typeTuple))
	{
		SPI_result = SPI_ERROR_TYPUNKNOWN;
		return NULL;
	}

	result = pstrdup(NameStr(((Form_pg_type) GETSTRUCT(typeTuple))->typname));
	ReleaseSysCache(typeTuple);
	return result;
}

/*
 * Get the data type OID for a column.
 *
 * There's nothing similar for typmod and typcollation.  The rare consumers
 * thereof should inspect the TupleDesc directly.
 */
Oid
SPI_gettypeid(TupleDesc tupdesc, int fnumber)
{
	SPI_result = 0;

	if (fnumber > tupdesc->natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return InvalidOid;
	}

	if (fnumber > 0)
		return TupleDescAttr(tupdesc, fnumber - 1)->atttypid;
	else
		return (SystemAttributeDefinition(fnumber))->atttypid;
}

char *
SPI_getrelname(Relation rel)
{
	return pstrdup(RelationGetRelationName(rel));
}

char *
SPI_getnspname(Relation rel)
{
	return get_namespace_name(RelationGetNamespace(rel));
}

void *
SPI_palloc(Size size)
{
	if (_SPI_current == NULL)
		elog(ERROR, "SPI_palloc called while not connected to SPI");

	return MemoryContextAlloc(_SPI_current->savedcxt, size);
}

void *
SPI_repalloc(void *pointer, Size size)
{
	/* No longer need to worry which context chunk was in... */
	return repalloc(pointer, size);
}

void
SPI_pfree(void *pointer)
{
	/* No longer need to worry which context chunk was in... */
	pfree(pointer);
}

Datum
SPI_datumTransfer(Datum value, bool typByVal, int typLen)
{
	MemoryContext oldcxt;
	Datum		result;

	if (_SPI_current == NULL)
		elog(ERROR, "SPI_datumTransfer called while not connected to SPI");

	oldcxt = MemoryContextSwitchTo(_SPI_current->savedcxt);

	result = datumTransfer(value, typByVal, typLen);

	MemoryContextSwitchTo(oldcxt);

	return result;
}

void
SPI_freetuple(HeapTuple tuple)
{
	/* No longer need to worry which context tuple was in... */
	heap_freetuple(tuple);
}

void
SPI_freetuptable(SPITupleTable *tuptable)
{
	bool		found = false;

	/* ignore call if NULL pointer */
	if (tuptable == NULL)
		return;

	/*
	 * Search only the topmost SPI context for a matching tuple table.
	 */
	if (_SPI_current != NULL)
	{
		slist_mutable_iter siter;

		/* find tuptable in active list, then remove it */
		slist_foreach_modify(siter, &_SPI_current->tuptables)
		{
			SPITupleTable *tt;

			tt = slist_container(SPITupleTable, next, siter.cur);
			if (tt == tuptable)
			{
				slist_delete_current(&siter);
				found = true;
				break;
			}
		}
	}

	/*
	 * Refuse the deletion if we didn't find it in the topmost SPI context.
	 * This is primarily a guard against double deletion, but might prevent
	 * other errors as well.  Since the worst consequence of not deleting a
	 * tuptable would be a transient memory leak, this is just a WARNING.
	 */
	if (!found)
	{
		elog(WARNING, "attempt to delete invalid SPITupleTable %p", tuptable);
		return;
	}

	/* for safety, reset global variables that might point at tuptable */
	if (tuptable == _SPI_current->tuptable)
		_SPI_current->tuptable = NULL;
	if (tuptable == SPI_tuptable)
		SPI_tuptable = NULL;

	/* release all memory belonging to tuptable */
	MemoryContextDelete(tuptable->tuptabcxt);
}

/*
 * Returns the Oid representing the type id for argument at argIndex. First
 * parameter is at index zero.
 */
Oid
SPI_getargtypeid(SPIPlanPtr plan, int argIndex)
{
	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC ||
		argIndex < 0 || argIndex >= plan->nargs)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return InvalidOid;
	}
	return plan->argtypes[argIndex];
}

/*
 * Returns the number of arguments for the prepared plan.
 */
int
SPI_getargcount(SPIPlanPtr plan)
{
	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return -1;
	}
	return plan->nargs;
}

/*
 * SPI_result_code_string --- convert any SPI return code to a string
 *
 * This is often useful in error messages.  Most callers will probably
 * only pass negative (error-case) codes, but for generality we recognize
 * the success codes too.
 */
const char *
SPI_result_code_string(int code)
{
	static char buf[64];

	switch (code)
	{
		case SPI_ERROR_CONNECT:
			return "SPI_ERROR_CONNECT";
		case SPI_ERROR_COPY:
			return "SPI_ERROR_COPY";
		case SPI_ERROR_OPUNKNOWN:
			return "SPI_ERROR_OPUNKNOWN";
		case SPI_ERROR_UNCONNECTED:
			return "SPI_ERROR_UNCONNECTED";
		case SPI_ERROR_ARGUMENT:
			return "SPI_ERROR_ARGUMENT";
		case SPI_ERROR_PARAM:
			return "SPI_ERROR_PARAM";
		case SPI_ERROR_TRANSACTION:
			return "SPI_ERROR_TRANSACTION";
		case SPI_ERROR_NOATTRIBUTE:
			return "SPI_ERROR_NOATTRIBUTE";
		case SPI_ERROR_NOOUTFUNC:
			return "SPI_ERROR_NOOUTFUNC";
		case SPI_ERROR_TYPUNKNOWN:
			return "SPI_ERROR_TYPUNKNOWN";
		case SPI_ERROR_REL_DUPLICATE:
			return "SPI_ERROR_REL_DUPLICATE";
		case SPI_ERROR_REL_NOT_FOUND:
			return "SPI_ERROR_REL_NOT_FOUND";
		case SPI_OK_CONNECT:
			return "SPI_OK_CONNECT";
		case SPI_OK_FINISH:
			return "SPI_OK_FINISH";
		case SPI_OK_FETCH:
			return "SPI_OK_FETCH";
		case SPI_OK_UTILITY:
			return "SPI_OK_UTILITY";
		case SPI_OK_SELECT:
			return "SPI_OK_SELECT";
		case SPI_OK_SELINTO:
			return "SPI_OK_SELINTO";
		case SPI_OK_INSERT:
			return "SPI_OK_INSERT";
		case SPI_OK_DELETE:
			return "SPI_OK_DELETE";
		case SPI_OK_UPDATE:
			return "SPI_OK_UPDATE";
		case SPI_OK_INSERT_RETURNING:
			return "SPI_OK_INSERT_RETURNING";
		case SPI_OK_DELETE_RETURNING:
			return "SPI_OK_DELETE_RETURNING";
		case SPI_OK_UPDATE_RETURNING:
			return "SPI_OK_UPDATE_RETURNING";
		case SPI_OK_REWRITTEN:
			return "SPI_OK_REWRITTEN";
		case SPI_OK_REL_REGISTER:
			return "SPI_OK_REL_REGISTER";
		case SPI_OK_REL_UNREGISTER:
			return "SPI_OK_REL_UNREGISTER";
	}
	/* Unrecognized code ... return something useful ... */
	sprintf(buf, "Unrecognized SPI code %d", code);
	return buf;
}


/* =================== private functions =================== */

/*
 * spi_dest_startup
 *		Initialize to receive tuples from Executor into SPITupleTable
 *		of current SPI procedure
 */
void
spi_dest_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	SPITupleTable *tuptable;
	MemoryContext oldcxt;
	MemoryContext tuptabcxt;

	if (_SPI_current == NULL)
		elog(ERROR, "spi_dest_startup called while not connected to SPI");

	if (_SPI_current->tuptable != NULL)
		elog(ERROR, "improper call to spi_dest_startup");

	/* We create the tuple table context as a child of procCxt */

	oldcxt = _SPI_procmem();	/* switch to procedure memory context */

	tuptabcxt = AllocSetContextCreate(CurrentMemoryContext,
									  "SPI TupTable",
									  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(tuptabcxt);

	_SPI_current->tuptable = tuptable = (SPITupleTable *)
		palloc0(sizeof(SPITupleTable));
	tuptable->tuptabcxt = tuptabcxt;
	tuptable->subid = GetCurrentSubTransactionId();

	/*
	 * The tuptable is now valid enough to be freed by AtEOSubXact_SPI, so put
	 * it onto the SPI context's tuptables list.  This will ensure it's not
	 * leaked even in the unlikely event the following few lines fail.
	 */
	slist_push_head(&_SPI_current->tuptables, &tuptable->next);

	/* set up initial allocations */
	tuptable->alloced = 128;
	tuptable->vals = (HeapTuple *) palloc(tuptable->alloced * sizeof(HeapTuple));
	tuptable->numvals = 0;
	tuptable->tupdesc = CreateTupleDescCopy(typeinfo);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * spi_printtup
 *		store tuple retrieved by Executor into SPITupleTable
 *		of current SPI procedure
 */
bool
spi_printtup(TupleTableSlot *slot, DestReceiver *self)
{
	SPITupleTable *tuptable;
	MemoryContext oldcxt;

	if (_SPI_current == NULL)
		elog(ERROR, "spi_printtup called while not connected to SPI");

	tuptable = _SPI_current->tuptable;
	if (tuptable == NULL)
		elog(ERROR, "improper call to spi_printtup");

	oldcxt = MemoryContextSwitchTo(tuptable->tuptabcxt);

	if (tuptable->numvals >= tuptable->alloced)
	{
		/* Double the size of the pointer array */
		uint64		newalloced = tuptable->alloced * 2;

		tuptable->vals = (HeapTuple *) repalloc_huge(tuptable->vals,
													 newalloced * sizeof(HeapTuple));
		tuptable->alloced = newalloced;
	}

	tuptable->vals[tuptable->numvals] = ExecCopySlotHeapTuple(slot);
	(tuptable->numvals)++;

	MemoryContextSwitchTo(oldcxt);

	return true;
}

/*
 * Static functions
 */

/*
 * Store the query string in a SPI plan.
 *
 * After removing the plan cache, SPI plans simply store the original query
 * text and parameter info.  Each execution re-parses, re-analyzes,
 * re-rewrites, and re-plans the query.
 *
 * At entry, plan->argtypes and plan->nargs (or alternatively plan->parserSetup
 * and plan->parserSetupArg) must be valid, as must plan->parse_mode and
 * plan->cursor_options.
 */
static void
_SPI_prepare_plan(const char *src, SPIPlanPtr plan)
{
	/*
	 * Simply store the query string in the plan.  The actual parsing,
	 * analysis, rewriting, and planning will be done at execution time.
	 */
	plan->query_string = pstrdup(src);
}

/*
 * _SPI_execute_plan: execute the given plan with the given options
 *
 * Since the plan cache has been removed, this function re-parses,
 * re-analyzes, re-rewrites, and re-plans the query string on each call.
 */
static int
_SPI_execute_plan(SPIPlanPtr plan, const SPIExecuteOptions *options,
				  Snapshot snapshot, Snapshot crosscheck_snapshot,
				  bool fire_triggers)
{
	int			my_res = 0;
	uint64		my_processed = 0;
	SPITupleTable *my_tuptable = NULL;
	int			res = 0;
	bool		allow_nonatomic;
	bool		pushed_active_snap = false;
	SPICallbackArg spicallbackarg;
	ErrorContextCallback spierrcontext;
	List	   *raw_parsetree_list;
	ListCell   *lc1;
	const char *src = plan->query_string;

	allow_nonatomic = options->allow_nonatomic &&
		!_SPI_current->atomic && !IsSubTransaction();

	/*
	 * Setup error traceback support for ereport()
	 */
	spicallbackarg.query = src;
	spicallbackarg.mode = plan->parse_mode;
	spierrcontext.callback = _SPI_error_callback;
	spierrcontext.arg = &spicallbackarg;
	spierrcontext.previous = error_context_stack;
	error_context_stack = &spierrcontext;

	/*
	 * Push caller's snapshot if provided.
	 */
	if (snapshot != InvalidSnapshot)
	{
		Assert(!options->allow_nonatomic);
		if (options->read_only)
		{
			PushActiveSnapshot(snapshot);
			pushed_active_snap = true;
		}
		else
		{
			PushCopiedSnapshot(snapshot);
			pushed_active_snap = true;
		}
	}

	/*
	 * Parse the query string.
	 */
	raw_parsetree_list = raw_parser(src, plan->parse_mode);

	/*
	 * Empty query check.
	 */
	if (options->must_return_tuples && raw_parsetree_list == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("empty query does not return tuples")));

	/*
	 * Iterate over raw parse trees.  For each one: analyze, rewrite,
	 * plan, and execute.
	 */
	foreach(lc1, raw_parsetree_list)
	{
		RawStmt    *parsetree = lfirst_node(RawStmt, lc1);
		List	   *querytree_list;
		List	   *stmt_list;
		ListCell   *lc2;

		/*
		 * Analyze and rewrite.  Parameter datatypes are driven by
		 * parserSetup hook if provided, otherwise we use the fixed
		 * parameter list.
		 */
		if (plan->parserSetup != NULL)
		{
			Assert(plan->nargs == 0);
			querytree_list = pg_analyze_and_rewrite_params(parsetree,
														   src,
														   plan->parserSetup,
														   plan->parserSetupArg,
														   _SPI_current->queryEnv);
		}
		else
		{
			querytree_list = pg_analyze_and_rewrite(parsetree,
													src,
													plan->argtypes,
													plan->nargs,
													_SPI_current->queryEnv);
		}

		/*
		 * If asked to, complain when query does not return tuples.
		 */
		if (options->must_return_tuples)
		{
			bool		returns_tuples = false;
			ListCell   *qcl;

			foreach(qcl, querytree_list)
			{
				Query	   *query = lfirst_node(Query, qcl);

				if (query->commandType != CMD_UTILITY &&
					query->returningList != NIL)
				{
					returns_tuples = true;
					break;
				}
				if (query->commandType == CMD_SELECT)
				{
					returns_tuples = true;
					break;
				}
			}
			if (!returns_tuples)
			{
				const char *cmdtag;

				cmdtag = GetCommandTagName(
					CreateCommandTag(parsetree->stmt));
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("%s query does not return tuples", cmdtag)));
			}
		}

		/* Plan the queries. */
		stmt_list = pg_plan_queries(querytree_list, src,
								  plan->cursor_options, options->params);

		/*
		 * Snapshot management: if the statement list requires a snapshot
		 * and we weren't given a specific one, set it up.
		 */
		if (snapshot == InvalidSnapshot &&
			(list_length(stmt_list) > 1 ||
			 (list_length(stmt_list) == 1 &&
			  PlannedStmtRequiresSnapshot(linitial_node(PlannedStmt,
														stmt_list)))))
		{
			EnsurePortalSnapshotExists();

			if (!options->read_only && !allow_nonatomic)
			{
				if (pushed_active_snap)
					PopActiveSnapshot();
				PushActiveSnapshot(GetTransactionSnapshot());
				pushed_active_snap = true;
			}
		}

		/* Execute each planned statement. */
		foreach(lc2, stmt_list)
		{
			PlannedStmt *stmt = lfirst_node(PlannedStmt, lc2);
			bool		canSetTag = stmt->canSetTag;
			DestReceiver *dest;

			_SPI_current->processed = 0;
			_SPI_current->tuptable = NULL;

			if (stmt->utilityStmt)
			{
				if (IsA(stmt->utilityStmt, TransactionStmt))
				{
					my_res = SPI_ERROR_TRANSACTION;
					goto fail;
				}
			}

			if (options->read_only && !CommandIsReadOnly(stmt))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("%s is not allowed in a non-volatile function",
								CreateCommandName((Node *) stmt))));

			if (!options->read_only && pushed_active_snap)
			{
				CommandCounterIncrement();
				UpdateActiveSnapshotCommandId();
			}

			if (!canSetTag)
				dest = CreateDestReceiver(DestNone);
			else if (options->dest)
				dest = options->dest;
			else
				dest = CreateDestReceiver(DestSPI);

			if (stmt->utilityStmt == NULL)
			{
				QueryDesc  *qdesc;
				Snapshot	snap;

				if (ActiveSnapshotSet())
					snap = GetActiveSnapshot();
				else
					snap = InvalidSnapshot;

				qdesc = CreateQueryDesc(stmt,
										src,
										snap, crosscheck_snapshot,
										dest,
										options->params,
										_SPI_current->queryEnv,
										0);
				res = _SPI_pquery(qdesc, fire_triggers,
								  canSetTag ? options->tcount : 0);
				FreeQueryDesc(qdesc);
			}
			else
			{
				ProcessUtilityContext context;
				QueryCompletion qc;

				if (allow_nonatomic)
					context = PROCESS_UTILITY_QUERY_NONATOMIC;
				else
					context = PROCESS_UTILITY_QUERY;

				InitializeQueryCompletion(&qc);
				ProcessUtility(stmt,
							   src,
							   false,
							   context,
							   options->params,
							   _SPI_current->queryEnv,
							   dest,
							   &qc);

				if (_SPI_current->tuptable)
					_SPI_current->processed = _SPI_current->tuptable->numvals;

				res = SPI_OK_UTILITY;
			}

			if (canSetTag)
			{
				my_processed = _SPI_current->processed;
				SPI_freetuptable(my_tuptable);
				my_tuptable = _SPI_current->tuptable;
				my_res = res;
			}
			else
			{
				SPI_freetuptable(_SPI_current->tuptable);
				_SPI_current->tuptable = NULL;
			}

			if (res < 0)
			{
				my_res = res;
				goto fail;
			}
		}

		if (!options->read_only)
			CommandCounterIncrement();
	}

fail:

	if (pushed_active_snap)
		PopActiveSnapshot();

	error_context_stack = spierrcontext.previous;

	SPI_processed = my_processed;
	SPI_tuptable = my_tuptable;
	_SPI_current->tuptable = NULL;

	if (my_res == 0)
		my_res = SPI_OK_REWRITTEN;

	return my_res;
}

/*
 * Convert arrays of query parameters to form wanted by planner and executor
 */
static ParamListInfo
_SPI_convert_params(int nargs, Oid *argtypes,
					Datum *Values, const char *Nulls)
{
	ParamListInfo paramLI;

	if (nargs > 0)
	{
		paramLI = makeParamList(nargs);

		for (int i = 0; i < nargs; i++)
		{
			ParamExternData *prm = &paramLI->params[i];

			prm->value = Values[i];
			prm->isnull = (Nulls && Nulls[i] == 'n');
			prm->pflags = PARAM_FLAG_CONST;
			prm->ptype = argtypes[i];
		}
	}
	else
		paramLI = NULL;
	return paramLI;
}

static int
_SPI_pquery(QueryDesc *queryDesc, bool fire_triggers, uint64 tcount)
{
	int			operation = queryDesc->operation;
	int			eflags;
	int			res;

	switch (operation)
	{
		case CMD_SELECT:
			if (queryDesc->dest->mydest == DestNone)
			{
				/* Don't return SPI_OK_SELECT if we're discarding result */
				res = SPI_OK_UTILITY;
			}
			else
				res = SPI_OK_SELECT;
			break;
		case CMD_INSERT:
			if (queryDesc->plannedstmt->hasReturning)
				res = SPI_OK_INSERT_RETURNING;
			else
				res = SPI_OK_INSERT;
			break;
		case CMD_DELETE:
			if (queryDesc->plannedstmt->hasReturning)
				res = SPI_OK_DELETE_RETURNING;
			else
				res = SPI_OK_DELETE;
			break;
		case CMD_UPDATE:
			if (queryDesc->plannedstmt->hasReturning)
				res = SPI_OK_UPDATE_RETURNING;
			else
				res = SPI_OK_UPDATE;
			break;
		default:
			return SPI_ERROR_OPUNKNOWN;
	}

#ifdef SPI_EXECUTOR_STATS
	if (ShowExecutorStats)
		ResetUsage();
#endif

	/* Select execution options */
	eflags = 0;					/* default run-to-completion flags */

	ExecutorStart(queryDesc, eflags);

	ExecutorRun(queryDesc, ForwardScanDirection, tcount, true);

	_SPI_current->processed = queryDesc->estate->es_processed;

	if ((res == SPI_OK_SELECT || queryDesc->plannedstmt->hasReturning) &&
		queryDesc->dest->mydest == DestSPI)
	{
		if (_SPI_checktuples())
			elog(ERROR, "consistency check on SPI tuple count failed");
	}

	ExecutorFinish(queryDesc);
	ExecutorEnd(queryDesc);
	/* FreeQueryDesc is done by the caller */

#ifdef SPI_EXECUTOR_STATS
	if (ShowExecutorStats)
		ShowUsage("SPI EXECUTOR STATS");
#endif

	return res;
}

/*
 * _SPI_error_callback
 *
 * Add context information when a query invoked via SPI fails
 */
static void
_SPI_error_callback(void *arg)
{
	SPICallbackArg *carg = (SPICallbackArg *) arg;
	const char *query = carg->query;
	int			syntaxerrposition;

	if (query == NULL)			/* in case arg wasn't set yet */
		return;

	/*
	 * If there is a syntax error position, convert to internal syntax error;
	 * otherwise treat the query as an item of context stack
	 */
	syntaxerrposition = geterrposition();
	if (syntaxerrposition > 0)
	{
		errposition(0);
		internalerrposition(syntaxerrposition);
		internalerrquery(query);
	}
	else
	{
		errcontext("SQL statement \"%s\"", query);
	}
}

static MemoryContext
_SPI_execmem(void)
{
	return MemoryContextSwitchTo(_SPI_current->execCxt);
}

static MemoryContext
_SPI_procmem(void)
{
	return MemoryContextSwitchTo(_SPI_current->procCxt);
}

/*
 * _SPI_begin_call: begin a SPI operation within a connected procedure
 *
 * use_exec is true if we intend to make use of the procedure's execCxt
 * during this SPI operation.  We'll switch into that context, and arrange
 * for it to be cleaned up at _SPI_end_call or if an error occurs.
 */
static int
_SPI_begin_call(bool use_exec)
{
	if (_SPI_current == NULL)
		return SPI_ERROR_UNCONNECTED;

	if (use_exec)
	{
		/* remember when the Executor operation started */
		_SPI_current->execSubid = GetCurrentSubTransactionId();
		/* switch to the Executor memory context */
		_SPI_execmem();
	}

	return 0;
}

/*
 * _SPI_end_call: end a SPI operation within a connected procedure
 *
 * use_exec must be the same as in the previous _SPI_begin_call
 *
 * Note: this currently has no failure return cases, so callers don't check
 */
static int
_SPI_end_call(bool use_exec)
{
	if (use_exec)
	{
		/* switch to the procedure memory context */
		_SPI_procmem();
		/* mark Executor context no longer in use */
		_SPI_current->execSubid = InvalidSubTransactionId;
		/* and free Executor memory */
		MemoryContextResetAndDeleteChildren(_SPI_current->execCxt);
	}

	return 0;
}

static bool
_SPI_checktuples(void)
{
	uint64		processed = _SPI_current->processed;
	SPITupleTable *tuptable = _SPI_current->tuptable;
	bool		failed = false;

	if (tuptable == NULL)		/* spi_dest_startup was not called */
		failed = true;
	else if (processed != tuptable->numvals)
		failed = true;

	return failed;
}

/*
 * Convert a "temporary" SPIPlan into a persistent plan.
 *
 * The passed _SPI_plan struct is on the stack.  Copy it into the SPI
 * procedure context so it will survive _SPI_end_call().
 */
static SPIPlanPtr
_SPI_make_plan_non_temp(SPIPlanPtr plan)
{
	SPIPlanPtr	newplan;
	MemoryContext parentcxt = _SPI_current->procCxt;
	MemoryContext plancxt;
	MemoryContext oldcxt;

	Assert(plan->magic == _SPI_PLAN_MAGIC);
	Assert(plan->plancxt == NULL);

	plancxt = AllocSetContextCreate(parentcxt,
									"SPI Plan",
									ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(plancxt);

	newplan = (SPIPlanPtr) palloc0(sizeof(_SPI_plan));
	newplan->magic = _SPI_PLAN_MAGIC;
	newplan->plancxt = plancxt;
	newplan->query_string = pstrdup(plan->query_string);
	newplan->parse_mode = plan->parse_mode;
	newplan->cursor_options = plan->cursor_options;
	newplan->nargs = plan->nargs;
	if (plan->nargs > 0)
	{
		newplan->argtypes = (Oid *) palloc(plan->nargs * sizeof(Oid));
		memcpy(newplan->argtypes, plan->argtypes, plan->nargs * sizeof(Oid));
	}
	else
		newplan->argtypes = NULL;
	newplan->parserSetup = plan->parserSetup;
	newplan->parserSetupArg = plan->parserSetupArg;

	MemoryContextSwitchTo(oldcxt);

	return newplan;
}

/*
 * Make a copy of the given plan.
 */
static SPIPlanPtr
_SPI_save_plan(SPIPlanPtr plan)
{
	SPIPlanPtr	newplan;
	MemoryContext plancxt;
	MemoryContext oldcxt;

	plancxt = AllocSetContextCreate(CurrentMemoryContext,
									"SPI Plan",
									ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(plancxt);

	newplan = (SPIPlanPtr) palloc0(sizeof(_SPI_plan));
	newplan->magic = _SPI_PLAN_MAGIC;
	newplan->plancxt = plancxt;
	newplan->query_string = pstrdup(plan->query_string);
	newplan->parse_mode = plan->parse_mode;
	newplan->cursor_options = plan->cursor_options;
	newplan->nargs = plan->nargs;
	if (plan->nargs > 0)
	{
		newplan->argtypes = (Oid *) palloc(plan->nargs * sizeof(Oid));
		memcpy(newplan->argtypes, plan->argtypes, plan->nargs * sizeof(Oid));
	}
	else
		newplan->argtypes = NULL;
	newplan->parserSetup = plan->parserSetup;
	newplan->parserSetupArg = plan->parserSetupArg;

	MemoryContextSwitchTo(oldcxt);

	return newplan;
}

/*
 * Internal lookup of ephemeral named relation by name.
 */
static EphemeralNamedRelation
_SPI_find_ENR_by_name(const char *name)
{
	/* internal static function; any error is bug in SPI itself */
	Assert(name != NULL);

	/* fast exit if no tuplestores have been added */
	if (_SPI_current->queryEnv == NULL)
		return NULL;

	return get_ENR(_SPI_current->queryEnv, name);
}

/*
 * Register an ephemeral named relation for use by the planner and executor on
 * subsequent calls using this SPI connection.
 */
int
SPI_register_relation(EphemeralNamedRelation enr)
{
	EphemeralNamedRelation match;
	int			res;

	if (enr == NULL || enr->md.name == NULL)
		return SPI_ERROR_ARGUMENT;

	res = _SPI_begin_call(false);	/* keep current memory context */
	if (res < 0)
		return res;

	match = _SPI_find_ENR_by_name(enr->md.name);
	if (match)
		res = SPI_ERROR_REL_DUPLICATE;
	else
	{
		if (_SPI_current->queryEnv == NULL)
			_SPI_current->queryEnv = create_queryEnv();

		register_ENR(_SPI_current->queryEnv, enr);
		res = SPI_OK_REL_REGISTER;
	}

	_SPI_end_call(false);

	return res;
}

/*
 * Unregister an ephemeral named relation by name.  This will probably be a
 * rarely used function, since SPI_finish will clear it automatically.
 */
int
SPI_unregister_relation(const char *name)
{
	EphemeralNamedRelation match;
	int			res;

	if (name == NULL)
		return SPI_ERROR_ARGUMENT;

	res = _SPI_begin_call(false);	/* keep current memory context */
	if (res < 0)
		return res;

	match = _SPI_find_ENR_by_name(name);
	if (match)
	{
		unregister_ENR(_SPI_current->queryEnv, match->md.name);
		res = SPI_OK_REL_UNREGISTER;
	}
	else
		res = SPI_ERROR_REL_NOT_FOUND;

	_SPI_end_call(false);

	return res;
}

