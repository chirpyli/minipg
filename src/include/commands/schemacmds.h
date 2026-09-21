/*-------------------------------------------------------------------------
 *
 * schemacmds.h
 *	  prototypes for schemacmds.c.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/schemacmds.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef SCHEMACMDS_H
#define SCHEMACMDS_H

#include "catalog/objectaddress.h"
#include "nodes/parsenodes.h"

extern Oid	CreateSchemaCommand(CreateSchemaStmt *parsetree,
								const char *queryString,
								int stmt_location, int stmt_len);


#endif							/* SCHEMACMDS_H */
