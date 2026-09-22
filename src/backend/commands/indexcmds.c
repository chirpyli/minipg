/*-------------------------------------------------------------------------
 *
 * indexcmds.c
 *	  POSTGRES define and remove index code.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/indexcmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_am.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "commands/tablecmds.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "postgres_ext.h"
#include "rewrite/rewriteManip.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinvaladt.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/regproc.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/* non-export function prototypes */
static bool CompareOpclassOptions(Datum *opts1, Datum *opts2, int natts);
static void CheckPredicate(Expr *predicate);
static void ComputeIndexAttrs(IndexInfo *indexInfo,
						  Oid *typeOidP,
						  Oid *classOidP,
						  int16 *colOptionP,
							  List *attList,
							  List *exclusionOpNames,
							  Oid relId,
							  const char *accessMethodName, Oid accessMethodId,
							  bool amcanorder,
							  bool isconstraint,
							  Oid ddl_userid,
							  int ddl_sec_context,
							  int *ddl_save_nestlevel);
static char *ChooseIndexName(const char *tabname, Oid namespaceId,
							 List *colnames, List *exclusionOpNames,
							 bool primary, bool isconstraint);
static char *ChooseIndexNameAddition(List *colnames);
static List *ChooseIndexColumnNames(List *indexElems);
static inline void set_indexsafe_procflags(void);


/*
 * CheckIndexCompatible
 *		Determine whether an existing index definition is compatible with a
 *		prospective index definition, such that the existing index storage
 *		could become the storage of the new index, avoiding a rebuild.
 *
 * 'heapRelation': the relation the index would apply to.
 * 'accessMethodName': name of the AM to use.
 * 'attributeList': a list of IndexElem specifying columns and expressions
 *		to index on.
 * 'exclusionOpNames': list of names of exclusion-constraint operators,
 *		or NIL (always NIL; EXCLUDE constraints are not supported).
 *
 * This is tailored to the needs of ALTER TABLE ALTER TYPE, which recreates
 * any indexes that depended on a changing column from their pg_get_indexdef
 * or pg_get_constraintdef definitions.  We omit some of the sanity checks of
 * DefineIndex.  We assume that the old and new indexes have the same number
 * of columns and that if one has an expression column or predicate, both do.
 * Errors arising from the attribute list still apply.
 *
 * Most column type changes that can skip a table rewrite do not invalidate
 * indexes.  We acknowledge this when all operator classes, collations and
 * exclusion operators match.  Though we could further permit intra-opfamily
 * changes for btree and hash indexes, that adds subtle complexity with no
 * concrete benefit for core types. Note, that INCLUDE columns aren't
 * checked by this function, for them it's enough that table rewrite is
 * skipped.
 *
 * When a comparison operator has a polymorphic input type, the actual input
 * types must also match.  This defends against the possibility that
 * operators could vary behavior in response to get_fn_expr_argtype().
 * At present, this hazard is theoretical: all core index access methods
 * decline to set fn_expr for such calls.
 *
 * We do not yet implement a test to verify compatibility of expression
 * columns or predicates, so assume any such index is incompatible.
 */
bool
CheckIndexCompatible(Oid oldId,
					 const char *accessMethodName,
					 List *attributeList,
					 List *exclusionOpNames)
{
	bool		isconstraint;
	Oid		   *typeObjectId;
	Oid		   *classObjectId;
	Oid			accessMethodId;
	Oid			relationId;
	HeapTuple	tuple;
	Form_pg_index indexForm;
	Form_pg_am	accessMethodForm;
	IndexAmRoutine *amRoutine;
	bool		amcanorder;
	int16	   *coloptions;
	IndexInfo  *indexInfo;
	int			numberOfAttributes;
	int			old_natts;
	bool		isnull;
	bool		ret = true;
	oidvector  *old_indclass;
	Relation	irel;
	int			i;
	Datum		d;

	/* Caller should already have the relation locked in some way. */
	relationId = IndexGetRelation(oldId, false);

	/*
	 * We can pretend isconstraint = false unconditionally.  It only serves to
	 * decide the text of an error message that should never happen for us.
	 */
	isconstraint = false;

	numberOfAttributes = list_length(attributeList);
	Assert(numberOfAttributes > 0);
	Assert(numberOfAttributes <= INDEX_MAX_KEYS);

	/* look up the access method */
	tuple = SearchSysCache1(AMNAME, PointerGetDatum(accessMethodName));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method \"%s\" does not exist",
						accessMethodName)));
	accessMethodForm = (Form_pg_am) GETSTRUCT(tuple);
	accessMethodId = accessMethodForm->oid;
	amRoutine = GetIndexAmRoutine(accessMethodForm->amhandler);
	ReleaseSysCache(tuple);

	amcanorder = amRoutine->amcanorder;

	/*
	 * Compute the operator classes, collations, and exclusion operators for
	 * the new index, so we can test whether it's compatible with the existing
	 * one.  Note that ComputeIndexAttrs might fail here, but that's OK:
	 * DefineIndex would have failed later.  Our attributeList contains only
	 * key attributes, thus we're filling ii_NumIndexAttrs and
	 * ii_NumIndexKeyAttrs with same value.
	 */
	indexInfo = makeIndexInfo(numberOfAttributes, numberOfAttributes,
							  accessMethodId, NIL, NIL, false, false, false);
	typeObjectId = (Oid *) palloc(numberOfAttributes * sizeof(Oid));
	classObjectId = (Oid *) palloc(numberOfAttributes * sizeof(Oid));
	coloptions = (int16 *) palloc(numberOfAttributes * sizeof(int16));
	ComputeIndexAttrs(indexInfo,
					  typeObjectId, classObjectId,
					  coloptions, attributeList,
					  exclusionOpNames, relationId,
					  accessMethodName, accessMethodId,
					  amcanorder, isconstraint, InvalidOid, 0, NULL);


	/* Get the soon-obsolete pg_index tuple. */
	tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(oldId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for index %u", oldId);
	indexForm = (Form_pg_index) GETSTRUCT(tuple);

	/*
	 * We don't assess expressions or predicates; assume incompatibility.
	 * Also, if the index is invalid for any reason, treat it as incompatible.
	 */
	if (!(heap_attisnull(tuple, Anum_pg_index_indpred, NULL) &&
		  heap_attisnull(tuple, Anum_pg_index_indexprs, NULL) &&
		  indexForm->indisvalid))
	{
		ReleaseSysCache(tuple);
		return false;
	}

	/* Any change in operator class breaks compatibility. */
	old_natts = indexForm->indnkeyatts;
	Assert(old_natts == numberOfAttributes);

	d = SysCacheGetAttr(INDEXRELID, tuple, Anum_pg_index_indclass, &isnull);
	Assert(!isnull);
	old_indclass = (oidvector *) DatumGetPointer(d);

	ret = (memcmp(old_indclass->values, classObjectId,
				  old_natts * sizeof(Oid)) == 0);

	ReleaseSysCache(tuple);

	if (!ret)
		return false;

	/* For polymorphic opcintype, column type changes break compatibility. */
	irel = index_open(oldId, AccessShareLock);	/* caller probably has a lock */
	for (i = 0; i < old_natts; i++)
	{
		if (IsPolymorphicType(get_opclass_input_type(classObjectId[i])) &&
			TupleDescAttr(irel->rd_att, i)->atttypid != typeObjectId[i])
		{
			ret = false;
			break;
		}
	}

	/* Any change in opclass options break compatibility. */
	if (ret)
	{
		Datum	   *opclassOptions = RelationGetIndexRawAttOptions(irel);

		ret = CompareOpclassOptions(opclassOptions,
									indexInfo->ii_OpclassOptions, old_natts);

		if (opclassOptions)
			pfree(opclassOptions);
	}

	index_close(irel, NoLock);
	return ret;
}

/*
 * CompareOpclassOptions
 *
 * Compare per-column opclass options which are represented by arrays of text[]
 * datums.  Both elements of arrays and array themselves can be NULL.
 */
static bool
CompareOpclassOptions(Datum *opts1, Datum *opts2, int natts)
{
	int			i;
	FmgrInfo	fm;

	if (!opts1 && !opts2)
		return true;

	fmgr_info(F_ARRAY_EQ, &fm);
	for (i = 0; i < natts; i++)
	{
		Datum		opt1 = opts1 ? opts1[i] : (Datum) 0;
		Datum		opt2 = opts2 ? opts2[i] : (Datum) 0;

		if (opt1 == (Datum) 0)
		{
			if (opt2 == (Datum) 0)
				continue;
			else
				return false;
		}
		else if (opt2 == (Datum) 0)
			return false;

		/*
		 * Compare non-NULL text[] datums.  Use C collation to enforce binary
		 * equivalence of texts, because we don't know anything about the
		 * semantics of opclass options.
		 */
		if (!DatumGetBool(FunctionCall2Coll(&fm, C_COLLATION_OID, opt1, opt2)))
			return false;
	}

	return true;
}

/*
 * WaitForOlderSnapshots
 *
 * Wait for transactions that might have an older snapshot than the given xmin
 * limit, because it might not contain tuples deleted just before it has
 * been taken. Obtain a list of VXIDs of such transactions, and wait for them
 * individually. This is used when building an index concurrently.
 *
 * We can exclude any running transactions that have xmin > the xmin given;
 * their oldest snapshot must be newer than our xmin limit.
 * We can also exclude any transactions that have xmin = zero, since they
 * evidently have no live snapshot at all (and any one they might be in
 * process of taking is certainly newer than ours).  Transactions in other
 * DBs can be ignored too, since they'll never even be able to see the
 * index being worked on.
 *
 * We can also exclude autovacuum processes and processes running manual
 * lazy VACUUMs, because they won't be fazed by missing index entries
 * either.  (Manual ANALYZEs, however, can't be excluded because they
 * might be within transactions that are going to do arbitrary operations
 * later.)  Processes running CREATE INDEX CONCURRENTLY or REINDEX CONCURRENTLY
 * on indexes that are neither expressional nor partial are also safe to
 * ignore, since we know that those processes won't examine any data
 * outside the table they're indexing.
 *
 * Also, GetCurrentVirtualXIDs never reports our own vxid, so we need not
 * check for that.
 *
 * If a process goes idle-in-transaction with xmin zero, we do not need to
 * wait for it anymore, per the above argument.  We do not have the
 * infrastructure right now to stop waiting if that happens, but we can at
 * least avoid the folly of waiting when it is idle at the time we would
 * begin to wait.  We do this by repeatedly rechecking the output of
 * GetCurrentVirtualXIDs.  If, during any iteration, a particular vxid
 * doesn't show up in the output, we know we can forget about it.
 */
void
WaitForOlderSnapshots(TransactionId limitXmin)
{
	int			n_old_snapshots;
	int			i;
	VirtualTransactionId *old_snapshots;

	old_snapshots = GetCurrentVirtualXIDs(limitXmin, true, false,
										  PROC_IN_VACUUM
										  | PROC_IN_SAFE_IC,
										  &n_old_snapshots);
	for (i = 0; i < n_old_snapshots; i++)
	{
		if (!VirtualTransactionIdIsValid(old_snapshots[i]))
			continue;			/* found uninteresting in previous cycle */

		if (i > 0)
		{
			/* see if anything's changed ... */
			VirtualTransactionId *newer_snapshots;
			int			n_newer_snapshots;
			int			j;
			int			k;

			newer_snapshots = GetCurrentVirtualXIDs(limitXmin,
													true, false,
													PROC_IN_VACUUM
													| PROC_IN_SAFE_IC,
													&n_newer_snapshots);
			for (j = i; j < n_old_snapshots; j++)
			{
				if (!VirtualTransactionIdIsValid(old_snapshots[j]))
					continue;	/* found uninteresting in previous cycle */
				for (k = 0; k < n_newer_snapshots; k++)
				{
					if (VirtualTransactionIdEquals(old_snapshots[j],
												   newer_snapshots[k]))
						break;
				}
				if (k >= n_newer_snapshots) /* not there anymore */
					SetInvalidVirtualTransactionId(old_snapshots[j]);
			}
			pfree(newer_snapshots);
		}

		if (VirtualTransactionIdIsValid(old_snapshots[i]))
		{
			VirtualXactLock(old_snapshots[i], true);
		}

	}
}


/*
 * DefineIndex
 *		Creates a new index.
 *
 * This function manages the current userid according to the needs of pg_dump.
 * Recreating old-database catalog entries in new-database is fine, regardless
 * of which users would have permission to recreate those entries now.  That's
 * just preservation of state.  Running opaque expressions, like calling a
 * function named in a catalog entry or evaluating a pg_node_tree in a catalog
 * entry, as anyone other than the object owner, is not fine.  To adhere to
 * those principles and to remain fail-safe, use the table owner userid for
 * most ACL checks.  Use the original userid for ACL checks reached without
 * traversing opaque expressions.  (pg_dump can predict such ACL checks from
 * catalogs.)  Overall, this is a mess.  Future DDL development should
 * consider offering one DDL command for catalog setup and a separate DDL
 * command for steps that run opaque expressions.
 *
 * 'relationId': the OID of the heap relation on which the index is to be
 *		created
 * 'stmt': IndexStmt describing the properties of the new index.
 * 'indexRelationId': normally InvalidOid, but during bootstrap can be
 *		nonzero to specify a preselected OID for the index.
 * 'is_alter_table': this is due to an ALTER rather than a CREATE operation.
 * 'check_rights': check for CREATE rights in namespace and tablespace.  (This
 *		should be true except when ALTER is deleting/recreating an index.)
 * 'check_not_in_use': check for table not already in use in current session.
 *		This should be true unless caller is holding the table open, in which
 *		case the caller had better have checked it earlier.
 * 'skip_build': make the catalog entries but don't create the index files
 * 'quiet': suppress the NOTICE chatter ordinarily provided for constraints.
 *
 * Returns the object address of the created index.
 */
ObjectAddress
DefineIndex(Oid relationId,
			IndexStmt *stmt,
			Oid indexRelationId,
			bool is_alter_table,
			bool check_rights,
			bool check_not_in_use,
			bool skip_build,
			bool quiet)
{
	bool		concurrent;
	char	   *indexRelationName;
	char	   *accessMethodName;
	Oid		   *typeObjectId;
	Oid		   *classObjectId;
	Oid			accessMethodId;
	Oid			namespaceId;
	Oid			tablespaceId;
	Oid			createdConstraintId = InvalidOid;
	List	   *indexColNames;
	List	   *allIndexParams;
	Relation	rel;
	HeapTuple	tuple;
	Form_pg_am	accessMethodForm;
	IndexAmRoutine *amRoutine;
	bool		amcanorder;
	bool		safe_index;
	Datum		reloptions;
	int16	   *coloptions;
	IndexInfo  *indexInfo;
	bits16		flags;
	bits16		constr_flags;
	int			numberOfAttributes;
	int			numberOfKeyAttributes;
	TransactionId limitXmin;
	ObjectAddress address;
	LockRelId	heaprelid;
	LOCKTAG		heaplocktag;
	LOCKMODE	lockmode;
	Snapshot	snapshot;
	Oid			root_save_userid;
	int			root_save_sec_context;
	int			root_save_nestlevel;
	int			i;

	root_save_nestlevel = NewGUCNestLevel();

	/*
	 * Decide whether to build the index concurrently.  This build does not
	 * support temporary relations, so CONCURRENTLY is honored whenever
	 * requested.
	 */
	concurrent = stmt->concurrent;

	/*
	 * Start progress report.
	 */

	/*
	 * No index OID to report yet
	 */

	/*
	 * count key attributes in index
	 */
	numberOfKeyAttributes = list_length(stmt->indexParams);

	/*
	 * Calculate the new list of index columns including both key columns and
	 * INCLUDE columns.  Later we can determine which of these are key
	 * columns, and which are just part of the INCLUDE list by checking the
	 * list position.  A list item in a position less than ii_NumIndexKeyAttrs
	 * is part of the key columns, and anything equal to and over is part of
	 * the INCLUDE columns.
	 */
	allIndexParams = list_concat_copy(stmt->indexParams,
									  stmt->indexIncludingParams);
	numberOfAttributes = list_length(allIndexParams);

	if (numberOfKeyAttributes <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("must specify at least one column")));
	if (numberOfAttributes > INDEX_MAX_KEYS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("cannot use more than %d columns in an index",
						INDEX_MAX_KEYS)));

	/*
	 * Only SELECT ... FOR UPDATE/SHARE are allowed while doing a standard
	 * index build; but for concurrent builds we allow INSERT/UPDATE/DELETE
	 * (but not VACUUM).
	 *
	 * NB: Caller is responsible for making sure that relationId refers to the
	 * relation on which the index should be built; except in bootstrap mode,
	 * this will typically require the caller to have already locked the
	 * relation.  To avoid lock upgrade hazards, that lock should be at least
	 * as strong as the one we take here.
	 *
	 * NB: If the lock strength here ever changes, code that is run by
	 * parallel workers under the control of certain particular ambuild
	 * functions will need to be updated, too.
	 */
	lockmode = concurrent ? ShareUpdateExclusiveLock : ShareLock;
	rel = table_open(relationId, lockmode);

	/*
	 * Switch to the table owner's userid, so that any index functions are run
	 * as that user.  Also lock down security-restricted operations.  We
	 * already arranged to make GUC variable changes local to this command.
	 */
	GetUserIdAndSecContext(&root_save_userid, &root_save_sec_context);
	SetUserIdAndSecContext(GetUserId(),
						   root_save_sec_context | SECURITY_RESTRICTED_OPERATION);

	namespaceId = RelationGetNamespace(rel);

	/* Ensure that it makes sense to index this kind of relation */
	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
			/* OK */
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					errmsg("\"%s\" is not a table",
							RelationGetRelationName(rel))));
			break;
	}

	/*
	 * Unless our caller vouches for having checked this already, insist that
	 * the table not be in use by our own session, either.  Otherwise we might
	 * fail to make entries in the new index (for instance, if an INSERT or
	 * UPDATE is in progress and has already made its list of target indexes).
	 */
	if (check_not_in_use)
		CheckTableNotInUse(rel, "CREATE INDEX");

	/*
	 * Verify we (still) have CREATE rights in the rel's namespace.
	 * (Presumably we did when the rel was created, but maybe not anymore.)
	 * Skip check if caller doesn't want it.  Also skip check if
	 * bootstrapping, since permissions machinery may not be working yet.
	 */
	/*
	 * Select tablespace to use.  Always use default tablespace.
	 */
	tablespaceId = InvalidOid;
	/* note InvalidOid is OK in this case */

	/*
	 * Force shared indexes into the pg_global tablespace.  This is a bit of a
	 * hack but seems simpler than marking them in the BKI commands.  On the
	 * other hand, if it's not shared, don't allow it to be placed there.
	 */
	if (rel->rd_rel->relisshared)
		tablespaceId = GLOBALTABLESPACE_OID;
	else if (tablespaceId == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only shared relations can be placed in pg_global tablespace")));

	/*
	 * Choose the index column names.
	 */
	indexColNames = ChooseIndexColumnNames(allIndexParams);

	/*
	 * Select name for index if caller didn't specify
	 */
	indexRelationName = stmt->idxname;
	if (indexRelationName == NULL)
		indexRelationName = ChooseIndexName(RelationGetRelationName(rel),
											namespaceId,
											indexColNames,
											NIL,
											stmt->primary,
											stmt->isconstraint);

	/*
	 * look up the access method, verify it can handle the requested features
	 */
	accessMethodName = stmt->accessMethod;
	tuple = SearchSysCache1(AMNAME, PointerGetDatum(accessMethodName));
	if (!HeapTupleIsValid(tuple))
	{
		/*
		 * Hack to provide more-or-less-transparent updating of old RTREE
		 * indexes to GiST: if RTREE is requested and not found, use GIST.
		 */
		if (strcmp(accessMethodName, "rtree") == 0)
		{
			ereport(NOTICE,
					(errmsg("substituting access method \"gist\" for obsolete method \"rtree\"")));
			accessMethodName = "gist";
			tuple = SearchSysCache1(AMNAME, PointerGetDatum(accessMethodName));
		}

		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("access method \"%s\" does not exist",
							accessMethodName)));
	}
	accessMethodForm = (Form_pg_am) GETSTRUCT(tuple);
	accessMethodId = accessMethodForm->oid;
	amRoutine = GetIndexAmRoutine(accessMethodForm->amhandler);


	if (stmt->unique && !amRoutine->amcanunique)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support unique indexes",
						accessMethodName)));
	if (stmt->indexIncludingParams != NIL && !amRoutine->amcaninclude)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support included columns",
						accessMethodName)));
	if (numberOfKeyAttributes > 1 && !amRoutine->amcanmulticol)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support multicolumn indexes",
						accessMethodName)));
	amcanorder = amRoutine->amcanorder;

	pfree(amRoutine);
	ReleaseSysCache(tuple);

	/*
	 * Validate predicate, if given
	 */
	if (stmt->whereClause)
		CheckPredicate((Expr *) stmt->whereClause);

	reloptions = (Datum) 0;

	/*
	 * Prepare arguments for index_create, primarily an IndexInfo structure.
	 * Note that predicates must be in implicit-AND format.  In a concurrent
	 * build, mark it not-ready-for-inserts.
	 */
	indexInfo = makeIndexInfo(numberOfAttributes,
							  numberOfKeyAttributes,
							  accessMethodId,
							  NIL,	/* expressions, NIL for now */
							  make_ands_implicit((Expr *) stmt->whereClause),
							  stmt->unique,
							  !concurrent,
							  concurrent);

	typeObjectId = (Oid *) palloc(numberOfAttributes * sizeof(Oid));
	classObjectId = (Oid *) palloc(numberOfAttributes * sizeof(Oid));
	coloptions = (int16 *) palloc(numberOfAttributes * sizeof(int16));
	ComputeIndexAttrs(indexInfo,
					  typeObjectId, classObjectId,
					  coloptions, allIndexParams,
					  NIL, relationId,
					  accessMethodName, accessMethodId,
					  amcanorder, stmt->isconstraint, root_save_userid,
					  root_save_sec_context, &root_save_nestlevel);

	/*
	 * Extra checks when creating a PRIMARY KEY index.
	 */
	if (stmt->primary)
		index_check_primary_key(rel, indexInfo, is_alter_table, stmt);

	/*
	 * We disallow indexes on system columns.  They would not necessarily get
	 * updated correctly, and they don't seem useful anyway.
	 */
	for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
	{
		AttrNumber	attno = indexInfo->ii_IndexAttrNumbers[i];

		if (attno < 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("index creation on system columns is not supported")));
	}

	/*
	 * Also check for system columns used in expressions or predicates.
	 */
	if (indexInfo->ii_Expressions || indexInfo->ii_Predicate)
	{
		Bitmapset  *indexattrs = NULL;

		pull_varattnos((Node *) indexInfo->ii_Expressions, 1, &indexattrs);
		pull_varattnos((Node *) indexInfo->ii_Predicate, 1, &indexattrs);

		for (i = FirstLowInvalidHeapAttributeNumber + 1; i < 0; i++)
		{
			if (bms_is_member(i - FirstLowInvalidHeapAttributeNumber,
							  indexattrs))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("index creation on system columns is not supported")));
		}
	}

	/* Is index safe for others to ignore?  See set_indexsafe_procflags() */
	safe_index = indexInfo->ii_Expressions == NIL &&
		indexInfo->ii_Predicate == NIL;

	/*
	 * Report index creation if appropriate (delay this till after most of the
	 * error checks)
	 */
	if (stmt->isconstraint && !quiet)
	{
		const char *constraint_type;

		if (stmt->primary)
			constraint_type = "PRIMARY KEY";
		else if (stmt->unique)
			constraint_type = "UNIQUE";
		else
		{
			elog(ERROR, "unknown constraint type");
			constraint_type = NULL; /* keep compiler quiet */
		}

		ereport(DEBUG1,
				(errmsg_internal("%s %s will create implicit index \"%s\" for table \"%s\"",
								 is_alter_table ? "ALTER TABLE / ADD" : "CREATE TABLE /",
								 constraint_type,
								 indexRelationName, RelationGetRelationName(rel))));
	}

	/*
	 * A valid stmt->oldNode implies that we already have a built form of the
	 * index.  The caller should also decline any index build.
	 */
	Assert(!OidIsValid(stmt->oldNode) || (skip_build && !concurrent));

	/*
	 * Make the catalog entries for the index, including constraints. This
	 * step also actually builds the index, except if caller requested not to
	 * or in concurrent mode, in which case it'll be done later.
	 */
	flags = constr_flags = 0;
	if (stmt->isconstraint)
		flags |= INDEX_CREATE_ADD_CONSTRAINT;
	if (skip_build || concurrent)
		flags |= INDEX_CREATE_SKIP_BUILD;
	if (stmt->if_not_exists)
		flags |= INDEX_CREATE_IF_NOT_EXISTS;
	if (concurrent)
		flags |= INDEX_CREATE_CONCURRENT;
	if (stmt->primary)
		flags |= INDEX_CREATE_IS_PRIMARY;

	indexRelationId =
		index_create(rel, indexRelationName, indexRelationId,
					 stmt->oldNode, indexInfo, indexColNames,
					 accessMethodId, tablespaceId,
					 classObjectId,
					 coloptions, reloptions,
					 flags, constr_flags,
					 allowSystemTableMods, !check_rights,
					 &createdConstraintId);

	ObjectAddressSet(address, RelationRelationId, indexRelationId);

	if (!OidIsValid(indexRelationId))
	{
		/*
		 * Roll back any GUC changes executed by index functions.
		 */
		AtEOXact_GUC(false, root_save_nestlevel);

		/* Restore userid and security context */
		SetUserIdAndSecContext(root_save_userid, root_save_sec_context);

		table_close(rel, NoLock);


		return address;
	}

	/*
	 * Roll back any GUC changes executed by index functions, and keep
	 * subsequent changes local to this command.  This is essential if some
	 * index function changed a behavior-affecting GUC, e.g. search_path.
	 */
	AtEOXact_GUC(false, root_save_nestlevel);
	root_save_nestlevel = NewGUCNestLevel();

	AtEOXact_GUC(false, root_save_nestlevel);
	SetUserIdAndSecContext(root_save_userid, root_save_sec_context);

	if (!concurrent)
	{
		/* Close the heap and we're done, in the non-concurrent case */
		table_close(rel, NoLock);


		return address;
	}

	/* save lockrelid and locktag for below, then close rel */
	heaprelid = rel->rd_lockInfo.lockRelId;
	SET_LOCKTAG_RELATION(heaplocktag, heaprelid.dbId, heaprelid.relId);
	table_close(rel, NoLock);

	/*
	 * For a concurrent build, it's important to make the catalog entries
	 * visible to other transactions before we start to build the index. That
	 * will prevent them from making incompatible HOT updates.  The new index
	 * will be marked not indisready and not indisvalid, so that no one else
	 * tries to either insert into it or use it for queries.
	 *
	 * We must commit our current transaction so that the index becomes
	 * visible; then start another.  Note that all the data structures we just
	 * built are lost in the commit.  The only data we keep past here are the
	 * relation IDs.
	 *
	 * Before committing, get a session-level lock on the table, to ensure
	 * that neither it nor the index can be dropped before we finish. This
	 * cannot block, even if someone else is waiting for access, because we
	 * already have the same lock within our transaction.
	 *
	 * Note: we don't currently bother with a session lock on the index,
	 * because there are no operations that could change its state while we
	 * hold lock on the parent table.  This might need to change later.
	 */
	LockRelationIdForSession(&heaprelid, ShareUpdateExclusiveLock);

	PopActiveSnapshot();
	CommitTransactionCommand();
	StartTransactionCommand();

	/* Tell concurrent index builds to ignore us, if index qualifies */
	if (safe_index)
		set_indexsafe_procflags();

	/*
	 * The index is now visible, so we can report the OID.  While on it,
	 * include the report for the beginning of phase 2.
	 */
	{

	}

	/*
	 * Phase 2 of concurrent index build (see comments for validate_index()
	 * for an overview of how this works)
	 *
	 * Now we must wait until no running transaction could have the table open
	 * with the old list of indexes.  Use ShareLock to consider running
	 * transactions that hold locks that permit writing to the table.  Note we
	 * do not need to worry about xacts that open the table for writing after
	 * this point; they will see the new index when they open it.
	 *
	 * Note: the reason we use actual lock acquisition here, rather than just
	 * checking the ProcArray and sleeping, is that deadlock is possible if
	 * one of the transactions in question is blocked trying to acquire an
	 * exclusive lock on our table.  The lock code will detect deadlock and
	 * error out properly.
	 */
	WaitForLockers(heaplocktag, ShareLock);

	/*
	 * At this moment we are sure that there are no transactions with the
	 * table open for write that don't have this new index in their list of
	 * indexes.  We have waited out all the existing transactions and any new
	 * transaction will have the new index in its list, but the index is still
	 * marked as "not-ready-for-inserts".  The index is consulted while
	 * deciding HOT-safety though.  This arrangement ensures that no new HOT
	 * chains can be created where the new tuple and the old tuple in the
	 * chain have different index keys.
	 *
	 * We now take a new snapshot, and build the index using all tuples that
	 * are visible in this snapshot.  We can be sure that any HOT updates to
	 * these tuples will be compatible with the index, since any updates made
	 * by transactions that didn't know about the index are now committed or
	 * rolled back.  Thus, each visible tuple is either the end of its
	 * HOT-chain or the extension of the chain is HOT-safe for this index.
	 */

	/* Set ActiveSnapshot since functions in the indexes may need it */
	PushActiveSnapshot(GetTransactionSnapshot());

	/* Perform concurrent build of index */
	index_concurrently_build(relationId, indexRelationId);

	/* we can do away with our snapshot */
	PopActiveSnapshot();

	/*
	 * Commit this transaction to make the indisready update visible.
	 */
	CommitTransactionCommand();
	StartTransactionCommand();

	/* Tell concurrent index builds to ignore us, if index qualifies */
	if (safe_index)
		set_indexsafe_procflags();

	/*
	 * Phase 3 of concurrent index build
	 *
	 * We once again wait until no transaction can have the table open with
	 * the index marked as read-only for updates.
	 */
	WaitForLockers(heaplocktag, ShareLock);

	/*
	 * Now take the "reference snapshot" that will be used by validate_index()
	 * to filter candidate tuples.  Beware!  There might still be snapshots in
	 * use that treat some transaction as in-progress that our reference
	 * snapshot treats as committed.  If such a recently-committed transaction
	 * deleted tuples in the table, we will not include them in the index; yet
	 * those transactions which see the deleting one as still-in-progress will
	 * expect such tuples to be there once we mark the index as valid.
	 *
	 * We solve this by waiting for all endangered transactions to exit before
	 * we mark the index as valid.
	 *
	 * We also set ActiveSnapshot to this snap, since functions in indexes may
	 * need a snapshot.
	 */
	snapshot = RegisterSnapshot(GetTransactionSnapshot());
	PushActiveSnapshot(snapshot);

	/*
	 * Scan the index and the heap, insert any missing index entries.
	 */
	validate_index(relationId, indexRelationId, snapshot);

	/*
	 * Drop the reference snapshot.  We must do this before waiting out other
	 * snapshot holders, else we will deadlock against other processes also
	 * doing CREATE INDEX CONCURRENTLY, which would see our snapshot as one
	 * they must wait for.  But first, save the snapshot's xmin to use as
	 * limitXmin for GetCurrentVirtualXIDs().
	 */
	limitXmin = snapshot->xmin;

	PopActiveSnapshot();
	UnregisterSnapshot(snapshot);

	/*
	 * The snapshot subsystem could still contain registered snapshots that
	 * are holding back our process's advertised xmin; in particular, if
	 * default_transaction_isolation = serializable, there is a transaction
	 * snapshot that is still active.  The CatalogSnapshot is likewise a
	 * hazard.  To ensure no deadlocks, we must commit and start yet another
	 * transaction, and do our wait before any snapshot has been taken in it.
	 */
	CommitTransactionCommand();
	StartTransactionCommand();

	/* Tell concurrent index builds to ignore us, if index qualifies */
	if (safe_index)
		set_indexsafe_procflags();

	/* We should now definitely not be advertising any xmin. */
	Assert(MyProc->xmin == InvalidTransactionId);

	/*
	 * The index is now valid in the sense that it contains all currently
	 * interesting tuples.  But since it might not contain tuples deleted just
	 * before the reference snap was taken, we have to wait out any
	 * transactions that might have older snapshots.
	 */
	WaitForOlderSnapshots(limitXmin);

	/*
	 * Index can now be marked valid -- update its pg_index entry
	 */
	index_set_state_flags(indexRelationId, INDEX_CREATE_SET_VALID);

	/*
	 * The pg_index update will cause backends (including this one) to update
	 * relcache entries for the index itself, but we should also send a
	 * relcache inval on the parent table to force replanning of cached plans.
	 * Otherwise existing sessions might fail to use the new index where it
	 * would be useful.  (Note that our earlier commits did not create reasons
	 * to replan; so relcache flush on the index itself was sufficient.)
	 */
	CacheInvalidateRelcacheByRelid(heaprelid.relId);

	/*
	 * Last thing to do is release the session-level lock on the parent table.
	 */
	UnlockRelationIdForSession(&heaprelid, ShareUpdateExclusiveLock);


	return address;
}


/*
 * CheckPredicate
 *		Checks that the given partial-index predicate is valid.
 *
 * This used to also constrain the form of the predicate to forms that
 * indxpath.c could do something with.  However, that seems overly
 * restrictive.  One useful application of partial indexes is to apply
 * a UNIQUE constraint across a subset of a table, and in that scenario
 * any evaluable predicate will work.  So accept any predicate here
 * (except ones requiring a plan), and let indxpath.c fend for itself.
 */
static void
CheckPredicate(Expr *predicate)
{
	/*
	 * transformExpr() should have already rejected subqueries, aggregates,
	 * and window functions, based on the EXPR_KIND_ for a predicate.
	 */

	/*
	 * A predicate using mutable functions is probably wrong, for the same
	 * reasons that we don't allow an index expression to use one.
	 */
	if (contain_mutable_functions_after_planning(predicate))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("functions in index predicate must be marked IMMUTABLE")));
}

/*
 * Compute per-index-column information, including indexed column numbers
 * or index expressions, opclasses and their options. Note, all output vectors
 * should be allocated for all columns, including "including" ones.
 *
 * If the caller switched to the table owner, ddl_userid is the role for ACL
 * checks reached without traversing opaque expressions.  Otherwise, it's
 * InvalidOid, and other ddl_* arguments are undefined.
 */
static void
ComputeIndexAttrs(IndexInfo *indexInfo,
				  Oid *typeOidP,
				  Oid *classOidP,
				  int16 *colOptionP,
				  List *attList,	/* list of IndexElem's */
				  List *exclusionOpNames,
				  Oid relId,
				  const char *accessMethodName,
				  Oid accessMethodId,
				  bool amcanorder,
				  bool isconstraint,
				  Oid ddl_userid,
				  int ddl_sec_context,
				  int *ddl_save_nestlevel)
{
	ListCell   *lc;
	int			attn;
	int			nkeycols = indexInfo->ii_NumIndexKeyAttrs;
	Oid			save_userid;
	int			save_sec_context;

	(void) exclusionOpNames;		/* EXCLUDE constraints are not supported */

	if (OidIsValid(ddl_userid))
		GetUserIdAndSecContext(&save_userid, &save_sec_context);

	/*
	 * process attributeList
	 */
	attn = 0;
	foreach(lc, attList)
	{
		IndexElem  *attribute = (IndexElem *) lfirst(lc);
		Oid			atttype;
		Oid			attcollation;

		/*
		 * Process the column-or-expression to be indexed.
		 */
		if (attribute->name != NULL)
		{
			/* Simple index attribute */
			HeapTuple	atttuple;
			Form_pg_attribute attform;

			Assert(attribute->expr == NULL);
			atttuple = SearchSysCacheAttName(relId, attribute->name);
			if (!HeapTupleIsValid(atttuple))
			{
				/* difference in error message spellings is historical */
				if (isconstraint)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" named in key does not exist",
									attribute->name)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" does not exist",
									attribute->name)));
			}
			attform = (Form_pg_attribute) GETSTRUCT(atttuple);
			indexInfo->ii_IndexAttrNumbers[attn] = attform->attnum;
			atttype = attform->atttypid;
			attcollation = get_typcollation(atttype);
			ReleaseSysCache(atttuple);
		}
		else
		{
			/* Index expression */
			Node	   *expr = attribute->expr;

			Assert(expr != NULL);

			if (attn >= nkeycols)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("expressions are not supported in included columns")));
			atttype = exprType(expr);
			attcollation = exprCollation(expr);

			/*
			 * Strip any top-level COLLATE clause.  This ensures that we treat
			 * "x COLLATE y" and "(x COLLATE y)" alike.
			 */
			while (IsA(expr, CollateExpr))
				expr = (Node *) ((CollateExpr *) expr)->arg;

			if (IsA(expr, Var) &&
				((Var *) expr)->varattno != InvalidAttrNumber)
			{
				/*
				 * User wrote "(column)" or "(column COLLATE something)".
				 * Treat it like simple attribute anyway.
				 */
				indexInfo->ii_IndexAttrNumbers[attn] = ((Var *) expr)->varattno;
			}
			else
			{
				indexInfo->ii_IndexAttrNumbers[attn] = 0;	/* marks expression */
				indexInfo->ii_Expressions = lappend(indexInfo->ii_Expressions,
													expr);

				/*
				 * transformExpr() should have already rejected subqueries,
				 * aggregates, and window functions, based on the EXPR_KIND_
				 * for an index expression.
				 */

				/*
				 * An expression using mutable functions is probably wrong,
				 * since if you aren't going to get the same result for the
				 * same data every time, it's not clear what the index entries
				 * mean at all.
				 */
				if (contain_mutable_functions_after_planning((Expr *) expr))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("functions in index expression must be marked IMMUTABLE")));
			}
		}

		typeOidP[attn] = atttype;

		/*
		 * Included columns have no opclass and no ordering options.
		 */
		if (attn >= nkeycols)
		{
			if (attribute->opclass)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("including column does not support an operator class")));
			if (attribute->ordering != SORTBY_DEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("including column does not support ASC/DESC options")));
			if (attribute->nulls_ordering != SORTBY_NULLS_DEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("including column does not support NULLS FIRST/LAST options")));

			classOidP[attn] = InvalidOid;
			colOptionP[attn] = 0;
			attn++;

			continue;
		}

		/*
		 * Check we have a collation iff it's a collatable type.  The only
		 * expected failures here are (1) COLLATE applied to a noncollatable
		 * type, or (2) index expression had an unresolved collation.  But we
		 * might as well code this to be a complete consistency check.
		 */
		if (type_is_collatable(atttype))
		{
			if (!OidIsValid(attcollation))
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_COLLATION),
						 errmsg("could not determine which collation to use for index expression"),
						 errhint("Use the COLLATE clause to set the collation explicitly.")));
		}
		else
		{
			if (OidIsValid(attcollation))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("collations are not supported by type %s",
								format_type_be(atttype))));
		}

		/*
		 * Identify the opclass to use.  Use of ddl_userid is necessary due to
		 * ACL checks therein.  This is safe despite opclasses containing
		 * opaque expressions (specifically, functions), because only
		 * superusers can define opclasses.
		 */
		if (OidIsValid(ddl_userid))
		{
			AtEOXact_GUC(false, *ddl_save_nestlevel);
			SetUserIdAndSecContext(ddl_userid, ddl_sec_context);
		}
		classOidP[attn] = ResolveOpClass(attribute->opclass,
										 atttype,
										 accessMethodName,
										 accessMethodId);
		if (OidIsValid(ddl_userid))
		{
			SetUserIdAndSecContext(save_userid, save_sec_context);
			*ddl_save_nestlevel = NewGUCNestLevel();
		}

		/*
		 * Set up the per-column options (indoption field).  For now, this is
		 * zero for any un-ordered index, while ordered indexes have DESC and
		 * NULLS FIRST/LAST options.
		 */
		colOptionP[attn] = 0;
		if (amcanorder)
		{
			/* default ordering is ASC */
			if (attribute->ordering == SORTBY_DESC)
				colOptionP[attn] |= INDOPTION_DESC;
			/* default null ordering is LAST for ASC, FIRST for DESC */
			if (attribute->nulls_ordering == SORTBY_NULLS_DEFAULT)
			{
				if (attribute->ordering == SORTBY_DESC)
					colOptionP[attn] |= INDOPTION_NULLS_FIRST;
			}
			else if (attribute->nulls_ordering == SORTBY_NULLS_FIRST)
				colOptionP[attn] |= INDOPTION_NULLS_FIRST;
		}
		else
		{
			/* index AM does not support ordering */
			if (attribute->ordering != SORTBY_DEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("access method \"%s\" does not support ASC/DESC options",
								accessMethodName)));
			if (attribute->nulls_ordering != SORTBY_NULLS_DEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("access method \"%s\" does not support NULLS FIRST/LAST options",
								accessMethodName)));
		}

		/* Set up the per-column opclass options (attoptions field). */
		if (attribute->opclassopts)
		{
			Assert(attn < nkeycols);

			if (!indexInfo->ii_OpclassOptions)
				indexInfo->ii_OpclassOptions =
					palloc0(sizeof(Datum) * indexInfo->ii_NumIndexAttrs);

			indexInfo->ii_OpclassOptions[attn] = (Datum) 0;
		}

		attn++;
	}
}

/*
 * Resolve possibly-defaulted operator class specification
 *
 * Note: This is used to resolve operator class specifications in index and
 * partition key definitions.
 */
Oid
ResolveOpClass(List *opclass, Oid attrType,
			   const char *accessMethodName, Oid accessMethodId)
{
	char	   *schemaname;
	char	   *opcname;
	HeapTuple	tuple;
	Form_pg_opclass opform;
	Oid			opClassId,
				opInputType;

	if (opclass == NIL)
	{
		/* no operator class specified, so find the default */
		opClassId = GetDefaultOpClass(attrType, accessMethodId);
		if (!OidIsValid(opClassId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("data type %s has no default operator class for access method \"%s\"",
							format_type_be(attrType), accessMethodName),
					 errhint("You must specify an operator class for the index or define a default operator class for the data type.")));
		return opClassId;
	}

	/*
	 * Specific opclass name given, so look up the opclass.
	 */

	/* deconstruct the name list */
	DeconstructQualifiedName(opclass, &schemaname, &opcname);

	if (schemaname)
	{
		/* Look in specific schema only */
		Oid			namespaceId;

		namespaceId = LookupExplicitNamespace(schemaname, false);
		tuple = SearchSysCache3(CLAAMNAMENSP,
								ObjectIdGetDatum(accessMethodId),
								PointerGetDatum(opcname),
								ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* Unqualified opclass name, so search the search path */
		opClassId = OpclassnameGetOpcid(accessMethodId, opcname);
		if (!OidIsValid(opClassId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
							opcname, accessMethodName)));
		tuple = SearchSysCache1(CLAOID, ObjectIdGetDatum(opClassId));
	}

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
						NameListToString(opclass), accessMethodName)));

	/*
	 * Verify that the index operator class accepts this datatype.  Note we
	 * will accept binary compatibility.
	 */
	opform = (Form_pg_opclass) GETSTRUCT(tuple);
	opClassId = opform->oid;
	opInputType = opform->opcintype;

	if (!IsBinaryCoercible(attrType, opInputType))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("operator class \"%s\" does not accept data type %s",
						NameListToString(opclass), format_type_be(attrType))));

	ReleaseSysCache(tuple);

	return opClassId;
}

/*
 * GetDefaultOpClass
 *
 * Given the OIDs of a datatype and an access method, find the default
 * operator class, if any.  Returns InvalidOid if there is none.
 */
Oid
GetDefaultOpClass(Oid type_id, Oid am_id)
{
	Oid			result = InvalidOid;
	int			nexact = 0;
	int			ncompatible = 0;
	int			ncompatiblepreferred = 0;
	Relation	rel;
	ScanKeyData skey[1];
	SysScanDesc scan;
	HeapTuple	tup;
	TYPCATEGORY tcategory;

	/* If it's a domain, look at the base type instead */
	type_id = getBaseType(type_id);

	tcategory = TypeCategory(type_id);

	/*
	 * We scan through all the opclasses available for the access method,
	 * looking for one that is marked default and matches the target type
	 * (either exactly or binary-compatibly, but prefer an exact match).
	 *
	 * We could find more than one binary-compatible match.  If just one is
	 * for a preferred type, use that one; otherwise we fail, forcing the user
	 * to specify which one he wants.  (The preferred-type special case is a
	 * kluge for varchar: it's binary-compatible to both text and bpchar, so
	 * we need a tiebreaker.)  If we find more than one exact match, then
	 * someone put bogus entries in pg_opclass.
	 */
	rel = table_open(OperatorClassRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_opclass_opcmethod,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(am_id));

	scan = systable_beginscan(rel, OpclassAmNameNspIndexId, true,
							  NULL, 1, skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_opclass opclass = (Form_pg_opclass) GETSTRUCT(tup);

		/* ignore altogether if not a default opclass */
		if (!opclass->opcdefault)
			continue;
		if (opclass->opcintype == type_id)
		{
			nexact++;
			result = opclass->oid;
		}
		else if (nexact == 0 &&
				 IsBinaryCoercible(type_id, opclass->opcintype))
		{
			if (IsPreferredType(tcategory, opclass->opcintype))
			{
				ncompatiblepreferred++;
				result = opclass->oid;
			}
			else if (ncompatiblepreferred == 0)
			{
				ncompatible++;
				result = opclass->oid;
			}
		}
	}

	systable_endscan(scan);

	table_close(rel, AccessShareLock);

	/* raise error if pg_opclass contains inconsistent data */
	if (nexact > 1)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("there are multiple default operator classes for data type %s",
						format_type_be(type_id))));

	if (nexact == 1 ||
		ncompatiblepreferred == 1 ||
		(ncompatiblepreferred == 0 && ncompatible == 1))
		return result;

	return InvalidOid;
}

/*
 *	makeObjectName()
 *
 *	Create a name for an implicitly created index, sequence, constraint,
 *	extended statistics, etc.
 *
 *	The parameters are typically: the original table name, the original field
 *	name, and a "type" string (such as "seq" or "pkey").    The field name
 *	and/or type can be NULL if not relevant.
 *
 *	The result is a palloc'd string.
 *
 *	The basic result we want is "name1_name2_label", omitting "_name2" or
 *	"_label" when those parameters are NULL.  However, we must generate
 *	a name with less than NAMEDATALEN characters!  So, we truncate one or
 *	both names if necessary to make a short-enough string.  The label part
 *	is never truncated (so it had better be reasonably short).
 *
 *	The caller is responsible for checking uniqueness of the generated
 *	name and retrying as needed; retrying will be done by altering the
 *	"label" string (which is why we never truncate that part).
 */
char *
makeObjectName(const char *name1, const char *name2, const char *label)
{
	char	   *name;
	int			overhead = 0;	/* chars needed for label and underscores */
	int			availchars;		/* chars available for name(s) */
	int			name1chars;		/* chars allocated to name1 */
	int			name2chars;		/* chars allocated to name2 */
	int			ndx;

	name1chars = strlen(name1);
	if (name2)
	{
		name2chars = strlen(name2);
		overhead++;				/* allow for separating underscore */
	}
	else
		name2chars = 0;
	if (label)
		overhead += strlen(label) + 1;

	availchars = NAMEDATALEN - 1 - overhead;
	Assert(availchars > 0);		/* else caller chose a bad label */

	/*
	 * If we must truncate,  preferentially truncate the longer name. This
	 * logic could be expressed without a loop, but it's simple and obvious as
	 * a loop.
	 */
	while (name1chars + name2chars > availchars)
	{
		if (name1chars > name2chars)
			name1chars--;
		else
			name2chars--;
	}

	name1chars = pg_mbcliplen(name1, name1chars, name1chars);
	if (name2)
		name2chars = pg_mbcliplen(name2, name2chars, name2chars);

	/* Now construct the string using the chosen lengths */
	name = palloc(name1chars + name2chars + overhead + 1);
	memcpy(name, name1, name1chars);
	ndx = name1chars;
	if (name2)
	{
		name[ndx++] = '_';
		memcpy(name + ndx, name2, name2chars);
		ndx += name2chars;
	}
	if (label)
	{
		name[ndx++] = '_';
		strcpy(name + ndx, label);
	}
	else
		name[ndx] = '\0';

	return name;
}

/*
 * Select a nonconflicting name for a new relation.  This is ordinarily
 * used to choose index names (which is why it's here) but it can also
 * be used for sequences, or any autogenerated relation kind.
 *
 * name1, name2, and label are used the same way as for makeObjectName(),
 * except that the label can't be NULL; digits will be appended to the label
 * if needed to create a name that is unique within the specified namespace.
 *
 * If isconstraint is true, we also avoid choosing a name matching any
 * existing constraint in the same namespace.  (This is stricter than what
 * Postgres itself requires, but the SQL standard says that constraint names
 * should be unique within schemas, so we follow that for autogenerated
 * constraint names.)
 *
 * Note: it is theoretically possible to get a collision anyway, if someone
 * else chooses the same name concurrently.  We shorten the race condition
 * window by checking for conflicting relations using SnapshotDirty, but
 * that doesn't close the window entirely.  This is fairly unlikely to be
 * a problem in practice, especially if one is holding an exclusive lock on
 * the relation identified by name1.  However, if choosing multiple names
 * within a single command, you'd better create the new object and do
 * CommandCounterIncrement before choosing the next one!
 *
 * Returns a palloc'd string.
 */
char *
ChooseRelationName(const char *name1, const char *name2,
				   const char *label, Oid namespaceid,
				   bool isconstraint)
{
	int			pass = 0;
	char	   *relname = NULL;
	char		modlabel[NAMEDATALEN];
	SnapshotData SnapshotDirty;
	Relation	pgclassrel;

	/* prepare to search pg_class with a dirty snapshot */
	InitDirtySnapshot(SnapshotDirty);
	pgclassrel = table_open(RelationRelationId, AccessShareLock);

	/* try the unmodified label first */
	strlcpy(modlabel, label, sizeof(modlabel));

	for (;;)
	{
		ScanKeyData key[2];
		SysScanDesc scan;
		bool		collides;

		relname = makeObjectName(name1, name2, modlabel);

		/* is there any conflicting relation name? */
		ScanKeyInit(&key[0],
					Anum_pg_class_relname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(relname));
		ScanKeyInit(&key[1],
					Anum_pg_class_relnamespace,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(namespaceid));

		scan = systable_beginscan(pgclassrel, ClassNameNspIndexId,
								  true /* indexOK */ ,
								  &SnapshotDirty,
								  2, key);

		collides = HeapTupleIsValid(systable_getnext(scan));

		systable_endscan(scan);

		/* break out of loop if no conflict */
		if (!collides)
		{
			if (!isconstraint ||
				!ConstraintNameExists(relname, namespaceid))
				break;
		}

		/* found a conflict, so try a new name component */
		pfree(relname);
		snprintf(modlabel, sizeof(modlabel), "%s%d", label, ++pass);
	}

	table_close(pgclassrel, AccessShareLock);

	return relname;
}

/*
 * Select the name to be used for an index.
 *
 * The argument list is pretty ad-hoc :-(
 */
static char *
ChooseIndexName(const char *tabname, Oid namespaceId,
				List *colnames, List *exclusionOpNames,
				bool primary, bool isconstraint)
{
	char	   *indexname;

	(void) exclusionOpNames;		/* EXCLUDE constraints are not supported */

	if (primary)
	{
		/* the primary key's name does not depend on the specific column(s) */
		indexname = ChooseRelationName(tabname,
									   NULL,
									   "pkey",
									   namespaceId,
									   true);
	}
	else if (isconstraint)
	{
		indexname = ChooseRelationName(tabname,
									   ChooseIndexNameAddition(colnames),
									   "key",
									   namespaceId,
									   true);
	}
	else
	{
		indexname = ChooseRelationName(tabname,
									   ChooseIndexNameAddition(colnames),
									   "idx",
									   namespaceId,
									   false);
	}

	return indexname;
}

/*
 * Generate "name2" for a new index given the list of column names for it
 * (as produced by ChooseIndexColumnNames).  This will be passed to
 * ChooseRelationName along with the parent table name and a suitable label.
 *
 * We know that less than NAMEDATALEN characters will actually be used,
 * so we can truncate the result once we've generated that many.
 *
 * XXX See also ChooseForeignKeyConstraintNameAddition and
 * ChooseExtendedStatisticNameAddition.
 */
static char *
ChooseIndexNameAddition(List *colnames)
{
	char		buf[NAMEDATALEN * 2];
	int			buflen = 0;
	ListCell   *lc;

	buf[0] = '\0';
	foreach(lc, colnames)
	{
		const char *name = (const char *) lfirst(lc);

		if (buflen > 0)
			buf[buflen++] = '_';	/* insert _ between names */

		/*
		 * At this point we have buflen <= NAMEDATALEN.  name should be less
		 * than NAMEDATALEN already, but use strlcpy for paranoia.
		 */
		strlcpy(buf + buflen, name, NAMEDATALEN);
		buflen += strlen(buf + buflen);
		if (buflen >= NAMEDATALEN)
			break;
	}
	return pstrdup(buf);
}

/*
 * Select the actual names to be used for the columns of an index, given the
 * list of IndexElems for the columns.  This is mostly about ensuring the
 * names are unique so we don't get a conflicting-attribute-names error.
 *
 * Returns a List of plain strings (char *, not String nodes).
 */
static List *
ChooseIndexColumnNames(List *indexElems)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, indexElems)
	{
		IndexElem  *ielem = (IndexElem *) lfirst(lc);
		const char *origname;
		const char *curname;
		int			i;
		char		buf[NAMEDATALEN];

		/* Get the preliminary name from the IndexElem */
		if (ielem->indexcolname)
			origname = ielem->indexcolname; /* caller-specified name */
		else if (ielem->name)
			origname = ielem->name; /* simple column reference */
		else
			origname = "expr";	/* default name for expression */

		/* If it conflicts with any previous column, tweak it */
		curname = origname;
		for (i = 1;; i++)
		{
			ListCell   *lc2;
			char		nbuf[32];
			int			nlen;

			foreach(lc2, result)
			{
				if (strcmp(curname, (char *) lfirst(lc2)) == 0)
					break;
			}
			if (lc2 == NULL)
				break;			/* found nonconflicting name */

			sprintf(nbuf, "%d", i);

			/* Ensure generated names are shorter than NAMEDATALEN */
			nlen = pg_mbcliplen(origname, strlen(origname),
								NAMEDATALEN - 1 - strlen(nbuf));
			memcpy(buf, origname, nlen);
			strcpy(buf + nlen, nbuf);
			curname = buf;
		}

		/* And attach to the result list */
		result = lappend(result, pstrdup(curname));
	}
	return result;
}


/*
 * Set the PROC_IN_SAFE_IC flag in MyProc->statusFlags.
 *
 * When doing concurrent index builds, we can set this flag
 * to tell other processes concurrently running CREATE
 * INDEX CONCURRENTLY or REINDEX CONCURRENTLY to ignore us when
 * doing their waits for concurrent snapshots.  On one hand it
 * avoids pointlessly waiting for a process that's not interesting
 * anyway; but more importantly it avoids deadlocks in some cases.
 *
 * This can be done safely only for indexes that don't execute any
 * expressions that could access other tables, so index must not be
 * expressional nor partial.  Caller is responsible for only calling
 * this routine when that assumption holds true.
 *
 * (The flag is reset automatically at transaction end, so it must be
 * set for each transaction.)
 */
static inline void
set_indexsafe_procflags(void)
{
	/*
	 * This should only be called before installing xid or xmin in MyProc;
	 * otherwise, concurrent processes could see an Xmin that moves backwards.
	 */
	Assert(MyProc->xid == InvalidTransactionId &&
		   MyProc->xmin == InvalidTransactionId);

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	MyProc->statusFlags |= PROC_IN_SAFE_IC;
	ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
	LWLockRelease(ProcArrayLock);
}
