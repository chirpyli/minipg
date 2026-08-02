/*-------------------------------------------------------------------------
 *
 * pg_strong_random.c
 *	  generate a cryptographically secure random number
 *
 * Our definition of "strong" is that it's suitable for generating random
 * salts and query cancellation keys, during authentication.
 *
 * Note: this code is run quite early in postmaster and backend startup;
 * therefore, even when built for backend, it cannot rely on backend
 * infrastructure such as elog() or palloc().
 *
 * Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/pg_strong_random.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>

/*
 * pg_strong_random & pg_strong_random_init
 *
 * Generate requested number of random bytes. The returned bytes are
 * cryptographically secure, suitable for use e.g. in authentication.
 *
 * Before pg_strong_random is called in any process, the generator must first
 * be initialized by calling pg_strong_random_init().
 *
 * We rely on system facilities for actually generating the numbers.
 * The random data is read from /dev/urandom.
 *
 * Returns true on success, and false if none of the sources
 * were available. NB: It is important to check the return value!
 * Proceeding with key generation when no random data was available
 * would lead to predictable keys and security issues.
 */



/*
 * Read /dev/urandom ourselves.
 */

void
pg_strong_random_init(void)
{
	/* No initialization needed */
}

bool
pg_strong_random(void *buf, size_t len)
{
	int			f;
	char	   *p = buf;
	ssize_t		res;

	f = open("/dev/urandom", O_RDONLY, 0);
	if (f == -1)
		return false;

	while (len)
	{
		res = read(f, p, len);
		if (res <= 0)
		{
			if (errno == EINTR)
				continue;		/* interrupted by signal, just retry */

			close(f);
			return false;
		}

		p += res;
		len -= res;
	}

	close(f);
	return true;
}
