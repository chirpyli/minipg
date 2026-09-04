/*-------------------------------------------------------------------------
 *
 * pg_constraint.c
 *	  routines to support manipulation of the pg_constraint relation
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_constraint.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * CreateConstraintEntry
 *	Create a constraint table entry.
 *
 * Subsidiary records (such as triggers or indexes to implement the
 * constraint) are *not* created here.  But we do make dependency links
 * from the constraint to the things it depends on.
 *
 * The new constraint's OID is returned.
 */
Oid
CreateConstraintEntry(const char *constraintName,
					  Oid constraintNamespace,
					  char constraintType,
					  Oid relId,
					  const int16 *constraintKey,
					  int constraintNKeys,
					  int constraintNTotalKeys,
					  Oid indexRelId,
					  bool is_internal)
{
	Relation	conDesc;
	Oid			conOid;
	HeapTuple	tup;
	bool		nulls[Natts_pg_constraint];
	Datum		values[Natts_pg_constraint];
	ArrayType  *conkeyArray;
	NameData	cname;
	int			i;
	ObjectAddress conobject;
	ObjectAddresses *addrs_auto;
	ObjectAddresses *addrs_normal;

	conDesc = table_open(ConstraintRelationId, RowExclusiveLock);

	Assert(constraintName);
	namestrcpy(&cname, constraintName);

	/*
	 * Convert C arrays into Postgres arrays.
	 */
	if (constraintNKeys > 0)
	{
		Datum	   *conkey;

		conkey = (Datum *) palloc(constraintNKeys * sizeof(Datum));
		for (i = 0; i < constraintNKeys; i++)
			conkey[i] = Int16GetDatum(constraintKey[i]);
		conkeyArray = construct_array(conkey, constraintNKeys,
									  INT2OID, 2, true, TYPALIGN_SHORT);
	}
	else
		conkeyArray = NULL;

	/* initialize nulls and values */
	for (i = 0; i < Natts_pg_constraint; i++)
	{
		nulls[i] = false;
		values[i] = (Datum) NULL;
	}

	conOid = GetNewOidWithIndex(conDesc, ConstraintOidIndexId,
								Anum_pg_constraint_oid);
	values[Anum_pg_constraint_oid - 1] = ObjectIdGetDatum(conOid);
	values[Anum_pg_constraint_conname - 1] = NameGetDatum(&cname);
	values[Anum_pg_constraint_connamespace - 1] = ObjectIdGetDatum(constraintNamespace);
	values[Anum_pg_constraint_contype - 1] = CharGetDatum(constraintType);
	values[Anum_pg_constraint_conrelid - 1] = ObjectIdGetDatum(relId);
	values[Anum_pg_constraint_conindid - 1] = ObjectIdGetDatum(indexRelId);

	if (conkeyArray)
		values[Anum_pg_constraint_conkey - 1] = PointerGetDatum(conkeyArray);
	else
		nulls[Anum_pg_constraint_conkey - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(conDesc), values, nulls);

	CatalogTupleInsert(conDesc, tup);

	ObjectAddressSet(conobject, ConstraintRelationId, conOid);

	table_close(conDesc, RowExclusiveLock);

	/* Handle set of auto dependencies */
	addrs_auto = new_object_addresses();

	if (OidIsValid(relId))
	{
		/*
		 * Register auto dependency from constraint to owning relation, or to
		 * specific column(s) if any are mentioned.
		 */
		ObjectAddress relobject;

		if (constraintNTotalKeys > 0)
		{
			for (i = 0; i < constraintNTotalKeys; i++)
			{
				ObjectAddressSubSet(relobject, RelationRelationId, relId,
									constraintKey[i]);
				add_exact_object_address(&relobject, addrs_auto);
			}
		}
		else
		{
			ObjectAddressSet(relobject, RelationRelationId, relId);
			add_exact_object_address(&relobject, addrs_auto);
		}
	}

	record_object_address_dependencies(&conobject, addrs_auto,
									   DEPENDENCY_AUTO);
	free_object_addresses(addrs_auto);

	/* Handle set of normal dependencies */
	addrs_normal = new_object_addresses();

	record_object_address_dependencies(&conobject, addrs_normal,
									   DEPENDENCY_NORMAL);
	free_object_addresses(addrs_normal);

	/*
	 * We don't register dependencies on the operators of a constraint.  We
	 * assume they are members of the opclass supporting the index, so there's
	 * an indirect dependency via that.
	 */

	/* Post creation hook for new constraint */
	InvokeObjectPostCreateHookArg(ConstraintRelationId, conOid, 0,
								  is_internal);

	return conOid;
}

/*
 * Test whether given name is currently used as a constraint name
 * for the given relation.
 *
 * This is used to decide whether to accept a user-specified constraint name.
 * It is deliberately not the same test as ChooseConstraintName uses to decide
 * whether an auto-generated name is OK: here, we will allow it unless there
 * is an identical constraint name in use *on the same relation*.
 *
 * NB: Caller should hold exclusive lock on the given relation, else
 * this test can be fooled by concurrent additions.
 */
bool
ConstraintNameIsUsed(Oid relid,
					 const char *conname)
{
	bool		found;
	Relation	conDesc;
	SysScanDesc conscan;
	ScanKeyData skey[2];

	conDesc = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(conname));

	conscan = systable_beginscan(conDesc, ConstraintRelidNameIndexId,
								 true, NULL, 2, skey);

	/* There can be at most one matching row */
	found = (HeapTupleIsValid(systable_getnext(conscan)));

	systable_endscan(conscan);
	table_close(conDesc, AccessShareLock);

	return found;
}

/*
 * Does any constraint of the given name exist in the given namespace?
 *
 * This is used for code that wants to match ChooseConstraintName's rule
 * that we should avoid autogenerating duplicate constraint names within a
 * namespace.
 */
bool
ConstraintNameExists(const char *conname, Oid namespaceid)
{
	bool		found;
	Relation	conDesc;
	SysScanDesc conscan;
	ScanKeyData skey[2];

	conDesc = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(conname));

	ScanKeyInit(&skey[1],
				Anum_pg_constraint_connamespace,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(namespaceid));

	conscan = systable_beginscan(conDesc, ConstraintNameNspIndexId, true,
								 NULL, 2, skey);

	found = (HeapTupleIsValid(systable_getnext(conscan)));

	systable_endscan(conscan);
	table_close(conDesc, AccessShareLock);

	return found;
}

/*
 * Select a nonconflicting name for a new constraint.
 *
 * The objective here is to choose a name that is unique within the
 * specified namespace.  Postgres does not require this, but the SQL
 * spec does, and some apps depend on it.  Therefore we avoid choosing
 * default names that so conflict.
 *
 * name1, name2, and label are used the same way as for makeObjectName(),
 * except that the label can't be NULL; digits will be appended to the label
 * if needed to create a name that is unique within the specified namespace.
 *
 * 'others' can be a list of string names already chosen within the current
 * command (but not yet reflected into the catalogs); we will not choose
 * a duplicate of one of these either.
 *
 * Note: it is theoretically possible to get a collision anyway, if someone
 * else chooses the same name concurrently.  This is fairly unlikely to be
 * a problem in practice, especially if one is holding an exclusive lock on
 * the relation identified by name1.
 *
 * Returns a palloc'd string.
 */
char *
ChooseConstraintName(const char *name1, const char *name2,
					 const char *label, Oid namespaceid,
					 List *others)
{
	int			pass = 0;
	char	   *conname = NULL;
	char		modlabel[NAMEDATALEN];
	Relation	conDesc;
	SysScanDesc conscan;
	ScanKeyData skey[2];
	bool		found;
	ListCell   *l;

	conDesc = table_open(ConstraintRelationId, AccessShareLock);

	/* try the unmodified label first */
	strlcpy(modlabel, label, sizeof(modlabel));

	for (;;)
	{
		conname = makeObjectName(name1, name2, modlabel);

		found = false;

		foreach(l, others)
		{
			if (strcmp((char *) lfirst(l), conname) == 0)
			{
				found = true;
				break;
			}
		}

		if (!found)
		{
			ScanKeyInit(&skey[0],
						Anum_pg_constraint_conname,
						BTEqualStrategyNumber, F_NAMEEQ,
						CStringGetDatum(conname));

			ScanKeyInit(&skey[1],
						Anum_pg_constraint_connamespace,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(namespaceid));

			conscan = systable_beginscan(conDesc, ConstraintNameNspIndexId, true,
										 NULL, 2, skey);

			found = (HeapTupleIsValid(systable_getnext(conscan)));

			systable_endscan(conscan);
		}

		if (!found)
			break;

		/* found a conflict, so try a new name component */
		pfree(conname);
		snprintf(modlabel, sizeof(modlabel), "%s%d", label, ++pass);
	}

	table_close(conDesc, AccessShareLock);

	return conname;
}

/*
 * Delete a single constraint record.
 */
void
RemoveConstraintById(Oid conId)
{
	Relation	conDesc;
	HeapTuple	tup;
	Form_pg_constraint con;

	conDesc = table_open(ConstraintRelationId, RowExclusiveLock);

	tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(conId));
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for constraint %u", conId);
	con = (Form_pg_constraint) GETSTRUCT(tup);

	/*
	 * Special processing depending on what the constraint is for.
	 */
	if (OidIsValid(con->conrelid))
	{
		Relation	rel;

		/*
		 * If the constraint is for a relation, open and exclusive-lock the
		 * relation it's for.
		 */
		rel = table_open(con->conrelid, AccessExclusiveLock);

		/* Keep lock on constraint's rel until end of xact */
		table_close(rel, NoLock);
	}
	else
		elog(ERROR, "constraint %u is not of a known type", conId);

	/* Fry the constraint itself */
	CatalogTupleDelete(conDesc, &tup->t_self);

	/* Clean up */
	ReleaseSysCache(tup);
	table_close(conDesc, RowExclusiveLock);
}

/*
 * RenameConstraintById
 *		Rename a constraint.
 *
 * Note: this isn't intended to be a user-exposed function; it doesn't check
 * permissions etc.  Currently this is only invoked when renaming an index
 * that is associated with a constraint, but it's made a little more general
 * than that with the expectation of someday having ALTER TABLE RENAME
 * CONSTRAINT.
 */
void
RenameConstraintById(Oid conId, const char *newname)
{
	Relation	conDesc;
	HeapTuple	tuple;
	Form_pg_constraint con;

	conDesc = table_open(ConstraintRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(CONSTROID, ObjectIdGetDatum(conId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for constraint %u", conId);
	con = (Form_pg_constraint) GETSTRUCT(tuple);

	/*
	 * For user-friendliness, check whether the name is already in use.
	 */
	if (OidIsValid(con->conrelid) &&
		ConstraintNameIsUsed(con->conrelid, newname))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("constraint \"%s\" for relation \"%s\" already exists",
						newname, get_rel_name(con->conrelid))));

	/* OK, do the rename --- tuple is a copy, so OK to scribble on it */
	namestrcpy(&(con->conname), newname);

	CatalogTupleUpdate(conDesc, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(ConstraintRelationId, conId, 0);

	heap_freetuple(tuple);
	table_close(conDesc, RowExclusiveLock);
}

/*
 * AlterConstraintNamespaces
 *		Find any constraints belonging to the specified relation,
 *		and move them to the specified new namespace.
 */
void
AlterConstraintNamespaces(Oid ownerId, Oid oldNspId,
						  Oid newNspId, ObjectAddresses *objsMoved)
{
	Relation	conRel;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tup;

	conRel = table_open(ConstraintRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ownerId));

	scan = systable_beginscan(conRel, ConstraintRelidNameIndexId, true,
							  NULL, 1, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_constraint conform = (Form_pg_constraint) GETSTRUCT(tup);
		ObjectAddress thisobj;

		ObjectAddressSet(thisobj, ConstraintRelationId, conform->oid);

		if (object_address_present(&thisobj, objsMoved))
			continue;

		/* Don't update if the object is already part of the namespace */
		if (conform->connamespace == oldNspId && oldNspId != newNspId)
		{
			tup = heap_copytuple(tup);
			conform = (Form_pg_constraint) GETSTRUCT(tup);

			conform->connamespace = newNspId;

			CatalogTupleUpdate(conRel, &tup->t_self, tup);

			/*
			 * Note: currently, the constraint will not have its own
			 * dependency on the namespace, so we don't need to do
			 * changeDependencyFor().
			 */
		}

		InvokeObjectPostAlterHook(ConstraintRelationId, thisobj.objectId, 0);

		add_exact_object_address(&thisobj, objsMoved);
	}

	systable_endscan(scan);

	table_close(conRel, RowExclusiveLock);
}

/*
 * get_relation_constraint_oid
 *		Find a constraint on the specified relation with the specified name.
 *		Returns constraint's OID.
 */
Oid
get_relation_constraint_oid(Oid relid, const char *conname, bool missing_ok)
{
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[2];
	Oid			conOid = InvalidOid;

	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(conname));

	scan = systable_beginscan(pg_constraint, ConstraintRelidNameIndexId, true,
							  NULL, 2, skey);

	/* There can be at most one matching row */
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
		conOid = ((Form_pg_constraint) GETSTRUCT(tuple))->oid;

	systable_endscan(scan);

	/* If no such constraint exists, complain */
	if (!OidIsValid(conOid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" for table \"%s\" does not exist",
						conname, get_rel_name(relid))));

	table_close(pg_constraint, AccessShareLock);

	return conOid;
}

/*
 * get_relation_constraint_attnos
 *		Find a constraint on the specified relation with the specified name
 *		and return the constrained columns.
 *
 * Returns a Bitmapset of the column attnos of the constrained columns, with
 * attnos being offset by FirstLowInvalidHeapAttributeNumber so that system
 * columns can be represented.
 *
 * *constraintOid is set to the OID of the constraint, or InvalidOid on
 * failure.
 */
Bitmapset *
get_relation_constraint_attnos(Oid relid, const char *conname,
							   bool missing_ok, Oid *constraintOid)
{
	Bitmapset  *conattnos = NULL;
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[2];

	/* Set *constraintOid, to avoid complaints about uninitialized vars */
	*constraintOid = InvalidOid;

	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(conname));

	scan = systable_beginscan(pg_constraint, ConstraintRelidNameIndexId, true,
							  NULL, 2, skey);

	/* There can be at most one matching row */
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Datum		adatum;
		bool		isNull;

		*constraintOid = ((Form_pg_constraint) GETSTRUCT(tuple))->oid;

		/* Extract the conkey array, ie, attnums of constrained columns */
		adatum = heap_getattr(tuple, Anum_pg_constraint_conkey,
							  RelationGetDescr(pg_constraint), &isNull);
		if (!isNull)
		{
			ArrayType  *arr;
			int			numcols;
			int16	   *attnums;
			int			i;

			arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
			numcols = ARR_DIMS(arr)[0];
			if (ARR_NDIM(arr) != 1 ||
				numcols < 0 ||
				ARR_HASNULL(arr) ||
				ARR_ELEMTYPE(arr) != INT2OID)
				elog(ERROR, "conkey is not a 1-D smallint array");
			attnums = (int16 *) ARR_DATA_PTR(arr);

			/* Construct the result value */
			for (i = 0; i < numcols; i++)
			{
				conattnos = bms_add_member(conattnos,
										   attnums[i] - FirstLowInvalidHeapAttributeNumber);
			}
		}
	}

	systable_endscan(scan);

	/* If no such constraint exists, complain */
	if (!OidIsValid(*constraintOid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" for table \"%s\" does not exist",
						conname, get_rel_name(relid))));

	table_close(pg_constraint, AccessShareLock);

	return conattnos;
}

/*
 * Return the OID of the constraint enforced by the given index in the
 * given relation; or InvalidOid if no such index is catalogued.
 *
 * Much like get_constraint_index, this function is concerned only with the
 * one constraint that "owns" the given index.  Therefore, constraints of
 * types other than unique, primary-key, and exclusion are ignored.
 */
Oid
get_relation_idx_constraint_oid(Oid relationId, Oid indexId)
{
	Relation	pg_constraint;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;
	Oid			constraintId = InvalidOid;

	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(relationId));
	scan = systable_beginscan(pg_constraint, ConstraintRelidNameIndexId,
							  true, NULL, 1, &key);
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint constrForm;

		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/* See above */
		if (constrForm->contype != CONSTRAINT_PRIMARY &&
			constrForm->contype != CONSTRAINT_UNIQUE)
			continue;

		if (constrForm->conindid == indexId)
		{
			constraintId = constrForm->oid;
			break;
		}
	}
	systable_endscan(scan);

	table_close(pg_constraint, AccessShareLock);
	return constraintId;
}

/*
 * get_primary_key_attnos
 *		Identify the columns in a relation's primary key, if any.
 *
 * Returns a Bitmapset of the column attnos of the primary key's columns,
 * with attnos being offset by FirstLowInvalidHeapAttributeNumber so that
 * system columns can be represented.
 *
 * If there is no primary key, return NULL.
 *
 * *constraintOid is set to the OID of the pkey constraint, or InvalidOid
 * on failure.
 */
Bitmapset *
get_primary_key_attnos(Oid relid, Oid *constraintOid)
{
	Bitmapset  *pkattnos = NULL;
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[1];

	/* Set *constraintOid, to avoid complaints about uninitialized vars */
	*constraintOid = InvalidOid;

	/* Scan pg_constraint for constraints of the target rel */
	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	scan = systable_beginscan(pg_constraint, ConstraintRelidNameIndexId, true,
							  NULL, 1, skey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);
		Datum		adatum;
		bool		isNull;
		ArrayType  *arr;
		int16	   *attnums;
		int			numkeys;
		int			i;

		/* Skip constraints that are not PRIMARY KEYs */
		if (con->contype != CONSTRAINT_PRIMARY)
			continue;

		/* Extract the conkey array, ie, attnums of PK's columns */
		adatum = heap_getattr(tuple, Anum_pg_constraint_conkey,
							  RelationGetDescr(pg_constraint), &isNull);
		if (isNull)
			elog(ERROR, "null conkey for constraint %u",
				 ((Form_pg_constraint) GETSTRUCT(tuple))->oid);
		arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
		numkeys = ARR_DIMS(arr)[0];
		if (ARR_NDIM(arr) != 1 ||
			numkeys < 0 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != INT2OID)
			elog(ERROR, "conkey is not a 1-D smallint array");
		attnums = (int16 *) ARR_DATA_PTR(arr);

		/* Construct the result value */
		for (i = 0; i < numkeys; i++)
		{
			pkattnos = bms_add_member(pkattnos,
									  attnums[i] - FirstLowInvalidHeapAttributeNumber);
		}
		*constraintOid = ((Form_pg_constraint) GETSTRUCT(tuple))->oid;

		/* No need to search further */
		break;
	}

	systable_endscan(scan);

	table_close(pg_constraint, AccessShareLock);

	return pkattnos;
}

/*
 * Determine whether a relation can be proven functionally dependent on
 * a set of grouping columns.  If so, return true and add the pg_constraint
 * OIDs of the constraints needed for the proof to the *constraintDeps list.
 *
 * grouping_columns is a list of grouping expressions, in which columns of
 * the rel of interest are Vars with the indicated varno/varlevelsup.
 *
 * Currently we only check to see if the rel has a primary key that is a
 * subset of the grouping_columns.  We could also use plain unique constraints
 * if all their columns are known not null, but there's a problem: we need
 * to be able to represent the not-null-ness as part of the constraints added
 * to *constraintDeps.  FIXME whenever not-null constraints get represented
 * in pg_constraint.
 */
bool
check_functional_grouping(Oid relid,
						  Index varno, Index varlevelsup,
						  List *grouping_columns,
						  List **constraintDeps)
{
	Bitmapset  *pkattnos;
	Bitmapset  *groupbyattnos;
	Oid			constraintOid;
	ListCell   *gl;

	/* If the rel has no PK, then we can't prove functional dependency */
	pkattnos = get_primary_key_attnos(relid, &constraintOid);
	if (pkattnos == NULL)
		return false;

	/* Identify all the rel's columns that appear in grouping_columns */
	groupbyattnos = NULL;
	foreach(gl, grouping_columns)
	{
		Var		   *gvar = (Var *) lfirst(gl);

		if (IsA(gvar, Var) &&
			gvar->varno == varno &&
			gvar->varlevelsup == varlevelsup)
			groupbyattnos = bms_add_member(groupbyattnos,
										   gvar->varattno - FirstLowInvalidHeapAttributeNumber);
	}

	if (bms_is_subset(pkattnos, groupbyattnos))
	{
		/* The PK is a subset of grouping_columns, so we win */
		*constraintDeps = lappend_oid(*constraintDeps, constraintOid);
		return true;
	}

	return false;
}
