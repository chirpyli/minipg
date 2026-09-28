/*-------------------------------------------------------------------------
 *
 * planmain.c
 *	  规划单条查询的例程
 *
 * 名字有什么要紧的呢？规划器/优化器的顶层入口其实在 planner.c 中，
 * 而不像你从这个文件名想象的那样在这里。不过，这里确实是规划基本连接
 * （join）操作的主要代码，只是去掉了子查询（subselect）、继承
 * （inheritance）等特性。（那些是 planner.c 负责处理的东西。）
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planmain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/orclauses.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/planmain.h"


/*
 * query_planner
 *	  为一条基本查询生成路径（path，也就是简化版的计划），
 *	  该查询可以包含连接（join），但不涉及任何更花哨的特性。
 *
 * 由于 query_planner 不负责顶层处理（分组、排序等），它无法独自选出
 * 最优路径。因此它返回连接顶层的 RelOptInfo，由调用方
 * （upper_planner）在该 rel 存活的诸多路径中进行选择。
 *
 * root 描述要规划的查询
 * qp_callback 是一个函数，用于在可以安全计算时算出 query_pathkeys
 * qp_extra 是要传给 qp_callback 的可选额外数据
 *
 * 注意：PlannerInfo 节点中还包含一个 query_pathkeys 字段，它告诉
 * query_planner 最终输出计划所期望的排序顺序。该值在调用时 *并不* 可用，
 * 而是在我们完成查询等价类（equivalence class）合并之后，由
 * qp_callback 计算出来。（在那之前我们无法构造规范的 pathkeys。）
 */
RelOptInfo *
query_planner(PlannerInfo *root,
			  query_pathkeys_callback qp_callback, void *qp_extra)
{
	Query	   *parse = root->parse;
	List	   *joinlist;
	RelOptInfo *final_rel;

	/*
	 * Init planner lists to empty.
	 *
	 * NOTE: append_rel_list was set up by subquery_planner, so do not touch
	 * here.
	 */
	root->join_rel_list = NIL;
	root->join_rel_hash = NULL;
	root->join_rel_level = NULL;
	root->join_cur_level = 0;
	root->canon_pathkeys = NIL;
	root->left_join_clauses = NIL;
	root->right_join_clauses = NIL;
	root->full_join_clauses = NIL;
	root->join_info_list = NIL;
	root->placeholder_list = NIL;
	root->initial_rels = NIL;

	/*
	 * Set up arrays for accessing base relations and AppendRelInfos.
	 */
	setup_simple_rel_arrays(root);

	/*
	 * In the trivial case where the jointree is a single RTE_RESULT relation,
	 * bypass all the rest of this function and just make a RelOptInfo and its
	 * one access path.  This is worth optimizing because it applies for
	 * common cases like "SELECT expression" and "INSERT ... VALUES()".
	 */
	Assert(parse->jointree->fromlist != NIL);
	if (list_length(parse->jointree->fromlist) == 1)
	{
		Node	   *jtnode = (Node *) linitial(parse->jointree->fromlist);

		if (IsA(jtnode, RangeTblRef))
		{
			int			varno = ((RangeTblRef *) jtnode)->rtindex;
			RangeTblEntry *rte = root->simple_rte_array[varno];

			Assert(rte != NULL);
			if (rte->rtekind == RTE_RESULT)
			{
				/* Make the RelOptInfo for it directly */
				final_rel = build_simple_rel(root, varno, false);

				/*
				 * The only path for it is a trivial Result path.  We use a
				 * ResultPath because that way we can just jam the quals into
				 * it without preprocessing them.
				 */
				add_path(final_rel, (Path *)
						 create_result_path(root, final_rel,
												  final_rel->reltarget,
												  (List *) parse->jointree->quals));

				/* Select cheapest path (pretty easy in this case...) */
				set_cheapest(final_rel);

				/*
				 * We don't need to run generate_base_implied_equalities, but
				 * we do need to pretend that EC merging is complete.
				 */
				root->ec_merging_done = true;

				/*
				 * We still are required to call qp_callback, in case it's
				 * something like "SELECT 2+2 ORDER BY 1".
				 */
				(*qp_callback) (root, qp_extra);

				return final_rel;
			}
		}
	}

	/*
	 * Construct RelOptInfo nodes for all base relations used in the query.
	 * Appendrel member relations ("other rels") will be added later.
	 *
	 * Note: the reason we find the baserels by searching the jointree, rather
	 * than scanning the rangetable, is that the rangetable may contain RTEs
	 * for rels not actively part of the query, for example views.  We don't
	 * want to make RelOptInfos for them.
	 */
	add_base_rels_to_query(root, (Node *) parse->jointree);

	/*
	 * Examine the targetlist and join tree, adding entries to baserel
	 * targetlists for all referenced Vars, and generating PlaceHolderInfo
	 * entries for all referenced PlaceHolderVars.  Restrict and join clauses
	 * are added to appropriate lists belonging to the mentioned relations. We
	 * also build EquivalenceClasses for provably equivalent expressions. The
	 * SpecialJoinInfo list is also built to hold information about join order
	 * restrictions.  Finally, we form a target joinlist for make_one_rel() to
	 * work from.
	 */
	build_base_rel_tlists(root, root->processed_tlist);

	find_placeholders_in_jointree(root);

	find_lateral_references(root);

	joinlist = deconstruct_jointree(root);

	/*
	 * Reconsider any postponed outer-join quals now that we have built up
	 * equivalence classes.  (This could result in further additions or
	 * mergings of classes.)
	 */
	reconsider_outer_join_clauses(root);

	/*
	 * If we formed any equivalence classes, generate additional restriction
	 * clauses as appropriate.  (Implied join clauses are formed on-the-fly
	 * later.)
	 */
	generate_base_implied_equalities(root);

	/*
	 * We have completed merging equivalence sets, so it's now possible to
	 * generate pathkeys in canonical form; so compute query_pathkeys and
	 * other pathkeys fields in PlannerInfo.
	 */
	(*qp_callback) (root, qp_extra);

	/*
	 * Examine any "placeholder" expressions generated during subquery pullup.
	 * Make sure that the Vars they need are marked as needed at the relevant
	 * join level.  This must be done before join removal because it might
	 * cause Vars or placeholders to be needed above a join when they weren't
	 * so marked before.
	 */
	fix_placeholder_input_needed_levels(root);

	/*
	 * Remove any useless outer joins.  Ideally this would be done during
	 * jointree preprocessing, but the necessary information isn't available
	 * until we've built baserel data structures and classified qual clauses.
	 */
	joinlist = remove_useless_joins(root, joinlist);

	/*
	 * Also, reduce any semijoins with unique inner rels to plain inner joins.
	 * Likewise, this can't be done until now for lack of needed info.
	 */
	reduce_unique_semijoins(root);

	/*
	 * Now distribute "placeholders" to base rels as needed.  This has to be
	 * done after join removal because removal could change whether a
	 * placeholder is evaluable at a base rel.
	 */
	add_placeholders_to_base_rels(root);

	/*
	 * Construct the lateral reference sets now that we have finalized
	 * PlaceHolderVar eval levels.
	 */
	create_lateral_join_info(root);

	/*
	 * Look for join OR clauses that we can extract single-relation
	 * restriction OR clauses from.
	 */
	extract_restriction_or_clauses(root);

	/*
	 * Distribute any UPDATE/DELETE row identity variables to the target
	 * relations.  This can't be done till we've finished expansion of
	 * appendrels.
	 */
	distribute_row_identity_vars(root);

	/*
	 * Ready to do the primary planning.
	 */
	final_rel = make_one_rel(root, joinlist);

	/* Check that we got at least one usable path */
	if (!final_rel || !final_rel->cheapest_total_path ||
		final_rel->cheapest_total_path->param_info != NULL)
		elog(ERROR, "failed to construct the join relation");

	return final_rel;
}
