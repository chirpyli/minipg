--
-- expression evaluation tests that don't fit into a more specific file
--

--
-- Tests for SQLVAlueFunction
--


-- current_date  (always matches because of transactional behaviour)
SELECT date(now())::text = current_date::text;


-- localtime
SELECT now()::time::text = localtime::text;
SELECT now()::time(3)::text = localtime(3)::text;

-- current_timestamp / localtimestamp (always matches because of transactional behaviour)
SELECT current_timestamp = NOW();
-- precision
SELECT length(current_timestamp::text) >= length(current_timestamp(0)::text);
-- localtimestamp
SELECT now()::timestamp::text = localtimestamp::text;

-- current_role/user/user is tested in rolenames.sql

-- current database / catalog
SELECT current_catalog = current_database();

-- current_schema
SELECT current_schema;
SET search_path = 'notme';
SELECT current_schema;
SET search_path = 'pg_catalog';
SELECT current_schema;
RESET search_path;


--
-- Tests for BETWEEN
--

explain (costs off)
select count(*) from date_tbl
  where f1 between '1997-01-01' and '1998-01-01';
select count(*) from date_tbl
  where f1 between '1997-01-01' and '1998-01-01';

explain (costs off)
select count(*) from date_tbl
  where f1 not between '1997-01-01' and '1998-01-01';
select count(*) from date_tbl
  where f1 not between '1997-01-01' and '1998-01-01';

explain (costs off)
select count(*) from date_tbl
  where f1 between symmetric '1997-01-01' and '1998-01-01';
select count(*) from date_tbl
  where f1 between symmetric '1997-01-01' and '1998-01-01';

explain (costs off)
select count(*) from date_tbl
  where f1 not between symmetric '1997-01-01' and '1998-01-01';
select count(*) from date_tbl
  where f1 not between symmetric '1997-01-01' and '1998-01-01';


--
-- Test parsing of a no-op cast to a type with unspecified typmod
--
begin;

-- minipg: numeric 类型已裁剪，numeric_tbl / numeric_view 用例移除

-- bpchar, lacking planner support for its length coercion function,
-- could behave differently

create table bpchar_tbl (f1 character(16) unique, f2 bpchar);

create view bpchar_view as
  select
    f1, f1::character(14) as f114, f1::bpchar as f1n,
    f2, f2::character(14) as f214, f2::bpchar as f2n
  from bpchar_tbl;

\d+ bpchar_view

explain (verbose, costs off) select * from bpchar_view
  where f1::bpchar = 'foo';

rollback;


--
-- Ordinarily, IN/NOT IN can be converted to a ScalarArrayOpExpr
-- with a suitably-chosen array type.
--
explain (verbose, costs off)
select random() IN (1, 4, 8.0);
explain (verbose, costs off)
select random()::int IN (1, 4, 8.0);
-- However, if there's not a common supertype for the IN elements,
-- we should instead try to produce "x = v1 OR x = v2 OR ...".
-- In most cases that'll fail for lack of all the requisite = operators,
-- but it can succeed sometimes.  So this should complain about lack of
-- an = operator, not about cast failure.
-- minipg: point / box 类型已裁剪，该用例移除

-- minipg: ScalarArrayOpExpr with a hashfn 一节依赖用户定义的 stable 函数
-- 阻止常量折叠，而 CREATE FUNCTION 已裁剪，整节移除。
