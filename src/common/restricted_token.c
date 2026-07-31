/*-------------------------------------------------------------------------
 *
 * restricted_token.c
 *		helper routine to ensure restricted token on Windows
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/restricted_token.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#error "This file is not expected to be compiled for backend code"
#endif

#include "postgres_fe.h"

#include "common/logging.h"
#include "common/restricted_token.h"

/*
 * On Windows make sure that we are running with a restricted token,
 * On other platforms do nothing.
 */
void
get_restricted_token(void)
{
}
