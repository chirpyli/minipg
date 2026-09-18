/*-------------------------------------------------------------------------
 *
 * analyze.h
 *		parse analysis for optimizable statements
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/analyze.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ANALYZE_H
#define ANALYZE_H

#include "parser/parse_node.h"
#include "utils/queryjumble.h"


extern Query *parse_analyze(RawStmt *parseTree, const char *sourceText,
							Oid *paramTypes, int numParams, QueryEnvironment *queryEnv);

extern Query *parse_sub_analyze(Node *parseTree, ParseState *parentParseState,
								bool resolve_unknowns);

extern Query *transformTopLevelStmt(ParseState *pstate, RawStmt *parseTree);
extern Query *transformStmt(ParseState *pstate, Node *parseTree);

extern bool stmt_requires_parse_analysis(RawStmt *parseTree);
extern bool analyze_requires_snapshot(RawStmt *parseTree);

#endif							/* ANALYZE_H */
