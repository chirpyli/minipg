/*-------------------------------------------------------------------------
 *
 * nodeAppend.c
 *	  routines to handle append nodes.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeAppend.c
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
 *		ExecInitAppend	- initialize the append node
 *		ExecAppend		- retrieve the next tuple from the node
 *		ExecEndAppend	- shut down the append node
 *		ExecReScanAppend - rescan the append node
 *
 *	 NOTES
 *		Each append node contains a list of one or more subplans which
 *		must be iteratively processed (forwards or backwards).
 *		Tuples are retrieved by executing the 'whichplan'th subplan
 *		until the subplan stops returning tuples, at which point that
 *		plan is shut down and the next started up.
 *
 *		Append nodes don't make use of their left and right
 *		subtrees, rather they maintain a list of subplans so
 *		a typical append node looks like this in the plan tree:
 *
 *				   ...
 *				   /
 *				Append -------+------+------+--- nil
 *				/	\		  |		 |		|
 *			  nil	nil		 ...    ...    ...
 *								 subplans
 *
 *		Append nodes are currently used for unions, and to support
 *		inheritance queries, where several relations need to be scanned.
 *		For example, in our standard person/student/employee/student-emp
 *		example, where student and employee inherit from person
 *		and student-emp inherits from student and employee, the
 *		query:
 *
 *				select name from person
 *
 *		generates the plan:
 *
 *				  |
 *				Append -------+-------+--------+--------+
 *				/	\		  |		  |		   |		|
 *			  nil	nil		 Scan	 Scan	  Scan	   Scan
 *							  |		  |		   |		|
 *							person employee student student-emp
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeAppend.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/latch.h"

#define INVALID_SUBPLAN_INDEX		-1
#define EVENT_BUFFER_SIZE			16

static TupleTableSlot *ExecAppend(PlanState *pstate);
static bool choose_next_subplan_locally(AppendState *node);

/* ----------------------------------------------------------------
 *		ExecInitAppend
 *
 *		Begin all of the subscans of the append node.
 *
 *	   (This is potentially wasteful, since the entire result of the
 *		append node may not be scanned, but this way all of the
 *		structures get allocated in the executor's top level memory
 *		block instead of that of the call to ExecAppend.)
 * ----------------------------------------------------------------
 */
AppendState *
ExecInitAppend(Append *node, EState *estate, int eflags)
{
	AppendState *appendstate = makeNode(AppendState);
	PlanState **appendplanstates;
	Bitmapset  *validsubplans;
	int			nplans;
	int			i,
				j;

	/* check for unsupported flags */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * create new AppendState for our append node
	 */
	appendstate->ps.plan = (Plan *) node;
	appendstate->ps.state = estate;
	appendstate->ps.ExecProcNode = ExecAppend;

	/* Let choose_next_subplan_locally handle setting the first subplan */
	appendstate->as_whichplan = INVALID_SUBPLAN_INDEX;
	appendstate->as_syncdone = false;
	appendstate->as_begun = false;

	nplans = list_length(node->appendplans);

	/*
	 * Mark all subplans as valid; they must also all be initialized.
	 */
	Assert(nplans > 0);
	appendstate->as_valid_subplans = validsubplans =
		bms_add_range(NULL, 0, nplans - 1);

	/*
	 * Initialize result tuple type and slot.
	 */
	ExecInitResultTupleSlotTL(&appendstate->ps, &TTSOpsVirtual);

	/* node returns slots from each of its subnodes, therefore not fixed */
	appendstate->ps.resultopsset = true;
	appendstate->ps.resultopsfixed = false;

	appendplanstates = (PlanState **) palloc(nplans *
											 sizeof(PlanState *));

	/*
	 * call ExecInitNode on each of the valid plans to be executed and save
	 * the results into the appendplanstates array.
	 */
	j = 0;
	i = -1;
	while ((i = bms_next_member(validsubplans, i)) >= 0)
	{
		Plan	   *initNode = (Plan *) list_nth(node->appendplans, i);

		appendplanstates[j++] = ExecInitNode(initNode, estate, eflags);
	}

	appendstate->appendplans = appendplanstates;
	appendstate->as_nplans = nplans;

	/*
	 * Miscellaneous initialization
	 */

	appendstate->ps.ps_ProjInfo = NULL;

	appendstate->choose_next_subplan = choose_next_subplan_locally;

	return appendstate;
}

/* ----------------------------------------------------------------
 *	   ExecAppend
 *
 *		Handles iteration over multiple subplans.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecAppend(PlanState *pstate)
{
	AppendState *node = castNode(AppendState, pstate);
	TupleTableSlot *result;

	/*
	 * If this is the first call after Init or ReScan, we need to do the
	 * initialization work.
	 */
	if (!node->as_begun)
	{
		Assert(node->as_whichplan == INVALID_SUBPLAN_INDEX);
		Assert(!node->as_syncdone);

		/* Nothing to do if there are no subplans */
		if (node->as_nplans == 0)
			return ExecClearTuple(node->ps.ps_ResultTupleSlot);

		/*
		 * If no sync subplan has been chosen, we must choose one before
		 * proceeding.
		 */
		if (!node->choose_next_subplan(node))
			return ExecClearTuple(node->ps.ps_ResultTupleSlot);

		Assert(node->as_syncdone ||
			   (node->as_whichplan >= 0 &&
				node->as_whichplan < node->as_nplans));

		/* And we're initialized. */
		node->as_begun = true;
	}

	for (;;)
	{
		PlanState  *subnode;

		CHECK_FOR_INTERRUPTS();

		/*
		 * figure out which sync subplan we are currently processing
		 */
		Assert(node->as_whichplan >= 0 && node->as_whichplan < node->as_nplans);
		subnode = node->appendplans[node->as_whichplan];

		/*
		 * get a tuple from the subplan
		 */
		result = ExecProcNode(subnode);

		if (!TupIsNull(result))
		{
			/*
			 * If the subplan gave us something then return it as-is. We do
			 * NOT make use of the result slot that was set up in
			 * ExecInitAppend; there's no need for it.
			 */
			return result;
		}

		/*
		 * choose new sync subplan; if no sync subplans, we're done
		 */
		if (!node->choose_next_subplan(node))
			return ExecClearTuple(node->ps.ps_ResultTupleSlot);
	}
}

/* ----------------------------------------------------------------
 *		ExecEndAppend
 *
 *		Shuts down the subscans of the append node.
 *
 *		Returns nothing of interest.
 * ----------------------------------------------------------------
 */
void
ExecEndAppend(AppendState *node)
{
	PlanState **appendplans;
	int			nplans;
	int			i;

	/*
	 * get information from the node
	 */
	appendplans = node->appendplans;
	nplans = node->as_nplans;

	/*
	 * shut down each of the subscans
	 */
	for (i = 0; i < nplans; i++)
		ExecEndNode(appendplans[i]);
}

void
ExecReScanAppend(AppendState *node)
{
	int			i;

	/*
	 * If any PARAM_EXEC Params used in pruning expressions have changed, then
	 * we'd better unset the valid subplans so that they are reselected for
	 * the new parameter values.
	 */

	for (i = 0; i < node->as_nplans; i++)
	{
		PlanState  *subnode = node->appendplans[i];

		/*
		 * ExecReScan doesn't know about my subplans, so I have to do
		 * changed-parameter signaling myself.
		 */
		if (node->ps.chgParam != NULL)
			UpdateChangedParamSet(subnode, node->ps.chgParam);

		/*
		 * If chgParam of subnode is not null then plan will be re-scanned by
		 * first ExecProcNode.
		 */
		if (subnode->chgParam == NULL)
			ExecReScan(subnode);
	}

	/* Let choose_next_subplan_* function handle setting the first subplan */
	node->as_whichplan = INVALID_SUBPLAN_INDEX;
	node->as_syncdone = false;
	node->as_begun = false;
}

/* ----------------------------------------------------------------
 *		choose_next_subplan_locally
 *
 *		Choose next sync subplan for a non-parallel-aware Append,
 *		returning false if there are no more.
 * ----------------------------------------------------------------
 */
static bool
choose_next_subplan_locally(AppendState *node)
{
	int			whichplan = node->as_whichplan;
	int			nextplan;

	/* We should never be called when there are no subplans */
	Assert(node->as_nplans > 0);

	/* Nothing to do if syncdone */
	if (node->as_syncdone)
		return false;

	/*
	 * If first call then have the bms member function choose the first valid
	 * sync subplan by initializing whichplan to -1.  If there happen to be no
	 * valid sync subplans then the bms member function will handle that by
	 * returning a negative number which will allow us to exit returning a
	 * false value.
	 */
	if (whichplan == INVALID_SUBPLAN_INDEX)
	{
		if (node->as_valid_subplans == NULL)
			node->as_valid_subplans =
				bms_add_range(NULL, 0, node->as_nplans - 1);

		whichplan = -1;
	}

	/* Ensure whichplan is within the expected range */
	Assert(whichplan >= -1 && whichplan <= node->as_nplans);

	if (ScanDirectionIsForward(node->ps.state->es_direction))
		nextplan = bms_next_member(node->as_valid_subplans, whichplan);
	else
		nextplan = bms_prev_member(node->as_valid_subplans, whichplan);

	if (nextplan < 0)
	{
		node->as_syncdone = true;
		return false;
	}

	node->as_whichplan = nextplan;

	return true;
}
