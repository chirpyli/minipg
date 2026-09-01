--
-- Tests for polymorphic SQL functions and aggregates based on them.
-- Tests for other features related to function-calling have snuck in, too.
--

create function polyf(x anyelement) returns anyelement as $$
  select x + 1
$$ language sql;

select polyf(42) as int, polyf(4.5) as num;
select polyf(point(3,4));  -- fail for lack of + operator

drop function polyf(x anyelement);

create function polyf(x anyelement) returns anyarray as $$
  select array[x + 1, x + 2]
$$ language sql;

select polyf(42) as int, polyf(4.5) as num;

drop function polyf(x anyelement);

create function polyf(x anyarray) returns anyelement as $$
  select x[1]
$$ language sql;

select polyf(array[2,4]) as int, polyf(array[4.5, 7.7]) as num;

select polyf(stavalues1) from pg_statistic;  -- fail, can't infer element type

drop function polyf(x anyarray);

create function polyf(x anyarray) returns anyarray as $$
  select x
$$ language sql;

select polyf(array[2,4]) as int, polyf(array[4.5, 7.7]) as num;

select polyf(stavalues1) from pg_statistic;  -- fail, can't infer element type

drop function polyf(x anyarray);

create function polyf(x anycompatible, y anycompatible) returns anycompatiblearray as $$
  select array[x, y]
$$ language sql;

select polyf(2, 4) as int, polyf(2, 4.5) as num;

drop function polyf(x anycompatible, y anycompatible);

create function polyf(a anyelement, b anyarray,
                      c anycompatible, d anycompatible,
                      OUT x anyarray, OUT y anycompatiblearray)
as $$
  select a || b, array[c, d]
$$ language sql;

select x, pg_typeof(x), y, pg_typeof(y)
  from polyf(11, array[1, 2], 42, 34.5);
select x, pg_typeof(x), y, pg_typeof(y)
  from polyf(11, array[1, 2], point(1,2), point(3,4));
select x, pg_typeof(x), y, pg_typeof(y)
  from polyf(11, '{1,2}', point(1,2), '(3,4)');
select x, pg_typeof(x), y, pg_typeof(y)
  from polyf(11, array[1, 2.2], 42, 34.5);  -- fail

drop function polyf(a anyelement, b anyarray,
                    c anycompatible, d anycompatible);


-- minipg: PL/pgSQL removed. The original bleat() raised a NOTICE to observe
-- which branch of sql_if() was evaluated; as a SQL function it just returns $1.
-- The inlining correctness is still checked via the returned values below.
create function bleat(int) returns int as $$
  SELECT $1;
$$ language sql;

create function sql_if(bool, anyelement, anyelement) returns anyelement as $$
select case when $1 then $2 else $3 end $$ language sql;

-- Note this would fail with integer overflow, never mind wrong bleat() output,
-- if the CASE expression were not successfully inlined
select f1, sql_if(f1 > 0, bleat(f1), bleat(f1 + 1)) from int4_tbl;

select q2, sql_if(q2 > 0, q2, q2 + 1) from int8_tbl;

-- check that we can apply functions taking ANYARRAY to pg_stats
select distinct array_ndims(histogram_bounds) from pg_stats
where histogram_bounds is not null;

-- such functions must protect themselves if varying element type isn't OK
-- (WHERE clause here is to avoid possibly getting a collation error instead)
select max(histogram_bounds) from pg_stats where tablename = 'pg_am';

-- another corner case is the input functions for polymorphic pseudotypes
select array_in('{1,2,3}','int4'::regtype,-1);  -- this has historically worked
select * from array_in('{1,2,3}','int4'::regtype,-1);  -- this not

-- test variadic polymorphic functions

create function myleast(variadic anyarray) returns anyelement as $$
  select min($1[i]) from generate_subscripts($1,1) g(i)
$$ language sql immutable strict;

select myleast(10, 1, 20, 33);
select myleast(1.1, 0.22, 0.55);
select myleast('z'::text);
select myleast(); -- fail

-- test with variadic call parameter
select myleast(variadic array[1,2,3,4,-1]);
select myleast(variadic array[1.1, -5.5]);

--test with empty variadic call parameter
select myleast(variadic array[]::int[]);

-- an example with some ordinary arguments too
create function concat(text, variadic anyarray) returns text as $$
  select array_to_string($2, $1);
$$ language sql immutable strict;

select concat('%', 1, 2, 3, 4, 5);
select concat('|', 'a'::text, 'b', 'c');
select concat('|', variadic array[1,2,33]);
select concat('|', variadic array[]::int[]);

drop function concat(text, anyarray);

-- mix variadic with anyelement
create function formarray(anyelement, variadic anyarray) returns anyarray as $$
  select array_prepend($1, $2);
$$ language sql immutable strict;

select formarray(1,2,3,4,5);
select formarray(1.1, variadic array[1.2,55.5]);
select formarray(1.1, array[1.2,55.5]); -- fail without variadic
select formarray(1, 'x'::text); -- fail, type mismatch
select formarray(1, variadic array['x'::text]); -- fail, type mismatch

drop function formarray(anyelement, variadic anyarray);

-- test pg_typeof() function
select pg_typeof(null);           -- unknown
select pg_typeof(0);              -- integer
select pg_typeof(0.0);            -- numeric
select pg_typeof(1+1 = 2);        -- boolean
select pg_typeof('x');            -- unknown
select pg_typeof('' || '');       -- text
select pg_typeof(pg_typeof(0));   -- regtype
select pg_typeof(array[1.2,55.5]); -- numeric[]
select pg_typeof(myleast(10, 1, 20, 33));  -- polymorphic input

-- test functions with default parameters

-- test basic functionality
create function dfunc(a int = 1, int = 2) returns int as $$
  select $1 + $2;
$$ language sql;

select dfunc();
select dfunc(10);
select dfunc(10, 20);
select dfunc(10, 20, 30);  -- fail

drop function dfunc();  -- fail
drop function dfunc(int);  -- fail
drop function dfunc(int, int);  -- ok

-- fail: defaults must be at end of argument list
create function dfunc(a int = 1, b int) returns int as $$
  select $1 + $2;
$$ language sql;

-- however, this should work:
create function dfunc(a int = 1, out sum int, b int = 2) as $$
  select $1 + $2;
$$ language sql;

select dfunc();

-- verify it lists properly
\df dfunc

drop function dfunc(int, int);

-- check implicit coercion
create function dfunc(a int DEFAULT 1.0, int DEFAULT '-1') returns int as $$
  select $1 + $2;
$$ language sql;
select dfunc();

create function dfunc(a text DEFAULT 'Hello', b text DEFAULT 'World') returns text as $$
  select $1 || ', ' || $2;
$$ language sql;

select dfunc();  -- fail: which dfunc should be called? int or text
select dfunc('Hi');  -- ok
select dfunc('Hi', 'City');  -- ok
select dfunc(0);  -- ok
select dfunc(10, 20);  -- ok

drop function dfunc(int, int);
drop function dfunc(text, text);

create function dfunc(int = 1, int = 2) returns int as $$
  select 2;
$$ language sql;

create function dfunc(int = 1, int = 2, int = 3, int = 4) returns int as $$
  select 4;
$$ language sql;

-- Now, dfunc(nargs = 2) and dfunc(nargs = 4) are ambiguous when called
-- with 0 to 2 arguments.

select dfunc();  -- fail
select dfunc(1);  -- fail
select dfunc(1, 2);  -- fail
select dfunc(1, 2, 3);  -- ok
select dfunc(1, 2, 3, 4);  -- ok

drop function dfunc(int, int);
drop function dfunc(int, int, int, int);

-- default values are not allowed for output parameters
create function dfunc(out int = 20) returns int as $$
  select 1;
$$ language sql;

-- polymorphic parameter test
create function dfunc(anyelement = 'World'::text) returns text as $$
  select 'Hello, ' || $1::text;
$$ language sql;

select dfunc();
select dfunc(0);
select dfunc(to_date('20081215','YYYYMMDD'));
select dfunc('City'::text);

drop function dfunc(anyelement);

-- check defaults for variadics

create function dfunc(a variadic int[]) returns int as
$$ select array_upper($1, 1) $$ language sql;

select dfunc();  -- fail
select dfunc(10);
select dfunc(10,20);

create or replace function dfunc(a variadic int[] default array[]::int[]) returns int as
$$ select array_upper($1, 1) $$ language sql;

select dfunc();  -- now ok
select dfunc(10);
select dfunc(10,20);

-- can't remove the default once it exists
create or replace function dfunc(a variadic int[]) returns int as
$$ select array_upper($1, 1) $$ language sql;

\df dfunc

drop function dfunc(a variadic int[]);

-- Ambiguity should be reported only if there's not a better match available

create function dfunc(int = 1, int = 2, int = 3) returns int as $$
  select 3;
$$ language sql;

create function dfunc(int = 1, int = 2) returns int as $$
  select 2;
$$ language sql;

create function dfunc(text) returns text as $$
  select $1;
$$ language sql;

-- dfunc(narg=2) and dfunc(narg=3) are ambiguous
select dfunc(1);  -- fail

-- but this works since the ambiguous functions aren't preferred anyway
select dfunc('Hi');

drop function dfunc(int, int, int);
drop function dfunc(int, int);
drop function dfunc(text);

--
-- Tests for named- and mixed-notation function calling
--

create function dfunc(a int, b int, c int = 0, d int = 0)
  returns table (a int, b int, c int, d int) as $$
  select $1, $2, $3, $4;
$$ language sql;

select (dfunc(10,20,30)).*;
select (dfunc(a := 10, b := 20, c := 30)).*;
select * from dfunc(a := 10, b := 20);
select * from dfunc(b := 10, a := 20);
select * from dfunc(0);  -- fail
select * from dfunc(1,2);
select * from dfunc(1,2,c := 3);
select * from dfunc(1,2,d := 3);

select * from dfunc(x := 20, b := 10, x := 30);  -- fail, duplicate name
select * from dfunc(10, b := 20, 30);  -- fail, named args must be last
select * from dfunc(x := 10, b := 20, c := 30);  -- fail, unknown param
select * from dfunc(10, 10, a := 20);  -- fail, a overlaps positional parameter
select * from dfunc(1,c := 2,d := 3); -- fail, no value for b

drop function dfunc(int, int, int, int);

-- test with different parameter types
create function dfunc(a varchar, b numeric, c date = current_date)
  returns table (a varchar, b numeric, c date) as $$
  select $1, $2, $3;
$$ language sql;

select (dfunc('Hello World', 20, '2009-07-25'::date)).*;
select * from dfunc('Hello World', 20, '2009-07-25'::date);
select * from dfunc(c := '2009-07-25'::date, a := 'Hello World', b := 20);
select * from dfunc('Hello World', b := 20, c := '2009-07-25'::date);
select * from dfunc('Hello World', c := '2009-07-25'::date, b := 20);
select * from dfunc('Hello World', c := 20, b := '2009-07-25'::date);  -- fail

drop function dfunc(varchar, numeric, date);

-- test out parameters with named params
create function dfunc(a varchar = 'def a', out _a varchar, c numeric = NULL, out _c numeric)
returns record as $$
  select $1, $2;
$$ language sql;

select (dfunc()).*;
select * from dfunc();
select * from dfunc('Hello', 100);
select * from dfunc(a := 'Hello', c := 100);
select * from dfunc(c := 100, a := 'Hello');
select * from dfunc('Hello');
select * from dfunc('Hello', c := 100);
select * from dfunc(c := 100);

-- fail, can no longer change an input parameter's name
create or replace function dfunc(a varchar = 'def a', out _a varchar, x numeric = NULL, out _c numeric)
returns record as $$
  select $1, $2;
$$ language sql;

create or replace function dfunc(a varchar = 'def a', out _a varchar, numeric = NULL, out _c numeric)
returns record as $$
  select $1, $2;
$$ language sql;

drop function dfunc(varchar, numeric);

--fail, named parameters are not unique
create function testpolym(a int, a int) returns int as $$ select 1;$$ language sql;
create function testpolym(int, out a int, out a int) returns int as $$ select 1;$$ language sql;
create function testpolym(out a int, inout a int) returns int as $$ select 1;$$ language sql;
create function testpolym(a int, inout a int) returns int as $$ select 1;$$ language sql;

-- valid
create function testpolym(a int, out a int) returns int as $$ select $1;$$ language sql;
select testpolym(37);
drop function testpolym(int);
create function testpolym(a int) returns table(a int) as $$ select $1;$$ language sql;
select * from testpolym(37);
drop function testpolym(int);

-- test polymorphic params and defaults
create function dfunc(a anyelement, b anyelement = null, flag bool = true)
returns anyelement as $$
  select case when $3 then $1 else $2 end;
$$ language sql;

select dfunc(1,2);
select dfunc('a'::text, 'b'); -- positional notation with default

select dfunc(a := 1, b := 2);
select dfunc(a := 'a'::text, b := 'b');
select dfunc(a := 'a'::text, b := 'b', flag := false); -- named notation

select dfunc(b := 'b'::text, a := 'a'); -- named notation with default
select dfunc(a := 'a'::text, flag := true); -- named notation with default
select dfunc(a := 'a'::text, flag := false); -- named notation with default
select dfunc(b := 'b'::text, a := 'a', flag := true); -- named notation

select dfunc('a'::text, 'b', false); -- full positional notation
select dfunc('a'::text, 'b', flag := false); -- mixed notation
select dfunc('a'::text, 'b', true); -- full positional notation
select dfunc('a'::text, 'b', flag := true); -- mixed notation

-- ansi/sql syntax
select dfunc(a => 1, b => 2);
select dfunc(a => 'a'::text, b => 'b');
select dfunc(a => 'a'::text, b => 'b', flag => false); -- named notation

select dfunc(b => 'b'::text, a => 'a'); -- named notation with default
select dfunc(a => 'a'::text, flag => true); -- named notation with default
select dfunc(a => 'a'::text, flag => false); -- named notation with default
select dfunc(b => 'b'::text, a => 'a', flag => true); -- named notation

select dfunc('a'::text, 'b', false); -- full positional notation
select dfunc('a'::text, 'b', flag => false); -- mixed notation
select dfunc('a'::text, 'b', true); -- full positional notation
select dfunc('a'::text, 'b', flag => true); -- mixed notation

-- this tests lexer edge cases around =>
select dfunc(a =>-1);
select dfunc(a =>+1);
select dfunc(a =>/**/1);
select dfunc(a =>--comment to be removed by psql
  1);
-- minipg: PL/pgSQL removed. The original DO block verified the lexer parses
-- "dfunc(a=>-- comment\n 1)"; replicate with a plain SQL SELECT.
SELECT dfunc(a=> 1) AS r;

-- check reverse-listing of named-arg calls
CREATE VIEW dfview AS
   SELECT q1, q2,
     dfunc(q1,q2, flag := q1>q2) as c3,
     dfunc(q1, flag := q1<q2, b := q2) as c4
     FROM int8_tbl;

select * from dfview;

\d+ dfview

drop view dfview;
drop function dfunc(anyelement, anyelement, bool);

--
-- Tests for ANYCOMPATIBLE polymorphism family
--

create function anyctest(anycompatible, anycompatible)
returns anycompatible as $$
  select greatest($1, $2)
$$ language sql;

select x, pg_typeof(x) from anyctest(11, 12) x;
select x, pg_typeof(x) from anyctest(11, 12.3) x;
select x, pg_typeof(x) from anyctest(11, point(1,2)) x;  -- fail
select x, pg_typeof(x) from anyctest('11', '12.3') x;  -- defaults to text

drop function anyctest(anycompatible, anycompatible);

create function anyctest(anycompatible, anycompatible)
returns anycompatiblearray as $$
  select array[$1, $2]
$$ language sql;

select x, pg_typeof(x) from anyctest(11, 12) x;
select x, pg_typeof(x) from anyctest(11, 12.3) x;
select x, pg_typeof(x) from anyctest(11, array[1,2]) x;  -- fail

drop function anyctest(anycompatible, anycompatible);

create function anyctest(anycompatible, anycompatiblearray)
returns anycompatiblearray as $$
  select array[$1] || $2
$$ language sql;

select x, pg_typeof(x) from anyctest(11, array[12]) x;
select x, pg_typeof(x) from anyctest(11, array[12.3]) x;
select x, pg_typeof(x) from anyctest(12.3, array[13]) x;
select x, pg_typeof(x) from anyctest(12.3, '{13,14.4}') x;
select x, pg_typeof(x) from anyctest(11, array[point(1,2)]) x;  -- fail
select x, pg_typeof(x) from anyctest(11, 12) x;  -- fail

drop function anyctest(anycompatible, anycompatiblearray);

create function anyctest(anycompatiblenonarray, anycompatiblenonarray)
returns anycompatiblearray as $$
  select array[$1, $2]
$$ language sql;

select x, pg_typeof(x) from anyctest(11, 12) x;
select x, pg_typeof(x) from anyctest(11, 12.3) x;
select x, pg_typeof(x) from anyctest(array[11], array[1,2]) x;  -- fail

drop function anyctest(anycompatiblenonarray, anycompatiblenonarray);

create function anyctest(a anyelement, b anyarray,
                         c anycompatible, d anycompatible)
returns anycompatiblearray as $$
  select array[c, d]
$$ language sql;

select x, pg_typeof(x) from anyctest(11, array[1, 2], 42, 34.5) x;
select x, pg_typeof(x) from anyctest(11, array[1, 2], point(1,2), point(3,4)) x;
select x, pg_typeof(x) from anyctest(11, '{1,2}', point(1,2), '(3,4)') x;
select x, pg_typeof(x) from anyctest(11, array[1, 2.2], 42, 34.5) x;  -- fail

drop function anyctest(a anyelement, b anyarray,
                       c anycompatible, d anycompatible);

create function anyctest(variadic anycompatiblearray)
returns anycompatiblearray as $$
  select $1
$$ language sql;

select x, pg_typeof(x) from anyctest(11, 12) x;
select x, pg_typeof(x) from anyctest(11, 12.2) x;
select x, pg_typeof(x) from anyctest(11, '12') x;
select x, pg_typeof(x) from anyctest(11, '12.2') x;  -- fail
select x, pg_typeof(x) from anyctest(variadic array[11, 12]) x;
select x, pg_typeof(x) from anyctest(variadic array[11, 12.2]) x;

drop function anyctest(variadic anycompatiblearray);
