/*-------------------------------------------------------------------------
 *
 * acl.c
 *	  Basic access control list data structures manipulation routines.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/acl.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/proclang.h"
#include "commands/tablespace.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "lib/qunique.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/varlena.h"



/* does what it says ... */
Oid
get_role_oid(const char *rolname, bool missing_ok)
{
	return BOOTSTRAP_SUPERUSERID;
}


/*
 * Given a RoleSpec node, return the OID it corresponds to.  If missing_ok is
 * true, return InvalidOid if the role does not exist.
 *
 * minipg 不存在角色，仅保留唯一的 BOOTSTRAP_SUPERUSERID 实体。
 */
Oid
get_rolespec_oid(const RoleSpec *role, bool missing_ok)
{
	return BOOTSTRAP_SUPERUSERID;
}


/*
 * Given a RoleSpec, returns a palloc'ed copy of the corresponding role's name.
 *
 * minipg 仅有一个固定角色名 "postgres"。
 */
char *
get_rolespec_name(const RoleSpec *role)
{
	return pstrdup("postgres");
}


