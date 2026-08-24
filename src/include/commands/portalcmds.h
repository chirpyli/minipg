/*-------------------------------------------------------------------------
 *
 * portalcmds.h
 *	  prototypes for portalcmds.c.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/portalcmds.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORTALCMDS_H
#define PORTALCMDS_H

#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "utils/portal.h"


extern void PortalCleanup(Portal portal);

#endif							/* PORTALCMDS_H */
