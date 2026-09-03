/*-------------------------------------------------------------------------
 *
 * tablespace.h
 *		Helpers for the built-in tablespaces (pg_default / pg_global).
 *
 *		用户自建表空间（create/drop/alter tablespace）已裁剪，仅保留
 *		内建表空间所需的最小接口。
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/tablespace.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLESPACE_H
#define TABLESPACE_H

extern void TablespaceCreateDbspace(Oid spcNode, Oid dbNode, bool isRedo);

extern void PrepareTempTablespaces(void);

extern char *get_tablespace_name(Oid spc_oid);

extern bool directory_is_empty(const char *path);
extern void remove_tablespace_symlink(const char *linkloc);

#endif							/* TABLESPACE_H */
