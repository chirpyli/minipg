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
	CAC_TOOMANY,
	CAC_SUPERUSER
} CAC_state;


/*
 * 该结构由 postmaster 在其与前端的通信中使用。它包含在后端运行之前，这次
 * 通信过程中所需的全部状态信息。Port 结构保存在 malloc 分配的内存中，并且
 * 在后端运行期间仍然可用（参见 MyProcPort）。它所指向的数据也必须是 malloc
 * 分配的，或者在 TopMemoryContext 中用 palloc 分配，以便它能存活到
 * PostgresMain 执行期间！
 *
 * 如果在连接建立期间成功地对客户端的 IP 地址做了反向查询，就会设置
 * remote_hostname。
 * remote_hostname_resolv 跟踪主机名验证的状态：
 *	+1 = 已知 remote_hostname 能解析到客户端的 IP 地址
 *	-1 = 已知 remote_hostname *不能*解析到客户端的 IP 地址
 *	 0 = 我们尚未做正向 DNS 查询
 *	-2 = 名称解析时出错
 * 如果对客户端 IP 地址的反向查询失败，remote_hostname 将保持为 NULL，而
 * remote_hostname_resolv 被设为 -2。如果反向查询成功但正向查询失败，
 * remote_hostname_resolv 同样被设为 -2（这两种情况可以区分，因为前一种情况下
 * remote_hostname 不是 NULL）。在上述两种 -2 情况下，remote_hostname_errcode
 * 保存着查询的返回码，以便日后可能配合 gai_strerror 使用。
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
