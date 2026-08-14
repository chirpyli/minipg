/*-------------------------------------------------------------------------
 *
 * proclang.c
 *	  PostgreSQL LANGUAGE support code.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/proclang.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_language.h"
#include "commands/proclang.h"
#include "utils/syscache.h"


/*
 * get_language_oid - given a language name, look up the OID
 *
 * If missing_ok is false, throw an error if language name not found.  If
 * true, just return InvalidOid.
 */
Oid
get_language_oid(const char *langname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(LANGNAME, Anum_pg_language_oid,
						  CStringGetDatum(langname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("language \"%s\" does not exist", langname)));
	return oid;
}
