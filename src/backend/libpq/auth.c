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

#include "commands/user.h"
#include "common/ip.h"
#include "common/scram-common.h"
#include "libpq/auth.h"
#include "libpq/crypt.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/scram.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
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
static void auth_failed(Port *port, int status, char *logdetail);
static char *recv_password_packet(Port *port);
static void set_authn_id(Port *port, const char *id);


/*----------------------------------------------------------------
 * Password-based authentication methods (password and scram-sha-256)
 *----------------------------------------------------------------
 */
static int	CheckPasswordAuth(Port *port, char **logdetail);
static int	CheckPWChallengeAuth(Port *port, char **logdetail);

static int	CheckSCRAMAuth(Port *port, char *shadow_pass, char **logdetail);


/*
 * Maximum accepted size of ordinary password packet lengths.
 */
#define PG_MAX_AUTH_TOKEN_LENGTH	65535

/*
 * Maximum accepted size of SASL messages.
 *
 * The messages that the server or libpq generate are much smaller than this,
 * but have some headroom.
 */
#define PG_MAX_SASL_MESSAGE_LENGTH	1024

/*----------------------------------------------------------------
 * Global authentication functions
 *----------------------------------------------------------------
 */

/*
 * This hook allows plugins to get control following client authentication,
 * but before the user has been informed about the results.  It could be used
 * to record login events, insert a delay after failed authentication, etc.
 */
ClientAuthentication_hook_type ClientAuthentication_hook = NULL;

/*
 * Tell the user the authentication failed, but not (much about) why.
 *
 * There is a tradeoff here between security concerns and making life
 * unnecessarily difficult for legitimate users.  We would not, for example,
 * want to report the password we were expecting to receive...
 * But it seems useful to report the username and authorization method
 * in use, and these are items that must be presumed known to an attacker
 * anyway.
 * Note that many sorts of failure report additional information in the
 * postmaster log, which we hope is only readable by good guys.  In
 * particular, if logdetail isn't NULL, we send that string to the log.
 */
static void
auth_failed(Port *port, int status, char *logdetail)
{
	const char *errstr;
	char	   *cdetail;
	int			errcode_return = ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION;

	/*
	 * If we failed due to EOF from client, just quit; there's no point in
	 * trying to send a message to the client, and not much point in logging
	 * the failure in the postmaster log.  (Logging the failure might be
	 * desirable, were it not for the fact that libpq closes the connection
	 * unceremoniously if challenged for a password when it hasn't got one to
	 * send.  We'll get a useless log entry for every psql connection under
	 * password auth, even if it's perfectly successful, if we log STATUS_EOF
	 * events.)
	 */
	if (status == STATUS_EOF)
		proc_exit(0);

	switch (port->hba->auth_method)
	{
		case uaReject:
		case uaImplicitReject:
			errstr = gettext_noop("authentication failed for user \"%s\": host rejected");
			break;
		case uaTrust:
			errstr = gettext_noop("\"trust\" authentication failed for user \"%s\"");
			break;
		case uaPassword:
		case uaSCRAM:
			errstr = gettext_noop("password authentication failed for user \"%s\"");
			/* We use it to indicate if a .pgpass password failed. */
			errcode_return = ERRCODE_INVALID_PASSWORD;
			break;
		default:
			errstr = gettext_noop("authentication failed for user \"%s\": invalid authentication method");
			break;
	}

	cdetail = psprintf(_("Connection matched pg_hba.conf line %d: \"%s\""),
					   port->hba->linenumber, port->hba->rawline);
	if (logdetail)
		logdetail = psprintf("%s\n%s", logdetail, cdetail);
	else
		logdetail = cdetail;

	ereport(FATAL,
			(errcode(errcode_return),
			 errmsg(errstr, port->user_name),
			 logdetail ? errdetail_log("%s", logdetail) : 0));

	/* doesn't return */
}


/*
 * Sets the authenticated identity for the current user.  The provided string
 * will be copied into the TopMemoryContext.  The ID will be logged if
 * log_connections is enabled.
 *
 * Auth methods should call this routine exactly once, as soon as the user is
 * successfully authenticated, even if they have reasons to know that
 * authorization will fail later.
 *
 * The provided string will be copied into TopMemoryContext, to match the
 * lifetime of the Port, so it is safe to pass a string that is managed by an
 * external library.
 */
static void
set_authn_id(Port *port, const char *id)
{
	Assert(id);

	if (port->authn_id)
	{
		/*
		 * An existing authn_id should never be overwritten; that means two
		 * authentication providers are fighting (or one is fighting itself).
		 * Don't leak any authn details to the client, but don't let the
		 * connection continue, either.
		 */
		ereport(FATAL,
				(errmsg("authentication identifier set more than once"),
				 errdetail_log("previous identifier: \"%s\"; new identifier: \"%s\"",
							   port->authn_id, id)));
	}

	port->authn_id = MemoryContextStrdup(TopMemoryContext, id);

	if (Log_connections)
	{
		ereport(LOG,
				errmsg("connection authenticated: identity=\"%s\" method=%s "
					   "(%s:%d)",
					   port->authn_id, hba_authname(port->hba->auth_method), HbaFileName,
					   port->hba->linenumber));
	}
}


/*
 * Client authentication starts here.  If there is an error, this
 * function does not return and the backend process is terminated.
 */
void
ClientAuthentication(Port *port)
{
	int			status = STATUS_ERROR;
	char	   *logdetail = NULL;

	/*
	 * Get the authentication method to use for this frontend/database
	 * combination.  Note: we do not parse the file at this point; this has
	 * already been done elsewhere.  hba.c dropped an error message into the
	 * server logfile if parsing the hba config file failed.
	 */
	hba_getauthmethod(port);

	CHECK_FOR_INTERRUPTS();

	/*
	 * Now proceed to do the actual authentication check
	 */
	switch (port->hba->auth_method)
	{
		case uaReject:

			/*
			 * An explicit "reject" entry in pg_hba.conf.  This report exposes
			 * the fact that there's an explicit reject entry, which is
			 * perhaps not so desirable from a security standpoint; but the
			 * message for an implicit reject could confuse the DBA a lot when
			 * the true situation is a match to an explicit reject.  And we
			 * don't want to change the message for an implicit reject.  As
			 * noted below, the additional information shown here doesn't
			 * expose anything not known to an attacker.
			 */
			{
				char		hostinfo[NI_MAXHOST];
				const char *encryption_state;

				pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								   hostinfo, sizeof(hostinfo),
								   NULL, 0,
								   NI_NUMERICHOST);

				encryption_state = _("no encryption");

				if (am_walsender && !am_db_walsender)
					ereport(FATAL,
							(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					/* translator: last %s describes encryption state */
							 errmsg("pg_hba.conf rejects replication connection for host \"%s\", user \"%s\", %s",
									hostinfo, port->user_name,
									encryption_state)));
				else
					ereport(FATAL,
							(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					/* translator: last %s describes encryption state */
							 errmsg("pg_hba.conf rejects connection for host \"%s\", user \"%s\", database \"%s\", %s",
									hostinfo, port->user_name,
									port->database_name,
									encryption_state)));
				break;
			}

		case uaImplicitReject:

			/*
			 * No matching entry, so tell the user we fell through.
			 *
			 * NOTE: the extra info reported here is not a security breach,
			 * because all that info is known at the frontend and must be
			 * assumed known to bad guys.  We're merely helping out the less
			 * clueful good guys.
			 */
			{
				char		hostinfo[NI_MAXHOST];
				const char *encryption_state;

				pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								   hostinfo, sizeof(hostinfo),
								   NULL, 0,
								   NI_NUMERICHOST);

				encryption_state = _("no encryption");

#define HOSTNAME_LOOKUP_DETAIL(port) \
				(port->remote_hostname ? \
				 (port->remote_hostname_resolv == +1 ? \
				  errdetail_log("Client IP address resolved to \"%s\", forward lookup matches.", \
								port->remote_hostname) : \
				  port->remote_hostname_resolv == 0 ? \
				  errdetail_log("Client IP address resolved to \"%s\", forward lookup not checked.", \
								port->remote_hostname) : \
				  port->remote_hostname_resolv == -1 ? \
				  errdetail_log("Client IP address resolved to \"%s\", forward lookup does not match.", \
								port->remote_hostname) : \
				  port->remote_hostname_resolv == -2 ? \
				  errdetail_log("Could not translate client host name \"%s\" to IP address: %s.", \
								port->remote_hostname, \
								gai_strerror(port->remote_hostname_errcode)) : \
				  0) \
				 : (port->remote_hostname_resolv == -2 ? \
					errdetail_log("Could not resolve client IP address to a host name: %s.", \
								  gai_strerror(port->remote_hostname_errcode)) : \
					0))

				if (am_walsender && !am_db_walsender)
					ereport(FATAL,
							(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					/* translator: last %s describes encryption state */
							 errmsg("no pg_hba.conf entry for replication connection from host \"%s\", user \"%s\", %s",
									hostinfo, port->user_name,
									encryption_state),
							 HOSTNAME_LOOKUP_DETAIL(port)));
				else
					ereport(FATAL,
							(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					/* translator: last %s describes encryption state */
							 errmsg("no pg_hba.conf entry for host \"%s\", user \"%s\", database \"%s\", %s",
									hostinfo, port->user_name,
									port->database_name,
									encryption_state),
							 HOSTNAME_LOOKUP_DETAIL(port)));
				break;
			}

		case uaSCRAM:
			status = CheckPWChallengeAuth(port, &logdetail);
			break;

		case uaPassword:
			status = CheckPasswordAuth(port, &logdetail);
			break;

		case uaTrust:
			status = STATUS_OK;
			break;
	}

	if (ClientAuthentication_hook)
		(*ClientAuthentication_hook) (port, status);

	if (status == STATUS_OK)
		sendAuthRequest(port, AUTH_REQ_OK, NULL, 0);
	else
		auth_failed(port, status, logdetail);
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
	 * Flush message so client will see it, except for AUTH_REQ_OK and
	 * AUTH_REQ_SASL_FIN, which need not be sent until we are ready for
	 * queries.
	 */
	if (areq != AUTH_REQ_OK && areq != AUTH_REQ_SASL_FIN)
		pq_flush();

	CHECK_FOR_INTERRUPTS();
}

/*
 * Collect password response packet from frontend.
 *
 * Returns NULL if couldn't get password, else palloc'd string.
 */
static char *
recv_password_packet(Port *port)
{
	StringInfoData buf;
	int			mtype;

	pq_startmsgread();

	/* Expect 'p' message type */
	mtype = pq_getbyte();
	if (mtype != 'p')
	{
		/*
		 * If the client just disconnects without offering a password, don't
		 * make a log entry.  This is legal per protocol spec and in fact
		 * commonly done by psql, so complaining just clutters the log.
		 */
		if (mtype != EOF)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("expected password response, got message type %d",
							mtype)));
		return NULL;			/* EOF or bad message type */
	}

	initStringInfo(&buf);
	if (pq_getmessage(&buf, PG_MAX_AUTH_TOKEN_LENGTH))	/* receive password */
	{
		/* EOF - pq_getmessage already logged a suitable message */
		pfree(buf.data);
		return NULL;
	}

	/*
	 * Apply sanity check: password packet length should agree with length of
	 * contained string.  Note it is safe to use strlen here because
	 * StringInfo is guaranteed to have an appended '\0'.
	 */
	if (strlen(buf.data) + 1 != buf.len)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid password packet size")));

	/*
	 * Don't allow an empty password. Libpq treats an empty password the same
	 * as no password at all, and won't even try to authenticate. But other
	 * clients might, so allowing it would be confusing.
	 *
	 * Note that this only catches an empty password sent by the client in
	 * plaintext. There's also a check in CREATE/ALTER USER that prevents an
	 * empty string from being stored as a user's password in the first place.
	 */
	if (buf.len == 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PASSWORD),
				 errmsg("empty password returned by client")));

	/* Do not echo password to logs, for security. */
	elog(DEBUG5, "received password packet");

	/*
	 * Return the received string.  Note we do not attempt to do any
	 * character-set conversion on it; since we don't yet know the client's
	 * encoding, there wouldn't be much point.
	 */
	return buf.data;
}


/*----------------------------------------------------------------
 * Password-based authentication mechanisms
 *----------------------------------------------------------------
 */

/*
 * Plaintext password authentication.
 */
static int
CheckPasswordAuth(Port *port, char **logdetail)
{
	char	   *passwd;
	int			result;
	char	   *shadow_pass;

	sendAuthRequest(port, AUTH_REQ_PASSWORD, NULL, 0);

	passwd = recv_password_packet(port);
	if (passwd == NULL)
		return STATUS_EOF;		/* client wouldn't send password */

	shadow_pass = get_role_password(port->user_name, logdetail);
	if (shadow_pass)
	{
		result = plain_crypt_verify(port->user_name, shadow_pass, passwd,
									logdetail);
	}
	else
		result = STATUS_ERROR;

	if (shadow_pass)
		pfree(shadow_pass);
	pfree(passwd);

	if (result == STATUS_OK)
		set_authn_id(port, port->user_name);

	return result;
}

/*
 * SCRAM authentication.
 */
static int
CheckPWChallengeAuth(Port *port, char **logdetail)
{
	int			auth_result;
	char	   *shadow_pass;

	Assert(port->hba->auth_method == uaSCRAM);

	/* First look up the user's password. */
	shadow_pass = get_role_password(port->user_name, logdetail);

	/*
	 * If the user does not exist, or has no password or it's expired, we
	 * still go through the motions of authentication, to avoid revealing to
	 * the client that the user didn't exist.  If the user had an MD5
	 * password, CheckSCRAMAuth() will fail.
	 */
	auth_result = CheckSCRAMAuth(port, shadow_pass, logdetail);

	if (shadow_pass)
		pfree(shadow_pass);
	else
	{
		/*
		 * If get_role_password() returned error, authentication better not
		 * have succeeded.
		 */
		Assert(auth_result != STATUS_OK);
	}

	if (auth_result == STATUS_OK)
		set_authn_id(port, port->user_name);

	return auth_result;
}

static int
CheckSCRAMAuth(Port *port, char *shadow_pass, char **logdetail)
{
	StringInfoData sasl_mechs;
	int			mtype;
	StringInfoData buf;
	void	   *scram_opaq = NULL;
	char	   *output = NULL;
	int			outputlen = 0;
	const char *input;
	int			inputlen;
	int			result;
	bool		initial;

	/*
	 * Send the SASL authentication request to user.  It includes the list of
	 * authentication mechanisms that are supported.
	 */
	initStringInfo(&sasl_mechs);

	pg_be_scram_get_mechanisms(port, &sasl_mechs);
	/* Put another '\0' to mark that list is finished. */
	appendStringInfoChar(&sasl_mechs, '\0');

	sendAuthRequest(port, AUTH_REQ_SASL, sasl_mechs.data, sasl_mechs.len);
	pfree(sasl_mechs.data);

	/*
	 * Loop through SASL message exchange.  This exchange can consist of
	 * multiple messages sent in both directions.  First message is always
	 * from the client.  All messages from client to server are password
	 * packets (type 'p').
	 */
	initial = true;
	do
	{
		pq_startmsgread();
		mtype = pq_getbyte();
		if (mtype != 'p')
		{
			/* Only log error if client didn't disconnect. */
			if (mtype != EOF)
			{
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("expected SASL response, got message type %d",
								mtype)));
			}
			else
				return STATUS_EOF;
		}

		/* Get the actual SASL message */
		initStringInfo(&buf);
		if (pq_getmessage(&buf, PG_MAX_SASL_MESSAGE_LENGTH))
		{
			/* EOF - pq_getmessage already logged error */
			pfree(buf.data);
			return STATUS_ERROR;
		}

		elog(DEBUG4, "processing received SASL response of length %d", buf.len);

		/*
		 * The first SASLInitialResponse message is different from the others.
		 * It indicates which SASL mechanism the client selected, and contains
		 * an optional Initial Client Response payload.  The subsequent
		 * SASLResponse messages contain just the SASL payload.
		 */
		if (initial)
		{
			const char *selected_mech;

			selected_mech = pq_getmsgrawstring(&buf);

			/*
			 * Initialize the status tracker for message exchanges.
			 *
			 * If the user doesn't exist, or doesn't have a valid password, or
			 * it's expired, we still go through the motions of SASL
			 * authentication, but tell the authentication method that the
			 * authentication is "doomed". That is, it's going to fail, no
			 * matter what.
			 *
			 * This is because we don't want to reveal to an attacker what
			 * usernames are valid, nor which users have a valid password.
			 */
			scram_opaq = pg_be_scram_init(port, selected_mech, shadow_pass);

			inputlen = pq_getmsgint(&buf, 4);
			if (inputlen == -1)
				input = NULL;
			else
				input = pq_getmsgbytes(&buf, inputlen);

			initial = false;
		}
		else
		{
			inputlen = buf.len;
			input = pq_getmsgbytes(&buf, buf.len);
		}
		pq_getmsgend(&buf);

		/*
		 * The StringInfo guarantees that there's a \0 byte after the
		 * response.
		 */
		Assert(input == NULL || input[inputlen] == '\0');

		/*
		 * we pass 'logdetail' as NULL when doing a mock authentication,
		 * because we should already have a better error message in that case
		 */
		result = pg_be_scram_exchange(scram_opaq, input, inputlen,
									  &output, &outputlen,
									  logdetail);

		/* input buffer no longer used */
		pfree(buf.data);

		if (output)
		{
			/*
			 * Negotiation generated data to be sent to the client.
			 */
			elog(DEBUG4, "sending SASL challenge of length %u", outputlen);

			if (result == SASL_EXCHANGE_SUCCESS)
				sendAuthRequest(port, AUTH_REQ_SASL_FIN, output, outputlen);
			else
				sendAuthRequest(port, AUTH_REQ_SASL_CONT, output, outputlen);

			pfree(output);
		}
	} while (result == SASL_EXCHANGE_CONTINUE);

	/* Oops, Something bad happened */
	if (result != SASL_EXCHANGE_SUCCESS)
	{
		return STATUS_ERROR;
	}

	return STATUS_OK;
}

