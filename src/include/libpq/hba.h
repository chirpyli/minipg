/*-------------------------------------------------------------------------
 *
 * hba.h
 *	  Interface to hba.c
 *
 *
 * src/include/libpq/hba.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HBA_H
#define HBA_H

#include "libpq/pqcomm.h"	/* pgrminclude ignore */	/* needed for NetBSD */
#include "nodes/pg_list.h"
#include "regex/regex.h"


/*
 * The following enum represents the authentication methods that
 * are supported by PostgreSQL.
 *
 * Note: keep this in sync with the UserAuthName array in hba.c.
 */
typedef enum UserAuth
{
	uaReject,
	uaImplicitReject,			/* Not a user-visible option */
	uaTrust,
	uaPassword,
	uaSCRAM
#define USER_AUTH_LAST uaSCRAM	/* Must be last value of this enum */
} UserAuth;

/*
 * Data structures representing pg_hba.conf entries
 */

typedef enum IPCompareMethod
{
	ipCmpMask,
	ipCmpSameHost,
	ipCmpSameNet,
	ipCmpAll
} IPCompareMethod;

typedef enum ConnType
{
	ctLocal,
	ctHost,
} ConnType;

typedef struct HbaLine
{
	int			linenumber;
	char	   *rawline;
	ConnType	conntype;
	List	   *databases;
	List	   *roles;
	struct sockaddr_storage addr;
	int			addrlen;		/* zero if we don't have a valid addr */
	struct sockaddr_storage mask;
	int			masklen;		/* zero if we don't have a valid mask */
	IPCompareMethod ip_cmp_method;
	char	   *hostname;
	UserAuth	auth_method;
	char	   *usermap;
} HbaLine;

typedef struct IdentLine
{
	int			linenumber;

	char	   *usermap;
	char	   *ident_user;
	char	   *pg_role;
	regex_t		re;
} IdentLine;

/* kluge to avoid including libpq/libpq-be.h here */
typedef struct Port hbaPort;

extern bool load_hba(void);
extern bool load_ident(void);
extern const char *hba_authname(UserAuth auth_method);
extern void hba_getauthmethod(hbaPort *port);
extern int	check_usermap(const char *usermap_name,
						  const char *pg_role, const char *auth_user,
						  bool case_insensitive);
extern bool pg_isblank(const char c);

#endif							/* HBA_H */
