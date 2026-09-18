/*-------------------------------------------------------------------------
 *
 * planagg.c
 *	  Special planning for aggregate queries.
 *
 * This module tries to replace MIN/MAX aggregate functions by subqueries
 * of the form
 *		(SELECT col FROM tab
 *		 WHERE col IS NOT NULL AND existing-quals
 *		 ORDER BY col ASC/DESC
 *		 LIMIT 1)
 * Given a suitable index on tab.col, this can be much faster than the
 * generic scan-all-the-rows aggregation plan.  We can handle multiple
 * MIN/MAX aggregates by generating multiple subqueries, and their
 * orderings can be different.  However, if the query contains any
 * non-optimizable aggregates, there's no point since we'll have to
 * scan all the rows anyway.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planagg.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

