/*-------------------------------------------------------------------------
 *
 * schemacmds.c
 *	  schema creation/manipulation commands
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/schemacmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "commands/dbcommands.h"
#include "commands/schemacmds.h"
#include "miscadmin.h"
#include "parser/parse_utilcmd.h"
#include "parser/scansup.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * CREATE SCHEMA
 *
 * Note: caller should pass in location information for the whole
 * CREATE SCHEMA statement, which in turn we pass down as the location
 * of the component commands.  This comports with our general plan of
 * reporting location/len for the whole command even when executing
 * a subquery.
 */
Oid
CreateSchemaCommand(CreateSchemaStmt *stmt, const char *queryString,
					int stmt_location, int stmt_len)
{
	const char *schemaName = stmt->schemaname;
	Oid			namespaceId;
	List	   *parsetree_list;
	ListCell   *parsetree_item;
	Oid			owner_uid;
	Oid			saved_uid;
	int			save_sec_context;
	int			save_nestlevel;
	char	   *nsp = namespace_search_path;
	ObjectAddress address;
	StringInfoData pathbuf;

	GetUserIdAndSecContext(&saved_uid, &save_sec_context);

	/*
	 * Who is supposed to own the new schema?
	 */
	if (stmt->authrole)
		owner_uid = get_rolespec_oid(stmt->authrole, false);
	else
		owner_uid = saved_uid;

	/* fill schema name with the user name if not specified */
	if (!schemaName)
	{
		/*
		 * minipg 没有用户/角色概念，schema 名退化为唯一的角色名 "postgres"。
		 */
		schemaName = GetUserNameFromId(owner_uid, false);
	}


	/* Additional check to protect reserved schema names */
	if (!allowSystemTableMods && IsReservedName(schemaName))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable schema name \"%s\"", schemaName),
				 errdetail("The prefix \"pg_\" is reserved for system schemas.")));

	/*
	 * If if_not_exists was given and the schema already exists, bail out.
	 * (Note: we needn't check this when not if_not_exists, because
	 * NamespaceCreate will complain anyway.)  We could do this before making
	 * the permissions checks, but since CREATE TABLE IF NOT EXISTS makes its
	 * creation-permission check first, we do likewise.
	 */
	if (stmt->if_not_exists)
	{
		namespaceId = get_namespace_oid(schemaName, true);
		if (OidIsValid(namespaceId))
		{
			/*
			 * If we are in an extension script, insist that the pre-existing
			 * object be a member of the extension, to avoid security risks.
			 */
			ObjectAddressSet(address, NamespaceRelationId, namespaceId);
			checkMembershipInCurrentExtension(&address);

			/* OK to skip */
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_SCHEMA),
					 errmsg("schema \"%s\" already exists, skipping",
							schemaName)));
			return InvalidOid;
		}
	}

	/*
	 * If the requested authorization is different from the current user,
	 * temporarily set the current user so that the object(s) will be created
	 * with the correct ownership.
	 *
	 * (The setting will be restored at the end of this routine, or in case of
	 * error, transaction abort will clean things up.)
	 */
	if (saved_uid != owner_uid)
		SetUserIdAndSecContext(owner_uid,
							   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);

	/* Create the schema's namespace */
	namespaceId = NamespaceCreate(schemaName, owner_uid, false);

	/* Advance cmd counter to make the namespace visible */
	CommandCounterIncrement();

	/*
	 * Prepend the new schema to the current search path.
	 *
	 * We use the equivalent of a function SET option to allow the setting to
	 * persist for exactly the duration of the schema creation.  guc.c also
	 * takes care of undoing the setting on error.
	 */
	save_nestlevel = NewGUCNestLevel();

	initStringInfo(&pathbuf);
	appendStringInfoString(&pathbuf, quote_identifier(schemaName));

	while (scanner_isspace(*nsp))
		nsp++;

	if (*nsp != '\0')
		appendStringInfo(&pathbuf, ", %s", nsp);

	(void) set_config_option("search_path", pathbuf.data,
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * Report the new schema to possibly interested event triggers.  Note we
	 * must do this here and not in ProcessUtilitySlow because otherwise the
	 * objects created below are reported before the schema, which would be
	 * wrong.
	 */
	ObjectAddressSet(address, NamespaceRelationId, namespaceId);

	/*
	 * Examine the list of commands embedded in the CREATE SCHEMA command, and
	 * reorganize them into a sequentially executable order with no forward
	 * references.  Note that the result is still a list of raw parsetrees ---
	 * we cannot, in general, run parse analysis on one statement until we
	 * have actually executed the prior ones.
	 */
	parsetree_list = transformCreateSchemaStmtElements(stmt->schemaElts,
													   schemaName);

	/*
	 * Execute each command contained in the CREATE SCHEMA.  Since the grammar
	 * allows only utility commands in CREATE SCHEMA, there is no need to pass
	 * them through parse_analyze() or the rewriter; we can just hand them
	 * straight to ProcessUtility.
	 */
	foreach(parsetree_item, parsetree_list)
	{
		Node	   *stmt = (Node *) lfirst(parsetree_item);
		PlannedStmt *wrapper;

		/* need to make a wrapper PlannedStmt */
		wrapper = makeNode(PlannedStmt);
		wrapper->commandType = CMD_UTILITY;
		wrapper->canSetTag = false;
		wrapper->utilityStmt = stmt;
		wrapper->stmt_location = stmt_location;
		wrapper->stmt_len = stmt_len;

		/* do this step */
		ProcessUtility(wrapper,
					   queryString,
					   false,
					   PROCESS_UTILITY_SUBCOMMAND,
					   NULL,
					   NULL,
					   None_Receiver,
					   NULL);

		/* make sure later steps can see the object created here */
		CommandCounterIncrement();
	}

	/*
	 * Restore the GUC variable search_path we set above.
	 */
	AtEOXact_GUC(true, save_nestlevel);

	/* Reset current user and security context */
	SetUserIdAndSecContext(saved_uid, save_sec_context);

	return namespaceId;
}



