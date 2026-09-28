/*-------------------------------------------------------------------------
 *
 * tablespace.c
 *	  Helpers for the built-in tablespaces (pg_default / pg_global)
 *
 * minipg 已裁剪用户表空间：CREATE/DROP/ALTER TABLESPACE 语句、表空间选项
 * （spcoptions）、pg_tablespace_size 等函数以及 pg_tblspc 符号链接目录都已
 * 移除。集群初始化后 pg_tablespace 中仅存在 pg_global 与 pg_default 两个
 * 内建表空间，分别映射 $PGDATA/global 与 $PGDATA/base。
 *
 * 本文件仅保留内核运行所需的基础能力：
 *
 *	- TablespaceCreateDbspace：按需创建 per-database 子目录（md.c 依赖）
 *	- directory_is_empty：目录为空判断（CREATE DATABASE 复制库时使用）
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/tablespace.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"
#include "common/file_perm.h"
#include "common/relpath.h"
#include "storage/fd.h"


/*
 * Each database is isolated into its own name space by a subdirectory named
 * for the database OID, under $PGDATA/base.  On first creation of an object
 * in the database, create the subdirectory.  If the subdirectory already
 * exists, fall through quietly.
 *
 * isRedo indicates that we are creating an object during WAL replay.
 *
 * minipg：用户表空间已裁剪，只有 pg_default（$PGDATA/base）需要 per-database
 * 子目录，pg_global 没有子目录；DROP TABLESPACE 也不复存在，因此这里不再需要
 * TablespaceCreateLock 来防止并发删除。
 */
void
TablespaceCreateDbspace(Oid spcNode, Oid dbNode, bool isRedo)
{
	struct stat st;
	char	   *dir;

	/*
	 * The global tablespace doesn't have per-database subdirectories, so
	 * nothing to do for it.
	 */
	if (spcNode == GLOBALTABLESPACE_OID)
		return;

	Assert(spcNode == DEFAULTTABLESPACE_OID);
	Assert(OidIsValid(dbNode));

	dir = GetDatabasePath(dbNode, spcNode);

	if (stat(dir, &st) < 0)
	{
		/* Directory does not exist? */
		if (errno == ENOENT)
		{
			/* Directory creation failed? */
			if (MakePGDirectory(dir) < 0)
			{
				/*
				 * During WAL replay, it's conceivable that the database was
				 * dropped further ahead of the WAL stream than we're
				 * currently replaying, so the directory is legitimately
				 * gone.  Tolerate that case, but report anything else.
				 */
				if (errno != ENOENT || !isRedo)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not create directory \"%s\": %m",
									dir)));
			}
		}
		else
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat directory \"%s\": %m", dir)));
		}
	}
	else if (!S_ISDIR(st.st_mode))
	{
		/* Is it not a directory? */
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" exists but is not a directory",
						dir)));
	}

	pfree(dir);
}


/*
 * Check if a directory is empty.
 *
 * This probably belongs somewhere else, but not sure where...
 */
bool
directory_is_empty(const char *path)
{
	DIR		   *dirdesc;
	struct dirent *de;

	dirdesc = AllocateDir(path);

	while ((de = ReadDir(dirdesc, path)) != NULL)
	{
		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;
		FreeDir(dirdesc);
		return false;
	}

	FreeDir(dirdesc);
	return true;
}
