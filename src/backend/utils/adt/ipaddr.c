/*-------------------------------------------------------------------------
 *
 * ipaddr.c
 *	  Miscellaneous IP address utility routines shared by network and
 *	  connection-handling code.  (Split out from network.c when inet/cidr
 *	  types were removed, because clean_ipv6_addr is still used by hba.c
 *	  and pgstatfuncs.c.)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/utils/adt/ipaddr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "libpq/ifaddr.h"
#include "utils/builtins.h"


/*
 * clean_ipv6_addr --- remove any '%zone' part from an IPv6 address string
 *
 * XXX This should go away someday!
 *
 * This is a kluge needed because we don't yet support zones in stored inet
 * values.  Since the result of getnameinfo() might include a zone spec,
 * call this to remove it anywhere we want to feed getnameinfo's output to
 * network_in.  Beats failing entirely.
 *
 * An alternative approach would be to let network_in ignore %-parts for
 * itself, but that would mean we'd silently drop zone specs in user input,
 * which seems not such a good idea.
 */
void
clean_ipv6_addr(int addr_family, char *addr)
{
#ifdef HAVE_IPV6
	if (addr_family == AF_INET6)
	{
		char	   *pct = strchr(addr, '%');

		if (pct)
			*pct = '\0';
	}
#endif
}
