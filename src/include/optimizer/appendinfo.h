/*-------------------------------------------------------------------------
 *
 * appendinfo.h
 *	  Routines for mapping expressions between append rel parent(s) and
 *	  children
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/appendinfo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef APPENDINFO_H
#define APPENDINFO_H

#include "nodes/pathnodes.h"
#include "utils/relcache.h"

extern List *adjust_inherited_attnums(List *attnums, AppendRelInfo *context);
extern List *adjust_inherited_attnums_multilevel(PlannerInfo *root,
												 List *attnums,
												 Index child_relid,
												 Index top_parent_relid);
extern void add_row_identity_var(PlannerInfo *root, Var *rowid_var,
								 Index rtindex, const char *rowid_name);
extern void add_row_identity_columns(PlannerInfo *root, Index rtindex,
									 RangeTblEntry *target_rte,
									 Relation target_relation);
extern void distribute_row_identity_vars(PlannerInfo *root);

#endif							/* APPENDINFO_H */
