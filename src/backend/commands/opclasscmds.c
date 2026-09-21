/*-------------------------------------------------------------------------
 *
 * opclasscmds.c
 *
 *	  Routines for opclass (and opfamily) name lookup.
 *
 * minipg note: the CREATE/ALTER/DROP OPERATOR CLASS and OPERATOR FAMILY
 * DDL has been cropped (the parser no longer accepts it), so all that
 * remains here are the name-to-OID lookups needed by the index AM code.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/opclasscmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "commands/defrem.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

/*
 * OpFamilyCacheLookup
 *		Look up an existing opfamily by name.
 *
 * Returns a syscache tuple reference, or NULL if not found.
 */
static HeapTuple
OpFamilyCacheLookup(Oid amID, List *opfamilyname, bool missing_ok)
{
	char	   *schemaname;
	char	   *opfname;
	HeapTuple	htup;

	/* deconstruct the name list */
	DeconstructQualifiedName(opfamilyname, &schemaname, &opfname);

	if (schemaname)
	{
		/* Look in specific schema only */
		Oid			namespaceId;

		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (!OidIsValid(namespaceId))
			htup = NULL;
		else
			htup = SearchSysCache3(OPFAMILYAMNAMENSP,
								   ObjectIdGetDatum(amID),
								   PointerGetDatum(opfname),
								   ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* Unqualified opfamily name, so search the search path */
		Oid			opfID = OpfamilynameGetOpfid(amID, opfname);

		if (!OidIsValid(opfID))
			htup = NULL;
		else
			htup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opfID));
	}

	if (!HeapTupleIsValid(htup) && !missing_ok)
	{
		HeapTuple	amtup;

		amtup = SearchSysCache1(AMOID, ObjectIdGetDatum(amID));
		if (!HeapTupleIsValid(amtup))
			elog(ERROR, "cache lookup failed for access method %u", amID);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator family \"%s\" does not exist for access method \"%s\"",
						NameListToString(opfamilyname),
						NameStr(((Form_pg_am) GETSTRUCT(amtup))->amname))));
	}

	return htup;
}

/*
 * get_opfamily_oid
 *	  find an opfamily OID by possibly qualified name
 *
 * If not found, returns InvalidOid if missing_ok, else throws error.
 */
Oid
get_opfamily_oid(Oid amID, List *opfamilyname, bool missing_ok)
{
	HeapTuple	htup;
	Form_pg_opfamily opfamform;
	Oid			opfID;

	htup = OpFamilyCacheLookup(amID, opfamilyname, missing_ok);
	if (!HeapTupleIsValid(htup))
		return InvalidOid;
	opfamform = (Form_pg_opfamily) GETSTRUCT(htup);
	opfID = opfamform->oid;
	ReleaseSysCache(htup);

	return opfID;
}

/*
 * OpClassCacheLookup
 *		Look up an existing opclass by name.
 *
 * Returns a syscache tuple reference, or NULL if not found.
 */
static HeapTuple
OpClassCacheLookup(Oid amID, List *opclassname, bool missing_ok)
{
	char	   *schemaname;
	char	   *opcname;
	HeapTuple	htup;

	/* deconstruct the name list */
	DeconstructQualifiedName(opclassname, &schemaname, &opcname);

	if (schemaname)
	{
		/* Look in specific schema only */
		Oid			namespaceId;

		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (!OidIsValid(namespaceId))
			htup = NULL;
		else
			htup = SearchSysCache3(CLAAMNAMENSP,
								   ObjectIdGetDatum(amID),
								   PointerGetDatum(opcname),
								   ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* Unqualified opclass name, so search the search path */
		Oid			opcID = OpclassnameGetOpcid(amID, opcname);

		if (!OidIsValid(opcID))
			htup = NULL;
		else
			htup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opcID));
	}

	if (!HeapTupleIsValid(htup) && !missing_ok)
	{
		HeapTuple	amtup;

		amtup = SearchSysCache1(AMOID, ObjectIdGetDatum(amID));
		if (!HeapTupleIsValid(amtup))
			elog(ERROR, "cache lookup failed for access method %u", amID);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
						NameListToString(opclassname),
						NameStr(((Form_pg_am) GETSTRUCT(amtup))->amname))));
	}

	return htup;
}

/*
 * get_opclass_oid
 *	  find an opclass OID by possibly qualified name
 *
 * If not found, returns InvalidOid if missing_ok, else throws error.
 */
Oid
get_opclass_oid(Oid amID, List *opclassname, bool missing_ok)
{
	HeapTuple	htup;
	Form_pg_opclass opcform;
	Oid			opcID;

	htup = OpClassCacheLookup(amID, opclassname, missing_ok);
	if (!HeapTupleIsValid(htup))
		return InvalidOid;
	opcform = (Form_pg_opclass) GETSTRUCT(htup);
	opcID = opcform->oid;
	ReleaseSysCache(htup);

	return opcID;
}

