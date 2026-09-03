/*-------------------------------------------------------------------------
 *
 * explain.c
 *	  Explain query execution plans
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/explain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/nodeHash.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "storage/bufmgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc_tables.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"


/* Hook for plugins to get control in ExplainOneQuery() */
ExplainOneQuery_hook_type ExplainOneQuery_hook = NULL;

/* Hook for plugins to get control in explain_get_index_name() */
explain_get_index_name_hook_type explain_get_index_name_hook = NULL;


static void ExplainOneQuery(Query *query, int cursorOptions,
							ExplainState *es,
							const char *queryString, ParamListInfo params,
							QueryEnvironment *queryEnv);
static double elapsed_time(instr_time *starttime);
static bool ExplainPreScanNode(PlanState *planstate, Bitmapset **rels_used);
static void ExplainNode(PlanState *planstate, List *ancestors,
						const char *relationship, const char *plan_name,
						ExplainState *es);
static void show_plan_tlist(PlanState *planstate, List *ancestors,
							ExplainState *es);
static void show_expression(Node *node, const char *qlabel,
							PlanState *planstate, List *ancestors,
							bool useprefix, ExplainState *es);
static void show_qual(List *qual, const char *qlabel,
					  PlanState *planstate, List *ancestors,
					  bool useprefix, ExplainState *es);
static void show_scan_qual(List *qual, const char *qlabel,
						   PlanState *planstate, List *ancestors,
						   ExplainState *es);
static void show_upper_qual(List *qual, const char *qlabel,
							PlanState *planstate, List *ancestors,
							ExplainState *es);
static void show_sort_keys(SortState *sortstate, List *ancestors,
						   ExplainState *es);
static void show_incremental_sort_keys(IncrementalSortState *incrsortstate,
									   List *ancestors, ExplainState *es);
static void show_merge_append_keys(MergeAppendState *mstate, List *ancestors,
								   ExplainState *es);
static void show_agg_keys(AggState *astate, List *ancestors,
						  ExplainState *es);
static void show_grouping_sets(PlanState *planstate, Agg *agg,
							   List *ancestors, ExplainState *es);
static void show_grouping_set_keys(PlanState *planstate,
								   Agg *aggnode, Sort *sortnode,
								   List *context, bool useprefix,
								   List *ancestors, ExplainState *es);
static void show_group_keys(GroupState *gstate, List *ancestors,
							ExplainState *es);
static void show_sort_group_keys(PlanState *planstate, const char *qlabel,
								 int nkeys, int nPresortedKeys, AttrNumber *keycols,
								 Oid *sortOperators, Oid *collations, bool *nullsFirst,
								 List *ancestors, ExplainState *es);
static void show_sortorder_options(StringInfo buf, Node *sortexpr,
								   Oid sortOperator, Oid collation, bool nullsFirst);
static void show_tablesample(TableSampleClause *tsc, PlanState *planstate,
							 List *ancestors, ExplainState *es);
static void show_sort_info(SortState *sortstate, ExplainState *es);
static void show_incremental_sort_info(IncrementalSortState *incrsortstate,
									   ExplainState *es);
static void show_hash_info(HashState *hashstate, ExplainState *es);
static void show_memoize_info(MemoizeState *mstate, List *ancestors,
							  ExplainState *es);
static void show_hashagg_info(AggState *hashstate, ExplainState *es);
static void show_tidbitmap_info(BitmapHeapScanState *planstate,
								ExplainState *es);
static void show_instrumentation_count(const char *qlabel, int which,
									   PlanState *planstate, ExplainState *es);
static void show_eval_params(Bitmapset *bms_params, ExplainState *es);
static const char *explain_get_index_name(Oid indexId);
static void show_buffer_usage(ExplainState *es, const BufferUsage *usage,
							  bool planning);
static void show_wal_usage(ExplainState *es, const WalUsage *usage);
static void ExplainIndexScanDetails(Oid indexid, ScanDirection indexorderdir,
									ExplainState *es);
static void ExplainScanTarget(Scan *plan, ExplainState *es);
static void ExplainModifyTarget(ModifyTable *plan, ExplainState *es);
static void ExplainTargetRel(Plan *plan, Index rti, ExplainState *es);
static void show_modifytable_info(ModifyTableState *mtstate, List *ancestors,
								  ExplainState *es);
static void ExplainMemberNodes(PlanState **planstates, int nplans,
							   List *ancestors, ExplainState *es);
static void ExplainMissingMembers(int nplans, int nchildren, ExplainState *es);
static void ExplainSubPlans(List *plans, List *ancestors,
							const char *relationship, ExplainState *es);
static void ExplainCustomChildren(CustomScanState *css,
								  List *ancestors, ExplainState *es);
static ExplainWorkersState *ExplainCreateWorkersState(int num_workers);
static void ExplainOpenWorker(int n, ExplainState *es);
static void ExplainCloseWorker(int n, ExplainState *es);
static void ExplainFlushWorkersState(ExplainState *es);
static void ExplainProperty(const char *qlabel, const char *unit,
							const char *value, ExplainState *es);
static void ExplainIndentText(ExplainState *es);


/*
 * ExplainQuery -
 *	  execute an EXPLAIN command
 */
void
ExplainQuery(ParseState *pstate, ExplainStmt *stmt,
			 ParamListInfo params, DestReceiver *dest)
{
	ExplainState *es = NewExplainState();
	TupOutputState *tstate;
	JumbleState *jstate = NULL;
	Query	   *query;
	List	   *rewritten;
	ListCell   *lc;
	bool		timing_set = false;
	bool		summary_set = false;

	/* Parse options list. */
	foreach(lc, stmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "analyze") == 0)
			es->analyze = defGetBoolean(opt);
		else if (strcmp(opt->defname, "verbose") == 0)
			es->verbose = defGetBoolean(opt);
		else if (strcmp(opt->defname, "costs") == 0)
			es->costs = defGetBoolean(opt);
		else if (strcmp(opt->defname, "buffers") == 0)
			es->buffers = defGetBoolean(opt);
		else if (strcmp(opt->defname, "wal") == 0)
			es->wal = defGetBoolean(opt);
		else if (strcmp(opt->defname, "settings") == 0)
			es->settings = defGetBoolean(opt);
		else if (strcmp(opt->defname, "timing") == 0)
		{
			timing_set = true;
			es->timing = defGetBoolean(opt);
		}
		else if (strcmp(opt->defname, "summary") == 0)
		{
			summary_set = true;
			es->summary = defGetBoolean(opt);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized EXPLAIN option \"%s\"",
							opt->defname),
					 parser_errposition(pstate, opt->location)));
	}

	if (es->wal && !es->analyze)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("EXPLAIN option WAL requires ANALYZE")));

	/* if the timing was not set explicitly, set default value */
	es->timing = (timing_set) ? es->timing : es->analyze;

	/* check that timing is used with EXPLAIN ANALYZE */
	if (es->timing && !es->analyze)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("EXPLAIN option TIMING requires ANALYZE")));

	/* if the summary was not set explicitly, set default value */
	es->summary = (summary_set) ? es->summary : es->analyze;

	query = castNode(Query, stmt->query);
	if (IsQueryIdEnabled())
		jstate = JumbleQuery(query, pstate->p_sourcetext);

	/*
	 * Parse analysis was done already, but we still have to run the rule
	 * rewriter.  We do not do AcquireRewriteLocks: we assume the query either
	 * came straight from the parser, or suitable locks were acquired by
	 * the caller.
	 */
	rewritten = QueryRewrite(castNode(Query, stmt->query));

	if (rewritten == NIL)
	{
		/*
		 * In the case of an INSTEAD NOTHING, tell at least that.
		 */
		appendStringInfoString(es->str, "Query rewrites to nothing\n");
	}
	else
	{
		ListCell   *l;

		/* Explain every plan */
		foreach(l, rewritten)
		{
			ExplainOneQuery(lfirst_node(Query, l),
							CURSOR_OPT_PARALLEL_OK, es,
							pstate->p_sourcetext, params, pstate->p_queryEnv);

			/* Separate plans with a blank line */
			if (lnext(rewritten, l) != NULL)
				appendStringInfoChar(es->str, '\n');
		}
	}

	Assert(es->indent == 0);

	/* output tuples */
	tstate = begin_tup_output_tupdesc(dest, ExplainResultDesc(stmt),
									  &TTSOpsVirtual);
	do_text_output_multiline(tstate, es->str->data);
	end_tup_output(tstate);

	pfree(es->str->data);
}

/*
 * Create a new ExplainState struct initialized with default options.
 */
ExplainState *
NewExplainState(void)
{
	ExplainState *es = (ExplainState *) palloc0(sizeof(ExplainState));

	/* Set default options (most fields can be left as zeroes). */
	es->costs = true;
	/* Prepare output buffer. */
	es->str = makeStringInfo();

	return es;
}

/*
 * ExplainResultDesc -
 *	  construct the result tupledesc for an EXPLAIN
 */
TupleDesc
ExplainResultDesc(ExplainStmt *stmt)
{
	TupleDesc	tupdesc;
	Oid			result_type = TEXTOID;

	tupdesc = CreateTemplateTupleDesc(1);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "QUERY PLAN",
					   result_type, -1, 0);
	return tupdesc;
}

/*
 * ExplainOneQuery -
 *	  print out the execution plan for one Query
 */
static void
ExplainOneQuery(Query *query, int cursorOptions,
				ExplainState *es,
				const char *queryString, ParamListInfo params,
				QueryEnvironment *queryEnv)
{
	/* planner will not cope with utility statements */
	if (query->commandType == CMD_UTILITY)
	{
		ExplainOneUtility(query->utilityStmt, es, queryString, params,
						  queryEnv);
		return;
	}

	/* if an advisor plugin is present, let it manage things */
	if (ExplainOneQuery_hook)
		(*ExplainOneQuery_hook) (query, cursorOptions, es,
								 queryString, params, queryEnv);
	else
	{
		PlannedStmt *plan;
		instr_time	planstart,
					planduration;
		BufferUsage bufusage_start,
					bufusage;

		if (es->buffers)
			bufusage_start = pgBufferUsage;
		INSTR_TIME_SET_CURRENT(planstart);

		/* plan the query */
		plan = pg_plan_query(query, queryString, cursorOptions, params);

		INSTR_TIME_SET_CURRENT(planduration);
		INSTR_TIME_SUBTRACT(planduration, planstart);

		/* calc differences of buffer counters. */
		if (es->buffers)
		{
			memset(&bufusage, 0, sizeof(BufferUsage));
			BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);
		}

		/* run it (if needed) and produce output */
		ExplainOnePlan(plan, es, queryString, params, queryEnv,
					   &planduration, (es->buffers ? &bufusage : NULL));
	}
}

/*
 * ExplainOneUtility -
 *	  print out the execution plan for one utility statement
 *	  (Utility statements have no plan structure after removing EXPLAIN EXECUTE)
 */
void
ExplainOneUtility(Node *utilityStmt, ExplainState *es,
				  const char *queryString, ParamListInfo params,
				  QueryEnvironment *queryEnv)
{
	if (utilityStmt == NULL)
		return;

	appendStringInfoString(es->str,
						   "Utility statements have no plan structure\n");
}

/*
 * ExplainOnePlan -
 *		given a planned query, execute it if needed, and then print
 *		EXPLAIN output
 *
 * This is exported because an index advisor plugin would need
 * to call it.
 */
void
ExplainOnePlan(PlannedStmt *plannedstmt, ExplainState *es,
			   const char *queryString, ParamListInfo params,
			   QueryEnvironment *queryEnv, const instr_time *planduration,
			   const BufferUsage *bufusage)
{
	DestReceiver *dest;
	QueryDesc  *queryDesc;
	instr_time	starttime;
	double		totaltime = 0;
	int			eflags;
	int			instrument_option = 0;

	Assert(plannedstmt->commandType != CMD_UTILITY);

	if (es->analyze && es->timing)
		instrument_option |= INSTRUMENT_TIMER;
	else if (es->analyze)
		instrument_option |= INSTRUMENT_ROWS;

	if (es->buffers)
		instrument_option |= INSTRUMENT_BUFFERS;
	if (es->wal)
		instrument_option |= INSTRUMENT_WAL;

	/*
	 * We always collect timing for the entire statement, even when node-level
	 * timing is off, so we don't look at es->timing here.  (We could skip
	 * this if !es->summary, but it's hardly worth the complication.)
	 */
	INSTR_TIME_SET_CURRENT(starttime);

	/*
	 * Use a snapshot with an updated command ID to ensure this query sees
	 * results of any previously executed queries.
	 */
	PushCopiedSnapshot(GetActiveSnapshot());
	UpdateActiveSnapshotCommandId();

	/*
	 * Normally we discard the query's output.
	 */
	dest = None_Receiver;

	/* Create a QueryDesc for the query */
	queryDesc = CreateQueryDesc(plannedstmt, queryString,
								GetActiveSnapshot(), InvalidSnapshot,
								dest, params, queryEnv, instrument_option);

	/* Select execution options */
	if (es->analyze)
		eflags = 0;				/* default run-to-completion flags */
	else
		eflags = EXEC_FLAG_EXPLAIN_ONLY;

	/* call ExecutorStart to prepare the plan for execution */
	ExecutorStart(queryDesc, eflags);

	/* Execute the plan for statistics if asked for */
	if (es->analyze)
	{
		ScanDirection dir;

		dir = ForwardScanDirection;

		/* run the plan */
		ExecutorRun(queryDesc, dir, 0L, true);

		/* run cleanup too */
		ExecutorFinish(queryDesc);

		/* We can't run ExecutorEnd 'till we're done printing the stats... */
		totaltime += elapsed_time(&starttime);
	}

	/* Create textual dump of plan tree */
	ExplainPrintPlan(es, queryDesc);

	/*
	 * COMPUTE_QUERY_ID_REGRESS means COMPUTE_QUERY_ID_AUTO, but we don't show
	 * the queryid in any of the EXPLAIN plans to keep stable the results
	 * generated by regression test suites.
	 */
	if (es->verbose && plannedstmt->queryId != UINT64CONST(0) &&
		compute_query_id != COMPUTE_QUERY_ID_REGRESS)
	{
		/*
		 * Output the queryid as an int64 rather than a uint64 so we match
		 * what would be seen in the BIGINT pg_stat_statements.queryid column.
		 */
		ExplainPropertyInteger("Query Identifier", NULL, (int64)
							   plannedstmt->queryId, es);
	}

	/* Show buffer usage in planning */
	if (bufusage)
		show_buffer_usage(es, bufusage, true);

	if (es->summary && planduration)
	{
		double		plantime = INSTR_TIME_GET_DOUBLE(*planduration);

		ExplainPropertyFloat("Planning Time", "ms", 1000.0 * plantime, 3, es);
	}

	/*
	 * Close down the query and free resources.  Include time for this in the
	 * total execution time (although it should be pretty minimal).
	 */
	INSTR_TIME_SET_CURRENT(starttime);

	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);

	PopActiveSnapshot();

	/* We need a CCI just in case query expanded to multiple plans */
	if (es->analyze)
		CommandCounterIncrement();

	totaltime += elapsed_time(&starttime);

	/*
	 * We only report execution time if we actually ran the query (that is,
	 * the user specified ANALYZE), and if summary reporting is enabled (the
	 * user can set SUMMARY OFF to not have the timing information included in
	 * the output).  By default, ANALYZE sets SUMMARY to true.
	 */
	if (es->summary && es->analyze)
		ExplainPropertyFloat("Execution Time", "ms", 1000.0 * totaltime, 3,
							 es);
}

/*
 * ExplainPrintSettings -
 *    Print summary of modified settings affecting query planning.
 */
static void
ExplainPrintSettings(ExplainState *es)
{
	int			num;
	struct config_generic **gucs;
	StringInfoData str;

	/* bail out if information about settings not requested */
	if (!es->settings)
		return;

	/* request an array of relevant settings */
	gucs = get_explain_guc_options(&num);

	/* print nothing if there are no options */
	if (num <= 0)
		return;

	initStringInfo(&str);

	for (int i = 0; i < num; i++)
	{
		char	   *setting;
		struct config_generic *conf = gucs[i];

		if (i > 0)
			appendStringInfoString(&str, ", ");

		setting = GetConfigOptionByName(conf->name, NULL, true);

		if (setting)
			appendStringInfo(&str, "%s = '%s'", conf->name, setting);
		else
			appendStringInfo(&str, "%s = NULL", conf->name);
	}

	ExplainPropertyText("Settings", str.data, es);
}

/*
 * ExplainPrintPlan -
 *	  convert a QueryDesc's plan tree to text and append it to es->str
 *
 * The caller should have set up the options fields of *es, as well as
 * initializing the output buffer es->str.  Also, output formatting state
 * such as the indent level is assumed valid.  Plan-tree-specific fields
 * in *es are initialized here.
 *
 * NB: will not work on utility statements
 */
void
ExplainPrintPlan(ExplainState *es, QueryDesc *queryDesc)
{
	Bitmapset  *rels_used = NULL;
	PlanState  *ps;

	/* Set up ExplainState fields associated with this plan tree */
	Assert(queryDesc->plannedstmt != NULL);
	es->pstmt = queryDesc->plannedstmt;
	es->rtable = queryDesc->plannedstmt->rtable;
	ExplainPreScanNode(queryDesc->planstate, &rels_used);
	es->rtable_names = select_rtable_names_for_explain(es->rtable, rels_used);
	es->deparse_cxt = deparse_context_for_plan_tree(queryDesc->plannedstmt,
													es->rtable_names);
	es->printed_subplans = NULL;

	/*
	 * Sometimes we mark a Gather node as "invisible", which means that it's
	 * not to be displayed in EXPLAIN output.  The purpose of this is to allow
	 * running regression tests with force_parallel_mode=regress to get the
	 * same results as running the same tests with force_parallel_mode=off.
	 * Such marking is currently only supported on a Gather at the top of the
	 * plan.  We skip that node, and we must also hide per-worker detail data
	 * further down in the plan tree.
	 */
	ps = queryDesc->planstate;
	if (IsA(ps, GatherState) && ((Gather *) ps->plan)->invisible)
	{
		ps = outerPlanState(ps);
		es->hide_workers = true;
	}
	ExplainNode(ps, NIL, NULL, NULL, es);

	/*
	 * If requested, include information about GUC parameters with values that
	 * don't match the built-in defaults.
	 */
	ExplainPrintSettings(es);
}

/* Compute elapsed time in seconds since given timestamp */
static double
elapsed_time(instr_time *starttime)
{
	instr_time	endtime;

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_SUBTRACT(endtime, *starttime);
	return INSTR_TIME_GET_DOUBLE(endtime);
}

/*
 * ExplainPreScanNode -
 *	  Prescan the planstate tree to identify which RTEs are referenced
 *
 * Adds the relid of each referenced RTE to *rels_used.  The result controls
 * which RTEs are assigned aliases by select_rtable_names_for_explain.
 * This ensures that we don't confusingly assign un-suffixed aliases to RTEs
 * that never appear in the EXPLAIN output (such as inheritance parents).
 */
static bool
ExplainPreScanNode(PlanState *planstate, Bitmapset **rels_used)
{
	Plan	   *plan = planstate->plan;

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_ValuesScan:
		case T_NamedTuplestoreScan:
			*rels_used = bms_add_member(*rels_used,
										((Scan *) plan)->scanrelid);
			break;
		case T_CustomScan:
			*rels_used = bms_add_members(*rels_used,
										 ((CustomScan *) plan)->custom_relids);
			break;
		case T_ModifyTable:
			*rels_used = bms_add_member(*rels_used,
										((ModifyTable *) plan)->nominalRelation);
			if (((ModifyTable *) plan)->exclRelRTI)
				*rels_used = bms_add_member(*rels_used,
											((ModifyTable *) plan)->exclRelRTI);
			break;
		case T_Append:
			*rels_used = bms_add_members(*rels_used,
										 ((Append *) plan)->apprelids);
			break;
		case T_MergeAppend:
			*rels_used = bms_add_members(*rels_used,
										 ((MergeAppend *) plan)->apprelids);
			break;
		default:
			break;
	}

	return planstate_tree_walker(planstate, ExplainPreScanNode, rels_used);
}

/*
 * ExplainNode -
 *	  Appends a description of a plan tree to es->str
 *
 * planstate points to the executor state node for the current plan node.
 * We need to work from a PlanState node, not just a Plan node, in order to
 * get at the instrumentation data (if any) as well as the list of subplans.
 *
 * ancestors is a list of parent Plan and SubPlan nodes, most-closely-nested
 * first.  These are needed in order to interpret PARAM_EXEC Params.
 *
 * relationship describes the relationship of this plan node to its parent
 * (eg, "Outer", "Inner"); it can be null at top level.  plan_name is an
 * optional name to be attached to the node.
 *
 * es->indent is controlled in this function since we only want it to change
 * at plan-node boundaries (but a few subroutines will transiently increment
 * it).
 */
static void
ExplainNode(PlanState *planstate, List *ancestors,
			const char *relationship, const char *plan_name,
			ExplainState *es)
{
	Plan	   *plan = planstate->plan;
	const char *pname;			/* node type name for text output */
	const char *custom_name = NULL;
	ExplainWorkersState *save_workers_state = es->workers_state;
	int			save_indent = es->indent;
	bool		haschildren;

	/*
	 * Prepare per-worker output buffers, if needed.  We'll append the data in
	 * these to the main output string further down.
	 */
	if (planstate->worker_instrument && es->analyze && !es->hide_workers)
		es->workers_state = ExplainCreateWorkersState(planstate->worker_instrument->num_workers);
	else
		es->workers_state = NULL;

	/* Identify plan node type, and print generic details */
	switch (nodeTag(plan))
	{
		case T_Result:
			pname = "Result";
			break;
		case T_ProjectSet:
			pname = "ProjectSet";
			break;
		case T_ModifyTable:
			switch (((ModifyTable *) plan)->operation)
			{
				case CMD_INSERT:
					pname = "Insert";
					break;
				case CMD_UPDATE:
					pname = "Update";
					break;
				case CMD_DELETE:
					pname = "Delete";
					break;
				default:
					pname = "???";
					break;
			}
			break;
		case T_Append:
			pname = "Append";
			break;
		case T_MergeAppend:
			pname = "Merge Append";
			break;
		case T_BitmapAnd:
			pname = "BitmapAnd";
			break;
		case T_BitmapOr:
			pname = "BitmapOr";
			break;
		case T_NestLoop:
			pname = "Nested Loop";
			break;
		case T_MergeJoin:
			pname = "Merge";	/* "Join" gets added by jointype switch */
			break;
		case T_HashJoin:
			pname = "Hash";		/* "Join" gets added by jointype switch */
			break;
		case T_SeqScan:
			pname = "Seq Scan";
			break;
		case T_SampleScan:
			pname = "Sample Scan";
			break;
		case T_Gather:
			pname = "Gather";
			break;
		case T_GatherMerge:
			pname = "Gather Merge";
			break;
		case T_IndexScan:
			pname = "Index Scan";
			break;
		case T_IndexOnlyScan:
			pname = "Index Only Scan";
			break;
		case T_BitmapIndexScan:
			pname = "Bitmap Index Scan";
			break;
		case T_BitmapHeapScan:
			pname = "Bitmap Heap Scan";
			break;
		case T_TidScan:
			pname = "Tid Scan";
			break;
		case T_TidRangeScan:
			pname = "Tid Range Scan";
			break;
		case T_SubqueryScan:
			pname = "Subquery Scan";
			break;
		case T_FunctionScan:
			pname = "Function Scan";
			break;
		case T_ValuesScan:
			pname = "Values Scan";
			break;
		case T_NamedTuplestoreScan:
			pname = "Named Tuplestore Scan";
			break;
		case T_CustomScan:
			custom_name = ((CustomScan *) plan)->methods->CustomName;
			if (custom_name)
				pname = psprintf("Custom Scan (%s)", custom_name);
			else
				pname = "Custom Scan";
			break;
		case T_Material:
			pname = "Materialize";
			break;
		case T_Memoize:
			pname = "Memoize";
			break;
		case T_Sort:
			pname = "Sort";
			break;
		case T_IncrementalSort:
			pname = "Incremental Sort";
			break;
		case T_Group:
			pname = "Group";
			break;
		case T_Agg:
			{
				Agg		   *agg = (Agg *) plan;

				switch (agg->aggstrategy)
				{
					case AGG_PLAIN:
						pname = "Aggregate";
						break;
					case AGG_SORTED:
						pname = "GroupAggregate";
						break;
					case AGG_HASHED:
						pname = "HashAggregate";
						break;
					case AGG_MIXED:
						pname = "MixedAggregate";
						break;
					default:
						pname = "Aggregate ???";
						break;
				}

				if (DO_AGGSPLIT_SKIPFINAL(agg->aggsplit))
					pname = psprintf("Partial %s", pname);
				else if (DO_AGGSPLIT_COMBINE(agg->aggsplit))
					pname = psprintf("Finalize %s", pname);
			}
			break;
		case T_Unique:
			pname = "Unique";
			break;
		case T_LockRows:
			pname = "LockRows";
			break;
		case T_Limit:
			pname = "Limit";
			break;
		case T_Hash:
			pname = "Hash";
			break;
		default:
			pname = "???";
			break;
	}

	if (plan_name)
	{
		ExplainIndentText(es);
		appendStringInfo(es->str, "%s\n", plan_name);
		es->indent++;
	}
	if (es->indent)
	{
		ExplainIndentText(es);
		appendStringInfoString(es->str, "->  ");
		es->indent += 2;
	}
	if (plan->parallel_aware)
		appendStringInfoString(es->str, "Parallel ");
	appendStringInfoString(es->str, pname);
	es->indent++;

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_ValuesScan:
			ExplainScanTarget((Scan *) plan, es);
			break;
		case T_CustomScan:
			if (((Scan *) plan)->scanrelid > 0)
				ExplainScanTarget((Scan *) plan, es);
			break;
		case T_IndexScan:
			{
				IndexScan  *indexscan = (IndexScan *) plan;

				ExplainIndexScanDetails(indexscan->indexid,
										indexscan->indexorderdir,
										es);
				ExplainScanTarget((Scan *) indexscan, es);
			}
			break;
		case T_IndexOnlyScan:
			{
				IndexOnlyScan *indexonlyscan = (IndexOnlyScan *) plan;

				ExplainIndexScanDetails(indexonlyscan->indexid,
										indexonlyscan->indexorderdir,
										es);
				ExplainScanTarget((Scan *) indexonlyscan, es);
			}
			break;
		case T_BitmapIndexScan:
			{
				BitmapIndexScan *bitmapindexscan = (BitmapIndexScan *) plan;
				const char *indexname =
				explain_get_index_name(bitmapindexscan->indexid);

				appendStringInfo(es->str, " on %s",
								 quote_identifier(indexname));
			}
			break;
		case T_ModifyTable:
			ExplainModifyTarget((ModifyTable *) plan, es);
			break;
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			{
				const char *jointype;

				switch (((Join *) plan)->jointype)
				{
					case JOIN_INNER:
						jointype = "Inner";
						break;
					case JOIN_LEFT:
						jointype = "Left";
						break;
					case JOIN_FULL:
						jointype = "Full";
						break;
					case JOIN_RIGHT:
						jointype = "Right";
						break;
					case JOIN_SEMI:
						jointype = "Semi";
						break;
					case JOIN_ANTI:
						jointype = "Anti";
						break;
					default:
						jointype = "???";
						break;
				}

				/*
				 * For historical reasons, the join type is interpolated
				 * into the node type name...
				 */
				if (((Join *) plan)->jointype != JOIN_INNER)
					appendStringInfo(es->str, " %s Join", jointype);
				else if (!IsA(plan, NestLoop))
					appendStringInfoString(es->str, " Join");
			}
			break;
		default:
			break;
	}

	if (es->costs)
	{
		appendStringInfo(es->str, "  (cost=%.2f..%.2f rows=%.0f width=%d)",
						 plan->startup_cost, plan->total_cost,
						 plan->plan_rows, plan->plan_width);
	}

	/*
	 * We have to forcibly clean up the instrumentation state because we
	 * haven't done ExecutorEnd yet.  This is pretty grotty ...
	 *
	 * Note: contrib/auto_explain could cause instrumentation to be set up
	 * even though we didn't ask for it here.  Be careful not to print any
	 * instrumentation results the user didn't ask for.  But we do the
	 * InstrEndLoop call anyway, if possible, to reduce the number of cases
	 * auto_explain has to contend with.
	 */
	if (planstate->instrument)
		InstrEndLoop(planstate->instrument);

	if (es->analyze &&
		planstate->instrument && planstate->instrument->nloops > 0)
	{
		double		nloops = planstate->instrument->nloops;
		double		startup_ms = 1000.0 * planstate->instrument->startup / nloops;
		double		total_ms = 1000.0 * planstate->instrument->total / nloops;
		double		rows = planstate->instrument->ntuples / nloops;

		if (es->timing)
			appendStringInfo(es->str,
							 " (actual time=%.3f..%.3f rows=%.0f loops=%.0f)",
							 startup_ms, total_ms, rows, nloops);
		else
			appendStringInfo(es->str,
							 " (actual rows=%.0f loops=%.0f)",
							 rows, nloops);
	}
	else if (es->analyze)
	{
		appendStringInfoString(es->str, " (never executed)");
	}

	/* first line ends here */
	appendStringInfoChar(es->str, '\n');

	/* prepare per-worker general execution details */
	if (es->workers_state && es->verbose)
	{
		WorkerInstrumentation *w = planstate->worker_instrument;

		for (int n = 0; n < w->num_workers; n++)
		{
			Instrumentation *instrument = &w->instrument[n];
			double		nloops = instrument->nloops;
			double		startup_ms;
			double		total_ms;
			double		rows;

			if (nloops <= 0)
				continue;
			startup_ms = 1000.0 * instrument->startup / nloops;
			total_ms = 1000.0 * instrument->total / nloops;
			rows = instrument->ntuples / nloops;

			ExplainOpenWorker(n, es);

			ExplainIndentText(es);
			if (es->timing)
				appendStringInfo(es->str,
								 "actual time=%.3f..%.3f rows=%.0f loops=%.0f\n",
								 startup_ms, total_ms, rows, nloops);
			else
				appendStringInfo(es->str,
								 "actual rows=%.0f loops=%.0f\n",
								 rows, nloops);

			ExplainCloseWorker(n, es);
		}
	}

	/* target list */
	if (es->verbose)
		show_plan_tlist(planstate, ancestors, es);

	/* unique join */
	switch (nodeTag(plan))
	{
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			/* try not to be too chatty about this in text mode */
			if (es->verbose && ((Join *) plan)->inner_unique)
				ExplainPropertyBool("Inner Unique",
									((Join *) plan)->inner_unique,
									es);
			break;
		default:
			break;
	}

	/* quals, sort keys, etc */
	switch (nodeTag(plan))
	{
		case T_IndexScan:
			show_scan_qual(((IndexScan *) plan)->indexqualorig,
						   "Index Cond", planstate, ancestors, es);
			if (((IndexScan *) plan)->indexqualorig)
				show_instrumentation_count("Rows Removed by Index Recheck", 2,
										   planstate, es);
			show_scan_qual(((IndexScan *) plan)->indexorderbyorig,
						   "Order By", planstate, ancestors, es);
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_IndexOnlyScan:
			show_scan_qual(((IndexOnlyScan *) plan)->indexqual,
						   "Index Cond", planstate, ancestors, es);
			if (((IndexOnlyScan *) plan)->recheckqual)
				show_instrumentation_count("Rows Removed by Index Recheck", 2,
										   planstate, es);
			show_scan_qual(((IndexOnlyScan *) plan)->indexorderby,
						   "Order By", planstate, ancestors, es);
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			if (es->analyze)
				ExplainPropertyFloat("Heap Fetches", NULL,
									 planstate->instrument->ntuples2, 0, es);
			break;
		case T_BitmapIndexScan:
			show_scan_qual(((BitmapIndexScan *) plan)->indexqualorig,
						   "Index Cond", planstate, ancestors, es);
			break;
		case T_BitmapHeapScan:
			show_scan_qual(((BitmapHeapScan *) plan)->bitmapqualorig,
						   "Recheck Cond", planstate, ancestors, es);
			if (((BitmapHeapScan *) plan)->bitmapqualorig)
				show_instrumentation_count("Rows Removed by Index Recheck", 2,
										   planstate, es);
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			if (es->analyze)
				show_tidbitmap_info((BitmapHeapScanState *) planstate, es);
			break;
		case T_SampleScan:
			show_tablesample(((SampleScan *) plan)->tablesample,
							 planstate, ancestors, es);
			/* fall through to print additional fields the same as SeqScan */
			/* FALLTHROUGH */
		case T_SeqScan:
		case T_ValuesScan:
		case T_NamedTuplestoreScan:
		case T_SubqueryScan:
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_Gather:
			{
				Gather	   *gather = (Gather *) plan;

				show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
				if (plan->qual)
					show_instrumentation_count("Rows Removed by Filter", 1,
											   planstate, es);
				ExplainPropertyInteger("Workers Planned", NULL,
									   gather->num_workers, es);

				/* Show params evaluated at gather node */
				if (gather->initParam)
					show_eval_params(gather->initParam, es);

				if (es->analyze)
				{
					int			nworkers;

					nworkers = ((GatherState *) planstate)->nworkers_launched;
					ExplainPropertyInteger("Workers Launched", NULL,
										   nworkers, es);
				}

				if (gather->single_copy)
					ExplainPropertyBool("Single Copy", gather->single_copy, es);
			}
			break;
		case T_GatherMerge:
			{
				GatherMerge *gm = (GatherMerge *) plan;

				show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
				if (plan->qual)
					show_instrumentation_count("Rows Removed by Filter", 1,
											   planstate, es);
				ExplainPropertyInteger("Workers Planned", NULL,
									   gm->num_workers, es);

				/* Show params evaluated at gather-merge node */
				if (gm->initParam)
					show_eval_params(gm->initParam, es);

				if (es->analyze)
				{
					int			nworkers;

					nworkers = ((GatherMergeState *) planstate)->nworkers_launched;
					ExplainPropertyInteger("Workers Launched", NULL,
										   nworkers, es);
				}
			}
			break;
		case T_FunctionScan:
			if (es->verbose)
			{
				List	   *fexprs = NIL;
				ListCell   *lc;

				foreach(lc, ((FunctionScan *) plan)->functions)
				{
					RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);

					fexprs = lappend(fexprs, rtfunc->funcexpr);
				}
				/* We rely on show_expression to insert commas as needed */
				show_expression((Node *) fexprs,
								"Function Call", planstate, ancestors,
								es->verbose, es);
			}
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_TidScan:
			{
				/*
				 * The tidquals list has OR semantics, so be sure to show it
				 * as an OR condition.
				 */
				List	   *tidquals = ((TidScan *) plan)->tidquals;

				if (list_length(tidquals) > 1)
					tidquals = list_make1(make_orclause(tidquals));
				show_scan_qual(tidquals, "TID Cond", planstate, ancestors, es);
				show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
				if (plan->qual)
					show_instrumentation_count("Rows Removed by Filter", 1,
											   planstate, es);
			}
			break;
		case T_TidRangeScan:
			{
				/*
				 * The tidrangequals list has AND semantics, so be sure to
				 * show it as an AND condition.
				 */
				List	   *tidquals = ((TidRangeScan *) plan)->tidrangequals;

				if (list_length(tidquals) > 1)
					tidquals = list_make1(make_andclause(tidquals));
				show_scan_qual(tidquals, "TID Cond", planstate, ancestors, es);
				show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
				if (plan->qual)
					show_instrumentation_count("Rows Removed by Filter", 1,
											   planstate, es);
			}
			break;
		case T_CustomScan:
			{
				CustomScanState *css = (CustomScanState *) planstate;

				show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
				if (plan->qual)
					show_instrumentation_count("Rows Removed by Filter", 1,
											   planstate, es);
				if (css->methods->ExplainCustomScan)
					css->methods->ExplainCustomScan(css, ancestors, es);
			}
			break;
		case T_NestLoop:
			show_upper_qual(((NestLoop *) plan)->join.joinqual,
							"Join Filter", planstate, ancestors, es);
			if (((NestLoop *) plan)->join.joinqual)
				show_instrumentation_count("Rows Removed by Join Filter", 1,
										   planstate, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 2,
										   planstate, es);
			break;
		case T_MergeJoin:
			show_upper_qual(((MergeJoin *) plan)->mergeclauses,
							"Merge Cond", planstate, ancestors, es);
			show_upper_qual(((MergeJoin *) plan)->join.joinqual,
							"Join Filter", planstate, ancestors, es);
			if (((MergeJoin *) plan)->join.joinqual)
				show_instrumentation_count("Rows Removed by Join Filter", 1,
										   planstate, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 2,
										   planstate, es);
			break;
		case T_HashJoin:
			show_upper_qual(((HashJoin *) plan)->hashclauses,
							"Hash Cond", planstate, ancestors, es);
			show_upper_qual(((HashJoin *) plan)->join.joinqual,
							"Join Filter", planstate, ancestors, es);
			if (((HashJoin *) plan)->join.joinqual)
				show_instrumentation_count("Rows Removed by Join Filter", 1,
										   planstate, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 2,
										   planstate, es);
			break;
		case T_Agg:
			show_agg_keys(castNode(AggState, planstate), ancestors, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			show_hashagg_info((AggState *) planstate, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_Group:
			show_group_keys(castNode(GroupState, planstate), ancestors, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_Sort:
			show_sort_keys(castNode(SortState, planstate), ancestors, es);
			show_sort_info(castNode(SortState, planstate), es);
			break;
		case T_IncrementalSort:
			show_incremental_sort_keys(castNode(IncrementalSortState, planstate),
									   ancestors, es);
			show_incremental_sort_info(castNode(IncrementalSortState, planstate),
									   es);
			break;
		case T_MergeAppend:
			show_merge_append_keys(castNode(MergeAppendState, planstate),
								   ancestors, es);
			break;
		case T_Result:
			show_upper_qual((List *) ((Result *) plan)->resconstantqual,
							"One-Time Filter", planstate, ancestors, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_ModifyTable:
			show_modifytable_info(castNode(ModifyTableState, planstate), ancestors,
								  es);
			break;
		case T_Hash:
			show_hash_info(castNode(HashState, planstate), es);
			break;
		case T_Memoize:
			show_memoize_info(castNode(MemoizeState, planstate), ancestors,
							  es);
			break;
		default:
			break;
	}

	/* Show buffer/WAL usage */
	if (es->buffers && planstate->instrument)
		show_buffer_usage(es, &planstate->instrument->bufusage, false);
	if (es->wal && planstate->instrument)
		show_wal_usage(es, &planstate->instrument->walusage);

	/* Prepare per-worker buffer/WAL usage */
	if (es->workers_state && (es->buffers || es->wal) && es->verbose)
	{
		WorkerInstrumentation *w = planstate->worker_instrument;

		for (int n = 0; n < w->num_workers; n++)
		{
			Instrumentation *instrument = &w->instrument[n];
			double		nloops = instrument->nloops;

			if (nloops <= 0)
				continue;

			ExplainOpenWorker(n, es);
			if (es->buffers)
				show_buffer_usage(es, &instrument->bufusage, false);
			if (es->wal)
				show_wal_usage(es, &instrument->walusage);
			ExplainCloseWorker(n, es);
		}
	}

	/* Show per-worker details for this plan node, then pop that stack */
	if (es->workers_state)
		ExplainFlushWorkersState(es);
	es->workers_state = save_workers_state;

	/*
	 * If partition pruning was done during executor initialization, the
	 * number of child plans we'll display below will be less than the number
	 * of subplans that was specified in the plan.  To make this a bit less
	 * mysterious, emit an indication that this happened.  Note that this
	 * field is emitted now because we want it to be a property of the parent
	 * node; it *cannot* be emitted within the Plans sub-node we'll open next.
	 */
	switch (nodeTag(plan))
	{
		case T_Append:
			ExplainMissingMembers(((AppendState *) planstate)->as_nplans,
								  list_length(((Append *) plan)->appendplans),
								  es);
			break;
		case T_MergeAppend:
			ExplainMissingMembers(((MergeAppendState *) planstate)->ms_nplans,
								  list_length(((MergeAppend *) plan)->mergeplans),
								  es);
			break;
		default:
			break;
	}

	/* Get ready to display the child plans */
	haschildren = planstate->initPlan ||
		outerPlanState(planstate) ||
		innerPlanState(planstate) ||
		IsA(plan, Append) ||
		IsA(plan, MergeAppend) ||
		IsA(plan, BitmapAnd) ||
		IsA(plan, BitmapOr) ||
		IsA(plan, SubqueryScan) ||
		(IsA(planstate, CustomScanState) &&
		 ((CustomScanState *) planstate)->custom_ps != NIL) ||
		planstate->subPlan;
	if (haschildren)
	{
		/* Pass current Plan as head of ancestors list for children */
		ancestors = lcons(plan, ancestors);
	}

	/* initPlan-s */
	if (planstate->initPlan)
		ExplainSubPlans(planstate->initPlan, ancestors, "InitPlan", es);

	/* lefttree */
	if (outerPlanState(planstate))
		ExplainNode(outerPlanState(planstate), ancestors,
					"Outer", NULL, es);

	/* righttree */
	if (innerPlanState(planstate))
		ExplainNode(innerPlanState(planstate), ancestors,
					"Inner", NULL, es);

	/* special child plans */
	switch (nodeTag(plan))
	{
		case T_Append:
			ExplainMemberNodes(((AppendState *) planstate)->appendplans,
							   ((AppendState *) planstate)->as_nplans,
							   ancestors, es);
			break;
		case T_MergeAppend:
			ExplainMemberNodes(((MergeAppendState *) planstate)->mergeplans,
							   ((MergeAppendState *) planstate)->ms_nplans,
							   ancestors, es);
			break;
		case T_BitmapAnd:
			ExplainMemberNodes(((BitmapAndState *) planstate)->bitmapplans,
							   ((BitmapAndState *) planstate)->nplans,
							   ancestors, es);
			break;
		case T_BitmapOr:
			ExplainMemberNodes(((BitmapOrState *) planstate)->bitmapplans,
							   ((BitmapOrState *) planstate)->nplans,
							   ancestors, es);
			break;
		case T_SubqueryScan:
			ExplainNode(((SubqueryScanState *) planstate)->subplan, ancestors,
						"Subquery", NULL, es);
			break;
		case T_CustomScan:
			ExplainCustomChildren((CustomScanState *) planstate,
								  ancestors, es);
			break;
		default:
			break;
	}

	/* subPlan-s */
	if (planstate->subPlan)
		ExplainSubPlans(planstate->subPlan, ancestors, "SubPlan", es);

	/* end of child plans */
	if (haschildren)
	{
		ancestors = list_delete_first(ancestors);
	}

	/* undo whatever indentation we added */
	es->indent = save_indent;
}

/*
 * Show the targetlist of a plan node
 */
static void
show_plan_tlist(PlanState *planstate, List *ancestors, ExplainState *es)
{
	Plan	   *plan = planstate->plan;
	List	   *context;
	List	   *result = NIL;
	bool		useprefix;
	ListCell   *lc;

	/* No work if empty tlist (this occurs eg in bitmap indexscans) */
	if (plan->targetlist == NIL)
		return;
	/* The tlist of an Append isn't real helpful, so suppress it */
	if (IsA(plan, Append))
		return;
	/* Likewise for MergeAppend */
	if (IsA(plan, MergeAppend))
		return;

	/* Set up deparsing context */
	context = set_deparse_context_plan(es->deparse_cxt,
									   plan,
									   ancestors);
	useprefix = list_length(es->rtable) > 1;

	/* Deparse each result column (we now include resjunk ones) */
	foreach(lc, plan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		result = lappend(result,
						 deparse_expression((Node *) tle->expr, context,
											useprefix, false));
	}

	/* Print results */
	ExplainPropertyList("Output", result, es);
}

/*
 * Show a generic expression
 */
static void
show_expression(Node *node, const char *qlabel,
				PlanState *planstate, List *ancestors,
				bool useprefix, ExplainState *es)
{
	List	   *context;
	char	   *exprstr;

	/* Set up deparsing context */
	context = set_deparse_context_plan(es->deparse_cxt,
									   planstate->plan,
									   ancestors);

	/* Deparse the expression */
	exprstr = deparse_expression(node, context, useprefix, false);

	/* And add to es->str */
	ExplainPropertyText(qlabel, exprstr, es);
}

/*
 * Show a qualifier expression (which is a List with implicit AND semantics)
 */
static void
show_qual(List *qual, const char *qlabel,
		  PlanState *planstate, List *ancestors,
		  bool useprefix, ExplainState *es)
{
	Node	   *node;

	/* No work if empty qual */
	if (qual == NIL)
		return;

	/* Convert AND list to explicit AND */
	node = (Node *) make_ands_explicit(qual);

	/* And show it */
	show_expression(node, qlabel, planstate, ancestors, useprefix, es);
}

/*
 * Show a qualifier expression for a scan plan node
 */
static void
show_scan_qual(List *qual, const char *qlabel,
			   PlanState *planstate, List *ancestors,
			   ExplainState *es)
{
	bool		useprefix;

	useprefix = (IsA(planstate->plan, SubqueryScan) || es->verbose);
	show_qual(qual, qlabel, planstate, ancestors, useprefix, es);
}

/*
 * Show a qualifier expression for an upper-level plan node
 */
static void
show_upper_qual(List *qual, const char *qlabel,
				PlanState *planstate, List *ancestors,
				ExplainState *es)
{
	bool		useprefix;

	useprefix = (list_length(es->rtable) > 1 || es->verbose);
	show_qual(qual, qlabel, planstate, ancestors, useprefix, es);
}

/*
 * Show the sort keys for a Sort node.
 */
static void
show_sort_keys(SortState *sortstate, List *ancestors, ExplainState *es)
{
	Sort	   *plan = (Sort *) sortstate->ss.ps.plan;

	show_sort_group_keys((PlanState *) sortstate, "Sort Key",
						 plan->numCols, 0, plan->sortColIdx,
						 plan->sortOperators, plan->collations,
						 plan->nullsFirst,
						 ancestors, es);
}

/*
 * Show the sort keys for a IncrementalSort node.
 */
static void
show_incremental_sort_keys(IncrementalSortState *incrsortstate,
						   List *ancestors, ExplainState *es)
{
	IncrementalSort *plan = (IncrementalSort *) incrsortstate->ss.ps.plan;

	show_sort_group_keys((PlanState *) incrsortstate, "Sort Key",
						 plan->sort.numCols, plan->nPresortedCols,
						 plan->sort.sortColIdx,
						 plan->sort.sortOperators, plan->sort.collations,
						 plan->sort.nullsFirst,
						 ancestors, es);
}

/*
 * Likewise, for a MergeAppend node.
 */
static void
show_merge_append_keys(MergeAppendState *mstate, List *ancestors,
					   ExplainState *es)
{
	MergeAppend *plan = (MergeAppend *) mstate->ps.plan;

	show_sort_group_keys((PlanState *) mstate, "Sort Key",
						 plan->numCols, 0, plan->sortColIdx,
						 plan->sortOperators, plan->collations,
						 plan->nullsFirst,
						 ancestors, es);
}

/*
 * Show the grouping keys for an Agg node.
 */
static void
show_agg_keys(AggState *astate, List *ancestors,
			  ExplainState *es)
{
	Agg		   *plan = (Agg *) astate->ss.ps.plan;

	if (plan->numCols > 0 || plan->groupingSets)
	{
		/* The key columns refer to the tlist of the child plan */
		ancestors = lcons(plan, ancestors);

		if (plan->groupingSets)
			show_grouping_sets(outerPlanState(astate), plan, ancestors, es);
		else
			show_sort_group_keys(outerPlanState(astate), "Group Key",
								 plan->numCols, 0, plan->grpColIdx,
								 NULL, NULL, NULL,
								 ancestors, es);

		ancestors = list_delete_first(ancestors);
	}
}

static void
show_grouping_sets(PlanState *planstate, Agg *agg,
				   List *ancestors, ExplainState *es)
{
	List	   *context;
	bool		useprefix;
	ListCell   *lc;

	/* Set up deparsing context */
	context = set_deparse_context_plan(es->deparse_cxt,
									   planstate->plan,
									   ancestors);
	useprefix = (list_length(es->rtable) > 1 || es->verbose);

	show_grouping_set_keys(planstate, agg, NULL,
						   context, useprefix, ancestors, es);

	foreach(lc, agg->chain)
	{
		Agg		   *aggnode = lfirst(lc);
		Sort	   *sortnode = (Sort *) aggnode->plan.lefttree;

		show_grouping_set_keys(planstate, aggnode, sortnode,
							   context, useprefix, ancestors, es);
	}
}

static void
show_grouping_set_keys(PlanState *planstate,
					   Agg *aggnode, Sort *sortnode,
					   List *context, bool useprefix,
					   List *ancestors, ExplainState *es)
{
	Plan	   *plan = planstate->plan;
	char	   *exprstr;
	ListCell   *lc;
	List	   *gsets = aggnode->groupingSets;
	AttrNumber *keycols = aggnode->grpColIdx;
	const char *keyname;

	if (aggnode->aggstrategy == AGG_HASHED || aggnode->aggstrategy == AGG_MIXED)
		keyname = "Hash Key";
	else
		keyname = "Group Key";

	if (sortnode)
	{
		show_sort_group_keys(planstate, "Sort Key",
							 sortnode->numCols, 0, sortnode->sortColIdx,
							 sortnode->sortOperators, sortnode->collations,
							 sortnode->nullsFirst,
							 ancestors, es);
		es->indent++;
	}

	foreach(lc, gsets)
	{
		List	   *result = NIL;
		ListCell   *lc2;

		foreach(lc2, (List *) lfirst(lc))
		{
			Index		i = lfirst_int(lc2);
			AttrNumber	keyresno = keycols[i];
			TargetEntry *target = get_tle_by_resno(plan->targetlist,
												   keyresno);

			if (!target)
				elog(ERROR, "no tlist entry for key %d", keyresno);
			/* Deparse the expression, showing any top-level cast */
			exprstr = deparse_expression((Node *) target->expr, context,
										 useprefix, true);

			result = lappend(result, exprstr);
		}

		if (!result)
			ExplainPropertyText(keyname, "()", es);
		else
			ExplainPropertyList(keyname, result, es);
	}

	if (sortnode)
		es->indent--;
}

/*
 * Show the grouping keys for a Group node.
 */
static void
show_group_keys(GroupState *gstate, List *ancestors,
				ExplainState *es)
{
	Group	   *plan = (Group *) gstate->ss.ps.plan;

	/* The key columns refer to the tlist of the child plan */
	ancestors = lcons(plan, ancestors);
	show_sort_group_keys(outerPlanState(gstate), "Group Key",
						 plan->numCols, 0, plan->grpColIdx,
						 NULL, NULL, NULL,
						 ancestors, es);
	ancestors = list_delete_first(ancestors);
}

/*
 * Common code to show sort/group keys, which are represented in plan nodes
 * as arrays of targetlist indexes.  If it's a sort key rather than a group
 * key, also pass sort operators/collations/nullsFirst arrays.
 */
static void
show_sort_group_keys(PlanState *planstate, const char *qlabel,
					 int nkeys, int nPresortedKeys, AttrNumber *keycols,
					 Oid *sortOperators, Oid *collations, bool *nullsFirst,
					 List *ancestors, ExplainState *es)
{
	Plan	   *plan = planstate->plan;
	List	   *context;
	List	   *result = NIL;
	List	   *resultPresorted = NIL;
	StringInfoData sortkeybuf;
	bool		useprefix;
	int			keyno;

	if (nkeys <= 0)
		return;

	initStringInfo(&sortkeybuf);

	/* Set up deparsing context */
	context = set_deparse_context_plan(es->deparse_cxt,
									   plan,
									   ancestors);
	useprefix = (list_length(es->rtable) > 1 || es->verbose);

	for (keyno = 0; keyno < nkeys; keyno++)
	{
		/* find key expression in tlist */
		AttrNumber	keyresno = keycols[keyno];
		TargetEntry *target = get_tle_by_resno(plan->targetlist,
											   keyresno);
		char	   *exprstr;

		if (!target)
			elog(ERROR, "no tlist entry for key %d", keyresno);
		/* Deparse the expression, showing any top-level cast */
		exprstr = deparse_expression((Node *) target->expr, context,
									 useprefix, true);
		resetStringInfo(&sortkeybuf);
		appendStringInfoString(&sortkeybuf, exprstr);
		/* Append sort order information, if relevant */
		if (sortOperators != NULL)
			show_sortorder_options(&sortkeybuf,
								   (Node *) target->expr,
								   sortOperators[keyno],
								   collations[keyno],
								   nullsFirst[keyno]);
		/* Emit one property-list item per sort key */
		result = lappend(result, pstrdup(sortkeybuf.data));
		if (keyno < nPresortedKeys)
			resultPresorted = lappend(resultPresorted, exprstr);
	}

	ExplainPropertyList(qlabel, result, es);
	if (nPresortedKeys > 0)
		ExplainPropertyList("Presorted Key", resultPresorted, es);
}

/*
 * Append nondefault characteristics of the sort ordering of a column to buf
 * (collation, direction, NULLS FIRST/LAST)
 */
static void
show_sortorder_options(StringInfo buf, Node *sortexpr,
					   Oid sortOperator, Oid collation, bool nullsFirst)
{
	Oid			sortcoltype = exprType(sortexpr);
	bool		reverse = false;
	TypeCacheEntry *typentry;

	typentry = lookup_type_cache(sortcoltype,
								 TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);

	/*
	 * Print COLLATE if it's not default for the column's type.  There are
	 * some cases where this is redundant, eg if expression is a column whose
	 * declared collation is that collation, but it's hard to distinguish that
	 * here (and arguably, printing COLLATE explicitly is a good idea anyway
	 * in such cases).
	 */
	if (OidIsValid(collation) && collation != get_typcollation(sortcoltype))
	{
		char	   *collname = get_collation_name(collation);

		if (collname == NULL)
			elog(ERROR, "cache lookup failed for collation %u", collation);
		appendStringInfo(buf, " COLLATE %s", quote_identifier(collname));
	}

	/* Print direction if not ASC, or USING if non-default sort operator */
	if (sortOperator == typentry->gt_opr)
	{
		appendStringInfoString(buf, " DESC");
		reverse = true;
	}
	else if (sortOperator != typentry->lt_opr)
	{
		char	   *opname = get_opname(sortOperator);

		if (opname == NULL)
			elog(ERROR, "cache lookup failed for operator %u", sortOperator);
		appendStringInfo(buf, " USING %s", opname);
		/* Determine whether operator would be considered ASC or DESC */
		(void) get_equality_op_for_ordering_op(sortOperator, &reverse);
	}

	/* Add NULLS FIRST/LAST only if it wouldn't be default */
	if (nullsFirst && !reverse)
	{
		appendStringInfoString(buf, " NULLS FIRST");
	}
	else if (!nullsFirst && reverse)
	{
		appendStringInfoString(buf, " NULLS LAST");
	}
}

/*
 * Show TABLESAMPLE properties
 */
static void
show_tablesample(TableSampleClause *tsc, PlanState *planstate,
				 List *ancestors, ExplainState *es)
{
	List	   *context;
	bool		useprefix;
	char	   *method_name;
	List	   *params = NIL;
	char	   *repeatable;
	ListCell   *lc;

	/* Set up deparsing context */
	context = set_deparse_context_plan(es->deparse_cxt,
									   planstate->plan,
									   ancestors);
	useprefix = list_length(es->rtable) > 1;

	/* Get the tablesample method name */
	method_name = get_func_name(tsc->tsmhandler);

	/* Deparse parameter expressions */
	foreach(lc, tsc->args)
	{
		Node	   *arg = (Node *) lfirst(lc);

		params = lappend(params,
						 deparse_expression(arg, context,
											useprefix, false));
	}
	if (tsc->repeatable)
		repeatable = deparse_expression((Node *) tsc->repeatable, context,
										useprefix, false);
	else
		repeatable = NULL;

	/* Print results */
	{
		bool		first = true;

		ExplainIndentText(es);
		appendStringInfo(es->str, "Sampling: %s (", method_name);
		foreach(lc, params)
		{
			if (!first)
				appendStringInfoString(es->str, ", ");
			appendStringInfoString(es->str, (const char *) lfirst(lc));
			first = false;
		}
		appendStringInfoChar(es->str, ')');
		if (repeatable)
			appendStringInfo(es->str, " REPEATABLE (%s)", repeatable);
		appendStringInfoChar(es->str, '\n');
	}
}

/*
 * If it's EXPLAIN ANALYZE, show tuplesort stats for a sort node
 */
static void
show_sort_info(SortState *sortstate, ExplainState *es)
{
	if (!es->analyze)
		return;

	if (sortstate->sort_Done && sortstate->tuplesortstate != NULL)
	{
		Tuplesortstate *state = (Tuplesortstate *) sortstate->tuplesortstate;
		TuplesortInstrumentation stats;
		const char *sortMethod;
		const char *spaceType;
		int64		spaceUsed;

		tuplesort_get_stats(state, &stats);
		sortMethod = tuplesort_method_name(stats.sortMethod);
		spaceType = tuplesort_space_type_name(stats.spaceType);
		spaceUsed = stats.spaceUsed;

		ExplainIndentText(es);
		appendStringInfo(es->str, "Sort Method: %s  %s: " INT64_FORMAT "kB\n",
						 sortMethod, spaceType, spaceUsed);
	}

	/*
	 * You might think we should just skip this stanza entirely when
	 * es->hide_workers is true, but then we'd get no sort-method output at
	 * all.  We have to make it look like worker 0's data is top-level data.
	 * This is easily done by just skipping the OpenWorker/CloseWorker calls.
	 * Currently, we don't worry about the possibility that there are multiple
	 * workers in such a case; if there are, duplicate output fields will be
	 * emitted.
	 */
	if (sortstate->shared_info != NULL)
	{
		int			n;

		for (n = 0; n < sortstate->shared_info->num_workers; n++)
		{
			TuplesortInstrumentation *sinstrument;
			const char *sortMethod;
			const char *spaceType;
			int64		spaceUsed;

			sinstrument = &sortstate->shared_info->sinstrument[n];
			if (sinstrument->sortMethod == SORT_TYPE_STILL_IN_PROGRESS)
				continue;		/* ignore any unfilled slots */
			sortMethod = tuplesort_method_name(sinstrument->sortMethod);
			spaceType = tuplesort_space_type_name(sinstrument->spaceType);
			spaceUsed = sinstrument->spaceUsed;

			if (es->workers_state)
				ExplainOpenWorker(n, es);

			ExplainIndentText(es);
			appendStringInfo(es->str,
							 "Sort Method: %s  %s: " INT64_FORMAT "kB\n",
							 sortMethod, spaceType, spaceUsed);

			if (es->workers_state)
				ExplainCloseWorker(n, es);
		}
	}
}

/*
 * Incremental sort nodes sort in (a potentially very large number of) batches,
 * so EXPLAIN ANALYZE needs to roll up the tuplesort stats from each batch into
 * an intelligible summary.
 *
 * This function is used for both a non-parallel node and each worker in a
 * parallel incremental sort node.
 */
static void
show_incremental_sort_group_info(IncrementalSortGroupInfo *groupInfo,
								 const char *groupLabel, bool indent, ExplainState *es)
{
	ListCell   *methodCell;
	List	   *methodNames = NIL;

	/* Generate a list of sort methods used across all groups. */
	for (int bit = 0; bit < NUM_TUPLESORTMETHODS; bit++)
	{
		TuplesortMethod sortMethod = (1 << bit);

		if (groupInfo->sortMethods & sortMethod)
		{
			const char *methodName = tuplesort_method_name(sortMethod);

			methodNames = lappend(methodNames, unconstify(char *, methodName));
		}
	}

	if (indent)
		appendStringInfoSpaces(es->str, es->indent * 2);
	appendStringInfo(es->str, "%s Groups: " INT64_FORMAT "  Sort Method", groupLabel,
					 groupInfo->groupCount);
	/* plural/singular based on methodNames size */
	if (list_length(methodNames) > 1)
		appendStringInfoString(es->str, "s: ");
	else
		appendStringInfoString(es->str, ": ");
	foreach(methodCell, methodNames)
	{
		appendStringInfoString(es->str, (char *) methodCell->ptr_value);
		if (foreach_current_index(methodCell) < list_length(methodNames) - 1)
			appendStringInfoString(es->str, ", ");
	}

	if (groupInfo->maxMemorySpaceUsed > 0)
	{
		int64		avgSpace = groupInfo->totalMemorySpaceUsed / groupInfo->groupCount;
		const char *spaceTypeName;

		spaceTypeName = tuplesort_space_type_name(SORT_SPACE_TYPE_MEMORY);
		appendStringInfo(es->str, "  Average %s: " INT64_FORMAT "kB  Peak %s: " INT64_FORMAT "kB",
						 spaceTypeName, avgSpace,
						 spaceTypeName, groupInfo->maxMemorySpaceUsed);
	}

	if (groupInfo->maxDiskSpaceUsed > 0)
	{
		int64		avgSpace = groupInfo->totalDiskSpaceUsed / groupInfo->groupCount;
		const char *spaceTypeName;

		spaceTypeName = tuplesort_space_type_name(SORT_SPACE_TYPE_DISK);
		appendStringInfo(es->str, "  Average %s: " INT64_FORMAT "kB  Peak %s: " INT64_FORMAT "kB",
						 spaceTypeName, avgSpace,
						 spaceTypeName, groupInfo->maxDiskSpaceUsed);
	}
}

/*
 * If it's EXPLAIN ANALYZE, show tuplesort stats for an incremental sort node
 */
static void
show_incremental_sort_info(IncrementalSortState *incrsortstate,
						   ExplainState *es)
{
	IncrementalSortGroupInfo *fullsortGroupInfo;
	IncrementalSortGroupInfo *prefixsortGroupInfo;

	fullsortGroupInfo = &incrsortstate->incsort_info.fullsortGroupInfo;

	if (!es->analyze)
		return;

	/*
	 * Since we never have any prefix groups unless we've first sorted a full
	 * groups and transitioned modes (copying the tuples into a prefix group),
	 * we don't need to do anything if there were 0 full groups.
	 *
	 * We still have to continue after this block if there are no full groups,
	 * though, since it's possible that we have workers that did real work
	 * even if the leader didn't participate.
	 */
	if (fullsortGroupInfo->groupCount > 0)
	{
		show_incremental_sort_group_info(fullsortGroupInfo, "Full-sort", true, es);
		prefixsortGroupInfo = &incrsortstate->incsort_info.prefixsortGroupInfo;
		if (prefixsortGroupInfo->groupCount > 0)
		{
			appendStringInfoChar(es->str, '\n');
			show_incremental_sort_group_info(prefixsortGroupInfo, "Pre-sorted", true, es);
		}
		appendStringInfoChar(es->str, '\n');
	}

	if (incrsortstate->shared_info != NULL)
	{
		int			n;
		bool		indent_first_line;

		for (n = 0; n < incrsortstate->shared_info->num_workers; n++)
		{
			IncrementalSortInfo *incsort_info =
			&incrsortstate->shared_info->sinfo[n];

			/*
			 * If a worker hasn't processed any sort groups at all, then
			 * exclude it from output since it either didn't launch or didn't
			 * contribute anything meaningful.
			 */
			fullsortGroupInfo = &incsort_info->fullsortGroupInfo;

			/*
			 * Since we never have any prefix groups unless we've first sorted
			 * a full groups and transitioned modes (copying the tuples into a
			 * prefix group), we don't need to do anything if there were 0
			 * full groups.
			 */
			if (fullsortGroupInfo->groupCount == 0)
				continue;

			if (es->workers_state)
				ExplainOpenWorker(n, es);

			indent_first_line = es->workers_state == NULL || es->verbose;
			show_incremental_sort_group_info(fullsortGroupInfo, "Full-sort",
											 indent_first_line, es);
			prefixsortGroupInfo = &incsort_info->prefixsortGroupInfo;
			if (prefixsortGroupInfo->groupCount > 0)
			{
				appendStringInfoChar(es->str, '\n');
				show_incremental_sort_group_info(prefixsortGroupInfo, "Pre-sorted", true, es);
			}
			appendStringInfoChar(es->str, '\n');

			if (es->workers_state)
				ExplainCloseWorker(n, es);
		}
	}
}

/*
 * Show information on hash buckets/batches.
 */
static void
show_hash_info(HashState *hashstate, ExplainState *es)
{
	HashInstrumentation hinstrument = {0};

	/*
	 * Collect stats from the local process, even when it's a parallel query.
	 * In a parallel query, the leader process may or may not have run the
	 * hash join, and even if it did it may not have built a hash table due to
	 * timing (if it started late it might have seen no tuples in the outer
	 * relation and skipped building the hash table).  Therefore we have to be
	 * prepared to get instrumentation data from all participants.
	 */
	if (hashstate->hinstrument)
		memcpy(&hinstrument, hashstate->hinstrument,
			   sizeof(HashInstrumentation));

	/*
	 * Merge results from workers.  In the parallel-oblivious case, the
	 * results from all participants should be identical, except where
	 * participants didn't run the join at all so have no data.  In the
	 * parallel-aware case, we need to consider all the results.  Each worker
	 * may have seen a different subset of batches and we want to report the
	 * highest memory usage across all batches.  We take the maxima of other
	 * values too, for the same reasons as in ExecHashAccumInstrumentation.
	 */
	if (hashstate->shared_info)
	{
		SharedHashInfo *shared_info = hashstate->shared_info;
		int			i;

		for (i = 0; i < shared_info->num_workers; ++i)
		{
			HashInstrumentation *worker_hi = &shared_info->hinstrument[i];

			hinstrument.nbuckets = Max(hinstrument.nbuckets,
									   worker_hi->nbuckets);
			hinstrument.nbuckets_original = Max(hinstrument.nbuckets_original,
												worker_hi->nbuckets_original);
			hinstrument.nbatch = Max(hinstrument.nbatch,
									 worker_hi->nbatch);
			hinstrument.nbatch_original = Max(hinstrument.nbatch_original,
											  worker_hi->nbatch_original);
			hinstrument.space_peak = Max(hinstrument.space_peak,
										 worker_hi->space_peak);
		}
	}

	if (hinstrument.nbatch > 0)
	{
		long		spacePeakKb = (hinstrument.space_peak + 1023) / 1024;

		if (hinstrument.nbatch_original != hinstrument.nbatch ||
			hinstrument.nbuckets_original != hinstrument.nbuckets)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str,
							 "Buckets: %d (originally %d)  Batches: %d (originally %d)  Memory Usage: %ldkB\n",
							 hinstrument.nbuckets,
							 hinstrument.nbuckets_original,
							 hinstrument.nbatch,
							 hinstrument.nbatch_original,
							 spacePeakKb);
		}
		else
		{
			ExplainIndentText(es);
			appendStringInfo(es->str,
							 "Buckets: %d  Batches: %d  Memory Usage: %ldkB\n",
							 hinstrument.nbuckets, hinstrument.nbatch,
							 spacePeakKb);
		}
	}
}

/*
 * Show information on memoize hits/misses/evictions and memory usage.
 */
static void
show_memoize_info(MemoizeState *mstate, List *ancestors, ExplainState *es)
{
	Plan	   *plan = ((PlanState *) mstate)->plan;
	ListCell   *lc;
	List	   *context;
	StringInfoData keystr;
	char	   *seperator = "";
	bool		useprefix;
	int64		memPeakKb;

	initStringInfo(&keystr);

	/*
	 * It's hard to imagine having a memoize node with fewer than 2 RTEs, but
	 * let's just keep the same useprefix logic as elsewhere in this file.
	 */
	useprefix = list_length(es->rtable) > 1 || es->verbose;

	/* Set up deparsing context */
	context = set_deparse_context_plan(es->deparse_cxt,
									   plan,
									   ancestors);

	foreach(lc, ((Memoize *) plan)->param_exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);

		appendStringInfoString(&keystr, seperator);

		appendStringInfoString(&keystr, deparse_expression(expr, context,
														   useprefix, false));
		seperator = ", ";
	}

	ExplainIndentText(es);
	appendStringInfo(es->str, "Cache Key: %s\n", keystr.data);
	ExplainIndentText(es);
	appendStringInfo(es->str, "Cache Mode: %s\n", mstate->binary_mode ? "binary" : "logical");

	pfree(keystr.data);

	if (!es->analyze)
		return;

	if (mstate->stats.cache_misses > 0)
	{
		/*
		 * mem_peak is only set when we freed memory, so we must use mem_used
		 * when mem_peak is 0.
		 */
		if (mstate->stats.mem_peak > 0)
			memPeakKb = (mstate->stats.mem_peak + 1023) / 1024;
		else
			memPeakKb = (mstate->mem_used + 1023) / 1024;

		ExplainIndentText(es);
		appendStringInfo(es->str,
						 "Hits: " UINT64_FORMAT "  Misses: " UINT64_FORMAT "  Evictions: " UINT64_FORMAT "  Overflows: " UINT64_FORMAT "  Memory Usage: " INT64_FORMAT "kB\n",
						 mstate->stats.cache_hits,
						 mstate->stats.cache_misses,
						 mstate->stats.cache_evictions,
						 mstate->stats.cache_overflows,
						 memPeakKb);
	}

	if (mstate->shared_info == NULL)
		return;

	/* Show details from parallel workers */
	for (int n = 0; n < mstate->shared_info->num_workers; n++)
	{
		MemoizeInstrumentation *si;

		si = &mstate->shared_info->sinstrument[n];

		/*
		 * Skip workers that didn't do any work.  We needn't bother checking
		 * for cache hits as a miss will always occur before a cache hit.
		 */
		if (si->cache_misses == 0)
			continue;

		if (es->workers_state)
			ExplainOpenWorker(n, es);

		/*
		 * Since the worker's MemoizeState.mem_used field is unavailable to
		 * us, ExecEndMemoize will have set the
		 * MemoizeInstrumentation.mem_peak field for us.  No need to do the
		 * zero checks like we did for the serial case above.
		 */
		memPeakKb = (si->mem_peak + 1023) / 1024;

		ExplainIndentText(es);
		appendStringInfo(es->str,
						 "Hits: " UINT64_FORMAT "  Misses: " UINT64_FORMAT "  Evictions: " UINT64_FORMAT "  Overflows: " UINT64_FORMAT "  Memory Usage: " INT64_FORMAT "kB\n",
						 si->cache_hits, si->cache_misses,
						 si->cache_evictions, si->cache_overflows,
						 memPeakKb);

		if (es->workers_state)
			ExplainCloseWorker(n, es);
	}
}

/*
 * Show information on hash aggregate memory usage and batches.
 */
static void
show_hashagg_info(AggState *aggstate, ExplainState *es)
{
	Agg		   *agg = (Agg *) aggstate->ss.ps.plan;
	int64		memPeakKb = (aggstate->hash_mem_peak + 1023) / 1024;

	if (agg->aggstrategy != AGG_HASHED &&
		agg->aggstrategy != AGG_MIXED)
		return;

	{
		bool		gotone = false;

		if (es->costs && aggstate->hash_planned_partitions > 0)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "Planned Partitions: %d",
							 aggstate->hash_planned_partitions);
			gotone = true;
		}

		/*
		 * During parallel query the leader may have not helped out.  We
		 * detect this by checking how much memory it used.  If we find it
		 * didn't do any work then we don't show its properties.
		 */
		if (es->analyze && aggstate->hash_mem_peak > 0)
		{
			if (!gotone)
				ExplainIndentText(es);
			else
				appendStringInfoString(es->str, "  ");

			appendStringInfo(es->str, "Batches: %d  Memory Usage: " INT64_FORMAT "kB",
							 aggstate->hash_batches_used, memPeakKb);
			gotone = true;

			/* Only display disk usage if we spilled to disk */
			if (aggstate->hash_batches_used > 1)
			{
				appendStringInfo(es->str, "  Disk Usage: " UINT64_FORMAT "kB",
								 aggstate->hash_disk_used);
			}
		}

		if (gotone)
			appendStringInfoChar(es->str, '\n');
	}

	/* Display stats for each parallel worker */
	if (es->analyze && aggstate->shared_info != NULL)
	{
		for (int n = 0; n < aggstate->shared_info->num_workers; n++)
		{
			AggregateInstrumentation *sinstrument;
			uint64		hash_disk_used;
			int			hash_batches_used;

			sinstrument = &aggstate->shared_info->sinstrument[n];
			/* Skip workers that didn't do anything */
			if (sinstrument->hash_mem_peak == 0)
				continue;
			hash_disk_used = sinstrument->hash_disk_used;
			hash_batches_used = sinstrument->hash_batches_used;
			memPeakKb = (sinstrument->hash_mem_peak + 1023) / 1024;

			if (es->workers_state)
				ExplainOpenWorker(n, es);

			ExplainIndentText(es);

			appendStringInfo(es->str, "Batches: %d  Memory Usage: " INT64_FORMAT "kB",
							 hash_batches_used, memPeakKb);

			/* Only display disk usage if we spilled to disk */
			if (hash_batches_used > 1)
				appendStringInfo(es->str, "  Disk Usage: " UINT64_FORMAT "kB",
								 hash_disk_used);
			appendStringInfoChar(es->str, '\n');

			if (es->workers_state)
				ExplainCloseWorker(n, es);
		}
	}
}

/*
 * If it's EXPLAIN ANALYZE, show exact/lossy pages for a BitmapHeapScan node
 */
static void
show_tidbitmap_info(BitmapHeapScanState *planstate, ExplainState *es)
{
	if (planstate->exact_pages > 0 || planstate->lossy_pages > 0)
	{
		ExplainIndentText(es);
		appendStringInfoString(es->str, "Heap Blocks:");
		if (planstate->exact_pages > 0)
			appendStringInfo(es->str, " exact=%ld", planstate->exact_pages);
		if (planstate->lossy_pages > 0)
			appendStringInfo(es->str, " lossy=%ld", planstate->lossy_pages);
		appendStringInfoChar(es->str, '\n');
	}
}

/*
 * If it's EXPLAIN ANALYZE, show instrumentation information for a plan node
 *
 * "which" identifies which instrumentation counter to print
 */
static void
show_instrumentation_count(const char *qlabel, int which,
						   PlanState *planstate, ExplainState *es)
{
	double		nfiltered;
	double		nloops;

	if (!es->analyze || !planstate->instrument)
		return;

	if (which == 2)
		nfiltered = planstate->instrument->nfiltered2;
	else
		nfiltered = planstate->instrument->nfiltered1;
	nloops = planstate->instrument->nloops;

	/* suppress zero counts; they're not interesting enough */
	if (nfiltered > 0)
	{
		if (nloops > 0)
			ExplainPropertyFloat(qlabel, NULL, nfiltered / nloops, 0, es);
		else
			ExplainPropertyFloat(qlabel, NULL, 0.0, 0, es);
	}
}


/*
 * Show initplan params evaluated at Gather or Gather Merge node.
 */
static void
show_eval_params(Bitmapset *bms_params, ExplainState *es)
{
	int			paramid = -1;
	List	   *params = NIL;

	Assert(bms_params);

	while ((paramid = bms_next_member(bms_params, paramid)) >= 0)
	{
		char		param[32];

		snprintf(param, sizeof(param), "$%d", paramid);
		params = lappend(params, pstrdup(param));
	}

	if (params)
		ExplainPropertyList("Params Evaluated", params, es);
}

/*
 * Fetch the name of an index in an EXPLAIN
 *
 * We allow plugins to get control here so that plans involving hypothetical
 * indexes can be explained.
 *
 * Note: names returned by this function should be "raw"; the caller will
 * apply quoting if needed.  Formerly the convention was to do quoting here,
 * but we don't want that in non-text output formats.
 */
static const char *
explain_get_index_name(Oid indexId)
{
	const char *result;

	if (explain_get_index_name_hook)
		result = (*explain_get_index_name_hook) (indexId);
	else
		result = NULL;
	if (result == NULL)
	{
		/* default behavior: look it up in the catalogs */
		result = get_rel_name(indexId);
		if (result == NULL)
			elog(ERROR, "cache lookup failed for index %u", indexId);
	}
	return result;
}

/*
 * Show buffer usage details.
 */
static void
show_buffer_usage(ExplainState *es, const BufferUsage *usage, bool planning)
{
	bool		has_shared = (usage->shared_blks_hit > 0 ||
							  usage->shared_blks_read > 0 ||
							  usage->shared_blks_dirtied > 0 ||
							  usage->shared_blks_written > 0);
	bool		has_local = (usage->local_blks_hit > 0 ||
							 usage->local_blks_read > 0 ||
							 usage->local_blks_dirtied > 0 ||
							 usage->local_blks_written > 0);
	bool		has_temp = (usage->temp_blks_read > 0 ||
							usage->temp_blks_written > 0);
	bool		has_timing = (!INSTR_TIME_IS_ZERO(usage->blk_read_time) ||
							  !INSTR_TIME_IS_ZERO(usage->blk_write_time));
	bool		show_planning = (planning && (has_shared ||
											  has_local || has_temp || has_timing));

	if (show_planning)
	{
		ExplainIndentText(es);
		appendStringInfoString(es->str, "Planning:\n");
		es->indent++;
	}

	/* Show only positive counter values. */
	if (has_shared || has_local || has_temp)
	{
		ExplainIndentText(es);
		appendStringInfoString(es->str, "Buffers:");

		if (has_shared)
		{
			appendStringInfoString(es->str, " shared");
			if (usage->shared_blks_hit > 0)
				appendStringInfo(es->str, " hit=%lld",
								 (long long) usage->shared_blks_hit);
			if (usage->shared_blks_read > 0)
				appendStringInfo(es->str, " read=%lld",
								 (long long) usage->shared_blks_read);
			if (usage->shared_blks_dirtied > 0)
				appendStringInfo(es->str, " dirtied=%lld",
								 (long long) usage->shared_blks_dirtied);
			if (usage->shared_blks_written > 0)
				appendStringInfo(es->str, " written=%lld",
								 (long long) usage->shared_blks_written);
			if (has_local || has_temp)
				appendStringInfoChar(es->str, ',');
		}
		if (has_local)
		{
			appendStringInfoString(es->str, " local");
			if (usage->local_blks_hit > 0)
				appendStringInfo(es->str, " hit=%lld",
								 (long long) usage->local_blks_hit);
			if (usage->local_blks_read > 0)
				appendStringInfo(es->str, " read=%lld",
								 (long long) usage->local_blks_read);
			if (usage->local_blks_dirtied > 0)
				appendStringInfo(es->str, " dirtied=%lld",
								 (long long) usage->local_blks_dirtied);
			if (usage->local_blks_written > 0)
				appendStringInfo(es->str, " written=%lld",
								 (long long) usage->local_blks_written);
			if (has_temp)
				appendStringInfoChar(es->str, ',');
		}
		if (has_temp)
		{
			appendStringInfoString(es->str, " temp");
			if (usage->temp_blks_read > 0)
				appendStringInfo(es->str, " read=%lld",
								 (long long) usage->temp_blks_read);
			if (usage->temp_blks_written > 0)
				appendStringInfo(es->str, " written=%lld",
								 (long long) usage->temp_blks_written);
		}
		appendStringInfoChar(es->str, '\n');
	}

	/* As above, show only positive counter values. */
	if (has_timing)
	{
		ExplainIndentText(es);
		appendStringInfoString(es->str, "I/O Timings:");
		if (!INSTR_TIME_IS_ZERO(usage->blk_read_time))
			appendStringInfo(es->str, " read=%0.3f",
							 INSTR_TIME_GET_MILLISEC(usage->blk_read_time));
		if (!INSTR_TIME_IS_ZERO(usage->blk_write_time))
			appendStringInfo(es->str, " write=%0.3f",
							 INSTR_TIME_GET_MILLISEC(usage->blk_write_time));
		appendStringInfoChar(es->str, '\n');
	}

	if (show_planning)
		es->indent--;
}

/*
 * Show WAL usage details.
 */
static void
show_wal_usage(ExplainState *es, const WalUsage *usage)
{
	/* Show only positive counter values. */
	if ((usage->wal_records > 0) || (usage->wal_fpi > 0) ||
		(usage->wal_bytes > 0))
	{
		ExplainIndentText(es);
		appendStringInfoString(es->str, "WAL:");

		if (usage->wal_records > 0)
			appendStringInfo(es->str, " records=%lld",
							 (long long) usage->wal_records);
		if (usage->wal_fpi > 0)
			appendStringInfo(es->str, " fpi=%lld",
							 (long long) usage->wal_fpi);
		if (usage->wal_bytes > 0)
			appendStringInfo(es->str, " bytes=" UINT64_FORMAT,
							 usage->wal_bytes);
		appendStringInfoChar(es->str, '\n');
	}
}

/*
 * Add some additional details about an IndexScan or IndexOnlyScan
 */
static void
ExplainIndexScanDetails(Oid indexid, ScanDirection indexorderdir,
						ExplainState *es)
{
	const char *indexname = explain_get_index_name(indexid);

	if (ScanDirectionIsBackward(indexorderdir))
		appendStringInfoString(es->str, " Backward");
	appendStringInfo(es->str, " using %s", quote_identifier(indexname));
}

/*
 * Show the target of a Scan node
 */
static void
ExplainScanTarget(Scan *plan, ExplainState *es)
{
	ExplainTargetRel((Plan *) plan, plan->scanrelid, es);
}

/*
 * Show the target of a ModifyTable node
 *
 * Here we show the nominal target (ie, the relation that was named in the
 * original query).  If the actual target(s) is/are different, we'll show them
 * in show_modifytable_info().
 */
static void
ExplainModifyTarget(ModifyTable *plan, ExplainState *es)
{
	ExplainTargetRel((Plan *) plan, plan->nominalRelation, es);
}

/*
 * Show the target relation of a scan or modify node
 */
static void
ExplainTargetRel(Plan *plan, Index rti, ExplainState *es)
{
	char	   *objectname = NULL;
	char	   *namespace = NULL;
	RangeTblEntry *rte;
	char	   *refname;

	rte = rt_fetch(rti, es->rtable);
	refname = (char *) list_nth(es->rtable_names, rti - 1);
	if (refname == NULL)
		refname = rte->eref->aliasname;

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_CustomScan:
		case T_ModifyTable:
			/* Assert it's on a real relation */
			Assert(rte->rtekind == RTE_RELATION);
			objectname = get_rel_name(rte->relid);
			if (es->verbose)
				namespace = get_namespace_name(get_rel_namespace(rte->relid));
			break;
		case T_FunctionScan:
			{
				FunctionScan *fscan = (FunctionScan *) plan;

				/* Assert it's on a RangeFunction */
				Assert(rte->rtekind == RTE_FUNCTION);

				/*
				 * If the expression is still a function call of a single
				 * function, we can get the real name of the function.
				 * Otherwise, punt.  (Even if it was a single function call
				 * originally, the optimizer could have simplified it away.)
				 */
				if (list_length(fscan->functions) == 1)
				{
					RangeTblFunction *rtfunc = (RangeTblFunction *) linitial(fscan->functions);

					if (IsA(rtfunc->funcexpr, FuncExpr))
					{
						FuncExpr   *funcexpr = (FuncExpr *) rtfunc->funcexpr;
						Oid			funcid = funcexpr->funcid;

						objectname = get_func_name(funcid);
						if (es->verbose)
							namespace =
								get_namespace_name(get_func_namespace(funcid));
					}
				}
			}
			break;
		case T_ValuesScan:
			Assert(rte->rtekind == RTE_VALUES);
			break;
		case T_NamedTuplestoreScan:
			Assert(rte->rtekind == RTE_NAMEDTUPLESTORE);
			objectname = rte->enrname;
			break;
		default:
			break;
	}

	appendStringInfoString(es->str, " on");
	if (namespace != NULL)
		appendStringInfo(es->str, " %s.%s", quote_identifier(namespace),
						 quote_identifier(objectname));
	else if (objectname != NULL)
		appendStringInfo(es->str, " %s", quote_identifier(objectname));
	if (objectname == NULL || strcmp(refname, objectname) != 0)
		appendStringInfo(es->str, " %s", quote_identifier(refname));
}

/*
 * Show extra information for a ModifyTable node
 *
 * We have two objectives here.  First, if there's more than one target
 * table or it's different from the nominal target, identify the actual
 * target(s).  Second, show information about ON CONFLICT.
 */
static void
show_modifytable_info(ModifyTableState *mtstate, List *ancestors,
					  ExplainState *es)
{
	ModifyTable *node = (ModifyTable *) mtstate->ps.plan;
	const char *operation;
	bool		labeltargets;
	int			j;
	List	   *idxNames = NIL;
	ListCell   *lst;

	switch (node->operation)
	{
		case CMD_INSERT:
			operation = "Insert";
			break;
		case CMD_UPDATE:
			operation = "Update";
			break;
		case CMD_DELETE:
			operation = "Delete";
			break;
		default:
			operation = "???";
			break;
	}

	/* Should we explicitly label target relations? */
	labeltargets = (mtstate->mt_nrels > 1 ||
					(mtstate->mt_nrels == 1 &&
					 mtstate->resultRelInfo[0].ri_RangeTableIndex != node->nominalRelation));

	for (j = 0; j < mtstate->mt_nrels; j++)
	{
		ResultRelInfo *resultRelInfo = mtstate->resultRelInfo + j;

		if (labeltargets)
		{
			/*
			 * In text mode, decorate each target with operation type, so that
			 * ExplainTargetRel's output of " on foo" will read nicely.
			 */
			ExplainIndentText(es);
			appendStringInfoString(es->str, operation);
		}

		/* Identify target */
		ExplainTargetRel((Plan *) node,
						 resultRelInfo->ri_RangeTableIndex,
						 es);

		if (labeltargets)
		{
			appendStringInfoChar(es->str, '\n');
			es->indent++;
		}
	}

	/* Gather names of ON CONFLICT arbiter indexes */
	foreach(lst, node->arbiterIndexes)
	{
		char	   *indexname = get_rel_name(lfirst_oid(lst));

		idxNames = lappend(idxNames, indexname);
	}

	if (node->onConflictAction != ONCONFLICT_NONE)
	{
		ExplainPropertyText("Conflict Resolution",
							node->onConflictAction == ONCONFLICT_NOTHING ?
							"NOTHING" : "UPDATE",
							es);

		/*
		 * Don't display arbiter indexes at all when DO NOTHING variant
		 * implicitly ignores all conflicts
		 */
		if (idxNames)
			ExplainPropertyList("Conflict Arbiter Indexes", idxNames, es);

		/* ON CONFLICT DO UPDATE WHERE qual is specially displayed */
		if (node->onConflictWhere)
		{
			show_upper_qual((List *) node->onConflictWhere, "Conflict Filter",
							&mtstate->ps, ancestors, es);
			show_instrumentation_count("Rows Removed by Conflict Filter", 1, &mtstate->ps, es);
		}

		/* EXPLAIN ANALYZE display of actual outcome for each tuple proposed */
		if (es->analyze && mtstate->ps.instrument)
		{
			double		total;
			double		insert_path;
			double		other_path;

			InstrEndLoop(outerPlanState(mtstate)->instrument);

			/* count the number of source rows */
			total = outerPlanState(mtstate)->instrument->ntuples;
			other_path = mtstate->ps.instrument->ntuples2;
			insert_path = total - other_path;

			ExplainPropertyFloat("Tuples Inserted", NULL,
								 insert_path, 0, es);
			ExplainPropertyFloat("Conflicting Tuples", NULL,
								 other_path, 0, es);
		}
	}
}

/*
 * Explain the constituent plans of an Append, MergeAppend,
 * BitmapAnd, or BitmapOr node.
 *
 * The ancestors list should already contain the immediate parent of these
 * plans.
 */
static void
ExplainMemberNodes(PlanState **planstates, int nplans,
				   List *ancestors, ExplainState *es)
{
	int			j;

	for (j = 0; j < nplans; j++)
		ExplainNode(planstates[j], ancestors,
					"Member", NULL, es);
}

/*
 * Report about any pruned subnodes of an Append or MergeAppend node.
 *
 * nplans indicates the number of live subplans.
 * nchildren indicates the original number of subnodes in the Plan;
 * some of these may have been pruned by the run-time pruning code.
 */
static void
ExplainMissingMembers(int nplans, int nchildren, ExplainState *es)
{
	if (nplans < nchildren)
		ExplainPropertyInteger("Subplans Removed", NULL,
							   nchildren - nplans, es);
}

/*
 * Explain a list of SubPlans (or initPlans, which also use SubPlan nodes).
 *
 * The ancestors list should already contain the immediate parent of these
 * SubPlans.
 */
static void
ExplainSubPlans(List *plans, List *ancestors,
				const char *relationship, ExplainState *es)
{
	ListCell   *lst;

	foreach(lst, plans)
	{
		SubPlanState *sps = (SubPlanState *) lfirst(lst);
		SubPlan    *sp = sps->subplan;

		/*
		 * There can be multiple SubPlan nodes referencing the same physical
		 * subplan (same plan_id, which is its index in PlannedStmt.subplans).
		 * We should print a subplan only once, so track which ones we already
		 * printed.  This state must be global across the plan tree, since the
		 * duplicate nodes could be in different plan nodes, eg both a bitmap
		 * indexscan's indexqual and its parent heapscan's recheck qual.  (We
		 * do not worry too much about which plan node we show the subplan as
		 * attached to in such cases.)
		 */
		if (bms_is_member(sp->plan_id, es->printed_subplans))
			continue;
		es->printed_subplans = bms_add_member(es->printed_subplans,
											  sp->plan_id);

		/*
		 * Treat the SubPlan node as an ancestor of the plan node(s) within
		 * it, so that ruleutils.c can find the referents of subplan
		 * parameters.
		 */
		ancestors = lcons(sp, ancestors);

		ExplainNode(sps->planstate, ancestors,
					relationship, sp->plan_name, es);

		ancestors = list_delete_first(ancestors);
	}
}

/*
 * Explain a list of children of a CustomScan.
 */
static void
ExplainCustomChildren(CustomScanState *css, List *ancestors, ExplainState *es)
{
	ListCell   *cell;
	const char *label =
	(list_length(css->custom_ps) != 1 ? "children" : "child");

	foreach(cell, css->custom_ps)
		ExplainNode((PlanState *) lfirst(cell), ancestors, label, NULL, es);
}

/*
 * Create a per-plan-node workspace for collecting per-worker data.
 *
 * Output related to each worker will be temporarily "set aside" into a
 * separate buffer, which we'll merge into the main output stream once
 * we've processed all data for the plan node.  This makes it feasible to
 * generate a coherent sub-group of fields for each worker, even though the
 * code that produces the fields is in several different places in this file.
 */
static ExplainWorkersState *
ExplainCreateWorkersState(int num_workers)
{
	ExplainWorkersState *wstate;

	wstate = (ExplainWorkersState *) palloc(sizeof(ExplainWorkersState));
	wstate->num_workers = num_workers;
	wstate->worker_inited = (bool *) palloc0(num_workers * sizeof(bool));
	wstate->worker_str = (StringInfoData *)
		palloc0(num_workers * sizeof(StringInfoData));
	return wstate;
}

/*
 * Begin or resume output into the set-aside group for worker N.
 */
static void
ExplainOpenWorker(int n, ExplainState *es)
{
	ExplainWorkersState *wstate = es->workers_state;

	Assert(wstate);
	Assert(n >= 0 && n < wstate->num_workers);

	/* Save prior output buffer pointer */
	wstate->prev_str = es->str;

	if (!wstate->worker_inited[n])
	{
		/* First time through, so create the buffer for this worker */
		initStringInfo(&wstate->worker_str[n]);
		wstate->worker_inited[n] = true;
	}

	/* Resume output for a worker we've already emitted some data for */
	es->str = &wstate->worker_str[n];

	/*
	 * Prefix the first output line for this worker with "Worker N:".  Then,
	 * any additional lines should be indented one more stop than the
	 * "Worker N" line is.
	 */
	if (es->str->len == 0)
	{
		ExplainIndentText(es);
		appendStringInfo(es->str, "Worker %d:  ", n);
	}

	es->indent++;
}

/*
 * End output for worker N --- must pair with previous ExplainOpenWorker call
 */
static void
ExplainCloseWorker(int n, ExplainState *es)
{
	ExplainWorkersState *wstate = es->workers_state;

	Assert(wstate);
	Assert(n >= 0 && n < wstate->num_workers);
	Assert(wstate->worker_inited[n]);

	/*
	 * If we didn't actually produce any output line(s) then truncate off the
	 * partial line emitted by ExplainOpenWorker.  (This is to avoid bogus
	 * output if, say, show_buffer_usage chooses not to print anything for the
	 * worker.)  Also fix up the indent level.
	 */
	while (es->str->len > 0 && es->str->data[es->str->len - 1] != '\n')
		es->str->data[--(es->str->len)] = '\0';

	es->indent--;

	/* Restore prior output buffer pointer */
	es->str = wstate->prev_str;
}

/*
 * Print per-worker info for current node, then free the ExplainWorkersState.
 */
static void
ExplainFlushWorkersState(ExplainState *es)
{
	ExplainWorkersState *wstate = es->workers_state;

	for (int i = 0; i < wstate->num_workers; i++)
	{
		if (wstate->worker_inited[i])
		{
			appendStringInfoString(es->str, wstate->worker_str[i].data);

			pfree(wstate->worker_str[i].data);
		}
	}

	pfree(wstate->worker_inited);
	pfree(wstate->worker_str);
	pfree(wstate);
}

/*
 * Explain a property, such as sort keys or targets, that takes the form of
 * a list of unlabeled items.  "data" is a list of C strings.
 */
void
ExplainPropertyList(const char *qlabel, List *data, ExplainState *es)
{
	ListCell   *lc;
	bool		first = true;

	ExplainIndentText(es);
	appendStringInfo(es->str, "%s: ", qlabel);
	foreach(lc, data)
	{
		if (!first)
			appendStringInfoString(es->str, ", ");
		appendStringInfoString(es->str, (const char *) lfirst(lc));
		first = false;
	}
	appendStringInfoChar(es->str, '\n');
}

/*
 * Explain a simple property.
 *
 * If unit is non-NULL the text format will display it after the value.
 *
 * This usually should not be invoked directly, but via one of the datatype
 * specific routines ExplainPropertyText, ExplainPropertyInteger, etc.
 */
static void
ExplainProperty(const char *qlabel, const char *unit, const char *value,
				ExplainState *es)
{
	ExplainIndentText(es);
	if (unit)
		appendStringInfo(es->str, "%s: %s %s\n", qlabel, value, unit);
	else
		appendStringInfo(es->str, "%s: %s\n", qlabel, value);
}

/*
 * Explain a string-valued property.
 */
void
ExplainPropertyText(const char *qlabel, const char *value, ExplainState *es)
{
	ExplainProperty(qlabel, NULL, value, es);
}

/*
 * Explain an integer-valued property.
 */
void
ExplainPropertyInteger(const char *qlabel, const char *unit, int64 value,
					   ExplainState *es)
{
	char		buf[32];

	snprintf(buf, sizeof(buf), INT64_FORMAT, value);
	ExplainProperty(qlabel, unit, buf, es);
}

/*
 * Explain an unsigned integer-valued property.
 */
void
ExplainPropertyUInteger(const char *qlabel, const char *unit, uint64 value,
						ExplainState *es)
{
	char		buf[32];

	snprintf(buf, sizeof(buf), UINT64_FORMAT, value);
	ExplainProperty(qlabel, unit, buf, es);
}

/*
 * Explain a float-valued property, using the specified number of
 * fractional digits.
 */
void
ExplainPropertyFloat(const char *qlabel, const char *unit, double value,
					 int ndigits, ExplainState *es)
{
	char	   *buf;

	buf = psprintf("%.*f", ndigits, value);
	ExplainProperty(qlabel, unit, buf, es);
	pfree(buf);
}

/*
 * Explain a bool-valued property.
 */
void
ExplainPropertyBool(const char *qlabel, bool value, ExplainState *es)
{
	ExplainProperty(qlabel, NULL, value ? "true" : "false", es);
}

/*
 * Indent a text-format line.
 *
 * We indent by two spaces per indentation level.  However, when emitting
 * data for a parallel worker there might already be data on the current line
 * (cf. ExplainOpenWorker); in that case, don't indent any more.
 */
static void
ExplainIndentText(ExplainState *es)
{
	if (es->str->len == 0 || es->str->data[es->str->len - 1] == '\n')
		appendStringInfoSpaces(es->str, es->indent * 2);
}

