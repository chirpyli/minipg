/* contrib/pageinspect/pageinspect--1.8--1.9.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pageinspect UPDATE TO '1.9'" to load this file. \quit

--
-- get_raw_page()
--
DROP FUNCTION get_raw_page(text, int4);
CREATE FUNCTION get_raw_page(text, int8)
RETURNS bytea
AS 'MODULE_PATHNAME', 'get_raw_page_1_9'
LANGUAGE C STRICT PARALLEL SAFE;

DROP FUNCTION get_raw_page(text, text, int4);
CREATE FUNCTION get_raw_page(text, text, int8)
RETURNS bytea
AS 'MODULE_PATHNAME', 'get_raw_page_fork_1_9'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- bt_metap()
--
DROP FUNCTION bt_metap(text);
CREATE FUNCTION bt_metap(IN relname text,
    OUT magic int4,
    OUT version int4,
    OUT root int8,
    OUT level int8,
    OUT fastroot int8,
    OUT fastlevel int8,
    OUT last_cleanup_num_delpages int8,
    OUT last_cleanup_num_tuples float8,
    OUT allequalimage boolean)
AS 'MODULE_PATHNAME', 'bt_metap'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- bt_page_stats()
--
DROP FUNCTION bt_page_stats(text, int4);
CREATE FUNCTION bt_page_stats(IN relname text, IN blkno int8,
    OUT blkno int8,
    OUT type "char",
    OUT live_items int4,
    OUT dead_items int4,
    OUT avg_item_size int4,
    OUT page_size int4,
    OUT free_size int4,
    OUT btpo_prev int8,
    OUT btpo_next int8,
    OUT btpo_level int8,
    OUT btpo_flags int4)
AS 'MODULE_PATHNAME', 'bt_page_stats_1_9'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- bt_page_items()
--
DROP FUNCTION bt_page_items(text, int4);
CREATE FUNCTION bt_page_items(IN relname text, IN blkno int8,
    OUT itemoffset smallint,
    OUT ctid tid,
    OUT itemlen smallint,
    OUT nulls bool,
    OUT vars bool,
    OUT data text,
    OUT dead boolean,
    OUT htid tid,
    OUT tids tid[])
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'bt_page_items_1_9'
LANGUAGE C STRICT PARALLEL SAFE;

