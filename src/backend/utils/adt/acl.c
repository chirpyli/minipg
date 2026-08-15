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


static void RoleMembershipCacheCallback(Datum arg, int cacheid, uint32 hashvalue);


/*
 * Test whether an identifier char can be left unquoted in ACLs.
 *
 * Formerly, we used isalnum() even on non-ASCII characters, resulting in
 * unportable behavior.  To ensure dump compatibility with old versions,
 * we now treat high-bit-set characters as always requiring quoting during
 * putid(), but getid() will always accept them without quotes.
 */
bool
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
