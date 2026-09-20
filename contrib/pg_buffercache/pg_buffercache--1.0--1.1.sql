/* contrib/pg_buffercache/pg_buffercache--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_buffercache UPDATE TO '1.1'" to load this file. \quit

-- Upgrade view to 1.1. format
-- minipg: function-in-FROM（含列定义列表）已裁剪，改为在子查询目标列中展开记录。
CREATE OR REPLACE VIEW pg_buffercache AS
	SELECT * FROM (SELECT (pg_buffercache_pages()).* ) AS P;
