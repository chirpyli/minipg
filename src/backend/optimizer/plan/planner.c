/*-------------------------------------------------------------------------
 *
 * planner.c
 *	  The query optimizer external interface.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planner.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "lib/bipartite_match.h"
#include "lib/knapsack.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#ifdef OPTIMIZER_DEBUG
#include "nodes/print.h"
#endif
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/paramassign.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/analyze.h"
#include "parser/parse_agg.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"

/* Hook for plugins to get control when grouping_planner() plans upper rels */
create_upper_paths_hook_type create_upper_paths_hook = NULL;


/* Expression kind codes for preprocess_expression */
#define EXPRKIND_QUAL				0
#define EXPRKIND_TARGET				1
#define EXPRKIND_VALUES				2
#define EXPRKIND_VALUES_LATERAL		3
#define EXPRKIND_LIMIT				4
#define EXPRKIND_APPINFO			5
#define EXPRKIND_PHV				6
#define EXPRKIND_TABLEFUNC			11
#define EXPRKIND_TABLEFUNC_LATERAL	12

/* Passthrough data for standard_qp_callback */
typedef struct
{
	List	   *groupClause;	/* overrides parse->groupClause */
} standard_qp_extra;


/* Local functions */
static Node *preprocess_expression(PlannerInfo *root, Node *expr, int kind);
static void preprocess_qual_conditions(PlannerInfo *root, Node *jtnode);
static void grouping_planner(PlannerInfo *root, double tuple_fraction);

static void preprocess_rowmarks(PlannerInfo *root);
static void remove_useless_groupby_columns(PlannerInfo *root);
static List *preprocess_groupclause(PlannerInfo *root, List *force);
static void standard_qp_callback(PlannerInfo *root, void *extra);
static double get_number_of_groups(PlannerInfo *root,
								   double path_rows,
								   List *target_list);
static RelOptInfo *create_grouping_paths(PlannerInfo *root,
										 RelOptInfo *input_rel,
										 PathTarget *target);
static bool is_degenerate_grouping(PlannerInfo *root);
static void create_degenerate_grouping_paths(PlannerInfo *root,
											 RelOptInfo *input_rel,
											 RelOptInfo *grouped_rel);
static RelOptInfo *make_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
									 PathTarget *target);
static void create_ordinary_grouping_paths(PlannerInfo *root,
										   RelOptInfo *input_rel,
										   RelOptInfo *grouped_rel,
										   const AggClauseCosts *agg_costs,
										   GroupPathExtraData *extra);
static RelOptInfo *create_distinct_paths(PlannerInfo *root,
										 RelOptInfo *input_rel);
static RelOptInfo *create_ordered_paths(PlannerInfo *root,
										RelOptInfo *input_rel,
										PathTarget *target);
static PathTarget *make_group_input_target(PlannerInfo *root,
										   PathTarget *final_target);
static PathTarget *make_sort_input_target(PlannerInfo *root,
										  PathTarget *final_target,
										  bool *have_postponed_srfs);
static void adjust_paths_for_srfs(PlannerInfo *root, RelOptInfo *rel,
								  List *targets, List *targets_contain_srfs);
static void add_paths_to_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
									  RelOptInfo *grouped_rel,
									  const AggClauseCosts *agg_costs,
									  double dNumGroups,
									  GroupPathExtraData *extra);
static void apply_scanjoin_target_to_paths(PlannerInfo *root,
										   RelOptInfo *rel,
										   List *scanjoin_targets,
										   List *scanjoin_targets_contain_srfs,
										   bool tlist_same_exprs);


/*****************************************************************************
 *
 *	   Query optimizer entry point
 *
 * To support loadable plugins that monitor or modify planner behavior,
 * we provide a hook variable that lets a plugin get control before and
 * after the standard planning process.  The plugin would normally call
 * standard_planner().
 *
 * Note to plugin authors: standard_planner() scribbles on its Query input,
 * so you'd better copy that data structure if you want to plan more than once.
 *
 *****************************************************************************/
PlannedStmt *
planner(Query *parse, const char *query_string, int cursorOptions,
		ParamListInfo boundParams)
{
	PlannedStmt *result = standard_planner(parse, query_string, cursorOptions, boundParams);
	return result;
}

PlannedStmt *
standard_planner(Query *parse, const char *query_string, int cursorOptions,
				 ParamListInfo boundParams)
{
	PlannedStmt *result;
	PlannerGlobal *glob;
	double		tuple_fraction;
	PlannerInfo *root;
	RelOptInfo *final_rel;
	Path	   *best_path;
	Plan	   *top_plan;
	ListCell   *lp,
			   *lr;

	/*
	 * Set up global state for this planner invocation.  This data is needed
	 * across all levels of sub-Query that might exist in the given command,
	 * so we keep it in a separate struct that's linked to by each per-Query
	 * PlannerInfo.
	 */
	glob = makeNode(PlannerGlobal);

	glob->boundParams = boundParams;
	glob->subplans = NIL;
	glob->subroots = NIL;
	glob->rewindPlanIDs = NULL;
	glob->finalrtable = NIL;
	glob->finalrowmarks = NIL;
	glob->resultRelations = NIL;
	glob->appendRelations = NIL;
	glob->relationOids = NIL;
	glob->invalItems = NIL;
	glob->paramExecTypes = NIL;
	glob->lastPHId = 0;
	glob->lastRowMarkId = 0;
	glob->lastPlanNodeId = 0;
	glob->transientPlan = false;
	glob->dependsOnRole = false;

	/* Default assumption is we need all the tuples */
	tuple_fraction = 0.0;

	/* primary planning entry point (may recurse for subqueries) */
	root = subquery_planner(glob, parse, NULL,
							false, tuple_fraction);

	/* Select best Path and turn it into a Plan */
	final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);
	best_path = get_cheapest_fractional_path(final_rel, tuple_fraction);

	top_plan = create_plan(root, best_path);

	/*
	 * If any Params were generated, run through the plan tree and compute
	 * each plan node's extParam/allParam sets.  Ideally we'd merge this into
	 * set_plan_references' tree traversal, but for now it has to be separate
	 * because we need to visit subplans before not after main plan.
	 */
	if (glob->paramExecTypes != NIL)
	{
		Assert(list_length(glob->subplans) == list_length(glob->subroots));
		forboth(lp, glob->subplans, lr, glob->subroots)
		{
			Plan	   *subplan = (Plan *) lfirst(lp);
			PlannerInfo *subroot = lfirst_node(PlannerInfo, lr);

			SS_finalize_plan(subroot, subplan);
		}
		SS_finalize_plan(root, top_plan);
	}

	/* final cleanup of the plan */
	Assert(glob->finalrtable == NIL);
	Assert(glob->finalrowmarks == NIL);
	Assert(glob->resultRelations == NIL);
	Assert(glob->appendRelations == NIL);
	top_plan = set_plan_references(root, top_plan);
	/* ... and the subplans (both regular subplans and initplans) */
	Assert(list_length(glob->subplans) == list_length(glob->subroots));
	forboth(lp, glob->subplans, lr, glob->subroots)
	{
		Plan	   *subplan = (Plan *) lfirst(lp);
		PlannerInfo *subroot = lfirst_node(PlannerInfo, lr);

		lfirst(lp) = set_plan_references(subroot, subplan);
	}

	/* build the PlannedStmt result */
	result = makeNode(PlannedStmt);

	result->commandType = parse->commandType;
	result->queryId = parse->queryId;
	result->canSetTag = parse->canSetTag;
	result->transientPlan = glob->transientPlan;
	result->dependsOnRole = glob->dependsOnRole;
	result->planTree = top_plan;
	result->rtable = glob->finalrtable;
	result->resultRelations = glob->resultRelations;
	result->appendRelations = glob->appendRelations;
	result->subplans = glob->subplans;
	result->rewindPlanIDs = glob->rewindPlanIDs;
	result->rowMarks = glob->finalrowmarks;
	result->relationOids = glob->relationOids;
	result->invalItems = glob->invalItems;
	result->paramExecTypes = glob->paramExecTypes;
	/* utilityStmt should be null, but we might as well copy it */
	result->utilityStmt = parse->utilityStmt;
	result->stmt_location = parse->stmt_location;
	result->stmt_len = parse->stmt_len;

	return result;
}


/*--------------------
 * subquery_planner
 *	  Invokes the planner on a subquery.  We recurse to here for each
 *	  sub-SELECT found in the query tree.
 *
 * glob is the global state for the current planner run.
 * parse is the querytree produced by the parser & rewriter.
 * parent_root is the immediate parent Query's info (NULL at the top level).
 * hasRecursion is true if this is a recursive WITH query.
 * tuple_fraction is the fraction of tuples we expect will be retrieved.
 * tuple_fraction is interpreted as explained for grouping_planner, below.
 *
 * Basically, this routine does the stuff that should only be done once
 * per Query object.  It then calls grouping_planner.  At one time,
 * grouping_planner could be invoked recursively on the same Query object;
 * that's not currently true, but we keep the separation between the two
 * routines anyway, in case we need it again someday.
 *
 * subquery_planner will be called recursively to handle sub-Query nodes
 * found within the query's expressions and rangetable.
 *
 * Returns the PlannerInfo struct ("root") that contains all data generated
 * while planning the subquery.  In particular, the Path(s) attached to
 * the (UPPERREL_FINAL, NULL) upperrel represent our conclusions about the
 * cheapest way(s) to implement the query.  The top level will select the
 * best Path and pass it through createplan.c to produce a finished Plan.
 *--------------------
 */
PlannerInfo *
subquery_planner(PlannerGlobal *glob, Query *parse,
				 PlannerInfo *parent_root,
				 bool hasRecursion, double tuple_fraction)
{
	PlannerInfo *root;
	List	   *newHaving;
	bool		hasOuterJoins;
	bool		hasResultRTEs;
	RelOptInfo *final_rel;
	ListCell   *l;

	/* Create a PlannerInfo data structure for this subquery */
	root = makeNode(PlannerInfo);
	root->parse = parse;
	root->glob = glob;
	root->query_level = parent_root ? parent_root->query_level + 1 : 1;
	root->parent_root = parent_root;
	root->plan_params = NIL;
	root->outer_params = NULL;
	root->planner_cxt = CurrentMemoryContext;
	root->init_plans = NIL;
	root->eq_classes = NIL;
	root->ec_merging_done = false;
	root->all_result_relids =
		parse->resultRelation ? bms_make_singleton(parse->resultRelation) : NULL;
	root->leaf_result_relids = NULL;	/* we'll find out leaf-ness later */
	root->append_rel_list = NIL;
	root->row_identity_vars = NIL;
	root->rowMarks = NIL;
	memset(root->upper_rels, 0, sizeof(root->upper_rels));
	memset(root->upper_targets, 0, sizeof(root->upper_targets));
	root->processed_tlist = NIL;
	root->update_colnos = NIL;

	root->qual_security_level = 0;
	root->hasPseudoConstantQuals = false;
	root->hasRecursion = hasRecursion;
	if (hasRecursion)
		root->wt_param_id = assign_special_exec_param(root);
	else
		root->wt_param_id = -1;
	root->non_recursive_path = NULL;


	/*
	 * If the FROM clause is empty, replace it with a dummy RTE_RESULT RTE, so
	 * that we don't need so many special cases to deal with that situation.
	 */
	replace_empty_jointree(parse);

	/*
	 * Look for ANY and EXISTS SubLinks in WHERE and JOIN/ON clauses, and try
	 * to transform them into joins.  Note that this step does not descend
	 * into subqueries; if we pull up any subqueries below, their SubLinks are
	 * processed just before pulling them up.
	 */
	if (parse->hasSubLinks)
		pull_up_sublinks(root);

	/*
	 * Check to see if any subqueries in the jointree can be merged into this
	 * query.
	 */
	pull_up_subqueries(root);

	/*
	 * Survey the rangetable to see what kinds of entries are present.  We can
	 * skip some later processing if relevant SQL features are not used; for
	 * example if there are no JOIN RTEs we can avoid the expense of doing
	 * flatten_join_alias_vars().  This must be done after we have finished
	 * adding rangetable entries, of course.  (Note: actually, processing of
	 * inherited rels can cause RTEs for their child tables to
	 * get added later; but those must all be RTE_RELATION entries, so they
	 * don't invalidate the conclusions drawn here.)
	 */
	root->hasJoinRTEs = false;
	root->hasLateralRTEs = false;
	hasOuterJoins = false;
	hasResultRTEs = false;
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);

		switch (rte->rtekind)
		{
			case RTE_RELATION:

				/*
				 * Inheritance is no longer supported, so no relation can have
				 * children; clear the inh flag so the rel is always treated
				 * as a plain base relation.
				 */
				break;
			case RTE_JOIN:
				root->hasJoinRTEs = true;
				if (IS_OUTER_JOIN(rte->jointype))
					hasOuterJoins = true;
				break;
			case RTE_RESULT:
				hasResultRTEs = true;
				break;
			default:
				/* No work here for other RTE types */
				break;
		}

		if (rte->lateral)
			root->hasLateralRTEs = true;
	}

	/*
	 * If we have now verified that the query target relation is
	 * non-inheriting, mark it as a leaf target.
	 */
	if (parse->resultRelation)
	{
		root->leaf_result_relids = bms_make_singleton(parse->resultRelation);
	}

	/*
	 * Preprocess RowMark information.  We need to do this after subquery
	 * pullup, so that all base relations are present.
	 */
	preprocess_rowmarks(root);

	/*
	 * Set hasHavingQual to remember if HAVING clause is present.  Needed
	 * because preprocess_expression will reduce a constant-true condition to
	 * an empty qual list ... but "HAVING TRUE" is not a semantic no-op.
	 */
	root->hasHavingQual = (parse->havingQual != NULL);

	/*
	 * Do expression preprocessing on targetlist and quals, as well as other
	 * random expressions in the querytree.  Note that we do not need to
	 * handle sort/group expressions explicitly, because they are actually
	 * part of the targetlist.
	 */
	parse->targetList = (List *)
		preprocess_expression(root, (Node *) parse->targetList,
							  EXPRKIND_TARGET);

	/* Constant-folding might have removed all set-returning functions */
	if (parse->hasTargetSRFs)
		parse->hasTargetSRFs = expression_returns_set((Node *) parse->targetList);

	preprocess_qual_conditions(root, (Node *) parse->jointree);

	parse->havingQual = preprocess_expression(root, parse->havingQual,
											  EXPRKIND_QUAL);

	root->append_rel_list = (List *)
		preprocess_expression(root, (Node *) root->append_rel_list,
							  EXPRKIND_APPINFO);

	/* Also need to preprocess expressions within RTEs */
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);
		int			kind;

		if (rte->rtekind == RTE_SUBQUERY)
		{
			/*
			 * We don't want to do all preprocessing yet on the subquery's
			 * expressions, since that will happen when we plan it.  But if it
			 * contains any join aliases of our level, those have to get
			 * expanded now, because planning of the subquery won't do it.
			 * That's only possible if the subquery is LATERAL.
			 */
			if (rte->lateral && root->hasJoinRTEs)
				rte->subquery = (Query *)
					flatten_join_alias_vars(root->parse,
											(Node *) rte->subquery);
		}
		else if (rte->rtekind == RTE_VALUES)
		{
			/* Preprocess the values lists fully */
			kind = rte->lateral ? EXPRKIND_VALUES_LATERAL : EXPRKIND_VALUES;
			rte->values_lists = (List *)
				preprocess_expression(root, (Node *) rte->values_lists, kind);
		}
	}

	/*
	 * Now that we are done preprocessing expressions, and in particular done
	 * flattening join alias variables, get rid of the joinaliasvars lists.
	 * They no longer match what expressions in the rest of the tree look
	 * like, because we have not preprocessed expressions in those lists (and
	 * do not want to; for example, expanding a SubLink there would result in
	 * a useless unreferenced subplan).  Leaving them in place simply creates
	 * a hazard for later scans of the tree.  We could try to prevent that by
	 * using QTW_IGNORE_JOINALIASES in every tree scan done after this point,
	 * but that doesn't sound very reliable.
	 */
	if (root->hasJoinRTEs)
	{
		foreach(l, parse->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);

			rte->joinaliasvars = NIL;
		}
	}

	/*
	 * In some cases we may want to transfer a HAVING clause into WHERE. We
	 * cannot do so if the HAVING clause contains aggregates (obviously) or
	 * volatile functions (since a HAVING clause is supposed to be executed
	 * only once per group).  We also can't do this if there are any nonempty
	 * grouping sets; moving such a clause into WHERE would potentially change
	 * the results, if any referenced column isn't present in all the grouping
	 * sets.  (If there are only empty grouping sets, then the HAVING clause
	 * must be degenerate as discussed below.)
	 *
	 * Also, it may be that the clause is so expensive to execute that we're
	 * better off doing it only once per group, despite the loss of
	 * selectivity.  This is hard to estimate short of doing the entire
	 * planning process twice, so we use a heuristic: clauses containing
	 * subplans are left in HAVING.  Otherwise, we move or copy the HAVING
	 * clause into WHERE, in hopes of eliminating tuples before aggregation
	 * instead of after.
	 *
	 * If the query has explicit grouping then we can simply move such a
	 * clause into WHERE; any group that fails the clause will not be in the
	 * output because none of its tuples will reach the grouping or
	 * aggregation stage.  Otherwise we must have a degenerate (variable-free)
	 * HAVING clause, which we put in WHERE so that query_planner() can use it
	 * in a gating Result node, but also keep in HAVING to ensure that we
	 * don't emit a bogus aggregated row. (This could be done better, but it
	 * seems not worth optimizing.)
	 *
	 * Note that both havingQual and parse->jointree->quals are in
	 * implicitly-ANDed-list form at this point, even though they are declared
	 * as Node *.
	 */
	newHaving = NIL;
	foreach(l, (List *) parse->havingQual)
	{
		Node	   *havingclause = (Node *) lfirst(l);

		if (contain_agg_clause(havingclause) ||
			contain_volatile_functions(havingclause) ||
			contain_subplans(havingclause))
		{
			/* keep it in HAVING */
			newHaving = lappend(newHaving, havingclause);
		}
		else if (parse->groupClause)
		{
			/* move it to WHERE */
			parse->jointree->quals = (Node *)
				lappend((List *) parse->jointree->quals, havingclause);
		}
		else
		{
			/* put a copy in WHERE, keep it in HAVING */
			parse->jointree->quals = (Node *)
				lappend((List *) parse->jointree->quals,
						copyObject(havingclause));
			newHaving = lappend(newHaving, havingclause);
		}
	}
	parse->havingQual = (Node *) newHaving;

	/* Remove any redundant GROUP BY columns */
	remove_useless_groupby_columns(root);

	/*
	 * If we have any outer joins, try to reduce them to plain inner joins.
	 * This step is most easily done after we've done expression
	 * preprocessing.
	 */
	if (hasOuterJoins)
		reduce_outer_joins(root);

	/*
	 * If we have any RTE_RESULT relations, see if they can be deleted from
	 * the jointree.  This step is most effectively done after we've done
	 * expression preprocessing and outer join reduction.
	 */
	if (hasResultRTEs)
		remove_useless_result_rtes(root);

	/*
	 * Do the main planning.
	 */
	grouping_planner(root, tuple_fraction);

	/*
	 * Capture the set of outer-level param IDs we have access to, for use in
	 * extParam/allParam calculations later.
	 */
	SS_identify_outer_params(root);

	/*
	 * If any initPlans were created in this query level, adjust the surviving
	 * Paths' costs to account for them.  The initPlans won't actually get
	 * attached to the plan tree till create_plan() runs, but we must include
	 * their effects now.
	 */
	final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);
	SS_charge_for_initplans(root, final_rel);

	/*
	 * Make sure we've identified the cheapest Path for the final rel.  (By
	 * doing this here not in grouping_planner, we include initPlan costs in
	 * the decision, though it's unlikely that will change anything.)
	 */
	set_cheapest(final_rel);

	return root;
}

/*
 * preprocess_expression
 *		Do subquery_planner's preprocessing work for an expression,
 *		which can be a targetlist, a WHERE clause (including JOIN/ON
 *		conditions), a HAVING clause, or a few other things.
 */
static Node *
preprocess_expression(PlannerInfo *root, Node *expr, int kind)
{
	/*
	 * Fall out quickly if expression is empty.  This occurs often enough to
	 * be worth checking.  Note that null->null is the correct conversion for
	 * implicit-AND result format, too.
	 */
	if (expr == NULL)
		return NULL;

	/*
	 * If the query has any join RTEs, replace join alias variables with
	 * base-relation variables.  We must do this first, since any expressions
	 * we may extract from the joinaliasvars lists have not been preprocessed.
	 * For example, if we did this after sublink processing, sublinks expanded
	 * out from join aliases would not get processed.  But we can skip this in
	 * non-lateral VALUES lists, since they can't contain
	 * any Vars of the current query level.
	 */
	if (root->hasJoinRTEs &&
		!(kind == EXPRKIND_VALUES ||
		  kind == EXPRKIND_TABLEFUNC))
		expr = flatten_join_alias_vars(root->parse, expr);

	/*
	 * Simplify constant expressions.
	 *
	 * Note: an essential effect of this is to convert named-argument function
	 * calls to positional notation and insert the current actual values of
	 * any default arguments for functions.  To ensure that happens, we *must*
	 * process all expressions here.  Previous PG versions sometimes skipped
	 * const-simplification if it didn't seem worth the trouble, but we can't
	 * do that anymore.
	 *
	 * Note: this also flattens nested AND and OR expressions into N-argument
	 * form.  All processing of a qual expression after this point must be
	 * careful to maintain AND/OR flatness --- that is, do not generate a tree
	 * with AND directly under AND, nor OR directly under OR.
	 */
	expr = eval_const_expressions(root, expr);

	/*
	 * If it's a qual or havingQual, canonicalize it.
	 */
	if (kind == EXPRKIND_QUAL)
	{
		expr = (Node *) canonicalize_qual((Expr *) expr, false);

#ifdef OPTIMIZER_DEBUG
		printf("After canonicalize_qual()\n");
		pprint(expr);
#endif
	}

	/*
	 * Check for ANY ScalarArrayOpExpr with Const arrays and set the
	 * hashfuncid of any that might execute more quickly by using hash lookups
	 * instead of a linear search.
	 */
	if (kind == EXPRKIND_QUAL || kind == EXPRKIND_TARGET)
	{
		convert_saop_to_hashed_saop(expr);
	}

	/* Expand SubLinks to SubPlans */
	if (root->parse->hasSubLinks)
		expr = SS_process_sublinks(root, expr, (kind == EXPRKIND_QUAL));


	/* Replace uplevel vars with Param nodes (this IS possible in VALUES) */
	if (root->query_level > 1)
		expr = SS_replace_correlation_vars(root, expr);

	/*
	 * If it's a qual or havingQual, convert it to implicit-AND format. (We
	 * don't want to do this before eval_const_expressions, since the latter
	 * would be unable to simplify a top-level AND correctly. Also,
	 * SS_process_sublinks expects explicit-AND format.)
	 */
	if (kind == EXPRKIND_QUAL)
		expr = (Node *) make_ands_implicit((Expr *) expr);

	return expr;
}

/*
 * preprocess_qual_conditions
 *		Recursively scan the query's jointree and do subquery_planner's
 *		preprocessing work on each qual condition found therein.
 */
static void
preprocess_qual_conditions(PlannerInfo *root, Node *jtnode)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		/* nothing to do here */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			preprocess_qual_conditions(root, lfirst(l));

		f->quals = preprocess_expression(root, f->quals, EXPRKIND_QUAL);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		preprocess_qual_conditions(root, j->larg);
		preprocess_qual_conditions(root, j->rarg);

		j->quals = preprocess_expression(root, j->quals, EXPRKIND_QUAL);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * preprocess_phv_expression
 *	  Do preprocessing on a PlaceHolderVar expression that's been pulled up.
 *
 * If a LATERAL subquery references an output of another subquery, and that
 * output must be wrapped in a PlaceHolderVar because of an intermediate outer
 * join, then we'll push the PlaceHolderVar expression down into the subquery
 * and later pull it back up during find_lateral_references, which runs after
 * subquery_planner has preprocessed all the expressions that were in the
 * current query level to start with.  So we need to preprocess it then.
 */
Expr *
preprocess_phv_expression(PlannerInfo *root, Expr *expr)
{
	return (Expr *) preprocess_expression(root, (Node *) expr, EXPRKIND_PHV);
}

/*--------------------
 * grouping_planner
 *	  Perform planning steps related to grouping, aggregation, etc.
 *
 * This function adds all required top-level processing to the scan/join
 * Path(s) produced by query_planner.
 *
 * tuple_fraction is the fraction of tuples we expect will be retrieved.
 * tuple_fraction is interpreted as follows:
 *	  0: expect all tuples to be retrieved (normal case)
 *	  0 < tuple_fraction < 1: expect the given fraction of tuples available
 *		from the plan to be retrieved
 *	  tuple_fraction >= 1: tuple_fraction is the absolute number of tuples
 *		expected to be retrieved (ie, a LIMIT specification)
 *
 * Returns nothing; the useful output is in the Paths we attach to the
 * (UPPERREL_FINAL, NULL) upperrel in *root.  In addition,
 * root->processed_tlist contains the final processed targetlist.
 *
 * Note that we have not done set_cheapest() on the final rel; it's convenient
 * to leave this to the caller.
 *--------------------
 */
static void
grouping_planner(PlannerInfo *root, double tuple_fraction)
{
	Query	   *parse = root->parse;
	bool		have_postponed_srfs = false;
	PathTarget *final_target;
	List	   *final_targets;
	List	   *final_targets_contain_srfs;
	RelOptInfo *current_rel;
	RelOptInfo *final_rel;
	ListCell   *lc;
	PathTarget *sort_input_target;
	List	   *sort_input_targets;
	List	   *sort_input_targets_contain_srfs;
	PathTarget *grouping_target;
	List	   *grouping_targets;
	List	   *grouping_targets_contain_srfs;
	PathTarget *scanjoin_target;
	List	   *scanjoin_targets;
	List	   *scanjoin_targets_contain_srfs;
	bool		scanjoin_target_same_exprs;
	bool		have_grouping;
	standard_qp_extra qp_extra;

		/* Make tuple_fraction accessible to lower-level routines */
		root->tuple_fraction = tuple_fraction;

		/* A recursive query (WITH RECURSIVE) is handled separately */
		Assert(!root->hasRecursion);

		/* Preprocess regular GROUP BY clause, if any */
		if (parse->groupClause)
			parse->groupClause = preprocess_groupclause(root, NIL);

		/*
		 * Preprocess targetlist.  Note that much of the remaining planning
		 * work will be done with the PathTarget representation of tlists, but
		 * we must also maintain the full representation of the final tlist so
		 * that we can transfer its decoration (resnames etc) to the topmost
		 * tlist of the finished Plan.  This is kept in processed_tlist.
		 */
		preprocess_targetlist(root);

		/*
		 * Mark all the aggregates with resolved aggtranstypes, and detect
		 * aggregates that are duplicates or can share transition state.  We
		 * must do this before slicing and dicing the tlist into various
		 * pathtargets, else some copies of the Aggref nodes might escape
		 * being marked.
		 */
		if (parse->hasAggs)
		{
			preprocess_aggrefs(root, (Node *) root->processed_tlist);
			preprocess_aggrefs(root, (Node *) parse->havingQual);
		}

		/* Set up data needed by standard_qp_callback */
		qp_extra.groupClause = parse->groupClause;

		/*
		 * Generate the best unsorted and presorted paths for the scan/join
		 * portion of this Query, ie the processing represented by the
		 * FROM/WHERE clauses.  (Note there may not be any presorted paths.)
		 * We also generate (in standard_qp_callback) pathkey representations
		 * of the query's sort clause, distinct clause, etc.
		 */
		current_rel = query_planner(root, standard_qp_callback, &qp_extra);

		/*
		 * Convert the query's result tlist into PathTarget format.
		 *
		 * Note: this cannot be done before query_planner() has performed
		 * appendrel expansion, because that might add resjunk entries to
		 * root->processed_tlist.  Waiting till afterwards is also helpful
		 * because the target width estimates can use per-Var width numbers
		 * that were obtained within query_planner().
		 */
		final_target = create_pathtarget(root, root->processed_tlist);

		/*
		 * If ORDER BY was given, consider whether we should use a post-sort
		 * projection, and compute the adjusted target for preceding steps if
		 * so.
		 */
		if (parse->sortClause)
		{
			sort_input_target = make_sort_input_target(root,
													   final_target,
													   &have_postponed_srfs);
		}
		else
		{
			sort_input_target = final_target;
		}

		/*
		 * If we have window functions to deal with, the output from any
		 * grouping step needs to be what the window functions want;
		 * otherwise, it should be sort_input_target.
		 */
		grouping_target = sort_input_target;

		/*
		 * If we have grouping or aggregation to do, the topmost scan/join
		 * plan node must emit what the grouping step wants; otherwise, it
		 * should emit grouping_target.
		 */
		have_grouping = (parse->groupClause ||
						 parse->hasAggs || root->hasHavingQual);
		if (have_grouping)
		{
			scanjoin_target = make_group_input_target(root, final_target);
		}
		else
		{
			scanjoin_target = grouping_target;
		}

		/*
		 * If there are any SRFs in the targetlist, we must separate each of
		 * these PathTargets into SRF-computing and SRF-free targets.  Replace
		 * each of the named targets with a SRF-free version, and remember the
		 * list of additional projection steps we need to add afterwards.
		 */
		if (parse->hasTargetSRFs)
		{
			/* final_target doesn't recompute any SRFs in sort_input_target */
			split_pathtarget_at_srfs(root, final_target, sort_input_target,
									 &final_targets,
									 &final_targets_contain_srfs);
			final_target = linitial_node(PathTarget, final_targets);
			Assert(!linitial_int(final_targets_contain_srfs));
			/* likewise for sort_input_target vs. grouping_target */
			split_pathtarget_at_srfs(root, sort_input_target, grouping_target,
									 &sort_input_targets,
									 &sort_input_targets_contain_srfs);
			sort_input_target = linitial_node(PathTarget, sort_input_targets);
			Assert(!linitial_int(sort_input_targets_contain_srfs));
			/* likewise for grouping_target vs. scanjoin_target */
			split_pathtarget_at_srfs(root, grouping_target, scanjoin_target,
									 &grouping_targets,
									 &grouping_targets_contain_srfs);
			grouping_target = linitial_node(PathTarget, grouping_targets);
			Assert(!linitial_int(grouping_targets_contain_srfs));
			/* scanjoin_target will not have any SRFs precomputed for it */
			split_pathtarget_at_srfs(root, scanjoin_target, NULL,
									 &scanjoin_targets,
									 &scanjoin_targets_contain_srfs);
			scanjoin_target = linitial_node(PathTarget, scanjoin_targets);
			Assert(!linitial_int(scanjoin_targets_contain_srfs));
		}
		else
		{
			/* initialize lists; for most of these, dummy values are OK */
			final_targets = final_targets_contain_srfs = NIL;
			sort_input_targets = sort_input_targets_contain_srfs = NIL;
			grouping_targets = grouping_targets_contain_srfs = NIL;
			scanjoin_targets = list_make1(scanjoin_target);
			scanjoin_targets_contain_srfs = NIL;
		}

		/* Apply scan/join target. */
		scanjoin_target_same_exprs = list_length(scanjoin_targets) == 1
			&& equal(scanjoin_target->exprs, current_rel->reltarget->exprs);
		apply_scanjoin_target_to_paths(root, current_rel, scanjoin_targets,
									   scanjoin_targets_contain_srfs,
									   scanjoin_target_same_exprs);

		/*
		 * Save the various upper-rel PathTargets we just computed into
		 * root->upper_targets[].  The core code doesn't use this, but it
		 * provides a convenient place for extensions to get at the info.  For
		 * consistency, we save all the intermediate targets, even though some
		 * of the corresponding upperrels might not be needed for this query.
		 */
		root->upper_targets[UPPERREL_FINAL] = final_target;
		root->upper_targets[UPPERREL_ORDERED] = final_target;
		root->upper_targets[UPPERREL_DISTINCT] = sort_input_target;
		root->upper_targets[UPPERREL_GROUP_AGG] = grouping_target;

		/*
		 * If we have grouping and/or aggregation, consider ways to implement
		 * that.  We build a new upperrel representing the output of this
		 * phase.
		 */
		if (have_grouping)
		{
			current_rel = create_grouping_paths(root,
												current_rel,
												grouping_target);
			/* Fix things up if grouping_target contains SRFs */
			if (parse->hasTargetSRFs)
				adjust_paths_for_srfs(root, current_rel,
									  grouping_targets,
									  grouping_targets_contain_srfs);
		}

		/*
		 * If there is a DISTINCT clause, consider ways to implement that. We
		 * build a new upperrel representing the output of this phase.
		 */
		if (parse->distinctClause)
		{
			current_rel = create_distinct_paths(root,
												current_rel);
		}

	/*
	 * If ORDER BY was given, consider ways to implement that, and generate a
	 * new upperrel containing only paths that emit the correct ordering and
	 * project the correct final_target.
	 */
	if (parse->sortClause)
	{
		current_rel = create_ordered_paths(root,
										   current_rel,
										   final_target);
		/* Fix things up if final_target contains SRFs */
		if (parse->hasTargetSRFs)
			adjust_paths_for_srfs(root, current_rel,
								  final_targets,
								  final_targets_contain_srfs);
	}

	/*
	 * Now we are prepared to build the final-output upperrel.
	 */
	final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);

	/*
	 * Generate paths for the final_rel.  Insert all surviving paths, with
	 * Limit and/or ModifyTable steps added if needed.
	 */
	foreach(lc, current_rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		/*
		 * If this is an INSERT/UPDATE/DELETE, add the ModifyTable node.
		 */
		if (parse->commandType != CMD_SELECT)
		{
			Index		rootRelation;
			List	   *resultRelations = NIL;
			List	   *updateColnosLists = NIL;
			List	   *rowMarks;

			if (bms_membership(root->all_result_relids) == BMS_MULTIPLE)
			{
				/* Inherited UPDATE/DELETE */
				RelOptInfo *top_result_rel = find_base_rel(root,
														   parse->resultRelation);
				int			resultRelation = -1;

				/* Pass the root result rel forward to the executor. */
				rootRelation = parse->resultRelation;

				/* Add only leaf children to ModifyTable. */
				while ((resultRelation = bms_next_member(root->leaf_result_relids,
														 resultRelation)) >= 0)
				{
					RelOptInfo *this_result_rel = find_base_rel(root,
																resultRelation);

					/*
					 * Also exclude any leaf rels that have turned dummy since
					 * being added to the list, for example, by being excluded
					 * by constraint exclusion.
					 */
					if (IS_DUMMY_REL(this_result_rel))
						continue;

					/* Build per-target-rel lists needed by ModifyTable */
					resultRelations = lappend_int(resultRelations,
												  resultRelation);
					if (parse->commandType == CMD_UPDATE)
					{
						List	   *update_colnos = root->update_colnos;

						if (this_result_rel != top_result_rel)
							update_colnos =
								adjust_inherited_attnums_multilevel(root,
																	update_colnos,
																	this_result_rel->relid,
																	top_result_rel->relid);
						updateColnosLists = lappend(updateColnosLists,
													update_colnos);
					}
				}

				if (resultRelations == NIL)
				{
					/*
					 * We managed to exclude every child rel, so generate a
					 * dummy one-relation plan using info for the top target
					 * rel (even though that may not be a leaf target).
					 * Although it's clear that no data will be updated or
					 * deleted, we still need to have a ModifyTable node so
					 * that any statement triggers will be executed.  (This
					 * could be cleaner if we fixed nodeModifyTable.c to allow
					 * zero target relations, but that probably wouldn't be a
					 * net win.)
					 */
					resultRelations = list_make1_int(parse->resultRelation);
					if (parse->commandType == CMD_UPDATE)
						updateColnosLists = list_make1(root->update_colnos);
				}
			}
			else
			{
				/* Single-relation INSERT/UPDATE/DELETE. */
				rootRelation = 0;	/* there's no separate root rel */
				resultRelations = list_make1_int(parse->resultRelation);
				if (parse->commandType == CMD_UPDATE)
					updateColnosLists = list_make1(root->update_colnos);
			}

			/*
			 * ModifyTable fetches non-locked marked rows for EvalPlanQual.
			 */
			rowMarks = root->rowMarks;

			path = (Path *)
				create_modifytable_path(root, final_rel,
										path,
										parse->commandType,
										parse->canSetTag,
										parse->resultRelation,
										rootRelation,
										resultRelations,
										updateColnosLists,
										rowMarks,
										assign_special_exec_param(root));
		}

		/* And shove it into final_rel */
		add_path(final_rel, path);
	}

	/* Note: currently, we leave it to callers to do set_cheapest() */
}



/*
 * preprocess_rowmarks - set up PlanRowMarks if needed
 */
static void
preprocess_rowmarks(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	Bitmapset  *rels;
	List	   *prowmarks;
	ListCell   *l;
	int			i;

	/*
	 * We only need rowmarks for UPDATE or DELETE, so that EvalPlanQual can
	 * re-fetch the other relations referenced in the query.
	 */
	if (parse->commandType != CMD_UPDATE &&
		parse->commandType != CMD_DELETE)
		return;

	/*
	 * We need to have rowmarks for all base relations except the target. We
	 * make a bitmapset of all base rels and then remove the items we don't
	 * need.
	 */
	rels = get_relids_in_jointree((Node *) parse->jointree, false);
	if (parse->resultRelation)
		rels = bms_del_member(rels, parse->resultRelation);

	/*
	 * Add rowmarks for all non-target base relations.
	 */
	prowmarks = NIL;
	i = 0;
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);
		PlanRowMark *newrc;

		i++;
		if (!bms_is_member(i, rels))
			continue;

		newrc = makeNode(PlanRowMark);
		newrc->rti = newrc->prti = i;
		newrc->rowmarkId = ++(root->glob->lastRowMarkId);
		newrc->markType = select_rowmark_type(rte, LCS_NONE);
		newrc->allMarkTypes = (1 << newrc->markType);
		newrc->strength = LCS_NONE;
		newrc->waitPolicy = LockWaitBlock;	/* doesn't matter */
		newrc->isParent = false;

		prowmarks = lappend(prowmarks, newrc);
	}

	root->rowMarks = prowmarks;
}

/*
 * Select RowMarkType to use for a given table
 */
RowMarkType
select_rowmark_type(RangeTblEntry *rte, LockClauseStrength strength)
{
	if (rte->rtekind != RTE_RELATION)
	{
		/* If it's not a table at all, use ROW_MARK_COPY */
		return ROW_MARK_COPY;
	}
	else
	{
		/*
		 * We don't need a tuple lock, only the ability to re-fetch the row.
		 */
		Assert(strength == LCS_NONE);
		return ROW_MARK_REFERENCE;
	}
}



/*
 * remove_useless_groupby_columns
 *		Remove any columns in the GROUP BY clause that are redundant due to
 *		being functionally dependent on other GROUP BY columns.
 *
 * Since some other DBMSes do not allow references to ungrouped columns, it's
 * not unusual to find all columns listed in GROUP BY even though listing the
 * primary-key columns would be sufficient.  Deleting such excess columns
 * avoids redundant sorting work, so it's worth doing.
 *
 * Relcache invalidations will ensure that cached plans become invalidated
 * when the underlying index of the pkey constraint is dropped.
 *
 * Currently, we only make use of pkey constraints for this, however, we may
 * wish to take this further in the future and also use unique constraints
 * which have NOT NULL columns.  In that case, plan invalidation will still
 * work since relations will receive a relcache invalidation when a NOT NULL
 * constraint is dropped.
 */
static void
remove_useless_groupby_columns(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	Bitmapset **groupbyattnos;
	Bitmapset **surplusvars;
	ListCell   *lc;
	int			relid;

	/* No chance to do anything if there are less than two GROUP BY items */
	if (list_length(parse->groupClause) < 2)
		return;



	/*
	 * Scan the GROUP BY clause to find GROUP BY items that are simple Vars.
	 * Fill groupbyattnos[k] with a bitmapset of the column attnos of RTE k
	 * that are GROUP BY items.
	 */
	groupbyattnos = (Bitmapset **) palloc0(sizeof(Bitmapset *) *
										   (list_length(parse->rtable) + 1));
	foreach(lc, parse->groupClause)
	{
		SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, parse->targetList);
		Var		   *var = (Var *) tle->expr;

		/*
		 * Ignore non-Vars and Vars from other query levels.
		 *
		 * XXX in principle, stable expressions containing Vars could also be
		 * removed, if all the Vars are functionally dependent on other GROUP
		 * BY items.  But it's not clear that such cases occur often enough to
		 * be worth troubling over.
		 */
		if (!IsA(var, Var) ||
			var->varlevelsup > 0)
			continue;

		/* OK, remember we have this Var */
		relid = var->varno;
		Assert(relid <= list_length(parse->rtable));
		groupbyattnos[relid] = bms_add_member(groupbyattnos[relid],
											  var->varattno - FirstLowInvalidHeapAttributeNumber);
	}

	/*
	 * Consider each relation and see if it is possible to remove some of its
	 * Vars from GROUP BY.  For simplicity and speed, we do the actual removal
	 * in a separate pass.  Here, we just fill surplusvars[k] with a bitmapset
	 * of the column attnos of RTE k that are removable GROUP BY items.
	 */
	surplusvars = NULL;			/* don't allocate array unless required */
	relid = 0;
	foreach(lc, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
		Bitmapset  *relattnos;
		Bitmapset  *pkattnos;
		Oid			constraintOid;

		relid++;

		/* Only plain relations could have primary-key constraints */
		if (rte->rtekind != RTE_RELATION)
			continue;


		/* Nothing to do unless this rel has multiple Vars in GROUP BY */
		relattnos = groupbyattnos[relid];
		if (bms_membership(relattnos) != BMS_MULTIPLE)
			continue;

		/*
		 * Can't remove any columns for this rel if there is no suitable
		 * (i.e., nondeferrable) primary key constraint.
		 */
		pkattnos = get_primary_key_attnos(rte->relid, &constraintOid);
		if (pkattnos == NULL)
			continue;

		/*
		 * If the primary key is a proper subset of relattnos then we have
		 * some items in the GROUP BY that can be removed.
		 */
		if (bms_subset_compare(pkattnos, relattnos) == BMS_SUBSET1)
		{
			/*
			 * To easily remember whether we've found anything to do, we don't
			 * allocate the surplusvars[] array until we find something.
			 */
			if (surplusvars == NULL)
				surplusvars = (Bitmapset **) palloc0(sizeof(Bitmapset *) *
													 (list_length(parse->rtable) + 1));

			/* Remember the attnos of the removable columns */
			surplusvars[relid] = bms_difference(relattnos, pkattnos);
		}
	}

	/*
	 * If we found any surplus Vars, build a new GROUP BY clause without them.
	 * (Note: this may leave some TLEs with unreferenced ressortgroupref
	 * markings, but that's harmless.)
	 */
	if (surplusvars != NULL)
	{
		List	   *new_groupby = NIL;

		foreach(lc, parse->groupClause)
		{
			SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);
			TargetEntry *tle = get_sortgroupclause_tle(sgc, parse->targetList);
			Var		   *var = (Var *) tle->expr;

			/*
			 * New list must include non-Vars, outer Vars, and anything not
			 * marked as surplus.
			 */
			if (!IsA(var, Var) ||
				var->varlevelsup > 0 ||
				!bms_is_member(var->varattno - FirstLowInvalidHeapAttributeNumber,
							   surplusvars[var->varno]))
				new_groupby = lappend(new_groupby, sgc);
		}

		parse->groupClause = new_groupby;
	}
}

/*
 * preprocess_groupclause - do preparatory work on GROUP BY clause
 *
 * The idea here is to adjust the ordering of the GROUP BY elements
 * (which in itself is semantically insignificant) to match ORDER BY,
 * thereby allowing a single sort operation to both implement the ORDER BY
 * requirement and set up for a Unique step that implements GROUP BY.
 *
 * In principle it might be interesting to consider other orderings of the
 * GROUP BY elements, which could match the sort ordering of other
 * possible plans (eg an indexscan) and thereby reduce cost.  We don't
 * bother with that, though.  Hashed grouping will frequently win anyway.
 *
 * Note: we need no comparable processing of the distinctClause because
 * the parser already enforced that that matches ORDER BY.
 *
 * For grouping sets, the order of items is instead forced to agree with that
 * of the grouping set (and items not in the grouping set are skipped). The
 * work of sorting the order of grouping set elements to match the ORDER BY if
 * possible is done elsewhere.
 */
static List *
preprocess_groupclause(PlannerInfo *root, List *force)
{
	Query	   *parse = root->parse;
	List	   *new_groupclause = NIL;
	bool		partial_match;
	ListCell   *sl;
	ListCell   *gl;

	/* For grouping sets, we need to force the ordering */
	if (force)
	{
		foreach(sl, force)
		{
			Index		ref = lfirst_int(sl);
			SortGroupClause *cl = get_sortgroupref_clause(ref, parse->groupClause);

			new_groupclause = lappend(new_groupclause, cl);
		}

		return new_groupclause;
	}

	/* If no ORDER BY, nothing useful to do here */
	if (parse->sortClause == NIL)
		return parse->groupClause;

	/*
	 * Scan the ORDER BY clause and construct a list of matching GROUP BY
	 * items, but only as far as we can make a matching prefix.
	 *
	 * This code assumes that the sortClause contains no duplicate items.
	 */
	foreach(sl, parse->sortClause)
	{
		SortGroupClause *sc = lfirst_node(SortGroupClause, sl);

		foreach(gl, parse->groupClause)
		{
			SortGroupClause *gc = lfirst_node(SortGroupClause, gl);

			if (equal(gc, sc))
			{
				new_groupclause = lappend(new_groupclause, gc);
				break;
			}
		}
		if (gl == NULL)
			break;				/* no match, so stop scanning */
	}

	/* Did we match all of the ORDER BY list, or just some of it? */
	partial_match = (sl != NULL);

	/* If no match at all, no point in reordering GROUP BY */
	if (new_groupclause == NIL)
		return parse->groupClause;

	/*
	 * Add any remaining GROUP BY items to the new list, but only if we were
	 * able to make a complete match.  In other words, we only rearrange the
	 * GROUP BY list if the result is that one list is a prefix of the other
	 * --- otherwise there's no possibility of a common sort.  Also, give up
	 * if there are any non-sortable GROUP BY items, since then there's no
	 * hope anyway.
	 */
	foreach(gl, parse->groupClause)
	{
		SortGroupClause *gc = lfirst_node(SortGroupClause, gl);

		if (list_member_ptr(new_groupclause, gc))
			continue;			/* it matched an ORDER BY item */
		if (partial_match)
			return parse->groupClause;	/* give up, no common sort possible */
		if (!OidIsValid(gc->sortop))
			return parse->groupClause;	/* give up, GROUP BY can't be sorted */
		new_groupclause = lappend(new_groupclause, gc);
	}

	/* Success --- install the rearranged GROUP BY list */
	Assert(list_length(parse->groupClause) == list_length(new_groupclause));
	return new_groupclause;
}

/*
 * Compute query_pathkeys and other pathkeys during plan generation
 */
static void
standard_qp_callback(PlannerInfo *root, void *extra)
{
	Query	   *parse = root->parse;
	standard_qp_extra *qp_extra = (standard_qp_extra *) extra;
	List	   *tlist = root->processed_tlist;

	/*
	 * Calculate pathkeys that represent grouping/ordering requirements.  The
	 * sortClause is certainly sort-able, but GROUP BY and DISTINCT might not
	 * be, in which case we just leave their pathkeys empty.
	 */
	if (qp_extra->groupClause &&
		grouping_is_sortable(qp_extra->groupClause))
		root->group_pathkeys =
			make_pathkeys_for_sortclauses(root,
										  qp_extra->groupClause,
										  tlist);
	else
		root->group_pathkeys = NIL;


	if (parse->distinctClause &&
		grouping_is_sortable(parse->distinctClause))
		root->distinct_pathkeys =
			make_pathkeys_for_sortclauses(root,
										  parse->distinctClause,
										  tlist);
	else
		root->distinct_pathkeys = NIL;

	root->sort_pathkeys =
		make_pathkeys_for_sortclauses(root,
									  parse->sortClause,
									  tlist);

	/*
	 * Figure out whether we want a sorted result from query_planner.
	 *
	 * If we have a sortable GROUP BY clause, then we want a result sorted
	 * properly for grouping.  Otherwise, if we have window functions to
	 * evaluate, we try to sort for the first window.  Otherwise, if there's a
	 * sortable DISTINCT clause that's more rigorous than the ORDER BY clause,
	 * we try to produce output that's sufficiently well sorted for the
	 * DISTINCT.  Otherwise, if there is an ORDER BY clause, we want to sort
	 * by the ORDER BY clause.
	 *
	 * Note: if we have both ORDER BY and GROUP BY, and ORDER BY is a superset
	 * of GROUP BY, it would be tempting to request sort by ORDER BY --- but
	 * that might just leave us failing to exploit an available sort order at
	 * all.  Needs more thought.  The choice for DISTINCT versus ORDER BY is
	 * much easier, since we know that the parser ensured that one is a
	 * superset of the other.
	 */
	if (root->group_pathkeys)
		root->query_pathkeys = root->group_pathkeys;
	else if (list_length(root->distinct_pathkeys) >
			 list_length(root->sort_pathkeys))
		root->query_pathkeys = root->distinct_pathkeys;
	else if (root->sort_pathkeys)
		root->query_pathkeys = root->sort_pathkeys;
	else
		root->query_pathkeys = NIL;
}

/*
 * Estimate number of groups produced by grouping clauses (1 if not grouping)
 *
 * path_rows: number of output rows from scan/join step
 */
static double
get_number_of_groups(PlannerInfo *root,
					 double path_rows,
					 List *target_list)
{
	Query	   *parse = root->parse;
	double		dNumGroups;

	if (parse->groupClause)
	{
		List	   *groupExprs;

		{
			/* Plain GROUP BY */
			groupExprs = get_sortgrouplist_exprs(parse->groupClause,
												 target_list);

			dNumGroups = estimate_num_groups(root, groupExprs, path_rows,
											 NULL, NULL);
		}
	}
	else if (parse->hasAggs || root->hasHavingQual)
	{
		/* Plain aggregation, one result row */
		dNumGroups = 1;
	}
	else
	{
		/* Not grouping */
		dNumGroups = 1;
	}

	return dNumGroups;
}

/*
 * create_grouping_paths
 *
 * Build a new upperrel containing Paths for grouping and/or aggregation.
 * Along the way, we also build an upperrel for Paths which are partially
 * grouped and/or aggregated.  A partially grouped and/or aggregated path
 * needs a FinalizeAggregate node to complete the aggregation.  Currently,
 * the only partially grouped paths we build are also partial paths; that
 * is, they need a FinalizeAggregate.
 *
 * input_rel: contains the source-data Paths
 * target: the pathtarget for the result Paths to compute
 *
 * Note: all Paths in input_rel are expected to return the target computed
 * by make_group_input_target.
 */
static RelOptInfo *
create_grouping_paths(PlannerInfo *root,
					  RelOptInfo *input_rel,
					  PathTarget *target)
{
	Query	   *parse = root->parse;
	RelOptInfo *grouped_rel;
	AggClauseCosts agg_costs;

	MemSet(&agg_costs, 0, sizeof(AggClauseCosts));
	get_agg_clause_costs(root, &agg_costs);

	/*
	 * Create grouping relation to hold fully aggregated grouping and/or
	 * aggregation paths.
	 */
	grouped_rel = make_grouping_rel(root, input_rel, target);

	/*
	 * Create either paths for a degenerate grouping or paths for ordinary
	 * grouping, as appropriate.
	 */
	if (is_degenerate_grouping(root))
		create_degenerate_grouping_paths(root, input_rel, grouped_rel);
	else
	{
		int			flags = 0;
		GroupPathExtraData extra;

		/*
		 * Determine whether it's possible to perform sort-based
		 * implementations of grouping.  (Note that if groupClause is empty,
		 * grouping_is_sortable() is trivially true, and all the
		 * pathkeys_contained_in() tests will succeed too, so that we'll
		 * consider every surviving input path.)
		 *
		 * If we have grouping sets, we might be able to sort some but not all
		 * of them; in this case, we need can_sort to be true as long as we
		 * must consider any sorted-input plan.
		 */
		if (grouping_is_sortable(parse->groupClause))
			flags |= GROUPING_CAN_USE_SORT;

		/*
		 * Determine whether we should consider hash-based implementations of
		 * grouping.
		 *
		 * Hashed aggregation only applies if we're grouping. If we have
		 * grouping sets, some groups might be hashable but others not; in
		 * this case we set can_hash true as long as there is nothing globally
		 * preventing us from hashing (and we should therefore consider plans
		 * with hashes).
		 *
		 * Executor doesn't support hashed aggregation with DISTINCT or ORDER
		 * BY aggregates.  (Doing so would imply storing *all* the input
		 * values in the hash table, and/or running many sorts in parallel,
		 * either of which seems like a certain loser.)  We similarly don't
		 * support ordered-set aggregates in hashed aggregation, but that case
		 * is also included in the numOrderedAggs count.
		 *
		 * Note: grouping_is_hashable() is much more expensive to check than
		 * the other gating conditions, so we want to do it last.
		 */
		if ((parse->groupClause != NIL &&
			 root->numOrderedAggs == 0 &&
			 grouping_is_hashable(parse->groupClause)))
			flags |= GROUPING_CAN_USE_HASH;

		extra.flags = flags;
		extra.havingQual = parse->havingQual;
		extra.targetList = parse->targetList;
		extra.partial_costs_set = false;

		create_ordinary_grouping_paths(root, input_rel, grouped_rel,
									   &agg_costs, &extra);
	}

	set_cheapest(grouped_rel);
	return grouped_rel;
}

/*
 * make_grouping_rel
 *
 * Create a new grouping rel and set basic properties.
 *
 * input_rel represents the underlying scan/join relation.
 * target is the output expected from the grouping relation.
 */
static RelOptInfo *
make_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
				  PathTarget *target)
{
	RelOptInfo *grouped_rel;

	if (IS_OTHER_REL(input_rel))
	{
		grouped_rel = fetch_upper_rel(root, UPPERREL_GROUP_AGG,
									  input_rel->relids);
		grouped_rel->reloptkind = RELOPT_OTHER_UPPER_REL;
	}
	else
	{
		/*
		 * By tradition, the relids set for the main grouping relation is
		 * NULL.  (This could be changed, but might require adjustments
		 * elsewhere.)
		 */
		grouped_rel = fetch_upper_rel(root, UPPERREL_GROUP_AGG, NULL);
	}

	/* Set target. */
	grouped_rel->reltarget = target;

	return grouped_rel;
}

/*
 * is_degenerate_grouping
 *
 * A degenerate grouping is one in which the query has a HAVING qual and/or
 * grouping sets, but no aggregates and no GROUP BY (which implies that the
 * grouping sets are all empty).
 */
static bool
is_degenerate_grouping(PlannerInfo *root)
{
	Query	   *parse = root->parse;

	return root->hasHavingQual &&
		!parse->hasAggs && parse->groupClause == NIL;
}

/*
 * create_degenerate_grouping_paths
 *
 * When the grouping is degenerate (see is_degenerate_grouping), we are
 * supposed to emit either zero or one row, depending on whether HAVING
 * succeeds.  Furthermore, there cannot be any variables in either HAVING or
 * the targetlist, so we actually do not need the FROM table at all! We can
 * just throw away the plan-so-far and generate a Result node.  This is a
 * sufficiently unusual corner case that it's not worth contorting the
 * structure of this module to avoid having to generate the earlier paths in
 * the first place.
 */
static void
create_degenerate_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
								 RelOptInfo *grouped_rel)
{
	Query	   *parse = root->parse;
	Path	   *path;

	/* No grouping sets, so exactly one output row (if HAVING passes) */
	path = (Path *)
		create_group_result_path(root, grouped_rel,
								 grouped_rel->reltarget,
								 (List *) parse->havingQual);

	add_path(grouped_rel, path);
}

/*
 * create_ordinary_grouping_paths
 *
 * Create grouping paths for the ordinary (that is, non-degenerate) case.
 *
 * We need to consider sorted and hashed aggregation in the same function,
 * because otherwise (1) it would be harder to throw an appropriate error
 * message if neither way works, and (2) we should not allow hashtable size
 * considerations to dissuade us from using hashing if sorting is not possible.
 */
static void
create_ordinary_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
							   RelOptInfo *grouped_rel,
							   const AggClauseCosts *agg_costs,
							   GroupPathExtraData *extra)
{
	Path	   *cheapest_path = input_rel->cheapest_total_path;
	double		dNumGroups;

	/*
	 * Estimate number of groups.
	 */
	dNumGroups = get_number_of_groups(root,
									  cheapest_path->rows,
									  extra->targetList);

	/* Build final grouping paths */
	add_paths_to_grouping_rel(root, input_rel, grouped_rel,
							  agg_costs,
							  dNumGroups, extra);

	/* Give a helpful error if we failed to find any implementation */
	if (grouped_rel->pathlist == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement GROUP BY"),
				 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_GROUP_AGG,
									input_rel, grouped_rel,
									extra);
}


/*
 * make_sort_input_target
 *	  Generate appropriate PathTarget for initial input to Sort step.
 *
 * If the query has ORDER BY, this function chooses the target to be computed
 * by the node just below the Sort (and DISTINCT, if any, since Unique can't
 * project) steps.  This might or might not be identical to the query's final
 * output target.
 *
 * The main argument for keeping the sort-input tlist the same as the final
 * is that we avoid a separate projection node (which will be needed if
 * they're different, because Sort can't project).  However, there are also
 * advantages to postponing tlist evaluation till after the Sort: it ensures
 * a consistent order of evaluation for any volatile functions in the tlist,
 * and if there's also a LIMIT, we can stop the query without ever computing
 * tlist functions for later rows, which is beneficial for both volatile and
 * expensive functions.
 *
 * Our current policy is to postpone volatile expressions till after the sort
 * unconditionally (assuming that that's possible, ie they are in plain tlist
 * columns and not ORDER BY/GROUP BY/DISTINCT columns).  We also prefer to
 * postpone set-returning expressions, because running them beforehand would
 * bloat the sort dataset, and because it might cause unexpected output order
 * if the sort isn't stable.  However there's a constraint on that: all SRFs
 * in the tlist should be evaluated at the same plan step, so that they can
 * run in sync in nodeProjectSet.  So if any SRFs are in sort columns, we
 * mustn't postpone any SRFs.  (Note that in principle that policy should
 * probably get applied to the group/window input targetlists too, but we
 * have not done that historically.)  Lastly, expensive expressions are
 * postponed if there is a LIMIT, or if root->tuple_fraction shows that
 * partial evaluation of the query is possible (if neither is true, we expect
 * to have to evaluate the expressions for every row anyway), or if there are
 * any volatile or set-returning expressions (since once we've put in a
 * projection at all, it won't cost any more to postpone more stuff).
 *
 * Another issue that could potentially be considered here is that
 * evaluating tlist expressions could result in data that's either wider
 * or narrower than the input Vars, thus changing the volume of data that
 * has to go through the Sort.  However, we usually have only a very bad
 * idea of the output width of any expression more complex than a Var,
 * so for now it seems too risky to try to optimize on that basis.
 *
 * Note that if we do produce a modified sort-input target, and then the
 * query ends up not using an explicit Sort, no particular harm is done:
 * we'll initially use the modified target for the preceding path nodes,
 * but then change them to the final target with apply_projection_to_path.
 * Moreover, in such a case the guarantees about evaluation order of
 * volatile functions still hold, since the rows are sorted already.
 *
 * This function has some things in common with make_group_input_target and
 * make_window_input_target, though the detailed rules for what to do are
 * different.  We never flatten/postpone any grouping or ordering columns;
 * those are needed before the sort.  If we do flatten a particular
 * expression, we leave Aggref and WindowFunc nodes alone, since those were
 * computed earlier.
 *
 * 'final_target' is the query's final target list (in PathTarget form)
 * 'have_postponed_srfs' is an output argument, see below
 *
 * The result is the PathTarget to be computed by the plan node immediately
 * below the Sort step (and the Distinct step, if any).  This will be
 * exactly final_target if we decide a projection step wouldn't be helpful.
 *
 * In addition, *have_postponed_srfs is set to true if we choose to postpone
 * any set-returning functions to after the Sort.
 */
static PathTarget *
make_sort_input_target(PlannerInfo *root,
					   PathTarget *final_target,
					   bool *have_postponed_srfs)
{
	Query	   *parse = root->parse;
	PathTarget *input_target;
	int			ncols;
	bool	   *col_is_srf;
	bool	   *postpone_col;
	bool		have_srf;
	bool		have_volatile;
	bool		have_expensive;
	bool		have_srf_sortcols;
	bool		postpone_srfs;
	List	   *postponable_cols;
	List	   *postponable_vars;
	int			i;
	ListCell   *lc;

	/* Shouldn't get here unless query has ORDER BY */
	Assert(parse->sortClause);

	*have_postponed_srfs = false;	/* default result */

	/* Inspect tlist and collect per-column information */
	ncols = list_length(final_target->exprs);
	col_is_srf = (bool *) palloc0(ncols * sizeof(bool));
	postpone_col = (bool *) palloc0(ncols * sizeof(bool));
	have_srf = have_volatile = have_expensive = have_srf_sortcols = false;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		/*
		 * If the column has a sortgroupref, assume it has to be evaluated
		 * before sorting.  Generally such columns would be ORDER BY, GROUP
		 * BY, etc targets.  One exception is columns that were removed from
		 * GROUP BY by remove_useless_groupby_columns() ... but those would
		 * only be Vars anyway.  There don't seem to be any cases where it
		 * would be worth the trouble to double-check.
		 */
		if (get_pathtarget_sortgroupref(final_target, i) == 0)
		{
			/*
			 * Check for SRF or volatile functions.  Check the SRF case first
			 * because we must know whether we have any postponed SRFs.
			 */
			if (parse->hasTargetSRFs &&
				expression_returns_set((Node *) expr))
			{
				/* We'll decide below whether these are postponable */
				col_is_srf[i] = true;
				have_srf = true;
			}
			else if (contain_volatile_functions((Node *) expr))
			{
				/* Unconditionally postpone */
				postpone_col[i] = true;
				have_volatile = true;
			}
			else
			{
				/*
				 * Else check the cost.  XXX it's annoying to have to do this
				 * when set_pathtarget_cost_width() just did it.  Refactor to
				 * allow sharing the work?
				 */
				QualCost	cost;

				cost_qual_eval_node(&cost, (Node *) expr, root);

				/*
				 * We arbitrarily define "expensive" as "more than 10X
				 * cpu_operator_cost".  Note this will take in any PL function
				 * with default cost.
				 */
				if (cost.per_tuple > 10 * cpu_operator_cost)
				{
					postpone_col[i] = true;
					have_expensive = true;
				}
			}
		}
		else
		{
			/* For sortgroupref cols, just check if any contain SRFs */
			if (!have_srf_sortcols &&
				parse->hasTargetSRFs &&
				expression_returns_set((Node *) expr))
				have_srf_sortcols = true;
		}

		i++;
	}

	/*
	 * We can postpone SRFs if we have some but none are in sortgroupref cols.
	 */
	postpone_srfs = (have_srf && !have_srf_sortcols);

	/*
	 * If we don't need a post-sort projection, just return final_target.
	 */
	if (!(postpone_srfs || have_volatile ||
		  (have_expensive &&
		   (root->tuple_fraction > 0))))
		return final_target;

	/*
	 * Report whether the post-sort projection will contain set-returning
	 * functions.  This is important because it affects whether the Sort can
	 * rely on the query's LIMIT (if any) to bound the number of rows it needs
	 * to return.
	 */
	*have_postponed_srfs = postpone_srfs;

	/*
	 * Construct the sort-input target, taking all non-postponable columns and
	 * then adding Vars, PlaceHolderVars, Aggrefs, and WindowFuncs found in
	 * the postponable ones.
	 */
	input_target = create_empty_pathtarget();
	postponable_cols = NIL;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		if (postpone_col[i] || (postpone_srfs && col_is_srf[i]))
			postponable_cols = lappend(postponable_cols, expr);
		else
			add_column_to_pathtarget(input_target, expr,
									 get_pathtarget_sortgroupref(final_target, i));

		i++;
	}

	/*
	 * Pull out all the Vars, Aggrefs, and WindowFuncs mentioned in
	 * postponable columns, and add them to the sort-input target if not
	 * already present.  (Some might be there already.)  We mustn't
	 * deconstruct Aggrefs or WindowFuncs here, since the projection node
	 * would be unable to recompute them.
	 */
	postponable_vars = pull_var_clause((Node *) postponable_cols,
									   PVC_INCLUDE_AGGREGATES |
									   PVC_INCLUDE_PLACEHOLDERS);
	add_new_columns_to_pathtarget(input_target, postponable_vars);

	/* clean up cruft */
	list_free(postponable_vars);
	list_free(postponable_cols);

	/* XXX this represents even more redundant cost calculation ... */
	return set_pathtarget_cost_width(root, input_target);
}

/*
 * get_cheapest_fractional_path
 *	  Find the cheapest path for retrieving a specified fraction of all
 *	  the tuples expected to be returned by the given relation.
 *
 * We interpret tuple_fraction the same way as grouping_planner.
 *
 * We assume set_cheapest() has been run on the given rel.
 */
Path *
get_cheapest_fractional_path(RelOptInfo *rel, double tuple_fraction)
{
	Path	   *best_path = rel->cheapest_total_path;
	ListCell   *l;

	/* If all tuples will be retrieved, just return the cheapest-total path */
	if (tuple_fraction <= 0.0)
		return best_path;

	/* Convert absolute # of tuples to a fraction; no need to clamp to 0..1 */
	if (tuple_fraction >= 1.0 && best_path->rows > 0)
		tuple_fraction /= best_path->rows;

	foreach(l, rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(l);

		if (path == rel->cheapest_total_path ||
			compare_fractional_path_costs(best_path, path, tuple_fraction) <= 0)
			continue;

		best_path = path;
	}

	return best_path;
}

/*
 * adjust_paths_for_srfs
 *		Fix up the Paths of the given upperrel to handle tSRFs properly.
 *
 * The executor can only handle set-returning functions that appear at the
 * top level of the targetlist of a ProjectSet plan node.  If we have any SRFs
 * that are not at top level, we need to split up the evaluation into multiple
 * plan levels in which each level satisfies this constraint.  This function
 * modifies each Path of an upperrel that (might) compute any SRFs in its
 * output tlist to insert appropriate projection steps.
 *
 * The given targets and targets_contain_srfs lists are from
 * split_pathtarget_at_srfs().  We assume the existing Paths emit the first
 * target in targets.
 */
static void
adjust_paths_for_srfs(PlannerInfo *root, RelOptInfo *rel,
					  List *targets, List *targets_contain_srfs)
{
	ListCell   *lc;

	Assert(list_length(targets) == list_length(targets_contain_srfs));
	Assert(!linitial_int(targets_contain_srfs));

	/* If no SRFs appear at this plan level, nothing to do */
	if (list_length(targets) == 1)
		return;

	/*
	 * Stack SRF-evaluation nodes atop each path for the rel.
	 *
	 * In principle we should re-run set_cheapest() here to identify the
	 * cheapest path, but it seems unlikely that adding the same tlist eval
	 * costs to all the paths would change that, so we don't bother. Instead,
	 * just assume that the cheapest-startup and cheapest-total paths remain
	 * so.  (There should be no parameterized paths anymore, so we needn't
	 * worry about updating cheapest_parameterized_paths.)
	 */
	foreach(lc, rel->pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);
		Path	   *newpath = subpath;
		ListCell   *lc1,
				   *lc2;

		Assert(subpath->param_info == NULL);
		forboth(lc1, targets, lc2, targets_contain_srfs)
		{
			PathTarget *thistarget = lfirst_node(PathTarget, lc1);
			bool		contains_srfs = (bool) lfirst_int(lc2);

			/* If this level doesn't contain SRFs, do regular projection */
			if (contains_srfs)
				newpath = (Path *) create_set_projection_path(root,
															  rel,
															  newpath,
															  thistarget);
			else
				newpath = (Path *) apply_projection_to_path(root,
															rel,
															newpath,
															thistarget);
		}
		lfirst(lc) = newpath;
		if (subpath == rel->cheapest_startup_path)
			rel->cheapest_startup_path = newpath;
		if (subpath == rel->cheapest_total_path)
			rel->cheapest_total_path = newpath;
	}

}

/*
 * expression_planner
 *		Perform planner's transformations on a standalone expression.
 *
 * Various utility commands need to evaluate expressions that are not part
 * of a plannable query.  They can do so using the executor's regular
 * expression-execution machinery, but first the expression has to be fed
 * through here to transform it from parser output to something executable.
 *
 * Currently, we disallow sublinks in standalone expressions, so there's no
 * real "planning" involved here.  (That might not always be true though.)
 * What we must do is run eval_const_expressions to ensure that any function
 * calls are converted to positional notation and function default arguments
 * get inserted.  The fact that constant subexpressions get simplified is a
 * side-effect that is useful when the expression will get evaluated more than
 * once.  Also, we must fix operator function IDs.
 *
 * This does not return any information about dependencies of the expression.
 * Hence callers should use the results only for the duration of the current
 * query.  Callers that would like to cache the results for longer should use
 * expression_planner_with_deps.
 *
 * Note: this must not make any damaging changes to the passed-in expression
 * tree.  (It would actually be okay to apply fix_opfuncids to it, but since
 * we first do an expression_tree_mutator-based walk, what is returned will
 * be a new node tree.)  The result is constructed in the current memory
 * context; beware that this can leak a lot of additional stuff there, too.
 */
Expr *
expression_planner(Expr *expr)
{
	Node	   *result;

	/*
	 * Convert named-argument function calls, insert default arguments and
	 * simplify constant subexprs
	 */
	result = eval_const_expressions(NULL, (Node *) expr);

	/* Fill in opfuncid values if missing */
	fix_opfuncids(result);

	return (Expr *) result;
}

/*
 * expression_planner_with_deps
 *		Perform planner's transformations on a standalone expression,
 *		returning expression dependency information along with the result.
 *
 * This is identical to expression_planner() except that it also returns
 * information about possible dependencies of the expression, ie identities of
 * objects whose definitions affect the result.  As in a PlannedStmt, these
 * are expressed as a list of relation Oids and a list of PlanInvalItems.
 */
Expr *
expression_planner_with_deps(Expr *expr,
							 List **relationOids,
							 List **invalItems)
{
	Node	   *result;
	PlannerGlobal glob;
	PlannerInfo root;

	/* Make up dummy planner state so we can use setrefs machinery */
	MemSet(&glob, 0, sizeof(glob));
	glob.type = T_PlannerGlobal;
	glob.relationOids = NIL;
	glob.invalItems = NIL;

	MemSet(&root, 0, sizeof(root));
	root.type = T_PlannerInfo;
	root.glob = &glob;

	/*
	 * Convert named-argument function calls, insert default arguments and
	 * simplify constant subexprs.  Collect identities of inlined functions
	 * and elided domains, too.
	 */
	result = eval_const_expressions(&root, (Node *) expr);

	/* Fill in opfuncid values if missing */
	fix_opfuncids(result);

	/*
	 * Now walk the finished expression to find anything else we ought to
	 * record as an expression dependency.
	 */
	(void) extract_query_dependencies_walker(result, &root);

	*relationOids = glob.relationOids;
	*invalItems = glob.invalItems;

	return (Expr *) result;
}


/*
 * add_paths_to_grouping_rel
 *
 * Add non-partial paths to grouping relation.
 */
static void
add_paths_to_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
						  RelOptInfo *grouped_rel,
						  const AggClauseCosts *agg_costs,
						  double dNumGroups,
						  GroupPathExtraData *extra)
{
	Query	   *parse = root->parse;
	Path	   *cheapest_path = input_rel->cheapest_total_path;
	ListCell   *lc;
	bool		can_hash = (extra->flags & GROUPING_CAN_USE_HASH) != 0;
	bool		can_sort = (extra->flags & GROUPING_CAN_USE_SORT) != 0;
	List	   *havingQual = (List *) extra->havingQual;

	if (can_sort)
	{
		/*
		 * Use any available suitably-sorted path as input, and also consider
		 * sorting the cheapest-total path.
		 */
		foreach(lc, input_rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);
			Path	   *path_original = path;
			bool		is_sorted;
			int			presorted_keys;

			is_sorted = pathkeys_count_contained_in(root->group_pathkeys,
													path->pathkeys,
													&presorted_keys);

			if (path == cheapest_path || is_sorted)
			{
				/* Sort the cheapest-total path if it isn't already sorted */
				if (!is_sorted)
					path = (Path *) create_sort_path(root,
													 grouped_rel,
													 path,
													 root->group_pathkeys);

				/* Now decide what to stick atop it */
				if (parse->hasAggs)
				{
					/*
					 * We have aggregation, possibly with plain GROUP BY. Make
					 * an AggPath.
					 */
					add_path(grouped_rel, (Path *)
							 create_agg_path(root,
											 grouped_rel,
											 path,
											 grouped_rel->reltarget,
											 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
											 parse->groupClause,
											 havingQual,
											 agg_costs,
											 dNumGroups));
				}
				else if (parse->groupClause)
				{
					/*
					 * We have GROUP BY without aggregation or grouping sets.
					 * Make a GroupPath.
					 */
					add_path(grouped_rel, (Path *)
							 create_group_path(root,
											   grouped_rel,
											   path,
											   parse->groupClause,
											   havingQual,
											   dNumGroups));
				}
				else
				{
					/* Other cases should have been handled above */
					Assert(false);
				}
			}

			/*
			 * Now we may consider incremental sort on this path, but only
			 * when the path is not already sorted and when incremental sort
			 * is enabled.
			 */
			if (is_sorted || !enable_incremental_sort)
				continue;

			/* Restore the input path (we might have added Sort on top). */
			path = path_original;

			/* no shared prefix, no point in building incremental sort */
			if (presorted_keys == 0)
				continue;

			/*
			 * We should have already excluded pathkeys of length 1 because
			 * then presorted_keys > 0 would imply is_sorted was true.
			 */
			Assert(list_length(root->group_pathkeys) != 1);

			path = (Path *) create_incremental_sort_path(root,
													 grouped_rel,
													 path,
													 root->group_pathkeys,
													 presorted_keys);

			/* Now decide what to stick atop it */
			if (parse->hasAggs)
			{
				/*
				 * We have aggregation, possibly with plain GROUP BY. Make an
				 * AggPath.
				 */
				add_path(grouped_rel, (Path *)
						 create_agg_path(root,
										 grouped_rel,
										 path,
										 grouped_rel->reltarget,
										 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
										 parse->groupClause,
										 havingQual,
										 agg_costs,
										 dNumGroups));
			}
			else if (parse->groupClause)
			{
				/*
				 * We have GROUP BY without aggregation or grouping sets. Make
				 * a GroupPath.
				 */
				add_path(grouped_rel, (Path *)
						 create_group_path(root,
										   grouped_rel,
										   path,
										   parse->groupClause,
										   havingQual,
										   dNumGroups));
			}
			else
			{
				/* Other cases should have been handled above */
				Assert(false);
			}
		}


	}
	if (can_hash)
	{
		{
			/*
			 * Generate a HashAgg Path.  We just need an Agg over the
			 * cheapest-total input path, since input order won't matter.
			 */
			add_path(grouped_rel, (Path *)
					 create_agg_path(root, grouped_rel,
									 cheapest_path,
									 grouped_rel->reltarget,
									 AGG_HASHED,
									 parse->groupClause,
									 havingQual,
									 agg_costs,
									 dNumGroups));
		}

	}

}

/*
 * Adjust the final scan/join relation, and recursively all of its children,
 * to generate the final scan/join target.  It would be more correct to model
 * this as a separate planning step with a new RelOptInfo at the toplevel and
 * for each child relation, but doing it this way is noticeably cheaper.
 * Maybe that problem can be solved at some point, but for now we do this.
 *
 * If tlist_same_exprs is true, then the scan/join target to be applied has
 * the same expressions as the existing reltarget, so we need only insert the
 * appropriate sortgroupref information.  By avoiding the creation of
 * projection paths we save effort both immediately and at plan creation time.
 */
static void
apply_scanjoin_target_to_paths(PlannerInfo *root,
							   RelOptInfo *rel,
							   List *scanjoin_targets,
							   List *scanjoin_targets_contain_srfs,
							   bool tlist_same_exprs)
{
	PathTarget *scanjoin_target;
	ListCell   *lc;

	/* This recurses, so be paranoid. */
	check_stack_depth();

	/* Extract SRF-free scan/join target. */
	scanjoin_target = linitial_node(PathTarget, scanjoin_targets);

	/*
	 * Apply the SRF-free scan/join target to each existing path.
	 *
	 * If the tlist exprs are the same, we can just inject the sortgroupref
	 * information into the existing pathtargets.  Otherwise, replace each
	 * path with a projection path that generates the SRF-free scan/join
	 * target.  This can't change the ordering of paths within rel->pathlist,
	 * so we just modify the list in place.
	 */
	foreach(lc, rel->pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);

		/* Shouldn't have any parameterized paths anymore */
		Assert(subpath->param_info == NULL);

		if (tlist_same_exprs)
			subpath->pathtarget->sortgrouprefs =
				scanjoin_target->sortgrouprefs;
		else
		{
			Path	   *newpath;

			newpath = (Path *) create_projection_path(root, rel, subpath,
													  scanjoin_target);
			lfirst(lc) = newpath;
		}
	}

	/*
	 * Now, if final scan/join target contains SRFs, insert ProjectSetPath(s)
	 * atop each existing path.  (Note that this function doesn't look at the
	 * cheapest-path fields, which is a good thing because they're bogus right
	 * now.)
	 */
	if (root->parse->hasTargetSRFs)
		adjust_paths_for_srfs(root, rel,
							  scanjoin_targets,
							  scanjoin_targets_contain_srfs);

	/*
	 * Update the rel's target to be the final (with SRFs) scan/join target.
	 * This now matches the actual output of all the paths, and we might get
	 * confused in createplan.c if they don't agree.  We must do this now so
	 * that any append paths made in the next part will use the correct
	 * pathtarget (cf. create_append_path).
	 */
	rel->reltarget = llast_node(PathTarget, scanjoin_targets);

	/*
	 * Reassess which paths are the cheapest.
	 */
	set_cheapest(rel);
}

static RelOptInfo *
create_distinct_paths(PlannerInfo *root,
					  RelOptInfo *input_rel)
{
	Query	   *parse = root->parse;
	Path	   *cheapest_input_path = input_rel->cheapest_total_path;
	RelOptInfo *distinct_rel;
	double		numDistinctRows;
	bool		allow_hash;
	Path	   *path;
	ListCell   *lc;

	/* For now, do all work in the (DISTINCT, NULL) upperrel */
	distinct_rel = fetch_upper_rel(root, UPPERREL_DISTINCT, NULL);

	/* Estimate number of distinct rows there will be */
	if (parse->groupClause || parse->hasAggs ||
		root->hasHavingQual)
	{
		/*
		 * If there was grouping or aggregation, use the number of input rows
		 * as the estimated number of DISTINCT rows (ie, assume the input is
		 * already mostly unique).
		 */
		numDistinctRows = cheapest_input_path->rows;
	}
	else
	{
		/*
		 * Otherwise, the UNIQUE filter has effects comparable to GROUP BY.
		 */
		List	   *distinctExprs;

		distinctExprs = get_sortgrouplist_exprs(parse->distinctClause,
												parse->targetList);
		numDistinctRows = estimate_num_groups(root, distinctExprs,
											  cheapest_input_path->rows,
											  NULL, NULL);
	}

	/*
	 * Consider sort-based implementations of DISTINCT, if possible.
	 */
	if (grouping_is_sortable(parse->distinctClause))
	{
		/*
		 * First, if we have any adequately-presorted paths, just stick a
		 * Unique node on those.  Then consider doing an explicit sort of the
		 * cheapest input path and Unique'ing that.
		 */
		List	   *needed_pathkeys;

		needed_pathkeys = root->distinct_pathkeys;

		foreach(lc, input_rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);

			if (pathkeys_contained_in(needed_pathkeys, path->pathkeys))
			{
				add_path(distinct_rel, (Path *)
						 create_upper_unique_path(root, distinct_rel,
												  path,
												  list_length(root->distinct_pathkeys),
												  numDistinctRows));
			}
		}

		needed_pathkeys = root->distinct_pathkeys;

		path = cheapest_input_path;
		if (!pathkeys_contained_in(needed_pathkeys, path->pathkeys))
			path = (Path *) create_sort_path(root, distinct_rel,
											 path,
											 needed_pathkeys);

		add_path(distinct_rel, (Path *)
				 create_upper_unique_path(root, distinct_rel,
										  path,
										  list_length(root->distinct_pathkeys),
										  numDistinctRows));
	}

	/*
	 * Consider hash-based implementations of DISTINCT, if possible.
	 *
	 * If we were not able to make any other types of path, we *must* hash or
	 * die trying.  If we do have other choices, the only thing that should
	 * prevent selection of hashing is if enable_hashagg is off.
	 *
	 * Note: grouping_is_hashable() is much more expensive to check than the
	 * other gating conditions, so we want to do it last.
	 */
	if (distinct_rel->pathlist == NIL)
		allow_hash = true;		/* we have no alternatives */
	else if (!enable_hashagg)
		allow_hash = false;		/* policy-based decision not to hash */
	else
		allow_hash = true;		/* default */

	if (allow_hash && grouping_is_hashable(parse->distinctClause))
	{
		/* Generate hashed aggregate path --- no sort needed */
		add_path(distinct_rel, (Path *)
				 create_agg_path(root,
								 distinct_rel,
								 cheapest_input_path,
								 cheapest_input_path->pathtarget,
								 AGG_HASHED,
								 parse->distinctClause,
								 NIL,
								 NULL,
								 numDistinctRows));
	}

	/* Give a helpful error if we failed to find any implementation */
	if (distinct_rel->pathlist == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement DISTINCT"),
				 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_DISTINCT,
									input_rel, distinct_rel, NULL);

	/* Now choose the best path(s) */
	set_cheapest(distinct_rel);

	return distinct_rel;
}

/*
 * create_ordered_paths
 *
 * Build a new upperrel containing Paths for ORDER BY evaluation.
 *
 * All paths in the result must satisfy the ORDER BY ordering.
 * The only new paths we need consider are an explicit full sort
 * and incremental sort on the cheapest-total existing path.
 *
 * input_rel: contains the source-data Paths
 * target: the output tlist the result Paths must emit
 *
 * XXX This only looks at sort_pathkeys. I wonder if it needs to look at the
 * other pathkeys (grouping, ...) like generate_useful_gather_paths.
 */
static RelOptInfo *
create_ordered_paths(PlannerInfo *root,
					 RelOptInfo *input_rel,
					 PathTarget *target)
{
	Path	   *cheapest_input_path = input_rel->cheapest_total_path;
	RelOptInfo *ordered_rel;
	ListCell   *lc;

	/* For now, do all work in the (ORDERED, NULL) upperrel */
	ordered_rel = fetch_upper_rel(root, UPPERREL_ORDERED, NULL);

	foreach(lc, input_rel->pathlist)
	{
		Path	   *input_path = (Path *) lfirst(lc);
		Path	   *sorted_path = input_path;
		bool		is_sorted;
		int			presorted_keys;

		is_sorted = pathkeys_count_contained_in(root->sort_pathkeys,
												input_path->pathkeys, &presorted_keys);

		if (is_sorted)
		{
			/* Use the input path as is, but add a projection step if needed */
			if (sorted_path->pathtarget != target)
				sorted_path = apply_projection_to_path(root, ordered_rel,
													   sorted_path, target);

			add_path(ordered_rel, sorted_path);
		}
		else
		{
			/*
			 * Try adding an explicit sort, but only to the cheapest total
			 * path since a full sort should generally add the same cost to
			 * all paths.
			 */
			if (input_path == cheapest_input_path)
			{
				/*
				 * Sort the cheapest input path.
				 */
				sorted_path = (Path *) create_sort_path(root,
															ordered_rel,
															input_path,
															root->sort_pathkeys);
				/* Add projection step if needed */
				if (sorted_path->pathtarget != target)
					sorted_path = apply_projection_to_path(root, ordered_rel,
														   sorted_path, target);

				add_path(ordered_rel, sorted_path);
			}

			/*
			 * If incremental sort is enabled, then try it as well. Unlike
			 * with regular sorts, we can't just look at the cheapest path,
			 * because the cost of incremental sort depends on how well
			 * presorted the path is. Additionally incremental sort may enable
			 * a cheaper startup path to win out despite higher total cost.
			 */
			if (!enable_incremental_sort)
				continue;

			/* Likewise, if the path can't be used for incremental sort. */
			if (!presorted_keys)
				continue;

			/* Also consider incremental sort. */
			sorted_path = (Path *) create_incremental_sort_path(root,
																ordered_rel,
																input_path,
																root->sort_pathkeys,
																presorted_keys);

			/* Add projection step if needed */
			if (sorted_path->pathtarget != target)
				sorted_path = apply_projection_to_path(root, ordered_rel,
													   sorted_path, target);

			add_path(ordered_rel, sorted_path);
		}
	}

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_ORDERED,
									input_rel, ordered_rel, NULL);

	/*
	 * No need to bother with set_cheapest here; grouping_planner does not
	 * need us to do it.
	 */
	Assert(ordered_rel->pathlist != NIL);

	return ordered_rel;
}


/*
 * make_group_input_target
 *	  Generate appropriate PathTarget for initial input to grouping nodes.
 *
 * If there is grouping or aggregation, the scan/join subplan cannot emit
 * the query's final targetlist; for example, it certainly can't emit any
 * aggregate function calls.  This routine generates the correct target
 * for the scan/join subplan.
 *
 * The query target list passed from the parser already contains entries
 * for all ORDER BY and GROUP BY expressions, but it will not have entries
 * for variables used only in HAVING clauses; so we need to add those
 * variables to the subplan target list.  Also, we flatten all expressions
 * except GROUP BY items into their component variables; other expressions
 * will be computed by the upper plan nodes rather than by the subplan.
 * For example, given a query like
 *		SELECT a+b,SUM(c+d) FROM table GROUP BY a+b;
 * we want to pass this targetlist to the subplan:
 *		a+b,c,d
 * where the a+b target will be used by the Sort/Group steps, and the
 * other targets will be used for computing the final results.
 *
 * 'final_target' is the query's final target list (in PathTarget form)
 *
 * The result is the PathTarget to be computed by the Paths returned from
 * query_planner().
 */
static PathTarget *
make_group_input_target(PlannerInfo *root, PathTarget *final_target)
{
	Query	   *parse = root->parse;
	PathTarget *input_target;
	List	   *non_group_cols;
	List	   *non_group_vars;
	int			i;
	ListCell   *lc;

	/*
	 * We must build a target containing all grouping columns, plus any other
	 * Vars mentioned in the query's targetlist and HAVING qual.
	 */
	input_target = create_empty_pathtarget();
	non_group_cols = NIL;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(final_target, i);

		if (sgref && parse->groupClause &&
			get_sortgroupref_clause_noerr(sgref, parse->groupClause) != NULL)
		{
			/*
			 * It's a grouping column, so add it to the input target as-is.
			 */
			add_column_to_pathtarget(input_target, expr, sgref);
		}
		else
		{
			/*
			 * Non-grouping column, so just remember the expression for later
			 * call to pull_var_clause.
			 */
			non_group_cols = lappend(non_group_cols, expr);
		}

		i++;
	}

	/*
	 * If there's a HAVING clause, we'll need the Vars it uses, too.
	 */
	if (parse->havingQual)
		non_group_cols = lappend(non_group_cols, parse->havingQual);

	/*
	 * Pull out all the Vars mentioned in non-group cols (plus HAVING), and
	 * add them to the input target if not already present.  (A Var used
	 * directly as a GROUP BY item will be present already.)  Note this
	 * includes Vars used in resjunk items, so we are covering the needs of
	 * ORDER BY and window specifications.  Vars used within Aggrefs and
	 * WindowFuncs will be pulled out here, too.
	 */
	non_group_vars = pull_var_clause((Node *) non_group_cols,
									 PVC_RECURSE_AGGREGATES |
									 PVC_INCLUDE_PLACEHOLDERS);
	add_new_columns_to_pathtarget(input_target, non_group_vars);

	/* clean up cruft */
	list_free(non_group_vars);
	list_free(non_group_cols);

	/* XXX this causes some redundant cost calculation ... */
	return set_pathtarget_cost_width(root, input_target);
}

