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

static void makeRangeConstructors(const char *name, Oid namespace,
								  Oid rangeOid, Oid subtype);
static void makeMultirangeConstructors(const char *name, Oid namespace,
									   Oid multirangeOid, Oid rangeOid,
									   Oid rangeArrayOid, Oid *castFuncOid);
static Oid	findTypeReceiveFunction(List *procname, Oid typeOid);
static Oid	findTypeSendFunction(List *procname, Oid typeOid);
static Oid	findTypeTypmodinFunction(List *procname);
static Oid	findTypeTypmodoutFunction(List *procname);
static Oid	findTypeAnalyzeFunction(List *procname, Oid typeOid);
static Oid	findTypeSubscriptingFunction(List *procname, Oid typeOid);
static Oid	findRangeSubOpclass(List *opcname, Oid subtype);
static Oid	findRangeCanonicalFunction(List *procname, Oid typeOid);
static Oid	findRangeSubtypeDiffFunction(List *procname, Oid subtype);
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
 * DefineRange
 *		Registers a new range type.
 *
 * Perhaps it might be worthwhile to set pg_type.typelem to the base type,
 * and likewise on multiranges to set it to the range type. But having a
 * non-zero typelem is treated elsewhere as a synonym for being an array,
 * and users might have queries with that same assumption.
 */
ObjectAddress
DefineRange(CreateRangeStmt *stmt)
{
	char	   *typeName;
	Oid			typeNamespace;
	Oid			typoid;
	char	   *rangeArrayName;
	char	   *multirangeTypeName = NULL;
	char	   *multirangeArrayName;
	Oid			multirangeNamespace = InvalidOid;
	Oid			rangeArrayOid;
	Oid			multirangeOid;
	Oid			multirangeArrayOid;
	Oid			rangeSubtype = InvalidOid;
	List	   *rangeSubOpclassName = NIL;
	List	   *rangeCollationName = NIL;
	List	   *rangeCanonicalName = NIL;
	List	   *rangeSubtypeDiffName = NIL;
	Oid			rangeSubOpclass;
	Oid			rangeCollation;
	regproc		rangeCanonical;
	regproc		rangeSubtypeDiff;
	int16		subtyplen;
	bool		subtypbyval;
	char		subtypalign;
	char		alignment;
	ListCell   *lc;
	ObjectAddress address;
	ObjectAddress mltrngaddress PG_USED_FOR_ASSERTS_ONLY;
	Oid			castFuncOid;

	/* Convert list of names to a name and namespace */
	typeNamespace = QualifiedNameGetCreationNamespace(stmt->typeName,
													  &typeName);

	/*
	 * Look to see if type already exists.
	 */
	typoid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
							 CStringGetDatum(typeName),
							 ObjectIdGetDatum(typeNamespace));

	/*
	 * If it's not a shell, see if it's an autogenerated array type, and if so
	 * rename it out of the way.
	 */
	if (OidIsValid(typoid) && get_typisdefined(typoid))
	{
		if (moveArrayTypeName(typoid, typeName, typeNamespace))
			typoid = InvalidOid;
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", typeName)));
	}

	/*
	 * Unlike DefineType(), we don't insist on a shell type existing first, as
	 * it's only needed if the user wants to specify a canonical function.
	 */

	/* Extract the parameters from the parameter list */
	foreach(lc, stmt->params)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "subtype") == 0)
		{
			if (OidIsValid(rangeSubtype))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			/* we can look up the subtype name immediately */
			rangeSubtype = typenameTypeId(NULL, defGetTypeName(defel));
		}
		else if (strcmp(defel->defname, "subtype_opclass") == 0)
		{
			if (rangeSubOpclassName != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			rangeSubOpclassName = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "collation") == 0)
		{
			if (rangeCollationName != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			rangeCollationName = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "canonical") == 0)
		{
			if (rangeCanonicalName != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			rangeCanonicalName = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "subtype_diff") == 0)
		{
			if (rangeSubtypeDiffName != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			rangeSubtypeDiffName = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "multirange_type_name") == 0)
		{
			if (multirangeTypeName != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			/* we can look up the subtype name immediately */
			multirangeNamespace = QualifiedNameGetCreationNamespace(defGetQualifiedName(defel),
																	&multirangeTypeName);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type attribute \"%s\" not recognized",
							defel->defname)));
	}

	/* Must have a subtype */
	if (!OidIsValid(rangeSubtype))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("type attribute \"subtype\" is required")));
	/* disallow ranges of pseudotypes */
	if (get_typtype(rangeSubtype) == TYPTYPE_PSEUDO)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("range subtype cannot be %s",
						format_type_be(rangeSubtype))));

	/* Identify subopclass */
	rangeSubOpclass = findRangeSubOpclass(rangeSubOpclassName, rangeSubtype);

	/* Identify collation to use, if any */
	if (type_is_collatable(rangeSubtype))
	{
		if (rangeCollationName != NIL)
			rangeCollation = get_collation_oid(rangeCollationName, false);
		else
			rangeCollation = get_typcollation(rangeSubtype);
	}
	else
	{
		if (rangeCollationName != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("range collation specified but subtype does not support collation")));
		rangeCollation = InvalidOid;
	}

	/* Identify support functions, if provided */
	if (rangeCanonicalName != NIL)
	{
		if (!OidIsValid(typoid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot specify a canonical function without a pre-created shell type"),
					 errhint("Create the type as a shell type, then create its canonicalization function, then do a full CREATE TYPE.")));
		rangeCanonical = findRangeCanonicalFunction(rangeCanonicalName,
													typoid);
	}
	else
		rangeCanonical = InvalidOid;

	if (rangeSubtypeDiffName != NIL)
		rangeSubtypeDiff = findRangeSubtypeDiffFunction(rangeSubtypeDiffName,
														rangeSubtype);
	else
		rangeSubtypeDiff = InvalidOid;

	get_typlenbyvalalign(rangeSubtype,
						 &subtyplen, &subtypbyval, &subtypalign);

	/* alignment must be TYPALIGN_INT or TYPALIGN_DOUBLE for ranges */
	alignment = (subtypalign == TYPALIGN_DOUBLE) ? TYPALIGN_DOUBLE : TYPALIGN_INT;

	/* Allocate OID for array type, its multirange, and its multirange array */
	rangeArrayOid = AssignTypeArrayOid();
	multirangeOid = AssignTypeMultirangeOid();
	multirangeArrayOid = AssignTypeMultirangeArrayOid();

	/* Create the pg_type entry */
	address =
		TypeCreate(InvalidOid,	/* no predetermined type OID */
				   typeName,	/* type name */
				   typeNamespace,	/* namespace */
				   InvalidOid,	/* relation oid (n/a here) */
				   0,			/* relation kind (ditto) */
				   GetUserId(), /* owner's ID */
				   -1,			/* internal size (always varlena) */
				   TYPTYPE_RANGE,	/* type-type (range type) */
				   TYPCATEGORY_RANGE,	/* type-category (range type) */
				   false,		/* range types are never preferred */
				   DEFAULT_TYPDELIM,	/* array element delimiter */
				   F_RANGE_IN,	/* input procedure */
				   F_RANGE_OUT, /* output procedure */
				   F_RANGE_RECV,	/* receive procedure */
				   F_RANGE_SEND,	/* send procedure */
				   InvalidOid,	/* typmodin procedure - none */
				   InvalidOid,	/* typmodout procedure - none */
				   F_RANGE_TYPANALYZE,	/* analyze procedure */
				   InvalidOid,	/* subscript procedure - none */
				   InvalidOid,	/* element type ID - none */
				   false,		/* this is not an array type */
				   rangeArrayOid,	/* array type we are about to create */
				   InvalidOid,	/* base type ID (only for domains) */
				   NULL,		/* never a default type value */
				   NULL,		/* no binary form available either */
				   false,		/* never passed by value */
				   alignment,	/* alignment */
				   TYPSTORAGE_EXTENDED, /* TOAST strategy (always extended) */
				   -1,			/* typMod (Domains only) */
				   0,			/* Array dimensions of typbasetype */
				   false,		/* Type NOT NULL */
				   InvalidOid); /* type's collation (ranges never have one) */
	Assert(typoid == InvalidOid || typoid == address.objectId);
	typoid = address.objectId;

	/* Create the multirange that goes with it */
	if (multirangeTypeName)
	{
		Oid			old_typoid;

		/*
		 * Look to see if multirange type already exists.
		 */
		old_typoid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
									 CStringGetDatum(multirangeTypeName),
									 ObjectIdGetDatum(multirangeNamespace));

		/*
		 * If it's not a shell, see if it's an autogenerated array type, and
		 * if so rename it out of the way.
		 */
		if (OidIsValid(old_typoid) && get_typisdefined(old_typoid))
		{
			if (!moveArrayTypeName(old_typoid, multirangeTypeName, multirangeNamespace))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("type \"%s\" already exists", multirangeTypeName)));
		}
	}
	else
	{
		/* Generate multirange name automatically */
		multirangeNamespace = typeNamespace;
		multirangeTypeName = makeMultirangeTypeName(typeName, multirangeNamespace);
	}

	mltrngaddress =
		TypeCreate(multirangeOid,	/* force assignment of this type OID */
				   multirangeTypeName,	/* type name */
				   multirangeNamespace, /* namespace */
				   InvalidOid,	/* relation oid (n/a here) */
				   0,			/* relation kind (ditto) */
				   GetUserId(), /* owner's ID */
				   -1,			/* internal size (always varlena) */
				   TYPTYPE_MULTIRANGE,	/* type-type (multirange type) */
				   TYPCATEGORY_RANGE,	/* type-category (range type) */
				   false,		/* multirange types are never preferred */
				   DEFAULT_TYPDELIM,	/* array element delimiter */
				   F_MULTIRANGE_IN, /* input procedure */
				   F_MULTIRANGE_OUT,	/* output procedure */
				   F_MULTIRANGE_RECV,	/* receive procedure */
				   F_MULTIRANGE_SEND,	/* send procedure */
				   InvalidOid,	/* typmodin procedure - none */
				   InvalidOid,	/* typmodout procedure - none */
				   F_MULTIRANGE_TYPANALYZE, /* analyze procedure */
				   InvalidOid,	/* subscript procedure - none */
				   InvalidOid,	/* element type ID - none */
				   false,		/* this is not an array type */
				   multirangeArrayOid,	/* array type we are about to create */
				   InvalidOid,	/* base type ID (only for domains) */
				   NULL,		/* never a default type value */
				   NULL,		/* no binary form available either */
				   false,		/* never passed by value */
				   alignment,	/* alignment */
				   'x',			/* TOAST strategy (always extended) */
				   -1,			/* typMod (Domains only) */
				   0,			/* Array dimensions of typbasetype */
				   false,		/* Type NOT NULL */
				   InvalidOid); /* type's collation (ranges never have one) */
	Assert(multirangeOid == mltrngaddress.objectId);

	/* Create the entry in pg_range */
	RangeCreate(typoid, rangeSubtype, rangeCollation, rangeSubOpclass,
				rangeCanonical, rangeSubtypeDiff, multirangeOid);

	/*
	 * Create the array type that goes with it.
	 */
	rangeArrayName = makeArrayTypeName(typeName, typeNamespace);

	TypeCreate(rangeArrayOid,	/* force assignment of this type OID */
			   rangeArrayName,	/* type name */
			   typeNamespace,	/* namespace */
			   InvalidOid,		/* relation oid (n/a here) */
			   0,				/* relation kind (ditto) */
			   GetUserId(),		/* owner's ID */
			   -1,				/* internal size (always varlena) */
			   TYPTYPE_BASE,	/* type-type (base type) */
			   TYPCATEGORY_ARRAY,	/* type-category (array) */
			   false,			/* array types are never preferred */
			   DEFAULT_TYPDELIM,	/* array element delimiter */
			   F_ARRAY_IN,		/* input procedure */
			   F_ARRAY_OUT,		/* output procedure */
			   F_ARRAY_RECV,	/* receive procedure */
			   F_ARRAY_SEND,	/* send procedure */
			   InvalidOid,		/* typmodin procedure - none */
			   InvalidOid,		/* typmodout procedure - none */
			   F_ARRAY_TYPANALYZE,	/* analyze procedure */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* array subscript procedure */
			   typoid,			/* element type ID */
			   true,			/* yes this is an array type */
			   InvalidOid,		/* no further array type */
			   InvalidOid,		/* base type ID */
			   NULL,			/* never a default type value */
			   NULL,			/* binary default isn't sent either */
			   false,			/* never passed by value */
			   alignment,		/* alignment - same as range's */
			   TYPSTORAGE_EXTENDED, /* ARRAY is always toastable */
			   -1,				/* typMod (Domains only) */
			   0,				/* Array dimensions of typbasetype */
			   false,			/* Type NOT NULL */
			   InvalidOid);		/* typcollation */

	pfree(rangeArrayName);

	/* Create the multirange's array type */

	multirangeArrayName = makeArrayTypeName(multirangeTypeName, typeNamespace);

	TypeCreate(multirangeArrayOid,	/* force assignment of this type OID */
			   multirangeArrayName, /* type name */
			   multirangeNamespace, /* namespace */
			   InvalidOid,		/* relation oid (n/a here) */
			   0,				/* relation kind (ditto) */
			   GetUserId(),		/* owner's ID */
			   -1,				/* internal size (always varlena) */
			   TYPTYPE_BASE,	/* type-type (base type) */
			   TYPCATEGORY_ARRAY,	/* type-category (array) */
			   false,			/* array types are never preferred */
			   DEFAULT_TYPDELIM,	/* array element delimiter */
			   F_ARRAY_IN,		/* input procedure */
			   F_ARRAY_OUT,		/* output procedure */
			   F_ARRAY_RECV,	/* receive procedure */
			   F_ARRAY_SEND,	/* send procedure */
			   InvalidOid,		/* typmodin procedure - none */
			   InvalidOid,		/* typmodout procedure - none */
			   F_ARRAY_TYPANALYZE,	/* analyze procedure */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* array subscript procedure */
			   multirangeOid,	/* element type ID */
			   true,			/* yes this is an array type */
			   InvalidOid,		/* no further array type */
			   InvalidOid,		/* base type ID */
			   NULL,			/* never a default type value */
			   NULL,			/* binary default isn't sent either */
			   false,			/* never passed by value */
			   alignment,		/* alignment - same as range's */
			   'x',				/* ARRAY is always toastable */
			   -1,				/* typMod (Domains only) */
			   0,				/* Array dimensions of typbasetype */
			   false,			/* Type NOT NULL */
			   InvalidOid);		/* typcollation */

	/* And create the constructor functions for this range type */
	makeRangeConstructors(typeName, typeNamespace, typoid, rangeSubtype);
	makeMultirangeConstructors(multirangeTypeName, typeNamespace,
							   multirangeOid, typoid, rangeArrayOid,
							   &castFuncOid);

	/* Create cast from the range type to its multirange type */
	CastCreate(typoid, multirangeOid, castFuncOid, 'e', 'f', DEPENDENCY_INTERNAL);

	pfree(multirangeArrayName);

	return address;
}

/*
 * Because there may exist several range types over the same subtype, the
 * range type can't be uniquely determined from the subtype.  So it's
 * impossible to define a polymorphic constructor; we have to generate new
 * constructor functions explicitly for each range type.
 *
 * We actually define 4 functions, with 0 through 3 arguments.  This is just
 * to offer more convenience for the user.
 */
static void
makeRangeConstructors(const char *name, Oid namespace,
					  Oid rangeOid, Oid subtype)
{
	static const char *const prosrc[2] = {"range_constructor2",
	"range_constructor3"};
	static const int pronargs[2] = {2, 3};

	Oid			constructorArgTypes[3];
	ObjectAddress myself,
				referenced;
	int			i;

	constructorArgTypes[0] = subtype;
	constructorArgTypes[1] = subtype;
	constructorArgTypes[2] = TEXTOID;

	referenced.classId = TypeRelationId;
	referenced.objectId = rangeOid;
	referenced.objectSubId = 0;

	for (i = 0; i < lengthof(prosrc); i++)
	{
		oidvector  *constructorArgTypesVector;

		constructorArgTypesVector = buildoidvector(constructorArgTypes,
												   pronargs[i]);

		myself = ProcedureCreate(name,	/* name: same as range type */
								 namespace, /* namespace */
								 false, /* replace */
								 false, /* returns set */
								 rangeOid,	/* return type */
								 BOOTSTRAP_SUPERUSERID, /* proowner */
								 INTERNALlanguageId,	/* language */
								 F_FMGR_INTERNAL_VALIDATOR, /* language validator */
								 prosrc[i], /* prosrc */
								 NULL,	/* probin */
								 NULL,	/* prosqlbody */
								 PROKIND_FUNCTION,
								 false, /* security_definer */
								 false, /* leakproof */
								 false, /* isStrict */
								 PROVOLATILE_IMMUTABLE, /* volatility */
								 PROPARALLEL_SAFE,	/* parallel safety */
								 constructorArgTypesVector, /* parameterTypes */
								 PointerGetDatum(NULL), /* allParameterTypes */
								 PointerGetDatum(NULL), /* parameterModes */
								 PointerGetDatum(NULL), /* parameterNames */
								 NIL,	/* parameterDefaults */
								 PointerGetDatum(NULL), /* trftypes */
								 PointerGetDatum(NULL), /* proconfig */
								 InvalidOid,	/* prosupport */
								 1.0,	/* procost */
								 0.0);	/* prorows */

		/*
		 * Make the constructors internally-dependent on the range type so
		 * that they go away silently when the type is dropped.  Note that
		 * pg_dump depends on this choice to avoid dumping the constructors.
		 */
		recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	}
}

/*
 * We make a separate multirange constructor for each range type
 * so its name can include the base type, like range constructors do.
 * If we had an anyrangearray polymorphic type we could use it here,
 * but since each type has its own constructor name there's no need.
 *
 * Sets castFuncOid to the oid of the new constructor that can be used
 * to cast from a range to a multirange.
 */
static void
makeMultirangeConstructors(const char *name, Oid namespace,
						   Oid multirangeOid, Oid rangeOid, Oid rangeArrayOid,
						   Oid *castFuncOid)
{
	ObjectAddress myself,
				referenced;
	oidvector  *argtypes;
	Datum		allParamTypes;
	ArrayType  *allParameterTypes;
	Datum		paramModes;
	ArrayType  *parameterModes;

	referenced.classId = TypeRelationId;
	referenced.objectId = multirangeOid;
	referenced.objectSubId = 0;

	/* 0-arg constructor - for empty multiranges */
	argtypes = buildoidvector(NULL, 0);
	myself = ProcedureCreate(name,	/* name: same as multirange type */
							 namespace,
							 false, /* replace */
							 false, /* returns set */
							 multirangeOid, /* return type */
							 BOOTSTRAP_SUPERUSERID, /* proowner */
							 INTERNALlanguageId,	/* language */
							 F_FMGR_INTERNAL_VALIDATOR,
							 "multirange_constructor0", /* prosrc */
							 NULL,	/* probin */
							 NULL,	/* prosqlbody */
							 PROKIND_FUNCTION,
							 false, /* security_definer */
							 false, /* leakproof */
							 true,	/* isStrict */
							 PROVOLATILE_IMMUTABLE, /* volatility */
							 PROPARALLEL_SAFE,	/* parallel safety */
							 argtypes,	/* parameterTypes */
							 PointerGetDatum(NULL), /* allParameterTypes */
							 PointerGetDatum(NULL), /* parameterModes */
							 PointerGetDatum(NULL), /* parameterNames */
							 NIL,	/* parameterDefaults */
							 PointerGetDatum(NULL), /* trftypes */
							 PointerGetDatum(NULL), /* proconfig */
							 InvalidOid,	/* prosupport */
							 1.0,	/* procost */
							 0.0);	/* prorows */

	/*
	 * Make the constructor internally-dependent on the multirange type so
	 * that they go away silently when the type is dropped.  Note that pg_dump
	 * depends on this choice to avoid dumping the constructors.
	 */
	recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	pfree(argtypes);

	/*
	 * 1-arg constructor - for casts
	 *
	 * In theory we shouldn't need both this and the vararg (n-arg)
	 * constructor, but having a separate 1-arg function lets us define casts
	 * against it.
	 */
	argtypes = buildoidvector(&rangeOid, 1);
	myself = ProcedureCreate(name,	/* name: same as multirange type */
							 namespace,
							 false, /* replace */
							 false, /* returns set */
							 multirangeOid, /* return type */
							 BOOTSTRAP_SUPERUSERID, /* proowner */
							 INTERNALlanguageId,	/* language */
							 F_FMGR_INTERNAL_VALIDATOR,
							 "multirange_constructor1", /* prosrc */
							 NULL,	/* probin */
							 NULL,	/* prosqlbody */
							 PROKIND_FUNCTION,
							 false, /* security_definer */
							 false, /* leakproof */
							 true,	/* isStrict */
							 PROVOLATILE_IMMUTABLE, /* volatility */
							 PROPARALLEL_SAFE,	/* parallel safety */
							 argtypes,	/* parameterTypes */
							 PointerGetDatum(NULL), /* allParameterTypes */
							 PointerGetDatum(NULL), /* parameterModes */
							 PointerGetDatum(NULL), /* parameterNames */
							 NIL,	/* parameterDefaults */
							 PointerGetDatum(NULL), /* trftypes */
							 PointerGetDatum(NULL), /* proconfig */
							 InvalidOid,	/* prosupport */
							 1.0,	/* procost */
							 0.0);	/* prorows */
	/* ditto */
	recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	pfree(argtypes);
	*castFuncOid = myself.objectId;

	/* n-arg constructor - vararg */
	argtypes = buildoidvector(&rangeArrayOid, 1);
	allParamTypes = ObjectIdGetDatum(rangeArrayOid);
	allParameterTypes = construct_array(&allParamTypes,
										1, OIDOID,
										sizeof(Oid), true, 'i');
	paramModes = CharGetDatum(FUNC_PARAM_VARIADIC);
	parameterModes = construct_array(&paramModes, 1, CHAROID,
									 1, true, 'c');
	myself = ProcedureCreate(name,	/* name: same as multirange type */
							 namespace,
							 false, /* replace */
							 false, /* returns set */
							 multirangeOid, /* return type */
							 BOOTSTRAP_SUPERUSERID, /* proowner */
							 INTERNALlanguageId,	/* language */
							 F_FMGR_INTERNAL_VALIDATOR,
							 "multirange_constructor2", /* prosrc */
							 NULL,	/* probin */
							 NULL,	/* prosqlbody */
							 PROKIND_FUNCTION,
							 false, /* security_definer */
							 false, /* leakproof */
							 true,	/* isStrict */
							 PROVOLATILE_IMMUTABLE, /* volatility */
							 PROPARALLEL_SAFE,	/* parallel safety */
							 argtypes,	/* parameterTypes */
							 PointerGetDatum(allParameterTypes),	/* allParameterTypes */
							 PointerGetDatum(parameterModes),	/* parameterModes */
							 PointerGetDatum(NULL), /* parameterNames */
							 NIL,	/* parameterDefaults */
							 PointerGetDatum(NULL), /* trftypes */
							 PointerGetDatum(NULL), /* proconfig */
							 InvalidOid,	/* prosupport */
							 1.0,	/* procost */
							 0.0);	/* prorows */
	/* ditto */
	recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	pfree(argtypes);
	pfree(allParameterTypes);
	pfree(parameterModes);
}

static Oid
findTypeReceiveFunction(List *procname, Oid typeOid)
{
	Oid			argList[3];
	Oid			procOid;
	Oid			procOid2;

	/*
	 * Receive functions can take a single argument of type INTERNAL, or three
	 * arguments (internal, typioparam OID, typmod).  Whine about ambiguity if
	 * both forms exist.
	 */
	argList[0] = INTERNALOID;
	argList[1] = OIDOID;
	argList[2] = INT4OID;

	procOid = LookupFuncName(procname, 1, argList, true);
	procOid2 = LookupFuncName(procname, 3, argList, true);
	if (OidIsValid(procOid))
	{
		if (OidIsValid(procOid2))
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("type receive function %s has multiple matches",
							NameListToString(procname))));
	}
	else
	{
		procOid = procOid2;
		/* If not found, reference the 1-argument signature in error msg */
		if (!OidIsValid(procOid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("function %s does not exist",
							func_signature_string(procname, 1, NIL, argList))));
	}

	/* Receive functions must return the target type. */
	if (get_func_rettype(procOid) != typeOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type receive function %s must return type %s",
						NameListToString(procname), format_type_be(typeOid))));

	/* Just a warning for now, per comments in findTypeInputFunction */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type receive function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

static Oid
findTypeSendFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * Send functions always take a single argument of the type and return
	 * bytea.
	 */
	argList[0] = typeOid;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != BYTEAOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type send function %s must return type %s",
						NameListToString(procname), "bytea")));

	/* Just a warning for now, per comments in findTypeInputFunction */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type send function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

static Oid
findTypeTypmodinFunction(List *procname)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * typmodin functions always take one cstring[] argument and return int4.
	 */
	argList[0] = CSTRINGARRAYOID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != INT4OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("typmod_in function %s must return type %s",
						NameListToString(procname), "integer")));

	/* Just a warning for now, per comments in findTypeInputFunction */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type modifier input function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

static Oid
findTypeTypmodoutFunction(List *procname)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * typmodout functions always take one int4 argument and return cstring.
	 */
	argList[0] = INT4OID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != CSTRINGOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("typmod_out function %s must return type %s",
						NameListToString(procname), "cstring")));

	/* Just a warning for now, per comments in findTypeInputFunction */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type modifier output function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

static Oid
findTypeAnalyzeFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * Analyze functions always take one INTERNAL argument and return bool.
	 */
	argList[0] = INTERNALOID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != BOOLOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type analyze function %s must return type %s",
						NameListToString(procname), "boolean")));

	return procOid;
}

static Oid
findTypeSubscriptingFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * Subscripting support functions always take one INTERNAL argument and
	 * return INTERNAL.  (The argument is not used, but we must have it to
	 * maintain type safety.)
	 */
	argList[0] = INTERNALOID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != INTERNALOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type subscripting function %s must return type %s",
						NameListToString(procname), "internal")));

	/*
	 * We disallow array_subscript_handler() from being selected explicitly,
	 * since that must only be applied to autogenerated array types.
	 */
	if (procOid == F_ARRAY_SUBSCRIPT_HANDLER)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("user-defined types cannot use subscripting function %s",
						NameListToString(procname))));

	return procOid;
}

/*
 * Find suitable support functions and opclasses for a range type.
 */

/*
 * Find named btree opclass for subtype, or default btree opclass if
 * opcname is NIL.
 */
static Oid
findRangeSubOpclass(List *opcname, Oid subtype)
{
	Oid			opcid;
	Oid			opInputType;

	if (opcname != NIL)
	{
		opcid = get_opclass_oid(BTREE_AM_OID, opcname, false);

		/*
		 * Verify that the operator class accepts this datatype. Note we will
		 * accept binary compatibility.
		 */
		opInputType = get_opclass_input_type(opcid);
		if (!IsBinaryCoercible(subtype, opInputType))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("operator class \"%s\" does not accept data type %s",
							NameListToString(opcname),
							format_type_be(subtype))));
	}
	else
	{
		opcid = GetDefaultOpClass(subtype, BTREE_AM_OID);
		if (!OidIsValid(opcid))
		{
			/* We spell the error message identically to ResolveOpClass */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("data type %s has no default operator class for access method \"%s\"",
							format_type_be(subtype), "btree"),
					 errhint("You must specify an operator class for the range type or define a default operator class for the subtype.")));
		}
	}

	return opcid;
}

static Oid
findRangeCanonicalFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * Range canonical functions must take and return the range type, and must
	 * be immutable.
	 */
	argList[0] = typeOid;

	procOid = LookupFuncName(procname, 1, argList, true);

	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != typeOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("range canonical function %s must return range type",
						func_signature_string(procname, 1, NIL, argList))));

	if (func_volatile(procOid) != PROVOLATILE_IMMUTABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("range canonical function %s must be immutable",
						func_signature_string(procname, 1, NIL, argList))));

	return procOid;
}

static Oid
findRangeSubtypeDiffFunction(List *procname, Oid subtype)
{
	Oid			argList[2];
	Oid			procOid;

	/*
	 * Range subtype diff functions must take two arguments of the subtype,
	 * must return float8, and must be immutable.
	 */
	argList[0] = subtype;
	argList[1] = subtype;

	procOid = LookupFuncName(procname, 2, argList, true);

	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 2, NIL, argList))));

	if (get_func_rettype(procOid) != FLOAT8OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("range subtype diff function %s must return type %s",
						func_signature_string(procname, 2, NIL, argList),
						"double precision")));

	if (func_volatile(procOid) != PROVOLATILE_IMMUTABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("range subtype diff function %s must be immutable",
						func_signature_string(procname, 2, NIL, argList))));

	return procOid;
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
 *	AssignTypeMultirangeOid
 *
 *	Pre-assign the range type's multirange OID for use in pg_type.oid
 */
Oid
AssignTypeMultirangeOid(void)
{
	Oid			type_multirange_oid;

	{
		Relation	pg_type = table_open(TypeRelationId, AccessShareLock);

		type_multirange_oid = GetNewOidWithIndex(pg_type, TypeOidIndexId,
												 Anum_pg_type_oid);
		table_close(pg_type, AccessShareLock);
	}

	return type_multirange_oid;
}

/*
 *	AssignTypeMultirangeArrayOid
 *
 *	Pre-assign the range type's multirange array OID for use in pg_type.typarray
 */
Oid
AssignTypeMultirangeArrayOid(void)
{
	Oid			type_multirange_array_oid;

	{
		Relation	pg_type = table_open(TypeRelationId, AccessShareLock);

		type_multirange_array_oid = GetNewOidWithIndex(pg_type, TypeOidIndexId,
													   Anum_pg_type_oid);
		table_close(pg_type, AccessShareLock);
	}

	return type_multirange_array_oid;
}


/*-------------------------------------------------------------------
 * DefineCompositeType
 *
 * Create a Composite Type relation.
 * `DefineRelation' does all the work, we just provide the correct
 * arguments!
 *
 * If the relation already exists, then 'DefineRelation' will abort
 * the xact...
 *
 * Return type is the new type's object address.
 *-------------------------------------------------------------------
 */
ObjectAddress
DefineCompositeType(RangeVar *typevar, List *coldeflist)
{
	CreateStmt *createStmt = makeNode(CreateStmt);
	Oid			old_type_oid;
	Oid			typeNamespace;
	ObjectAddress address;

	/*
	 * now set the parameters for keys/inheritance etc. All of these are
	 * uninteresting for composite types...
	 */
	createStmt->relation = typevar;
	createStmt->tableElts = coldeflist;
	createStmt->constraints = NIL;
	createStmt->options = NIL;
	createStmt->oncommit = ONCOMMIT_NOOP;
	createStmt->if_not_exists = false;

	/*
	 * Check for collision with an existing type name. If there is one and
	 * it's an autogenerated array, we can rename it out of the way.  This
	 * check is here mainly to get a better error message about a "type"
	 * instead of below about a "relation".
	 */
	typeNamespace = RangeVarGetAndCheckCreationNamespace(createStmt->relation,
														 NoLock, NULL);
	RangeVarAdjustRelationPersistence(createStmt->relation, typeNamespace);
	old_type_oid =
		GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
						CStringGetDatum(createStmt->relation->relname),
						ObjectIdGetDatum(typeNamespace));
	if (OidIsValid(old_type_oid))
	{
		if (!moveArrayTypeName(old_type_oid, createStmt->relation->relname, typeNamespace))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", createStmt->relation->relname)));
	}

	/*
	 * Finally create the relation.  This also creates the type.
	 */
	DefineRelation(createStmt, RELKIND_COMPOSITE_TYPE, InvalidOid, &address,
				   NULL);

	return address;
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
