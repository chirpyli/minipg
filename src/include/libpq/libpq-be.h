/*-------------------------------------------------------------------------
 *
 * libpq-be.h
 *	  This file contains definitions for structures and externs used
 *	  by the postmaster during client authentication.
 *
 *	  Note that this is backend-internal and is NOT exported to clients.
 *	  Structs that need to be client-visible are in pqcomm.h.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/libpq-be.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LIBPQ_BE_H
#define LIBPQ_BE_H

#include <sys/time.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif

#include "datatype/timestamp.h"
#include "libpq/pqcomm.h"
#include "nodes/pg_list.h"


typedef enum CAC_state
{
	CAC_OK,
	CAC_STARTUP,
	CAC_SHUTDOWN,
	CAC_RECOVERY,
	CAC_NOTCONSISTENT,
	CAC_TOOMANY,
	CAC_SUPERUSER
} CAC_state;


/*
 * This is used by the postmaster in its communication with frontends.  It
 * contains all state information needed during this communication before the
 * backend is run.  The Port structure is kept in malloc'd memory and is
 * still available when a backend is running (see MyProcPort).  The data
 * it points to must also be malloc'd, or else palloc'd in TopMemoryContext,
 * so that it survives into PostgresMain execution!
 *
 * remote_hostname is set if we did a successful reverse lookup of the
 * client's IP address during connection setup.
 * remote_hostname_resolv tracks the state of hostname verification:
 *	+1 = remote_hostname is known to resolve to client's IP address
 *	-1 = remote_hostname is known NOT to resolve to client's IP address
 *	 0 = we have not done the forward DNS lookup yet
 *	-2 = there was an error in name resolution
 * If reverse lookup of the client IP address fails, remote_hostname will be
 * left NULL while remote_hostname_resolv is set to -2.  If reverse lookup
 * succeeds but forward lookup fails, remote_hostname_resolv is also set to -2
 * (the case is distinguishable because remote_hostname isn't NULL).  In
 * either of the -2 cases, remote_hostname_errcode saves the lookup return
 * code for possible later use with gai_strerror.
 */

typedef struct Port
{
	pgsocket	sock;			/* File descriptor */
	bool		noblock;		/* is the socket in non-blocking mode? */
	ProtocolVersion proto;		/* FE/BE protocol version */
	SockAddr	laddr;			/* local addr (postmaster) */
	SockAddr	raddr;			/* remote addr (client) */
	char	   *remote_host;	/* name (or ip addr) of remote host */
	char	   *remote_hostname;	/* name (not ip addr) of remote host, if
									 * available */
	int			remote_hostname_resolv; /* see above */
	int			remote_hostname_errcode;	/* see above */
	char	   *remote_port;	/* text rep of remote port */
	CAC_state	canAcceptConnections;	/* postmaster connection status */

	/*
	 * Information that needs to be saved from the startup packet and passed
	 * into backend execution.  "char *" fields are NULL if not set.
	 * guc_options points to a List of alternating option names and values.
	 */
	char	   *database_name;
	char	   *user_name;
	char	   *cmdline_options;
	List	   *guc_options;

	/*
	 * The startup packet application name, only used here for the "connection
	 * authorized" log message. We shouldn't use this post-startup, instead
	 * the GUC should be used as application can change it afterward.
	 */
	char	   *application_name;

	/*
	 * Authenticated identity.  The meaning of this identifier is dependent on
	 * hba->auth_method; it is the identity (if any) that the user presented
	 * during the authentication cycle, before they were assigned a database
	 * role.  (It is effectively the "SYSTEM-USERNAME" of a pg_ident usermap
	 * -- though the exact string in use may be different, depending on pg_hba
	 * options.)
	 *
	 * authn_id is NULL if the user has not actually been authenticated, for
	 * example if the "trust" auth method is in use.
	 */
	const char *authn_id;

	/*
	 * TCP keepalive and user timeout settings.
	 *
	 * default values are 0 if AF_UNIX or not yet known; current values are 0
	 * if AF_UNIX or using the default. Also, -1 in a default value means we
	 * were unable to find out the default (getsockopt failed).
	 */
	int			default_keepalives_idle;
	int			default_keepalives_interval;
	int			default_keepalives_count;
	int			default_tcp_user_timeout;
	int			keepalives_idle;
	int			keepalives_interval;
	int			keepalives_count;
	int			tcp_user_timeout;

} Port;


extern ProtocolVersion FrontendProtocol;

/* TCP keepalives configuration. These are no-ops on an AF_UNIX socket. */

extern int	pq_getkeepalivesidle(Port *port);
extern int	pq_getkeepalivesinterval(Port *port);
extern int	pq_getkeepalivescount(Port *port);
extern int	pq_gettcpusertimeout(Port *port);

extern int	pq_setkeepalivesidle(int idle, Port *port);
extern int	pq_setkeepalivesinterval(int interval, Port *port);
extern int	pq_setkeepalivescount(int count, Port *port);
extern int	pq_settcpusertimeout(int timeout, Port *port);

#endif							/* LIBPQ_BE_H */
