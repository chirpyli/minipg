--
-- CREATE_VIEW
-- Virtual class definitions
--	(this also tests the query rewrite system)
--
-- minipg: 本文件已按当前支持的语法重写（移除 geometry/继承表/临时视图/
-- reloptions/CREATE TYPE/ROW 构造器等已裁剪特性相关的用例）。

-- minipg: geometry 类型（path/point）、## 操作符、interpt_pp() 函数以及
-- 继承表 emp 均已裁剪，street / iexit / toyemp 三个视图用例移除

-- These views are left around mainly to exercise special cases in pg_dump.

CREATE TABLE view_base_table (key int PRIMARY KEY, data varchar(20));

CREATE VIEW key_dependent_view AS
   SELECT * FROM view_base_table GROUP BY key;

ALTER TABLE view_base_table DROP CONSTRAINT view_base_table_pkey;  -- fails

CREATE VIEW key_dependent_view_no_cols AS
   SELECT FROM view_base_table GROUP BY key HAVING length(data) > 0;

--
-- CREATE OR REPLACE VIEW
--

CREATE TABLE viewtest_tbl (a int, b int);
\set ECHO none
\i data/load_viewtest.sql
\set ECHO all

CREATE OR REPLACE VIEW viewtest AS
	SELECT * FROM viewtest_tbl;

CREATE OR REPLACE VIEW viewtest AS
	SELECT * FROM viewtest_tbl WHERE a > 10;

SELECT * FROM viewtest;

CREATE OR REPLACE VIEW viewtest AS
	SELECT a, b FROM viewtest_tbl WHERE a > 5 ORDER BY b DESC;

SELECT * FROM viewtest;

-- should fail
CREATE OR REPLACE VIEW viewtest AS
	SELECT a FROM viewtest_tbl WHERE a <> 20;

-- should fail
CREATE OR REPLACE VIEW viewtest AS
	SELECT 1, * FROM viewtest_tbl;

-- should fail
CREATE OR REPLACE VIEW viewtest AS
	SELECT a, b::int8 FROM viewtest_tbl;

-- should work
CREATE OR REPLACE VIEW viewtest AS
	SELECT a, b, 0 AS c FROM viewtest_tbl;

DROP VIEW viewtest;
DROP TABLE viewtest_tbl;

-- minipg: 临时表 / 临时视图、临时 SEQUENCE 均已裁剪，整个
-- "temporary views" 小节移除；这里只为后续用例准备好
-- testviewschm2 schema 与基础表

CREATE SCHEMA testviewschm2;
SET search_path TO testviewschm2, public;

CREATE TABLE tbl1 ( a int, b int);
CREATE TABLE tbl2 (c int, d int);
CREATE TABLE tbl3 (e int, f int);
CREATE TABLE tbl4 (g int, h int);

--Should be in testviewschm2
CREATE   VIEW  pubview AS SELECT * FROM tbl1 WHERE tbl1.a
BETWEEN (SELECT d FROM tbl2 WHERE c = 1) AND (SELECT e FROM tbl3 WHERE f = 2)
AND EXISTS (SELECT g FROM tbl4 LEFT JOIN tbl3 ON tbl4.h = tbl3.f);

SELECT count(*) FROM pg_class where relname = 'pubview'
AND relnamespace IN (SELECT OID FROM pg_namespace WHERE nspname = 'testviewschm2');

--
-- CREATE VIEW and WITH(...) clause
--
-- minipg: reloptions 已裁剪，CREATE VIEW 不再接受 WITH (...) 子句，
-- security_barrier 相关用例（含两个故意报错的用例）整节移除
CREATE VIEW mysecview1
       AS SELECT * FROM tbl1 WHERE a = 0;
CREATE VIEW mysecview2
       AS SELECT * FROM tbl1 WHERE a > 0;
CREATE VIEW mysecview3
       AS SELECT * FROM tbl1 WHERE a < 0;
CREATE VIEW mysecview4
       AS SELECT * FROM tbl1 WHERE a <> 0;
SELECT relname, relkind FROM pg_class
       WHERE oid in ('mysecview1'::regclass, 'mysecview2'::regclass,
                     'mysecview3'::regclass, 'mysecview4'::regclass)
       ORDER BY relname;

CREATE OR REPLACE VIEW mysecview1
       AS SELECT * FROM tbl1 WHERE a = 256;
CREATE OR REPLACE VIEW mysecview2
       AS SELECT * FROM tbl1 WHERE a > 256;
CREATE OR REPLACE VIEW mysecview3
       AS SELECT * FROM tbl1 WHERE a < 256;
CREATE OR REPLACE VIEW mysecview4
       AS SELECT * FROM tbl1 WHERE a <> 256;
SELECT relname, relkind FROM pg_class
       WHERE oid in ('mysecview1'::regclass, 'mysecview2'::regclass,
                     'mysecview3'::regclass, 'mysecview4'::regclass)
       ORDER BY relname;

-- Check that unknown literals are converted to "text" in CREATE VIEW,
-- so that we don't end up with unknown-type columns.

CREATE VIEW unspecified_types AS
  SELECT 42 as i, 42.5 as num, 'foo' as u, 'foo'::unknown as u2, null as n;
\d+ unspecified_types
SELECT * FROM unspecified_types;

-- This test checks that proper typmods are assigned in a multi-row VALUES

CREATE VIEW tt1 AS
  SELECT * FROM (
    VALUES
       ('abc'::varchar(3), '0123456789', 42, 'abcd'::varchar(4)),
       ('0123456789', 'abc'::varchar(3), 42.12, 'abc'::varchar(4))
  ) vv(a,b,c,d);
\d+ tt1
SELECT * FROM tt1;
SELECT a::varchar(3) FROM tt1;
DROP VIEW tt1;

-- minipg: ALTER TABLE ... RENAME 已裁剪，"view decompilation in the face of
-- relation renaming conflicts" 整节移除

-- Test aliasing of joins

create view view_of_joins as
select * from
  (select * from (tbl1 cross join tbl2) same) ss,
  (tbl3 cross join tbl4) same;

\d+ view_of_joins

create table tbl1a (a int, c int);
create view view_of_joins_2a as select * from tbl1 join tbl1a using (a);
create view view_of_joins_2b as select * from tbl1 join tbl1a using (a) as x;
create view view_of_joins_2c as select * from (tbl1 join tbl1a using (a)) as y;
create view view_of_joins_2d as select * from (tbl1 join tbl1a using (a) as x) as y;

select pg_get_viewdef('view_of_joins_2a', true);
select pg_get_viewdef('view_of_joins_2b', true);
select pg_get_viewdef('view_of_joins_2c', true);
select pg_get_viewdef('view_of_joins_2d', true);

-- Test view decompilation in the face of column addition/deletion/renaming

create table tt2 (a int, b int, c int);
-- minipg: numeric 已裁剪，改用 int4
create table tt3 (ax int8, b int2, c int4);
create table tt4 (ay int, b int, q int);

create view v1 as select * from tt2 natural join tt3;
create view v1a as select * from (tt2 natural join tt3) j;
create view v2 as select * from tt2 join tt3 using (b,c) join tt4 using (b);
create view v2a as select * from (tt2 join tt3 using (b,c) join tt4 using (b)) j;
create view v3 as select * from tt2 join tt3 using (b,c) full join tt4 using (b);

select pg_get_viewdef('v1', true);
select pg_get_viewdef('v1a', true);
select pg_get_viewdef('v2', true);
select pg_get_viewdef('v2a', true);
select pg_get_viewdef('v3', true);

alter table tt2 add column d int;
alter table tt2 add column e int;

select pg_get_viewdef('v1', true);
select pg_get_viewdef('v1a', true);
select pg_get_viewdef('v2', true);
select pg_get_viewdef('v2a', true);
select pg_get_viewdef('v3', true);

-- minipg: ALTER TABLE ... RENAME 已裁剪，改为新增同名列 d 触发同样的
-- 视图反解析冲突场景
alter table tt3 add column d int4;

select pg_get_viewdef('v1', true);
select pg_get_viewdef('v1a', true);
select pg_get_viewdef('v2', true);
select pg_get_viewdef('v2a', true);
select pg_get_viewdef('v3', true);

alter table tt3 add column cc int;
alter table tt3 add column e int;

select pg_get_viewdef('v1', true);
select pg_get_viewdef('v1a', true);
select pg_get_viewdef('v2', true);
select pg_get_viewdef('v2a', true);
select pg_get_viewdef('v3', true);

alter table tt2 drop column d;

select pg_get_viewdef('v1', true);
select pg_get_viewdef('v1a', true);
select pg_get_viewdef('v2', true);
select pg_get_viewdef('v2a', true);
select pg_get_viewdef('v3', true);

create table tt5 (a int, b int);
create table tt6 (c int, d int);
create view vv1 as select * from (tt5 cross join tt6) j(aa,bb,cc,dd);
select pg_get_viewdef('vv1', true);
alter table tt5 add column c int;
select pg_get_viewdef('vv1', true);
alter table tt5 add column cc int;
select pg_get_viewdef('vv1', true);
alter table tt5 drop column c;
select pg_get_viewdef('vv1', true);

create view v4 as select * from v1;
-- minipg: ALTER VIEW ... RENAME COLUMN 已裁剪，相关用例移除
select pg_get_viewdef('v1', true);
select pg_get_viewdef('v4', true);


-- Unnamed FULL JOIN USING is lots of fun too

create table tt7 (x int, xx int, y int);
alter table tt7 drop column xx;
create table tt8 (x int, z int);

-- minipg: 上游用 "SELECT * FROM (VALUES ...) v(a,b,c,d,e) UNION ALL ..." 给
-- 视图列显式命名（否则 SELECT * 里 x 出现两次，CREATE VIEW 会报
-- column "x" specified more than once）。UNION 已裁剪，改用
-- CREATE VIEW 的显式列别名达到同样效果。

create view vv2 (a, b, c, d, e) as
select * from tt7 full join tt8 using (x), tt8 tt8x;

select pg_get_viewdef('vv2', true);

create view vv3 (a, b, c, d, e, f) as
select * from
  tt7 full join tt8 using (x),
  tt7 tt7x full join tt8 tt8x using (x);

select pg_get_viewdef('vv3', true);

create view vv4 (a, b, c, d, e, f, g) as
select * from
  tt7 full join tt8 using (x),
  tt7 tt7x full join tt8 tt8x using (x) full join tt8 tt8y using (x);

select pg_get_viewdef('vv4', true);

alter table tt7 add column zz int;
alter table tt7 add column z int;
alter table tt7 drop column zz;
alter table tt8 add column z2 int;

select pg_get_viewdef('vv2', true);
select pg_get_viewdef('vv3', true);
select pg_get_viewdef('vv4', true);

-- Implicit coercions in a JOIN USING create issues similar to FULL JOIN

create table tt7a (x date, xx int, y int);
alter table tt7a drop column xx;
create table tt8a (x timestamptz, z int);

create view vv2a (a, b, c, d, e) as
select * from tt7a left join tt8a using (x), tt8a tt8ax;

select pg_get_viewdef('vv2a', true);

--
-- Also check dropping a column that existed when the view was made
--

create table tt9 (x int, xx int, y int);
create table tt10 (x int, z int);

create view vv5 as select x,y,z from tt9 join tt10 using(x);

select pg_get_viewdef('vv5', true);

alter table tt9 drop column xx;

select pg_get_viewdef('vv5', true);

--
-- Another corner case is that we might add a column to a table below a
-- JOIN USING, and thereby make the USING column name ambiguous
--

create table tt11 (x int, y int);
create table tt12 (x int, z int);
create table tt13 (z int, q int);

create view vv6 as select x,y,z,q from
  (tt11 join tt12 using(x)) join tt13 using(z);

select pg_get_viewdef('vv6', true);

alter table tt11 add column z int;

select pg_get_viewdef('vv6', true);

-- minipg: CREATE FUNCTION 已裁剪，"dropped/altered columns in a function's
-- rowtype result" 失去测试载体，整节移除
-- minipg: CREATE TYPE 与 ROW 构造器已裁剪，whole-row variables 的
-- corner case（nestedcomposite / tt15v / tt16v / tt17v / updlog 规则）整节移除

-- check display of ScalarArrayOp with a sub-select

select 'foo'::text = any(array['abc','def','foo']::text[]);
select 'foo'::text = any((select array['abc','def','foo']::text[]));  -- fail
select 'foo'::text = any((select array['abc','def','foo']::text[])::text[]);

create view tt19v as
select 'foo'::text = any(array['abc','def','foo']::text[]) c1,
       'foo'::text = any((select array['abc','def','foo']::text[])::text[]) c2;
select pg_get_viewdef('tt19v', true);

-- check display of assorted RTE_FUNCTION expressions

create view tt20v as
select * from
  coalesce(1,2) as c,
-- minipg: COLLATION FOR 已随 COLLATE 一并裁剪
  current_date as d,
  localtimestamp(3) as t,
  cast(1+2 as int4) as i4,
  cast(1+2 as int8) as i8;
select pg_get_viewdef('tt20v', true);

-- reverse-listing of various special function syntaxes required by SQL

-- minipg: interval 类型已裁剪，AT TIME ZONE / OVERLAPS 相关列移除
create view tt201v as
select
  extract(day from now()) as extr,
  overlay('foo' placing 'bar' from 2) as ovl,
  overlay('foo' placing 'bar' from 2 for 3) as ovl2,
  position('foo' in 'foobar') as p,
  substring('foo' from 2 for 3) as s,
  trim(' ' from ' foo ') as bt,
  trim(leading ' ' from ' foo ') as lt,
  trim(trailing ' foo ') as rt,
  trim(E'\\000'::bytea from E'\\000Tom\\000'::bytea) as btb,
  trim(leading E'\\000'::bytea from E'\\000Tom\\000'::bytea) as ltb,
  trim(trailing E'\\000'::bytea from E'\\000Tom\\000'::bytea) as rtb;
select pg_get_viewdef('tt201v', true);

-- corner cases with empty join conditions

create view tt21v as
select * from tt5 natural inner join tt6;
select pg_get_viewdef('tt21v', true);

create view tt22v as
select * from tt5 natural left join tt6;
select pg_get_viewdef('tt22v', true);

-- check handling of views with immediately-renamed columns

create view tt23v (col_a, col_b) as
select q1 as other_name1, q2 as other_name2 from int8_tbl;

select pg_get_viewdef('tt23v', true);
select pg_get_ruledef(oid, true) from pg_rewrite
  where ev_class = 'tt23v'::regclass and ev_type = '1';

-- test extraction of FieldSelect field names (get_name_for_var_field)

explain (verbose, costs off)
select (r).column2 from (select r from (values(1,2),(3,4)) r limit 1) ss;

-- test pretty-print parenthesization rules, and SubLink deparsing

create view tt26v as
select x + y + z as c1,
       (x * y) + z as c2,
       x + (y * z) as c3,
       (x + y) * z as c4,
       x * (y + z) as c5,
       x + (y + z) as c6,
       x + (y # z) as c7,
       (x > y) AND (y > z OR x > z) as c8,
       (x > y) OR (y > z AND NOT (x > z)) as c9
-- minipg: 行构造器 (x,y) 已裁剪，c10/c11 两列移除
from (values(1,2,3)) v(x,y,z);
select pg_get_viewdef('tt26v', true);


-- Test that changing the relkind of a relcache entry doesn't cause
-- trouble. Prior instances of where it did:
-- CALDaNm2yXz+zOtv7y5zBd5WKT8O0Ld3YxikuU3dcyCvxF7gypA@mail.gmail.com
-- CALDaNm3oZA-8Wbps2Jd1g5_Gjrr-x3YWrJPek-mF5Asrrvz2Dg@mail.gmail.com
CREATE TABLE tt26(c int);

BEGIN;
CREATE TABLE tt27(c int);
SAVEPOINT q;
CREATE RULE "_RETURN" AS ON SELECT TO tt27 DO INSTEAD SELECT * FROM tt26;
SELECT * FROM tt27;
ROLLBACK TO q;
CREATE RULE "_RETURN" AS ON SELECT TO tt27 DO INSTEAD SELECT * FROM tt26;
ROLLBACK;

BEGIN;
CREATE TABLE tt28(c int);
CREATE RULE "_RETURN" AS ON SELECT TO tt28 DO INSTEAD SELECT * FROM tt26;
CREATE RULE "_RETURN" AS ON SELECT TO tt28 DO INSTEAD SELECT * FROM tt26;
ROLLBACK;

-- test restriction on non-system view expansion.
create table tt27v_tbl (a int);
create view tt27v as select a from tt27v_tbl;
set restrict_nonsystem_relation_kind to 'view';
select a from tt27v where a > 0; -- Error
insert into tt27v values (1); -- Error
select viewname from pg_views where viewname = 'tt27v'; -- Ok to access a system view.
reset restrict_nonsystem_relation_kind;

-- clean up all the random objects we made above
DROP SCHEMA testviewschm2 CASCADE;
