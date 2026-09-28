/*-------------------------------------------------------------------------
 *
 * pg_tablespace.h
 *	  definition of the "tablespace" system catalog (pg_tablespace)
 *
 * minipg 已裁剪用户表空间：CREATE/DROP/ALTER TABLESPACE 语句、表空间选项
 * （spcoptions 及其 TOAST 表）以及 pg_tablespace_size 等函数都已移除。
 * pg_tablespace 中仅保留 pg_default（1663）与 pg_global（1664）两个内建
 * 表空间，分别映射 $PGDATA/base 与 $PGDATA/global。
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_tablespace.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TABLESPACE_H
#define PG_TABLESPACE_H

#include "catalog/genbki.h"
#include "catalog/pg_tablespace_d.h"

/* ----------------
 *		pg_tablespace definition.  cpp turns this into
 *		typedef struct FormData_pg_tablespace
 * ----------------
 */
CATALOG(pg_tablespace,1213,TableSpaceRelationId) BKI_SHARED_RELATION
{
	Oid			oid;			/* oid */
	NameData	spcname;		/* tablespace name */
} FormData_pg_tablespace;

/* ----------------
 *		Form_pg_tablespace corresponds to a pointer to a tuple with
 *		the format of pg_tablespace relation.
 * ----------------
 */
typedef FormData_pg_tablespace *Form_pg_tablespace;

DECLARE_UNIQUE_INDEX_PKEY(pg_tablespace_oid_index, 2697, on pg_tablespace using btree(oid oid_ops));
#define TablespaceOidIndexId  2697
DECLARE_UNIQUE_INDEX(pg_tablespace_spcname_index, 2698, on pg_tablespace using btree(spcname name_ops));
#define TablespaceNameIndexId  2698

#endif							/* PG_TABLESPACE_H */
