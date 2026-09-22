/*-------------------------------------------------------------------------
 *
 * standbydesc.c
 *	  rmgr descriptor routines for storage/ipc/standby.c
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/standbydesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/standbydefs.h"

/*
 * This routine is used by both standby_desc and xact_desc, because
 * transaction commits and XLOG_INVALIDATIONS messages contain invalidations;
 * it seems pointless to duplicate the code.
 */
void
standby_desc_invalidations(StringInfo buf,
						   int nmsgs, SharedInvalidationMessage *msgs,
						   Oid dbId, Oid tsId,
						   bool relcacheInitFileInval)
{
	int			i;

	/* Do nothing if there are no invalidation messages */
	if (nmsgs <= 0)
		return;

	if (relcacheInitFileInval)
		appendStringInfo(buf, "; relcache init file inval dbid %u tsid %u",
						 dbId, tsId);

	appendStringInfoString(buf, "; inval msgs:");
	for (i = 0; i < nmsgs; i++)
	{
		SharedInvalidationMessage *msg = &msgs[i];

		if (msg->id >= 0)
			appendStringInfo(buf, " catcache %d", msg->id);
		else if (msg->id == SHAREDINVALCATALOG_ID)
			appendStringInfo(buf, " catalog %u", msg->cat.catId);
		else if (msg->id == SHAREDINVALRELCACHE_ID)
			appendStringInfo(buf, " relcache %u", msg->rc.relId);
		/* not expected, but print something anyway */
		else if (msg->id == SHAREDINVALSMGR_ID)
			appendStringInfoString(buf, " smgr");
		/* not expected, but print something anyway */
		else if (msg->id == SHAREDINVALRELMAP_ID)
			appendStringInfo(buf, " relmap db %u", msg->rm.dbId);
		else if (msg->id == SHAREDINVALSNAPSHOT_ID)
			appendStringInfo(buf, " snapshot %u", msg->sn.relId);
		else
			appendStringInfo(buf, " unrecognized id %d", msg->id);
	}
}
