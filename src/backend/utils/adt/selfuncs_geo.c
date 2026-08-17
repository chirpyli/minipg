/*-------------------------------------------------------------------------
 *
 * selfuncs_geo.c
 *	  Generic selectivity routines for operators registered in pg_operator
 *	  via the "oprrest" and "oprjoin" attributes.
 *
 * These were originally part of geo_selfuncs.c, but are kept (even though
 * the geometric types were removed) because non-geometric operators such as
 * range operators still reference them for join/selectivity estimation.
 * The estimates are intentionally crude placeholders.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/utils/adt/selfuncs_geo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
extern Datum contjoinsel(PG_FUNCTION_ARGS);
extern Datum contsel(PG_FUNCTION_ARGS);
extern Datum positionjoinsel(PG_FUNCTION_ARGS);
extern Datum positionsel(PG_FUNCTION_ARGS);
extern Datum areajoinsel(PG_FUNCTION_ARGS);
extern Datum areasel(PG_FUNCTION_ARGS);

#include "catalog/pg_operator.h"
#include "utils/selfuncs.h"


/* Selectivity for operators that depend on area, such as "overlap". */
Datum
areasel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.005);
}

Datum
areajoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.005);
}

/* How likely is a box to be strictly left of (right of, above, below) a box? */
Datum
positionsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.1);
}

Datum
positionjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.1);
}

/*
 * contsel -- How likely is a box to contain (be contained by) a given box?
 * This is a tighter constraint than "overlap", so produce a smaller
 * estimate than areasel does.
 */
Datum
contsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.001);
}

Datum
contjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.001);
}
