/*-------------------------------------------------------------------------
 *
 * extended_stats_internal.h
 *	  POSTGRES statistics internal declarations (core ANALYZE support)
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/statistics/extended_stats_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXTENDED_STATS_INTERNAL_H
#define EXTENDED_STATS_INTERNAL_H

#include "utils/sortsupport.h"

typedef struct
{
	Oid			eqopr;			/* '=' operator for datatype, if any */
	Oid			eqfunc;			/* and associated function */
	Oid			ltopr;			/* '<' operator for datatype, if any */
} StdAnalyzeData;

typedef struct
{
	Datum		value;			/* a data value */
	int			tupno;			/* position index for tuple it came from */
} ScalarItem;

/* multi-sort */
typedef struct MultiSortSupportData
{
	int			ndims;			/* number of dimensions */
	/* sort support data for each dimension: */
	SortSupportData ssup[FLEXIBLE_ARRAY_MEMBER];
} MultiSortSupportData;

typedef MultiSortSupportData *MultiSortSupport;

typedef struct SortItem
{
	Datum	   *values;
	bool	   *isnull;
	int			count;
} SortItem;

extern int	multi_sort_compare(const void *a, const void *b, void *arg);
extern int	multi_sort_compare_dim(int dim, const SortItem *a,
								   const SortItem *b, MultiSortSupport mss);
extern int	multi_sort_compare_dims(int start, int end, const SortItem *a,
									const SortItem *b, MultiSortSupport mss);
extern int	compare_scalars_simple(const void *a, const void *b, void *arg);
extern int	compare_datums_simple(Datum a, Datum b, SortSupport ssup);

#endif							/* EXTENDED_STATS_INTERNAL_H */
