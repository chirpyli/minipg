/*-------------------------------------------------------------------------
 *
 * dropcmds.c
 *	  handle various "DROP" operations
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/dropcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static void does_not_exist_skipping(ObjectType objtype,
									Node *object);


/*
 * Drop one or more objects.
 *
 * We don't currently handle all object types here.  Relations, for example,
 * require special handling, because (for example) indexes have additional
 * locking requirements.
 *
 * We look up all the objects first, and then delete them in a single
 * performMultipleDeletions() call.  This avoids unnecessary DROP RESTRICT
 * errors if there are dependencies between them.
 */
void
RemoveObjects(DropStmt *stmt)
{
	ObjectAddresses *objects;
	ListCell   *cell1;

	objects = new_object_addresses();

	foreach(cell1, stmt->objects)
	{
		ObjectAddress address;
		Node	   *object = lfirst(cell1);
		Relation	relation = NULL;

		/* Get an ObjectAddress for the object. */
		address = get_object_address(stmt->removeType,
									 object,
									 &relation,
									 AccessExclusiveLock,
									 stmt->missing_ok);

		/*
		 * Issue NOTICE if supplied object was not found.  Note this is only
		 * relevant in the missing_ok case, because otherwise
		 * get_object_address would have thrown an error.
		 */
		if (!OidIsValid(address.objectId))
		{
			Assert(stmt->missing_ok);
			does_not_exist_skipping(stmt->removeType, object);
			continue;
		}

		/*
		 * Although COMMENT ON FUNCTION, etc. are happy to operate on an
		 * aggregate as on any other function, we have historically not allowed
		 * this for DROP FUNCTION.
		 */
		if (stmt->removeType == OBJECT_FUNCTION)
		{
			if (get_func_prokind(address.objectId) == PROKIND_AGGREGATE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is an aggregate function",
								NameListToString(castNode(ObjectWithArgs, object)->objname)),
						 errhint("Use DROP AGGREGATE to drop aggregate functions.")));
		}

		/* Release any relcache reference count, but keep lock until commit. */
		if (relation)
			table_close(relation, NoLock);

		add_exact_object_address(&address, objects);
	}

	/* Here we really delete them. */
	performMultipleDeletions(objects, stmt->behavior, 0);

	free_object_addresses(objects);
}

/*
 * does_not_exist_skipping
 *		Subroutine for RemoveObjects
 *
 * Generate a NOTICE stating that the named object was not found, and is
 * being skipped.  This is only relevant when "IF EXISTS" is used; otherwise,
 * get_object_address() in RemoveObjects would have thrown an ERROR.
 */
static void
does_not_exist_skipping(ObjectType objtype, Node *object)
{
	const char *msg = NULL;
	char	   *name = NULL;

	switch (objtype)
	{
		case OBJECT_SCHEMA:
			msg = gettext_noop("schema \"%s\" does not exist, skipping");
			name = strVal((Value *) object);
			break;
		default:
			elog(ERROR, "unrecognized object type: %d", (int) objtype);
			break;
	}

	ereport(NOTICE, (errmsg(msg, name)));
}
