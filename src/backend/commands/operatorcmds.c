/*-------------------------------------------------------------------------
 *
 * operatorcmds.c
 *
 *	  Guts of operator deletion.
 *
 * minipg note: CREATE/ALTER/DROP OPERATOR have been cropped (the parser no
 * longer accepts them), so only the deletion helper used by generic object
 * deletion is left here.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/operatorcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_operator.h"
#include "commands/defrem.h"
#include "utils/syscache.h"

/*
 * Guts of operator deletion.
 */
void
RemoveOperatorById(Oid operOid)
{
	Relation	relation;
	HeapTuple	tup;
	Form_pg_operator op;

	relation = table_open(OperatorRelationId, RowExclusiveLock);

	tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operOid));
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for operator %u", operOid);
	op = (Form_pg_operator) GETSTRUCT(tup);

	/*
	 * Reset links from commutator and negator, if any.  In case of a
	 * self-commutator or self-negator, this means we have to re-fetch the
	 * updated tuple.  (We could optimize away updates on the tuple we're
	 * about to drop, but it doesn't seem worth convoluting the logic for.)
	 */
	if (OidIsValid(op->oprcom) || OidIsValid(op->oprnegate))
	{
		OperatorUpd(operOid, op->oprcom, op->oprnegate, true);
		if (operOid == op->oprcom || operOid == op->oprnegate)
		{
			ReleaseSysCache(tup);
			tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operOid));
			if (!HeapTupleIsValid(tup)) /* should not happen */
				elog(ERROR, "cache lookup failed for operator %u", operOid);
		}
	}

	CatalogTupleDelete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(relation, RowExclusiveLock);
}

