/*-------------------------------------------------------------------------
 *
 * fe-auth.c
 *	   The front-end (client) authorization routines
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-auth.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * INTERFACE ROUTINES
 *	   frontend (client) routines:
 *		pg_fe_sendauth			send authentication information
 *		pg_fe_getauthname		get user's name according to the client side
 *								of the authentication system
 */

#include "postgres_fe.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/param.h>			/* for MAXHOSTNAMELEN on most */
#include <sys/socket.h>
#ifdef HAVE_SYS_UCRED_H
#include <sys/ucred.h>
#endif
#ifndef  MAXHOSTNAMELEN
#include <netdb.h>				/* for MAXHOSTNAMELEN on some */
#endif
#include <pwd.h>


#include "common/md5.h"
#include "common/scram-common.h"
#include "fe-auth.h"
#include "libpq-fe.h"

/*
 * Initialize SASL authentication exchange.
 */
static int
pg_SASL_init(PGconn *conn, int payloadlen)
{
	char	   *initialresponse = NULL;
	int			initialresponselen;
	bool		done;
	bool		success;
	const char *selected_mechanism;
	PQExpBufferData mechanism_buf;
	char	   *password;

	initPQExpBuffer(&mechanism_buf);

	if (conn->channel_binding[0] == 'r')	/* require */
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("channel binding required, but SSL not in use\n"));
		goto error;
	}

	if (conn->sasl_state)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("duplicate SASL authentication request\n"));
		goto error;
	}

	/*
	 * Parse the list of SASL authentication mechanisms in the
	 * AuthenticationSASL message, and select the best mechanism that we
	 * support.  SCRAM-SHA-256 is the only one supported at the moment;
	 * SCRAM-SHA-256-PLUS requires TLS channel binding, which this build does
	 * not support.
	 */
	selected_mechanism = NULL;
	for (;;)
	{
		if (pqGets(&mechanism_buf, conn))
		{
			appendPQExpBufferStr(&conn->errorMessage,
								 "fe_sendauth: invalid authentication request from server: invalid list of authentication mechanisms\n");
			goto error;
		}
		if (PQExpBufferDataBroken(mechanism_buf))
			goto oom_error;

		/* An empty string indicates end of list */
		if (mechanism_buf.data[0] == '\0')
			break;

		/*
		 * Select the mechanism to use.  Pick SCRAM-SHA-256-PLUS over anything
		 * else if a channel binding type is set and if the client supports it
		 * (and did not set channel_binding=disable). Pick SCRAM-SHA-256 if
		 * nothing else has already been picked.  If we add more mechanisms, a
		 * more refined priority mechanism might become necessary.
		 */
		if (strcmp(mechanism_buf.data, SCRAM_SHA_256_NAME) == 0 &&
			!selected_mechanism)
			selected_mechanism = SCRAM_SHA_256_NAME;
	}

	if (!selected_mechanism)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("none of the server's SASL authentication mechanisms are supported\n"));
		goto error;
	}

	/*
	 * Now that the SASL mechanism has been chosen for the exchange,
	 * initialize its state information.
	 */

	/*
	 * First, select the password to use for the exchange, complaining if
	 * there isn't one.  Currently, all supported SASL mechanisms require a
	 * password, so we can just go ahead here without further distinction.
	 */
	conn->password_needed = true;
	password = conn->connhost[conn->whichhost].password;
	if (password == NULL)
		password = conn->pgpass;
	if (password == NULL || password[0] == '\0')
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 PQnoPasswordSupplied);
		goto error;
	}

	/*
	 * Initialize the SASL state information with all the information gathered
	 * during the initial exchange.
	 *
	 * Note: Only tls-unique is supported for the moment.
	 */
	conn->sasl_state = pg_fe_scram_init(conn,
										password,
										selected_mechanism);
	if (!conn->sasl_state)
		goto oom_error;

	/* Get the mechanism-specific Initial Client Response, if any */
	pg_fe_scram_exchange(conn->sasl_state,
						 NULL, -1,
						 &initialresponse, &initialresponselen,
						 &done, &success);

	if (done && !success)
		goto error;

	/*
	 * Build a SASLInitialResponse message, and send it.
	 */
	if (pqPutMsgStart('p', conn))
		goto error;
	if (pqPuts(selected_mechanism, conn))
		goto error;
	if (initialresponse)
	{
		if (pqPutInt(initialresponselen, 4, conn))
			goto error;
		if (pqPutnchar(initialresponse, initialresponselen, conn))
			goto error;
	}
	if (pqPutMsgEnd(conn))
		goto error;
	if (pqFlush(conn))
		goto error;

	termPQExpBuffer(&mechanism_buf);
	if (initialresponse)
		free(initialresponse);

	return STATUS_OK;

error:
	termPQExpBuffer(&mechanism_buf);
	if (initialresponse)
		free(initialresponse);
	return STATUS_ERROR;

oom_error:
	termPQExpBuffer(&mechanism_buf);
	if (initialresponse)
		free(initialresponse);
	appendPQExpBufferStr(&conn->errorMessage,
						 libpq_gettext("out of memory\n"));
	return STATUS_ERROR;
}

/*
 * Exchange a message for SASL communication protocol with the backend.
 * This should be used after calling pg_SASL_init to set up the status of
 * the protocol.
 */
static int
pg_SASL_continue(PGconn *conn, int payloadlen, bool final)
{
	char	   *output;
	int			outputlen;
	bool		done;
	bool		success;
	int			res;
	char	   *challenge;

	/* Read the SASL challenge from the AuthenticationSASLContinue message. */
	challenge = malloc(payloadlen + 1);
	if (!challenge)
	{
		appendPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("out of memory allocating SASL buffer (%d)\n"),
						  payloadlen);
		return STATUS_ERROR;
	}

	if (pqGetnchar(challenge, payloadlen, conn))
	{
		free(challenge);
		return STATUS_ERROR;
	}
	/* For safety and convenience, ensure the buffer is NULL-terminated. */
	challenge[payloadlen] = '\0';

	pg_fe_scram_exchange(conn->sasl_state,
						 challenge, payloadlen,
						 &output, &outputlen,
						 &done, &success);
	free(challenge);			/* don't need the input anymore */

	if (final && !done)
	{
		if (outputlen != 0)
			free(output);

		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("AuthenticationSASLFinal received from server, but SASL authentication was not completed\n"));
		return STATUS_ERROR;
	}
	if (outputlen != 0)
	{
		/*
		 * Send the SASL response to the server.
		 */
		res = pqPacketSend(conn, 'p', output, outputlen);
		free(output);

		if (res != STATUS_OK)
			return STATUS_ERROR;
	}

	if (done && !success)
		return STATUS_ERROR;

	return STATUS_OK;
}

static int
pg_password_sendauth(PGconn *conn, const char *password, AuthRequest areq)
{
	if (areq != AUTH_REQ_PASSWORD)
		return STATUS_ERROR;

	return pqPacketSend(conn, 'p', password, strlen(password) + 1);
}

/*
 * Verify that the authentication request is expected, given the connection
 * parameters. This is especially important when the client wishes to
 * authenticate the server before any sensitive information is exchanged.
 */
static bool
check_expected_areq(AuthRequest areq, PGconn *conn)
{
	bool		result = true;

	/*
	 * When channel_binding=require, we must protect against two cases: (1) we
	 * must not respond to non-SASL authentication requests, which might leak
	 * information such as the client's password; and (2) even if we receive
	 * AUTH_REQ_OK, we still must ensure that channel binding has happened in
	 * order to authenticate the server.
	 */
	if (conn->channel_binding[0] == 'r' /* require */ )
	{
		switch (areq)
		{
			case AUTH_REQ_SASL:
			case AUTH_REQ_SASL_CONT:
			case AUTH_REQ_SASL_FIN:
				break;
			case AUTH_REQ_OK:
				if (!pg_fe_scram_channel_bound(conn->sasl_state))
				{
					appendPQExpBufferStr(&conn->errorMessage,
										 libpq_gettext("channel binding required, but server authenticated client without channel binding\n"));
					result = false;
				}
				break;
			default:
				appendPQExpBufferStr(&conn->errorMessage,
									 libpq_gettext("channel binding required but not supported by server's authentication request\n"));
				result = false;
				break;
		}
	}

	return result;
}

/*
 * pg_fe_sendauth
 *		client demux routine for processing an authentication request
 *
 * The server has sent us an authentication challenge (or OK). Send an
 * appropriate response. The caller has ensured that the whole message is
 * now in the input buffer, and has already read the type and length of
 * it. We are responsible for reading any remaining extra data, specific
 * to the authentication method. 'payloadlen' is the remaining length in
 * the message.
 */
int
pg_fe_sendauth(AuthRequest areq, int payloadlen, PGconn *conn)
{
	int			oldmsglen;

	if (!check_expected_areq(areq, conn))
		return STATUS_ERROR;

	switch (areq)
	{
		case AUTH_REQ_OK:
			break;

		case AUTH_REQ_KRB4:
			appendPQExpBufferStr(&conn->errorMessage,
								 libpq_gettext("Kerberos 4 authentication not supported\n"));
			return STATUS_ERROR;

		case AUTH_REQ_KRB5:
			appendPQExpBufferStr(&conn->errorMessage,
								 libpq_gettext("Kerberos 5 authentication not supported\n"));
			return STATUS_ERROR;

		case AUTH_REQ_CRYPT:
			appendPQExpBufferStr(&conn->errorMessage,
								 libpq_gettext("Crypt authentication not supported\n"));
			return STATUS_ERROR;

		case AUTH_REQ_MD5:
			appendPQExpBufferStr(&conn->errorMessage,
								 libpq_gettext("MD5 authentication not supported\n"));
			return STATUS_ERROR;

		case AUTH_REQ_PASSWORD:
			{
				char	   *password;

				conn->password_needed = true;
				password = conn->connhost[conn->whichhost].password;
				if (password == NULL)
					password = conn->pgpass;
				if (password == NULL || password[0] == '\0')
				{
					appendPQExpBufferStr(&conn->errorMessage,
										 PQnoPasswordSupplied);
					return STATUS_ERROR;
				}
				if (pg_password_sendauth(conn, password, areq) != STATUS_OK)
				{
					appendPQExpBufferStr(&conn->errorMessage,
										 "fe_sendauth: error sending password authentication\n");
					return STATUS_ERROR;
				}
				break;
			}

		case AUTH_REQ_SASL:

			/*
			 * The request contains the name (as assigned by IANA) of the
			 * authentication mechanism.
			 */
			if (pg_SASL_init(conn, payloadlen) != STATUS_OK)
			{
				/* pg_SASL_init already set the error message */
				return STATUS_ERROR;
			}
			break;

		case AUTH_REQ_SASL_CONT:
		case AUTH_REQ_SASL_FIN:
			if (conn->sasl_state == NULL)
			{
				appendPQExpBufferStr(&conn->errorMessage,
									 "fe_sendauth: invalid authentication request from server: AUTH_REQ_SASL_CONT without AUTH_REQ_SASL\n");
				return STATUS_ERROR;
			}
			oldmsglen = conn->errorMessage.len;
			if (pg_SASL_continue(conn, payloadlen,
								 (areq == AUTH_REQ_SASL_FIN)) != STATUS_OK)
			{
				/* Use this message if pg_SASL_continue didn't supply one */
				if (conn->errorMessage.len == oldmsglen)
					appendPQExpBufferStr(&conn->errorMessage,
										 "fe_sendauth: error in SASL authentication\n");
				return STATUS_ERROR;
			}
			break;

		default:
			appendPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("authentication method %u not supported\n"), areq);
			return STATUS_ERROR;
	}

	return STATUS_OK;
}


/*
 * pg_fe_getauthname
 *
 * Returns a pointer to malloc'd space containing whatever name the user
 * has authenticated to the system.  If there is an error, return NULL,
 * and append a suitable error message to *errorMessage if that's not NULL.
 */
char *
pg_fe_getauthname(PQExpBuffer errorMessage)
{
	char	   *result = NULL;
	const char *name = NULL;

	uid_t		user_id = geteuid();
	char		pwdbuf[BUFSIZ];
	struct passwd pwdstr;
	struct passwd *pw = NULL;
	int			pwerr;

	/*
	 * Some users are using configure --enable-thread-safety-force, so we
	 * might as well do the locking within our library to protect
	 * pqGetpwuid(). In fact, application developers can use getpwuid() in
	 * their application if they use the locking call we provide, or install
	 * their own locking function using PQregisterThreadLock().
	 */
	pglock_thread();

	pwerr = pqGetpwuid(user_id, &pwdstr, pwdbuf, sizeof(pwdbuf), &pw);
	if (pw != NULL)
		name = pw->pw_name;
	else if (errorMessage)
	{
		if (pwerr != 0)
			appendPQExpBuffer(errorMessage,
							  libpq_gettext("could not look up local user ID %d: %s\n"),
							  (int) user_id,
							  strerror_r(pwerr, pwdbuf, sizeof(pwdbuf)));
		else
			appendPQExpBuffer(errorMessage,
							  libpq_gettext("local user with ID %d does not exist\n"),
							  (int) user_id);
	}

	if (name)
	{
		result = strdup(name);
		if (result == NULL && errorMessage)
			appendPQExpBufferStr(errorMessage,
								 libpq_gettext("out of memory\n"));
	}

	pgunlock_thread();

	return result;
}


/*
 * PQencryptPassword -- exported routine to encrypt a password with MD5
 *
 * This function is equivalent to calling PQencryptPasswordConn with
 * "md5" as the encryption method, except that this doesn't require
 * a connection object.  This function is deprecated, use
 * PQencryptPasswordConn instead.
 */
char *
PQencryptPassword(const char *passwd, const char *user)
{
	char	   *crypt_pwd;

	crypt_pwd = malloc(MD5_PASSWD_LEN + 1);
	if (!crypt_pwd)
		return NULL;

	if (!pg_md5_encrypt(passwd, user, strlen(user), crypt_pwd))
	{
		free(crypt_pwd);
		return NULL;
	}

	return crypt_pwd;
}

/*
 * PQencryptPasswordConn -- exported routine to encrypt a password
 *
 * This is intended to be used by client applications that wish to send
 * commands like ALTER USER joe PASSWORD 'pwd'.  The password need not
 * be sent in cleartext if it is encrypted on the client side.  This is
 * good because it ensures the cleartext password won't end up in logs,
 * pg_stat displays, etc.  We export the function so that clients won't
 * be dependent on low-level details like whether the encryption is MD5
 * or something else.
 *
 * Arguments are a connection object, the cleartext password, the SQL
 * name of the user it is for, and a string indicating the algorithm to
 * use for encrypting the password.  If algorithm is NULL, this queries
 * the server for the current 'password_encryption' value.  If you wish
 * to avoid that, e.g. to avoid blocking, you can execute
 * 'show password_encryption' yourself before calling this function, and
 * pass it as the algorithm.
 *
 * Return value is a malloc'd string.  The client may assume the string
 * doesn't contain any special characters that would require escaping.
 * On error, an error message is stored in the connection object, and
 * returns NULL.
 */
char *
PQencryptPasswordConn(PGconn *conn, const char *passwd, const char *user,
					  const char *algorithm)
{
#define MAX_ALGORITHM_NAME_LEN 50
	char		algobuf[MAX_ALGORITHM_NAME_LEN + 1];
	char	   *crypt_pwd = NULL;

	if (!conn)
		return NULL;

	resetPQExpBuffer(&conn->errorMessage);

	/* If no algorithm was given, ask the server. */
	if (algorithm == NULL)
	{
		PGresult   *res;
		char	   *val;

		res = PQexec(conn, "show password_encryption");
		if (res == NULL)
		{
			/* PQexec() should've set conn->errorMessage already */
			return NULL;
		}
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			/* PQexec() should've set conn->errorMessage already */
			PQclear(res);
			return NULL;
		}
		if (PQntuples(res) != 1 || PQnfields(res) != 1)
		{
			PQclear(res);
			appendPQExpBufferStr(&conn->errorMessage,
								 libpq_gettext("unexpected shape of result set returned for SHOW\n"));
			return NULL;
		}
		val = PQgetvalue(res, 0, 0);

		if (strlen(val) > MAX_ALGORITHM_NAME_LEN)
		{
			PQclear(res);
			appendPQExpBufferStr(&conn->errorMessage,
								 libpq_gettext("password_encryption value too long\n"));
			return NULL;
		}
		strcpy(algobuf, val);
		PQclear(res);

		algorithm = algobuf;
	}

	/*
	 * Also accept "on" and "off" as aliases for "md5", because
	 * password_encryption was a boolean before PostgreSQL 10.  We refuse to
	 * send the password in plaintext even if it was "off".
	 */
	if (strcmp(algorithm, "on") == 0 ||
		strcmp(algorithm, "off") == 0)
		algorithm = "md5";

	/*
	 * Ok, now we know what algorithm to use
	 */
	if (strcmp(algorithm, "scram-sha-256") == 0)
	{
		crypt_pwd = pg_fe_scram_build_secret(passwd);
	}
	else if (strcmp(algorithm, "md5") == 0)
	{
		crypt_pwd = malloc(MD5_PASSWD_LEN + 1);
		if (crypt_pwd)
		{
			if (!pg_md5_encrypt(passwd, user, strlen(user), crypt_pwd))
			{
				free(crypt_pwd);
				crypt_pwd = NULL;
			}
		}
	}
	else
	{
		appendPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("unrecognized password encryption algorithm \"%s\"\n"),
						  algorithm);
		return NULL;
	}

	if (!crypt_pwd)
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("out of memory\n"));

	return crypt_pwd;
}
