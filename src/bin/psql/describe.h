/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2021, PostgreSQL Global Development Group
 *
 * src/bin/psql/describe.h
 */
#ifndef DESCRIBE_H
#define DESCRIBE_H


/* \d foo */
extern bool describeTableDetails(const char *pattern, bool verbose, bool showSystem);

/* \l */
extern bool listAllDbs(const char *pattern, bool verbose);

/* \dt, \di, \ds, \dS, etc. */
extern bool listTables(const char *tabtypes, const char *pattern, bool verbose, bool showSystem);

#endif							/* DESCRIBE_H */
