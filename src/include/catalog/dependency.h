/*-------------------------------------------------------------------------
 *
 * dependency.h
 *	  Routines to support inter-object dependencies.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/dependency.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DEPENDENCY_H
#define DEPENDENCY_H

#include "catalog/objectaddress.h"


/*
 * Precise semantics of a dependency relationship are specified by the
 * DependencyType code (which is stored in a "char" field in pg_depend,
 * so we assign ASCII-code values to the enumeration members).
 *
 * In all cases, a dependency relationship indicates that the referenced
 * object may not be dropped without also dropping the dependent object.
 * However, there are several subflavors; see the description of pg_depend
 * in catalogs.sgml for details.
 */

typedef enum DependencyType
{
	DEPENDENCY_NORMAL = 'n',
	DEPENDENCY_AUTO = 'a',
	DEPENDENCY_INTERNAL = 'i',
	DEPENDENCY_EXTENSION = 'e',
	DEPENDENCY_AUTO_EXTENSION = 'x',
	DEPENDENCY_PIN = 'p'
} DependencyType;

/* expansible list of ObjectAddresses (private in dependency.c) */
typedef struct ObjectAddresses ObjectAddresses;

/*
 * This enum covers all system catalogs whose OIDs can appear in
 * pg_depend.classId.  Keep object_classes[] in sync.
 */
typedef enum ObjectClass
{
	OCLASS_CLASS,				/* pg_class */
	OCLASS_PROC,				/* pg_proc */
	OCLASS_TYPE,				/* pg_type */
	OCLASS_CAST,				/* pg_cast */
	OCLASS_COLLATION,			/* pg_collation */
	OCLASS_CONSTRAINT,			/* pg_constraint */
	OCLASS_CONVERSION,			/* pg_conversion */
	OCLASS_LANGUAGE,			/* pg_language */
	OCLASS_OPERATOR,			/* pg_operator */
	OCLASS_OPCLASS,				/* pg_opclass */
	OCLASS_OPFAMILY,			/* pg_opfamily */
	OCLASS_AM,					/* pg_am */
	OCLASS_AMOP,				/* pg_amop */
	OCLASS_AMPROC,				/* pg_amproc */
	OCLASS_REWRITE,				/* pg_rewrite */
	OCLASS_SCHEMA,				/* pg_namespace */
	OCLASS_DATABASE,			/* pg_database */
	OCLASS_EXTENSION,			/* pg_extension */
} ObjectClass;

#define LAST_OCLASS		OCLASS_EXTENSION

/* flag bits for performDeletion/performMultipleDeletions: */
#define PERFORM_DELETION_INTERNAL			0x0001	/* internal action */
#define PERFORM_DELETION_CONCURRENTLY		0x0002	/* concurrent drop */
#define PERFORM_DELETION_QUIETLY			0x0004	/* suppress notices */
#define PERFORM_DELETION_SKIP_ORIGINAL		0x0008	/* keep original obj */
#define PERFORM_DELETION_SKIP_EXTENSIONS	0x0010	/* keep extensions */
#define PERFORM_DELETION_CONCURRENT_LOCK	0x0020	/* normal drop with
													 * concurrent lock mode */


/* in dependency.c */

extern void AcquireDeletionLock(const ObjectAddress *object, int flags);

extern void ReleaseDeletionLock(const ObjectAddress *object);

extern void performDeletion(const ObjectAddress *object,
							DropBehavior behavior, int flags);

extern void performMultipleDeletions(const ObjectAddresses *objects,
									 DropBehavior behavior, int flags);

extern void recordDependencyOnExpr(const ObjectAddress *depender,
								   Node *expr, List *rtable,
								   DependencyType behavior);

extern void recordDependencyOnSingleRelExpr(const ObjectAddress *depender,
											Node *expr, Oid relId,
											DependencyType behavior,
											DependencyType self_behavior,
											bool reverse_self);

extern ObjectClass getObjectClass(const ObjectAddress *object);

extern ObjectAddresses *new_object_addresses(void);

extern void add_exact_object_address(const ObjectAddress *object,
									 ObjectAddresses *addrs);

extern bool object_address_present(const ObjectAddress *object,
								   const ObjectAddresses *addrs);

extern void record_object_address_dependencies(const ObjectAddress *depender,
											   ObjectAddresses *referenced,
											   DependencyType behavior);

extern void sort_object_addresses(ObjectAddresses *addrs);

extern void free_object_addresses(ObjectAddresses *addrs);

/* in pg_depend.c */

extern void recordDependencyOn(const ObjectAddress *depender,
							   const ObjectAddress *referenced,
							   DependencyType behavior);

extern void recordMultipleDependencies(const ObjectAddress *depender,
									   const ObjectAddress *referenced,
									   int nreferenced,
									   DependencyType behavior);

extern void recordDependencyOnCurrentExtension(const ObjectAddress *object,
											   bool isReplace);

extern void checkMembershipInCurrentExtension(const ObjectAddress *object);

extern long deleteDependencyRecordsFor(Oid classId, Oid objectId,
									   bool skipExtensionDeps);

extern long deleteDependencyRecordsForClass(Oid classId, Oid objectId,
											Oid refclassId, char deptype);

extern long deleteDependencyRecordsForSpecific(Oid classId, Oid objectId,
											   char deptype,
											   Oid refclassId, Oid refobjectId);

extern long changeDependencyFor(Oid classId, Oid objectId,
								Oid refClassId, Oid oldRefObjectId,
								Oid newRefObjectId);

extern long changeDependenciesOf(Oid classId, Oid oldObjectId,
								 Oid newObjectId);

extern long changeDependenciesOn(Oid refClassId, Oid oldRefObjectId,
								 Oid newRefObjectId);

extern Oid	getExtensionOfObject(Oid classId, Oid objectId);
extern List *getAutoExtensionsOfObject(Oid classId, Oid objectId);

extern Oid	getExtensionType(Oid extensionOid, const char *typname);

extern Oid	get_index_constraint(Oid indexId);

extern List *get_index_ref_constraints(Oid indexId);

#endif							/* DEPENDENCY_H */
