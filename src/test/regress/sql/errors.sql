--
-- ERRORS
--

-- bad in postquel, but ok in PostgreSQL
select 1;


--
-- UNSUPPORTED STUFF

-- doesn't work
-- notify pg_class
--

--
-- SELECT

-- this used to be a syntax error, but now we allow an empty target list
select;

-- no such relation
select * from nonesuch;

-- bad name in target list
select nonesuch from pg_database;

-- empty distinct list isn't OK
select distinct from pg_database;

-- bad attribute name on lhs of operator
select * from pg_database where nonesuch = pg_database.datname;

-- bad attribute name on rhs of operator
select * from pg_database where pg_database.datname = nonesuch;

-- bad attribute name in select distinct on
select distinct on (foobar) * from pg_database;

-- grouping with FOR UPDATE
select null from pg_database group by datname for update;
select null from pg_database group by grouping sets (()) for update;


--
-- DELETE

-- missing relation name (this had better not wildcard!)
delete from;

-- no such relation
delete from nonesuch;


--
-- DROP

-- missing relation name (this had better not wildcard!)
drop table;

-- no such relation
drop table nonesuch;


--
-- ALTER TABLE

-- minipg: ALTER TABLE ... RENAME 与继承表（emp/stud_emp）均已裁剪，
-- 改用仍支持的 ADD COLUMN 触发同类报错

-- missing relation name
alter table add column;

-- no such relation
alter table nonesuch add column newnonesuch int;

-- column already exists
alter table aggtest add column a int;

-- reserved column name
alter table aggtest add column ctid int;


--
-- TRANSACTION STUFF

-- not in a xact
abort;

-- not in a xact
end;


--
-- DROP INDEX

-- missing index name
drop index;

-- bad index name
drop index 314159;

-- no such index
drop index nonesuch;


--
-- minipg: DROP FUNCTION 已裁剪，改为 DROP VIEW 验证同类报错
-- DROP VIEW

-- missing view name
drop view;

-- bad view name
drop view 314159;

-- no such view
drop view nonesuch;


--
-- minipg: DROP TYPE 已裁剪，改为 DROP SCHEMA 验证同类报错
-- DROP SCHEMA

-- missing schema name
drop schema;

-- bad schema name
drop schema 314159;

-- no such schema
drop schema nonesuch;


--
-- Check that division-by-zero is properly caught.
--

select 1/0;

select 1::int8/0;

select 1/0::int8;

select 1::int2/0;

select 1/0::int2;

select 1::int8/0;

select 1/0::int8;

select 1::float8/0;

select 1/0::float8;

select 1::float4/0;

select 1/0::float4;


--
-- Test psql's reporting of syntax error location
--

xxx;

CREATE foo;

CREATE TABLE ;

CREATE TABLE
\g

INSERT INTO foo VALUES(123) foo;

INSERT INTO 123
VALUES(123);

INSERT INTO foo
VALUES(123) 123
;

-- with a tab
CREATE TABLE foo
  (id INT4 UNIQUE, id2 TEXT PRIMARY KEY,
	id3 INTEGER NUL,
   id4 INT4 UNIQUE, id5 TEXT UNIQUE);

-- long line to be truncated on the left
CREATE TABLE foo(id INT4 UNIQUE, id2 TEXT PRIMARY KEY, id3 INTEGER NUL,
id4 INT4 UNIQUE, id5 TEXT UNIQUE);

-- long line to be truncated on the right
CREATE TABLE foo(
id3 INTEGER NUL, id4 INT4 UNIQUE, id5 TEXT UNIQUE, id INT4 UNIQUE, id2 TEXT PRIMARY KEY);

-- long line to be truncated both ways
CREATE TABLE foo(id INT4 UNIQUE, id2 TEXT PRIMARY KEY, id3 INTEGER NUL, id4 INT4 UNIQUE, id5 TEXT UNIQUE);

-- long line to be truncated on the left, many lines
CREATE
TABLE
foo(id INT4 UNIQUE, id2 TEXT PRIMARY KEY, id3 INTEGER NUL,
id4 INT4
UNIQUE,
id5 TEXT
UNIQUE)
;

-- long line to be truncated on the right, many lines
CREATE
TABLE
foo(
id3 INTEGER NUL, id4 INT4 UNIQUE, id5 TEXT UNIQUE, id INT4 UNIQUE, id2 TEXT PRIMARY KEY)
;

-- long line to be truncated both ways, many lines
CREATE
TABLE
foo
(id
INT4
UNIQUE, idx INT4 UNIQUE, idy INT4 UNIQUE, id2 TEXT PRIMARY KEY, id3 INTEGER NUL, id4 INT4 UNIQUE, id5 TEXT UNIQUE,
idz INT4 UNIQUE,
idv INT4 UNIQUE);

-- more than 10 lines...
CREATE
TABLE
foo
(id
INT4
UNIQUE
,
idm
INT4
UNIQUE,
idx INT4 UNIQUE, idy INT4 UNIQUE, id2 TEXT PRIMARY KEY, id3 INTEGER NUL, id4 INT4 UNIQUE, id5 TEXT UNIQUE,
idz INT4 UNIQUE,
idv
INT4
UNIQUE);
