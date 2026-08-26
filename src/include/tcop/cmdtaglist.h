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

/* symbol name, textual name, event_trigger_ok, table_rewrite_ok, rowcount */
PG_CMDTAG(CMDTAG_UNKNOWN, "???", false, false, false)
PG_CMDTAG(CMDTAG_ALTER_INDEX, "ALTER INDEX", true, false, false)
PG_CMDTAG(CMDTAG_ALTER_TABLE, "ALTER TABLE", true, true, false)
PG_CMDTAG(CMDTAG_ANALYZE, "ANALYZE", false, false, false)
PG_CMDTAG(CMDTAG_BEGIN, "BEGIN", false, false, false)
PG_CMDTAG(CMDTAG_CHECKPOINT, "CHECKPOINT", false, false, false)
PG_CMDTAG(CMDTAG_CLUSTER, "CLUSTER", false, false, false)
PG_CMDTAG(CMDTAG_COMMIT, "COMMIT", false, false, false)
PG_CMDTAG(CMDTAG_COMMIT_PREPARED, "COMMIT PREPARED", false, false, false)
PG_CMDTAG(CMDTAG_CREATE_DATABASE, "CREATE DATABASE", false, false, false)
PG_CMDTAG(CMDTAG_CREATE_EXTENSION, "CREATE EXTENSION", true, false, false)
PG_CMDTAG(CMDTAG_CREATE_INDEX, "CREATE INDEX", true, false, false)
PG_CMDTAG(CMDTAG_CREATE_ROUTINE, "CREATE ROUTINE", true, false, false)
PG_CMDTAG(CMDTAG_CREATE_RULE, "CREATE RULE", true, false, false)
PG_CMDTAG(CMDTAG_CREATE_SCHEMA, "CREATE SCHEMA", true, false, false)
PG_CMDTAG(CMDTAG_CREATE_STATISTICS, "CREATE STATISTICS", true, false, false)
PG_CMDTAG(CMDTAG_CREATE_TABLE, "CREATE TABLE", true, false, false)
PG_CMDTAG(CMDTAG_CREATE_TRANSFORM, "CREATE TRANSFORM", true, false, false)
PG_CMDTAG(CMDTAG_CREATE_VIEW, "CREATE VIEW", true, false, false)
PG_CMDTAG(CMDTAG_DEALLOCATE, "DEALLOCATE", false, false, false)
PG_CMDTAG(CMDTAG_DEALLOCATE_ALL, "DEALLOCATE ALL", false, false, false)
PG_CMDTAG(CMDTAG_DELETE, "DELETE", false, false, true)
PG_CMDTAG(CMDTAG_DISCARD, "DISCARD", false, false, false)
PG_CMDTAG(CMDTAG_DISCARD_ALL, "DISCARD ALL", false, false, false)
PG_CMDTAG(CMDTAG_DISCARD_PLANS, "DISCARD PLANS", false, false, false)
PG_CMDTAG(CMDTAG_DISCARD_TEMP, "DISCARD TEMP", false, false, false)
PG_CMDTAG(CMDTAG_DROP_DATABASE, "DROP DATABASE", false, false, false)
PG_CMDTAG(CMDTAG_DROP_EXTENSION, "DROP EXTENSION", true, false, false)
PG_CMDTAG(CMDTAG_DROP_INDEX, "DROP INDEX", true, false, false)
PG_CMDTAG(CMDTAG_DROP_SCHEMA, "DROP SCHEMA", true, false, false)
PG_CMDTAG(CMDTAG_DROP_STATISTICS, "DROP STATISTICS", true, false, false)
PG_CMDTAG(CMDTAG_DROP_TABLE, "DROP TABLE", true, false, false)
PG_CMDTAG(CMDTAG_DROP_TRANSFORM, "DROP TRANSFORM", true, false, false)
PG_CMDTAG(CMDTAG_DROP_VIEW, "DROP VIEW", true, false, false)
PG_CMDTAG(CMDTAG_EXECUTE, "EXECUTE", false, false, false)
PG_CMDTAG(CMDTAG_EXPLAIN, "EXPLAIN", false, false, false)
PG_CMDTAG(CMDTAG_INSERT, "INSERT", false, false, true)
PG_CMDTAG(CMDTAG_PREPARE, "PREPARE", false, false, false)
PG_CMDTAG(CMDTAG_PREPARE_TRANSACTION, "PREPARE TRANSACTION", false, false, false)
PG_CMDTAG(CMDTAG_REINDEX, "REINDEX", false, false, false)
PG_CMDTAG(CMDTAG_RELEASE, "RELEASE", false, false, false)
PG_CMDTAG(CMDTAG_RESET, "RESET", false, false, false)
PG_CMDTAG(CMDTAG_ROLLBACK, "ROLLBACK", false, false, false)
PG_CMDTAG(CMDTAG_ROLLBACK_PREPARED, "ROLLBACK PREPARED", false, false, false)
PG_CMDTAG(CMDTAG_SAVEPOINT, "SAVEPOINT", false, false, false)
PG_CMDTAG(CMDTAG_SELECT, "SELECT", false, false, true)
PG_CMDTAG(CMDTAG_SELECT_FOR_KEY_SHARE, "SELECT FOR KEY SHARE", false, false, false)
PG_CMDTAG(CMDTAG_SELECT_FOR_NO_KEY_UPDATE, "SELECT FOR NO KEY UPDATE", false, false, false)
PG_CMDTAG(CMDTAG_SELECT_FOR_SHARE, "SELECT FOR SHARE", false, false, false)
PG_CMDTAG(CMDTAG_SELECT_FOR_UPDATE, "SELECT FOR UPDATE", false, false, false)
PG_CMDTAG(CMDTAG_SET, "SET", false, false, false)
PG_CMDTAG(CMDTAG_SHOW, "SHOW", false, false, false)
PG_CMDTAG(CMDTAG_START_TRANSACTION, "START TRANSACTION", false, false, false)
PG_CMDTAG(CMDTAG_TRUNCATE_TABLE, "TRUNCATE TABLE", false, false, false)
PG_CMDTAG(CMDTAG_UPDATE, "UPDATE", false, false, true)
PG_CMDTAG(CMDTAG_VACUUM, "VACUUM", false, false, false)
