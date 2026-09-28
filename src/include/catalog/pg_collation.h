/*-------------------------------------------------------------------------
 *
 * pg_collation.h
 *	  排序规则（collation）相关的固定 OID 常量
 *
 * minipg 已裁剪 collation 功能：COLLATE 语法、CREATE COLLATION 命令以及
 * pg_collation 系统表都已移除，所有表达式一律使用默认排序规则（C，即字节
 * 序比较，详见 pg_locale.c）。这里保留原有的 OID 常量，供表达式树中的
 * collation 字段占位使用。
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_collation.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_COLLATION_H
#define PG_COLLATION_H

/* pg_collation 中 "default" 条目原有的 OID */
#define DEFAULT_COLLATION_OID	100
/* pg_collation 中 "C" 条目原有的 OID */
#define C_COLLATION_OID			950
/* pg_collation 中 "POSIX" 条目原有的 OID */
#define POSIX_COLLATION_OID		951

#endif							/* PG_COLLATION_H */
