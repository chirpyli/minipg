/*-------------------------------------------------------------------------
 *
 * alter.c
 *	  Drivers for generic alter commands
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/alter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "commands/alter.h"
#include "commands/collationcmds.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/proclang.h"
#include "commands/tablecmds.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * Executes an ALTER OBJECT / SET SCHEMA statement.  Based on the object
 * type, the function appropriate to that type is executed.
 *
 * Return value is that of the altered object.
 *
 * oldSchemaAddr is an output argument which, if not NULL, is set to the object
 * address of the original schema.
 */
ObjectAddress
ExecAlterObjectSchemaStmt(AlterObjectSchemaStmt *stmt,
						  ObjectAddress *oldSchemaAddr)
{
	ObjectAddress address;
	Oid			oldNspOid;

	switch (stmt->objectType)
	{
		case OBJECT_TABLE:
		case OBJECT_VIEW:
			address = AlterTableNamespace(stmt,
										  oldSchemaAddr ? &oldNspOid : NULL);
			break;

		default:
			elog(ERROR, "unrecognized AlterObjectSchemaStmt type: %d",
				 (int) stmt->objectType);
			return InvalidObjectAddress;	/* keep compiler happy */
	}

	if (oldSchemaAddr)
		ObjectAddressSet(*oldSchemaAddr, NamespaceRelationId, oldNspOid);

	return address;
}


