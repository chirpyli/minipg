/*----------------------------------------------------------------------
 *
 * cmdtaglist.h
 *    Command tags
 *
 * The command tag list is kept in its own source file for possible use
 * by automatic tools.  The exact representation of a command tag is
 * determined by the PG_CMDTAG macro, which is not defined in this file;
 * it can be defined by the caller for special purposes.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/tcop/cmdtaglist.h
 *
 *----------------------------------------------------------------------
 */

/* there is deliberately not an #ifndef CMDTAGLIST_H here */

/*
 * List of command tags.  The entries must be sorted alphabetically on their
 * textual name, so that we can bsearch on it; see GetCommandTagEnum().
 */

PG_CMDTAG(CMDTAG_UNKNOWN, "???", false, false)
PG_CMDTAG(CMDTAG_ALTER_INDEX, "ALTER INDEX", false, false)
PG_CMDTAG(CMDTAG_ALTER_TABLE, "ALTER TABLE", true, false)
PG_CMDTAG(CMDTAG_ANALYZE, "ANALYZE", false, false)
PG_CMDTAG(CMDTAG_BEGIN, "BEGIN", false, false)
PG_CMDTAG(CMDTAG_CHECKPOINT, "CHECKPOINT", false, false)
PG_CMDTAG(CMDTAG_CLUSTER, "CLUSTER", false, false)
PG_CMDTAG(CMDTAG_COMMIT, "COMMIT", false, false)
PG_CMDTAG(CMDTAG_COMMIT_PREPARED, "COMMIT PREPARED", false, false)
PG_CMDTAG(CMDTAG_CREATE_DATABASE, "CREATE DATABASE", false, false)
PG_CMDTAG(CMDTAG_CREATE_EXTENSION, "CREATE EXTENSION", false, false)
PG_CMDTAG(CMDTAG_CREATE_INDEX, "CREATE INDEX", false, false)
PG_CMDTAG(CMDTAG_CREATE_RULE, "CREATE RULE", false, false)
PG_CMDTAG(CMDTAG_CREATE_SCHEMA, "CREATE SCHEMA", false, false)
PG_CMDTAG(CMDTAG_CREATE_STATISTICS, "CREATE STATISTICS", false, false)
PG_CMDTAG(CMDTAG_CREATE_TABLE, "CREATE TABLE", false, false)
PG_CMDTAG(CMDTAG_CREATE_VIEW, "CREATE VIEW", false, false)
PG_CMDTAG(CMDTAG_DELETE, "DELETE", false, true)
PG_CMDTAG(CMDTAG_DISCARD, "DISCARD", false, false)
PG_CMDTAG(CMDTAG_DISCARD_ALL, "DISCARD ALL", false, false)
PG_CMDTAG(CMDTAG_DISCARD_PLANS, "DISCARD PLANS", false, false)
PG_CMDTAG(CMDTAG_DISCARD_TEMP, "DISCARD TEMP", false, false)
PG_CMDTAG(CMDTAG_DROP_DATABASE, "DROP DATABASE", false, false)
PG_CMDTAG(CMDTAG_DROP_EXTENSION, "DROP EXTENSION", false, false)
PG_CMDTAG(CMDTAG_DROP_INDEX, "DROP INDEX", false, false)
PG_CMDTAG(CMDTAG_DROP_SCHEMA, "DROP SCHEMA", false, false)
PG_CMDTAG(CMDTAG_DROP_STATISTICS, "DROP STATISTICS", false, false)
PG_CMDTAG(CMDTAG_DROP_TABLE, "DROP TABLE", false, false)
PG_CMDTAG(CMDTAG_DROP_VIEW, "DROP VIEW", false, false)
PG_CMDTAG(CMDTAG_EXPLAIN, "EXPLAIN", false, false)
PG_CMDTAG(CMDTAG_INSERT, "INSERT", false, true)
PG_CMDTAG(CMDTAG_PREPARE_TRANSACTION, "PREPARE TRANSACTION", false, false)
PG_CMDTAG(CMDTAG_REINDEX, "REINDEX", false, false)
PG_CMDTAG(CMDTAG_RELEASE, "RELEASE", false, false)
PG_CMDTAG(CMDTAG_RESET, "RESET", false, false)
PG_CMDTAG(CMDTAG_ROLLBACK, "ROLLBACK", false, false)
PG_CMDTAG(CMDTAG_ROLLBACK_PREPARED, "ROLLBACK PREPARED", false, false)
PG_CMDTAG(CMDTAG_SAVEPOINT, "SAVEPOINT", false, false)
PG_CMDTAG(CMDTAG_SELECT, "SELECT", false, true)
PG_CMDTAG(CMDTAG_SELECT_FOR_KEY_SHARE, "SELECT FOR KEY SHARE", false, false)
PG_CMDTAG(CMDTAG_SELECT_FOR_NO_KEY_UPDATE, "SELECT FOR NO KEY UPDATE", false, false)
PG_CMDTAG(CMDTAG_SELECT_FOR_SHARE, "SELECT FOR SHARE", false, false)
PG_CMDTAG(CMDTAG_SELECT_FOR_UPDATE, "SELECT FOR UPDATE", false, false)
PG_CMDTAG(CMDTAG_SET, "SET", false, false)
PG_CMDTAG(CMDTAG_SHOW, "SHOW", false, false)
PG_CMDTAG(CMDTAG_START_TRANSACTION, "START TRANSACTION", false, false)
PG_CMDTAG(CMDTAG_TRUNCATE_TABLE, "TRUNCATE TABLE", false, false)
PG_CMDTAG(CMDTAG_UPDATE, "UPDATE", false, true)
PG_CMDTAG(CMDTAG_VACUUM, "VACUUM", false, false)
