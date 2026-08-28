/*-------------------------------------------------------------------------
 *
 * spi_priv.h
 *				Server Programming Interface private declarations
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/spi_priv.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPI_PRIV_H
#define SPI_PRIV_H

#include "executor/spi.h"
#include "utils/queryenvironment.h"


#define _SPI_PLAN_MAGIC		569278163

typedef struct
{
	/* current results */
	uint64		processed;		/* by Executor */
	SPITupleTable *tuptable;	/* tuptable currently being built */

	/* subtransaction in which current Executor call was started */
	SubTransactionId execSubid;

	/* resources of this execution context */
	slist_head	tuptables;		/* list of all live SPITupleTables */
	MemoryContext procCxt;		/* procedure context */
	MemoryContext execCxt;		/* executor context */
	MemoryContext savedcxt;		/* context of SPI_connect's caller */
	SubTransactionId connectSubid;	/* ID of connecting subtransaction */
	QueryEnvironment *queryEnv; /* query environment setup for SPI level */

	/* transaction management support */
	bool		atomic;			/* atomic execution context, does not allow
								 * transactions */
	bool		internal_xact;	/* SPI-managed transaction boundary, skip
								 * cleanup */

	/* saved values of API global variables for previous nesting level */
	uint64		outer_processed;
	SPITupleTable *outer_tuptable;
	int			outer_result;
} _SPI_connection;

/*
 * Simplified SPI plan structure.  After removing the plan cache, SPI plans
 * simply store the original query text and parameter info.  Each execution
 * re-parses, re-analyzes, re-rewrites, and re-plans the query.
 */
typedef struct _SPI_plan
{
	int			magic;			/* should equal _SPI_PLAN_MAGIC */
	char	   *query_string;	/* original query text */
	int			nargs;			/* number of plan arguments */
	Oid		   *argtypes;		/* Argument types (NULL if nargs is 0) */
	ParserSetupHook parserSetup;	/* alternative parameter spec method */
	void	   *parserSetupArg;
	RawParseMode parse_mode;	/* raw_parser() mode */
	int			cursor_options; /* Cursor options used for planning */
	MemoryContext plancxt;		/* Context containing _SPI_plan and data */
} _SPI_plan;

#endif							/* SPI_PRIV_H */
