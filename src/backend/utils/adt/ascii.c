/*-----------------------------------------------------------------------
 * ascii.c
 *	 The PostgreSQL routine for string to ascii conversion.
 *
 *	 Portions Copyright (c) 1999-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/ascii.c
 *
 *-----------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "utils/ascii.h"
#include "utils/builtins.h"

/* ----------
 * Copy a string in an arbitrary backend-safe encoding, converting it to a
 * valid ASCII string by replacing non-ASCII bytes with '?'.  Otherwise the
 * behavior is identical to strlcpy(), except that we don't bother with a
 * return value.
 *
 * This must not trigger ereport(ERROR), as it is called in postmaster.
 * ----------
 */
void
ascii_safe_strlcpy(char *dest, const char *src, size_t destsiz)
{
	if (destsiz == 0)			/* corner case: no room for trailing nul */
		return;

	while (--destsiz > 0)
	{
		/* use unsigned char here to avoid compiler warning */
		unsigned char ch = *src++;

		if (ch == '\0')
			break;
		/* Keep printable ASCII characters */
		if (32 <= ch && ch <= 127)
			*dest = ch;
		/* White-space is also OK */
		else if (ch == '\n' || ch == '\r' || ch == '\t')
			*dest = ch;
		/* Everything else is replaced with '?' */
		else
			*dest = '?';
		dest++;
	}

	*dest = '\0';
}
