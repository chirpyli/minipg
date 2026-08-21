/*
 * dbsize.c
 *		Database object size functions, and related inquiries
 *
 * Copyright (c) 2002-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/dbsize.c
 *
 */

#include "postgres.h"

#include <sys/stat.h>

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_tablespace.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/relfilenodemap.h"
#include "utils/relmapper.h"
#include "utils/syscache.h"

/* Divide by two and round away from zero */
#define half_rounded(x)   (((x) + ((x) < 0 ? -1 : 1)) / 2)

/* Return physical size of directory contents, or 0 if dir doesn't exist */
static int64
db_dir_size(const char *path)
{
	int64		dirsize = 0;
	struct dirent *direntry;
	DIR		   *dirdesc;
	char		filename[MAXPGPATH * 2];

	dirdesc = AllocateDir(path);

	if (!dirdesc)
		return 0;

	while ((direntry = ReadDir(dirdesc, path)) != NULL)
	{
		struct stat fst;

		CHECK_FOR_INTERRUPTS();

		if (strcmp(direntry->d_name, ".") == 0 ||
			strcmp(direntry->d_name, "..") == 0)
			continue;

		snprintf(filename, sizeof(filename), "%s/%s", path, direntry->d_name);

		if (stat(filename, &fst) < 0)
		{
			if (errno == ENOENT)
				continue;
			else
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m", filename)));
		}
		dirsize += fst.st_size;
	}

	FreeDir(dirdesc);
	return dirsize;
}

/*
 * calculate size of database
 *
 * 表空间管理已裁剪：数据库存储只存在于 pg_default（base/）中。
 */
static int64
calculate_database_size(Oid dbOid)
{
	int64		totalsize;
	char		pathname[MAXPGPATH];

	/* Shared storage in pg_global is not counted */

	/* Include pg_default storage */
	snprintf(pathname, sizeof(pathname), "base/%u", dbOid);
	totalsize = db_dir_size(pathname);

	return totalsize;
}

Datum
pg_database_size_oid(PG_FUNCTION_ARGS)
{
	Oid			dbOid = PG_GETARG_OID(0);
	int64		size;

	size = calculate_database_size(dbOid);

	if (size == 0)
		PG_RETURN_NULL();

	PG_RETURN_INT64(size);
}

Datum
pg_database_size_name(PG_FUNCTION_ARGS)
{
	Name		dbName = PG_GETARG_NAME(0);
	Oid			dbOid = get_database_oid(NameStr(*dbName), false);
	int64		size;

	size = calculate_database_size(dbOid);

	if (size == 0)
		PG_RETURN_NULL();

	PG_RETURN_INT64(size);
}


/*
 * calculate size of (one fork of) a relation
 *
 * Note: we can safely apply this to temp tables of other sessions, so there
 * is no check here or at the call sites for that.
 */
static int64
calculate_relation_size(RelFileNode *rfn, BackendId backend, ForkNumber forknum)
{
	int64		totalsize = 0;
	char	   *relationpath;
	char		pathname[MAXPGPATH];
	unsigned int segcount = 0;

	relationpath = relpathbackend(*rfn, backend, forknum);

	for (segcount = 0;; segcount++)
	{
		struct stat fst;

		CHECK_FOR_INTERRUPTS();

		if (segcount == 0)
			snprintf(pathname, MAXPGPATH, "%s",
					 relationpath);
		else
			snprintf(pathname, MAXPGPATH, "%s.%u",
					 relationpath, segcount);

		if (stat(pathname, &fst) < 0)
		{
			if (errno == ENOENT)
				break;
			else
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m", pathname)));
		}
		totalsize += fst.st_size;
	}

	return totalsize;
}

Datum
pg_relation_size(PG_FUNCTION_ARGS)
{
	Oid			relOid = PG_GETARG_OID(0);
	text	   *forkName = PG_GETARG_TEXT_PP(1);
	Relation	rel;
	int64		size;

	rel = try_relation_open(relOid, AccessShareLock);

	/*
	 * Before 9.2, we used to throw an error if the relation didn't exist, but
	 * that makes queries like "SELECT pg_relation_size(oid) FROM pg_class"
	 * less robust, because while we scan pg_class with an MVCC snapshot,
	 * someone else might drop the table. It's better to return NULL for
	 * already-dropped tables than throw an error and abort the whole query.
	 */
	if (rel == NULL)
		PG_RETURN_NULL();

	size = calculate_relation_size(&(rel->rd_node), rel->rd_backend,
								   forkname_to_number(text_to_cstring(forkName)));

	relation_close(rel, AccessShareLock);

	PG_RETURN_INT64(size);
}

/*
 * Calculate total on-disk size of a TOAST relation, including its indexes.
 * Must not be applied to non-TOAST relations.
 */
static int64
calculate_toast_table_size(Oid toastrelid)
{
	int64		size = 0;
	Relation	toastRel;
	ForkNumber	forkNum;
	ListCell   *lc;
	List	   *indexlist;

	toastRel = relation_open(toastrelid, AccessShareLock);

	/* toast heap size, including FSM and VM size */
	for (forkNum = 0; forkNum <= MAX_FORKNUM; forkNum++)
		size += calculate_relation_size(&(toastRel->rd_node),
										toastRel->rd_backend, forkNum);

	/* toast index size, including FSM and VM size */
	indexlist = RelationGetIndexList(toastRel);

	/* Size is calculated using all the indexes available */
	foreach(lc, indexlist)
	{
		Relation	toastIdxRel;

		toastIdxRel = relation_open(lfirst_oid(lc),
									AccessShareLock);
		for (forkNum = 0; forkNum <= MAX_FORKNUM; forkNum++)
			size += calculate_relation_size(&(toastIdxRel->rd_node),
											toastIdxRel->rd_backend, forkNum);

		relation_close(toastIdxRel, AccessShareLock);
	}
	list_free(indexlist);
	relation_close(toastRel, AccessShareLock);

	return size;
}

/*
 * Calculate total on-disk size of a given table,
 * including FSM and VM, plus TOAST table if any.
 * Indexes other than the TOAST table's index are not included.
 *
 * Note that this also behaves sanely if applied to an index or toast table;
 * those won't have attached toast tables, but they can have multiple forks.
 */
static int64
calculate_table_size(Relation rel)
{
	int64		size = 0;
	ForkNumber	forkNum;

	/*
	 * heap size, including FSM and VM
	 */
	for (forkNum = 0; forkNum <= MAX_FORKNUM; forkNum++)
		size += calculate_relation_size(&(rel->rd_node), rel->rd_backend,
										forkNum);

	/*
	 * Size of toast relation
	 */
	if (OidIsValid(rel->rd_rel->reltoastrelid))
		size += calculate_toast_table_size(rel->rd_rel->reltoastrelid);

	return size;
}

/*
 * Calculate total on-disk size of all indexes attached to the given table.
 *
 * Can be applied safely to an index, but you'll just get zero.
 */
static int64
calculate_indexes_size(Relation rel)
{
	int64		size = 0;

	/*
	 * Aggregate all indexes on the given relation
	 */
	if (rel->rd_rel->relhasindex)
	{
		List	   *index_oids = RelationGetIndexList(rel);
		ListCell   *cell;

		foreach(cell, index_oids)
		{
			Oid			idxOid = lfirst_oid(cell);
			Relation	idxRel;
			ForkNumber	forkNum;

			idxRel = relation_open(idxOid, AccessShareLock);

			for (forkNum = 0; forkNum <= MAX_FORKNUM; forkNum++)
				size += calculate_relation_size(&(idxRel->rd_node),
												idxRel->rd_backend,
												forkNum);

			relation_close(idxRel, AccessShareLock);
		}

		list_free(index_oids);
	}

	return size;
}

Datum
pg_table_size(PG_FUNCTION_ARGS)
{
	Oid			relOid = PG_GETARG_OID(0);
	Relation	rel;
	int64		size;

	rel = try_relation_open(relOid, AccessShareLock);

	if (rel == NULL)
		PG_RETURN_NULL();

	size = calculate_table_size(rel);

	relation_close(rel, AccessShareLock);

	PG_RETURN_INT64(size);
}

Datum
pg_indexes_size(PG_FUNCTION_ARGS)
{
	Oid			relOid = PG_GETARG_OID(0);
	Relation	rel;
	int64		size;

	rel = try_relation_open(relOid, AccessShareLock);

	if (rel == NULL)
		PG_RETURN_NULL();

	size = calculate_indexes_size(rel);

	relation_close(rel, AccessShareLock);

	PG_RETURN_INT64(size);
}

/*
 *	Compute the on-disk size of all files for the relation,
 *	including heap data, index data, toast data, FSM, VM.
 */
static int64
calculate_total_relation_size(Relation rel)
{
	int64		size;

	/*
	 * Aggregate the table size, this includes size of the heap, toast and
	 * toast index with free space and visibility map
	 */
	size = calculate_table_size(rel);

	/*
	 * Add size of all attached indexes as well
	 */
	size += calculate_indexes_size(rel);

	return size;
}

Datum
pg_total_relation_size(PG_FUNCTION_ARGS)
{
	Oid			relOid = PG_GETARG_OID(0);
	Relation	rel;
	int64		size;

	rel = try_relation_open(relOid, AccessShareLock);

	if (rel == NULL)
		PG_RETURN_NULL();

	size = calculate_total_relation_size(rel);

	relation_close(rel, AccessShareLock);

	PG_RETURN_INT64(size);
}

/*
 * formatting with size units
 */
Datum
pg_size_pretty(PG_FUNCTION_ARGS)
{
	int64		size = PG_GETARG_INT64(0);
	char		buf[64];
	int64		limit = 10 * 1024;
	int64		limit2 = limit * 2 - 1;

	if (Abs(size) < limit)
		snprintf(buf, sizeof(buf), INT64_FORMAT " bytes", size);
	else
	{
		/*
		 * We use divide instead of bit shifting so that behavior matches for
		 * both positive and negative size values.
		 */
		size /= (1 << 9);		/* keep one extra bit for rounding */
		if (Abs(size) < limit2)
			snprintf(buf, sizeof(buf), INT64_FORMAT " kB",
					 half_rounded(size));
		else
		{
			size /= (1 << 10);
			if (Abs(size) < limit2)
				snprintf(buf, sizeof(buf), INT64_FORMAT " MB",
						 half_rounded(size));
			else
			{
				size /= (1 << 10);
				if (Abs(size) < limit2)
					snprintf(buf, sizeof(buf), INT64_FORMAT " GB",
							 half_rounded(size));
				else
				{
					size /= (1 << 10);
					snprintf(buf, sizeof(buf), INT64_FORMAT " TB",
							 half_rounded(size));
				}
			}
		}
	}

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/*
 * Convert a human-readable size to a size in bytes
 */
Datum
pg_size_bytes(PG_FUNCTION_ARGS)
{
	text	   *arg = PG_GETARG_TEXT_PP(0);
	char	   *str,
			   *strptr,
			   *endptr;
	char		saved_char;
	double		num;
	int64		result;
	bool		have_digits = false;

	str = text_to_cstring(arg);

	/* Skip leading whitespace */
	strptr = str;
	while (isspace((unsigned char) *strptr))
		strptr++;

	/* Check that we have a valid number and determine where it ends */
	endptr = strptr;

	/* Part (1): sign */
	if (*endptr == '-' || *endptr == '+')
		endptr++;

	/* Part (2): main digit string */
	if (isdigit((unsigned char) *endptr))
	{
		have_digits = true;
		do
			endptr++;
		while (isdigit((unsigned char) *endptr));
	}

	/* Part (3): optional decimal point and fractional digits */
	if (*endptr == '.')
	{
		endptr++;
		if (isdigit((unsigned char) *endptr))
		{
			have_digits = true;
			do
				endptr++;
			while (isdigit((unsigned char) *endptr));
		}
	}

	/* Complain if we don't have a valid number at this point */
	if (!have_digits)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid size: \"%s\"", str)));

	/* Part (4): optional exponent */
	if (*endptr == 'e' || *endptr == 'E')
	{
		long		exponent;
		char	   *cp;

		/*
		 * Note we might one day support EB units, so if what follows 'E'
		 * isn't a number, just treat it all as a unit to be parsed.
		 */
		exponent = strtol(endptr + 1, &cp, 10);
		(void) exponent;		/* Silence -Wunused-result warnings */
		if (cp > endptr + 1)
			endptr = cp;
	}

	/*
	 * Parse the number, saving the next character, which may be the first
	 * character of the unit string.
	 */
	saved_char = *endptr;
	*endptr = '\0';

	num = strtod(strptr, NULL);

	*endptr = saved_char;

	/* Skip whitespace between number and unit */
	strptr = endptr;
	while (isspace((unsigned char) *strptr))
		strptr++;

	/* Handle possible unit */
	if (*strptr != '\0')
	{
		int64		multiplier = 0;

		/* Trim any trailing whitespace */
		endptr = str + VARSIZE_ANY_EXHDR(arg) - 1;

		while (isspace((unsigned char) *endptr))
			endptr--;

		endptr++;
		*endptr = '\0';

		/* Parse the unit case-insensitively */
		if (pg_strcasecmp(strptr, "bytes") == 0)
			multiplier = (int64) 1;
		else if (pg_strcasecmp(strptr, "kb") == 0)
			multiplier = (int64) 1024;
		else if (pg_strcasecmp(strptr, "mb") == 0)
			multiplier = ((int64) 1024) * 1024;

		else if (pg_strcasecmp(strptr, "gb") == 0)
			multiplier = ((int64) 1024) * 1024 * 1024;

		else if (pg_strcasecmp(strptr, "tb") == 0)
			multiplier = ((int64) 1024) * 1024 * 1024 * 1024;

		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid size: \"%s\"", text_to_cstring(arg)),
					 errdetail("Invalid size unit: \"%s\".", strptr),
					 errhint("Valid units are \"bytes\", \"kB\", \"MB\", \"GB\", and \"TB\".")));

		if (multiplier > 1)
			num = num * (double) multiplier;
	}

	if (num > (double) PG_INT64_MAX || num < (double) PG_INT64_MIN)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("size \"%s\" is out of range", text_to_cstring(arg))));

	result = (int64) num;

	PG_RETURN_INT64(result);
}

/*
 * Get the filenode of a relation
 *
 * This is expected to be used in queries like
 *		SELECT pg_relation_filenode(oid) FROM pg_class;
 * That leads to a couple of choices.  We work from the pg_class row alone
 * rather than actually opening each relation, for efficiency.  We don't
 * fail if we can't find the relation --- some rows might be visible in
 * the query's MVCC snapshot even though the relations have been dropped.
 * (Note: we could avoid using the catcache, but there's little point
 * because the relation mapper also works "in the now".)  We also don't
 * fail if the relation doesn't have storage.  In all these cases it
 * seems better to quietly return NULL.
 */
Datum
pg_relation_filenode(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Oid			result;
	HeapTuple	tuple;
	Form_pg_class relform;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		PG_RETURN_NULL();
	relform = (Form_pg_class) GETSTRUCT(tuple);

	if (RELKIND_HAS_STORAGE(relform->relkind))
	{
		if (relform->relfilenode)
			result = relform->relfilenode;
		else					/* Consult the relation mapper */
			result = RelationMapOidToFilenode(relid,
											  relform->relisshared);
	}
	else
	{
		/* no storage, return NULL */
		result = InvalidOid;
	}

	ReleaseSysCache(tuple);

	if (!OidIsValid(result))
		PG_RETURN_NULL();

	PG_RETURN_OID(result);
}

/*
 * Get the relation via (reltablespace, relfilenode)
 *
 * This is expected to be used when somebody wants to match an individual file
 * on the filesystem back to its table. That's not trivially possible via
 * pg_class, because that doesn't contain the relfilenodes of shared and nailed
 * tables.
 *
 * We don't fail but return NULL if we cannot find a mapping.
 *
 * Temporary relations are not detected, returning NULL (see
 * RelidByRelfilenumber() for the reasons).
 *
 * InvalidOid can be passed instead of the current database's default
 * tablespace.
 */
Datum
pg_filenode_relation(PG_FUNCTION_ARGS)
{
	Oid			reltablespace = PG_GETARG_OID(0);
	Oid			relfilenode = PG_GETARG_OID(1);
	Oid			heaprel;

	/* test needed so RelidByRelfilenode doesn't misbehave */
	if (!OidIsValid(relfilenode))
		PG_RETURN_NULL();

	heaprel = RelidByRelfilenode(reltablespace, relfilenode);

	if (!OidIsValid(heaprel))
		PG_RETURN_NULL();
	else
		PG_RETURN_OID(heaprel);
}

/*
 * Get the pathname (relative to $PGDATA) of a relation
 *
 * See comments for pg_relation_filenode.
 */
Datum
pg_relation_filepath(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_class relform;
	RelFileNode rnode;
	BackendId	backend;
	char	   *path;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		PG_RETURN_NULL();
	relform = (Form_pg_class) GETSTRUCT(tuple);

	if (RELKIND_HAS_STORAGE(relform->relkind))
	{
		/* This logic should match RelationInitPhysicalAddr */
		if (relform->reltablespace)
			rnode.spcNode = relform->reltablespace;
		else
			rnode.spcNode = MyDatabaseTableSpace;
		if (rnode.spcNode == GLOBALTABLESPACE_OID)
			rnode.dbNode = InvalidOid;
		else
			rnode.dbNode = MyDatabaseId;
		if (relform->relfilenode)
			rnode.relNode = relform->relfilenode;
		else					/* Consult the relation mapper */
			rnode.relNode = RelationMapOidToFilenode(relid,
													 relform->relisshared);
	}
	else
	{
		/* no storage, return NULL */
		rnode.relNode = InvalidOid;
		/* some compilers generate warnings without these next two lines */
		rnode.dbNode = InvalidOid;
		rnode.spcNode = InvalidOid;
	}

	if (!OidIsValid(rnode.relNode))
	{
		ReleaseSysCache(tuple);
		PG_RETURN_NULL();
	}

	/* Determine owning backend. */
	backend = InvalidBackendId;

	ReleaseSysCache(tuple);

	path = relpathbackend(rnode, backend, MAIN_FORKNUM);

	PG_RETURN_TEXT_P(cstring_to_text(path));
}
