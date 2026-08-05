/*-------------------------------------------------------------------------
 *
 * aclchk.c
 *	  Routines to check access control permissions.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/aclchk.c
 *
 * NOTES
 *	  See acl.h.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_init_privs.h"
#include "catalog/pg_language.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_largeobject_metadata.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/event_trigger.h"
#include "commands/extension.h"
#include "commands/proclang.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/aclchk_internal.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * Internal format used by ALTER DEFAULT PRIVILEGES.
 */
typedef struct
{
	Oid			roleid;			/* owning role */
	Oid			nspid;			/* namespace, or InvalidOid if none */
	/* remaining fields are same as in InternalGrant: */
	bool		is_grant;
	ObjectType	objtype;
	bool		all_privs;
	AclMode		privileges;
	List	   *grantees;
	bool		grant_option;
	DropBehavior behavior;
} InternalDefaultACL;

/*
 * When performing a binary-upgrade, pg_dump will call a function to set
 * this variable to let us know that we need to populate the pg_init_privs
 * table for the GRANT/REVOKE commands while this variable is set to true.
 */
bool		binary_upgrade_record_init_privs = false;

static AclMode pg_aclmask(ObjectType objtype, Oid table_oid, AttrNumber attnum,
						  Oid roleid, AclMode mask, AclMaskHow how);
static void recordExtensionInitPriv(Oid objoid, Oid classoid, int objsubid,
									Acl *new_acl);
static void recordExtensionInitPrivWorker(Oid objoid, Oid classoid, int objsubid,
										  Acl *new_acl);


/*
 * If is_grant is true, adds the given privileges for the list of
 * grantees to the existing old_acl.  If is_grant is false, the
 * privileges for the given grantees are removed from old_acl.
 *
 * NB: the original old_acl is pfree'd.
 */
static Acl *
merge_acl_with_grant(Acl *old_acl, bool is_grant,
					 bool grant_option, DropBehavior behavior,
					 List *grantees, AclMode privileges,
					 Oid grantorId, Oid ownerId)
{
	unsigned	modechg;
	ListCell   *j;
	Acl		   *new_acl;

	modechg = is_grant ? ACL_MODECHG_ADD : ACL_MODECHG_DEL;

	new_acl = old_acl;

	foreach(j, grantees)
	{
		AclItem		aclitem;
		Acl		   *newer_acl;

		aclitem.ai_grantee = lfirst_oid(j);

		/*
		 * Grant options can only be granted to individual roles, not PUBLIC.
		 * The reason is that if a user would re-grant a privilege that he
		 * held through PUBLIC, and later the user is removed, the situation
		 * is impossible to clean up.
		 */
		if (is_grant && grant_option && aclitem.ai_grantee == ACL_ID_PUBLIC)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_GRANT_OPERATION),
					 errmsg("grant options can only be granted to roles")));

		aclitem.ai_grantor = grantorId;

		/*
		 * The asymmetry in the conditions here comes from the spec.  In
		 * GRANT, the grant_option flag signals WITH GRANT OPTION, which means
		 * to grant both the basic privilege and its grant option. But in
		 * REVOKE, plain revoke revokes both the basic privilege and its grant
		 * option, while REVOKE GRANT OPTION revokes only the option.
		 */
		ACLITEM_SET_PRIVS_GOPTIONS(aclitem,
								   (is_grant || !grant_option) ? privileges : ACL_NO_RIGHTS,
								   (!is_grant || grant_option) ? privileges : ACL_NO_RIGHTS);

		newer_acl = aclupdate(new_acl, &aclitem, modechg, ownerId, behavior);

		/* avoid memory leak when there are many grantees */
		pfree(new_acl);
		new_acl = newer_acl;
	}

	return new_acl;
}

/*
 * Restrict the privileges to what we can actually grant, and emit
 * the standards-mandated warning and error messages.
 */
static AclMode
restrict_and_check_grant(bool is_grant, AclMode avail_goptions, bool all_privs,
						 AclMode privileges, Oid objectId, Oid grantorId,
						 ObjectType objtype, const char *objname,
						 AttrNumber att_number, const char *colname)
{
	AclMode		this_privileges;
	AclMode		whole_mask;

	switch (objtype)
	{
		case OBJECT_COLUMN:
			whole_mask = ACL_ALL_RIGHTS_COLUMN;
			break;
		case OBJECT_TABLE:
			whole_mask = ACL_ALL_RIGHTS_RELATION;
			break;
		case OBJECT_SEQUENCE:
			whole_mask = ACL_ALL_RIGHTS_SEQUENCE;
			break;
		case OBJECT_DATABASE:
			whole_mask = ACL_ALL_RIGHTS_DATABASE;
			break;
		case OBJECT_FUNCTION:
			whole_mask = ACL_ALL_RIGHTS_FUNCTION;
			break;
		case OBJECT_LANGUAGE:
			whole_mask = ACL_ALL_RIGHTS_LANGUAGE;
			break;
		case OBJECT_LARGEOBJECT:
			whole_mask = ACL_ALL_RIGHTS_LARGEOBJECT;
			break;
		case OBJECT_SCHEMA:
			whole_mask = ACL_ALL_RIGHTS_SCHEMA;
			break;
		case OBJECT_TABLESPACE:
			whole_mask = ACL_ALL_RIGHTS_TABLESPACE;
			break;
		case OBJECT_EVENT_TRIGGER:
			elog(ERROR, "grantable rights not supported for event triggers");
			/* not reached, but keep compiler quiet */
			return ACL_NO_RIGHTS;
		case OBJECT_TYPE:
			whole_mask = ACL_ALL_RIGHTS_TYPE;
			break;
		default:
			elog(ERROR, "unrecognized object type: %d", objtype);
			/* not reached, but keep compiler quiet */
			return ACL_NO_RIGHTS;
	}

	/*
	 * If we found no grant options, consider whether to issue a hard error.
	 * Per spec, having any privilege at all on the object will get you by
	 * here.
	 */
	if (avail_goptions == ACL_NO_RIGHTS)
	{
		if (pg_aclmask(objtype, objectId, att_number, grantorId,
					   whole_mask | ACL_GRANT_OPTION_FOR(whole_mask),
					   ACLMASK_ANY) == ACL_NO_RIGHTS)
		{
			if (objtype == OBJECT_COLUMN && colname)
				aclcheck_error_col(ACLCHECK_NO_PRIV, objtype, objname, colname);
			else
				aclcheck_error(ACLCHECK_NO_PRIV, objtype, objname);
		}
	}

	/*
	 * Restrict the operation to what we can actually grant or revoke, and
	 * issue a warning if appropriate.  (For REVOKE this isn't quite what the
	 * spec says to do: the spec seems to want a warning only if no privilege
	 * bits actually change in the ACL. In practice that behavior seems much
	 * too noisy, as well as inconsistent with the GRANT case.)
	 */
	this_privileges = privileges & ACL_OPTION_TO_PRIVS(avail_goptions);
	if (is_grant)
	{
		if (this_privileges == 0)
		{
			if (objtype == OBJECT_COLUMN && colname)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("no privileges were granted for column \"%s\" of relation \"%s\"",
								colname, objname)));
			else
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("no privileges were granted for \"%s\"",
								objname)));
		}
		else if (!all_privs && this_privileges != privileges)
		{
			if (objtype == OBJECT_COLUMN && colname)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("not all privileges were granted for column \"%s\" of relation \"%s\"",
								colname, objname)));
			else
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("not all privileges were granted for \"%s\"",
								objname)));
		}
	}
	else
	{
		if (this_privileges == 0)
		{
			if (objtype == OBJECT_COLUMN && colname)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("no privileges could be revoked for column \"%s\" of relation \"%s\"",
								colname, objname)));
			else
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("no privileges could be revoked for \"%s\"",
								objname)));
		}
		else if (!all_privs && this_privileges != privileges)
		{
			if (objtype == OBJECT_COLUMN && colname)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("not all privileges could be revoked for column \"%s\" of relation \"%s\"",
								colname, objname)));
			else
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("not all privileges could be revoked for \"%s\"",
								objname)));
		}
	}

	return this_privileges;
}


/*
 * Standardized reporting of aclcheck permissions failures.
 *
 * Note: we do not double-quote the %s's below, because many callers
 * supply strings that might be already quoted.
 */
void
aclcheck_error(AclResult aclerr, ObjectType objtype,
			   const char *objectname)
{
	switch (aclerr)
	{
		case ACLCHECK_OK:
			/* no error, so return to caller */
			break;
		case ACLCHECK_NO_PRIV:
			{
				const char *msg = "???";

				switch (objtype)
				{
					case OBJECT_AGGREGATE:
						msg = gettext_noop("permission denied for aggregate %s");
						break;
					case OBJECT_COLLATION:
						msg = gettext_noop("permission denied for collation %s");
						break;
					case OBJECT_COLUMN:
						msg = gettext_noop("permission denied for column %s");
						break;
					case OBJECT_CONVERSION:
						msg = gettext_noop("permission denied for conversion %s");
						break;
					case OBJECT_DATABASE:
						msg = gettext_noop("permission denied for database %s");
						break;
					case OBJECT_DOMAIN:
						msg = gettext_noop("permission denied for domain %s");
						break;
					case OBJECT_EVENT_TRIGGER:
						msg = gettext_noop("permission denied for event trigger %s");
						break;
					case OBJECT_EXTENSION:
						msg = gettext_noop("permission denied for extension %s");
						break;
					case OBJECT_FUNCTION:
						msg = gettext_noop("permission denied for function %s");
						break;
					case OBJECT_INDEX:
						msg = gettext_noop("permission denied for index %s");
						break;
					case OBJECT_LANGUAGE:
						msg = gettext_noop("permission denied for language %s");
						break;
					case OBJECT_LARGEOBJECT:
						msg = gettext_noop("permission denied for large object %s");
						break;
					case OBJECT_MATVIEW:
						msg = gettext_noop("permission denied for materialized view %s");
						break;
					case OBJECT_OPCLASS:
						msg = gettext_noop("permission denied for operator class %s");
						break;
					case OBJECT_OPERATOR:
						msg = gettext_noop("permission denied for operator %s");
						break;
					case OBJECT_OPFAMILY:
						msg = gettext_noop("permission denied for operator family %s");
						break;
						msg = gettext_noop("permission denied for policy %s");
						break;
					case OBJECT_PROCEDURE:
						msg = gettext_noop("permission denied for procedure %s");
						break;
					case OBJECT_PUBLICATION:
						msg = gettext_noop("permission denied for publication %s");
						break;
					case OBJECT_ROUTINE:
						msg = gettext_noop("permission denied for routine %s");
						break;
					case OBJECT_SCHEMA:
						msg = gettext_noop("permission denied for schema %s");
						break;
					case OBJECT_SEQUENCE:
						msg = gettext_noop("permission denied for sequence %s");
						break;
					case OBJECT_STATISTIC_EXT:
						msg = gettext_noop("permission denied for statistics object %s");
						break;
					case OBJECT_SUBSCRIPTION:
						msg = gettext_noop("permission denied for subscription %s");
						break;
					case OBJECT_TABLE:
						msg = gettext_noop("permission denied for table %s");
						break;
					case OBJECT_TABLESPACE:
						msg = gettext_noop("permission denied for tablespace %s");
						break;
						msg = gettext_noop("permission denied for text search configuration %s");
						break;
						msg = gettext_noop("permission denied for text search dictionary %s");
						break;
					case OBJECT_TYPE:
						msg = gettext_noop("permission denied for type %s");
						break;
					case OBJECT_VIEW:
						msg = gettext_noop("permission denied for view %s");
						break;
						/* these currently aren't used */
					case OBJECT_ACCESS_METHOD:
					case OBJECT_AMOP:
					case OBJECT_AMPROC:
					case OBJECT_ATTRIBUTE:
					case OBJECT_CAST:
					case OBJECT_DEFAULT:
					case OBJECT_DEFACL:
					case OBJECT_DOMCONSTRAINT:
					case OBJECT_PUBLICATION_REL:
					case OBJECT_ROLE:
					case OBJECT_RULE:
					case OBJECT_TABCONSTRAINT:
					case OBJECT_TRANSFORM:
					case OBJECT_TRIGGER:
						elog(ERROR, "unsupported object type %d", objtype);
				}

				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg(msg, objectname)));
				break;
			}
		case ACLCHECK_NOT_OWNER:
			{
				const char *msg = "???";

				switch (objtype)
				{
					case OBJECT_AGGREGATE:
						msg = gettext_noop("must be owner of aggregate %s");
						break;
					case OBJECT_COLLATION:
						msg = gettext_noop("must be owner of collation %s");
						break;
					case OBJECT_CONVERSION:
						msg = gettext_noop("must be owner of conversion %s");
						break;
					case OBJECT_DATABASE:
						msg = gettext_noop("must be owner of database %s");
						break;
					case OBJECT_DOMAIN:
						msg = gettext_noop("must be owner of domain %s");
						break;
					case OBJECT_EVENT_TRIGGER:
						msg = gettext_noop("must be owner of event trigger %s");
						break;
					case OBJECT_EXTENSION:
						msg = gettext_noop("must be owner of extension %s");
						break;
					case OBJECT_FUNCTION:
						msg = gettext_noop("must be owner of function %s");
						break;
					case OBJECT_INDEX:
						msg = gettext_noop("must be owner of index %s");
						break;
					case OBJECT_LANGUAGE:
						msg = gettext_noop("must be owner of language %s");
						break;
					case OBJECT_LARGEOBJECT:
						msg = gettext_noop("must be owner of large object %s");
						break;
					case OBJECT_MATVIEW:
						msg = gettext_noop("must be owner of materialized view %s");
						break;
					case OBJECT_OPCLASS:
						msg = gettext_noop("must be owner of operator class %s");
						break;
					case OBJECT_OPERATOR:
						msg = gettext_noop("must be owner of operator %s");
						break;
					case OBJECT_OPFAMILY:
						msg = gettext_noop("must be owner of operator family %s");
						break;
					case OBJECT_PROCEDURE:
						msg = gettext_noop("must be owner of procedure %s");
						break;
					case OBJECT_PUBLICATION:
						msg = gettext_noop("must be owner of publication %s");
						break;
					case OBJECT_ROUTINE:
						msg = gettext_noop("must be owner of routine %s");
						break;
					case OBJECT_SEQUENCE:
						msg = gettext_noop("must be owner of sequence %s");
						break;
					case OBJECT_SUBSCRIPTION:
						msg = gettext_noop("must be owner of subscription %s");
						break;
					case OBJECT_TABLE:
						msg = gettext_noop("must be owner of table %s");
						break;
					case OBJECT_TYPE:
						msg = gettext_noop("must be owner of type %s");
						break;
					case OBJECT_VIEW:
						msg = gettext_noop("must be owner of view %s");
						break;
					case OBJECT_SCHEMA:
						msg = gettext_noop("must be owner of schema %s");
						break;
					case OBJECT_STATISTIC_EXT:
						msg = gettext_noop("must be owner of statistics object %s");
						break;
					case OBJECT_TABLESPACE:
						msg = gettext_noop("must be owner of tablespace %s");
						break;
						msg = gettext_noop("must be owner of text search configuration %s");
						break;
						msg = gettext_noop("must be owner of text search dictionary %s");
						break;

						/*
						 * Special cases: For these, the error message talks
						 * about "relation", because that's where the
						 * ownership is attached.  See also
						 * check_object_ownership().
						 */
					case OBJECT_COLUMN:
					case OBJECT_RULE:
					case OBJECT_TABCONSTRAINT:
					case OBJECT_TRIGGER:
						msg = gettext_noop("must be owner of relation %s");
						break;
						/* these currently aren't used */
					case OBJECT_ACCESS_METHOD:
					case OBJECT_AMOP:
					case OBJECT_AMPROC:
					case OBJECT_ATTRIBUTE:
					case OBJECT_CAST:
					case OBJECT_DEFAULT:
					case OBJECT_DEFACL:
					case OBJECT_DOMCONSTRAINT:
					case OBJECT_PUBLICATION_REL:
					case OBJECT_ROLE:
					case OBJECT_TRANSFORM:
						elog(ERROR, "unsupported object type %d", objtype);
				}

				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg(msg, objectname)));
				break;
			}
		default:
			elog(ERROR, "unrecognized AclResult: %d", (int) aclerr);
			break;
	}
}


void
aclcheck_error_col(AclResult aclerr, ObjectType objtype,
				   const char *objectname, const char *colname)
{
	switch (aclerr)
	{
		case ACLCHECK_OK:
			/* no error, so return to caller */
			break;
		case ACLCHECK_NO_PRIV:
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied for column \"%s\" of relation \"%s\"",
							colname, objectname)));
			break;
		case ACLCHECK_NOT_OWNER:
			/* relation msg is OK since columns don't have separate owners */
			aclcheck_error(aclerr, objtype, objectname);
			break;
		default:
			elog(ERROR, "unrecognized AclResult: %d", (int) aclerr);
			break;
	}
}


/*
 * Special common handling for types: use element type instead of array type,
 * and format nicely
 */
void
aclcheck_error_type(AclResult aclerr, Oid typeOid)
{
	Oid			element_type = get_element_type(typeOid);

	aclcheck_error(aclerr, OBJECT_TYPE, format_type_be(element_type ? element_type : typeOid));
}


/*
 * Relay for the various pg_*_mask routines depending on object kind
 */
static AclMode
pg_aclmask(ObjectType objtype, Oid table_oid, AttrNumber attnum, Oid roleid,
		   AclMode mask, AclMaskHow how)
{
	switch (objtype)
	{
		case OBJECT_COLUMN:
			return
				pg_class_aclmask(table_oid, roleid, mask, how) |
				pg_attribute_aclmask(table_oid, attnum, roleid, mask, how);
		case OBJECT_TABLE:
		case OBJECT_SEQUENCE:
			return pg_class_aclmask(table_oid, roleid, mask, how);
		case OBJECT_DATABASE:
			return pg_database_aclmask(table_oid, roleid, mask, how);
		case OBJECT_FUNCTION:
			return pg_proc_aclmask(table_oid, roleid, mask, how);
		case OBJECT_LANGUAGE:
			return pg_language_aclmask(table_oid, roleid, mask, how);
		case OBJECT_LARGEOBJECT:
			return pg_largeobject_aclmask_snapshot(table_oid, roleid,
												   mask, how, NULL);
		case OBJECT_SCHEMA:
			return pg_namespace_aclmask(table_oid, roleid, mask, how);
		case OBJECT_STATISTIC_EXT:
			elog(ERROR, "grantable rights not supported for statistics objects");
			/* not reached, but keep compiler quiet */
			return ACL_NO_RIGHTS;
		case OBJECT_TABLESPACE:
			return pg_tablespace_aclmask(table_oid, roleid, mask, how);
		case OBJECT_EVENT_TRIGGER:
			elog(ERROR, "grantable rights not supported for event triggers");
			/* not reached, but keep compiler quiet */
			return ACL_NO_RIGHTS;
		case OBJECT_TYPE:
			return pg_type_aclmask(table_oid, roleid, mask, how);
		default:
			elog(ERROR, "unrecognized objtype: %d",
				 (int) objtype);
			/* not reached, but keep compiler quiet */
			return ACL_NO_RIGHTS;
	}
}


/* ****************************************************************
 * Exported routines for examining a user's privileges for various objects
 *
 * See aclmask() for a description of the common API for these functions.
 *
 * Note: we give lookup failure the full ereport treatment because the
 * has_xxx_privilege() family of functions allow users to pass any random
 * OID to these functions.
 * ****************************************************************
 */

/*
 * Exported routine for examining a user's privileges for a column
 *
 * Note: this considers only privileges granted specifically on the column.
 * It is caller's responsibility to take relation-level privileges into account
 * as appropriate.  (For the same reason, we have no special case for
 * superuser-ness here.)
 */
AclMode
pg_attribute_aclmask(Oid table_oid, AttrNumber attnum, Oid roleid,
					 AclMode mask, AclMaskHow how)
{
	return pg_attribute_aclmask_ext(table_oid, attnum, roleid,
									mask, how, NULL);
}

/*
 * Exported routine for examining a user's privileges for a column
 *
 * Does the bulk of the work for pg_attribute_aclmask(), and allows other
 * callers to avoid the missing attribute ERROR when is_missing is non-NULL.
 */
AclMode
pg_attribute_aclmask_ext(Oid table_oid, AttrNumber attnum, Oid roleid,
						 AclMode mask, AclMaskHow how, bool *is_missing)
{
	AclMode		result;
	HeapTuple	classTuple;
	HeapTuple	attTuple;
	Form_pg_class classForm;
	Form_pg_attribute attributeForm;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/*
	 * First, get the column's ACL from its pg_attribute entry
	 */
	attTuple = SearchSysCache2(ATTNUM,
							   ObjectIdGetDatum(table_oid),
							   Int16GetDatum(attnum));
	if (!HeapTupleIsValid(attTuple))
	{
		if (is_missing != NULL)
		{
			/* return "no privileges" instead of throwing an error */
			*is_missing = true;
			return 0;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("attribute %d of relation with OID %u does not exist",
							attnum, table_oid)));
	}

	attributeForm = (Form_pg_attribute) GETSTRUCT(attTuple);

	/* Check dropped columns, too */
	if (attributeForm->attisdropped)
	{
		if (is_missing != NULL)
		{
			/* return "no privileges" instead of throwing an error */
			*is_missing = true;
			ReleaseSysCache(attTuple);
			return 0;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("attribute %d of relation with OID %u does not exist",
							attnum, table_oid)));
	}

	aclDatum = SysCacheGetAttr(ATTNUM, attTuple, Anum_pg_attribute_attacl,
							   &isNull);

	/*
	 * Here we hard-wire knowledge that the default ACL for a column grants no
	 * privileges, so that we can fall out quickly in the very common case
	 * where attacl is null.
	 */
	if (isNull)
	{
		ReleaseSysCache(attTuple);
		return 0;
	}

	/*
	 * Must get the relation's ownerId from pg_class.  Since we already found
	 * a pg_attribute entry, the only likely reason for this to fail is that a
	 * concurrent DROP of the relation committed since then (which could only
	 * happen if we don't have lock on the relation).  We prefer to report "no
	 * privileges" rather than failing in such a case, so as to avoid unwanted
	 * failures in has_column_privilege() tests.
	 */
	classTuple = SearchSysCache1(RELOID, ObjectIdGetDatum(table_oid));
	if (!HeapTupleIsValid(classTuple))
	{
		ReleaseSysCache(attTuple);
		return 0;
	}
	classForm = (Form_pg_class) GETSTRUCT(classTuple);

	ownerId = classForm->relowner;

	ReleaseSysCache(classTuple);

	/* detoast column's ACL if necessary */
	acl = DatumGetAclP(aclDatum);

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(attTuple);

	return result;
}

/*
 * Exported routine for examining a user's privileges for a table
 */
AclMode
pg_class_aclmask(Oid table_oid, Oid roleid,
				 AclMode mask, AclMaskHow how)
{
	return pg_class_aclmask_ext(table_oid, roleid, mask, how, NULL);
}

/*
 * Exported routine for examining a user's privileges for a table
 *
 * Does the bulk of the work for pg_class_aclmask(), and allows other
 * callers to avoid the missing relation ERROR when is_missing is non-NULL.
 */
AclMode
pg_class_aclmask_ext(Oid table_oid, Oid roleid, AclMode mask,
					 AclMaskHow how, bool *is_missing)
{
	AclMode		result;
	HeapTuple	tuple;
	Form_pg_class classForm;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/*
	 * Must get the relation's tuple from pg_class
	 */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(table_oid));
	if (!HeapTupleIsValid(tuple))
	{
		if (is_missing != NULL)
		{
			/* return "no privileges" instead of throwing an error */
			*is_missing = true;
			return 0;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation with OID %u does not exist",
							table_oid)));
	}

	classForm = (Form_pg_class) GETSTRUCT(tuple);

	/*
	 * Deny anyone permission to update a system catalog unless
	 * pg_authid.rolsuper is set.
	 *
	 * As of 7.4 we have some updatable system views; those shouldn't be
	 * protected in this way.  Assume the view rules can take care of
	 * themselves.  ACL_USAGE is if we ever have system sequences.
	 */
	if ((mask & (ACL_INSERT | ACL_UPDATE | ACL_DELETE | ACL_TRUNCATE | ACL_USAGE)) &&
		IsSystemClass(table_oid, classForm) &&
		classForm->relkind != RELKIND_VIEW &&
		!superuser_arg(roleid))
		mask &= ~(ACL_INSERT | ACL_UPDATE | ACL_DELETE | ACL_TRUNCATE | ACL_USAGE);

	/*
	 * Otherwise, superusers bypass all permission-checking.
	 */
	if (superuser_arg(roleid))
	{
		ReleaseSysCache(tuple);
		return mask;
	}

	/*
	 * Normal case: get the relation's ACL from pg_class
	 */
	ownerId = classForm->relowner;

	aclDatum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relacl,
							   &isNull);
	if (isNull)
	{
		/* No ACL, so build default ACL */
		switch (classForm->relkind)
		{
			case RELKIND_SEQUENCE:
				acl = acldefault(OBJECT_SEQUENCE, ownerId);
				break;
			default:
				acl = acldefault(OBJECT_TABLE, ownerId);
				break;
		}
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast rel's ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	/*
	 * Check if ACL_SELECT is being checked and, if so, and not set already as
	 * part of the result, then check if the user is a member of the
	 * pg_read_all_data role, which allows read access to all relations.
	 */
	if (mask & ACL_SELECT && !(result & ACL_SELECT) &&
		has_privs_of_role(roleid, ROLE_PG_READ_ALL_DATA))
		result |= ACL_SELECT;

	/*
	 * Check if ACL_INSERT, ACL_UPDATE, or ACL_DELETE is being checked and, if
	 * so, and not set already as part of the result, then check if the user
	 * is a member of the pg_write_all_data role, which allows
	 * INSERT/UPDATE/DELETE access to all relations (except system catalogs,
	 * which requires superuser, see above).
	 */
	if (mask & (ACL_INSERT | ACL_UPDATE | ACL_DELETE) &&
		!(result & (ACL_INSERT | ACL_UPDATE | ACL_DELETE)) &&
		has_privs_of_role(roleid, ROLE_PG_WRITE_ALL_DATA))
		result |= (mask & (ACL_INSERT | ACL_UPDATE | ACL_DELETE));

	return result;
}

/*
 * Exported routine for examining a user's privileges for a database
 */
AclMode
pg_database_aclmask(Oid db_oid, Oid roleid,
					AclMode mask, AclMaskHow how)
{
	AclMode		result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return mask;

	/*
	 * Get the database's ACL from pg_database
	 */
	tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(db_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database with OID %u does not exist", db_oid)));

	ownerId = ((Form_pg_database) GETSTRUCT(tuple))->datdba;

	aclDatum = SysCacheGetAttr(DATABASEOID, tuple, Anum_pg_database_datacl,
							   &isNull);
	if (isNull)
	{
		/* No ACL, so build default ACL */
		acl = acldefault(OBJECT_DATABASE, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}

/*
 * Exported routine for examining a user's privileges for a function
 */
AclMode
pg_proc_aclmask(Oid proc_oid, Oid roleid,
				AclMode mask, AclMaskHow how)
{
	AclMode		result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return mask;

	/*
	 * Get the function's ACL from pg_proc
	 */
	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(proc_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function with OID %u does not exist", proc_oid)));

	ownerId = ((Form_pg_proc) GETSTRUCT(tuple))->proowner;

	aclDatum = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_proacl,
							   &isNull);
	if (isNull)
	{
		/* No ACL, so build default ACL */
		acl = acldefault(OBJECT_FUNCTION, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}

/*
 * Exported routine for examining a user's privileges for a language
 */
AclMode
pg_language_aclmask(Oid lang_oid, Oid roleid,
					AclMode mask, AclMaskHow how)
{
	AclMode		result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return mask;

	/*
	 * Get the language's ACL from pg_language
	 */
	tuple = SearchSysCache1(LANGOID, ObjectIdGetDatum(lang_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("language with OID %u does not exist", lang_oid)));

	ownerId = ((Form_pg_language) GETSTRUCT(tuple))->lanowner;

	aclDatum = SysCacheGetAttr(LANGOID, tuple, Anum_pg_language_lanacl,
							   &isNull);
	if (isNull)
	{
		/* No ACL, so build default ACL */
		acl = acldefault(OBJECT_LANGUAGE, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}

/*
 * Exported routine for examining a user's privileges for a largeobject
 *
 * When a large object is opened for reading, it is opened relative to the
 * caller's snapshot, but when it is opened for writing, a current
 * MVCC snapshot will be used.  See doc/src/sgml/lobj.sgml.  This function
 * takes a snapshot argument so that the permissions check can be made
 * relative to the same snapshot that will be used to read the underlying
 * data.  The caller will actually pass NULL for an instantaneous MVCC
 * snapshot, since all we do with the snapshot argument is pass it through
 * to systable_beginscan().
 */
AclMode
pg_largeobject_aclmask_snapshot(Oid lobj_oid, Oid roleid,
								AclMode mask, AclMaskHow how,
								Snapshot snapshot)
{
	AclMode		result;
	Relation	pg_lo_meta;
	ScanKeyData entry[1];
	SysScanDesc scan;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return mask;

	/*
	 * Get the largeobject's ACL from pg_largeobject_metadata
	 */
	pg_lo_meta = table_open(LargeObjectMetadataRelationId,
							AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_largeobject_metadata_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(lobj_oid));

	scan = systable_beginscan(pg_lo_meta,
							  LargeObjectMetadataOidIndexId, true,
							  snapshot, 1, entry);

	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("large object %u does not exist", lobj_oid)));

	ownerId = ((Form_pg_largeobject_metadata) GETSTRUCT(tuple))->lomowner;

	aclDatum = heap_getattr(tuple, Anum_pg_largeobject_metadata_lomacl,
							RelationGetDescr(pg_lo_meta), &isNull);

	if (isNull)
	{
		/* No ACL, so build default ACL */
		acl = acldefault(OBJECT_LARGEOBJECT, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	systable_endscan(scan);

	table_close(pg_lo_meta, AccessShareLock);

	return result;
}

/*
 * Exported routine for examining a user's privileges for a namespace
 */
AclMode
pg_namespace_aclmask(Oid nsp_oid, Oid roleid,
					 AclMode mask, AclMaskHow how)
{
	AclMode		result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return mask;

	/*
	 * If we have been assigned this namespace as a temp namespace, check to
	 * make sure we have CREATE TEMP permission on the database, and if so act
	 * as though we have all standard (but not GRANT OPTION) permissions on
	 * the namespace.  If we don't have CREATE TEMP, act as though we have
	 * only USAGE (and not CREATE) rights.
	 *
	 * This may seem redundant given the check in InitTempTableNamespace, but
	 * it really isn't since current user ID may have changed since then. The
	 * upshot of this behavior is that a SECURITY DEFINER function can create
	 * temp tables that can then be accessed (if permission is granted) by
	 * code in the same session that doesn't have permissions to create temp
	 * tables.
	 *
	 * XXX Would it be safe to ereport a special error message as
	 * InitTempTableNamespace does?  Returning zero here means we'll get a
	 * generic "permission denied for schema pg_temp_N" message, which is not
	 * remarkably user-friendly.
	 */
	if (isTempNamespace(nsp_oid))
	{
		if (pg_database_aclcheck(MyDatabaseId, roleid,
								 ACL_CREATE_TEMP) == ACLCHECK_OK)
			return mask & ACL_ALL_RIGHTS_SCHEMA;
		else
			return mask & ACL_USAGE;
	}

	/*
	 * Get the schema's ACL from pg_namespace
	 */
	tuple = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(nsp_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema with OID %u does not exist", nsp_oid)));

	ownerId = ((Form_pg_namespace) GETSTRUCT(tuple))->nspowner;

	aclDatum = SysCacheGetAttr(NAMESPACEOID, tuple, Anum_pg_namespace_nspacl,
							   &isNull);
	if (isNull)
	{
		/* No ACL, so build default ACL */
		acl = acldefault(OBJECT_SCHEMA, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	/*
	 * Check if ACL_USAGE is being checked and, if so, and not set already as
	 * part of the result, then check if the user is a member of the
	 * pg_read_all_data or pg_write_all_data roles, which allow usage access
	 * to all schemas.
	 */
	if (mask & ACL_USAGE && !(result & ACL_USAGE) &&
		(has_privs_of_role(roleid, ROLE_PG_READ_ALL_DATA) ||
		 has_privs_of_role(roleid, ROLE_PG_WRITE_ALL_DATA)))
		result |= ACL_USAGE;
	return result;
}

/*
 * Exported routine for examining a user's privileges for a tablespace
 */
AclMode
pg_tablespace_aclmask(Oid spc_oid, Oid roleid,
					  AclMode mask, AclMaskHow how)
{
	AclMode		result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return mask;

	/*
	 * Get the tablespace's ACL from pg_tablespace
	 */
	tuple = SearchSysCache1(TABLESPACEOID, ObjectIdGetDatum(spc_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace with OID %u does not exist", spc_oid)));

	ownerId = ((Form_pg_tablespace) GETSTRUCT(tuple))->spcowner;

	aclDatum = SysCacheGetAttr(TABLESPACEOID, tuple,
							   Anum_pg_tablespace_spcacl,
							   &isNull);

	if (isNull)
	{
		/* No ACL, so build default ACL */
		acl = acldefault(OBJECT_TABLESPACE, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}

/*
 * Exported routine for examining a user's privileges for a type.
 */
AclMode
pg_type_aclmask(Oid type_oid, Oid roleid, AclMode mask, AclMaskHow how)
{
	AclMode		result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	Form_pg_type typeForm;

	/* Bypass permission checks for superusers */
	if (superuser_arg(roleid))
		return mask;

	/*
	 * Must get the type's tuple from pg_type
	 */
	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type with OID %u does not exist",
						type_oid)));
	typeForm = (Form_pg_type) GETSTRUCT(tuple);

	/*
	 * "True" array types don't manage permissions of their own; consult the
	 * element type instead.
	 */
	if (IsTrueArrayType(typeForm))
	{
		Oid			elttype_oid = typeForm->typelem;

		ReleaseSysCache(tuple);

		tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(elttype_oid));
		/* this case is not a user-facing error, so elog not ereport */
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for type %u", elttype_oid);
		typeForm = (Form_pg_type) GETSTRUCT(tuple);
	}

	/*
	 * Now get the type's owner and ACL from the tuple
	 */
	ownerId = typeForm->typowner;

	aclDatum = SysCacheGetAttr(TYPEOID, tuple,
							   Anum_pg_type_typacl, &isNull);
	if (isNull)
	{
		/* No ACL, so build default ACL */
		acl = acldefault(OBJECT_TYPE, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast rel's ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}

/*
 * Exported routine for checking a user's access privileges to a column
 *
 * Returns ACLCHECK_OK if the user has any of the privileges identified by
 * 'mode'; otherwise returns a suitable error code (in practice, always
 * ACLCHECK_NO_PRIV).
 *
 * As with pg_attribute_aclmask, only privileges granted directly on the
 * column are considered here.
 */
AclResult
pg_attribute_aclcheck(Oid table_oid, AttrNumber attnum,
					  Oid roleid, AclMode mode)
{
	return pg_attribute_aclcheck_ext(table_oid, attnum, roleid, mode, NULL);
}


/*
 * Exported routine for checking a user's access privileges to a column
 *
 * Does the bulk of the work for pg_attribute_aclcheck(), and allows other
 * callers to avoid the missing attribute ERROR when is_missing is non-NULL.
 */
AclResult
pg_attribute_aclcheck_ext(Oid table_oid, AttrNumber attnum,
						  Oid roleid, AclMode mode, bool *is_missing)
{
	if (pg_attribute_aclmask_ext(table_oid, attnum, roleid, mode,
								 ACLMASK_ANY, is_missing) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to any/all columns
 *
 * If 'how' is ACLMASK_ANY, then returns ACLCHECK_OK if user has any of the
 * privileges identified by 'mode' on any non-dropped column in the relation;
 * otherwise returns a suitable error code (in practice, always
 * ACLCHECK_NO_PRIV).
 *
 * If 'how' is ACLMASK_ALL, then returns ACLCHECK_OK if user has any of the
 * privileges identified by 'mode' on each non-dropped column in the relation
 * (and there must be at least one such column); otherwise returns a suitable
 * error code (in practice, always ACLCHECK_NO_PRIV).
 *
 * As with pg_attribute_aclmask, only privileges granted directly on the
 * column(s) are considered here.
 *
 * Note: system columns are not considered here; there are cases where that
 * might be appropriate but there are also cases where it wouldn't.
 */
AclResult
pg_attribute_aclcheck_all(Oid table_oid, Oid roleid, AclMode mode,
						  AclMaskHow how)
{
	AclResult	result;
	HeapTuple	classTuple;
	Form_pg_class classForm;
	AttrNumber	nattrs;
	AttrNumber	curr_att;

	/*
	 * Must fetch pg_class row to check number of attributes.  As in
	 * pg_attribute_aclmask, we prefer to return "no privileges" instead of
	 * throwing an error if we get any unexpected lookup errors.
	 */
	classTuple = SearchSysCache1(RELOID, ObjectIdGetDatum(table_oid));
	if (!HeapTupleIsValid(classTuple))
		return ACLCHECK_NO_PRIV;
	classForm = (Form_pg_class) GETSTRUCT(classTuple);

	nattrs = classForm->relnatts;

	ReleaseSysCache(classTuple);

	/*
	 * Initialize result in case there are no non-dropped columns.  We want to
	 * report failure in such cases for either value of 'how'.
	 */
	result = ACLCHECK_NO_PRIV;

	for (curr_att = 1; curr_att <= nattrs; curr_att++)
	{
		HeapTuple	attTuple;
		AclMode		attmask;

		attTuple = SearchSysCache2(ATTNUM,
								   ObjectIdGetDatum(table_oid),
								   Int16GetDatum(curr_att));
		if (!HeapTupleIsValid(attTuple))
			continue;

		/* ignore dropped columns */
		if (((Form_pg_attribute) GETSTRUCT(attTuple))->attisdropped)
		{
			ReleaseSysCache(attTuple);
			continue;
		}

		/*
		 * Here we hard-wire knowledge that the default ACL for a column
		 * grants no privileges, so that we can fall out quickly in the very
		 * common case where attacl is null.
		 */
		if (heap_attisnull(attTuple, Anum_pg_attribute_attacl, NULL))
			attmask = 0;
		else
			attmask = pg_attribute_aclmask(table_oid, curr_att, roleid,
										   mode, ACLMASK_ANY);

		ReleaseSysCache(attTuple);

		if (attmask != 0)
		{
			result = ACLCHECK_OK;
			if (how == ACLMASK_ANY)
				break;			/* succeed on any success */
		}
		else
		{
			result = ACLCHECK_NO_PRIV;
			if (how == ACLMASK_ALL)
				break;			/* fail on any failure */
		}
	}

	return result;
}

/*
 * Exported routine for checking a user's access privileges to a table
 *
 * Returns ACLCHECK_OK if the user has any of the privileges identified by
 * 'mode'; otherwise returns a suitable error code (in practice, always
 * ACLCHECK_NO_PRIV).
 */
AclResult
pg_class_aclcheck(Oid table_oid, Oid roleid, AclMode mode)
{
	return pg_class_aclcheck_ext(table_oid, roleid, mode, NULL);
}

/*
 * Exported routine for checking a user's access privileges to a table
 *
 * Does the bulk of the work for pg_class_aclcheck(), and allows other
 * callers to avoid the missing relation ERROR when is_missing is non-NULL.
 */
AclResult
pg_class_aclcheck_ext(Oid table_oid, Oid roleid,
					  AclMode mode, bool *is_missing)
{
	if (pg_class_aclmask_ext(table_oid, roleid, mode,
							 ACLMASK_ANY, is_missing) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to a database
 */
AclResult
pg_database_aclcheck(Oid db_oid, Oid roleid, AclMode mode)
{
	if (pg_database_aclmask(db_oid, roleid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to a function
 */
AclResult
pg_proc_aclcheck(Oid proc_oid, Oid roleid, AclMode mode)
{
	if (pg_proc_aclmask(proc_oid, roleid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to a language
 */
AclResult
pg_language_aclcheck(Oid lang_oid, Oid roleid, AclMode mode)
{
	if (pg_language_aclmask(lang_oid, roleid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to a largeobject
 */
AclResult
pg_largeobject_aclcheck_snapshot(Oid lobj_oid, Oid roleid, AclMode mode,
								 Snapshot snapshot)
{
	if (pg_largeobject_aclmask_snapshot(lobj_oid, roleid, mode,
										ACLMASK_ANY, snapshot) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to a namespace
 */
AclResult
pg_namespace_aclcheck(Oid nsp_oid, Oid roleid, AclMode mode)
{
	if (pg_namespace_aclmask(nsp_oid, roleid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to a tablespace
 */
AclResult
pg_tablespace_aclcheck(Oid spc_oid, Oid roleid, AclMode mode)
{
	if (pg_tablespace_aclmask(spc_oid, roleid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}


/*
 * Exported routine for checking a user's access privileges to a type
 */
AclResult
pg_type_aclcheck(Oid type_oid, Oid roleid, AclMode mode)
{
	if (pg_type_aclmask(type_oid, roleid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Ownership check for a relation (specified by OID).
 */
bool
pg_class_ownercheck(Oid class_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(class_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation with OID %u does not exist", class_oid)));

	ownerId = ((Form_pg_class) GETSTRUCT(tuple))->relowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for a type (specified by OID).
 */
bool
pg_type_ownercheck(Oid type_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type with OID %u does not exist", type_oid)));

	ownerId = ((Form_pg_type) GETSTRUCT(tuple))->typowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for an operator (specified by OID).
 */
bool
pg_oper_ownercheck(Oid oper_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(oper_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("operator with OID %u does not exist", oper_oid)));

	ownerId = ((Form_pg_operator) GETSTRUCT(tuple))->oprowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for a function (specified by OID).
 */
bool
pg_proc_ownercheck(Oid proc_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(proc_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function with OID %u does not exist", proc_oid)));

	ownerId = ((Form_pg_proc) GETSTRUCT(tuple))->proowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for a procedural language (specified by OID)
 */
bool
pg_language_ownercheck(Oid lan_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache1(LANGOID, ObjectIdGetDatum(lan_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("language with OID %u does not exist", lan_oid)));

	ownerId = ((Form_pg_language) GETSTRUCT(tuple))->lanowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for a largeobject (specified by OID)
 *
 * This is only used for operations like ALTER LARGE OBJECT that are always
 * relative to an up-to-date snapshot.
 */
bool
pg_largeobject_ownercheck(Oid lobj_oid, Oid roleid)
{
	Relation	pg_lo_meta;
	ScanKeyData entry[1];
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	/* There's no syscache for pg_largeobject_metadata */
	pg_lo_meta = table_open(LargeObjectMetadataRelationId,
							AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_largeobject_metadata_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(lobj_oid));

	scan = systable_beginscan(pg_lo_meta,
							  LargeObjectMetadataOidIndexId, true,
							  NULL, 1, entry);

	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("large object %u does not exist", lobj_oid)));

	ownerId = ((Form_pg_largeobject_metadata) GETSTRUCT(tuple))->lomowner;

	systable_endscan(scan);
	table_close(pg_lo_meta, AccessShareLock);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for a namespace (specified by OID).
 */
bool
pg_namespace_ownercheck(Oid nsp_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(nsp_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema with OID %u does not exist", nsp_oid)));

	ownerId = ((Form_pg_namespace) GETSTRUCT(tuple))->nspowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for a tablespace (specified by OID).
 */
bool
pg_tablespace_ownercheck(Oid spc_oid, Oid roleid)
{
	HeapTuple	spctuple;
	Oid			spcowner;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	/* Search syscache for pg_tablespace */
	spctuple = SearchSysCache1(TABLESPACEOID, ObjectIdGetDatum(spc_oid));
	if (!HeapTupleIsValid(spctuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace with OID %u does not exist", spc_oid)));

	spcowner = ((Form_pg_tablespace) GETSTRUCT(spctuple))->spcowner;

	ReleaseSysCache(spctuple);

	return has_privs_of_role(roleid, spcowner);
}

/*
 * Ownership check for an operator class (specified by OID).
 */
bool
pg_opclass_ownercheck(Oid opc_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache1(CLAOID, ObjectIdGetDatum(opc_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator class with OID %u does not exist",
						opc_oid)));

	ownerId = ((Form_pg_opclass) GETSTRUCT(tuple))->opcowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for an operator family (specified by OID).
 */
bool
pg_opfamily_ownercheck(Oid opf_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opf_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator family with OID %u does not exist",
						opf_oid)));

	ownerId = ((Form_pg_opfamily) GETSTRUCT(tuple))->opfowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for an event trigger (specified by OID).
 */
bool
pg_event_trigger_ownercheck(Oid et_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache1(EVENTTRIGGEROID, ObjectIdGetDatum(et_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger with OID %u does not exist",
						et_oid)));

	ownerId = ((Form_pg_event_trigger) GETSTRUCT(tuple))->evtowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for a database (specified by OID).
 */
bool
pg_database_ownercheck(Oid db_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			dba;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(db_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database with OID %u does not exist", db_oid)));

	dba = ((Form_pg_database) GETSTRUCT(tuple))->datdba;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, dba);
}

/*
 * Ownership check for a collation (specified by OID).
 */
bool
pg_collation_ownercheck(Oid coll_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache1(COLLOID, ObjectIdGetDatum(coll_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("collation with OID %u does not exist", coll_oid)));

	ownerId = ((Form_pg_collation) GETSTRUCT(tuple))->collowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for a conversion (specified by OID).
 */
bool
pg_conversion_ownercheck(Oid conv_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache1(CONVOID, ObjectIdGetDatum(conv_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("conversion with OID %u does not exist", conv_oid)));

	ownerId = ((Form_pg_conversion) GETSTRUCT(tuple))->conowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for an extension (specified by OID).
 */
bool
pg_extension_ownercheck(Oid ext_oid, Oid roleid)
{
	Relation	pg_extension;
	ScanKeyData entry[1];
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	/* There's no syscache for pg_extension, so do it the hard way */
	pg_extension = table_open(ExtensionRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ext_oid));

	scan = systable_beginscan(pg_extension,
							  ExtensionOidIndexId, true,
							  NULL, 1, entry);

	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("extension with OID %u does not exist", ext_oid)));

	ownerId = ((Form_pg_extension) GETSTRUCT(tuple))->extowner;

	systable_endscan(scan);
	table_close(pg_extension, AccessShareLock);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for a publication (specified by OID).
 */
bool
pg_publication_ownercheck(Oid pub_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache1(PUBLICATIONOID, ObjectIdGetDatum(pub_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("publication with OID %u does not exist", pub_oid)));

	ownerId = ((Form_pg_publication) GETSTRUCT(tuple))->pubowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for a subscription (specified by OID).
 */
bool
pg_subscription_ownercheck(Oid sub_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache1(SUBSCRIPTIONOID, ObjectIdGetDatum(sub_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("subscription with OID %u does not exist", sub_oid)));

	ownerId = ((Form_pg_subscription) GETSTRUCT(tuple))->subowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for a statistics object (specified by OID).
 */
bool
pg_statistics_object_ownercheck(Oid stat_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(stat_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("statistics object with OID %u does not exist",
						stat_oid)));

	ownerId = ((Form_pg_statistic_ext) GETSTRUCT(tuple))->stxowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Check whether specified role has CREATEROLE privilege (or is a superuser)
 *
 * Note: roles do not have owners per se; instead we use this test in
 * places where an ownership-like permissions test is needed for a role.
 * Be sure to apply it to the role trying to do the operation, not the
 * role being operated on!	Also note that this generally should not be
 * considered enough privilege if the target role is a superuser.
 * (We don't handle that consideration here because we want to give a
 * separate error message for such cases, so the caller has to deal with it.)
 */
bool
has_createrole_privilege(Oid roleid)
{
	bool		result = false;
	HeapTuple	utup;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	utup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (HeapTupleIsValid(utup))
	{
		result = ((Form_pg_authid) GETSTRUCT(utup))->rolcreaterole;
		ReleaseSysCache(utup);
	}
	return result;
}

bool
has_bypassrls_privilege(Oid roleid)
{
	bool		result = false;
	HeapTuple	utup;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	utup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (HeapTupleIsValid(utup))
	{
		result = ((Form_pg_authid) GETSTRUCT(utup))->rolbypassrls;
		ReleaseSysCache(utup);
	}
	return result;
}

/*
 * Fetch pg_default_acl entry for given role, namespace and object type
 * (object type must be given in pg_default_acl's encoding).
 * Returns NULL if no such entry.
 */
static Acl *
get_default_acl_internal(Oid roleId, Oid nsp_oid, char objtype)
{
	Acl		   *result = NULL;
	HeapTuple	tuple;

	tuple = SearchSysCache3(DEFACLROLENSPOBJ,
							ObjectIdGetDatum(roleId),
							ObjectIdGetDatum(nsp_oid),
							CharGetDatum(objtype));

	if (HeapTupleIsValid(tuple))
	{
		Datum		aclDatum;
		bool		isNull;

		aclDatum = SysCacheGetAttr(DEFACLROLENSPOBJ, tuple,
								   Anum_pg_default_acl_defaclacl,
								   &isNull);
		if (!isNull)
			result = DatumGetAclPCopy(aclDatum);
		ReleaseSysCache(tuple);
	}

	return result;
}

/*
 * Get default permissions for newly created object within given schema
 *
 * Returns NULL if built-in system defaults should be used.
 *
 * If the result is not NULL, caller must call recordDependencyOnNewAcl
 * once the OID of the new object is known.
 */
Acl *
get_user_default_acl(ObjectType objtype, Oid ownerId, Oid nsp_oid)
{
	Acl		   *result;
	Acl		   *glob_acl;
	Acl		   *schema_acl;
	Acl		   *def_acl;
	char		defaclobjtype;

	/*
	 * Use NULL during bootstrap, since pg_default_acl probably isn't there
	 * yet.
	 */
	if (IsBootstrapProcessingMode())
		return NULL;

	/* Check if object type is supported in pg_default_acl */
	switch (objtype)
	{
		case OBJECT_TABLE:
			defaclobjtype = DEFACLOBJ_RELATION;
			break;

		case OBJECT_SEQUENCE:
			defaclobjtype = DEFACLOBJ_SEQUENCE;
			break;

		case OBJECT_FUNCTION:
			defaclobjtype = DEFACLOBJ_FUNCTION;
			break;

		case OBJECT_TYPE:
			defaclobjtype = DEFACLOBJ_TYPE;
			break;

		case OBJECT_SCHEMA:
			defaclobjtype = DEFACLOBJ_NAMESPACE;
			break;

		default:
			return NULL;
	}

	/* Look up the relevant pg_default_acl entries */
	glob_acl = get_default_acl_internal(ownerId, InvalidOid, defaclobjtype);
	schema_acl = get_default_acl_internal(ownerId, nsp_oid, defaclobjtype);

	/* Quick out if neither entry exists */
	if (glob_acl == NULL && schema_acl == NULL)
		return NULL;

	/* We need to know the hard-wired default value, too */
	def_acl = acldefault(objtype, ownerId);

	/* If there's no global entry, substitute the hard-wired default */
	if (glob_acl == NULL)
		glob_acl = def_acl;

	/* Merge in any per-schema privileges */
	result = aclmerge(glob_acl, schema_acl, ownerId);

	/*
	 * For efficiency, we want to return NULL if the result equals default.
	 * This requires sorting both arrays to get an accurate comparison.
	 */
	aclitemsort(result);
	aclitemsort(def_acl);
	if (aclequal(result, def_acl))
		result = NULL;

	return result;
}

/*
 * Record dependencies on roles mentioned in a new object's ACL.
 */
void
recordDependencyOnNewAcl(Oid classId, Oid objectId, int32 objsubId,
						 Oid ownerId, Acl *acl)
{
	int			nmembers;
	Oid		   *members;

	/* Nothing to do if ACL is defaulted */
	if (acl == NULL)
		return;

	/* Extract roles mentioned in ACL */
	nmembers = aclmembers(acl, &members);

	/* Update the shared dependency ACL info */
	updateAclDependencies(classId, objectId, objsubId,
						  ownerId,
						  0, NULL,
						  nmembers, members);
}

/*
 * Record initial privileges for the top-level object passed in.
 *
 * For the object passed in, this will record its ACL (if any) and the ACLs of
 * any sub-objects (eg: columns) into pg_init_privs.
 *
 * Any new kinds of objects which have ACLs associated with them and can be
 * added to an extension should be added to the if-else tree below.
 */
void
recordExtObjInitPriv(Oid objoid, Oid classoid)
{
	/*
	 * pg_class / pg_attribute
	 *
	 * If this is a relation then we need to see if there are any sub-objects
	 * (eg: columns) for it and, if so, be sure to call
	 * recordExtensionInitPrivWorker() for each one.
	 */
	if (classoid == RelationRelationId)
	{
		Form_pg_class pg_class_tuple;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	tuple;

		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(objoid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", objoid);
		pg_class_tuple = (Form_pg_class) GETSTRUCT(tuple);

		/*
		 * Indexes don't have permissions, neither do the pg_class rows for
		 * composite types.  (These cases are unreachable given the
		 * restrictions in ALTER EXTENSION ADD, but let's check anyway.)
		 */
		if (pg_class_tuple->relkind == RELKIND_INDEX ||
			pg_class_tuple->relkind == RELKIND_PARTITIONED_INDEX ||
			pg_class_tuple->relkind == RELKIND_COMPOSITE_TYPE)
		{
			ReleaseSysCache(tuple);
			return;
		}

		/*
		 * If this isn't a sequence then it's possibly going to have
		 * column-level ACLs associated with it.
		 */
		if (pg_class_tuple->relkind != RELKIND_SEQUENCE)
		{
			AttrNumber	curr_att;
			AttrNumber	nattrs = pg_class_tuple->relnatts;

			for (curr_att = 1; curr_att <= nattrs; curr_att++)
			{
				HeapTuple	attTuple;
				Datum		attaclDatum;

				attTuple = SearchSysCache2(ATTNUM,
										   ObjectIdGetDatum(objoid),
										   Int16GetDatum(curr_att));

				if (!HeapTupleIsValid(attTuple))
					continue;

				/* ignore dropped columns */
				if (((Form_pg_attribute) GETSTRUCT(attTuple))->attisdropped)
				{
					ReleaseSysCache(attTuple);
					continue;
				}

				attaclDatum = SysCacheGetAttr(ATTNUM, attTuple,
											  Anum_pg_attribute_attacl,
											  &isNull);

				/* no need to do anything for a NULL ACL */
				if (isNull)
				{
					ReleaseSysCache(attTuple);
					continue;
				}

				recordExtensionInitPrivWorker(objoid, classoid, curr_att,
											  DatumGetAclP(attaclDatum));

				ReleaseSysCache(attTuple);
			}
		}

		aclDatum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relacl,
								   &isNull);

		/* Add the record, if any, for the top-level object */
		if (!isNull)
			recordExtensionInitPrivWorker(objoid, classoid, 0,
										  DatumGetAclP(aclDatum));

		ReleaseSysCache(tuple);
	}
	/* pg_language */
	else if (classoid == LanguageRelationId)
	{
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	tuple;

		tuple = SearchSysCache1(LANGOID, ObjectIdGetDatum(objoid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for language %u", objoid);

		aclDatum = SysCacheGetAttr(LANGOID, tuple, Anum_pg_language_lanacl,
								   &isNull);

		/* Add the record, if any, for the top-level object */
		if (!isNull)
			recordExtensionInitPrivWorker(objoid, classoid, 0,
										  DatumGetAclP(aclDatum));

		ReleaseSysCache(tuple);
	}
	else if (classoid == LargeObjectRelationId)
	{
		/* For large objects, we must consult pg_largeobject_metadata */
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	tuple;
		ScanKeyData entry[1];
		SysScanDesc scan;
		Relation	relation;

		/*
		 * Note: this is dead code, given that we don't allow large objects to
		 * be made extension members.  But it seems worth carrying in case
		 * some future caller of this function has need for it.
		 */
		relation = table_open(LargeObjectMetadataRelationId, RowExclusiveLock);

		/* There's no syscache for pg_largeobject_metadata */
		ScanKeyInit(&entry[0],
					Anum_pg_largeobject_metadata_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(objoid));

		scan = systable_beginscan(relation,
								  LargeObjectMetadataOidIndexId, true,
								  NULL, 1, entry);

		tuple = systable_getnext(scan);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "could not find tuple for large object %u", objoid);

		aclDatum = heap_getattr(tuple,
								Anum_pg_largeobject_metadata_lomacl,
								RelationGetDescr(relation), &isNull);

		/* Add the record, if any, for the top-level object */
		if (!isNull)
			recordExtensionInitPrivWorker(objoid, classoid, 0,
										  DatumGetAclP(aclDatum));

		systable_endscan(scan);
	}
	/* pg_namespace */
	else if (classoid == NamespaceRelationId)
	{
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	tuple;

		tuple = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(objoid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for schema %u", objoid);

		aclDatum = SysCacheGetAttr(NAMESPACEOID, tuple,
								   Anum_pg_namespace_nspacl, &isNull);

		/* Add the record, if any, for the top-level object */
		if (!isNull)
			recordExtensionInitPrivWorker(objoid, classoid, 0,
										  DatumGetAclP(aclDatum));

		ReleaseSysCache(tuple);
	}
	/* pg_proc */
	else if (classoid == ProcedureRelationId)
	{
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	tuple;

		tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(objoid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for function %u", objoid);

		aclDatum = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_proacl,
								   &isNull);

		/* Add the record, if any, for the top-level object */
		if (!isNull)
			recordExtensionInitPrivWorker(objoid, classoid, 0,
										  DatumGetAclP(aclDatum));

		ReleaseSysCache(tuple);
	}
	/* pg_type */
	else if (classoid == TypeRelationId)
	{
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	tuple;

		tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(objoid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for type %u", objoid);

		aclDatum = SysCacheGetAttr(TYPEOID, tuple, Anum_pg_type_typacl,
								   &isNull);

		/* Add the record, if any, for the top-level object */
		if (!isNull)
			recordExtensionInitPrivWorker(objoid, classoid, 0,
										  DatumGetAclP(aclDatum));

		ReleaseSysCache(tuple);
	}
	else if (classoid == AccessMethodRelationId ||
			 classoid == AggregateRelationId ||
			 classoid == CastRelationId ||
			 classoid == CollationRelationId ||
			 classoid == ConversionRelationId ||
			 classoid == EventTriggerRelationId ||
			 classoid == OperatorRelationId ||
			 classoid == OperatorClassRelationId ||
			 classoid == OperatorFamilyRelationId ||
			 classoid == NamespaceRelationId ||
			 classoid == TransformRelationId
		)
	{
		/* no ACL for these object types, so do nothing. */
	}

	/*
	 * complain if we are given a class OID for a class that extensions don't
	 * support or that we don't recognize.
	 */
	else
	{
		elog(ERROR, "unrecognized or unsupported class OID: %u", classoid);
	}
}

/*
 * For the object passed in, remove its ACL and the ACLs of any object subIds
 * from pg_init_privs (via recordExtensionInitPrivWorker()).
 */
void
removeExtObjInitPriv(Oid objoid, Oid classoid)
{
	/*
	 * If this is a relation then we need to see if there are any sub-objects
	 * (eg: columns) for it and, if so, be sure to call
	 * recordExtensionInitPrivWorker() for each one.
	 */
	if (classoid == RelationRelationId)
	{
		Form_pg_class pg_class_tuple;
		HeapTuple	tuple;

		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(objoid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", objoid);
		pg_class_tuple = (Form_pg_class) GETSTRUCT(tuple);

		/*
		 * Indexes don't have permissions, neither do the pg_class rows for
		 * composite types.  (These cases are unreachable given the
		 * restrictions in ALTER EXTENSION DROP, but let's check anyway.)
		 */
		if (pg_class_tuple->relkind == RELKIND_INDEX ||
			pg_class_tuple->relkind == RELKIND_PARTITIONED_INDEX ||
			pg_class_tuple->relkind == RELKIND_COMPOSITE_TYPE)
		{
			ReleaseSysCache(tuple);
			return;
		}

		/*
		 * If this isn't a sequence then it's possibly going to have
		 * column-level ACLs associated with it.
		 */
		if (pg_class_tuple->relkind != RELKIND_SEQUENCE)
		{
			AttrNumber	curr_att;
			AttrNumber	nattrs = pg_class_tuple->relnatts;

			for (curr_att = 1; curr_att <= nattrs; curr_att++)
			{
				HeapTuple	attTuple;

				attTuple = SearchSysCache2(ATTNUM,
										   ObjectIdGetDatum(objoid),
										   Int16GetDatum(curr_att));

				if (!HeapTupleIsValid(attTuple))
					continue;

				/* when removing, remove all entries, even dropped columns */

				recordExtensionInitPrivWorker(objoid, classoid, curr_att, NULL);

				ReleaseSysCache(attTuple);
			}
		}

		ReleaseSysCache(tuple);
	}

	/* Remove the record, if any, for the top-level object */
	recordExtensionInitPrivWorker(objoid, classoid, 0, NULL);
}

/*
 * Record initial ACL for an extension object
 *
 * Can be called at any time, we check if 'creating_extension' is set and, if
 * not, exit immediately.
 *
 * Pass in the object OID, the OID of the class (the OID of the table which
 * the object is defined in) and the 'sub' id of the object (objsubid), if
 * any.  If there is no 'sub' id (they are currently only used for columns of
 * tables) then pass in '0'.  Finally, pass in the complete ACL to store.
 *
 * If an ACL already exists for this object/sub-object then we will replace
 * it with what is passed in.
 *
 * Passing in NULL for 'new_acl' will result in the entry for the object being
 * removed, if one is found.
 */
static void
recordExtensionInitPriv(Oid objoid, Oid classoid, int objsubid, Acl *new_acl)
{
	/*
	 * Generally, we only record the initial privileges when an extension is
	 * being created, but because we don't actually use CREATE EXTENSION
	 * during binary upgrades with pg_upgrade, there is a variable to let us
	 * know that the GRANT and REVOKE statements being issued, while this
	 * variable is true, are for the initial privileges of the extension
	 * object and therefore we need to record them.
	 */
	if (!creating_extension && !binary_upgrade_record_init_privs)
		return;

	recordExtensionInitPrivWorker(objoid, classoid, objsubid, new_acl);
}

/*
 * Record initial ACL for an extension object, worker.
 *
 * This will perform a wholesale replacement of the entire ACL for the object
 * passed in, therefore be sure to pass in the complete new ACL to use.
 *
 * Generally speaking, do *not* use this function directly but instead use
 * recordExtensionInitPriv(), which checks if 'creating_extension' is set.
 * This function does *not* check if 'creating_extension' is set as it is also
 * used when an object is added to or removed from an extension via ALTER
 * EXTENSION ... ADD/DROP.
 */
static void
recordExtensionInitPrivWorker(Oid objoid, Oid classoid, int objsubid, Acl *new_acl)
{
	Relation	relation;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	HeapTuple	oldtuple;

	relation = table_open(InitPrivsRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_init_privs_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objoid));
	ScanKeyInit(&key[1],
				Anum_pg_init_privs_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classoid));
	ScanKeyInit(&key[2],
				Anum_pg_init_privs_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(objsubid));

	scan = systable_beginscan(relation, InitPrivsObjIndexId, true,
							  NULL, 3, key);

	/* There should exist only one entry or none. */
	oldtuple = systable_getnext(scan);

	/* If we find an entry, update it with the latest ACL. */
	if (HeapTupleIsValid(oldtuple))
	{
		Datum		values[Natts_pg_init_privs];
		bool		nulls[Natts_pg_init_privs];
		bool		replace[Natts_pg_init_privs];

		/* If we have a new ACL to set, then update the row with it. */
		if (new_acl)
		{
			MemSet(values, 0, sizeof(values));
			MemSet(nulls, false, sizeof(nulls));
			MemSet(replace, false, sizeof(replace));

			values[Anum_pg_init_privs_initprivs - 1] = PointerGetDatum(new_acl);
			replace[Anum_pg_init_privs_initprivs - 1] = true;

			oldtuple = heap_modify_tuple(oldtuple, RelationGetDescr(relation),
										 values, nulls, replace);

			CatalogTupleUpdate(relation, &oldtuple->t_self, oldtuple);
		}
		else
		{
			/* new_acl is NULL, so delete the entry we found. */
			CatalogTupleDelete(relation, &oldtuple->t_self);
		}
	}
	else
	{
		Datum		values[Natts_pg_init_privs];
		bool		nulls[Natts_pg_init_privs];

		/*
		 * Only add a new entry if the new ACL is non-NULL.
		 *
		 * If we are passed in a NULL ACL and no entry exists, we can just
		 * fall through and do nothing.
		 */
		if (new_acl)
		{
			/* No entry found, so add it. */
			MemSet(nulls, false, sizeof(nulls));

			values[Anum_pg_init_privs_objoid - 1] = ObjectIdGetDatum(objoid);
			values[Anum_pg_init_privs_classoid - 1] = ObjectIdGetDatum(classoid);
			values[Anum_pg_init_privs_objsubid - 1] = Int32GetDatum(objsubid);

			/* This function only handles initial privileges of extensions */
			values[Anum_pg_init_privs_privtype - 1] =
				CharGetDatum(INITPRIVS_EXTENSION);

			values[Anum_pg_init_privs_initprivs - 1] = PointerGetDatum(new_acl);

			tuple = heap_form_tuple(RelationGetDescr(relation), values, nulls);

			CatalogTupleInsert(relation, tuple);
		}
	}

	systable_endscan(scan);

	/* prevent error when processing objects multiple times */
	CommandCounterIncrement();

	table_close(relation, RowExclusiveLock);
}
