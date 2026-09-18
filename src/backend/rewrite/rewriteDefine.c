/*-------------------------------------------------------------------------
 *
 * rewriteDefine.c
 *	  routines for defining a rewrite rule
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/rewrite/rewriteDefine.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_rewrite.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteSupport.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


static void checkRuleResultList(List *targetList, TupleDesc resultDesc,
								bool requireColumnNameMatch);


/*
 * InsertRule -
 *	  takes the arguments and inserts them as a row into the system
 *	  relation "pg_rewrite"
 */
static Oid
InsertRule(Oid eventrel_oid, List *action, bool replace)
{
	char	   *evqual = nodeToString(NULL);
	char	   *actiontree = nodeToString((Node *) action);
	Datum		values[Natts_pg_rewrite];
	bool		nulls[Natts_pg_rewrite];
	bool		replaces[Natts_pg_rewrite];
	NameData	rname;
	Relation	pg_rewrite_desc;
	HeapTuple	tup,
				oldtup;
	Oid			rewriteObjectId;
	ObjectAddress myself,
				referenced;
	bool		is_update = false;

	/*
	 * Set up *nulls and *values arrays
	 */
	MemSet(nulls, false, sizeof(nulls));

	namestrcpy(&rname, ViewSelectRuleName);
	values[Anum_pg_rewrite_rulename - 1] = NameGetDatum(&rname);
	values[Anum_pg_rewrite_ev_class - 1] = ObjectIdGetDatum(eventrel_oid);
	values[Anum_pg_rewrite_ev_type - 1] = CharGetDatum(CMD_SELECT + '0');
	values[Anum_pg_rewrite_ev_enabled - 1] = CharGetDatum(RULE_FIRES_ON_ORIGIN);
	values[Anum_pg_rewrite_is_instead - 1] = BoolGetDatum(true);
	values[Anum_pg_rewrite_ev_qual - 1] = CStringGetTextDatum(evqual);
	values[Anum_pg_rewrite_ev_action - 1] = CStringGetTextDatum(actiontree);

	/*
	 * Ready to store new pg_rewrite tuple
	 */
	pg_rewrite_desc = table_open(RewriteRelationId, RowExclusiveLock);

	/*
	 * Check to see if we are replacing an existing tuple
	 */
	oldtup = SearchSysCache2(RULERELNAME,
							 ObjectIdGetDatum(eventrel_oid),
							 PointerGetDatum(ViewSelectRuleName));

	if (HeapTupleIsValid(oldtup))
	{
		if (!replace)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("rule \"%s\" for relation \"%s\" already exists",
							ViewSelectRuleName, get_rel_name(eventrel_oid))));

		/*
		 * When replacing, we don't need to replace every attribute
		 */
		MemSet(replaces, false, sizeof(replaces));
		replaces[Anum_pg_rewrite_ev_type - 1] = true;
		replaces[Anum_pg_rewrite_is_instead - 1] = true;
		replaces[Anum_pg_rewrite_ev_qual - 1] = true;
		replaces[Anum_pg_rewrite_ev_action - 1] = true;

		tup = heap_modify_tuple(oldtup, RelationGetDescr(pg_rewrite_desc),
								values, nulls, replaces);

		CatalogTupleUpdate(pg_rewrite_desc, &tup->t_self, tup);

		ReleaseSysCache(oldtup);

		rewriteObjectId = ((Form_pg_rewrite) GETSTRUCT(tup))->oid;
		is_update = true;
	}
	else
	{
		rewriteObjectId = GetNewOidWithIndex(pg_rewrite_desc,
											 RewriteOidIndexId,
											 Anum_pg_rewrite_oid);
		values[Anum_pg_rewrite_oid - 1] = ObjectIdGetDatum(rewriteObjectId);

		tup = heap_form_tuple(pg_rewrite_desc->rd_att, values, nulls);

		CatalogTupleInsert(pg_rewrite_desc, tup);
	}

	heap_freetuple(tup);

	/* If replacing, get rid of old dependencies and make new ones */
	if (is_update)
		deleteDependencyRecordsFor(RewriteRelationId, rewriteObjectId, false);

	/*
	 * Install a dependency on the rule's view to ensure it will go away on
	 * relation deletion.  The dependency is internal, which prevents deleting
	 * a view's _RETURN rule directly.
	 */
	myself.classId = RewriteRelationId;
	myself.objectId = rewriteObjectId;
	myself.objectSubId = 0;

	referenced.classId = RelationRelationId;
	referenced.objectId = eventrel_oid;
	referenced.objectSubId = 0;

	recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);

	/*
	 * Also install dependencies on objects referenced in the action.
	 */
	recordDependencyOnExpr(&myself, (Node *) action, NIL,
						   DEPENDENCY_NORMAL);

	/* Post creation hook for new rule */
	InvokeObjectPostCreateHook(RewriteRelationId, rewriteObjectId, 0);

	table_close(pg_rewrite_desc, RowExclusiveLock);

	return rewriteObjectId;
}

/*
 * DefineQueryRewrite
 *		Create the ON SELECT INSTEAD rule that stores a view's query.
 *
 * The action list has already been passed through parse analysis; it must
 * consist of exactly one SELECT query.
 */
ObjectAddress
DefineQueryRewrite(Oid event_relid, bool replace, List *action)
{
	Relation	event_relation;
	Query	   *query;
	Oid			ruleId;
	ObjectAddress address;

	/*
	 * We had better grab AccessExclusiveLock to ensure no SELECTs are
	 * currently running on the event relation.
	 */
	event_relation = table_open(event_relid, AccessExclusiveLock);

	/*
	 * Only a view can carry an ON SELECT rule; DefineView has already created
	 * (or replaced) the relation with that relkind.
	 */
	if (event_relation->rd_rel->relkind != RELKIND_VIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a view",
						RelationGetRelationName(event_relation))));

	if (!allowSystemTableMods && IsSystemRelation(event_relation))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(event_relation))));

	/*
	 * A view definition is a single SELECT whose target list must exactly
	 * match the view's columns.
	 */
	Assert(list_length(action) == 1);
	query = linitial_node(Query, action);
	if (query->commandType != CMD_SELECT)
		elog(ERROR, "unexpected parse analysis result");

	checkRuleResultList(query->targetList,
						RelationGetDescr(event_relation),
						true);

	/*
	 * This rule is allowed - prepare to install it.
	 */
	ruleId = InsertRule(event_relid, action, replace);

	/*
	 * Set pg_class 'relhasrules' field true for event relation.
	 *
	 * Important side effect: an SI notice is broadcast to force all backends
	 * (including me!) to update relcache entries with the new rule.
	 */
	SetRelationRuleStatus(event_relid, true);

	ObjectAddressSet(address, RewriteRelationId, ruleId);

	/* Close rel, but keep lock till commit... */
	table_close(event_relation, NoLock);

	return address;
}


/*
 * checkRuleResultList
 *		Verify that targetList produces output compatible with a tupledesc
 *
 * The targetList is a SELECT targetlist (of a view's _RETURN rule).
 *
 * A SELECT targetlist may optionally require that column names match.
 */
static void
checkRuleResultList(List *targetList, TupleDesc resultDesc,
					bool requireColumnNameMatch)
{
	ListCell   *tllist;
	int			i;

	i = 0;
	foreach(tllist, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tllist);
		Oid			tletypid;
		int32		tletypmod;
		Form_pg_attribute attr;
		char	   *attname;

		/* resjunk entries may be ignored */
		if (tle->resjunk)
			continue;
		i++;
		if (i > resultDesc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("SELECT rule's target list has too many entries")));

		attr = TupleDescAttr(resultDesc, i - 1);
		attname = NameStr(attr->attname);

		/*
		 * Disallow dropped columns in the relation.  This is not really
		 * expected to happen when creating an ON SELECT rule.  It'd be
		 * possible if someone tried to convert a relation with dropped
		 * columns to a view, but the only case we care about supporting
		 * table-to-view conversion for is pg_dump, and pg_dump won't do that.
		 */
		if (attr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot convert relation containing dropped columns to view")));

		/* Check name match if required; no need for two error texts here */
		if (requireColumnNameMatch && strcmp(tle->resname, attname) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("SELECT rule's target entry %d has different column name from column \"%s\"",
							i, attname),
					 errdetail("SELECT target entry is named \"%s\".",
							   tle->resname)));

		/* Check type match. */
		tletypid = exprType((Node *) tle->expr);
		if (attr->atttypid != tletypid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("SELECT rule's target entry %d has different type from column \"%s\"",
							i, attname),
					 errdetail("SELECT target entry has type %s, but column has type %s.",
							   format_type_be(tletypid),
							   format_type_be(attr->atttypid))));

		/*
		 * Allow typmods to be different only if one of them is -1, ie,
		 * "unspecified".  This is necessary for cases like "numeric", where
		 * the table will have a filled-in default length but the select
		 * rule's expression will probably have typmod = -1.
		 */
		tletypmod = exprTypmod((Node *) tle->expr);
		if (attr->atttypmod != tletypmod &&
			attr->atttypmod != -1 && tletypmod != -1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("SELECT rule's target entry %d has different size from column \"%s\"",
							i, attname),
					 errdetail("SELECT target entry has type %s, but column has type %s.",
							   format_type_with_typemod(tletypid, tletypmod),
							   format_type_with_typemod(attr->atttypid,
														attr->atttypmod))));
	}

	if (i != resultDesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("SELECT rule's target list has too few entries")));
}


