/*-------------------------------------------------------------------------
 *
 * ip.c
 *	  IPv6-aware network access.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/ip.c
 *
 * This file and the IPV6 implementation were initially provided by
 * Nigel Kukard <nkukard@lbsd.net>, Linux Based Systems Design
 * http://www.lbsd.net.
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <arpa/inet.h>
#include <sys/file.h>

#include "common/ip.h"




/*
 *	pg_getaddrinfo_all - get address info for Unix, IPv4 and IPv6 sockets
 */
int
pg_getaddrinfo_all(const char *hostname, const char *servname,
				   const struct addrinfo *hintp, struct addrinfo **result)
{
	int			rc;

	/* not all versions of getaddrinfo() zero *result on failure */
	*result = NULL;

	/* NULL has special meaning to getaddrinfo(). */
	rc = getaddrinfo((!hostname || hostname[0] == '\0') ? NULL : hostname,
					 servname, hintp, result);

	return rc;
}


/*
 *	pg_freeaddrinfo_all - free addrinfo structures for IPv4, IPv6, or Unix
 *
 * Note: the ai_family field of the original hint structure must be passed
 * so that we can tell whether the addrinfo struct was built by the system's
 * getaddrinfo() routine or our own getaddrinfo_unix() routine.  Some versions
 * of getaddrinfo() might be willing to return AF_UNIX addresses, so it's
 * not safe to look at ai_family in the addrinfo itself.
 */
void
pg_freeaddrinfo_all(int hint_ai_family, struct addrinfo *ai)
{
	/* struct was built by getaddrinfo() */
	if (ai != NULL)
		freeaddrinfo(ai);
}


/*
 *	pg_getnameinfo_all - get name info for Unix, IPv4 and IPv6 sockets
 *
 * The API of this routine differs from the standard getnameinfo() definition
 * in two ways: first, the addr parameter is declared as sockaddr_storage
 * rather than struct sockaddr, and second, the node and service fields are
 * guaranteed to be filled with something even on failure return.
 */
int
pg_getnameinfo_all(const struct sockaddr_storage *addr, int salen,
				   char *node, int nodelen,
				   char *service, int servicelen,
				   int flags)
{
	int			rc;

	rc = getnameinfo((const struct sockaddr *) addr, salen,
					 node, nodelen,
					 service, servicelen,
					 flags);

	if (rc != 0)
	{
		if (node)
			strlcpy(node, "???", nodelen);
		if (service)
			strlcpy(service, "???", servicelen);
	}

	return rc;
}

