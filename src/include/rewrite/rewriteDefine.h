/*-------------------------------------------------------------------------
 *
 * rewriteDefine.h
 *
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/rewrite/rewriteDefine.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEDEFINE_H
#define REWRITEDEFINE_H

#include "catalog/objectaddress.h"
#include "nodes/parsenodes.h"
#include "utils/relcache.h"

#define RULE_FIRES_ON_ORIGIN	'O'


extern ObjectAddress DefineQueryRewrite(Oid event_relid, bool replace,
										List *action);


#endif							/* REWRITEDEFINE_H */
