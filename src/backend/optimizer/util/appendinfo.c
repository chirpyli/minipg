/*-------------------------------------------------------------------------
 *
 * appendinfo.c
 *	  Routines for mapping between append parent(s) and children
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/appendinfo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_collation.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * adjust_inherited_attnums
 *	  Translate an integer list of attribute numbers from parent to child.
 */
List *
adjust_inherited_attnums(List *attnums, AppendRelInfo *context)
{
	List	   *result = NIL;
	ListCell   *lc;

	/* This should only happen for an inheritance case, not UNION ALL */
	Assert(OidIsValid(context->parent_reloid));

	/* Look up each attribute in the AppendRelInfo's translated_vars list */
	foreach(lc, attnums)
	{
		AttrNumber	parentattno = lfirst_int(lc);
		Var		   *childvar;

		/* Look up the translation of this column: it must be a Var */
		if (parentattno <= 0 ||
			parentattno > list_length(context->translated_vars))
			elog(ERROR, "attribute %d of relation \"%s\" does not exist",
				 parentattno, get_rel_name(context->parent_reloid));
		childvar = (Var *) list_nth(context->translated_vars, parentattno - 1);
		if (childvar == NULL || !IsA(childvar, Var))
			elog(ERROR, "attribute %d of relation \"%s\" does not exist",
				 parentattno, get_rel_name(context->parent_reloid));

		result = lappend_int(result, childvar->varattno);
	}
	return result;
}

/*
 * adjust_inherited_attnums_multilevel
 *	  As above, but traverse multiple inheritance levels as needed.
 */
List *
adjust_inherited_attnums_multilevel(PlannerInfo *root, List *attnums,
									Index child_relid, Index top_parent_relid)
{
	AppendRelInfo *appinfo = root->append_rel_array[child_relid];

	if (!appinfo)
		elog(ERROR, "child rel %d not found in append_rel_array", child_relid);

	/* Recurse if immediate parent is not the top parent. */
	if (appinfo->parent_relid != top_parent_relid)
		attnums = adjust_inherited_attnums_multilevel(root, attnums,
													  appinfo->parent_relid,
													  top_parent_relid);

	/* Now translate for this child */
	return adjust_inherited_attnums(attnums, appinfo);
}

/*****************************************************************************
 *
 *		ROW-IDENTITY VARIABLE MANAGEMENT
 *
 * This code lacks a good home, perhaps.  We choose to keep it here for
 * historical reasons.
 *
 *****************************************************************************/

/*
 * add_row_identity_var
 *	  Register a row-identity column to be used in UPDATE/DELETE.
 *
 * The Var must be equal(), aside from varno, to any other row-identity
 * column with the same rowid_name.  Thus, for example, "wholerow"
 * row identities had better use vartype == RECORDOID.
 *
 * rtindex is currently redundant with rowid_var->varno, but we specify
 * it as a separate parameter in case this is ever generalized to support
 * non-Var expressions.  (We could reasonably handle expressions over
 * Vars of the specified rtindex, but for now that seems unnecessary.)
 */
void
add_row_identity_var(PlannerInfo *root, Var *orig_var,
					 Index rtindex, const char *rowid_name)
{
	TargetEntry *tle;
	Var		   *rowid_var;
	RowIdentityVarInfo *ridinfo;
	ListCell   *lc;

	/* For now, the argument must be just a Var of the given rtindex */
	Assert(IsA(orig_var, Var));
	Assert(orig_var->varno == rtindex);
	Assert(orig_var->varlevelsup == 0);

	/*
	 * If we're doing non-inherited UPDATE/DELETE, there's little need for
	 * ROWID_VAR shenanigans.  Just shove the presented Var into the
	 * processed_tlist, and we're done.
	 */
	if (rtindex == root->parse->resultRelation)
	{
		tle = makeTargetEntry((Expr *) orig_var,
							  list_length(root->processed_tlist) + 1,
							  pstrdup(rowid_name),
							  true);
		root->processed_tlist = lappend(root->processed_tlist, tle);
		return;
	}

	/*
	 * Otherwise, rtindex should reference a leaf target relation that's being
	 * added to the query during appendrel expansion.
	 */
	Assert(bms_is_member(rtindex, root->leaf_result_relids));
	Assert(root->append_rel_array[rtindex] != NULL);

	/*
	 * We have to find a matching RowIdentityVarInfo, or make one if there is
	 * none.  To allow using equal() to match the vars, change the varno to
	 * ROWID_VAR, leaving all else alone.
	 */
	rowid_var = copyObject(orig_var);
	/* This could eventually become ChangeVarNodes() */
	rowid_var->varno = ROWID_VAR;

	/* Look for an existing row-id column of the same name */
	foreach(lc, root->row_identity_vars)
	{
		ridinfo = (RowIdentityVarInfo *) lfirst(lc);
		if (strcmp(rowid_name, ridinfo->rowidname) != 0)
			continue;
		if (equal(rowid_var, ridinfo->rowidvar))
		{
			/* Found a match; we need only record that rtindex needs it too */
			ridinfo->rowidrels = bms_add_member(ridinfo->rowidrels, rtindex);
			return;
		}
		else
		{
			/* Ooops, can't handle this */
			elog(ERROR, "conflicting uses of row-identity name \"%s\"",
				 rowid_name);
		}
	}

	/* No request yet, so add a new RowIdentityVarInfo */
	ridinfo = makeNode(RowIdentityVarInfo);
	ridinfo->rowidvar = copyObject(rowid_var);
	/* for the moment, estimate width using just the datatype info */
	ridinfo->rowidwidth = get_typavgwidth(exprType((Node *) rowid_var),
										  exprTypmod((Node *) rowid_var));
	ridinfo->rowidname = pstrdup(rowid_name);
	ridinfo->rowidrels = bms_make_singleton(rtindex);

	root->row_identity_vars = lappend(root->row_identity_vars, ridinfo);

	/* Change rowid_var into a reference to this row_identity_vars entry */
	rowid_var->varattno = list_length(root->row_identity_vars);

	/* Push the ROWID_VAR reference variable into processed_tlist */
	tle = makeTargetEntry((Expr *) rowid_var,
						  list_length(root->processed_tlist) + 1,
						  pstrdup(rowid_name),
						  true);
	root->processed_tlist = lappend(root->processed_tlist, tle);
}

/*
 * add_row_identity_columns
 *
 * This function adds the row identity columns needed by the core code.
 */
void
add_row_identity_columns(PlannerInfo *root, Index rtindex,
						 RangeTblEntry *target_rte,
						 Relation target_relation)
{
	char		relkind = target_relation->rd_rel->relkind;
	Var		   *var;

	Assert(root->parse->commandType == CMD_UPDATE ||
		   root->parse->commandType == CMD_DELETE);

	if (relkind == RELKIND_RELATION)
	{
		/*
		 * Emit CTID so that executor can find the row to update or delete.
		 */
		var = makeVar(rtindex,
					  SelfItemPointerAttributeNumber,
					  TIDOID,
					  -1,
					  InvalidOid,
					  0);
		add_row_identity_var(root, var, rtindex, "ctid");
	}
}

/*
 * distribute_row_identity_vars
 *
 * After we have finished identifying all the row identity columns
 * needed by an inherited UPDATE/DELETE query, make sure that these
 * columns will be generated by all the target relations.
 *
 * This is more or less like what build_base_rel_tlists() does,
 * except that it would not understand what to do with ROWID_VAR Vars.
 * Since that function runs before inheritance relations are expanded,
 * it will never see any such Vars anyway.
 */
void
distribute_row_identity_vars(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	int			result_relation = parse->resultRelation;
	RangeTblEntry *target_rte;
	RelOptInfo *target_rel;
	ListCell   *lc;

	/* There's nothing to do if this isn't an inherited UPDATE/DELETE. */
	if (parse->commandType != CMD_UPDATE && parse->commandType != CMD_DELETE)
	{
		Assert(root->row_identity_vars == NIL);
		return;
	}
	target_rte = rt_fetch(result_relation, parse->rtable);
	Assert(root->row_identity_vars == NIL);
	return;

	/*
	 * Ordinarily, we expect that leaf result relation(s) will have added some
	 * ROWID_VAR Vars to the query.  However, it's possible that constraint
	 * exclusion suppressed every leaf relation.  The executor will get upset
	 * if the plan has no row identity columns at all, even though it will
	 * certainly process no rows.  Handle this edge case by re-opening the top
	 * result relation and adding the row identity columns it would have used,
	 * as preprocess_targetlist() would have done if it weren't marked "inh".
	 * (This is a bit ugly, but it seems better to confine the ugliness and
	 * extra cycles to this unusual corner case.)  We needn't worry about
	 * fixing the rel's reltarget, as that won't affect the finished plan.
	 */
	if (root->row_identity_vars == NIL)
	{
		Relation	target_relation;

		target_relation = table_open(target_rte->relid, NoLock);
		add_row_identity_columns(root, result_relation,
								 target_rte, target_relation);
		table_close(target_relation, NoLock);
		return;
	}

	/*
	 * Dig through the processed_tlist to find the ROWID_VAR reference Vars,
	 * and forcibly copy them into the reltarget list of the topmost target
	 * relation.  That's sufficient because they'll be copied to the
	 * individual leaf target rels (with appropriate translation) later,
	 * during appendrel expansion.
	 */
	target_rel = find_base_rel(root, result_relation);

	foreach(lc, root->processed_tlist)
	{
		TargetEntry *tle = lfirst(lc);
		Var		   *var = (Var *) tle->expr;

		if (var && IsA(var, Var) && var->varno == ROWID_VAR)
		{
			target_rel->reltarget->exprs =
				lappend(target_rel->reltarget->exprs, copyObject(var));
			/* reltarget cost and width will be computed later */
		}
	}
}


