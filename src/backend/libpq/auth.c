/*-------------------------------------------------------------------------
 *
 * auth.c
 *	  Routines to handle network authentication
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/auth.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/param.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "libpq/auth.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

/*----------------------------------------------------------------
 * Global authentication functions
 *----------------------------------------------------------------
 */
static void sendAuthRequest(Port *port, AuthRequest areq, const char *extradata,
							int extralen);


/*
 * Maximum accepted size of ordinary password packet lengths.
 */
#define PG_MAX_AUTH_TOKEN_LENGTH	65535

/*----------------------------------------------------------------
 * Global authentication functions
 *----------------------------------------------------------------
 */

/*
 * Client authentication starts here.  If there is an error, this
 * function does not return and the backend process is terminated.
 */
void
ClientAuthentication(Port *port)
{
	/*
	 * minipg: 已移除基于主机的访问控制（pg_hba.conf / pg_ident.conf）。所有入站
	 * 连接均被无条件信任放行，不做 IP/数据库/角色规则匹配，也不进行密码验证。
	 * 这使得 pg_hba 规则解析与匹配子系统可以整体删除，连接路径更短。
	 */
	CHECK_FOR_INTERRUPTS();

	sendAuthRequest(port, AUTH_REQ_OK, NULL, 0);

	/*
	 * 认证已成功完成，复位全局标志，使后续 NOTICE/WARNING 等级的消息
	 * 能够正常发往客户端（should_output_to_client 依赖此标志）。
	 */
	ClientAuthInProgress = false;
}


/*
 * Send an authentication request packet to the frontend.
 */
static void
sendAuthRequest(Port *port, AuthRequest areq, const char *extradata, int extralen)
{
	StringInfoData buf;

	CHECK_FOR_INTERRUPTS();

	pq_beginmessage(&buf, 'R');
	pq_sendint32(&buf, (int32) areq);
	if (extralen > 0)
		pq_sendbytes(&buf, extradata, extralen);

	pq_endmessage(&buf);

	/*
	 * Flush message so client will see it, except for AUTH_REQ_OK, which
	 * need not be sent until we are ready for queries.
	 */
	if (areq != AUTH_REQ_OK)
		pq_flush();

	CHECK_FOR_INTERRUPTS();
}