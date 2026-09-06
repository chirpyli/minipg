/*-------------------------------------------------------------------------
 *
 * heap.h
 *	  prototypes for functions in backend/catalog/heap.c
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/heap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAP_H
#define HEAP_H

#include "catalog/indexing.h"
#include "catalog/objectaddress.h"
#include "parser/parse_node.h"


/* flag bits for CheckAttributeType/CheckAttributeNamesTypes */
#define CHKATYPE_ANYARRAY		0x01	/* allow ANYARRAY */
#define CHKATYPE_ANYRECORD		0x02	/* allow RECORD and RECORD[] */

extern Relation heap_create(const char *relname,
							Oid relnamespace,
							Oid reltablespace,
							Oid relid,
							Oid relfilenode,
							Oid accessmtd,
							TupleDesc tupDesc,
							char relkind,
							char relpersistence,
							bool shared_relation,
							bool mapped_relation,
							bool allow_system_table_mods,
							TransactionId *relfrozenxid,
							MultiXactId *relminmxid);

extern Oid	heap_create_with_catalog(const char *relname,
									 Oid relnamespace,
									 Oid reltablespace,
									 Oid relid,
									 Oid reltypeid,
									 Oid ownerid,
									 Oid accessmtd,
									 TupleDesc tupdesc,
									 char relkind,
									 char relpersistence,
									 bool shared_relation,
									 bool mapped_relation,
									 OnCommitAction oncommit,
									 bool allow_system_table_mods,
									 bool is_internal,
									 Oid relrewrite,
									 ObjectAddress *typaddress);

extern void heap_drop_with_catalog(Oid relid);

extern void heap_truncate(List *relids);

extern void heap_truncate_one_rel(Relation rel);

extern void InsertPgAttributeTuples(Relation pg_attribute_rel,
									TupleDesc tupdesc,
									Oid new_rel_oid,
									Datum *attoptions,
									CatalogIndexState indstate);

extern void InsertPgClassTuple(Relation pg_class_desc,
							   Relation new_rel_desc,
							   Oid new_rel_oid);

extern void DeleteRelationTuple(Oid relid);
extern void DeleteAttributeTuples(Oid relid);
extern void DeleteSystemAttributeTuples(Oid relid);
extern void RemoveAttributeById(Oid relid, AttrNumber attnum);
extern void CopyStatistics(Oid fromrelid, Oid torelid);
extern void RemoveStatistics(Oid relid, AttrNumber attnum);

extern const FormData_pg_attribute *SystemAttributeDefinition(AttrNumber attno);

extern const FormData_pg_attribute *SystemAttributeByName(const char *attname);

extern void CheckAttributeNamesTypes(TupleDesc tupdesc, char relkind,
									 int flags);

extern void CheckAttributeType(const char *attname,
							   Oid atttypid, Oid attcollation,
							   List *containing_rowtypes,
							   int flags);

#endif							/* HEAP_H */
