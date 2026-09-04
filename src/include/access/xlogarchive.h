/*------------------------------------------------------------------------
 *
 * xlogarchive.h
 *		Prototypes for WAL archives in the backend
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/xlogarchive.h
 *
 *------------------------------------------------------------------------
 */

#ifndef XLOG_ARCHIVE_H
#define XLOG_ARCHIVE_H

#include "access/xlogdefs.h"

extern void ExecuteRecoveryCommand(const char *command, const char *commandName,
								   bool failOnSignal);
extern void KeepFileRestoredFromArchive(const char *path, const char *xlogfname);

#endif							/* XLOG_ARCHIVE_H */
