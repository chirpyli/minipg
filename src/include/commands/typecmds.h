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
extern Oid	AssignTypeArrayOid(void);

extern Oid	AlterTypeNamespaceInternal(Oid typeOid, Oid nspOid,
									   bool isImplicitArray,
									   bool errorOnTableType,
									   ObjectAddresses *objsMoved);

#endif							/* TYPECMDS_H */
