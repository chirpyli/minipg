/*-------------------------------------------------------------------------
 *
 * pread.c
 *	  Implementation of pread(2) for platforms that lack one.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/pread.c
 *
 * Note that this implementation changes the current file position, unlike
 * the POSIX function, so we use the name pg_pread().
 *
 *-------------------------------------------------------------------------
 */


#include "c.h"

#include <unistd.h>

ssize_t
pg_pread(int fd, void *buf, size_t size, off_t offset)
{
	if (lseek(fd, offset, SEEK_SET) < 0)
		return -1;

	return read(fd, buf, size);
}
