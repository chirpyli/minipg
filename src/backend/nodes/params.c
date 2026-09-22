/*-------------------------------------------------------------------------
 *
 * params.c
 *	  Support for finding the values associated with Param nodes.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/nodes/params.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "fmgr.h"
#include "mb/stringinfo_mb.h"
#include "nodes/params.h"
#include "parser/parse_node.h"
#include "storage/shmem.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"


static void paramlist_parser_setup(ParseState *pstate, void *arg);
static Node *paramlist_param_ref(ParseState *pstate, ParamRef *pref);


/*
 * Allocate and initialize a new ParamListInfo structure.
 *
 * To make a new structure for the "dynamic" way (with hooks), pass 0 for
 * numParams and set numParams manually.
 *
 * A default parserSetup function is supplied automatically.  Callers may
 * override it if they choose.  (Note that most use-cases for ParamListInfos
 * will never use the parserSetup function anyway.)
 */
ParamListInfo
makeParamList(int numParams)
{
	ParamListInfo retval;
	Size		size;

	size = offsetof(ParamListInfoData, params) +
		numParams * sizeof(ParamExternData);

	retval = (ParamListInfo) palloc(size);
	retval->paramFetch = NULL;
	retval->paramFetchArg = NULL;
	retval->paramCompile = NULL;
	retval->paramCompileArg = NULL;
	retval->parserSetup = paramlist_parser_setup;
	retval->parserSetupArg = (void *) retval;
	retval->numParams = numParams;

	return retval;
}


/*
 * Set up to parse a query containing references to parameters
 * sourced from a ParamListInfo.
 */
static void
paramlist_parser_setup(ParseState *pstate, void *arg)
{
	pstate->p_paramref_hook = paramlist_param_ref;
	/* no need to use p_coerce_param_hook */
	pstate->p_ref_hook_state = arg;
}

/*
 * Transform a ParamRef using parameter type data from a ParamListInfo.
 */
static Node *
paramlist_param_ref(ParseState *pstate, ParamRef *pref)
{
	ParamListInfo paramLI = (ParamListInfo) pstate->p_ref_hook_state;
	int			paramno = pref->number;
	ParamExternData *prm;
	ParamExternData prmdata;
	Param	   *param;

	/* check parameter number is valid */
	if (paramno <= 0 || paramno > paramLI->numParams)
		return NULL;

	/* give hook a chance in case parameter is dynamic */
	if (paramLI->paramFetch != NULL)
		prm = paramLI->paramFetch(paramLI, paramno, false, &prmdata);
	else
		prm = &paramLI->params[paramno - 1];

	if (!OidIsValid(prm->ptype))
		return NULL;

	param = makeNode(Param);
	param->paramkind = PARAM_EXTERN;
	param->paramid = paramno;
	param->paramtype = prm->ptype;
	param->paramtypmod = -1;
	param->paramcollid = get_typcollation(param->paramtype);
	param->location = pref->location;

	return (Node *) param;
}

