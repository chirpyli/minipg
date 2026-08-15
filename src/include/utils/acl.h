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
 *	  minipg 已移除细粒度 ACL（访问控制列表）的判定与数据运算机制：各系统
 *	  catalog 的 *acl 列、pg_acl catalog、aclitem 类型运算、GRANT/REVOKE 命令
 *	  及 pg_default_acl 均已删除，acl.c 中的 ACL 数据函数（acldefault/aclupdate/
 *	  aclnewowner/aclmask 等）也已一并删除。本头文件仅保留：
 *	  - Acl/AclItem 类型与 ACL_* 权限位宏（被 GenerateTypeDependencies 等核心
 *	    类型系统函数的签名以及解析器权限位机制所依赖，仅作为类型/位定义，
 *	    不再承载任何 ACL 判定语义）；
 *	  - 所有权检查（ownercheck）与角色成员关系判定所需的声明；
 *	  - 权限/所有权不足时的标准错误报告（aclcheck_error/aclcheck_error_type）。
 *-------------------------------------------------------------------------
 */
#ifndef ACL_H
#define ACL_H

#include "access/htup.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "utils/snapshot.h"


/*
 * typedef AclMode is declared in parsenodes.h, also the individual privilege
 * bit meanings are defined there
 */


#define ACL_ID_PUBLIC	0		/* placeholder for id in a PUBLIC acl item */

/*
 * AclItem
 *
 * Note: must be same size on all platforms, because the size is hardcoded
 * in the pg_type.h entry for aclitem.
 */

typedef struct AclItem
{
	Oid			ai_grantee;		/* ID that this item grants privs to */
	Oid			ai_grantor;		/* grantor of privs */
	AclMode		ai_privs;		/* privs being granted */

	/*
	 * Note: ai_grantor is not redundant with the grantor fields in the
	 * actual ACL SHOW command.  If the _PRIVILEGES functions are ever
	 * changed to return ACL items rather than strings, this may be
	 * irrelevant.
	 */
} AclItem;

#define ACLITEMOID 1033


/*
 * Definitions for convenient access to Acl (array of AclItem).
 * These are standard PostgreSQL arrays, but are restricted to have one
 * dimension and no nulls.  We also ignore the lower bound when reading,
 * and set it to one when writing.
 */

/*
 * Acl			a one-dimensional array of AclItem
 */
typedef AclItem *Acl;

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
extern bool has_privs_of_role(Oid member, Oid role);
extern bool is_member_of_role(Oid member, Oid role);
extern bool is_member_of_role_nosuper(Oid member, Oid role);
extern bool is_admin_of_role(Oid member, Oid role);
extern void check_is_member_of_role(Oid member, Oid role);
extern Oid	get_role_oid(const char *rolename, bool missing_ok);
extern Oid	get_rolespec_oid(const RoleSpec *role, bool missing_ok);
extern void check_rolespec_name(const RoleSpec *role, const char *detail_msg);
extern HeapTuple get_rolespec_tuple(const RoleSpec *role);
extern char *get_rolespec_name(const RoleSpec *role);


extern bool initialize_acl(void);

/*
 * standardized reporting of aclcheck permission failures
 */
extern void aclcheck_error(AclResult aclerr, ObjectType objtype,
						   const char *objectname);
extern void aclcheck_error_type(AclResult aclerr, Oid typeOid);

/* ownercheck routines just return true (owner) or false (not) */
extern bool pg_class_ownercheck(Oid class_oid, Oid roleid);
extern bool pg_type_ownercheck(Oid type_oid, Oid roleid);
extern bool pg_oper_ownercheck(Oid oper_oid, Oid roleid);
extern bool pg_proc_ownercheck(Oid proc_oid, Oid roleid);
extern bool pg_language_ownercheck(Oid lan_oid, Oid roleid);
extern bool pg_namespace_ownercheck(Oid nsp_oid, Oid roleid);
extern bool pg_tablespace_ownercheck(Oid spc_oid, Oid roleid);
extern bool pg_opclass_ownercheck(Oid opc_oid, Oid roleid);
extern bool pg_opfamily_ownercheck(Oid opf_oid, Oid roleid);
extern bool pg_database_ownercheck(Oid db_oid, Oid roleid);
extern bool pg_collation_ownercheck(Oid coll_oid, Oid roleid);
extern bool pg_conversion_ownercheck(Oid conv_oid, Oid roleid);
extern bool pg_event_trigger_ownercheck(Oid et_oid, Oid roleid);
extern bool pg_extension_ownercheck(Oid ext_oid, Oid roleid);

extern bool pg_statistics_object_ownercheck(Oid stat_oid, Oid roleid);

#endif							/* ACL_H */
