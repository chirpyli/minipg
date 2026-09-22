/*-------------------------------------------------------------------------
 *
 * cmdtag.c
 *	  Data and routines for commandtag names and enumeration.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/tcop/cmdtag.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "tcop/cmdtag.h"


typedef struct CommandTagBehavior
{
	const char *name;
	const bool	table_rewrite_ok;
	const bool	display_rowcount;
} CommandTagBehavior;

#define PG_CMDTAG(tag, name, rwrok, rowcnt) \
	{ name, rwrok, rowcnt },

const CommandTagBehavior tag_behavior[COMMAND_TAG_NEXTTAG] = {
#include "tcop/cmdtaglist.h"
};

#undef PG_CMDTAG

void
InitializeQueryCompletion(QueryCompletion *qc)
{
	qc->commandTag = CMDTAG_UNKNOWN;
	qc->nprocessed = 0;
}

const char *
GetCommandTagName(CommandTag commandTag)
{
	return tag_behavior[commandTag].name;
}

bool
command_tag_display_rowcount(CommandTag commandTag)
{
	return tag_behavior[commandTag].display_rowcount;
}

