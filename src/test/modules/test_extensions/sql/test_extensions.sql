CREATE SCHEMA has$dollar;

-- test some errors
CREATE EXTENSION test_ext1;
CREATE EXTENSION test_ext1 SCHEMA test_ext1;
CREATE EXTENSION test_ext1 SCHEMA test_ext;
CREATE EXTENSION test_ext1 SCHEMA has$dollar;

-- finally success
CREATE EXTENSION test_ext1 SCHEMA has$dollar CASCADE;

SELECT extname, nspname, extversion, extrelocatable FROM pg_extension e, pg_namespace n WHERE extname LIKE 'test_ext%' AND e.extnamespace = n.oid ORDER BY 1;

CREATE EXTENSION test_ext_cyclic1 CASCADE;

DROP SCHEMA has$dollar CASCADE;
CREATE SCHEMA has$dollar;

CREATE EXTENSION test_ext6;
DROP EXTENSION test_ext6;
CREATE EXTENSION test_ext6;

-- test dropping of member tables that own extensions:
-- this table will be absorbed into test_ext7
create table old_table1 (col1 serial primary key);
create extension test_ext7;
\dx+ test_ext7
alter extension test_ext7 update to '2.0';
\dx+ test_ext7

-- test handling of objects created by extensions
-- 注：minipg 已移除临时表功能，原测试依赖临时对象的场景已改写为永久对象
create extension test_ext8;

-- \dx+ would expose a variable schema name, so we can't use it here
-- Skipped: regexp_replace to normalize schema names requires regex,
-- which has been removed from minipg.
select 'skipped: requires regex'::text as "Object description";

-- Should be possible to drop and recreate this extension
drop extension test_ext8;
create extension test_ext8;

-- Skipped: regexp_replace requires regex, which has been removed from minipg.
select 'skipped: requires regex'::text as "Object description";

-- extension should now contain the created objects
\dx+ test_ext8

-- dropping it should still work
drop extension test_ext8;

-- Test creation of extension in a non-default (regular) schema with two-phase
-- commit, which should work.  This function wrapper is useful for portability.
-- 注：minipg 已移除临时表/TEMP 命名空间，原 "temporary schema" 场景改为普通 schema。

-- Avoid noise caused by CONTEXT and NOTICE messages including the schema name.
\set SHOW_CONTEXT never
SET client_min_messages TO 'warning';
CREATE SCHEMA test_ext4_ns;
CREATE OR REPLACE FUNCTION create_extension_with_schema()
  RETURNS VOID AS $$
  DECLARE
    myschema text;
    query text;
  BEGIN
    SELECT INTO myschema 'test_ext4_ns';
    query := 'CREATE EXTENSION test_ext4 SCHEMA ' || myschema || ' CASCADE;';
    RAISE NOTICE 'query %', query;
    EXECUTE query;
  END; $$ LANGUAGE plpgsql;
BEGIN;
SELECT create_extension_with_schema();
PREPARE TRANSACTION 'twophase_extension';
-- Clean up
ROLLBACK PREPARED 'twophase_extension';
DROP SCHEMA test_ext4_ns CASCADE;
DROP FUNCTION create_extension_with_schema();
RESET client_min_messages;
\unset SHOW_CONTEXT

-- It's generally bad style to use CREATE OR REPLACE unnecessarily.
-- Test what happens if an extension does it anyway.
-- Replacing a shell type or operator is sort of like CREATE OR REPLACE;
-- check that too.

CREATE FUNCTION ext_cor_func() RETURNS text
  AS $$ SELECT 'ext_cor_func: original'::text $$ LANGUAGE sql;

CREATE EXTENSION test_ext_cor;  -- fail

SELECT ext_cor_func();

DROP FUNCTION ext_cor_func();

CREATE VIEW ext_cor_view AS
  SELECT 'ext_cor_view: original'::text AS col;

CREATE EXTENSION test_ext_cor;  -- fail

SELECT ext_cor_func();

SELECT * FROM ext_cor_view;

DROP VIEW ext_cor_view;

CREATE TYPE test_ext_type;

CREATE EXTENSION test_ext_cor;  -- fail

DROP TYPE test_ext_type;


\dx+ test_ext_cor

--
-- CREATE IF NOT EXISTS is an entirely unsound thing for an extension
-- to be doing, but let's at least plug the major security hole in it.
--

CREATE COLLATION ext_cine_coll
  ( LC_COLLATE = "C", LC_CTYPE = "C" );

CREATE EXTENSION test_ext_cine;  -- fail

DROP COLLATION ext_cine_coll;

CREATE FOREIGN DATA WRAPPER dummy;

CREATE SERVER ext_cine_srv FOREIGN DATA WRAPPER dummy;

CREATE EXTENSION test_ext_cine;  -- fail

DROP SERVER ext_cine_srv;

CREATE SCHEMA ext_cine_schema;

CREATE EXTENSION test_ext_cine;  -- fail

DROP SCHEMA ext_cine_schema;

CREATE SEQUENCE ext_cine_seq;

CREATE EXTENSION test_ext_cine;  -- fail

DROP SEQUENCE ext_cine_seq;

CREATE TABLE ext_cine_tab1 (x int);

CREATE EXTENSION test_ext_cine;  -- fail

DROP TABLE ext_cine_tab1;

CREATE TABLE ext_cine_tab2 (y int);
INSERT INTO ext_cine_tab2 SELECT 42 AS y;

CREATE EXTENSION test_ext_cine;  -- fail

DROP TABLE ext_cine_tab2;

CREATE EXTENSION test_ext_cine;

\dx+ test_ext_cine

ALTER EXTENSION test_ext_cine UPDATE TO '1.1';

\dx+ test_ext_cine

--
-- Test @extschema@ syntax.
--
CREATE SCHEMA "has space";
CREATE EXTENSION test_ext_extschema SCHEMA has$dollar;
CREATE EXTENSION test_ext_extschema SCHEMA "has space";
