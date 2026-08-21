/*-------------------------------------------------------------------------
 *
 * tablespace.c
 *	  Helpers for the built-in tablespaces (pg_default / pg_global)
 *
 * 表空间管理语句（CREATE/DROP/ALTER TABLESPACE）已裁剪，本文件仅保留
 * 内核运行所需的表空间基础能力：
 *
 *	- TablespaceCreateDbspace：按需创建 per-database 子目录（md.c 依赖）
 *	- GetDefaultTablespace：恒返回数据库默认表空间
 *	- PrepareTempTablespaces：临时文件固定使用数据库默认表空间
 *	- get_tablespace_name：pg_tablespace 目录查询
 *	- directory_is_empty / remove_tablespace_symlink：文件系统工具函数
 *
 * 集群初始化后 pg_tablespace 中仅存在 pg_global 与 pg_default 两个内建
 * 表空间，分别映射 $PGDATA/global 与 $PGDATA/base。
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

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"
#include "common/file_perm.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/varlena.h"


/*
 * Each database using a table space is isolated into its own name space
 * by a subdirectory named for the database OID.  On first creation of an
 * object in the tablespace, create the subdirectory.  If the subdirectory
 * already exists, fall through quietly.
 *
 * isRedo indicates that we are creating an object during WAL replay.
 * In this case we will cope with the possibility of the tablespace
 * directory not being there either --- this could happen if we are
 * replaying an operation on a table in a subsequently-dropped tablespace.
 * We handle this by making a directory in the place where the tablespace
 * symlink would normally be.  This isn't an exact replay of course, but
 * it's the best we can do given the available information.
 *
 * If tablespaces are not supported, we still need it in case we have to
 * re-create a database subdirectory (of $PGDATA/base) during WAL replay.
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

	Assert(OidIsValid(spcNode));
	Assert(OidIsValid(dbNode));

	dir = GetDatabasePath(dbNode, spcNode);

	if (stat(dir, &st) < 0)
	{
		/* Directory does not exist? */
		if (errno == ENOENT)
		{
			/*
			 * Acquire TablespaceCreateLock to ensure that no DROP TABLESPACE
			 * or TablespaceCreateDbspace is running concurrently.
			 */
			LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);

			/*
			 * Recheck to see if someone created the directory while we were
			 * waiting for lock.
			 */
			if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
			{
				/* Directory was created */
			}
			else
			{
				/* Directory creation failed? */
				if (MakePGDirectory(dir) < 0)
				{
					/* Failure other than not exists or not in WAL replay? */
					if (errno != ENOENT || !isRedo)
						ereport(ERROR,
								(errcode_for_file_access(),
								 errmsg("could not create directory \"%s\": %m",
										dir)));

					/*
					 * During WAL replay, it's conceivable that several levels
					 * of directories are missing if tablespaces are dropped
					 * further ahead of the WAL stream than we're currently
					 * replaying.  An easy way forward is to create them as
					 * plain directories and hope they are removed by further
					 * WAL replay if necessary.  If this also fails, there is
					 * trouble we cannot get out of, so just report that and
					 * bail out.
					 */
					if (pg_mkdir_p(dir, pg_dir_create_mode) < 0)
						ereport(ERROR,
								(errcode_for_file_access(),
								 errmsg("could not create directory \"%s\": %m",
										dir)));
				}
			}

			LWLockRelease(TablespaceCreateLock);
		}
		else
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat directory \"%s\": %m", dir)));
		}
	}
	else
	{
		/* Is it not a directory? */
		if (!S_ISDIR(st.st_mode))
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

/*
 *	remove_tablespace_symlink
 *
 * This function removes symlinks in pg_tblspc.  On Windows, junction points
 * act like directories so we must be able to apply rmdir.  This function
 * works like the symlink removal code in destroy_tablespace_directories,
 * except that failure to remove is always an ERROR.  But if the file doesn't
 * exist at all, that's OK.
 */
void
remove_tablespace_symlink(const char *linkloc)
{
	struct stat st;

	if (lstat(linkloc, &st) < 0)
	{
		if (errno == ENOENT)
			return;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", linkloc)));
	}

	if (S_ISDIR(st.st_mode))
	{
		/*
		 * This will fail if the directory isn't empty, but not if it's a
		 * junction point.
		 */
		if (rmdir(linkloc) < 0 && errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m",
							linkloc)));
	}
	else if (S_ISLNK(st.st_mode))
	{
		if (unlink(linkloc) < 0 && errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove symbolic link \"%s\": %m",
							linkloc)));
	}
	else
	{
		/* Refuse to remove anything that's not a directory or symlink */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("\"%s\" is not a directory or symbolic link",
						linkloc)));
	}
}


/*
 * GetDefaultTablespace -- get the OID of the current default tablespace
 *
 * 表空间管理已裁剪：所有对象一律使用数据库默认表空间（pg_default），
 * 因此本函数固定返回数据库默认表空间 OID。
 */
Oid
GetDefaultTablespace(char relpersistence, bool partitioned)
{
	(void) relpersistence;		/* 临时表已裁剪，relpersistence 恒为 PERMANENT */
	(void) partitioned;			/* 分区表表空间指定已裁剪 */

	/*
	 * 表空间管理已裁剪：返回 InvalidOid 表示使用数据库默认表空间
	 * （与 postgres 默认表空间 GUC 为空时的行为一致）。
	 */
	return InvalidOid;
}


/*
 * Routines for handling the GUC variable 'temp_tablespaces'.
 */

/*
 * PrepareTempTablespaces -- prepare to use temp tablespaces
 *
 * 表空间管理已裁剪：临时文件一律使用数据库默认表空间，因此本函数
 * 直接让 fd.c 使用当前数据库的默认表空间。
 */
void
PrepareTempTablespaces(void)
{
	/* No work if already done in current transaction */
	if (TempTablespacesAreSet())
		return;

	/*
	 * Can't do catalog access unless within a transaction.  This is just a
	 * safety check in case this function is called by low-level code that
	 * could conceivably execute outside a transaction.  Note that in such a
	 * scenario, fd.c will fall back to using the current database's default
	 * tablespace, which should always be OK.
	 */
	if (!IsTransactionState())
		return;

	/*
	 * 临时表空间 GUC（temp_tablespaces）已裁剪，固定使用数据库默认表空间。
	 * 空列表（InvalidOid 表示数据库默认表空间）即满足需求。
	 */
	SetTempTablespaces(NULL, 0);
}


/*
 * get_tablespace_name - given a tablespace OID, look up the name
 *
 * Returns a palloc'd string, or NULL if no such tablespace.
 */
char *
get_tablespace_name(Oid spc_oid)
{
	char	   *result;
	Relation	rel;
	TableScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/*
	 * Search pg_tablespace.  We use a heapscan here even though there is an
	 * index on oid, on the theory that pg_tablespace will usually have just a
	 * few entries and so an indexed lookup is a waste of effort.
	 */
	rel = table_open(TableSpaceRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(spc_oid));
	scandesc = table_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = pstrdup(NameStr(((Form_pg_tablespace) GETSTRUCT(tuple))->spcname));
	else
		result = NULL;

	table_endscan(scandesc);
	table_close(rel, AccessShareLock);

	return result;
}
