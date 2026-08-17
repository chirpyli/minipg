/*-------------------------------------------------------------------------
 *
 * acl.h
 *	  Support for access/ownership checking.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/acl.h
 *
 * NOTES
 *-------------------------------------------------------------------------
 */
#ifndef ACL_H
#define ACL_H

#include "access/htup.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "utils/snapshot.h"


#define ACL_ID_PUBLIC	0		/* placeholder for id in a PUBLIC acl item */

#define ACLITEMOID 1033



/*
 * According to the SQL standard, the grantor of a privilege can be
 * "PUBLIC".  In GPDB, we represent this in the low bit of the grantee id.
 * Note that this bit is cleared before storing AclItems in the system
 * catalogs.
 */
#define ACLITEM_ALL_GRANTEES	((Oid) 0)

#define ACL_GRANT_OPTION_FOR(priv) (1 << (29 - (priv)))
#define ACL_OPTION_IS_GLOBAL(priv) (1 << (30 - (priv)))

#define ACL_GRANT_WGO(priv) (priv | ACL_GRANT_OPTION_FOR(priv))

#define ACL_ALL_RIGHTS_NO_GRANTOpts	0

#define ACL_ALL_RIGHTS_RELATION	(ACL_INSERT|ACL_SELECT|ACL_UPDATE|ACL_DELETE|ACL_TRUNCATE|ACL_REFERENCES|ACL_TRIGGER)
#define ACL_ALL_RIGHTS_SEQUENCE	(ACL_USAGE|ACL_SELECT|ACL_UPDATE)
#define ACL_ALL_RIGHTS_DATABASE	(ACL_CREATE|ACL_CREATE_TEMP|ACL_CONNECT)
#define ACL_ALL_RIGHTS_FDW		(ACL_USAGE)
#define ACL_ALL_RIGHTS_FOREIGN_SERVER (ACL_USAGE)
#define ACL_ALL_RIGHTS_FUNCTION	(ACL_EXECUTE)
#define ACL_ALL_RIGHTS_LANGUAGE	(ACL_USAGE)
#define ACL_ALL_RIGHTS_LARGEOBJECT	(ACL_SELECT|ACL_UPDATE)
#define ACL_ALL_RIGHTS_NAMESPACE	(ACL_USAGE|ACL_CREATE)
#define ACL_ALL_RIGHTS_OPSCHEMA	(ACL_USAGE|ACL_CREATE)
#define ACL_ALL_RIGHTS_TYPE		(ACL_USAGE)
#define ACL_ALL_RIGHTS_OPCLASS	(ACL_USAGE)
#define ACL_ALL_RIGHTS_OPFAMILY	(ACL_USAGE)
#define ACL_ALL_RIGHTS_SCHEMA	(ACL_USAGE|ACL_CREATE)

/*
 * Result codes for permission-check functions
 */
typedef enum
{
	ACLCHECK_OK = 0,
	ACLCHECK_NO_PRIV,
	ACLCHECK_NOT_OWNER
} AclResult;

/*
 * routines used internally (role membership)
 */
extern Oid	get_role_oid(const char *rolename, bool missing_ok);
extern Oid	get_rolespec_oid(const RoleSpec *role, bool missing_ok);
extern char *get_rolespec_name(const RoleSpec *role);
#endif							/* ACL_H */
