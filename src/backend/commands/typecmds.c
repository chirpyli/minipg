/*-------------------------------------------------------------------------
 *
 * typecmds.c
 *	  Routines for SQL commands that manipulate types (and domains).
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/typecmds.c
 *
 * DESCRIPTION
 *	  The "DefineFoo" routines take the parse tree and pick out the
 *	  appropriate arguments/flags, passing the results to the
 *	  corresponding "FooDefine" routines (in src/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 * NOTES
 *	  These things must be defined and committed in the following order:
 *		"create function":
 *				input/output, recv/send functions
 *		"create type":
 *				type
 *		"create operator":
 *				operators
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_range.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/typecmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/* parameter structure for AlterTypeRecurse() */
typedef struct
{
	/* Flags indicating which type attributes to update */
	bool		updateStorage;
	bool		updateReceive;
	bool		updateSend;
	bool		updateTypmodin;
	bool		updateTypmodout;
	bool		updateAnalyze;
	bool		updateSubscript;
	/* New values for relevant attributes */
	char		storage;
	Oid			receiveOid;
	Oid			sendOid;
	Oid			typmodinOid;
	Oid			typmodoutOid;
	Oid			analyzeOid;
	Oid			subscriptOid;
} AlterTypeRecurseParams;

static Oid	findTypeReceiveFunction(List *procname, Oid typeOid);
static Oid	findTypeSendFunction(List *procname, Oid typeOid);
static Oid	findTypeTypmodinFunction(List *procname);
static Oid	findTypeTypmodoutFunction(List *procname);
static Oid	findTypeAnalyzeFunction(List *procname, Oid typeOid);
static Oid	findTypeSubscriptingFunction(List *procname, Oid typeOid);
static void AlterTypeRecurse(Oid typeOid, bool isImplicitArray,
							 HeapTuple tup, Relation catalog,
							 AlterTypeRecurseParams *atparams);


/*
 * Guts of type deletion.
 */
void
RemoveTypeById(Oid typeOid)
{
	Relation	relation;
	HeapTuple	tup;

	relation = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);

	CatalogTupleDelete(relation, &tup->t_self);

	/*
	 * If it is a range type, delete the pg_range entry too; we don't bother
	 * with making a dependency entry for that, so it has to be done "by hand"
	 * here.
	 */
	if (((Form_pg_type) GETSTRUCT(tup))->typtype == TYPTYPE_RANGE)
		RangeDelete(typeOid);

	ReleaseSysCache(tup);

	table_close(relation, RowExclusiveLock);
}

/*
 *	AssignTypeArrayOid
 *
 *	Pre-assign the type's array OID for use in pg_type.typarray
 */
Oid
AssignTypeArrayOid(void)
{
	Oid			type_array_oid;

	{
		Relation	pg_type = table_open(TypeRelationId, AccessShareLock);

		type_array_oid = GetNewOidWithIndex(pg_type, TypeOidIndexId,
											Anum_pg_type_oid);
		table_close(pg_type, AccessShareLock);
	}

	return type_array_oid;
}

/*
 * Execute ALTER TYPE SET SCHEMA
 */
ObjectAddress
AlterTypeNamespace(List *names, const char *newschema, ObjectType objecttype,
				   Oid *oldschema)
{
	TypeName   *typename;
	Oid			typeOid;
	Oid			nspOid;
	Oid			oldNspOid;
	ObjectAddresses *objsMoved;
	ObjectAddress myself;

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeTypeNameFromNameList(names);
	typeOid = typenameTypeId(NULL, typename);

	/* Don't allow ALTER DOMAIN on a type */
	if (objecttype == OBJECT_DOMAIN && get_typtype(typeOid) != TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a domain",
						format_type_be(typeOid))));

	/* get schema OID and check its permissions */
	nspOid = LookupCreationNamespace(newschema);

	objsMoved = new_object_addresses();
	oldNspOid = AlterTypeNamespace_oid(typeOid, nspOid, objsMoved);
	free_object_addresses(objsMoved);

	if (oldschema)
		*oldschema = oldNspOid;

	ObjectAddressSet(myself, TypeRelationId, typeOid);

	return myself;
}

Oid
AlterTypeNamespace_oid(Oid typeOid, Oid nspOid, ObjectAddresses *objsMoved)
{
	Oid			elemOid;

	/* check permissions on type */

	/* don't allow direct alteration of array types */
	elemOid = get_element_type(typeOid);
	if (OidIsValid(elemOid) && get_array_type(elemOid) == typeOid)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter array type %s",
						format_type_be(typeOid)),
				 errhint("You can alter type %s, which will alter the array type as well.",
						 format_type_be(elemOid))));

	/* and do the work */
	return AlterTypeNamespaceInternal(typeOid, nspOid, false, true, objsMoved);
}

/*
 * Move specified type to new namespace.
 *
 * Caller must have already checked privileges.
 *
 * The function automatically recurses to process the type's array type,
 * if any.  isImplicitArray should be true only when doing this internal
 * recursion (outside callers must never try to move an array type directly).
 *
 * If errorOnTableType is true, the function errors out if the type is
 * a table type.  ALTER TABLE has to be used to move a table to a new
 * namespace.
 *
 * Returns the type's old namespace OID.
 */
Oid
AlterTypeNamespaceInternal(Oid typeOid, Oid nspOid,
						   bool isImplicitArray,
						   bool errorOnTableType,
						   ObjectAddresses *objsMoved)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_type typform;
	Oid			oldNspOid;
	Oid			arrayOid;
	bool		isCompositeType;
	ObjectAddress thisobj;

	/*
	 * Make sure we haven't moved this object previously.
	 */
	thisobj.classId = TypeRelationId;
	thisobj.objectId = typeOid;
	thisobj.objectSubId = 0;

	if (object_address_present(&thisobj, objsMoved))
		return InvalidOid;

	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typform = (Form_pg_type) GETSTRUCT(tup);

	oldNspOid = typform->typnamespace;
	arrayOid = typform->typarray;

	/* If the type is already there, we scan skip these next few checks. */
	if (oldNspOid != nspOid)
	{
		/* common checks on switching namespaces */
		CheckSetNamespace(oldNspOid, nspOid);

		/* check for duplicate name (more friendly than unique-index failure) */
		if (SearchSysCacheExists2(TYPENAMENSP,
								  NameGetDatum(&typform->typname),
								  ObjectIdGetDatum(nspOid)))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists in schema \"%s\"",
							NameStr(typform->typname),
							get_namespace_name(nspOid))));
	}

	/* Detect whether type is a composite type (but not a table rowtype) */
	isCompositeType =
		(typform->typtype == TYPTYPE_COMPOSITE &&
		 get_rel_relkind(typform->typrelid) == RELKIND_COMPOSITE_TYPE);

	/* Enforce not-table-type if requested */
	if (typform->typtype == TYPTYPE_COMPOSITE && !isCompositeType &&
		errorOnTableType)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is a table's row type",
						format_type_be(typeOid)),
				 errhint("Use ALTER TABLE instead.")));

	if (oldNspOid != nspOid)
	{
		/* OK, modify the pg_type row */

		/* tup is a copy, so we can scribble directly on it */
		typform->typnamespace = nspOid;

		CatalogTupleUpdate(rel, &tup->t_self, tup);
	}

	/*
	 * Composite types have pg_class entries.
	 *
	 * We need to modify the pg_class tuple as well to reflect the change of
	 * schema.
	 */
	if (isCompositeType)
	{
		Relation	classRel;

		classRel = table_open(RelationRelationId, RowExclusiveLock);

		AlterRelationNamespaceInternal(classRel, typform->typrelid,
									   oldNspOid, nspOid,
									   false, objsMoved);

		table_close(classRel, RowExclusiveLock);

		/*
		 * Check for constraints associated with the composite type (we don't
		 * currently support this, but probably will someday).
		 */
		AlterConstraintNamespaces(typform->typrelid, oldNspOid,
								  nspOid, false, objsMoved);
	}
	else
	{
		/* If it's a domain, it might have constraints */
		if (typform->typtype == TYPTYPE_DOMAIN)
			AlterConstraintNamespaces(typeOid, oldNspOid, nspOid, true,
									  objsMoved);
	}

	/*
	 * Update dependency on schema, if any --- a table rowtype has not got
	 * one, and neither does an implicit array.
	 */
	if (oldNspOid != nspOid &&
		(isCompositeType || typform->typtype != TYPTYPE_COMPOSITE) &&
		!isImplicitArray)
		if (changeDependencyFor(TypeRelationId, typeOid,
								NamespaceRelationId, oldNspOid, nspOid) != 1)
			elog(ERROR, "failed to change schema dependency for type %s",
				 format_type_be(typeOid));

	InvokeObjectPostAlterHook(TypeRelationId, typeOid, 0);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	add_exact_object_address(&thisobj, objsMoved);

	/* Recursively alter the associated array type, if any */
	if (OidIsValid(arrayOid))
		AlterTypeNamespaceInternal(arrayOid, nspOid, true, true, objsMoved);

	return oldNspOid;
}

/*
 * AlterType
 *		ALTER TYPE <type> SET (option = ...)
 *
 * NOTE: the set of changes that can be allowed here is constrained by many
 * non-obvious implementation restrictions.  Tread carefully when considering
 * adding new flexibility.
 */
