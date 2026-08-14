--
-- Tests for things affected by allow_system_table_mods
--
-- We run the same set of commands once with allow_system_table_mods
-- off and then again with on.
--
-- The "on" tests should where possible be wrapped in BEGIN/ROLLBACK
-- blocks so as to not leave a mess around.

CREATE USER regress_user_ast;

SET allow_system_table_mods = off;

-- create new table in pg_catalog
CREATE TABLE pg_catalog.test (a int);

-- anyarray column
CREATE TABLE t1x (a int, b anyarray);

-- index on system catalog
ALTER TABLE pg_namespace ADD CONSTRAINT foo UNIQUE USING INDEX pg_namespace_nspname_index;

-- reserved schema name
CREATE SCHEMA pg_foo;

-- reserved tablespace name
CREATE TABLESPACE pg_foo LOCATION '/no/such/location';

-- minipg: PL/pgSQL removed. The original also created a plpgsql trigger on a
-- system catalog and renamed it; that trigger-based portion is dropped.

-- cleanup:
SET allow_system_table_mods TO on;
RESET allow_system_table_mods;


SET allow_system_table_mods = on;

-- create new table in pg_catalog
BEGIN;
CREATE TABLE pg_catalog.test (a int);
ROLLBACK;

-- anyarray column
BEGIN;
CREATE TABLE t1 (a int, b anyarray);
ROLLBACK;

-- index on system catalog
BEGIN;
ALTER TABLE pg_namespace ADD CONSTRAINT foo UNIQUE USING INDEX pg_namespace_nspname_index;
ROLLBACK;


-- reserved schema name
BEGIN;
CREATE SCHEMA pg_foo;
ROLLBACK;

-- reserved tablespace name
SET client_min_messages = error;  -- disable ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS warning
CREATE TABLESPACE pg_foo LOCATION '/no/such/location';
RESET client_min_messages;


-- cleanup
DROP USER regress_user_ast;
DROP FUNCTION tf1;
