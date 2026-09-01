/*-------------------------------------------------------------------------
 *
 * typcache.c
 *	  POSTGRES type cache code
 *
 * The type cache exists to speed lookup of certain information about data
 * types that is not directly available from a type's pg_type row.  For
 * example, we use a type's default btree opclass, or the default hash
 * opclass if no btree opclass exists, to determine which operators should
 * be used for grouping and sorting the type (GROUP BY, ORDER BY ASC/DESC).
 *
 * Several seemingly-odd choices have been made to support use of the type
 * cache by generic array and record handling routines, such as array_eq(),
 * record_cmp(), and hash_array().  Because those routines are used as index
 * support operations, they cannot leak memory.  To allow them to execute
 * efficiently, all information that they would like to re-use across calls
 * is kept in the type cache.
 *
 * Once created, a type cache entry lives as long as the backend does, so
 * there is no need for a call to release a cache entry.  If the type is
 * dropped, the cache entry simply becomes wasted storage.  This is not
 * expected to happen often, and assuming that typcache entries are good
 * permanently allows caching pointers to them in long-lived places.
 *
 * We have some provisions for updating cache entries if the stored data
 * becomes obsolete.  Core data extracted from the pg_type row is updated
 * when we detect updates to pg_type.  Information dependent on opclasses is
 * cleared if we detect updates to pg_opclass.  We also support clearing the
 * tuple descriptor and operator/function parts of a rowtype's cache entry,
 * since those may need to change as a consequence of ALTER TABLE.  Domain
 * constraint changes are also tracked properly.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/typcache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/hash.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/session.h"
#include "access/table.h"
#include "catalog/pg_am.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "lib/dshash.h"
#include "optimizer/optimizer.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/* The main type cache hashtable searched by lookup_type_cache */
static HTAB *TypeCacheHash = NULL;

/* Private flag bits in the TypeCacheEntry.flags field */
#define TCFLAGS_HAVE_PG_TYPE_DATA			0x000001
#define TCFLAGS_CHECKED_BTREE_OPCLASS		0x000002
#define TCFLAGS_CHECKED_HASH_OPCLASS		0x000004
#define TCFLAGS_CHECKED_EQ_OPR				0x000008
#define TCFLAGS_CHECKED_LT_OPR				0x000010
#define TCFLAGS_CHECKED_GT_OPR				0x000020
#define TCFLAGS_CHECKED_CMP_PROC			0x000040
#define TCFLAGS_CHECKED_HASH_PROC			0x000080
#define TCFLAGS_CHECKED_HASH_EXTENDED_PROC	0x000100
#define TCFLAGS_CHECKED_ELEM_PROPERTIES		0x000200
#define TCFLAGS_HAVE_ELEM_EQUALITY			0x000400
#define TCFLAGS_HAVE_ELEM_COMPARE			0x000800
#define TCFLAGS_HAVE_ELEM_HASHING			0x001000
#define TCFLAGS_HAVE_ELEM_EXTENDED_HASHING	0x002000
#define TCFLAGS_CHECKED_FIELD_PROPERTIES	0x004000
#define TCFLAGS_HAVE_FIELD_EQUALITY			0x008000
#define TCFLAGS_HAVE_FIELD_COMPARE			0x010000
#define TCFLAGS_HAVE_FIELD_HASHING			0x020000
#define TCFLAGS_HAVE_FIELD_EXTENDED_HASHING	0x040000

/* The flags associated with equality/comparison/hashing are all but these: */
#define TCFLAGS_OPERATOR_FLAGS \
	(~(TCFLAGS_HAVE_PG_TYPE_DATA))


/*
 * We use a separate table for storing the definitions of non-anonymous
 * record types.  Once defined, a record type will be remembered for the
 * life of the backend.  Subsequent uses of the "same" record type (where
 * sameness means equalTupleDescs) will refer to the existing table entry.
 *
 * Stored record types are remembered in a linear array of TupleDescs,
 * which can be indexed quickly with the assigned typmod.  There is also
 * a hash table to speed searches for matching TupleDescs.
 */

typedef struct RecordCacheEntry
{
	TupleDesc	tupdesc;
} RecordCacheEntry;

/*
 * To deal with non-anonymous record types that are exchanged by backends
 * involved in a parallel query, we also need a shared version of the above.
 */
struct SharedRecordTypmodRegistry
{
	/* A hash table for finding a matching TupleDesc. */
	dshash_table_handle record_table_handle;
	/* A hash table for finding a TupleDesc by typmod. */
	dshash_table_handle typmod_table_handle;
	/* A source of new record typmod numbers. */
	pg_atomic_uint32 next_typmod;
};

/*
 * When using shared tuple descriptors as hash table keys we need a way to be
 * able to search for an equal shared TupleDesc using a backend-local
 * TupleDesc.  So we use this type which can hold either, and hash and compare
 * functions that know how to handle both.
 */
typedef struct SharedRecordTableKey
{
	union
	{
		TupleDesc	local_tupdesc;
		dsa_pointer shared_tupdesc;
	}			u;
	bool		shared;
} SharedRecordTableKey;

/*
 * The shared version of RecordCacheEntry.  This lets us look up a typmod
 * using a TupleDesc which may be in local or shared memory.
 */
typedef struct SharedRecordTableEntry
{
	SharedRecordTableKey key;
} SharedRecordTableEntry;

/*
 * An entry in SharedRecordTypmodRegistry's typmod table.  This lets us look
 * up a TupleDesc in shared memory using a typmod.
 */
typedef struct SharedTypmodTableEntry
{
	uint32		typmod;
	dsa_pointer shared_tupdesc;
} SharedTypmodTableEntry;

/*
 * A comparator function for SharedRecordTableKey.
 */
static int
shared_record_table_compare(const void *a, const void *b, size_t size,
							void *arg)
{
	dsa_area   *area = (dsa_area *) arg;
	SharedRecordTableKey *k1 = (SharedRecordTableKey *) a;
	SharedRecordTableKey *k2 = (SharedRecordTableKey *) b;
	TupleDesc	t1;
	TupleDesc	t2;

	if (k1->shared)
		t1 = (TupleDesc) dsa_get_address(area, k1->u.shared_tupdesc);
	else
		t1 = k1->u.local_tupdesc;

	if (k2->shared)
		t2 = (TupleDesc) dsa_get_address(area, k2->u.shared_tupdesc);
	else
		t2 = k2->u.local_tupdesc;

	return equalTupleDescs(t1, t2) ? 0 : 1;
}

/*
 * A hash function for SharedRecordTableKey.
 */
static uint32
shared_record_table_hash(const void *a, size_t size, void *arg)
{
	dsa_area   *area = (dsa_area *) arg;
	SharedRecordTableKey *k = (SharedRecordTableKey *) a;
	TupleDesc	t;

	if (k->shared)
		t = (TupleDesc) dsa_get_address(area, k->u.shared_tupdesc);
	else
		t = k->u.local_tupdesc;

	return hashTupleDesc(t);
}

/* Parameters for SharedRecordTypmodRegistry's TupleDesc table. */
static const dshash_parameters srtr_record_table_params = {
	sizeof(SharedRecordTableKey),	/* unused */
	sizeof(SharedRecordTableEntry),
	shared_record_table_compare,
	shared_record_table_hash,
	LWTRANCHE_PER_SESSION_RECORD_TYPE
};

/* Parameters for SharedRecordTypmodRegistry's typmod hash table. */
static const dshash_parameters srtr_typmod_table_params = {
	sizeof(uint32),
	sizeof(SharedTypmodTableEntry),
	dshash_memcmp,
	dshash_memhash,
	LWTRANCHE_PER_SESSION_RECORD_TYPMOD
};

/* hashtable for recognizing registered record types */
static HTAB *RecordCacheHash = NULL;

typedef struct RecordCacheArrayEntry
{
	uint64		id;
	TupleDesc	tupdesc;
} RecordCacheArrayEntry;

/* array of info about registered record types, indexed by assigned typmod */
static RecordCacheArrayEntry *RecordCacheArray = NULL;
static int32 RecordCacheArrayLen = 0;	/* allocated length of above array */
static int32 NextRecordTypmod = 0;	/* number of entries used */

/*
 * Process-wide counter for generating unique tupledesc identifiers.
 * Zero and one (INVALID_TUPLEDESC_IDENTIFIER) aren't allowed to be chosen
 * as identifiers, so we start the counter at INVALID_TUPLEDESC_IDENTIFIER.
 */
static uint64 tupledesc_id_counter = INVALID_TUPLEDESC_IDENTIFIER;

static void load_typcache_tupdesc(TypeCacheEntry *typentry);
static bool array_element_has_equality(TypeCacheEntry *typentry);
static bool array_element_has_compare(TypeCacheEntry *typentry);
static bool array_element_has_hashing(TypeCacheEntry *typentry);
static bool array_element_has_extended_hashing(TypeCacheEntry *typentry);
static void cache_array_element_properties(TypeCacheEntry *typentry);
static bool record_fields_have_equality(TypeCacheEntry *typentry);
static bool record_fields_have_compare(TypeCacheEntry *typentry);
static bool record_fields_have_hashing(TypeCacheEntry *typentry);
static bool record_fields_have_extended_hashing(TypeCacheEntry *typentry);
static void cache_record_field_properties(TypeCacheEntry *typentry);
static void TypeCacheRelCallback(Datum arg, Oid relid);
static void TypeCacheTypCallback(Datum arg, int cacheid, uint32 hashvalue);
static void TypeCacheOpcCallback(Datum arg, int cacheid, uint32 hashvalue);
static void shared_record_typmod_registry_detach(dsm_segment *segment,
												 Datum datum);
static TupleDesc find_or_make_matching_shared_tupledesc(TupleDesc tupdesc);
static dsa_pointer share_tupledesc(dsa_area *area, TupleDesc tupdesc,
								   uint32 typmod);


/*
 * lookup_type_cache
 *
 * Fetch the type cache entry for the specified datatype, and make sure that
 * all the fields requested by bits in 'flags' are valid.
 *
 * The result is never NULL --- we will ereport() if the passed type OID is
 * invalid.  Note however that we may fail to find one or more of the
 * values requested by 'flags'; the caller needs to check whether the fields
 * are InvalidOid or not.
 */
TypeCacheEntry *
lookup_type_cache(Oid type_id, int flags)
{
	TypeCacheEntry *typentry;
	bool		found;

	if (TypeCacheHash == NULL)
	{
		/* First time through: initialize the hash table */
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(TypeCacheEntry);
		TypeCacheHash = hash_create("Type information cache", 64,
									&ctl, HASH_ELEM | HASH_BLOBS);

		/* Also set up callbacks for SI invalidations */
		CacheRegisterRelcacheCallback(TypeCacheRelCallback, (Datum) 0);
		CacheRegisterSyscacheCallback(TYPEOID, TypeCacheTypCallback, (Datum) 0);
		CacheRegisterSyscacheCallback(CLAOID, TypeCacheOpcCallback, (Datum) 0);

		/* Also make sure CacheMemoryContext exists */
		if (!CacheMemoryContext)
			CreateCacheMemoryContext();
	}

	/* Try to look up an existing entry */
	typentry = (TypeCacheEntry *) hash_search(TypeCacheHash,
											  (void *) &type_id,
											  HASH_FIND, NULL);
	if (typentry == NULL)
	{
		/*
		 * If we didn't find one, we want to make one.  But first look up the
		 * pg_type row, just to make sure we don't make a cache entry for an
		 * invalid type OID.  If the type OID is not valid, present a
		 * user-facing error, since some code paths such as domain_in() allow
		 * this function to be reached with a user-supplied OID.
		 */
		HeapTuple	tp;
		Form_pg_type typtup;

		tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_id));
		if (!HeapTupleIsValid(tp))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type with OID %u does not exist", type_id)));
		typtup = (Form_pg_type) GETSTRUCT(tp);
		if (!typtup->typisdefined)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type \"%s\" is only a shell",
							NameStr(typtup->typname))));

		/* Now make the typcache entry */
		typentry = (TypeCacheEntry *) hash_search(TypeCacheHash,
												  (void *) &type_id,
												  HASH_ENTER, &found);
		Assert(!found);			/* it wasn't there a moment ago */

		MemSet(typentry, 0, sizeof(TypeCacheEntry));

		/* These fields can never change, by definition */
		typentry->type_id = type_id;
		typentry->type_id_hash = GetSysCacheHashValue1(TYPEOID,
													   ObjectIdGetDatum(type_id));

		/* Keep this part in sync with the code below */
		typentry->typlen = typtup->typlen;
		typentry->typbyval = typtup->typbyval;
		typentry->typalign = typtup->typalign;
		typentry->typstorage = typtup->typstorage;
		typentry->typtype = typtup->typtype;
		typentry->typrelid = typtup->typrelid;
		typentry->typsubscript = typtup->typsubscript;
		typentry->typelem = typtup->typelem;
		typentry->typcollation = typtup->typcollation;
		typentry->flags |= TCFLAGS_HAVE_PG_TYPE_DATA;

		ReleaseSysCache(tp);
	}
	else if (!(typentry->flags & TCFLAGS_HAVE_PG_TYPE_DATA))
	{
		/*
		 * We have an entry, but its pg_type row got changed, so reload the
		 * data obtained directly from pg_type.
		 */
		HeapTuple	tp;
		Form_pg_type typtup;

		tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_id));
		if (!HeapTupleIsValid(tp))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type with OID %u does not exist", type_id)));
		typtup = (Form_pg_type) GETSTRUCT(tp);
		if (!typtup->typisdefined)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type \"%s\" is only a shell",
							NameStr(typtup->typname))));

		/*
		 * Keep this part in sync with the code above.  Many of these fields
		 * shouldn't ever change, particularly typtype, but copy 'em anyway.
		 */
		typentry->typlen = typtup->typlen;
		typentry->typbyval = typtup->typbyval;
		typentry->typalign = typtup->typalign;
		typentry->typstorage = typtup->typstorage;
		typentry->typtype = typtup->typtype;
		typentry->typrelid = typtup->typrelid;
		typentry->typsubscript = typtup->typsubscript;
		typentry->typelem = typtup->typelem;
		typentry->typcollation = typtup->typcollation;
		typentry->flags |= TCFLAGS_HAVE_PG_TYPE_DATA;

		ReleaseSysCache(tp);
	}

	/*
	 * Look up opclasses if we haven't already and any dependent info is
	 * requested.
	 */
	if ((flags & (TYPECACHE_EQ_OPR | TYPECACHE_LT_OPR | TYPECACHE_GT_OPR |
				  TYPECACHE_CMP_PROC |
				  TYPECACHE_EQ_OPR_FINFO | TYPECACHE_CMP_PROC_FINFO |
				  TYPECACHE_BTREE_OPFAMILY)) &&
		!(typentry->flags & TCFLAGS_CHECKED_BTREE_OPCLASS))
	{
		Oid			opclass;

		opclass = GetDefaultOpClass(type_id, BTREE_AM_OID);
		if (OidIsValid(opclass))
		{
			typentry->btree_opf = get_opclass_family(opclass);
			typentry->btree_opintype = get_opclass_input_type(opclass);
		}
		else
		{
			typentry->btree_opf = typentry->btree_opintype = InvalidOid;
		}

		/*
		 * Reset information derived from btree opclass.  Note in particular
		 * that we'll redetermine the eq_opr even if we previously found one;
		 * this matters in case a btree opclass has been added to a type that
		 * previously had only a hash opclass.
		 */
		typentry->flags &= ~(TCFLAGS_CHECKED_EQ_OPR |
							 TCFLAGS_CHECKED_LT_OPR |
							 TCFLAGS_CHECKED_GT_OPR |
							 TCFLAGS_CHECKED_CMP_PROC);
		typentry->flags |= TCFLAGS_CHECKED_BTREE_OPCLASS;
	}

	/*
	 * If we need to look up equality operator, and there's no btree opclass,
	 * force lookup of hash opclass.
	 */
	if ((flags & (TYPECACHE_EQ_OPR | TYPECACHE_EQ_OPR_FINFO)) &&
		!(typentry->flags & TCFLAGS_CHECKED_EQ_OPR) &&
		typentry->btree_opf == InvalidOid)
		flags |= TYPECACHE_HASH_OPFAMILY;

	if ((flags & (TYPECACHE_HASH_PROC | TYPECACHE_HASH_PROC_FINFO |
				  TYPECACHE_HASH_EXTENDED_PROC |
				  TYPECACHE_HASH_EXTENDED_PROC_FINFO |
				  TYPECACHE_HASH_OPFAMILY)) &&
		!(typentry->flags & TCFLAGS_CHECKED_HASH_OPCLASS))
	{
		Oid			opclass;

		opclass = GetDefaultOpClass(type_id, HASH_AM_OID);
		if (OidIsValid(opclass))
		{
			typentry->hash_opf = get_opclass_family(opclass);
			typentry->hash_opintype = get_opclass_input_type(opclass);
		}
		else
		{
			typentry->hash_opf = typentry->hash_opintype = InvalidOid;
		}

		/*
		 * Reset information derived from hash opclass.  We do *not* reset the
		 * eq_opr; if we already found one from the btree opclass, that
		 * decision is still good.
		 */
		typentry->flags &= ~(TCFLAGS_CHECKED_HASH_PROC |
							 TCFLAGS_CHECKED_HASH_EXTENDED_PROC);
		typentry->flags |= TCFLAGS_CHECKED_HASH_OPCLASS;
	}

	/*
	 * Look for requested operators and functions, if we haven't already.
	 */
	if ((flags & (TYPECACHE_EQ_OPR | TYPECACHE_EQ_OPR_FINFO)) &&
		!(typentry->flags & TCFLAGS_CHECKED_EQ_OPR))
	{
		Oid			eq_opr = InvalidOid;

		if (typentry->btree_opf != InvalidOid)
			eq_opr = get_opfamily_member(typentry->btree_opf,
										 typentry->btree_opintype,
										 typentry->btree_opintype,
										 BTEqualStrategyNumber);
		if (eq_opr == InvalidOid &&
			typentry->hash_opf != InvalidOid)
			eq_opr = get_opfamily_member(typentry->hash_opf,
										 typentry->hash_opintype,
										 typentry->hash_opintype,
										 HTEqualStrategyNumber);

		/*
		 * If the proposed equality operator is array_eq or record_eq, check
		 * to see if the element type or column types support equality.  If
		 * not, array_eq or record_eq would fail at runtime, so we don't want
		 * to report that the type has equality.
		 */
		if (eq_opr == ARRAY_EQ_OP &&
			!array_element_has_equality(typentry))
			eq_opr = InvalidOid;
		else if (eq_opr == RECORD_EQ_OP &&
				 !record_fields_have_equality(typentry))
			eq_opr = InvalidOid;

		/* Force update of eq_opr_finfo only if we're changing state */
		if (typentry->eq_opr != eq_opr)
			typentry->eq_opr_finfo.fn_oid = InvalidOid;

		typentry->eq_opr = eq_opr;

		/*
		 * Reset info about hash functions whenever we pick up new info about
		 * equality operator.  This is so we can ensure that the hash
		 * functions match the operator.
		 */
		typentry->flags &= ~(TCFLAGS_CHECKED_HASH_PROC |
							 TCFLAGS_CHECKED_HASH_EXTENDED_PROC);
		typentry->flags |= TCFLAGS_CHECKED_EQ_OPR;
	}
	if ((flags & TYPECACHE_LT_OPR) &&
		!(typentry->flags & TCFLAGS_CHECKED_LT_OPR))
	{
		Oid			lt_opr = InvalidOid;

		if (typentry->btree_opf != InvalidOid)
			lt_opr = get_opfamily_member(typentry->btree_opf,
										 typentry->btree_opintype,
										 typentry->btree_opintype,
										 BTLessStrategyNumber);

		/*
		 * As above, make sure array_cmp or record_cmp will succeed.
		 */
		if (lt_opr == ARRAY_LT_OP &&
			!array_element_has_compare(typentry))
			lt_opr = InvalidOid;
		else if (lt_opr == RECORD_LT_OP &&
				 !record_fields_have_compare(typentry))
			lt_opr = InvalidOid;

		typentry->lt_opr = lt_opr;
		typentry->flags |= TCFLAGS_CHECKED_LT_OPR;
	}
	if ((flags & TYPECACHE_GT_OPR) &&
		!(typentry->flags & TCFLAGS_CHECKED_GT_OPR))
	{
		Oid			gt_opr = InvalidOid;

		if (typentry->btree_opf != InvalidOid)
			gt_opr = get_opfamily_member(typentry->btree_opf,
										 typentry->btree_opintype,
										 typentry->btree_opintype,
										 BTGreaterStrategyNumber);

		/*
		 * As above, make sure array_cmp or record_cmp will succeed.
		 */
		if (gt_opr == ARRAY_GT_OP &&
			!array_element_has_compare(typentry))
			gt_opr = InvalidOid;
		else if (gt_opr == RECORD_GT_OP &&
				 !record_fields_have_compare(typentry))
			gt_opr = InvalidOid;

		typentry->gt_opr = gt_opr;
		typentry->flags |= TCFLAGS_CHECKED_GT_OPR;
	}
	if ((flags & (TYPECACHE_CMP_PROC | TYPECACHE_CMP_PROC_FINFO)) &&
		!(typentry->flags & TCFLAGS_CHECKED_CMP_PROC))
	{
		Oid			cmp_proc = InvalidOid;

		if (typentry->btree_opf != InvalidOid)
			cmp_proc = get_opfamily_proc(typentry->btree_opf,
										 typentry->btree_opintype,
										 typentry->btree_opintype,
										 BTORDER_PROC);

		/*
		 * As above, make sure array_cmp or record_cmp will succeed.
		 */
		if (cmp_proc == F_BTARRAYCMP &&
			!array_element_has_compare(typentry))
			cmp_proc = InvalidOid;
		else if (cmp_proc == F_BTRECORDCMP &&
				 !record_fields_have_compare(typentry))
			cmp_proc = InvalidOid;

		/* Force update of cmp_proc_finfo only if we're changing state */
		if (typentry->cmp_proc != cmp_proc)
			typentry->cmp_proc_finfo.fn_oid = InvalidOid;

		typentry->cmp_proc = cmp_proc;
		typentry->flags |= TCFLAGS_CHECKED_CMP_PROC;
	}
	if ((flags & (TYPECACHE_HASH_PROC | TYPECACHE_HASH_PROC_FINFO)) &&
		!(typentry->flags & TCFLAGS_CHECKED_HASH_PROC))
	{
		Oid			hash_proc = InvalidOid;

		/*
		 * We insist that the eq_opr, if one has been determined, match the
		 * hash opclass; else report there is no hash function.
		 */
		if (typentry->hash_opf != InvalidOid &&
			(!OidIsValid(typentry->eq_opr) ||
			 typentry->eq_opr == get_opfamily_member(typentry->hash_opf,
													 typentry->hash_opintype,
													 typentry->hash_opintype,
													 HTEqualStrategyNumber)))
			hash_proc = get_opfamily_proc(typentry->hash_opf,
										  typentry->hash_opintype,
										  typentry->hash_opintype,
										  HASHSTANDARD_PROC);

		/*
		 * As above, make sure hash_array or hash_record will succeed.
		 */
		if (hash_proc == F_HASH_ARRAY &&
			!array_element_has_hashing(typentry))
			hash_proc = InvalidOid;
		else if (hash_proc == F_HASH_RECORD &&
				 !record_fields_have_hashing(typentry))
			hash_proc = InvalidOid;

		/* Force update of hash_proc_finfo only if we're changing state */
		if (typentry->hash_proc != hash_proc)
			typentry->hash_proc_finfo.fn_oid = InvalidOid;

		typentry->hash_proc = hash_proc;
		typentry->flags |= TCFLAGS_CHECKED_HASH_PROC;
	}
	if ((flags & (TYPECACHE_HASH_EXTENDED_PROC |
				  TYPECACHE_HASH_EXTENDED_PROC_FINFO)) &&
		!(typentry->flags & TCFLAGS_CHECKED_HASH_EXTENDED_PROC))
	{
		Oid			hash_extended_proc = InvalidOid;

		/*
		 * We insist that the eq_opr, if one has been determined, match the
		 * hash opclass; else report there is no hash function.
		 */
		if (typentry->hash_opf != InvalidOid &&
			(!OidIsValid(typentry->eq_opr) ||
			 typentry->eq_opr == get_opfamily_member(typentry->hash_opf,
													 typentry->hash_opintype,
													 typentry->hash_opintype,
													 HTEqualStrategyNumber)))
			hash_extended_proc = get_opfamily_proc(typentry->hash_opf,
												   typentry->hash_opintype,
												   typentry->hash_opintype,
												   HASHEXTENDED_PROC);

		/*
		 * As above, make sure hash_array_extended or hash_record_extended
		 * will succeed.
		 */
		if (hash_extended_proc == F_HASH_ARRAY_EXTENDED &&
			!array_element_has_extended_hashing(typentry))
			hash_extended_proc = InvalidOid;
		else if (hash_extended_proc == F_HASH_RECORD_EXTENDED &&
				 !record_fields_have_extended_hashing(typentry))
			hash_extended_proc = InvalidOid;

		/* Force update of proc finfo only if we're changing state */
		if (typentry->hash_extended_proc != hash_extended_proc)
			typentry->hash_extended_proc_finfo.fn_oid = InvalidOid;

		typentry->hash_extended_proc = hash_extended_proc;
		typentry->flags |= TCFLAGS_CHECKED_HASH_EXTENDED_PROC;
	}

	/*
	 * Set up fmgr lookup info as requested
	 *
	 * Note: we tell fmgr the finfo structures live in CacheMemoryContext,
	 * which is not quite right (they're really in the hash table's private
	 * memory context) but this will do for our purposes.
	 *
	 * Note: the code above avoids invalidating the finfo structs unless the
	 * referenced operator/function OID actually changes.  This is to prevent
	 * unnecessary leakage of any subsidiary data attached to an finfo, since
	 * that would cause session-lifespan memory leaks.
	 */
	if ((flags & TYPECACHE_EQ_OPR_FINFO) &&
		typentry->eq_opr_finfo.fn_oid == InvalidOid &&
		typentry->eq_opr != InvalidOid)
	{
		Oid			eq_opr_func;

		eq_opr_func = get_opcode(typentry->eq_opr);
		if (eq_opr_func != InvalidOid)
			fmgr_info_cxt(eq_opr_func, &typentry->eq_opr_finfo,
						  CacheMemoryContext);
	}
	if ((flags & TYPECACHE_CMP_PROC_FINFO) &&
		typentry->cmp_proc_finfo.fn_oid == InvalidOid &&
		typentry->cmp_proc != InvalidOid)
	{
		fmgr_info_cxt(typentry->cmp_proc, &typentry->cmp_proc_finfo,
					  CacheMemoryContext);
	}
	if ((flags & TYPECACHE_HASH_PROC_FINFO) &&
		typentry->hash_proc_finfo.fn_oid == InvalidOid &&
		typentry->hash_proc != InvalidOid)
	{
		fmgr_info_cxt(typentry->hash_proc, &typentry->hash_proc_finfo,
					  CacheMemoryContext);
	}
	if ((flags & TYPECACHE_HASH_EXTENDED_PROC_FINFO) &&
		typentry->hash_extended_proc_finfo.fn_oid == InvalidOid &&
		typentry->hash_extended_proc != InvalidOid)
	{
		fmgr_info_cxt(typentry->hash_extended_proc,
					  &typentry->hash_extended_proc_finfo,
					  CacheMemoryContext);
	}

	/*
	 * If it's a composite type (row type), get tupdesc if requested
	 */
	if ((flags & TYPECACHE_TUPDESC) &&
		typentry->tupDesc == NULL &&
		typentry->typtype == TYPTYPE_COMPOSITE)
	{
		load_typcache_tupdesc(typentry);
	}


	return typentry;
}

/*
 * load_typcache_tupdesc --- helper routine to set up composite type's tupDesc
 */
static void
load_typcache_tupdesc(TypeCacheEntry *typentry)
{
	Relation	rel;

	if (!OidIsValid(typentry->typrelid))	/* should not happen */
		elog(ERROR, "invalid typrelid for composite type %u",
			 typentry->type_id);
	rel = relation_open(typentry->typrelid, AccessShareLock);
	Assert(rel->rd_rel->reltype == typentry->type_id);

	/*
	 * Link to the tupdesc and increment its refcount (we assert it's a
	 * refcounted descriptor).  We don't use IncrTupleDescRefCount() for this,
	 * because the reference mustn't be entered in the current resource owner;
	 * it can outlive the current query.
	 */
	typentry->tupDesc = RelationGetDescr(rel);

	Assert(typentry->tupDesc->tdrefcount > 0);
	typentry->tupDesc->tdrefcount++;

	/*
	 * In future, we could take some pains to not change tupDesc_identifier if
	 * the tupdesc didn't really change; but for now it's not worth it.
	 */
	typentry->tupDesc_identifier = ++tupledesc_id_counter;

	relation_close(rel, AccessShareLock);
}

/*
 * array_element_has_equality and friends are helper routines to check
 * whether we should believe that array_eq and related functions will work
 * on the given array type or composite type.
 *
 * The logic above may call these repeatedly on the same type entry, so we
 * make use of the typentry->flags field to cache the results once known.
 * Also, we assume that we'll probably want all these facts about the type
 * if we want any, so we cache them all using only one lookup of the
 * component datatype(s).
 */

static bool
array_element_has_equality(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_ELEM_PROPERTIES))
		cache_array_element_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_ELEM_EQUALITY) != 0;
}

static bool
array_element_has_compare(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_ELEM_PROPERTIES))
		cache_array_element_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_ELEM_COMPARE) != 0;
}

static bool
array_element_has_hashing(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_ELEM_PROPERTIES))
		cache_array_element_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_ELEM_HASHING) != 0;
}

static bool
array_element_has_extended_hashing(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_ELEM_PROPERTIES))
		cache_array_element_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_ELEM_EXTENDED_HASHING) != 0;
}

static void
cache_array_element_properties(TypeCacheEntry *typentry)
{
	Oid			elem_type = get_base_element_type(typentry->type_id);

	if (OidIsValid(elem_type))
	{
		TypeCacheEntry *elementry;

		elementry = lookup_type_cache(elem_type,
									  TYPECACHE_EQ_OPR |
									  TYPECACHE_CMP_PROC |
									  TYPECACHE_HASH_PROC |
									  TYPECACHE_HASH_EXTENDED_PROC);
		if (OidIsValid(elementry->eq_opr))
			typentry->flags |= TCFLAGS_HAVE_ELEM_EQUALITY;
		if (OidIsValid(elementry->cmp_proc))
			typentry->flags |= TCFLAGS_HAVE_ELEM_COMPARE;
		if (OidIsValid(elementry->hash_proc))
			typentry->flags |= TCFLAGS_HAVE_ELEM_HASHING;
		if (OidIsValid(elementry->hash_extended_proc))
			typentry->flags |= TCFLAGS_HAVE_ELEM_EXTENDED_HASHING;
	}
	typentry->flags |= TCFLAGS_CHECKED_ELEM_PROPERTIES;
}

/*
 * Likewise, some helper functions for composite types.
 */

static bool
record_fields_have_equality(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_FIELD_PROPERTIES))
		cache_record_field_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_FIELD_EQUALITY) != 0;
}

static bool
record_fields_have_compare(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_FIELD_PROPERTIES))
		cache_record_field_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_FIELD_COMPARE) != 0;
}

static bool
record_fields_have_hashing(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_FIELD_PROPERTIES))
		cache_record_field_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_FIELD_HASHING) != 0;
}

static bool
record_fields_have_extended_hashing(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_FIELD_PROPERTIES))
		cache_record_field_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_FIELD_EXTENDED_HASHING) != 0;
}

static void
cache_record_field_properties(TypeCacheEntry *typentry)
{
	/*
	 * For type RECORD, we can't really tell what will work, since we don't
	 * have access here to the specific anonymous type.  Just assume that
	 * equality and comparison will (we may get a failure at runtime).  We
	 * could also claim that hashing works, but then if code that has the
	 * option between a comparison-based (sort-based) and a hash-based plan
	 * chooses hashing, stuff could fail that would otherwise work if it chose
	 * a comparison-based plan.  In practice more types support comparison
	 * than hashing.
	 */
	if (typentry->type_id == RECORDOID)
	{
		typentry->flags |= (TCFLAGS_HAVE_FIELD_EQUALITY |
							TCFLAGS_HAVE_FIELD_COMPARE);
	}
	else if (typentry->typtype == TYPTYPE_COMPOSITE)
	{
		TupleDesc	tupdesc;
		int			newflags;
		int			i;

		/* Fetch composite type's tupdesc if we don't have it already */
		if (typentry->tupDesc == NULL)
			load_typcache_tupdesc(typentry);
		tupdesc = typentry->tupDesc;

		/* Must bump the refcount while we do additional catalog lookups */
		IncrTupleDescRefCount(tupdesc);

		/* Have each property if all non-dropped fields have the property */
		newflags = (TCFLAGS_HAVE_FIELD_EQUALITY |
					TCFLAGS_HAVE_FIELD_COMPARE |
					TCFLAGS_HAVE_FIELD_HASHING |
					TCFLAGS_HAVE_FIELD_EXTENDED_HASHING);
		for (i = 0; i < tupdesc->natts; i++)
		{
			TypeCacheEntry *fieldentry;
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

			if (attr->attisdropped)
				continue;

			fieldentry = lookup_type_cache(attr->atttypid,
										   TYPECACHE_EQ_OPR |
										   TYPECACHE_CMP_PROC |
										   TYPECACHE_HASH_PROC |
										   TYPECACHE_HASH_EXTENDED_PROC);
			if (!OidIsValid(fieldentry->eq_opr))
				newflags &= ~TCFLAGS_HAVE_FIELD_EQUALITY;
			if (!OidIsValid(fieldentry->cmp_proc))
				newflags &= ~TCFLAGS_HAVE_FIELD_COMPARE;
			if (!OidIsValid(fieldentry->hash_proc))
				newflags &= ~TCFLAGS_HAVE_FIELD_HASHING;
			if (!OidIsValid(fieldentry->hash_extended_proc))
				newflags &= ~TCFLAGS_HAVE_FIELD_EXTENDED_HASHING;

			/* We can drop out of the loop once we disprove all bits */
			if (newflags == 0)
				break;
		}
		typentry->flags |= newflags;

		DecrTupleDescRefCount(tupdesc);
	}
	typentry->flags |= TCFLAGS_CHECKED_FIELD_PROPERTIES;
}


/*
 * Make sure that RecordCacheArray and RecordIdentifierArray are large enough
 * to store 'typmod'.
 */
static void
ensure_record_cache_typmod_slot_exists(int32 typmod)
{
	if (RecordCacheArray == NULL)
	{
		RecordCacheArray = (RecordCacheArrayEntry *)
			MemoryContextAllocZero(CacheMemoryContext, 64 * sizeof(RecordCacheArrayEntry));
		RecordCacheArrayLen = 64;
	}

	if (typmod >= RecordCacheArrayLen)
	{
		int32		newlen = RecordCacheArrayLen * 2;

		while (typmod >= newlen)
			newlen *= 2;

		RecordCacheArray = (RecordCacheArrayEntry *)
			repalloc(RecordCacheArray,
					 newlen * sizeof(RecordCacheArrayEntry));
		memset(RecordCacheArray + RecordCacheArrayLen, 0,
			   (newlen - RecordCacheArrayLen) * sizeof(RecordCacheArrayEntry));
		RecordCacheArrayLen = newlen;
	}
}

/*
 * lookup_rowtype_tupdesc_internal --- internal routine to lookup a rowtype
 *
 * Same API as lookup_rowtype_tupdesc_noerror, but the returned tupdesc
 * hasn't had its refcount bumped.
 */
static TupleDesc
lookup_rowtype_tupdesc_internal(Oid type_id, int32 typmod, bool noError)
{
	if (type_id != RECORDOID)
	{
		/*
		 * It's a named composite type, so use the regular typcache.
		 */
		TypeCacheEntry *typentry;

		typentry = lookup_type_cache(type_id, TYPECACHE_TUPDESC);
		if (typentry->tupDesc == NULL && !noError)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("type %s is not composite",
							format_type_be(type_id))));
		return typentry->tupDesc;
	}
	else
	{
		/*
		 * It's a transient record type, so look in our record-type table.
		 */
		if (typmod >= 0)
		{
			/* It is already in our local cache? */
			if (typmod < RecordCacheArrayLen &&
				RecordCacheArray[typmod].tupdesc != NULL)
				return RecordCacheArray[typmod].tupdesc;

			/* Are we attached to a shared record typmod registry? */
			if (CurrentSession->shared_typmod_registry != NULL)
			{
				SharedTypmodTableEntry *entry;

				/* Try to find it in the shared typmod index. */
				entry = dshash_find(CurrentSession->shared_typmod_table,
									&typmod, false);
				if (entry != NULL)
				{
					TupleDesc	tupdesc;

					tupdesc = (TupleDesc)
						dsa_get_address(CurrentSession->area,
										entry->shared_tupdesc);
					Assert(typmod == tupdesc->tdtypmod);

					/* We may need to extend the local RecordCacheArray. */
					ensure_record_cache_typmod_slot_exists(typmod);

					/*
					 * Our local array can now point directly to the TupleDesc
					 * in shared memory, which is non-reference-counted.
					 */
					RecordCacheArray[typmod].tupdesc = tupdesc;
					Assert(tupdesc->tdrefcount == -1);

					/*
					 * We don't share tupdesc identifiers across processes, so
					 * assign one locally.
					 */
					RecordCacheArray[typmod].id = ++tupledesc_id_counter;

					dshash_release_lock(CurrentSession->shared_typmod_table,
										entry);

					return RecordCacheArray[typmod].tupdesc;
				}
			}
		}

		if (!noError)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("record type has not been registered")));
		return NULL;
	}
}

/*
 * lookup_rowtype_tupdesc
 *
 * Given a typeid/typmod that should describe a known composite type,
 * return the tuple descriptor for the type.  Will ereport on failure.
 * (Use ereport because this is reachable with user-specified OIDs,
 * for example from record_in().)
 *
 * Note: on success, we increment the refcount of the returned TupleDesc,
 * and log the reference in CurrentResourceOwner.  Caller should call
 * ReleaseTupleDesc or DecrTupleDescRefCount when done using the tupdesc.
 */
TupleDesc
lookup_rowtype_tupdesc(Oid type_id, int32 typmod)
{
	TupleDesc	tupDesc;

	tupDesc = lookup_rowtype_tupdesc_internal(type_id, typmod, false);
	PinTupleDesc(tupDesc);
	return tupDesc;
}

/*
 * lookup_rowtype_tupdesc_noerror
 *
 * As above, but if the type is not a known composite type and noError
 * is true, returns NULL instead of ereport'ing.  (Note that if a bogus
 * type_id is passed, you'll get an ereport anyway.)
 */
TupleDesc
lookup_rowtype_tupdesc_noerror(Oid type_id, int32 typmod, bool noError)
{
	TupleDesc	tupDesc;

	tupDesc = lookup_rowtype_tupdesc_internal(type_id, typmod, noError);
	if (tupDesc != NULL)
		PinTupleDesc(tupDesc);
	return tupDesc;
}

/*
 * lookup_rowtype_tupdesc_copy
 *
 * Like lookup_rowtype_tupdesc(), but the returned TupleDesc has been
 * copied into the CurrentMemoryContext and is not reference-counted.
 */
TupleDesc
lookup_rowtype_tupdesc_copy(Oid type_id, int32 typmod)
{
	TupleDesc	tmp;

	tmp = lookup_rowtype_tupdesc_internal(type_id, typmod, false);
	return CreateTupleDescCopyConstr(tmp);
}

/*
 * Hash function for the hash table of RecordCacheEntry.
 */
static uint32
record_type_typmod_hash(const void *data, size_t size)
{
	RecordCacheEntry *entry = (RecordCacheEntry *) data;

	return hashTupleDesc(entry->tupdesc);
}

/*
 * Match function for the hash table of RecordCacheEntry.
 */
static int
record_type_typmod_compare(const void *a, const void *b, size_t size)
{
	RecordCacheEntry *left = (RecordCacheEntry *) a;
	RecordCacheEntry *right = (RecordCacheEntry *) b;

	return equalTupleDescs(left->tupdesc, right->tupdesc) ? 0 : 1;
}

/*
 * assign_record_type_typmod
 *
 * Given a tuple descriptor for a RECORD type, find or create a cache entry
 * for the type, and set the tupdesc's tdtypmod field to a value that will
 * identify this cache entry to lookup_rowtype_tupdesc.
 */
void
assign_record_type_typmod(TupleDesc tupDesc)
{
	RecordCacheEntry *recentry;
	TupleDesc	entDesc;
	bool		found;
	MemoryContext oldcxt;

	Assert(tupDesc->tdtypeid == RECORDOID);

	if (RecordCacheHash == NULL)
	{
		/* First time through: initialize the hash table */
		HASHCTL		ctl;

		ctl.keysize = sizeof(TupleDesc);	/* just the pointer */
		ctl.entrysize = sizeof(RecordCacheEntry);
		ctl.hash = record_type_typmod_hash;
		ctl.match = record_type_typmod_compare;
		RecordCacheHash = hash_create("Record information cache", 64,
									  &ctl,
									  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

		/* Also make sure CacheMemoryContext exists */
		if (!CacheMemoryContext)
			CreateCacheMemoryContext();
	}

	/*
	 * Find a hashtable entry for this tuple descriptor. We don't use
	 * HASH_ENTER yet, because if it's missing, we need to make sure that all
	 * the allocations succeed before we create the new entry.
	 */
	recentry = (RecordCacheEntry *) hash_search(RecordCacheHash,
												(void *) &tupDesc,
												HASH_FIND, &found);
	if (found && recentry->tupdesc != NULL)
	{
		tupDesc->tdtypmod = recentry->tupdesc->tdtypmod;
		return;
	}

	/* Not present, so need to manufacture an entry */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/* Look in the SharedRecordTypmodRegistry, if attached */
	entDesc = find_or_make_matching_shared_tupledesc(tupDesc);
	if (entDesc == NULL)
	{
		/*
		 * Make sure we have room before we CreateTupleDescCopy() or advance
		 * NextRecordTypmod.
		 */
		ensure_record_cache_typmod_slot_exists(NextRecordTypmod);

		/* Reference-counted local cache only. */
		entDesc = CreateTupleDescCopy(tupDesc);
		entDesc->tdrefcount = 1;
		entDesc->tdtypmod = NextRecordTypmod++;
	}
	else
	{
		ensure_record_cache_typmod_slot_exists(entDesc->tdtypmod);
	}

	RecordCacheArray[entDesc->tdtypmod].tupdesc = entDesc;

	/* Assign a unique tupdesc identifier, too. */
	RecordCacheArray[entDesc->tdtypmod].id = ++tupledesc_id_counter;

	/* Fully initialized; create the hash table entry */
	recentry = (RecordCacheEntry *) hash_search(RecordCacheHash,
												(void *) &tupDesc,
												HASH_ENTER, NULL);
	recentry->tupdesc = entDesc;

	/* Update the caller's tuple descriptor. */
	tupDesc->tdtypmod = entDesc->tdtypmod;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * assign_record_type_identifier
 *
 * Get an identifier, which will be unique over the lifespan of this backend
 * process, for the current tuple descriptor of the specified composite type.
 * For named composite types, the value is guaranteed to change if the type's
 * definition does.  For registered RECORD types, the value will not change
 * once assigned, since the registered type won't either.  If an anonymous
 * RECORD type is specified, we return a new identifier on each call.
 */
uint64
assign_record_type_identifier(Oid type_id, int32 typmod)
{
	if (type_id != RECORDOID)
	{
		/*
		 * It's a named composite type, so use the regular typcache.
		 */
		TypeCacheEntry *typentry;

		typentry = lookup_type_cache(type_id, TYPECACHE_TUPDESC);
		if (typentry->tupDesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("type %s is not composite",
							format_type_be(type_id))));
		Assert(typentry->tupDesc_identifier != 0);
		return typentry->tupDesc_identifier;
	}
	else
	{
		/*
		 * It's a transient record type, so look in our record-type table.
		 */
		if (typmod >= 0 && typmod < RecordCacheArrayLen &&
			RecordCacheArray[typmod].tupdesc != NULL)
		{
			Assert(RecordCacheArray[typmod].id != 0);
			return RecordCacheArray[typmod].id;
		}

		/* For anonymous or unrecognized record type, generate a new ID */
		return ++tupledesc_id_counter;
	}
}

/*
 * Return the amount of shmem required to hold a SharedRecordTypmodRegistry.
 * This exists only to avoid exposing private innards of
 * SharedRecordTypmodRegistry in a header.
 */
size_t
SharedRecordTypmodRegistryEstimate(void)
{
	return sizeof(SharedRecordTypmodRegistry);
}

/*
 * Initialize 'registry' in a pre-existing shared memory region, which must be
 * maximally aligned and have space for SharedRecordTypmodRegistryEstimate()
 * bytes.
 *
 * 'area' will be used to allocate shared memory space as required for the
 * typemod registration.  The current process, expected to be a leader process
 * in a parallel query, will be attached automatically and its current record
 * types will be loaded into *registry.  While attached, all calls to
 * assign_record_type_typmod will use the shared registry.  Worker backends
 * will need to attach explicitly.
 *
 * Note that this function takes 'area' and 'segment' as arguments rather than
 * accessing them via CurrentSession, because they aren't installed there
 * until after this function runs.
 */
void
SharedRecordTypmodRegistryInit(SharedRecordTypmodRegistry *registry,
							   dsm_segment *segment,
							   dsa_area *area)
{
	MemoryContext old_context;
	dshash_table *record_table;
	dshash_table *typmod_table;
	int32		typmod;

	Assert(!IsParallelWorker());

	/* We can't already be attached to a shared registry. */
	Assert(CurrentSession->shared_typmod_registry == NULL);
	Assert(CurrentSession->shared_record_table == NULL);
	Assert(CurrentSession->shared_typmod_table == NULL);

	old_context = MemoryContextSwitchTo(TopMemoryContext);

	/* Create the hash table of tuple descriptors indexed by themselves. */
	record_table = dshash_create(area, &srtr_record_table_params, area);

	/* Create the hash table of tuple descriptors indexed by typmod. */
	typmod_table = dshash_create(area, &srtr_typmod_table_params, NULL);

	MemoryContextSwitchTo(old_context);

	/* Initialize the SharedRecordTypmodRegistry. */
	registry->record_table_handle = dshash_get_hash_table_handle(record_table);
	registry->typmod_table_handle = dshash_get_hash_table_handle(typmod_table);
	pg_atomic_init_u32(&registry->next_typmod, NextRecordTypmod);

	/*
	 * Copy all entries from this backend's private registry into the shared
	 * registry.
	 */
	for (typmod = 0; typmod < NextRecordTypmod; ++typmod)
	{
		SharedTypmodTableEntry *typmod_table_entry;
		SharedRecordTableEntry *record_table_entry;
		SharedRecordTableKey record_table_key;
		dsa_pointer shared_dp;
		TupleDesc	tupdesc;
		bool		found;

		tupdesc = RecordCacheArray[typmod].tupdesc;
		if (tupdesc == NULL)
			continue;

		/* Copy the TupleDesc into shared memory. */
		shared_dp = share_tupledesc(area, tupdesc, typmod);

		/* Insert into the typmod table. */
		typmod_table_entry = dshash_find_or_insert(typmod_table,
												   &tupdesc->tdtypmod,
												   &found);
		if (found)
			elog(ERROR, "cannot create duplicate shared record typmod");
		typmod_table_entry->typmod = tupdesc->tdtypmod;
		typmod_table_entry->shared_tupdesc = shared_dp;
		dshash_release_lock(typmod_table, typmod_table_entry);

		/* Insert into the record table. */
		record_table_key.shared = false;
		record_table_key.u.local_tupdesc = tupdesc;
		record_table_entry = dshash_find_or_insert(record_table,
												   &record_table_key,
												   &found);
		if (!found)
		{
			record_table_entry->key.shared = true;
			record_table_entry->key.u.shared_tupdesc = shared_dp;
		}
		dshash_release_lock(record_table, record_table_entry);
	}

	/*
	 * Set up the global state that will tell assign_record_type_typmod and
	 * lookup_rowtype_tupdesc_internal about the shared registry.
	 */
	CurrentSession->shared_record_table = record_table;
	CurrentSession->shared_typmod_table = typmod_table;
	CurrentSession->shared_typmod_registry = registry;

	/*
	 * We install a detach hook in the leader, but only to handle cleanup on
	 * failure during GetSessionDsmHandle().  Once GetSessionDsmHandle() pins
	 * the memory, the leader process will use a shared registry until it
	 * exits.
	 */
	on_dsm_detach(segment, shared_record_typmod_registry_detach, (Datum) 0);
}

/*
 * Attach to 'registry', which must have been initialized already by another
 * backend.  Future calls to assign_record_type_typmod and
 * lookup_rowtype_tupdesc_internal will use the shared registry until the
 * current session is detached.
 */
void
SharedRecordTypmodRegistryAttach(SharedRecordTypmodRegistry *registry)
{
	MemoryContext old_context;
	dshash_table *record_table;
	dshash_table *typmod_table;

	Assert(IsParallelWorker());

	/* We can't already be attached to a shared registry. */
	Assert(CurrentSession != NULL);
	Assert(CurrentSession->segment != NULL);
	Assert(CurrentSession->area != NULL);
	Assert(CurrentSession->shared_typmod_registry == NULL);
	Assert(CurrentSession->shared_record_table == NULL);
	Assert(CurrentSession->shared_typmod_table == NULL);

	/*
	 * We can't already have typmods in our local cache, because they'd clash
	 * with those imported by SharedRecordTypmodRegistryInit.  This should be
	 * a freshly started parallel worker.  If we ever support worker
	 * recycling, a worker would need to zap its local cache in between
	 * servicing different queries, in order to be able to call this and
	 * synchronize typmods with a new leader; but that's problematic because
	 * we can't be very sure that record-typmod-related state hasn't escaped
	 * to anywhere else in the process.
	 */
	Assert(NextRecordTypmod == 0);

	old_context = MemoryContextSwitchTo(TopMemoryContext);

	/* Attach to the two hash tables. */
	record_table = dshash_attach(CurrentSession->area,
								 &srtr_record_table_params,
								 registry->record_table_handle,
								 CurrentSession->area);
	typmod_table = dshash_attach(CurrentSession->area,
								 &srtr_typmod_table_params,
								 registry->typmod_table_handle,
								 NULL);

	MemoryContextSwitchTo(old_context);

	/*
	 * Set up detach hook to run at worker exit.  Currently this is the same
	 * as the leader's detach hook, but in future they might need to be
	 * different.
	 */
	on_dsm_detach(CurrentSession->segment,
				  shared_record_typmod_registry_detach,
				  PointerGetDatum(registry));

	/*
	 * Set up the session state that will tell assign_record_type_typmod and
	 * lookup_rowtype_tupdesc_internal about the shared registry.
	 */
	CurrentSession->shared_typmod_registry = registry;
	CurrentSession->shared_record_table = record_table;
	CurrentSession->shared_typmod_table = typmod_table;
}

/*
 * TypeCacheRelCallback
 *		Relcache inval callback function
 *
 * Delete the cached tuple descriptor (if any) for the given rel's composite
 * type, or for all composite types if relid == InvalidOid.  Also reset
 * whatever info we have cached about the composite type's comparability.
 *
 * This is called when a relcache invalidation event occurs for the given
 * relid.  We must scan the whole typcache hash since we don't know the
 * type OID corresponding to the relid.  We could do a direct search if this
 * were a syscache-flush callback on pg_type, but then we would need all
 * ALTER-TABLE-like commands that could modify a rowtype to issue syscache
 * invals against the rel's pg_type OID.  The extra SI signaling could very
 * well cost more than we'd save, since in most usages there are not very
 * many entries in a backend's typcache.  The risk of bugs-of-omission seems
 * high, too.
 *
 * Another possibility, with only localized impact, is to maintain a second
 * hashtable that indexes composite-type typcache entries by their typrelid.
 * But it's still not clear it's worth the trouble.
 */
static void
TypeCacheRelCallback(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS status;
	TypeCacheEntry *typentry;

	/* TypeCacheHash must exist, else this callback wouldn't be registered */
	hash_seq_init(&status, TypeCacheHash);
	while ((typentry = (TypeCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		if (typentry->typtype == TYPTYPE_COMPOSITE)
		{
			/* Skip if no match, unless we're zapping all composite types */
			if (relid != typentry->typrelid && relid != InvalidOid)
				continue;

			/* Delete tupdesc if we have it */
			if (typentry->tupDesc != NULL)
			{
				/*
				 * Release our refcount, and free the tupdesc if none remain.
				 * (Can't use DecrTupleDescRefCount because this reference is
				 * not logged in current resource owner.)
				 */
				Assert(typentry->tupDesc->tdrefcount > 0);
				if (--typentry->tupDesc->tdrefcount == 0)
					FreeTupleDesc(typentry->tupDesc);
				typentry->tupDesc = NULL;

				/*
				 * Also clear tupDesc_identifier, so that anything watching
				 * that will realize that the tupdesc has possibly changed.
				 * (Alternatively, we could specify that to detect possible
				 * tupdesc change, one must check for tupDesc != NULL as well
				 * as tupDesc_identifier being the same as what was previously
				 * seen.  That seems error-prone.)
				 */
				typentry->tupDesc_identifier = 0;
			}

			/* Reset equality/comparison/hashing validity information */
			typentry->flags &= ~TCFLAGS_OPERATOR_FLAGS;
		}
	}
}

/*
 * TypeCacheTypCallback
 *		Syscache inval callback function
 *
 * This is called when a syscache invalidation event occurs for any
 * pg_type row.  If we have information cached about that type, mark
 * it as needing to be reloaded.
 */
static void
TypeCacheTypCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	TypeCacheEntry *typentry;

	/* TypeCacheHash must exist, else this callback wouldn't be registered */
	hash_seq_init(&status, TypeCacheHash);
	while ((typentry = (TypeCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		/* Is this the targeted type row (or it's a total cache flush)? */
		if (hashvalue == 0 || typentry->type_id_hash == hashvalue)
		{
			/* Mark the data obtained directly from pg_type as invalid. */
			typentry->flags &= ~TCFLAGS_HAVE_PG_TYPE_DATA;
		}
	}
}

/*
 * TypeCacheOpcCallback
 *		Syscache inval callback function
 *
 * This is called when a syscache invalidation event occurs for any pg_opclass
 * row.  In principle we could probably just invalidate data dependent on the
 * particular opclass, but since updates on pg_opclass are rare in production
 * it doesn't seem worth a lot of complication: we just mark all cached data
 * invalid.
 *
 * Note that we don't bother watching for updates on pg_amop or pg_amproc.
 * This should be safe because ALTER OPERATOR FAMILY ADD/DROP OPERATOR/FUNCTION
 * is not allowed to be used to add/drop the primary operators and functions
 * of an opclass, only cross-type members of a family; and the latter sorts
 * of members are not going to get cached here.
 */
static void
TypeCacheOpcCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	TypeCacheEntry *typentry;

	/* TypeCacheHash must exist, else this callback wouldn't be registered */
	hash_seq_init(&status, TypeCacheHash);
	while ((typentry = (TypeCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		/* Reset equality/comparison/hashing validity information */
		typentry->flags &= ~TCFLAGS_OPERATOR_FLAGS;
	}
}

/*
 * Copy 'tupdesc' into newly allocated shared memory in 'area', set its typmod
 * to the given value and return a dsa_pointer.
 */
static dsa_pointer
share_tupledesc(dsa_area *area, TupleDesc tupdesc, uint32 typmod)
{
	dsa_pointer shared_dp;
	TupleDesc	shared;

	shared_dp = dsa_allocate(area, TupleDescSize(tupdesc));
	shared = (TupleDesc) dsa_get_address(area, shared_dp);
	TupleDescCopy(shared, tupdesc);
	shared->tdtypmod = typmod;

	return shared_dp;
}

/*
 * If we are attached to a SharedRecordTypmodRegistry, use it to find or
 * create a shared TupleDesc that matches 'tupdesc'.  Otherwise return NULL.
 * Tuple descriptors returned by this function are not reference counted, and
 * will exist at least as long as the current backend remained attached to the
 * current session.
 */
static TupleDesc
find_or_make_matching_shared_tupledesc(TupleDesc tupdesc)
{
	TupleDesc	result;
	SharedRecordTableKey key;
	SharedRecordTableEntry *record_table_entry;
	SharedTypmodTableEntry *typmod_table_entry;
	dsa_pointer shared_dp;
	bool		found;
	uint32		typmod;

	/* If not even attached, nothing to do. */
	if (CurrentSession->shared_typmod_registry == NULL)
		return NULL;

	/* Try to find a matching tuple descriptor in the record table. */
	key.shared = false;
	key.u.local_tupdesc = tupdesc;
	record_table_entry = (SharedRecordTableEntry *)
		dshash_find(CurrentSession->shared_record_table, &key, false);
	if (record_table_entry)
	{
		Assert(record_table_entry->key.shared);
		dshash_release_lock(CurrentSession->shared_record_table,
							record_table_entry);
		result = (TupleDesc)
			dsa_get_address(CurrentSession->area,
							record_table_entry->key.u.shared_tupdesc);
		Assert(result->tdrefcount == -1);

		return result;
	}

	/* Allocate a new typmod number.  This will be wasted if we error out. */
	typmod = (int)
		pg_atomic_fetch_add_u32(&CurrentSession->shared_typmod_registry->next_typmod,
								1);

	/* Copy the TupleDesc into shared memory. */
	shared_dp = share_tupledesc(CurrentSession->area, tupdesc, typmod);

	/*
	 * Create an entry in the typmod table so that others will understand this
	 * typmod number.
	 */
	PG_TRY();
	{
		typmod_table_entry = (SharedTypmodTableEntry *)
			dshash_find_or_insert(CurrentSession->shared_typmod_table,
								  &typmod, &found);
		if (found)
			elog(ERROR, "cannot create duplicate shared record typmod");
	}
	PG_CATCH();
	{
		dsa_free(CurrentSession->area, shared_dp);
		PG_RE_THROW();
	}
	PG_END_TRY();
	typmod_table_entry->typmod = typmod;
	typmod_table_entry->shared_tupdesc = shared_dp;
	dshash_release_lock(CurrentSession->shared_typmod_table,
						typmod_table_entry);

	/*
	 * Finally create an entry in the record table so others with matching
	 * tuple descriptors can reuse the typmod.
	 */
	record_table_entry = (SharedRecordTableEntry *)
		dshash_find_or_insert(CurrentSession->shared_record_table, &key,
							  &found);
	if (found)
	{
		/*
		 * Someone concurrently inserted a matching tuple descriptor since the
		 * first time we checked.  Use that one instead.
		 */
		dshash_release_lock(CurrentSession->shared_record_table,
							record_table_entry);

		/* Might as well free up the space used by the one we created. */
		found = dshash_delete_key(CurrentSession->shared_typmod_table,
								  &typmod);
		Assert(found);
		dsa_free(CurrentSession->area, shared_dp);

		/* Return the one we found. */
		Assert(record_table_entry->key.shared);
		result = (TupleDesc)
			dsa_get_address(CurrentSession->area,
							record_table_entry->key.u.shared_tupdesc);
		Assert(result->tdrefcount == -1);

		return result;
	}

	/* Store it and return it. */
	record_table_entry->key.shared = true;
	record_table_entry->key.u.shared_tupdesc = shared_dp;
	dshash_release_lock(CurrentSession->shared_record_table,
						record_table_entry);
	result = (TupleDesc)
		dsa_get_address(CurrentSession->area, shared_dp);
	Assert(result->tdrefcount == -1);

	return result;
}

/*
 * On-DSM-detach hook to forget about the current shared record typmod
 * infrastructure.  This is currently used by both leader and workers.
 */
static void
shared_record_typmod_registry_detach(dsm_segment *segment, Datum datum)
{
	/* Be cautious here: maybe we didn't finish initializing. */
	if (CurrentSession->shared_record_table != NULL)
	{
		dshash_detach(CurrentSession->shared_record_table);
		CurrentSession->shared_record_table = NULL;
	}
	if (CurrentSession->shared_typmod_table != NULL)
	{
		dshash_detach(CurrentSession->shared_typmod_table);
		CurrentSession->shared_typmod_table = NULL;
	}
	CurrentSession->shared_typmod_registry = NULL;
}
