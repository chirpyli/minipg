--
-- IF EXISTS tests
--

-- table (will be really dropped at the end)

DROP TABLE test_exists;

DROP TABLE IF EXISTS test_exists;

CREATE TABLE test_exists (a int, b text);

-- view

DROP VIEW test_view_exists;

DROP VIEW IF EXISTS test_view_exists;

CREATE VIEW test_view_exists AS select * from test_exists;

DROP VIEW IF EXISTS test_view_exists;

DROP VIEW test_view_exists;

-- index

DROP INDEX test_index_exists;

DROP INDEX IF EXISTS test_index_exists;

CREATE INDEX test_index_exists on test_exists(a);

DROP INDEX IF EXISTS test_index_exists;

DROP INDEX test_index_exists;

-- minipg: SEQUENCE 已裁剪，DROP SEQUENCE 语法一并移除

-- schema

DROP SCHEMA test_schema_exists;

DROP SCHEMA IF EXISTS test_schema_exists;

CREATE SCHEMA test_schema_exists;

DROP SCHEMA IF EXISTS test_schema_exists;

DROP SCHEMA test_schema_exists;

-- minipg: CREATE/DROP TYPE、角色、COLLATION、全文检索对象均已裁剪，相关用例移除

-- extension

DROP EXTENSION test_extension_exists;
DROP EXTENSION IF EXISTS test_extension_exists;

-- minipg: CREATE/DROP FUNCTION 与 CREATE/DROP TRIGGER 已裁剪，相关用例移除

-- drop the table

DROP TABLE IF EXISTS test_exists;

DROP TABLE test_exists;

-- be tolerant with missing schemas, types, etc

DROP INDEX IF EXISTS no_such_schema.foo;
DROP TABLE IF EXISTS no_such_schema.foo;
DROP VIEW IF EXISTS no_such_schema.foo;

-- minipg: 歧义函数名用例依赖 CREATE FUNCTION，已移除

-- This test checks both the functionality of 'if exists' and the syntax
-- of the drop database command.
drop database test_database_exists (force);
drop database test_database_exists with (force);
drop database if exists test_database_exists (force);
drop database if exists test_database_exists with (force);
