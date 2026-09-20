/*-------------------------------------------------------------------------
 *
 * tcopprot.h
 *	  prototypes for postgres.c.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/tcop/tcopprot.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TCOPPROT_H
#define TCOPPROT_H

#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "storage/procsignal.h"
#include "utils/guc.h"


/* Required daylight between max_stack_depth and the kernel limit, in bytes */
#define STACK_DEPTH_SLOP (512 * 1024L)

extern CommandDest whereToSendOutput;
extern PGDLLIMPORT const char *debug_query_string;
extern int	max_stack_depth;
extern int	PostAuthDelay;

/* GUC-configurable parameters */

/* Flags for restrict_nonsystem_relation_kind value */
#define RESTRICT_RELKIND_VIEW			0x01

extern PGDLLIMPORT int restrict_nonsystem_relation_kind;

extern List *pg_parse_query(const char *query_string);
extern List *pg_rewrite_query(Query *query);
extern List *pg_analyze_and_rewrite(RawStmt *parsetree,
									const char *query_string,
									Oid *paramTypes, int numParams);
extern List *pg_analyze_and_rewrite_params(RawStmt *parsetree,
										   const char *query_string,
										   ParserSetupHook parserSetup,
										   void *parserSetupArg);
extern PlannedStmt *pg_plan_query(Query *querytree, const char *query_string,
								  int cursorOptions,
								  ParamListInfo boundParams);
extern List *pg_plan_queries(List *querytrees, const char *query_string,
							 int cursorOptions,
							 ParamListInfo boundParams);

extern bool check_max_stack_depth(int *newval, void **extra, GucSource source);
extern void assign_max_stack_depth(int newval, void *extra);
extern bool check_restrict_nonsystem_relation_kind(char **newval, void **extra,
												   GucSource source);
extern void assign_restrict_nonsystem_relation_kind(const char *newval, void *extra);

extern void die(SIGNAL_ARGS);
extern void quickdie(SIGNAL_ARGS) pg_attribute_noreturn();
extern void StatementCancelHandler(SIGNAL_ARGS);
extern void FloatExceptionHandler(SIGNAL_ARGS) pg_attribute_noreturn();
extern void RecoveryConflictInterrupt(ProcSignalReason reason); /* called from SIGUSR1
																 * handler */
extern void ProcessClientReadInterrupt(bool blocked);
extern void ProcessClientWriteInterrupt(bool blocked);

extern void process_postgres_switches(int argc, char *argv[],
									  GucContext ctx, const char **dbname);
extern void PostgresMain(int argc, char *argv[],
						 const char *dbname,
						 const char *username) pg_attribute_noreturn();
extern long get_stack_depth_rlimit(void);
extern void set_debug_options(int debug_flag,
							  GucContext context, GucSource source);
extern bool set_plan_disabling_options(const char *arg,
									   GucContext context, GucSource source);

#endif							/* TCOPPROT_H */
