/*-------------------------------------------------------------------------
 *
 * typecmds.h
 *	  prototypes for typecmds.c.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/typecmds.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TYPECMDS_H
#define TYPECMDS_H

#include "access/htup.h"
#include "catalog/dependency.h"
#include "parser/parse_node.h"


#define DEFAULT_TYPDELIM		','

extern void RemoveTypeById(Oid typeOid);
extern ObjectAddress DefineDomain(CreateDomainStmt *stmt);
extern ObjectAddress DefineEnum(CreateEnumStmt *stmt);
extern ObjectAddress DefineRange(CreateRangeStmt *stmt);
extern ObjectAddress DefineCompositeType(RangeVar *typevar, List *coldeflist);
extern Oid	AssignTypeArrayOid(void);
extern Oid	AssignTypeMultirangeOid(void);
extern Oid	AssignTypeMultirangeArrayOid(void);

extern void checkDomainOwner(HeapTuple tup);

extern ObjectAddress AlterTypeOwner(List *names, Oid newOwnerId, ObjectType objecttype);
extern void AlterTypeOwner_oid(Oid typeOid, Oid newOwnerId, bool hasDependEntry);
extern void AlterTypeOwnerInternal(Oid typeOid, Oid newOwnerId);

extern ObjectAddress AlterTypeNamespace(List *names, const char *newschema,
										ObjectType objecttype, Oid *oldschema);
extern Oid	AlterTypeNamespace_oid(Oid typeOid, Oid nspOid, ObjectAddresses *objsMoved);
extern Oid	AlterTypeNamespaceInternal(Oid typeOid, Oid nspOid,
									   bool isImplicitArray,
									   bool errorOnTableType,
									   ObjectAddresses *objsMoved);

#endif							/* TYPECMDS_H */
