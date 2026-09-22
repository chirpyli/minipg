/*-------------------------------------------------------------------------
 *
 * pgstatfuncs.c
 *	  Functions for accessing backend status and progress data
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/pgstatfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "common/ip.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#define UINT32_ACCESS_ONCE(var)		 ((uint32)(*((volatile uint32 *)&(var))))

#define HAS_PGSTAT_PERMISSIONS(role)	 (true)

Datum
pg_stat_get_backend_idset(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	int		   *fctx;
	int32		result;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		fctx = MemoryContextAlloc(funcctx->multi_call_memory_ctx,
								  2 * sizeof(int));
		funcctx->user_fctx = fctx;

		fctx[0] = 0;
		fctx[1] = pgstat_fetch_stat_numbackends();
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();
	fctx = funcctx->user_fctx;

	fctx[0] += 1;
	result = fctx[0];

	if (result <= fctx[1])
	{
		/* do when there is more left to send */
		SRF_RETURN_NEXT(funcctx, Int32GetDatum(result));
	}
	else
	{
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
	}
}

/* Returns activity of PG backends. */

/*
 * Returns activity of PG backends.
 */
Datum
pg_stat_get_activity(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_ACTIVITY_COLS	23
	int			num_backends = pgstat_fetch_stat_numbackends();
	int			curr_backend;
	int			pid = PG_ARGISNULL(0) ? -1 : PG_GETARG_INT32(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

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

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/* 1-based index */
	for (curr_backend = 1; curr_backend <= num_backends; curr_backend++)
	{
		/* for each row */
		Datum		values[PG_STAT_GET_ACTIVITY_COLS];
		bool		nulls[PG_STAT_GET_ACTIVITY_COLS];
		LocalPgBackendStatus *local_beentry;
		PgBackendStatus *beentry;
		PGPROC	   *proc;
		const char *wait_event_type = NULL;
		const char *wait_event = NULL;

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		/* Get the next one in the list */
		local_beentry = pgstat_fetch_stat_local_beentry(curr_backend);
		if (!local_beentry)
		{
			int			i;

			/* Ignore missing entries if looking for specific PID */
			if (pid != -1)
				continue;

			for (i = 0; i < lengthof(nulls); i++)
				nulls[i] = true;

			nulls[5] = false;
			values[5] = CStringGetTextDatum("<backend information not available>");

			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
			continue;
		}

		beentry = &local_beentry->backendStatus;

		/* If looking for specific PID, ignore all the others */
		if (pid != -1 && beentry->st_procpid != pid)
			continue;

		/* Values available to all callers */
		if (beentry->st_databaseid != InvalidOid)
			values[0] = ObjectIdGetDatum(beentry->st_databaseid);
		else
			nulls[0] = true;

		values[1] = Int32GetDatum(beentry->st_procpid);

		if (beentry->st_userid != InvalidOid)
			values[2] = ObjectIdGetDatum(beentry->st_userid);
		else
			nulls[2] = true;

		if (beentry->st_appname)
			values[3] = CStringGetTextDatum(beentry->st_appname);
		else
			nulls[3] = true;

		if (TransactionIdIsValid(local_beentry->backend_xid))
			values[15] = TransactionIdGetDatum(local_beentry->backend_xid);
		else
			nulls[15] = true;

		if (TransactionIdIsValid(local_beentry->backend_xmin))
			values[16] = TransactionIdGetDatum(local_beentry->backend_xmin);
		else
			nulls[16] = true;

		/* Values only available to role member or pg_read_all_stats */
		if (HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		{
			SockAddr	zero_clientaddr;
			char	   *clipped_activity;

			switch (beentry->st_state)
			{
				case STATE_IDLE:
					values[4] = CStringGetTextDatum("idle");
					break;
				case STATE_RUNNING:
					values[4] = CStringGetTextDatum("active");
					break;
				case STATE_IDLEINTRANSACTION:
					values[4] = CStringGetTextDatum("idle in transaction");
					break;
				case STATE_IDLEINTRANSACTION_ABORTED:
					values[4] = CStringGetTextDatum("idle in transaction (aborted)");
					break;
				case STATE_DISABLED:
					values[4] = CStringGetTextDatum("disabled");
					break;
				case STATE_UNDEFINED:
					nulls[4] = true;
					break;
			}

			clipped_activity = pgstat_clip_activity(beentry->st_activity_raw);
			values[5] = CStringGetTextDatum(clipped_activity);
			pfree(clipped_activity);

			/* leader_pid */
			nulls[21] = true;

			proc = BackendPidGetProc(beentry->st_procpid);

			if (proc == NULL && (beentry->st_backendType != B_BACKEND))
			{
				/*
				 * For an auxiliary process, retrieve process info from
				 * AuxiliaryProcs stored in shared-memory.
				 */
				proc = AuxiliaryPidGetProc(beentry->st_procpid);
			}

			/*
			 * If a PGPROC entry was retrieved, display wait events and lock
			 * group leader information if any.  To avoid extra overhead, no
			 * extra lock is being held, so there is no guarantee of
			 * consistency across multiple rows.
			 */
			if (proc != NULL)
			{
				uint32		raw_wait_event;
				PGPROC	   *leader;

				raw_wait_event = UINT32_ACCESS_ONCE(proc->wait_event_info);
				wait_event_type = pgstat_get_wait_event_type(raw_wait_event);
				wait_event = pgstat_get_wait_event(raw_wait_event);

				leader = proc->lockGroupLeader;

				/*
				 * Show the leader only for active parallel workers.  This
				 * leaves the field as NULL for the leader of a parallel
				 * group.
				 */
				if (leader && leader->pid != beentry->st_procpid)
				{
					values[21] = Int32GetDatum(leader->pid);
					nulls[21] = false;
				}
			}

			if (wait_event_type)
				values[6] = CStringGetTextDatum(wait_event_type);
			else
				nulls[6] = true;

			if (wait_event)
				values[7] = CStringGetTextDatum(wait_event);
			else
				nulls[7] = true;

			/*
			 * Don't expose transaction time for walsenders; it confuses
			 * monitoring, particularly because we don't keep the time up-to-
			 * date.
			 */
			if (beentry->st_xact_start_timestamp != 0 &&
				beentry->st_backendType != B_WAL_SENDER)
				values[8] = TimestampTzGetDatum(beentry->st_xact_start_timestamp);
			else
				nulls[8] = true;

			if (beentry->st_activity_start_timestamp != 0)
				values[9] = TimestampTzGetDatum(beentry->st_activity_start_timestamp);
			else
				nulls[9] = true;

			if (beentry->st_proc_start_timestamp != 0)
				values[10] = TimestampTzGetDatum(beentry->st_proc_start_timestamp);
			else
				nulls[10] = true;

			if (beentry->st_state_start_timestamp != 0)
				values[11] = TimestampTzGetDatum(beentry->st_state_start_timestamp);
			else
				nulls[11] = true;

			/* A zeroed client addr means we don't know */
			memset(&zero_clientaddr, 0, sizeof(zero_clientaddr));
			if (memcmp(&(beentry->st_clientaddr), &zero_clientaddr,
					   sizeof(zero_clientaddr)) == 0)
			{
				nulls[12] = true;
				nulls[13] = true;
				nulls[14] = true;
			}
			else
			{
				if (beentry->st_clientaddr.addr.ss_family == AF_INET)
				{
					char		remote_host[NI_MAXHOST];
					char		remote_port[NI_MAXSERV];
					int			ret;

					remote_host[0] = '\0';
					remote_port[0] = '\0';
					ret = pg_getnameinfo_all(&beentry->st_clientaddr.addr,
											 beentry->st_clientaddr.salen,
											 remote_host, sizeof(remote_host),
											 remote_port, sizeof(remote_port),
											 NI_NUMERICHOST | NI_NUMERICSERV);
					if (ret == 0)
					{
					values[12] = CStringGetTextDatum(remote_host);
						if (beentry->st_clienthostname &&
							beentry->st_clienthostname[0])
							values[13] = CStringGetTextDatum(beentry->st_clienthostname);
						else
							nulls[13] = true;
						values[14] = Int32GetDatum(atoi(remote_port));
					}
					else
					{
						nulls[12] = true;
						nulls[13] = true;
						nulls[14] = true;
					}
				}
				else
				{
					/* Unknown address type, should never happen */
					nulls[12] = true;
					nulls[13] = true;
					nulls[14] = true;
				}
			}
			/* Add backend type */
			values[17] =
				CStringGetTextDatum(GetBackendTypeDesc(beentry->st_backendType));

			/* GSSAPI information (not supported in minipg) */
			values[18] = BoolGetDatum(false);	/* gss_auth */
			nulls[19] = true;	/* No GSS principal */
			values[20] = BoolGetDatum(false);	/* GSS Encryption not in
												 * use */
			if (beentry->st_query_id == 0)
				nulls[22] = true;
			else
				values[22] = UInt64GetDatum(beentry->st_query_id);
		}
		else
		{
			/* No permissions to view data about this session */
			values[5] = CStringGetTextDatum("<insufficient privilege>");
			nulls[4] = true;
			nulls[6] = true;
			nulls[7] = true;
			nulls[8] = true;
			nulls[9] = true;
			nulls[10] = true;
			nulls[11] = true;
			nulls[12] = true;
			nulls[13] = true;
			nulls[14] = true;
			nulls[17] = true;
			nulls[18] = true;
			nulls[19] = true;
			nulls[20] = true;
			nulls[21] = true;
			nulls[22] = true;
		}

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);

		/* If only a single backend was requested, and we found it, break. */
		if (pid != -1)
			break;
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}


Datum
pg_backend_pid(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(MyProcPid);
}


Datum
pg_stat_get_backend_pid(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_INT32(beentry->st_procpid);
}


Datum
pg_stat_get_backend_dbid(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_OID(beentry->st_databaseid);
}


Datum
pg_stat_get_backend_userid(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_OID(beentry->st_userid);
}


Datum
pg_stat_get_backend_activity(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	const char *activity;
	char	   *clipped_activity;
	text	   *ret;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		activity = "<backend information not available>";
	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		activity = "<insufficient privilege>";
	else if (*(beentry->st_activity_raw) == '\0')
		activity = "<command string not enabled>";
	else
		activity = beentry->st_activity_raw;

	clipped_activity = pgstat_clip_activity(activity);
	ret = cstring_to_text(clipped_activity);
	pfree(clipped_activity);

	PG_RETURN_TEXT_P(ret);
}

Datum
pg_stat_get_backend_wait_event_type(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	PGPROC	   *proc;
	const char *wait_event_type = NULL;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		wait_event_type = "<backend information not available>";
	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		wait_event_type = "<insufficient privilege>";
	else
	{
		proc = BackendPidGetProc(beentry->st_procpid);
		if (!proc)
			proc = AuxiliaryPidGetProc(beentry->st_procpid);
		if (proc)
			wait_event_type = pgstat_get_wait_event_type(proc->wait_event_info);
	}

	if (!wait_event_type)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(wait_event_type));
}

Datum
pg_stat_get_backend_wait_event(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	PGPROC	   *proc;
	const char *wait_event = NULL;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		wait_event = "<backend information not available>";
	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		wait_event = "<insufficient privilege>";
	else
	{
		proc = BackendPidGetProc(beentry->st_procpid);
		if (!proc)
			proc = AuxiliaryPidGetProc(beentry->st_procpid);
		if (proc)
			wait_event = pgstat_get_wait_event(proc->wait_event_info);
	}

	if (!wait_event)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(wait_event));
}


Datum
pg_stat_get_backend_activity_start(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	TimestampTz result;
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		PG_RETURN_NULL();

	result = beentry->st_activity_start_timestamp;

	/*
	 * No time recorded for start of current query -- this is the case if the
	 * user hasn't enabled query-level stats collection.
	 */
	if (result == 0)
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(result);
}


Datum
pg_stat_get_backend_xact_start(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	TimestampTz result;
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		PG_RETURN_NULL();

	result = beentry->st_xact_start_timestamp;

	if (result == 0)			/* not in a transaction */
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(result);
}


Datum
pg_stat_get_backend_start(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	TimestampTz result;
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		PG_RETURN_NULL();

	result = beentry->st_proc_start_timestamp;

	if (result == 0)			/* probably can't happen? */
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(result);
}


Datum
pg_stat_get_backend_client_port(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	SockAddr	zero_clientaddr;
	char		remote_port[NI_MAXSERV];
	int			ret;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		PG_RETURN_NULL();

	/* A zeroed client addr means we don't know */
	memset(&zero_clientaddr, 0, sizeof(zero_clientaddr));
	if (memcmp(&(beentry->st_clientaddr), &zero_clientaddr,
			   sizeof(zero_clientaddr)) == 0)
		PG_RETURN_NULL();

	switch (beentry->st_clientaddr.addr.ss_family)
	{
		case AF_INET:
			break;
		default:
			PG_RETURN_NULL();
	}

	remote_port[0] = '\0';
	ret = pg_getnameinfo_all(&beentry->st_clientaddr.addr,
							 beentry->st_clientaddr.salen,
							 NULL, 0,
							 remote_port, sizeof(remote_port),
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(DirectFunctionCall1(int4in,
										CStringGetDatum(remote_port)));
}


Datum
pg_stat_get_db_numbackends(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int32		result;
	int			tot_backends = pgstat_fetch_stat_numbackends();
	int			beid;

	result = 0;
	for (beid = 1; beid <= tot_backends; beid++)
	{
		PgBackendStatus *beentry = pgstat_fetch_stat_beentry(beid);

		if (beentry && beentry->st_databaseid == dbid)
			result++;
	}

	PG_RETURN_INT32(result);
}
