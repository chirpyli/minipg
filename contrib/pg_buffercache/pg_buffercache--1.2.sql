/* contrib/pg_buffercache/pg_buffercache--1.2.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_buffercache" to load this file. \quit

-- Register the function.
-- minipg: function-in-FROM（含列定义列表）已裁剪，函数改用 OUT 参数声明，
-- 这样结果记录类型是有名字的，可以在子查询目标列中展开。
CREATE FUNCTION pg_buffercache_pages(
	bufferid OUT integer, relfilenode OUT oid, reltablespace OUT oid,
	reldatabase OUT oid, relforknumber OUT int2, relblocknumber OUT int8,
	isdirty OUT bool, usagecount OUT int2, pinning_backends OUT int4)
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'pg_buffercache_pages'
LANGUAGE C PARALLEL SAFE;

-- Create a view for convenient access.
CREATE VIEW pg_buffercache AS
	SELECT * FROM (SELECT (pg_buffercache_pages()).* ) AS P;

-- Don't want these to be available to public.
