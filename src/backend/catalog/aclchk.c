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
#include "catalog/pg_extension.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/extension.h"
#include "commands/proclang.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * Internal format used by ALTER DEFAULT PRIVILEGES.
 */




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


