/*-------------------------------------------------------------------------
 *
 * pg_depend.c
 *	  routines to support manipulation of the pg_depend relation
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_depend.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


static bool isObjectPinned(const ObjectAddress *object, Relation rel);
static void dependencyLockAndCheckObject(Oid classId, Oid objectId);


/*
 * Record a dependency between 2 objects via their respective objectAddress.
 * The first argument is the dependent object, the second the one it
 * references.
 *
 * This simply creates an entry in pg_depend, without any other processing.
 */
void
recordDependencyOn(const ObjectAddress *depender,
				   const ObjectAddress *referenced,
				   DependencyType behavior)
{
	recordMultipleDependencies(depender, referenced, 1, behavior);
}

/*
 * Record multiple dependencies (of the same kind) for a single dependent
 * object.  This has a little less overhead than recording each separately.
 */
void
recordMultipleDependencies(const ObjectAddress *depender,
						   const ObjectAddress *referenced,
						   int nreferenced,
						   DependencyType behavior)
{
	Relation	dependDesc;
	CatalogIndexState indstate;
	TupleTableSlot **slot;
	int			i,
				max_slots,
				slot_init_count,
				slot_stored_count;

	if (nreferenced <= 0)
		return;					/* nothing to do */

	/*
	 * During bootstrap, do nothing since pg_depend may not exist yet. initdb
	 * will fill in appropriate pg_depend entries after bootstrap.
	 */
	if (IsBootstrapProcessingMode())
		return;

	dependDesc = table_open(DependRelationId, RowExclusiveLock);

	/*
	 * Allocate the slots to use, but delay costly initialization until we
	 * know that they will be used.
	 */
	max_slots = Min(nreferenced,
					MAX_CATALOG_MULTI_INSERT_BYTES / sizeof(FormData_pg_depend));
	slot = palloc(sizeof(TupleTableSlot *) * max_slots);

	/* Don't open indexes unless we need to make an update */
	indstate = NULL;

	/* number of slots currently storing tuples */
	slot_stored_count = 0;
	/* number of slots currently initialized */
	slot_init_count = 0;
	for (i = 0; i < nreferenced; i++, referenced++)
	{
		/*
		 * If the referenced object is pinned by the system, there's no real
		 * need to record dependencies on it.  This saves lots of space in
		 * pg_depend, so it's worth the time taken to check.
		 */
		if (isObjectPinned(referenced, dependDesc))
			continue;

		/*
		 * Make sure the new referenced object doesn't go away while we record
		 * the dependency.  DROP routines should lock the object exclusively
		 * before they check dependencies.
		 */
		dependencyLockAndCheckObject(referenced->classId, referenced->objectId);

		if (slot_init_count < max_slots)
		{
			slot[slot_stored_count] = MakeSingleTupleTableSlot(RelationGetDescr(dependDesc),
															   &TTSOpsHeapTuple);
			slot_init_count++;
		}

		ExecClearTuple(slot[slot_stored_count]);

		/*
		 * Record the dependency.  Note we don't bother to check for duplicate
		 * dependencies; there's no harm in them.
		 */
		slot[slot_stored_count]->tts_values[Anum_pg_depend_refclassid - 1] = ObjectIdGetDatum(referenced->classId);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_refobjid - 1] = ObjectIdGetDatum(referenced->objectId);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_refobjsubid - 1] = Int32GetDatum(referenced->objectSubId);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_deptype - 1] = CharGetDatum((char) behavior);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_classid - 1] = ObjectIdGetDatum(depender->classId);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_objid - 1] = ObjectIdGetDatum(depender->objectId);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_objsubid - 1] = Int32GetDatum(depender->objectSubId);

		memset(slot[slot_stored_count]->tts_isnull, false,
			   slot[slot_stored_count]->tts_tupleDescriptor->natts * sizeof(bool));

		ExecStoreVirtualTuple(slot[slot_stored_count]);
		slot_stored_count++;

		/* If slots are full, insert a batch of tuples */
		if (slot_stored_count == max_slots)
		{
			/* fetch index info only when we know we need it */
			if (indstate == NULL)
				indstate = CatalogOpenIndexes(dependDesc);

			CatalogTuplesMultiInsertWithInfo(dependDesc, slot, slot_stored_count,
											 indstate);
			slot_stored_count = 0;
		}
	}

	/* Insert any tuples left in the buffer */
	if (slot_stored_count > 0)
	{
		/* fetch index info only when we know we need it */
		if (indstate == NULL)
			indstate = CatalogOpenIndexes(dependDesc);

		CatalogTuplesMultiInsertWithInfo(dependDesc, slot, slot_stored_count,
										 indstate);
	}

	if (indstate != NULL)
		CatalogCloseIndexes(indstate);

	table_close(dependDesc, RowExclusiveLock);

	/* Drop only the number of slots used */
	for (i = 0; i < slot_init_count; i++)
		ExecDropSingleTupleTableSlot(slot[i]);
	pfree(slot);
}

/*
 * deleteDependencyRecordsFor -- delete all records with given depender
 * classId/objectId.  Returns the number of records deleted.
 *
 * This is used when redefining an existing object.  Links leading to the
 * object do not change, and links leading from it will be recreated
 * (possibly with some differences from before).
 */
long
deleteDependencyRecordsFor(Oid classId, Oid objectId)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		CatalogTupleDelete(depRel, &tup->t_self);
		count++;
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * deleteDependencyRecordsForClass -- delete all records with given depender
 * classId/objectId, dependee classId, and deptype.
 * Returns the number of records deleted.
 *
 * This is a variant of deleteDependencyRecordsFor, useful when revoking
 * an object property that is expressed by a dependency record.
 */
long
deleteDependencyRecordsForClass(Oid classId, Oid objectId,
								Oid refclassId, char deptype)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

		if (depform->refclassid == refclassId && depform->deptype == deptype)
		{
			CatalogTupleDelete(depRel, &tup->t_self);
			count++;
		}
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * deleteDependencyRecordsForSpecific -- delete all records with given depender
 * classId/objectId, dependee classId/objectId, of the given deptype.
 * Returns the number of records deleted.
 */
long
deleteDependencyRecordsForSpecific(Oid classId, Oid objectId, char deptype,
								   Oid refclassId, Oid refobjectId)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

		if (depform->refclassid == refclassId &&
			depform->refobjid == refobjectId &&
			depform->deptype == deptype)
		{
			CatalogTupleDelete(depRel, &tup->t_self);
			count++;
		}
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * Adjust dependency record(s) to point to a different object of the same type
 *
 * classId/objectId specify the referencing object.
 * refClassId/oldRefObjectId specify the old referenced object.
 * newRefObjectId is the new referenced object (must be of class refClassId).
 *
 * Note the lack of objsubid parameters.  If there are subobject references
 * they will all be readjusted.  Also, there is an expectation that we are
 * dealing with NORMAL dependencies: if we have to replace an (implicit)
 * dependency on a pinned object with an explicit dependency on an unpinned
 * one, the new one will be NORMAL.
 *
 * Returns the number of records updated -- zero indicates a problem.
 */
long
changeDependencyFor(Oid classId, Oid objectId,
					Oid refClassId, Oid oldRefObjectId,
					Oid newRefObjectId)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	ObjectAddress objAddr;
	ObjectAddress depAddr;
	bool		oldIsPinned;
	bool		newIsPinned;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	/*
	 * Check to see if either oldRefObjectId or newRefObjectId is pinned.
	 * Pinned objects should not have any dependency entries pointing to them,
	 * so in these cases we should add or remove a pg_depend entry, or do
	 * nothing at all, rather than update an entry as in the normal case.
	 */
	objAddr.classId = refClassId;
	objAddr.objectId = oldRefObjectId;
	objAddr.objectSubId = 0;

	oldIsPinned = isObjectPinned(&objAddr, depRel);

	objAddr.objectId = newRefObjectId;

	newIsPinned = isObjectPinned(&objAddr, depRel);

	if (oldIsPinned)
	{
		table_close(depRel, RowExclusiveLock);

		/*
		 * If both are pinned, we need do nothing.  However, return 1 not 0,
		 * else callers will think this is an error case.
		 */
		if (newIsPinned)
			return 1;

		/*
		 * There is no old dependency record, but we should insert a new one.
		 * Assume a normal dependency is wanted.
		 */
		depAddr.classId = classId;
		depAddr.objectId = objectId;
		depAddr.objectSubId = 0;
		recordDependencyOn(&depAddr, &objAddr, DEPENDENCY_NORMAL);

		return 1;
	}

	/*
	 * Make sure the new referenced object doesn't go away while we record the
	 * dependency.
	 */
	if (!newIsPinned)
		dependencyLockAndCheckObject(refClassId, newRefObjectId);

	/* There should be existing dependency record(s), so search. */
	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

		if (depform->refclassid == refClassId &&
			depform->refobjid == oldRefObjectId)
		{
			if (newIsPinned)
				CatalogTupleDelete(depRel, &tup->t_self);
			else
			{
				/* make a modifiable copy */
				tup = heap_copytuple(tup);
				depform = (Form_pg_depend) GETSTRUCT(tup);

				depform->refobjid = newRefObjectId;

				CatalogTupleUpdate(depRel, &tup->t_self, tup);

				heap_freetuple(tup);
			}

			count++;
		}
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * Adjust all dependency records to come from a different object of the same type
 *
 * classId/oldObjectId specify the old referencing object.
 * newObjectId is the new referencing object (must be of class classId).
 *
 * Returns the number of records updated.
 */
long
changeDependenciesOf(Oid classId, Oid oldObjectId,
					 Oid newObjectId)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oldObjectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_depend depform;

		/* make a modifiable copy */
		tup = heap_copytuple(tup);
		depform = (Form_pg_depend) GETSTRUCT(tup);

		depform->objid = newObjectId;

		CatalogTupleUpdate(depRel, &tup->t_self, tup);

		heap_freetuple(tup);

		count++;
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * Adjust all dependency records to point to a different object of the same type
 *
 * refClassId/oldRefObjectId specify the old referenced object.
 * newRefObjectId is the new referenced object (must be of class refClassId).
 *
 * Returns the number of records updated.
 */
long
changeDependenciesOn(Oid refClassId, Oid oldRefObjectId,
					 Oid newRefObjectId)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	ObjectAddress objAddr;
	bool		newIsPinned;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	/*
	 * If oldRefObjectId is pinned, there won't be any dependency entries on
	 * it --- we can't cope in that case.  (This isn't really worth expending
	 * code to fix, in current usage; it just means you can't rename stuff out
	 * of pg_catalog, which would likely be a bad move anyway.)
	 */
	objAddr.classId = refClassId;
	objAddr.objectId = oldRefObjectId;
	objAddr.objectSubId = 0;

	if (isObjectPinned(&objAddr, depRel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot remove dependency on %s because it is a system object",
						getObjectDescription(&objAddr, false))));

	/*
	 * We can handle adding a dependency on something pinned, though, since
	 * that just means deleting the dependency entry.
	 */
	objAddr.objectId = newRefObjectId;

	newIsPinned = isObjectPinned(&objAddr, depRel);

	/* Now search for dependency records */
	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(refClassId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oldRefObjectId));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		if (newIsPinned)
			CatalogTupleDelete(depRel, &tup->t_self);
		else
		{
			Form_pg_depend depform;

			/* make a modifiable copy */
			tup = heap_copytuple(tup);
			depform = (Form_pg_depend) GETSTRUCT(tup);

			depform->refobjid = newRefObjectId;

			CatalogTupleUpdate(depRel, &tup->t_self, tup);

			heap_freetuple(tup);
		}

		count++;
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * isObjectPinned()
 *
 * Test if an object is required for basic database functionality.
 * Caller must already have opened pg_depend.
 *
 * The passed subId, if any, is ignored; we assume that only whole objects
 * are pinned (and that this implies pinning their components).
 */
static bool
isObjectPinned(const ObjectAddress *object, Relation rel)
{
	bool		ret = false;
	SysScanDesc scan;
	HeapTuple	tup;
	ScanKeyData key[2];

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));

	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));

	scan = systable_beginscan(rel, DependReferenceIndexId, true,
							  NULL, 2, key);

	/*
	 * Since we won't generate additional pg_depend entries for pinned
	 * objects, there can be at most one entry referencing a pinned object.
	 * Hence, it's sufficient to look at the first returned tuple; we don't
	 * need to loop.
	 */
	tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(tup);

		if (foundDep->deptype == DEPENDENCY_PIN)
			ret = true;
	}

	systable_endscan(scan);

	return ret;
}


/*
 * dependencyLockAndCheckObject
 *
 * Lock the object that we are about to record a dependency on.  After it's
 * locked, verify that it hasn't been dropped while we weren't looking.  If it
 * has been dropped, throw an an error.
 *
 * If the caller already holds a lock that conflicts with DROP
 * (AccessShareLock or stronger), this does nothing.  Callers should acquire
 * locks already when they look up the referenced objects, but many callers
 * currently do not.  This is a backstop to make sure that we don't record a
 * bogus reference permanently in the catalogs in that case.  In the future,
 * after we have tightened up all the callers to acquire locks earlier, this
 * could just verify that the object is already locked and throw an error if
 * not.
 */
static void
dependencyLockAndCheckObject(Oid classId, Oid objectId)
{
	/*
	 * Note: Pinned objects cannot be dropped concurrently, and callers
	 * checked this already.  objectId really should be valid too, but when
	 * this locking was added there were some corner cases where we created a
	 * transient dependency on InvalidOid.  Probably shouldn't happen anymore,
	 * but let's tolerate it.
	 */
	if (!OidIsValid(objectId))
		return;

	if (classId != RelationRelationId)
	{
		LOCKTAG		tag;
		int			cache;
		Relation	rel;
		SysScanDesc scan;
		ScanKeyData skey;
		HeapTuple	tuple;

		SET_LOCKTAG_OBJECT(tag,
						   MyDatabaseId,
						   classId,
						   objectId,
						   0);

		if (LockOrStrongerHeldByMe(&tag, AccessShareLock))
			return;

		/* Assume we should lock the whole object not a sub-object */
		LockDatabaseObject(classId, objectId, 0, AccessShareLock);

		/*
		 * Check that the object still exists.  If the catalog has a suitable
		 * syscache, check that first.
		 */
		cache = get_object_catcache_oid(classId);
		if (cache != -1)
		{
			if (SearchSysCacheExists1(cache, ObjectIdGetDatum(objectId)))
				return;
		}

		/*
		 * If it's not found in the syscache, or there's no suitable syscache
		 * we can use, scan the catalog table using SnapshotSelf.  This
		 * handles the case that it's an object we just created (for example,
		 * if it's a composite type created as part of creating a table).
		 */
		rel = table_open(classId, AccessShareLock);

		ScanKeyInit(&skey,
					get_object_attnum_oid(classId),
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(objectId));

		scan = systable_beginscan(rel, get_object_oid_index(classId),
								  true, SnapshotSelf, 1, &skey);

		tuple = systable_getnext(scan);
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("referenced %s was concurrently dropped",
							get_object_class_descr(classId))));

		systable_endscan(scan);
		table_close(rel, AccessShareLock);
	}
	else if (IsSharedRelation(objectId))
	{
		/* Shared relations are considered pinned, ignore them */
	}
	else
	{
		/*
		 * Same logic for pg_class entries, but locking relations is handled
		 * by different functions.
		 *
		 * Callers are more careful with locking relations than other objects,
		 * so we should already have a lock on the relation, or on another
		 * object that indirectly prevents the relation from being dropped.
		 * For example, we might have a strong lock on a table while adding
		 * dependency to its index.  However, we cannot detect the indirectly
		 * protected case here easily.  To err on the safe side, acquire a
		 * lock directly on the relation if we're not holding one already.
		 */
		if (CheckRelationOidLockedByMe(objectId, AccessShareLock, true))
			return;
		LockRelationOid(objectId, AccessShareLock);

		if (SearchSysCacheExists1(RELOID, ObjectIdGetDatum(objectId)))
			return;
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("referenced relation was concurrently dropped")));
	}
}

/*
 * Various special-purpose lookups and manipulations of pg_depend.
 */


/*
 * get_index_constraint
 *		Given the OID of an index, return the OID of the owning unique,
 *		primary-key, or exclusion constraint, or InvalidOid if there
 *		is no owning constraint.
 */
Oid
get_index_constraint(Oid indexId)
{
	Oid			constraintId = InvalidOid;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;

	/* Search the dependency table for the index */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(indexId));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend deprec = (Form_pg_depend) GETSTRUCT(tup);

		/*
		 * We assume any internal dependency on a constraint must be what we
		 * are looking for.
		 */
		if (deprec->refclassid == ConstraintRelationId &&
			deprec->refobjsubid == 0 &&
			deprec->deptype == DEPENDENCY_INTERNAL)
		{
			constraintId = deprec->refobjid;
			break;
		}
	}

	systable_endscan(scan);
	table_close(depRel, AccessShareLock);

	return constraintId;
}

/*
 * get_index_ref_constraints
 *		Given the OID of an index, return the OID of all constraints which
 *		reference the index (foreign-key constraints in a full build; none in
 *		this trimmed build where FK enforcement has been removed).
 */
List *
get_index_ref_constraints(Oid indexId)
{
	List	   *result = NIL;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;

	/* Search the dependency table for the index */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(indexId));
	ScanKeyInit(&key[2],
				Anum_pg_depend_refobjsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend deprec = (Form_pg_depend) GETSTRUCT(tup);

		/*
		 * We assume any normal dependency from a constraint must be what we
		 * are looking for.
		 */
		if (deprec->classid == ConstraintRelationId &&
			deprec->objsubid == 0 &&
			deprec->deptype == DEPENDENCY_NORMAL)
		{
			result = lappend_oid(result, deprec->objid);
		}
	}

	systable_endscan(scan);
	table_close(depRel, AccessShareLock);

	return result;
}
