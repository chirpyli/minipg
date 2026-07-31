/*-------------------------------------------------------------------------
 *
 * username.c
 *	  get user name
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/username.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <pwd.h>
#include <unistd.h>

#include "common/username.h"

/*
 * Returns the current user name in a static buffer
 * On error, returns NULL and sets *errstr to point to a palloc'd message
 */
const char *
get_user_name(char **errstr)
{
	struct passwd *pw;
	uid_t		user_id = geteuid();

	*errstr = NULL;

	errno = 0;					/* clear errno before call */
	pw = getpwuid(user_id);
	if (!pw)
	{
		*errstr = psprintf(_("could not look up effective user ID %ld: %s"),
						   (long) user_id,
						   errno ? strerror(errno) : _("user does not exist"));
		return NULL;
	}

	return pw->pw_name;
}


/*
 * Returns the current user name in a static buffer or exits
 */
const char *
get_user_name_or_exit(const char *progname)
{
	const char *user_name;
	char	   *errstr;

	user_name = get_user_name(&errstr);

	if (!user_name)
	{
		fprintf(stderr, "%s: %s\n", progname, errstr);
		exit(1);
	}
	return user_name;
}
