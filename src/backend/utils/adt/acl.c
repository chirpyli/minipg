/*-------------------------------------------------------------------------
 *
 * acl.c
 *	  Basic access control list data structures manipulation routines.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/acl.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/proclang.h"
#include "commands/tablespace.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "lib/qunique.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

typedef struct
{
	const char *name;
	AclMode		value;
} priv_map;

/*
 * We frequently need to test whether a given role is a member of some other
 * role.  In most of these tests the "given role" is the same, namely the
 * active current user.  So we can optimize it by keeping cached lists of all
 * the roles the "given role" is a member of, directly or indirectly.
 *
 * Possibly this mechanism should be generalized to allow caching membership
 * info for multiple roles?
 *
 * Each element of cached_roles is an OID list of constituent roles for the
 * corresponding element of cached_role (always including the cached_role
 * itself).  One cache has ROLERECURSE_PRIVS semantics, and the other has
 * ROLERECURSE_MEMBERS semantics.
 */
enum RoleRecurseType
{
	ROLERECURSE_PRIVS = 0,		/* recurse if rolinherit */
	ROLERECURSE_MEMBERS = 1		/* recurse unconditionally */
};
static Oid	cached_role[] = {InvalidOid, InvalidOid};
static List *cached_roles[] = {NIL, NIL};
static uint32 cached_db_hash;


static const char *getid(const char *s, char *n);
static void putid(char *p, const char *s);
static Acl *allocacl(int n);
static void check_acl(const Acl *acl);
static const char *aclparse(const char *s, AclItem *aip);
static bool aclitem_match(const AclItem *a1, const AclItem *a2);
static int	aclitemComparator(const void *arg1, const void *arg2);
static void check_circularity(const Acl *old_acl, const AclItem *mod_aip,
							  Oid ownerId);
static Acl *recursive_revoke(Acl *acl, Oid grantee, AclMode revoke_privs,
							 Oid ownerId, DropBehavior behavior);

static AclMode convert_priv_string(text *priv_type_text);

static void RoleMembershipCacheCallback(Datum arg, int cacheid, uint32 hashvalue);


/*
 * Test whether an identifier char can be left unquoted in ACLs.
 *
 * Formerly, we used isalnum() even on non-ASCII characters, resulting in
 * unportable behavior.  To ensure dump compatibility with old versions,
 * we now treat high-bit-set characters as always requiring quoting during
 * putid(), but getid() will always accept them without quotes.
 */
static inline bool
is_safe_acl_char(unsigned char c, bool is_getid)
{
	if (IS_HIGHBIT_SET(c))
		return is_getid;
	return isalnum(c) || c == '_';
}

/*
 * getid
 *		Consumes the first alphanumeric string (identifier) found in string
 *		's', ignoring any leading white space.  If it finds a double quote
 *		it returns the word inside the quotes.
 *
 * RETURNS:
 *		the string position in 's' that points to the next non-space character
 *		in 's', after any quotes.  Also:
 *		- loads the identifier into 'n'.  (If no identifier is found, 'n'
 *		  contains an empty string.)  'n' must be NAMEDATALEN bytes.
 */
static const char *
getid(const char *s, char *n)
{
	int			len = 0;
	bool		in_quotes = false;

	Assert(s && n);

	while (isspace((unsigned char) *s))
		s++;
	for (;
		 *s != '\0' &&
		 (in_quotes || *s == '"' || is_safe_acl_char(*s, true));
		 s++)
	{
		if (*s == '"')
		{
			if (!in_quotes)
			{
				in_quotes = true;
				continue;
			}
			/* safe to look at next char (could be '\0' though) */
			if (*(s + 1) != '"')
			{
				in_quotes = false;
				continue;
			}
			/* it's an escaped double quote; skip the escaping char */
			s++;
		}

		/* Add the character to the string */
		if (len >= NAMEDATALEN - 1)
			ereport(ERROR,
					(errcode(ERRCODE_NAME_TOO_LONG),
					 errmsg("identifier too long"),
					 errdetail("Identifier must be less than %d characters.",
							   NAMEDATALEN)));

		n[len++] = *s;
	}
	n[len] = '\0';
	while (isspace((unsigned char) *s))
		s++;
	return s;
}

/*
 * Write a role name at *p, adding double quotes if needed.
 * There must be at least (2*NAMEDATALEN)+2 bytes available at *p.
 * This needs to be kept in sync with copyAclUserName in pg_dump/dumputils.c
 */
static void
putid(char *p, const char *s)
{
	const char *src;
	bool		safe = true;

	/* Detect whether we need to use double quotes */
	for (src = s; *src; src++)
	{
		if (!is_safe_acl_char(*src, false))
		{
			safe = false;
			break;
		}
	}
	if (!safe)
		*p++ = '"';
	for (src = s; *src; src++)
	{
		/* A double quote character in a username is encoded as "" */
		if (*src == '"')
			*p++ = '"';
		*p++ = *src;
	}
	if (!safe)
		*p++ = '"';
	*p = '\0';
}

/*
 * aclparse
 *		Consumes and parses an ACL specification of the form:
 *				[group|user] [A-Za-z0-9]*=[rwaR]*
 *		from string 's', ignoring any leading white space or white space
 *		between the optional id type keyword (group|user) and the actual
 *		ACL specification.
 *
 *		The group|user decoration is unnecessary in the roles world,
 *		but we still accept it for backward compatibility.
 *
 *		This routine is called by the parser as well as aclitemin(), hence
 *		the added generality.
 *
 * RETURNS:
 *		the string position in 's' immediately following the ACL
 *		specification.  Also:
 *		- loads the structure pointed to by 'aip' with the appropriate
 *		  UID/GID, id type identifier and mode type values.
 */
static const char *
aclparse(const char *s, AclItem *aip)
{
	AclMode		privs,
				goption,
				read;
	char		name[NAMEDATALEN];
	char		name2[NAMEDATALEN];

	Assert(s && aip);

	s = getid(s, name);
	if (*s != '=')
	{
		/* we just read a keyword, not a name */
		if (strcmp(name, "group") != 0 && strcmp(name, "user") != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("unrecognized key word: \"%s\"", name),
					 errhint("ACL key word must be \"group\" or \"user\".")));
		s = getid(s, name);		/* move s to the name beyond the keyword */
		if (name[0] == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("missing name"),
					 errhint("A name must follow the \"group\" or \"user\" key word.")));
	}

	if (*s != '=')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("missing \"=\" sign")));

	privs = goption = ACL_NO_RIGHTS;

	for (++s, read = 0; isalpha((unsigned char) *s) || *s == '*'; s++)
	{
		switch (*s)
		{
			case '*':
				goption |= read;
				break;
			case ACL_INSERT_CHR:
				read = ACL_INSERT;
				break;
			case ACL_SELECT_CHR:
				read = ACL_SELECT;
				break;
			case ACL_UPDATE_CHR:
				read = ACL_UPDATE;
				break;
			case ACL_DELETE_CHR:
				read = ACL_DELETE;
				break;
			case ACL_TRUNCATE_CHR:
				read = ACL_TRUNCATE;
				break;
			case ACL_REFERENCES_CHR:
				read = ACL_REFERENCES;
				break;
			case ACL_TRIGGER_CHR:
				read = ACL_TRIGGER;
				break;
			case ACL_EXECUTE_CHR:
				read = ACL_EXECUTE;
				break;
			case ACL_USAGE_CHR:
				read = ACL_USAGE;
				break;
			case ACL_CREATE_CHR:
				read = ACL_CREATE;
				break;
			case ACL_CREATE_TEMP_CHR:
				read = ACL_CREATE_TEMP;
				break;
			case ACL_CONNECT_CHR:
				read = ACL_CONNECT;
				break;
			case 'R':			/* ignore old RULE privileges */
				read = 0;
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid mode character: must be one of \"%s\"",
								ACL_ALL_RIGHTS_STR)));
		}

		privs |= read;
	}

	if (name[0] == '\0')
		aip->ai_grantee = ACL_ID_PUBLIC;
	else
		aip->ai_grantee = get_role_oid(name, false);

	/*
	 * XXX Allow a degree of backward compatibility by defaulting the grantor
	 * to the superuser.
	 */
	if (*s == '/')
	{
		s = getid(s + 1, name2);
		if (name2[0] == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("a name must follow the \"/\" sign")));
		aip->ai_grantor = get_role_oid(name2, false);
	}
	else
	{
		aip->ai_grantor = BOOTSTRAP_SUPERUSERID;
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_GRANTOR),
				 errmsg("defaulting grantor to user ID %u",
						BOOTSTRAP_SUPERUSERID)));
	}

	ACLITEM_SET_PRIVS_GOPTIONS(*aip, privs, goption);

	return s;
}

/*
 * allocacl
 *		Allocates storage for a new Acl with 'n' entries.
 *
 * RETURNS:
 *		the new Acl
 */
static Acl *
allocacl(int n)
{
	Acl		   *new_acl;
	Size		size;

	if (n < 0)
		elog(ERROR, "invalid size: %d", n);
	size = ACL_N_SIZE(n);
	new_acl = (Acl *) palloc0(size);
	SET_VARSIZE(new_acl, size);
	new_acl->ndim = 1;
	new_acl->dataoffset = 0;	/* we never put in any nulls */
	new_acl->elemtype = ACLITEMOID;
	ARR_LBOUND(new_acl)[0] = 1;
	ARR_DIMS(new_acl)[0] = n;
	return new_acl;
}

/*
 * Create a zero-entry ACL
 */
Acl *
make_empty_acl(void)
{
	return allocacl(0);
}

/*
 * Copy an ACL
 */
Acl *
aclcopy(const Acl *orig_acl)
{
	Acl		   *result_acl;

	result_acl = allocacl(ACL_NUM(orig_acl));

	memcpy(ACL_DAT(result_acl),
		   ACL_DAT(orig_acl),
		   ACL_NUM(orig_acl) * sizeof(AclItem));

	return result_acl;
}

/*
 * Concatenate two ACLs
 *
 * This is a bit cheesy, since we may produce an ACL with redundant entries.
 * Be careful what the result is used for!
 */
Acl *
aclconcat(const Acl *left_acl, const Acl *right_acl)
{
	Acl		   *result_acl;

	result_acl = allocacl(ACL_NUM(left_acl) + ACL_NUM(right_acl));

	memcpy(ACL_DAT(result_acl),
		   ACL_DAT(left_acl),
		   ACL_NUM(left_acl) * sizeof(AclItem));

	memcpy(ACL_DAT(result_acl) + ACL_NUM(left_acl),
		   ACL_DAT(right_acl),
		   ACL_NUM(right_acl) * sizeof(AclItem));

	return result_acl;
}

/*
 * Merge two ACLs
 *
 * This produces a properly merged ACL with no redundant entries.
 * Returns NULL on NULL input.
 */
Acl *
aclmerge(const Acl *left_acl, const Acl *right_acl, Oid ownerId)
{
	Acl		   *result_acl;
	AclItem    *aip;
	int			i,
				num;

	/* Check for cases where one or both are empty/null */
	if (left_acl == NULL || ACL_NUM(left_acl) == 0)
	{
		if (right_acl == NULL || ACL_NUM(right_acl) == 0)
			return NULL;
		else
			return aclcopy(right_acl);
	}
	else
	{
		if (right_acl == NULL || ACL_NUM(right_acl) == 0)
			return aclcopy(left_acl);
	}

	/* Merge them the hard way, one item at a time */
	result_acl = aclcopy(left_acl);

	aip = ACL_DAT(right_acl);
	num = ACL_NUM(right_acl);

	for (i = 0; i < num; i++, aip++)
	{
		Acl		   *tmp_acl;

		tmp_acl = aclupdate(result_acl, aip, ACL_MODECHG_ADD,
							ownerId, DROP_RESTRICT);
		pfree(result_acl);
		result_acl = tmp_acl;
	}

	return result_acl;
}

/*
 * Sort the items in an ACL (into an arbitrary but consistent order)
 */
void
aclitemsort(Acl *acl)
{
	if (acl != NULL && ACL_NUM(acl) > 1)
		qsort(ACL_DAT(acl), ACL_NUM(acl), sizeof(AclItem), aclitemComparator);
}

/*
 * Check if two ACLs are exactly equal
 *
 * This will not detect equality if the two arrays contain the same items
 * in different orders.  To handle that case, sort both inputs first,
 * using aclitemsort().
 */
bool
aclequal(const Acl *left_acl, const Acl *right_acl)
{
	/* Check for cases where one or both are empty/null */
	if (left_acl == NULL || ACL_NUM(left_acl) == 0)
	{
		if (right_acl == NULL || ACL_NUM(right_acl) == 0)
			return true;
		else
			return false;
	}
	else
	{
		if (right_acl == NULL || ACL_NUM(right_acl) == 0)
			return false;
	}

	if (ACL_NUM(left_acl) != ACL_NUM(right_acl))
		return false;

	if (memcmp(ACL_DAT(left_acl),
			   ACL_DAT(right_acl),
			   ACL_NUM(left_acl) * sizeof(AclItem)) == 0)
		return true;

	return false;
}

/*
 * Verify that an ACL array is acceptable (one-dimensional and has no nulls)
 */
static void
check_acl(const Acl *acl)
{
	if (ARR_ELEMTYPE(acl) != ACLITEMOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ACL array contains wrong data type")));
	if (ARR_NDIM(acl) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ACL arrays must be one-dimensional")));
	if (ARR_HASNULL(acl))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("ACL arrays must not contain null values")));
}

/*
 * aclitemin
 *		Allocates storage for, and fills in, a new AclItem given a string
 *		's' that contains an ACL specification.  See aclparse for details.
 *
 * RETURNS:
 *		the new AclItem
 */
Datum
aclitemin(PG_FUNCTION_ARGS)
{
	const char *s = PG_GETARG_CSTRING(0);
	AclItem    *aip;

	aip = (AclItem *) palloc(sizeof(AclItem));
	s = aclparse(s, aip);
	while (isspace((unsigned char) *s))
		++s;
	if (*s)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("extra garbage at the end of the ACL specification")));

	PG_RETURN_ACLITEM_P(aip);
}

/*
 * aclitemout
 *		Allocates storage for, and fills in, a new null-delimited string
 *		containing a formatted ACL specification.  See aclparse for details.
 *
 * RETURNS:
 *		the new string
 */
Datum
aclitemout(PG_FUNCTION_ARGS)
{
	AclItem    *aip = PG_GETARG_ACLITEM_P(0);
	char	   *p;
	char	   *out;
	HeapTuple	htup;
	unsigned	i;

	out = palloc(strlen("=/") +
				 2 * N_ACL_RIGHTS +
				 2 * (2 * NAMEDATALEN + 2) +
				 1);

	p = out;
	*p = '\0';

	if (aip->ai_grantee != ACL_ID_PUBLIC)
	{
		htup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(aip->ai_grantee));
		if (HeapTupleIsValid(htup))
		{
			putid(p, NameStr(((Form_pg_authid) GETSTRUCT(htup))->rolname));
			ReleaseSysCache(htup);
		}
		else
		{
			/* Generate numeric OID if we don't find an entry */
			sprintf(p, "%u", aip->ai_grantee);
		}
	}
	while (*p)
		++p;

	*p++ = '=';

	for (i = 0; i < N_ACL_RIGHTS; ++i)
	{
		if (ACLITEM_GET_PRIVS(*aip) & (1 << i))
			*p++ = ACL_ALL_RIGHTS_STR[i];
		if (ACLITEM_GET_GOPTIONS(*aip) & (1 << i))
			*p++ = '*';
	}

	*p++ = '/';
	*p = '\0';

	htup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(aip->ai_grantor));
	if (HeapTupleIsValid(htup))
	{
		putid(p, NameStr(((Form_pg_authid) GETSTRUCT(htup))->rolname));
		ReleaseSysCache(htup);
	}
	else
	{
		/* Generate numeric OID if we don't find an entry */
		sprintf(p, "%u", aip->ai_grantor);
	}

	PG_RETURN_CSTRING(out);
}

/*
 * aclitem_match
 *		Two AclItems are considered to match iff they have the same
 *		grantee and grantor; the privileges are ignored.
 */
static bool
aclitem_match(const AclItem *a1, const AclItem *a2)
{
	return a1->ai_grantee == a2->ai_grantee &&
		a1->ai_grantor == a2->ai_grantor;
}

/*
 * aclitemComparator
 *		qsort comparison function for AclItems
 */
static int
aclitemComparator(const void *arg1, const void *arg2)
{
	const AclItem *a1 = (const AclItem *) arg1;
	const AclItem *a2 = (const AclItem *) arg2;

	if (a1->ai_grantee > a2->ai_grantee)
		return 1;
	if (a1->ai_grantee < a2->ai_grantee)
		return -1;
	if (a1->ai_grantor > a2->ai_grantor)
		return 1;
	if (a1->ai_grantor < a2->ai_grantor)
		return -1;
	if (a1->ai_privs > a2->ai_privs)
		return 1;
	if (a1->ai_privs < a2->ai_privs)
		return -1;
	return 0;
}

/*
 * aclitem equality operator
 */
Datum
aclitem_eq(PG_FUNCTION_ARGS)
{
	AclItem    *a1 = PG_GETARG_ACLITEM_P(0);
	AclItem    *a2 = PG_GETARG_ACLITEM_P(1);
	bool		result;

	result = a1->ai_privs == a2->ai_privs &&
		a1->ai_grantee == a2->ai_grantee &&
		a1->ai_grantor == a2->ai_grantor;
	PG_RETURN_BOOL(result);
}

/*
 * aclitem hash function
 *
 * We make aclitems hashable not so much because anyone is likely to hash
 * them, as because we want array equality to work on aclitem arrays, and
 * with the typcache mechanism we must have a hash or btree opclass.
 */
Datum
hash_aclitem(PG_FUNCTION_ARGS)
{
	AclItem    *a = PG_GETARG_ACLITEM_P(0);

	/* not very bright, but avoids any issue of padding in struct */
	PG_RETURN_UINT32((uint32) (a->ai_privs + a->ai_grantee + a->ai_grantor));
}

/*
 * 64-bit hash function for aclitem.
 *
 * Similar to hash_aclitem, but accepts a seed and returns a uint64 value.
 */
Datum
hash_aclitem_extended(PG_FUNCTION_ARGS)
{
	AclItem    *a = PG_GETARG_ACLITEM_P(0);
	uint64		seed = PG_GETARG_INT64(1);
	uint32		sum = (uint32) (a->ai_privs + a->ai_grantee + a->ai_grantor);

	return (seed == 0) ? UInt64GetDatum(sum) : hash_uint32_extended(sum, seed);
}

/*
 * acldefault()  --- create an ACL describing default access permissions
 *
 * Change this routine if you want to alter the default access policy for
 * newly-created objects (or any object with a NULL acl entry).  When
 * you make a change here, don't forget to update the GRANT man page,
 * which explains all the default permissions.
 *
 * Note that these are the hard-wired "defaults" that are used when no
 * explicit privileges are set.
 */
Acl *
acldefault(ObjectType objtype, Oid ownerId)
{
	AclMode		world_default;
	AclMode		owner_default;
	int			nacl;
	Acl		   *acl;
	AclItem    *aip;

	switch (objtype)
	{
		case OBJECT_COLUMN:
			/* by default, columns have no extra privileges */
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_NO_RIGHTS;
			break;
		case OBJECT_TABLE:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_RELATION;
			break;
		case OBJECT_SEQUENCE:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_SEQUENCE;
			break;
		case OBJECT_DATABASE:
			/* for backwards compatibility, grant some rights by default */
			world_default = ACL_CREATE_TEMP | ACL_CONNECT;
			owner_default = ACL_ALL_RIGHTS_DATABASE;
			break;
		case OBJECT_FUNCTION:
			/* Grant EXECUTE by default, for now */
			world_default = ACL_EXECUTE;
			owner_default = ACL_ALL_RIGHTS_FUNCTION;
			break;
		case OBJECT_LANGUAGE:
			/* Grant USAGE by default, for now */
			world_default = ACL_USAGE;
			owner_default = ACL_ALL_RIGHTS_LANGUAGE;
			break;
		case OBJECT_LARGEOBJECT:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_LARGEOBJECT;
			break;
		case OBJECT_SCHEMA:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_SCHEMA;
			break;
		case OBJECT_TABLESPACE:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_TABLESPACE;
			break;
		case OBJECT_DOMAIN:
		case OBJECT_TYPE:
			world_default = ACL_USAGE;
			owner_default = ACL_ALL_RIGHTS_TYPE;
			break;
		default:
			elog(ERROR, "unrecognized objtype: %d", (int) objtype);
			world_default = ACL_NO_RIGHTS;	/* keep compiler quiet */
			owner_default = ACL_NO_RIGHTS;
			break;
	}

	nacl = 0;
	if (world_default != ACL_NO_RIGHTS)
		nacl++;
	if (owner_default != ACL_NO_RIGHTS)
		nacl++;

	acl = allocacl(nacl);
	aip = ACL_DAT(acl);

	if (world_default != ACL_NO_RIGHTS)
	{
		aip->ai_grantee = ACL_ID_PUBLIC;
		aip->ai_grantor = ownerId;
		ACLITEM_SET_PRIVS_GOPTIONS(*aip, world_default, ACL_NO_RIGHTS);
		aip++;
	}

	/*
	 * Note that the owner's entry shows all ordinary privileges but no grant
	 * options.  This is because his grant options come "from the system" and
	 * not from his own efforts.  (The SQL spec says that the owner's rights
	 * come from a "_SYSTEM" authid.)  However, we do consider that the
	 * owner's ordinary privileges are self-granted; this lets him revoke
	 * them.  We implement the owner's grant options without any explicit
	 * "_SYSTEM"-like ACL entry, by internally special-casing the owner
	 * wherever we are testing grant options.
	 */
	if (owner_default != ACL_NO_RIGHTS)
	{
		aip->ai_grantee = ownerId;
		aip->ai_grantor = ownerId;
		ACLITEM_SET_PRIVS_GOPTIONS(*aip, owner_default, ACL_NO_RIGHTS);
	}

	return acl;
}


/*
 * SQL-accessible version of acldefault().  Hackish mapping from "char" type to
 * OBJECT_* values.
 */
Datum
acldefault_sql(PG_FUNCTION_ARGS)
{
	char		objtypec = PG_GETARG_CHAR(0);
	Oid			owner = PG_GETARG_OID(1);
	ObjectType	objtype = 0;

	switch (objtypec)
	{
		case 'c':
			objtype = OBJECT_COLUMN;
			break;
		case 'r':
			objtype = OBJECT_TABLE;
			break;
		case 's':
			objtype = OBJECT_SEQUENCE;
			break;
		case 'd':
			objtype = OBJECT_DATABASE;
			break;
		case 'f':
			objtype = OBJECT_FUNCTION;
			break;
		case 'l':
			objtype = OBJECT_LANGUAGE;
			break;
		case 'L':
			objtype = OBJECT_LARGEOBJECT;
			break;
		case 'n':
			objtype = OBJECT_SCHEMA;
			break;
		case 't':
			objtype = OBJECT_TABLESPACE;
			break;
		case 'T':
			objtype = OBJECT_TYPE;
			break;
		default:
			elog(ERROR, "unrecognized objtype abbreviation: %c", objtypec);
	}

	PG_RETURN_ACL_P(acldefault(objtype, owner));
}


/*
 * Update an ACL array to add or remove specified privileges.
 *
 *	old_acl: the input ACL array
 *	mod_aip: defines the privileges to be added, removed, or substituted
 *	modechg: ACL_MODECHG_ADD, ACL_MODECHG_DEL, or ACL_MODECHG_EQL
 *	ownerId: Oid of object owner
 *	behavior: RESTRICT or CASCADE behavior for recursive removal
 *
 * ownerid and behavior are only relevant when the update operation specifies
 * deletion of grant options.
 *
 * The result is a modified copy; the input object is not changed.
 *
 * NB: caller is responsible for having detoasted the input ACL, if needed.
 */
Acl *
aclupdate(const Acl *old_acl, const AclItem *mod_aip,
		  int modechg, Oid ownerId, DropBehavior behavior)
{
	Acl		   *new_acl = NULL;
	AclItem    *old_aip,
			   *new_aip = NULL;
	AclMode		old_rights,
				old_goptions,
				new_rights,
				new_goptions;
	int			dst,
				num;

	/* Caller probably already checked old_acl, but be safe */
	check_acl(old_acl);

	/* If granting grant options, check for circularity */
	if (modechg != ACL_MODECHG_DEL &&
		ACLITEM_GET_GOPTIONS(*mod_aip) != ACL_NO_RIGHTS)
		check_circularity(old_acl, mod_aip, ownerId);

	num = ACL_NUM(old_acl);
	old_aip = ACL_DAT(old_acl);

	/*
	 * Search the ACL for an existing entry for this grantee and grantor. If
	 * one exists, just modify the entry in-place (well, in the same position,
	 * since we actually return a copy); otherwise, insert the new entry at
	 * the end.
	 */

	for (dst = 0; dst < num; ++dst)
	{
		if (aclitem_match(mod_aip, old_aip + dst))
		{
			/* found a match, so modify existing item */
			new_acl = allocacl(num);
			new_aip = ACL_DAT(new_acl);
			memcpy(new_acl, old_acl, ACL_SIZE(old_acl));
			break;
		}
	}

	if (dst == num)
	{
		/* need to append a new item */
		new_acl = allocacl(num + 1);
		new_aip = ACL_DAT(new_acl);
		memcpy(new_aip, old_aip, num * sizeof(AclItem));

		/* initialize the new entry with no permissions */
		new_aip[dst].ai_grantee = mod_aip->ai_grantee;
		new_aip[dst].ai_grantor = mod_aip->ai_grantor;
		ACLITEM_SET_PRIVS_GOPTIONS(new_aip[dst],
								   ACL_NO_RIGHTS, ACL_NO_RIGHTS);
		num++;					/* set num to the size of new_acl */
	}

	old_rights = ACLITEM_GET_RIGHTS(new_aip[dst]);
	old_goptions = ACLITEM_GET_GOPTIONS(new_aip[dst]);

	/* apply the specified permissions change */
	switch (modechg)
	{
		case ACL_MODECHG_ADD:
			ACLITEM_SET_RIGHTS(new_aip[dst],
							   old_rights | ACLITEM_GET_RIGHTS(*mod_aip));
			break;
		case ACL_MODECHG_DEL:
			ACLITEM_SET_RIGHTS(new_aip[dst],
							   old_rights & ~ACLITEM_GET_RIGHTS(*mod_aip));
			break;
		case ACL_MODECHG_EQL:
			ACLITEM_SET_RIGHTS(new_aip[dst],
							   ACLITEM_GET_RIGHTS(*mod_aip));
			break;
	}

	new_rights = ACLITEM_GET_RIGHTS(new_aip[dst]);
	new_goptions = ACLITEM_GET_GOPTIONS(new_aip[dst]);

	/*
	 * If the adjusted entry has no permissions, delete it from the list.
	 */
	if (new_rights == ACL_NO_RIGHTS)
	{
		memmove(new_aip + dst,
				new_aip + dst + 1,
				(num - dst - 1) * sizeof(AclItem));
		/* Adjust array size to be 'num - 1' items */
		ARR_DIMS(new_acl)[0] = num - 1;
		SET_VARSIZE(new_acl, ACL_N_SIZE(num - 1));
	}

	/*
	 * Remove abandoned privileges (cascading revoke).  Currently we can only
	 * handle this when the grantee is not PUBLIC.
	 */
	if ((old_goptions & ~new_goptions) != 0)
	{
		Assert(mod_aip->ai_grantee != ACL_ID_PUBLIC);
		new_acl = recursive_revoke(new_acl, mod_aip->ai_grantee,
								   (old_goptions & ~new_goptions),
								   ownerId, behavior);
	}

	return new_acl;
}

/*
 * Update an ACL array to reflect a change of owner to the parent object
 *
 *	old_acl: the input ACL array (must not be NULL)
 *	oldOwnerId: Oid of the old object owner
 *	newOwnerId: Oid of the new object owner
 *
 * The result is a modified copy; the input object is not changed.
 *
 * NB: caller is responsible for having detoasted the input ACL, if needed.
 */
Acl *
aclnewowner(const Acl *old_acl, Oid oldOwnerId, Oid newOwnerId)
{
	Acl		   *new_acl;
	AclItem    *new_aip;
	AclItem    *old_aip;
	AclItem    *dst_aip;
	AclItem    *src_aip;
	AclItem    *targ_aip;
	bool		newpresent = false;
	int			dst,
				src,
				targ,
				num;

	check_acl(old_acl);

	/*
	 * Make a copy of the given ACL, substituting new owner ID for old
	 * wherever it appears as either grantor or grantee.  Also note if the new
	 * owner ID is already present.
	 */
	num = ACL_NUM(old_acl);
	old_aip = ACL_DAT(old_acl);
	new_acl = allocacl(num);
	new_aip = ACL_DAT(new_acl);
	memcpy(new_aip, old_aip, num * sizeof(AclItem));
	for (dst = 0, dst_aip = new_aip; dst < num; dst++, dst_aip++)
	{
		if (dst_aip->ai_grantor == oldOwnerId)
			dst_aip->ai_grantor = newOwnerId;
		else if (dst_aip->ai_grantor == newOwnerId)
			newpresent = true;
		if (dst_aip->ai_grantee == oldOwnerId)
			dst_aip->ai_grantee = newOwnerId;
		else if (dst_aip->ai_grantee == newOwnerId)
			newpresent = true;
	}

	/*
	 * If the old ACL contained any references to the new owner, then we may
	 * now have generated an ACL containing duplicate entries.  Find them and
	 * merge them so that there are not duplicates.  (This is relatively
	 * expensive since we use a stupid O(N^2) algorithm, but it's unlikely to
	 * be the normal case.)
	 *
	 * To simplify deletion of duplicate entries, we temporarily leave them in
	 * the array but set their privilege masks to zero; when we reach such an
	 * entry it's just skipped.  (Thus, a side effect of this code will be to
	 * remove privilege-free entries, should there be any in the input.)  dst
	 * is the next output slot, targ is the currently considered input slot
	 * (always >= dst), and src scans entries to the right of targ looking for
	 * duplicates.  Once an entry has been emitted to dst it is known
	 * duplicate-free and need not be considered anymore.
	 */
	if (newpresent)
	{
		dst = 0;
		for (targ = 0, targ_aip = new_aip; targ < num; targ++, targ_aip++)
		{
			/* ignore if deleted in an earlier pass */
			if (ACLITEM_GET_RIGHTS(*targ_aip) == ACL_NO_RIGHTS)
				continue;
			/* find and merge any duplicates */
			for (src = targ + 1, src_aip = targ_aip + 1; src < num;
				 src++, src_aip++)
			{
				if (ACLITEM_GET_RIGHTS(*src_aip) == ACL_NO_RIGHTS)
					continue;
				if (aclitem_match(targ_aip, src_aip))
				{
					ACLITEM_SET_RIGHTS(*targ_aip,
									   ACLITEM_GET_RIGHTS(*targ_aip) |
									   ACLITEM_GET_RIGHTS(*src_aip));
					/* mark the duplicate deleted */
					ACLITEM_SET_RIGHTS(*src_aip, ACL_NO_RIGHTS);
				}
			}
			/* and emit to output */
			new_aip[dst] = *targ_aip;
			dst++;
		}
		/* Adjust array size to be 'dst' items */
		ARR_DIMS(new_acl)[0] = dst;
		SET_VARSIZE(new_acl, ACL_N_SIZE(dst));
	}

	return new_acl;
}


/*
 * When granting grant options, we must disallow attempts to set up circular
 * chains of grant options.  Suppose A (the object owner) grants B some
 * privileges with grant option, and B re-grants them to C.  If C could
 * grant the privileges to B as well, then A would be unable to effectively
 * revoke the privileges from B, since recursive_revoke would consider that
 * B still has 'em from C.
 *
 * We check for this by recursively deleting all grant options belonging to
 * the target grantee, and then seeing if the would-be grantor still has the
 * grant option or not.
 */
static void
check_circularity(const Acl *old_acl, const AclItem *mod_aip,
				  Oid ownerId)
{
	Acl		   *acl;
	AclItem    *aip;
	int			i,
				num;
	AclMode		own_privs;

	check_acl(old_acl);

	/*
	 * For now, grant options can only be granted to roles, not PUBLIC.
	 * Otherwise we'd have to work a bit harder here.
	 */
	Assert(mod_aip->ai_grantee != ACL_ID_PUBLIC);

	/* The owner always has grant options, no need to check */
	if (mod_aip->ai_grantor == ownerId)
		return;

	/* Make a working copy */
	acl = allocacl(ACL_NUM(old_acl));
	memcpy(acl, old_acl, ACL_SIZE(old_acl));

	/* Zap all grant options of target grantee, plus what depends on 'em */
cc_restart:
	num = ACL_NUM(acl);
	aip = ACL_DAT(acl);
	for (i = 0; i < num; i++)
	{
		if (aip[i].ai_grantee == mod_aip->ai_grantee &&
			ACLITEM_GET_GOPTIONS(aip[i]) != ACL_NO_RIGHTS)
		{
			Acl		   *new_acl;

			/* We'll actually zap ordinary privs too, but no matter */
			new_acl = aclupdate(acl, &aip[i], ACL_MODECHG_DEL,
								ownerId, DROP_CASCADE);

			pfree(acl);
			acl = new_acl;

			goto cc_restart;
		}
	}

	/* Now we can compute grantor's independently-derived privileges */
	own_privs = aclmask(acl,
						mod_aip->ai_grantor,
						ownerId,
						ACL_GRANT_OPTION_FOR(ACLITEM_GET_GOPTIONS(*mod_aip)),
						ACLMASK_ALL);
	own_privs = ACL_OPTION_TO_PRIVS(own_privs);

	if ((ACLITEM_GET_GOPTIONS(*mod_aip) & ~own_privs) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_GRANT_OPERATION),
				 errmsg("grant options cannot be granted back to your own grantor")));

	pfree(acl);
}


/*
 * Ensure that no privilege is "abandoned".  A privilege is abandoned
 * if the user that granted the privilege loses the grant option.  (So
 * the chain through which it was granted is broken.)  Either the
 * abandoned privileges are revoked as well, or an error message is
 * printed, depending on the drop behavior option.
 *
 *	acl: the input ACL list
 *	grantee: the user from whom some grant options have been revoked
 *	revoke_privs: the grant options being revoked
 *	ownerId: Oid of object owner
 *	behavior: RESTRICT or CASCADE behavior for recursive removal
 *
 * The input Acl object is pfree'd if replaced.
 */
static Acl *
recursive_revoke(Acl *acl,
				 Oid grantee,
				 AclMode revoke_privs,
				 Oid ownerId,
				 DropBehavior behavior)
{
	AclMode		still_has;
	AclItem    *aip;
	int			i,
				num;

	check_acl(acl);

	/* The owner can never truly lose grant options, so short-circuit */
	if (grantee == ownerId)
		return acl;

	/* The grantee might still have some grant options via another grantor */
	still_has = aclmask(acl, grantee, ownerId,
						ACL_GRANT_OPTION_FOR(revoke_privs),
						ACLMASK_ALL);
	revoke_privs &= ~ACL_OPTION_TO_PRIVS(still_has);
	if (revoke_privs == ACL_NO_RIGHTS)
		return acl;

restart:
	num = ACL_NUM(acl);
	aip = ACL_DAT(acl);
	for (i = 0; i < num; i++)
	{
		if (aip[i].ai_grantor == grantee
			&& (ACLITEM_GET_PRIVS(aip[i]) & revoke_privs) != 0)
		{
			AclItem		mod_acl;
			Acl		   *new_acl;

			if (behavior == DROP_RESTRICT)
				ereport(ERROR,
						(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
						 errmsg("dependent privileges exist"),
						 errhint("Use CASCADE to revoke them too.")));

			mod_acl.ai_grantor = grantee;
			mod_acl.ai_grantee = aip[i].ai_grantee;
			ACLITEM_SET_PRIVS_GOPTIONS(mod_acl,
									   revoke_privs,
									   revoke_privs);

			new_acl = aclupdate(acl, &mod_acl, ACL_MODECHG_DEL,
								ownerId, behavior);

			pfree(acl);
			acl = new_acl;

			goto restart;
		}
	}

	return acl;
}


/*
 * aclmask --- compute bitmask of all privileges held by roleid.
 *
 * When 'how' = ACLMASK_ALL, this simply returns the privilege bits
 * held by the given roleid according to the given ACL list, ANDed
 * with 'mask'.  (The point of passing 'mask' is to let the routine
 * exit early if all privileges of interest have been found.)
 *
 * When 'how' = ACLMASK_ANY, returns as soon as any bit in the mask
 * is known true.  (This lets us exit soonest in cases where the
 * caller is only going to test for zero or nonzero result.)
 *
 * Usage patterns:
 *
 * To see if any of a set of privileges are held:
 *		if (aclmask(acl, roleid, ownerId, privs, ACLMASK_ANY) != 0)
 *
 * To see if all of a set of privileges are held:
 *		if (aclmask(acl, roleid, ownerId, privs, ACLMASK_ALL) == privs)
 *
 * To determine exactly which of a set of privileges are held:
 *		heldprivs = aclmask(acl, roleid, ownerId, privs, ACLMASK_ALL);
 */
AclMode
aclmask(const Acl *acl, Oid roleid, Oid ownerId,
		AclMode mask, AclMaskHow how)
{
	/*
	 * minipg: 权限（ACL）机制已裁剪。所有对象访问一律视为授予所请求的全部
	 * 权限，即任何连接用户都等同于超级用户。因此直接返回调用方所求的掩码，
	 * 而不再对 ACL 进行实际计算。
	 */
	return mask;
}


/*
 * aclmask_direct --- compute bitmask of all privileges held by roleid.
 *
 * This is exactly like aclmask() except that we consider only privileges
 * held *directly* by roleid, not those inherited via role membership.
 */
static AclMode
aclmask_direct(const Acl *acl, Oid roleid, Oid ownerId,
			   AclMode mask, AclMaskHow how)
{
	AclMode		result;
	AclItem    *aidat;
	int			i,
				num;

	/*
	 * Null ACL should not happen, since caller should have inserted
	 * appropriate default
	 */
	if (acl == NULL)
		elog(ERROR, "null ACL");

	check_acl(acl);

	/* Quick exit for mask == 0 */
	if (mask == 0)
		return 0;

	result = 0;

	/* Owner always implicitly has all grant options */
	if ((mask & ACLITEM_ALL_GOPTION_BITS) &&
		roleid == ownerId)
	{
		result = mask & ACLITEM_ALL_GOPTION_BITS;
		if ((how == ACLMASK_ALL) ? (result == mask) : (result != 0))
			return result;
	}

	num = ACL_NUM(acl);
	aidat = ACL_DAT(acl);

	/*
	 * Check privileges granted directly to roleid (and not to public)
	 */
	for (i = 0; i < num; i++)
	{
		AclItem    *aidata = &aidat[i];

		if (aidata->ai_grantee == roleid)
		{
			result |= aidata->ai_privs & mask;
			if ((how == ACLMASK_ALL) ? (result == mask) : (result != 0))
				return result;
		}
	}

	return result;
}


/*
 * aclmembers
 *		Find out all the roleids mentioned in an Acl.
 *		Note that we do not distinguish grantors from grantees.
 *
 * *roleids is set to point to a palloc'd array containing distinct OIDs
 * in sorted order.  The length of the array is the function result.
 */
int
aclmembers(const Acl *acl, Oid **roleids)
{
	Oid		   *list;
	const AclItem *acldat;
	int			i,
				j;

	if (acl == NULL || ACL_NUM(acl) == 0)
	{
		*roleids = NULL;
		return 0;
	}

	check_acl(acl);

	/* Allocate the worst-case space requirement */
	list = palloc(ACL_NUM(acl) * 2 * sizeof(Oid));
	acldat = ACL_DAT(acl);

	/*
	 * Walk the ACL collecting mentioned RoleIds.
	 */
	j = 0;
	for (i = 0; i < ACL_NUM(acl); i++)
	{
		const AclItem *ai = &acldat[i];

		if (ai->ai_grantee != ACL_ID_PUBLIC)
			list[j++] = ai->ai_grantee;
		/* grantor is currently never PUBLIC, but let's check anyway */
		if (ai->ai_grantor != ACL_ID_PUBLIC)
			list[j++] = ai->ai_grantor;
	}

	/* Sort the array */
	qsort(list, j, sizeof(Oid), oid_cmp);

	/*
	 * We could repalloc the array down to minimum size, but it's hardly worth
	 * it since it's only transient memory.
	 */
	*roleids = list;

	/* Remove duplicates from the array */
	return qunique(list, j, sizeof(Oid), oid_cmp);
}


/*
 * aclinsert (exported function)
 */
Datum
aclinsert(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("aclinsert is no longer supported")));

	PG_RETURN_NULL();			/* keep compiler quiet */
}

Datum
aclremove(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("aclremove is no longer supported")));

	PG_RETURN_NULL();			/* keep compiler quiet */
}

Datum
aclcontains(PG_FUNCTION_ARGS)
{
	Acl		   *acl = PG_GETARG_ACL_P(0);
	AclItem    *aip = PG_GETARG_ACLITEM_P(1);
	AclItem    *aidat;
	int			i,
				num;

	check_acl(acl);
	num = ACL_NUM(acl);
	aidat = ACL_DAT(acl);
	for (i = 0; i < num; ++i)
	{
		if (aip->ai_grantee == aidat[i].ai_grantee &&
			aip->ai_grantor == aidat[i].ai_grantor &&
			(ACLITEM_GET_RIGHTS(*aip) & ACLITEM_GET_RIGHTS(aidat[i])) == ACLITEM_GET_RIGHTS(*aip))
			PG_RETURN_BOOL(true);
	}
	PG_RETURN_BOOL(false);
}

Datum
makeaclitem(PG_FUNCTION_ARGS)
{
	Oid			grantee = PG_GETARG_OID(0);
	Oid			grantor = PG_GETARG_OID(1);
	text	   *privtext = PG_GETARG_TEXT_PP(2);
	bool		goption = PG_GETARG_BOOL(3);
	AclItem    *result;
	AclMode		priv;

	priv = convert_priv_string(privtext);

	result = (AclItem *) palloc(sizeof(AclItem));

	result->ai_grantee = grantee;
	result->ai_grantor = grantor;

	ACLITEM_SET_PRIVS_GOPTIONS(*result, priv,
							   (goption ? priv : ACL_NO_RIGHTS));

	PG_RETURN_ACLITEM_P(result);
}

static AclMode
convert_priv_string(text *priv_type_text)
{
	char	   *priv_type = text_to_cstring(priv_type_text);

	if (pg_strcasecmp(priv_type, "SELECT") == 0)
		return ACL_SELECT;
	if (pg_strcasecmp(priv_type, "INSERT") == 0)
		return ACL_INSERT;
	if (pg_strcasecmp(priv_type, "UPDATE") == 0)
		return ACL_UPDATE;
	if (pg_strcasecmp(priv_type, "DELETE") == 0)
		return ACL_DELETE;
	if (pg_strcasecmp(priv_type, "TRUNCATE") == 0)
		return ACL_TRUNCATE;
	if (pg_strcasecmp(priv_type, "REFERENCES") == 0)
		return ACL_REFERENCES;
	if (pg_strcasecmp(priv_type, "TRIGGER") == 0)
		return ACL_TRIGGER;
	if (pg_strcasecmp(priv_type, "EXECUTE") == 0)
		return ACL_EXECUTE;
	if (pg_strcasecmp(priv_type, "USAGE") == 0)
		return ACL_USAGE;
	if (pg_strcasecmp(priv_type, "CREATE") == 0)
		return ACL_CREATE;
	if (pg_strcasecmp(priv_type, "TEMP") == 0)
		return ACL_CREATE_TEMP;
	if (pg_strcasecmp(priv_type, "TEMPORARY") == 0)
		return ACL_CREATE_TEMP;
	if (pg_strcasecmp(priv_type, "CONNECT") == 0)
		return ACL_CONNECT;
	if (pg_strcasecmp(priv_type, "RULE") == 0)
		return 0;				/* ignore old RULE privileges */

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized privilege type: \"%s\"", priv_type)));
	return ACL_NO_RIGHTS;		/* keep compiler quiet */
}


static const char *
convert_aclright_to_string(int aclright)
{
	switch (aclright)
	{
		case ACL_INSERT:
			return "INSERT";
		case ACL_SELECT:
			return "SELECT";
		case ACL_UPDATE:
			return "UPDATE";
		case ACL_DELETE:
			return "DELETE";
		case ACL_TRUNCATE:
			return "TRUNCATE";
		case ACL_REFERENCES:
			return "REFERENCES";
		case ACL_TRIGGER:
			return "TRIGGER";
		case ACL_EXECUTE:
			return "EXECUTE";
		case ACL_USAGE:
			return "USAGE";
		case ACL_CREATE:
			return "CREATE";
		case ACL_CREATE_TEMP:
			return "TEMPORARY";
		case ACL_CONNECT:
			return "CONNECT";
		default:
			elog(ERROR, "unrecognized aclright: %d", aclright);
			return NULL;
	}
}


/*----------
 * Convert an aclitem[] to a table.
 *
 * Example:
 *
 * aclexplode('{=r/joe,foo=a*w/joe}'::aclitem[])
 *
 * returns the table
 *
 * {{ OID(joe), 0::OID,   'SELECT', false },
 *	{ OID(joe), OID(foo), 'INSERT', true },
 *	{ OID(joe), OID(foo), 'UPDATE', false }}
 *----------
 */
Datum
aclexplode(PG_FUNCTION_ARGS)
{
	Acl		   *acl = PG_GETARG_ACL_P(0);
	FuncCallContext *funcctx;
	int		   *idx;
	AclItem    *aidat;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		check_acl(acl);

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/*
		 * build tupdesc for result tuples (matches out parameters in pg_proc
		 * entry)
		 */
		tupdesc = CreateTemplateTupleDesc(4);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "grantor",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "grantee",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "privilege_type",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "is_grantable",
						   BOOLOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/* allocate memory for user context */
		idx = (int *) palloc(sizeof(int[2]));
		idx[0] = 0;				/* ACL array item index */
		idx[1] = -1;			/* privilege type counter */
		funcctx->user_fctx = (void *) idx;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	idx = (int *) funcctx->user_fctx;
	aidat = ACL_DAT(acl);

	/* need test here in case acl has no items */
	while (idx[0] < ACL_NUM(acl))
	{
		AclItem    *aidata;
		AclMode		priv_bit;

		idx[1]++;
		if (idx[1] == N_ACL_RIGHTS)
		{
			idx[1] = 0;
			idx[0]++;
			if (idx[0] >= ACL_NUM(acl)) /* done */
				break;
		}
		aidata = &aidat[idx[0]];
		priv_bit = 1 << idx[1];

		if (ACLITEM_GET_PRIVS(*aidata) & priv_bit)
		{
			Datum		result;
			Datum		values[4];
			bool		nulls[4];
			HeapTuple	tuple;

			values[0] = ObjectIdGetDatum(aidata->ai_grantor);
			values[1] = ObjectIdGetDatum(aidata->ai_grantee);
			values[2] = CStringGetTextDatum(convert_aclright_to_string(priv_bit));
			values[3] = BoolGetDatum((ACLITEM_GET_GOPTIONS(*aidata) & priv_bit) != 0);

			MemSet(nulls, 0, sizeof(nulls));

			tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
			result = HeapTupleGetDatum(tuple);

			SRF_RETURN_NEXT(funcctx, result);
		}
	}

	SRF_RETURN_DONE(funcctx);
}




/*
 * initialization function (called by InitPostgres)
 */
void
initialize_acl(void)
{
	if (!IsBootstrapProcessingMode())
	{
		cached_db_hash =
			GetSysCacheHashValue1(DATABASEOID,
								  ObjectIdGetDatum(MyDatabaseId));

		/*
		 * In normal mode, set a callback on any syscache invalidation of rows
		 * of pg_auth_members (for roles_is_member_of()), pg_authid (for
		 * has_rolinherit()), or pg_database (for roles_is_member_of())
		 */
		CacheRegisterSyscacheCallback(AUTHMEMROLEMEM,
									  RoleMembershipCacheCallback,
									  (Datum) 0);
		CacheRegisterSyscacheCallback(AUTHOID,
									  RoleMembershipCacheCallback,
									  (Datum) 0);
		CacheRegisterSyscacheCallback(DATABASEOID,
									  RoleMembershipCacheCallback,
									  (Datum) 0);
	}
}

/*
 * RoleMembershipCacheCallback
 *		Syscache inval callback function
 */
static void
RoleMembershipCacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	if (cacheid == DATABASEOID &&
		hashvalue != cached_db_hash &&
		hashvalue != 0)
	{
		return;					/* ignore pg_database changes for other DBs */
	}

	/* Force membership caches to be recomputed on next use */
	cached_role[ROLERECURSE_PRIVS] = InvalidOid;
	cached_role[ROLERECURSE_MEMBERS] = InvalidOid;
}


/* Check if specified role has rolinherit set */
static bool
has_rolinherit(Oid roleid)
{
	bool		result = false;
	HeapTuple	utup;

	utup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (HeapTupleIsValid(utup))
	{
		result = ((Form_pg_authid) GETSTRUCT(utup))->rolinherit;
		ReleaseSysCache(utup);
	}
	return result;
}


/*
 * Get a list of roles that the specified roleid is a member of
 *
 * Type ROLERECURSE_PRIVS recurses only through roles that have rolinherit
 * set, while ROLERECURSE_MEMBERS recurses through all roles.  This sets
 * *is_admin==true if and only if role "roleid" has an ADMIN OPTION membership
 * in role "admin_of".
 *
 * Since indirect membership testing is relatively expensive, we cache
 * a list of memberships.  Hence, the result is only guaranteed good until
 * the next call of roles_is_member_of()!
 *
 * For the benefit of select_best_grantor, the result is defined to be
 * in breadth-first order, ie, closer relationships earlier.
 */
static List *
roles_is_member_of(Oid roleid, enum RoleRecurseType type,
				   Oid admin_of, bool *is_admin)
{
	Oid			dba;
	List	   *roles_list;
	ListCell   *l;
	List	   *new_cached_roles;
	MemoryContext oldctx;

	Assert(OidIsValid(admin_of) == PointerIsValid(is_admin));

	/* If cache is valid and ADMIN OPTION not sought, just return the list */
	if (cached_role[type] == roleid && !OidIsValid(admin_of) &&
		OidIsValid(cached_role[type]))
		return cached_roles[type];

	/*
	 * Role expansion happens in a non-database backend when guc.c checks
	 * ROLE_PG_READ_ALL_SETTINGS for a physical walsender SHOW command.  In
	 * that case, no role gets pg_database_owner.
	 */
	if (!OidIsValid(MyDatabaseId))
		dba = InvalidOid;
	else
	{
		HeapTuple	dbtup;

		dbtup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
		if (!HeapTupleIsValid(dbtup))
			elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
		dba = ((Form_pg_database) GETSTRUCT(dbtup))->datdba;
		ReleaseSysCache(dbtup);
	}

	/*
	 * Find all the roles that roleid is a member of, including multi-level
	 * recursion.  The role itself will always be the first element of the
	 * resulting list.
	 *
	 * Each element of the list is scanned to see if it adds any indirect
	 * memberships.  We can use a single list as both the record of
	 * already-found memberships and the agenda of roles yet to be scanned.
	 * This is a bit tricky but works because the foreach() macro doesn't
	 * fetch the next list element until the bottom of the loop.
	 */
	roles_list = list_make1_oid(roleid);

	foreach(l, roles_list)
	{
		Oid			memberid = lfirst_oid(l);
		CatCList   *memlist;
		int			i;

		if (type == ROLERECURSE_PRIVS && !has_rolinherit(memberid))
			continue;			/* ignore non-inheriting roles */

		/* Find roles that memberid is directly a member of */
		memlist = SearchSysCacheList1(AUTHMEMMEMROLE,
									  ObjectIdGetDatum(memberid));
		for (i = 0; i < memlist->n_members; i++)
		{
			HeapTuple	tup = &memlist->members[i]->tuple;
			Oid			otherid = ((Form_pg_auth_members) GETSTRUCT(tup))->roleid;

			/*
			 * While otherid==InvalidOid shouldn't appear in the catalog, the
			 * OidIsValid() avoids crashing if that arises.
			 */
			if (otherid == admin_of &&
				((Form_pg_auth_members) GETSTRUCT(tup))->admin_option &&
				OidIsValid(admin_of))
				*is_admin = true;

			/*
			 * Even though there shouldn't be any loops in the membership
			 * graph, we must test for having already seen this role. It is
			 * legal for instance to have both A->B and A->C->B.
			 */
			roles_list = list_append_unique_oid(roles_list, otherid);
		}
		ReleaseSysCacheList(memlist);

		/* implement pg_database_owner implicit membership */
		if (memberid == dba && OidIsValid(dba))
			roles_list = list_append_unique_oid(roles_list,
												ROLE_PG_DATABASE_OWNER);
	}

	/*
	 * Copy the completed list into TopMemoryContext so it will persist.
	 */
	oldctx = MemoryContextSwitchTo(TopMemoryContext);
	new_cached_roles = list_copy(roles_list);
	MemoryContextSwitchTo(oldctx);
	list_free(roles_list);

	/*
	 * Now safe to assign to state variable
	 */
	cached_role[type] = InvalidOid; /* just paranoia */
	list_free(cached_roles[type]);
	cached_roles[type] = new_cached_roles;
	cached_role[type] = roleid;

	/* And now we can return the answer */
	return cached_roles[type];
}


/*
 * Does member have the privileges of role (directly or indirectly)?
 *
 * This is defined not to recurse through roles that don't have rolinherit
 * set; for such roles, membership implies the ability to do SET ROLE, but
 * the privileges are not available until you've done so.
 */
bool
has_privs_of_role(Oid member, Oid role)
{
	/* Fast path for simple case */
	if (member == role)
		return true;

	/* Superusers have every privilege, so are part of every role */
	if (superuser_arg(member))
		return true;

	/*
	 * Find all the roles that member has the privileges of, including
	 * multi-level recursion, then see if target role is any one of them.
	 */
	return list_member_oid(roles_is_member_of(member, ROLERECURSE_PRIVS,
											  InvalidOid, NULL),
						   role);
}


/*
 * Is member a member of role (directly or indirectly)?
 *
 * This is defined to recurse through roles regardless of rolinherit.
 */
bool
is_member_of_role(Oid member, Oid role)
{
	/* Fast path for simple case */
	if (member == role)
		return true;

	/* Superusers have every privilege, so are part of every role */
	if (superuser_arg(member))
		return true;

	/*
	 * Find all the roles that member is a member of, including multi-level
	 * recursion, then see if target role is any one of them.
	 */
	return list_member_oid(roles_is_member_of(member, ROLERECURSE_MEMBERS,
											  InvalidOid, NULL),
						   role);
}

/*
 * check_is_member_of_role
 *		is_member_of_role with a standard permission-violation error if not
 */
void
check_is_member_of_role(Oid member, Oid role)
{
	if (!is_member_of_role(member, role))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be member of role \"%s\"",
						GetUserNameFromId(role, false))));
}

/*
 * Is member a member of role, not considering superuserness?
 *
 * This is identical to is_member_of_role except we ignore superuser
 * status.
 */
bool
is_member_of_role_nosuper(Oid member, Oid role)
{
	/* Fast path for simple case */
	if (member == role)
		return true;

	/*
	 * Find all the roles that member is a member of, including multi-level
	 * recursion, then see if target role is any one of them.
	 */
	return list_member_oid(roles_is_member_of(member, ROLERECURSE_MEMBERS,
											  InvalidOid, NULL),
						   role);
}


/*
 * Is member an admin of role?	That is, is member the role itself (subject to
 * restrictions below), a member (directly or indirectly) WITH ADMIN OPTION,
 * or a superuser?
 */
bool
is_admin_of_role(Oid member, Oid role)
{
	bool		result = false;

	if (superuser_arg(member))
		return true;

	if (member == role)

		/*
		 * A role can admin itself when it matches the session user and we're
		 * outside any security-restricted operation, SECURITY DEFINER or
		 * similar context.  SQL-standard roles cannot self-admin.  However,
		 * SQL-standard users are distinct from roles, and they are not
		 * grantable like roles: PostgreSQL's role-user duality extends the
		 * standard.  Checking for a session user match has the effect of
		 * letting a role self-admin only when it's conspicuously behaving
		 * like a user.  Note that allowing self-admin under a mere SET ROLE
		 * would make WITH ADMIN OPTION largely irrelevant; any member could
		 * SET ROLE to issue the otherwise-forbidden command.
		 *
		 * Withholding self-admin in a security-restricted operation prevents
		 * object owners from harnessing the session user identity during
		 * administrative maintenance.  Suppose Alice owns a database, has
		 * issued "GRANT alice TO bob", and runs a daily ANALYZE.  Bob creates
		 * an alice-owned SECURITY DEFINER function that issues "REVOKE alice
		 * FROM carol".  If he creates an expression index calling that
		 * function, Alice will attempt the REVOKE during each ANALYZE.
		 * Checking InSecurityRestrictedOperation() thwarts that attack.
		 *
		 * Withholding self-admin in SECURITY DEFINER functions makes their
		 * behavior independent of the calling user.  There's no security or
		 * SQL-standard-conformance need for that restriction, though.
		 *
		 * A role cannot have actual WITH ADMIN OPTION on itself, because that
		 * would imply a membership loop.  Therefore, we're done either way.
		 */
		return member == GetSessionUserId() &&
			!InLocalUserIdChange() && !InSecurityRestrictedOperation();

	(void) roles_is_member_of(member, ROLERECURSE_MEMBERS, role, &result);
	return result;
}


/* does what it says ... */
static int
count_one_bits(AclMode mask)
{
	int			nbits = 0;

	/* this code relies on AclMode being an unsigned type */
	while (mask)
	{
		if (mask & 1)
			nbits++;
		mask >>= 1;
	}
	return nbits;
}


/*
 * Select the effective grantor ID for a GRANT or REVOKE operation.
 *
 * The grantor must always be either the object owner or some role that has
 * been explicitly granted grant options.  This ensures that all granted
 * privileges appear to flow from the object owner, and there are never
 * multiple "original sources" of a privilege.  Therefore, if the would-be
 * grantor is a member of a role that has the needed grant options, we have
 * to do the grant as that role instead.
 *
 * It is possible that the would-be grantor is a member of several roles
 * that have different subsets of the desired grant options, but no one
 * role has 'em all.  In this case we pick a role with the largest number
 * of desired options.  Ties are broken in favor of closer ancestors.
 *
 * roleId: the role attempting to do the GRANT/REVOKE
 * privileges: the privileges to be granted/revoked
 * acl: the ACL of the object in question
 * ownerId: the role owning the object in question
 * *grantorId: receives the OID of the role to do the grant as
 * *grantOptions: receives the grant options actually held by grantorId
 *
 * If no grant options exist, we set grantorId to roleId, grantOptions to 0.
 */
void
select_best_grantor(Oid roleId, AclMode privileges,
					const Acl *acl, Oid ownerId,
					Oid *grantorId, AclMode *grantOptions)
{
	AclMode		needed_goptions = ACL_GRANT_OPTION_FOR(privileges);
	List	   *roles_list;
	int			nrights;
	ListCell   *l;

	/*
	 * The object owner is always treated as having all grant options, so if
	 * roleId is the owner it's easy.  Also, if roleId is a superuser it's
	 * easy: superusers are implicitly members of every role, so they act as
	 * the object owner.
	 */
	if (roleId == ownerId || superuser_arg(roleId))
	{
		*grantorId = ownerId;
		*grantOptions = needed_goptions;
		return;
	}

	/*
	 * Otherwise we have to do a careful search to see if roleId has the
	 * privileges of any suitable role.  Note: we can hang onto the result of
	 * roles_is_member_of() throughout this loop, because aclmask_direct()
	 * doesn't query any role memberships.
	 */
	roles_list = roles_is_member_of(roleId, ROLERECURSE_PRIVS,
									InvalidOid, NULL);

	/* initialize candidate result as default */
	*grantorId = roleId;
	*grantOptions = ACL_NO_RIGHTS;
	nrights = 0;

	foreach(l, roles_list)
	{
		Oid			otherrole = lfirst_oid(l);
		AclMode		otherprivs;

		otherprivs = aclmask_direct(acl, otherrole, ownerId,
									needed_goptions, ACLMASK_ALL);
		if (otherprivs == needed_goptions)
		{
			/* Found a suitable grantor */
			*grantorId = otherrole;
			*grantOptions = otherprivs;
			return;
		}

		/*
		 * If it has just some of the needed privileges, remember best
		 * candidate.
		 */
		if (otherprivs != ACL_NO_RIGHTS)
		{
			int			nnewrights = count_one_bits(otherprivs);

			if (nnewrights > nrights)
			{
				*grantorId = otherrole;
				*grantOptions = otherprivs;
				nrights = nnewrights;
			}
		}
	}
}

/*
 * get_role_oid - Given a role name, look up the role's OID.
 *
 * If missing_ok is false, throw an error if role name not found.  If
 * true, just return InvalidOid.
 */
Oid
get_role_oid(const char *rolname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(AUTHNAME, Anum_pg_authid_oid,
						  CStringGetDatum(rolname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role \"%s\" does not exist", rolname)));
	return oid;
}

/*
 * Given a RoleSpec node, return the OID it corresponds to.  If missing_ok is
 * true, return InvalidOid if the role does not exist.
 *
 * PUBLIC is always disallowed here.  Routines wanting to handle the PUBLIC
 * case must check the case separately.
 */
Oid
get_rolespec_oid(const RoleSpec *role, bool missing_ok)
{
	Oid			oid;

	switch (role->roletype)
	{
		case ROLESPEC_CSTRING:
			Assert(role->rolename);
			oid = get_role_oid(role->rolename, missing_ok);
			break;

		case ROLESPEC_CURRENT_ROLE:
		case ROLESPEC_CURRENT_USER:
			oid = GetUserId();
			break;

		case ROLESPEC_SESSION_USER:
			oid = GetSessionUserId();
			break;

		case ROLESPEC_PUBLIC:
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("role \"%s\" does not exist", "public")));
			oid = InvalidOid;	/* make compiler happy */
			break;

		default:
			elog(ERROR, "unexpected role type %d", role->roletype);
	}

	return oid;
}

/*
 * Given a RoleSpec node, return the pg_authid HeapTuple it corresponds to.
 * Caller must ReleaseSysCache when done with the result tuple.
 */
HeapTuple
get_rolespec_tuple(const RoleSpec *role)
{
	HeapTuple	tuple;

	switch (role->roletype)
	{
		case ROLESPEC_CSTRING:
			Assert(role->rolename);
			tuple = SearchSysCache1(AUTHNAME, CStringGetDatum(role->rolename));
			if (!HeapTupleIsValid(tuple))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role \"%s\" does not exist", role->rolename)));
			break;

		case ROLESPEC_CURRENT_ROLE:
		case ROLESPEC_CURRENT_USER:
			tuple = SearchSysCache1(AUTHOID, GetUserId());
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for role %u", GetUserId());
			break;

		case ROLESPEC_SESSION_USER:
			tuple = SearchSysCache1(AUTHOID, GetSessionUserId());
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for role %u", GetSessionUserId());
			break;

		case ROLESPEC_PUBLIC:
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("role \"%s\" does not exist", "public")));
			tuple = NULL;		/* make compiler happy */
			break;

		default:
			elog(ERROR, "unexpected role type %d", role->roletype);
	}

	return tuple;
}

/*
 * Given a RoleSpec, returns a palloc'ed copy of the corresponding role's name.
 */
char *
get_rolespec_name(const RoleSpec *role)
{
	HeapTuple	tp;
	Form_pg_authid authForm;
	char	   *rolename;

	tp = get_rolespec_tuple(role);
	authForm = (Form_pg_authid) GETSTRUCT(tp);
	rolename = pstrdup(NameStr(authForm->rolname));
	ReleaseSysCache(tp);

	return rolename;
}

/*
 * Given a RoleSpec, throw an error if the name is reserved, using detail_msg,
 * if provided (which must be already translated).
 *
 * If node is NULL, no error is thrown.  If detail_msg is NULL then no detail
 * message is provided.
 */
void
check_rolespec_name(const RoleSpec *role, const char *detail_msg)
{
	if (!role)
		return;

	if (role->roletype != ROLESPEC_CSTRING)
		return;

	if (IsReservedName(role->rolename))
	{
		if (detail_msg)
			ereport(ERROR,
					(errcode(ERRCODE_RESERVED_NAME),
					 errmsg("role name \"%s\" is reserved",
							role->rolename),
					 errdetail_internal("%s", detail_msg)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_RESERVED_NAME),
					 errmsg("role name \"%s\" is reserved",
							role->rolename)));
	}
}
