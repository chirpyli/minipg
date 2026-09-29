/*-------------------------------------------------------------------------
 *
 * planner.h
 *	  prototypes for planner.c.
 *
 * Note that the primary entry points for planner.c are declared in
 * optimizer/optimizer.h, because they're intended to be called from
 * non-planner code.  Declarations here are meant for use by other
 * planner modules.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/planner.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNER_H
#define PLANNER_H

#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"


extern PlannerInfo *subquery_planner(PlannerGlobal *glob, Query *parse,
									 PlannerInfo *parent_root,
									 double tuple_fraction);

extern RowMarkType select_rowmark_type(RangeTblEntry *rte);


extern Path *get_cheapest_fractional_path(RelOptInfo *rel,
										  double tuple_fraction);

extern Expr *preprocess_phv_expression(PlannerInfo *root, Expr *expr);

#endif							/* PLANNER_H */
