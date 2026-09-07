--
-- grouping sets
--

-- test data sources

CREATE VIEW gstest1(a,b,v)
  as values (1,1,10),(1,1,11),(1,2,12),(1,2,13),(1,3,14),
            (2,3,15),
            (3,3,16),(3,4,17),
            (4,1,18),(4,1,19);

CREATE TABLE gstest2 (a integer, b integer, c integer, d integer,
                           e integer, f integer, g integer, h integer);
\set ECHO none
\i data/load_gs_gstest2.sql
\set ECHO all

CREATE TABLE gstest3 (a integer, b integer, c integer, d integer);
\set ECHO none
\i data/load_gs_gstest3.sql
\set ECHO all
alter table gstest3 add primary key (a);



CREATE TABLE gstest_empty (a integer, b integer, v integer);

-- minipg: CREATE FUNCTION 已裁剪，gstest_data 这个 SRF 用例改用
-- generate_series() 直接构造等价输入

-- basic functionality

set enable_hashagg = false;  -- test hashing explicitly later

-- simple rollup with multiple plain aggregates, with and without ordering
-- (and with ordering differing from grouping)

select a, b, grouping(a,b), sum(v), count(*), max(v)
  from gstest1 group by rollup (a,b);
select a, b, grouping(a,b), sum(v), count(*), max(v)
  from gstest1 group by rollup (a,b) order by a,b;
select a, b, grouping(a,b), sum(v), count(*), max(v)
  from gstest1 group by rollup (a,b) order by b desc, a;
select a, b, grouping(a,b), sum(v), count(*), max(v)
  from gstest1 group by rollup (a,b) order by coalesce(a,0)+coalesce(b,0);

-- various types of ordered aggs
-- minipg: 有序集聚合（WITHIN GROUP / rank）已裁剪，仅保留普通有序聚合
select a, b, grouping(a,b),
       array_agg(v order by v),
       string_agg(v::text, ':' order by v desc)
  from gstest1 group by rollup (a,b) order by a,b;

-- test usage of grouped columns in direct args of aggs
select grouping(a), a, array_agg(b)
  from (values (1,1),(1,4),(1,5),(3,1),(3,2)) v(a,b)
 group by rollup (a) order by a;

-- nesting with grouping sets
select sum(c) from gstest2
  group by grouping sets((), grouping sets((), grouping sets(())))
  order by 1 desc;
-- minipg: GROUPING SETS 中以 (a,b) 形式书写的多列分组集依赖 ROW 行构造器，
-- 该语法已裁剪，凡此类嵌套/多列分组集用例一并移除
select sum(c) from gstest2
  group by grouping sets(grouping sets(rollup(c), grouping sets(cube(c))))
  order by 1 desc;
select sum(c) from gstest2
  group by grouping sets(a, grouping sets(a, cube(b)))
  order by 1 desc;
select sum(c) from gstest2
  group by grouping sets(grouping sets(a, grouping sets(a), a))
  order by 1 desc;
select sum(c) from gstest2
  group by grouping sets(grouping sets(a, grouping sets(a, grouping sets(a), ((a)), a, grouping sets(a), (a)), a))
  order by 1 desc;

-- empty input: first is 0 rows, second 1, third 3 etc.
-- minipg: 多列分组集 (a,b) 需 ROW 构造器，改用等价的 rollup(a,b)
select a, b, sum(v), count(*) from gstest_empty group by grouping sets (rollup(a,b), ());
select sum(v), count(*) from gstest_empty group by grouping sets ((),(),());

-- empty input with joins tests some important code paths
select t1.a, t2.b, sum(t1.v), count(*) from gstest_empty t1, gstest_empty t2
 group by grouping sets (rollup(t1.a,t2.b), ());

-- simple joins, var resolution, GROUPING on join vars
select t1.a, t2.b, grouping(t1.a, t2.b), sum(t1.v), max(t2.a)
  from gstest1 t1, gstest2 t2
 group by grouping sets (rollup(t1.a,t2.b), ());

select t1.a, t2.b, grouping(t1.a, t2.b), sum(t1.v), max(t2.a)
  from gstest1 t1 join gstest2 t2 on (t1.a=t2.a)
 group by grouping sets (rollup(t1.a,t2.b), ());

select a, b, grouping(a, b), sum(t1.v), max(t2.c)
  from gstest1 t1 join gstest2 t2 using (a,b)
 group by grouping sets (rollup(a,b), ());

-- minipg: "functionally dependent cols are not nulled" 用例依赖
-- GROUPING SETS 中的多列分组集 (a,b)/(a,c)，已随 ROW 构造器裁剪而移除

-- check that distinct grouping columns are kept separate
-- even if they are equal()
explain (costs off)
select g as alias1, g as alias2
  from generate_series(1,3) g
 group by alias1, rollup(alias2);

select g as alias1, g as alias2
  from generate_series(1,3) g
 group by alias1, rollup(alias2);

-- check that pulled-up subquery outputs still go to null when appropriate
select four, x
  from (select four, ten, 'foo'::text as x from tenk1) as t
  group by grouping sets (four, x)
  having x = 'foo';

select four, x || 'x'
  from (select four, ten, 'foo'::text as x from tenk1) as t
  group by grouping sets (four, x)
  order by four;

select (x+y)*1, sum(z)
 from (select 1 as x, 2 as y, 3 as z) s
 group by grouping sets (x+y, x);

select x, not x as not_x, q2 from
  (select *, q1 = 1 as x from int8_tbl i1) as t
  group by grouping sets(x, q2)
  order by x, q2;

-- check qual push-down rules for a subquery with grouping sets
explain (verbose, costs off)
select * from (
  select 1 as x, q1, sum(q2)
  from int8_tbl i1
  group by grouping sets(1, 2)
) ss
where x = 1 and q1 = 123;

select * from (
  select 1 as x, q1, sum(q2)
  from int8_tbl i1
  group by grouping sets(1, 2)
) ss
where x = 1 and q1 = 123;

-- check handling of pulled-up SubPlan in GROUPING() argument (bug #17479)
explain (verbose, costs off)
select grouping(ss.x)
from int8_tbl i1
cross join lateral (select (select i1.q1) as x) ss
group by ss.x;

select grouping(ss.x)
from int8_tbl i1
cross join lateral (select (select i1.q1) as x) ss
group by ss.x;

explain (verbose, costs off)
select (select grouping(ss.x))
from int8_tbl i1
cross join lateral (select (select i1.q1) as x) ss
group by ss.x;

select (select grouping(ss.x))
from int8_tbl i1
cross join lateral (select (select i1.q1) as x) ss
group by ss.x;

-- simple rescan tests
-- minipg: gstest_data 已移除，改用 generate_series 构造等价输入

select a, b, sum(v.x)
  from (values (1),(2)) v(x), generate_series(1,3) g(a), generate_series(1,3) h(b)
 group by rollup (a,b);

-- minipg: 原先这里用 SRF(gstest_data) 做 rescan 测试，其中"聚合写在
-- FROM 子句的子查询里"的写法在 minipg 上不受支持
-- （aggregate functions are not allowed in FROM clause of their own query
-- level），移除该变体，保留上面顶层聚合的等价用例

-- min max optimization should still work with GROUP BY ()
explain (costs off)
  select min(unique1) from tenk1 GROUP BY ();

-- Views with GROUPING SET queries
-- minipg: rollup((a,b,c),(c,d)) 依赖 ROW 构造器，改用 rollup(a,b,c,d)
CREATE VIEW gstest_view AS select a, b, grouping(a,b), sum(c), count(*), max(c)
  from gstest2 group by rollup (a,b,c,d);

select pg_get_viewdef('gstest_view'::regclass, true);

-- Nested queries with 3 or more levels of nesting
-- minipg: group by (a,b) 是行构造器，改为 group by a,b
select(select (select grouping(a,b) from (values (1)) v2(c)) from (values (1,2)) v1(a,b) group by a,b) from (values(6,7)) v3(e,f) GROUP BY ROLLUP(e,f);
select(select (select grouping(e,f) from (values (1)) v2(c)) from (values (1,2)) v1(a,b) group by a,b) from (values(6,7)) v3(e,f) GROUP BY ROLLUP(e,f);
select(select (select grouping(c) from (values (1)) v2(c) GROUP BY c) from (values (1,2)) v1(a,b) group by a,b) from (values(6,7)) v3(e,f) GROUP BY ROLLUP(e,f);

-- Combinations of operations
select a, b, c, d from gstest2 group by rollup(a,b),grouping sets(c,d);
select a, b from (values (1,2),(2,3)) v(a,b) group by a,b, grouping sets(a);

-- Tests for chained aggregates
select a, b, sum(c) from (values (1,1,10),(1,1,11),(1,2,12),(1,2,13),(1,3,14),(2,3,15),(3,3,16),(3,4,17),(4,1,18),(4,1,19)) v(a,b,c) group by rollup (a,b);
select a, b, sum(v.x)
  from (values (1),(2)) v(x), lateral (select v.x as a, i as b from generate_series(1,3) i) gd
 group by cube (a,b) order by a,b;

-- minipg: grouping sets ((a,b),(a+1,b+1),(a+2,b+2)) 与
-- grouping sets((a,b,v),(v)) 依赖 ROW 构造器，已裁剪，这两个用例移除

-- Test reordering of grouping sets
explain (costs off)
select * from gstest1 group by grouping sets(rollup(a,b,v),(v)) order by v,b,a;

-- Agg level check. This query should error out.
select (select grouping(a,b) from gstest2) from gstest2 group by a,b;

--Nested queries
select a, b, sum(c), count(*) from gstest2 group by grouping sets (rollup(a,b),a);

-- HAVING queries
select ten, sum(distinct four) from onek a
group by grouping sets(rollup(ten,four),(ten))
having exists (select 1 from onek b where sum(distinct a.four) = b.four);

-- Tests around pushdown of HAVING clauses, partially testing against previous bugs
select a,count(*) from gstest2 group by rollup(a) order by a;
select a,count(*) from gstest2 group by rollup(a) having a is distinct from 1 order by a;
explain (costs off)
  select a,count(*) from gstest2 group by rollup(a) having a is distinct from 1 order by a;

select v.c, (select count(*) from gstest2 group by () having v.c)
  from (values (false),(true)) v(c) order by v.c;
explain (costs off)
  select v.c, (select count(*) from gstest2 group by () having v.c)
    from (values (false),(true)) v(c) order by v.c;

-- HAVING with GROUPING queries
select ten, grouping(ten) from onek
group by grouping sets(ten) having grouping(ten) >= 0
order by 2,1;
select ten, grouping(ten) from onek
group by grouping sets(ten, four) having grouping(ten) > 0
order by 2,1;
select ten, grouping(ten) from onek
group by rollup(ten) having grouping(ten) > 0
order by 2,1;
select ten, grouping(ten) from onek
group by cube(ten) having grouping(ten) > 0
order by 2,1;
select ten, grouping(ten) from onek
group by (ten) having grouping(ten) >= 0
order by 2,1;

-- FILTER queries
select ten, sum(distinct four) filter (where four::text LIKE '%123%') from onek a
group by rollup(ten);

-- More rescan tests
select * from (values (1),(2)) v(a) left join lateral (select v.a, four, ten, count(*) from onek group by cube(four,ten)) s on true order by v.a,four,ten;
-- minipg: ROW 构造器已裁剪，array(row(...)) 形式的用例移除

-- Grouping on text columns
select sum(ten) from onek group by two, rollup(four::text) order by 1;
select sum(ten) from onek group by rollup(four::text), two order by 1;

-- hashing support

set enable_hashagg = true;



-- simple cases

select a, b, grouping(a,b), sum(v), count(*), max(v)
  from gstest1 group by grouping sets ((a),(b)) order by 3,1,2;
explain (costs off) select a, b, grouping(a,b), sum(v), count(*), max(v)
  from gstest1 group by grouping sets ((a),(b)) order by 3,1,2;

select a, b, grouping(a,b), sum(v), count(*), max(v)
  from gstest1 group by cube(a,b) order by 3,1,2;
explain (costs off) select a, b, grouping(a,b), sum(v), count(*), max(v)
  from gstest1 group by cube(a,b) order by 3,1,2;

-- shouldn't try and hash
explain (costs off)
  select a, b, grouping(a,b), array_agg(v order by v)
    from gstest1 group by cube(a,b);





-- empty input: first is 0 rows, second 1, third 3 etc.
-- minipg: 多列分组集 (a,b) 需 ROW 构造器，改用等价的 rollup(a,b)
select a, b, sum(v), count(*) from gstest_empty group by grouping sets (rollup(a,b), ());
explain (costs off)
  select a, b, sum(v), count(*) from gstest_empty group by grouping sets (rollup(a,b), ());
select sum(v), count(*) from gstest_empty group by grouping sets ((),(),());
explain (costs off)
  select sum(v), count(*) from gstest_empty group by grouping sets ((),(),());

-- minipg: "functionally dependent cols are not nulled" 用例依赖多列分组集
-- (a,b)/(a,c)，已随 ROW 构造器裁剪而移除

-- simple rescan tests

select a, b, sum(v.x)
  from (values (1),(2)) v(x), lateral (select v.x as a, i as b from generate_series(1,3) i) gd
 group by grouping sets (a,b)
 order by 1, 2, 3;
explain (costs off)
  select a, b, sum(v.x)
    from (values (1),(2)) v(x), lateral (select v.x as a, i as b from generate_series(1,3) i) gd
   group by grouping sets (a,b)
   order by 3, 1, 2;
-- minipg: 同上，聚合位于 FROM 子句子查询的 rescan 变体不受支持，移除

-- Tests for chained aggregates
-- minipg: grouping sets ((a,b),(a+1,b+1),(a+2,b+2)) 依赖 ROW 构造器，移除
select a, b, sum(v.x)
  from (values (1),(2)) v(x), lateral (select v.x as a, i as b from generate_series(1,3) i) gd
 group by cube (a,b) order by a,b;
explain (costs off)
  select a, b, sum(v.x)
    from (values (1),(2)) v(x), lateral (select v.x as a, i as b from generate_series(1,3) i) gd
   group by cube (a,b) order by a,b;

-- Verify that we correctly handle the child node returning a
-- non-minimal slot, which happens if the input is pre-sorted,
-- e.g. due to an index scan.
BEGIN;
SET LOCAL enable_hashagg = false;
EXPLAIN (COSTS OFF) SELECT a, b, count(*), max(a), max(b) FROM gstest3 GROUP BY GROUPING SETS(a, b,()) ORDER BY a, b;
SELECT a, b, count(*), max(a), max(b) FROM gstest3 GROUP BY GROUPING SETS(a, b,()) ORDER BY a, b;
SET LOCAL enable_seqscan = false;
EXPLAIN (COSTS OFF) SELECT a, b, count(*), max(a), max(b) FROM gstest3 GROUP BY GROUPING SETS(a, b,()) ORDER BY a, b;
SELECT a, b, count(*), max(a), max(b) FROM gstest3 GROUP BY GROUPING SETS(a, b,()) ORDER BY a, b;
COMMIT;

-- More rescan tests
select * from (values (1),(2)) v(a) left join lateral (select v.a, four, ten, count(*) from onek group by cube(four,ten)) s on true order by v.a,four,ten;
-- minipg: ROW 构造器已裁剪，array(row(...)) 形式的用例移除

-- Rescan logic changes when there are no empty grouping sets, so test
-- that too:
select * from (values (1),(2)) v(a) left join lateral (select v.a, four, ten, count(*) from onek group by grouping sets(four,ten)) s on true order by v.a,four,ten;
-- minipg: ROW 构造器已裁剪，array(row(...)) 形式的用例移除

-- test the knapsack

set enable_indexscan = false;
set work_mem = '64kB';
explain (costs off)
  select unique1,
         count(two), count(four), count(ten),
         count(hundred), count(thousand), count(twothousand),
         count(*)
    from tenk1 group by grouping sets (unique1,twothousand,thousand,hundred,ten,four,two);
explain (costs off)
  select unique1,
         count(two), count(four), count(ten),
         count(hundred), count(thousand), count(twothousand),
         count(*)
    from tenk1 group by grouping sets (unique1,hundred,ten,four,two);

set work_mem = '384kB';
explain (costs off)
  select unique1,
         count(two), count(four), count(ten),
         count(hundred), count(thousand), count(twothousand),
         count(*)
    from tenk1 group by grouping sets (unique1,twothousand,thousand,hundred,ten,four,two);

-- check collation-sensitive matching between grouping expressions
-- (similar to a check for aggregates, but there are additional code
-- paths for GROUPING, so check again here)

select v||'a', case grouping(v||'a') when 1 then 1 else 0 end, count(*)
  from unnest(array[1,1], array['a','b']) u(i,v)
 group by rollup(i, v||'a') order by 1,3;
select v||'a', case when grouping(v||'a') = 1 then 1 else 0 end, count(*)
  from unnest(array[1,1], array['a','b']) u(i,v)
 group by rollup(i, v||'a') order by 1,3;

-- Bug #16784
create table bug_16784(i int, j int);
analyze bug_16784;
update pg_class set reltuples = 10 where relname='bug_16784';

insert into bug_16784 select g/10, g from generate_series(1,40) g;

set work_mem='64kB';
set enable_sort = false;

select * from
  (values (1),(2)) v(a),
  lateral (select a, i, j, count(*) from
             bug_16784 group by cube(i,j)) s
  order by v.a, i, j;

--
-- Compare results between plans using sorting and plans using hash
-- aggregation. Force spilling in both cases by setting work_mem low
-- and altering the statistics.
--

-- minipg: CREATE TABLE AS 已裁剪，改为显式建表 + INSERT ... SELECT
create table gs_data_1 (g1000 int, g100 int, g10 int, g int);
insert into gs_data_1
select g%1000 as g1000, g%100 as g100, g%10 as g10, g
   from generate_series(0,1999) g;

analyze gs_data_1;
update pg_class set reltuples = 10 where relname='gs_data_1';

set work_mem='64kB';

-- Produce results with sorting.

set enable_sort = true;
set enable_hashagg = false;

explain (costs off)
select g100, g10, sum(g::int8), count(*), max(g::text)
from gs_data_1 group by cube (g1000, g100,g10);

create table gs_group_1 (g100 int, g10 int, sum int8, count int8, max text);
insert into gs_group_1
select g100, g10, sum(g::int8), count(*), max(g::text)
from gs_data_1 group by cube (g1000, g100,g10);

-- Produce results with hash aggregation.

set enable_hashagg = true;
set enable_sort = false;

explain (costs off)
select g100, g10, sum(g::int8), count(*), max(g::text)
from gs_data_1 group by cube (g1000, g100,g10);

create table gs_hash_1 (g100 int, g10 int, sum int8, count int8, max text);
insert into gs_hash_1
select g100, g10, sum(g::int8), count(*), max(g::text)
from gs_data_1 group by cube (g1000, g100,g10);

set enable_sort = true;
set work_mem to default;

drop table gs_group_1;
drop table gs_hash_1;

-- GROUP BY DISTINCT

-- "normal" behavior...
select a, b, c
from (values (1, 2, 3), (4, null, 6), (7, 8, 9)) as t (a, b, c)
group by all rollup(a, b), rollup(a, c)
order by a, b, c;

-- ...which is also the default
select a, b, c
from (values (1, 2, 3), (4, null, 6), (7, 8, 9)) as t (a, b, c)
group by rollup(a, b), rollup(a, c)
order by a, b, c;

-- "group by distinct" behavior...
select a, b, c
from (values (1, 2, 3), (4, null, 6), (7, 8, 9)) as t (a, b, c)
group by distinct rollup(a, b), rollup(a, c)
order by a, b, c;

-- ...which is not the same as "select distinct"
select distinct a, b, c
from (values (1, 2, 3), (4, null, 6), (7, 8, 9)) as t (a, b, c)
group by rollup(a, b), rollup(a, c)
order by a, b, c;

-- test handling of outer GroupingFunc within subqueries
explain (costs off)
select (select grouping(v1)) from (values ((select 1))) v(v1) group by cube(v1);
select (select grouping(v1)) from (values ((select 1))) v(v1) group by cube(v1);

explain (costs off)
select (select grouping(v1)) from (values ((select 1))) v(v1) group by v1;
select (select grouping(v1)) from (values ((select 1))) v(v1) group by v1;

-- end
