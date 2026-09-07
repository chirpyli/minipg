--
-- Tests for psql features that aren't closely connected to any
-- specific server features
--

-- \set

-- fail: invalid name
\set invalid/name foo
-- fail: invalid value for special variable
\set AUTOCOMMIT foo
-- check handling of built-in boolean variable
\echo :ON_ERROR_ROLLBACK
\set ON_ERROR_ROLLBACK
\echo :ON_ERROR_ROLLBACK
\set ON_ERROR_ROLLBACK foo
\echo :ON_ERROR_ROLLBACK
\set ON_ERROR_ROLLBACK on
\echo :ON_ERROR_ROLLBACK
\unset ON_ERROR_ROLLBACK
\echo :ON_ERROR_ROLLBACK

-- \g and \gx

SELECT 1 as one, 2 as two \g
\gx
SELECT 3 as three, 4 as four \gx
\g

-- \g/\gx with pset options

SELECT 1 as one, 2 as two \g (format=csv csv_fieldsep='\t')
\g
SELECT 1 as one, 2 as two \gx (title='foo bar')
\g

-- \gset

select 10 as test01, 20 as test02, 'Hello' as test03 \gset pref01_

\echo :pref01_test01 :pref01_test02 :pref01_test03

-- should fail: bad variable name
select 10 as "bad name"
\gset

select 97 as "EOF", 'ok' as _foo \gset IGNORE
\echo :IGNORE_foo :IGNOREEOF

-- multiple backslash commands in one line
select 1 as x, 2 as y \gset pref01_ \\ \echo :pref01_x
select 3 as x, 4 as y \gset pref01_ \echo :pref01_x \echo :pref01_y
select 5 as x, 6 as y \gset pref01_ \\ \g \echo :pref01_x :pref01_y
select 7 as x, 8 as y \g \gset pref01_ \echo :pref01_x :pref01_y

-- NULL should unset the variable
\set var2 xyz
select 1 as var1, NULL as var2, 3 as var3 \gset
\echo :var1 :var2 :var3

-- \gset requires just one tuple
select 10 as test01, 20 as test02 from generate_series(1,3) \gset
select 10 as test01, 20 as test02 from generate_series(1,0) \gset

-- show all pset options
\pset

-- test multi-line headers, wrapping, and newline indicators
-- in aligned, unaligned, and wrapped formats
create view q as select array_to_string(array_agg(repeat('x',2*n)),E'\n') as "ab

c", array_to_string(array_agg(repeat('y',20-2*n)),E'\n') as "a
bc" from generate_series(1,10) as n(n) group by n>1 order by n>1;

\pset linestyle ascii

\pset expanded off
\pset columns 40

\pset border 0
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 1
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 2
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset expanded on
\pset columns 20

\pset border 0
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 1
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 2
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset linestyle old-ascii

\pset expanded off
\pset columns 40

\pset border 0
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 1
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 2
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset expanded on
\pset columns 20

\pset border 0
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 1
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 2
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

drop view q;

-- test single-line header and data
create view q as select repeat('x',2*n) as "0123456789abcdef", repeat('y',20-2*n) as "0123456789" from generate_series(1,10) as n;

\pset linestyle ascii

\pset expanded off
\pset columns 40

\pset border 0
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 1
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 2
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset expanded on
\pset columns 30

\pset border 0
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 1
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 2
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset expanded on
\pset columns 20

\pset border 0
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 1
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 2
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset linestyle old-ascii

\pset expanded off
\pset columns 40

\pset border 0
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 1
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 2
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset expanded on

\pset border 0
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 1
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

\pset border 2
\pset format unaligned
select * from q;
\pset format aligned
select * from q;
\pset format wrapped
select * from q;

drop view q;

-- expanded output with short-width columns
\pset border 2
\pset expanded on
create table psql_short_tab(a int, b int);
insert into psql_short_tab values(10,20),(30,40);
\pset format aligned
select * from psql_short_tab;
\pset format wrapped
select * from psql_short_tab;
drop table psql_short_tab;

\pset linestyle ascii
\pset border 1

-- support table for output-format tests (useful to create a footer)

create table psql_serial_tab (id serial);

-- test header/footer/tuples_only behavior in aligned/unaligned/wrapped cases

\pset format aligned

\pset expanded off
\d psql_serial_tab_id_seq
\pset tuples_only true
\df exp
\pset tuples_only false
\pset expanded on
\d psql_serial_tab_id_seq
\pset tuples_only true
\df exp
\pset tuples_only false
-- empty table is a special case for this format
select 1 where false;

\pset format unaligned

\pset expanded off
\d psql_serial_tab_id_seq
\pset tuples_only true
\df exp
\pset tuples_only false
\pset expanded on
\d psql_serial_tab_id_seq
\pset tuples_only true
\df exp
\pset tuples_only false

\pset format wrapped

\pset expanded off
\d psql_serial_tab_id_seq
\pset tuples_only true
\df exp
\pset tuples_only false
\pset expanded on
\d psql_serial_tab_id_seq
\pset tuples_only true
\df exp
\pset tuples_only false

-- test numericlocale (as best we can without control of psql's locale)

\pset format aligned
\pset expanded off
\pset numericlocale true

select n, -n as m, n * 111 as x, '1e90'::float8 as f
from generate_series(0,3) n;

\pset numericlocale false


-- test csv output format

\pset format csv

\pset border 1
\pset expanded off
\d psql_serial_tab_id_seq
\pset tuples_only true
\df exp
\pset tuples_only false
\pset expanded on
\d psql_serial_tab_id_seq
\pset tuples_only true
\df exp
\pset tuples_only false

create view q as
  select 'some"text' as "a""title", E'  <foo>\n<bar>' as "junk",
         '   ' as "empty", n as int
  from generate_series(1,2) as n;

\pset expanded off
select * from q;

\pset expanded on
select * from q;

drop view q;

-- special cases
\pset expanded off
select 'comma,comma' as comma, 'semi;semi' as semi;
\pset csv_fieldsep ';'
select 'comma,comma' as comma, 'semi;semi' as semi;
select '\.' as data;
\pset csv_fieldsep '.'
select '\' as d1, '' as d2;

-- illegal csv separators
\pset csv_fieldsep ''
\pset csv_fieldsep '\0'
\pset csv_fieldsep '\n'
\pset csv_fieldsep '\r'
\pset csv_fieldsep '"'
\pset csv_fieldsep ',,'

\pset csv_fieldsep ','


-- clean up after output format tests

drop table psql_serial_tab;

\pset format aligned
\pset expanded off
\pset border 1

-- \echo and allied features

\echo this is a test
\echo -n without newline
\echo with -n newline
\echo '-n' with newline

\set foo bar
\echo foo = :foo

\qecho this is a test
\qecho foo = :foo

\warn this is a test
\warn foo = :foo

-- tests for \if ... \endif

\if true
  select 'okay';
  select 'still okay';
\else
  not okay;
  still not okay
\endif

-- at this point query buffer should still have last valid line
\g

-- \if should work okay on part of a query
select
  \if true
    42
  \else
    (bogus
  \endif
  forty_two;

select \if false \\ (bogus \else \\ 42 \endif \\ forty_two;

-- test a large nested if using a variety of true-equivalents
\if true
	\if 1
		\if yes
			\if on
				\echo 'all true'
			\else
				\echo 'should not print #1-1'
			\endif
		\else
			\echo 'should not print #1-2'
		\endif
	\else
		\echo 'should not print #1-3'
	\endif
\else
	\echo 'should not print #1-4'
\endif

-- test a variety of false-equivalents in an if/elif/else structure
\if false
	\echo 'should not print #2-1'
\elif 0
	\echo 'should not print #2-2'
\elif no
	\echo 'should not print #2-3'
\elif off
	\echo 'should not print #2-4'
\else
	\echo 'all false'
\endif

-- test true-false elif after initial true branch
\if true
	\echo 'should print #2-5'
\elif true
	\echo 'should not print #2-6'
\elif false
	\echo 'should not print #2-7'
\else
	\echo 'should not print #2-8'
\endif

-- test simple true-then-else
\if true
	\echo 'first thing true'
\else
	\echo 'should not print #3-1'
\endif

-- test simple false-true-else
\if false
	\echo 'should not print #4-1'
\elif true
	\echo 'second thing true'
\else
	\echo 'should not print #5-1'
\endif

-- invalid boolean expressions are false
\if invalid boolean expression
	\echo 'will not print #6-1'
\else
	\echo 'will print anyway #6-2'
\endif

-- test un-matched endif
\endif

-- test un-matched else
\else

-- test un-matched elif
\elif

-- test double-else error
\if true
\else
\else
\endif

-- test elif out-of-order
\if false
\else
\elif
\endif

-- test if-endif matching in a false branch
\if false
    \if false
        \echo 'should not print #7-1'
    \else
        \echo 'should not print #7-2'
    \endif
    \echo 'should not print #7-3'
\else
    \echo 'should print #7-4'
\endif

-- show that vars and backticks are not expanded when ignoring extra args
\set foo bar
\echo :foo :'foo' :"foo"
\pset fieldsep | `nosuchcommand` :foo :'foo' :"foo"

-- show that vars and backticks are not expanded and commands are ignored
-- when in a false if-branch
\set try_to_quit '\\q'
\if false
	:try_to_quit
	\echo `nosuchcommand` :foo :'foo' :"foo"
	\pset fieldsep | `nosuchcommand` :foo :'foo' :"foo"
	\a
	\C arg1
	\c arg1 arg2 arg3 arg4
	\cd arg1
	\conninfo
	\dt arg1
	\e arg1 arg2
	\ev whole_line
	\echo arg1 arg2 arg3 arg4 arg5
	\echo arg1
	\encoding arg1
	\errverbose
	\f arg1
	\g arg1
	\gx arg1
	SELECT 1 AS one \gset
	\?
	\i arg1
	\ir arg1
	\l arg1
	\o arg1
	\p
	\prompt arg1 arg2
	\pset arg1 arg2
	\q
	\reset
	\restrict test
	\s arg1
	\set arg1 arg2 arg3 arg4 arg5 arg6 arg7
	\setenv arg1 arg2
	\sv whole_line
	\t arg1
	\timing arg1
	\unrestrict not_valid
	\unset arg1
	\w arg1
	\x arg1
	-- \else here is eaten as part of OT_FILEPIPE argument
	\w |/no/such/file \else
	-- \endif here is eaten as part of whole-line argument
	\! whole_line \endif
\else
	\echo 'should print #8-1'
\endif

-- :{?...} defined variable test
\set i 1
\if :{?i}
  \echo '#9-1 ok, variable i is defined'
\else
  \echo 'should not print #9-2'
\endif

\if :{?no_such_variable}
  \echo 'should not print #10-1'
\else
  \echo '#10-2 ok, variable no_such_variable is not defined'
\endif

SELECT :{?i} AS i_is_defined;

SELECT NOT :{?no_such_var} AS no_such_var_is_not_defined;

-- test printing and clearing the query buffer
SELECT 1;
\p
SELECT 2 \r
\p
SELECT 3 \p
  + 4 \p
  + 5
ORDER BY 1;
\r
\p

-- tests for special result variables

-- working query, 2 rows selected
SELECT 1 AS stuff FROM generate_series(1,2);
\echo 'error:' :ERROR
\echo 'error code:' :SQLSTATE
\echo 'number of rows:' :ROW_COUNT

-- syntax error
SELECT 1 UNION;
\echo 'error:' :ERROR
\echo 'error code:' :SQLSTATE
\echo 'number of rows:' :ROW_COUNT
\echo 'last error message:' :LAST_ERROR_MESSAGE
\echo 'last error code:' :LAST_ERROR_SQLSTATE

-- empty query
;
\echo 'error:' :ERROR
\echo 'error code:' :SQLSTATE
\echo 'number of rows:' :ROW_COUNT
-- must have kept previous values
\echo 'last error message:' :LAST_ERROR_MESSAGE
\echo 'last error code:' :LAST_ERROR_SQLSTATE

-- other query error
DROP TABLE this_table_does_not_exist;
\echo 'error:' :ERROR
\echo 'error code:' :SQLSTATE
\echo 'number of rows:' :ROW_COUNT
\echo 'last error message:' :LAST_ERROR_MESSAGE
\echo 'last error code:' :LAST_ERROR_SQLSTATE

-- nondefault verbosity error settings (except verbose, which is too unstable)
\set VERBOSITY terse
SELECT 1 UNION;
\echo 'error:' :ERROR
\echo 'error code:' :SQLSTATE
\echo 'last error message:' :LAST_ERROR_MESSAGE

\set VERBOSITY sqlstate
SELECT 1/0;
\echo 'error:' :ERROR
\echo 'error code:' :SQLSTATE
\echo 'last error message:' :LAST_ERROR_MESSAGE

\set VERBOSITY default

-- \d on toast table (use pg_statistic's toast table, which has a known name)
\d pg_toast.pg_toast_2619

-- check printing info about access methods
\dA
\dA *
\dA h*
\dA foo
\dA foo bar
\dA+
\dA+ *
\dA+ h*
\dA+ foo


-- check \df, \do with argument specifications
\df *sqrt
\df *sqrt num*
\df int*pl
\df int*pl int4
\df int*pl * pg_catalog.int8
\df has_database_privilege oid text
\df has_database_privilege oid text -
\dfa bit* small*
\df *._pg_expandarray
\do - pg_catalog.int4
\do && anyarray *

-- check describing invalid multipart names
\dA regression.heap
\dA nonesuch.heap
\dt host.regression.pg_catalog.pg_class
\dt |.pg_catalog.pg_class
\dt nonesuch.pg_catalog.pg_class
\da host.regression.pg_catalog.sum
\da +.pg_catalog.sum
\da nonesuch.pg_catalog.sum
\dC host.regression.pg_catalog.int8
\dC ).pg_catalog.int8
\dC nonesuch.pg_catalog.int8
\dd host.regression.pg_catalog.pg_class
\dd [.pg_catalog.pg_class
\dd nonesuch.pg_catalog.pg_class
\ddp host.regression.pg_catalog.pg_class
\ddp {.pg_catalog.pg_class
\ddp nonesuch.pg_catalog.pg_class
\di host.regression.public.tenk1_hundred
\di ..public.tenk1_hundred
\di nonesuch.public.tenk1_hundred
\dm host.regression.public.mvtest_bb
\dm ^.public.mvtest_bb
\dm nonesuch.public.mvtest_bb
\dt host.regression.public.b_star
\dt regres+ion.public.b_star
\dt nonesuch.public.b_star
\dv host.regression.public.shoe
\dv regress(ion).public.shoe
\dv nonesuch.public.shoe
\df host.regression.public.namelen
\df regres[qrstuv]ion.public.namelen
\df nonesuch.public.namelen
\dL host.regression.plpgsql
\dL *.plpgsql
\dL nonesuch.plpgsql
\dn host.regression.public
\dn """".public
\dn nonesuch.public
\do host.regression.public.!=-
\do "regression|mydb".public.!=-
\do nonesuch.public.!=-
\dO host.regression.pg_catalog.POSIX
\dO .pg_catalog.POSIX
\dO nonesuch.pg_catalog.POSIX
\drds nonesuch.lc_messages
\drds regression.lc_messages
\dx regression.plpgsql
\dx nonesuch.plpgsql
\dy regression.myevt
\dy nonesuch.myevt

-- check that dots within quoted name segments are not counted
\dA "no.such.access.method"
\dt "no.such.table.relation"
\da "no.such.aggregate.function"
\dAc "no.such.operator.class"
\dC "no.such.cast"
\dd "no.such.object.description"
\ddp "no.such.default.access.privilege"
\di "no.such.index.relation"
\dm "no.such.materialized.view"
\dt "no.such.relation"
\dv "no.such.relation"
\df "no.such.function"
\dL "no.such.language"
\dn "no.such.schema"
\do "no.such.operator"
\dO "no.such.collation"
\drds "no.such.setting"
\dT "no.such.data.type"
\dx "no.such.installed.extension"
\dy "no.such.event.trigger"

-- again, but with dotted schema qualifications.
\dA "no.such.schema"."no.such.access.method"
\dt "no.such.schema"."no.such.table.relation"
\da "no.such.schema"."no.such.aggregate.function"
\dAc "no.such.schema"."no.such.operator.class"
\dC "no.such.schema"."no.such.cast"
\dd "no.such.schema"."no.such.object.description"
\ddp "no.such.schema"."no.such.default.access.privilege"
\di "no.such.schema"."no.such.index.relation"
\dm "no.such.schema"."no.such.materialized.view"
\dt "no.such.schema"."no.such.relation"
\dv "no.such.schema"."no.such.relation"
\df "no.such.schema"."no.such.function"
\dL "no.such.schema"."no.such.language"
\do "no.such.schema"."no.such.operator"
\dO "no.such.schema"."no.such.collation"
\drds "no.such.schema"."no.such.setting"
\dT "no.such.schema"."no.such.data.type"
\dx "no.such.schema"."no.such.installed.extension"
\dy "no.such.schema"."no.such.event.trigger"

-- again, but with current database and dotted schema qualifications.
\dt regression."no.such.schema"."no.such.table.relation"
\da regression."no.such.schema"."no.such.aggregate.function"
\dC regression."no.such.schema"."no.such.cast"
\dd regression."no.such.schema"."no.such.object.description"
\di regression."no.such.schema"."no.such.index.relation"
\dm regression."no.such.schema"."no.such.materialized.view"
\dt regression."no.such.schema"."no.such.relation"
\dv regression."no.such.schema"."no.such.relation"
\df regression."no.such.schema"."no.such.function"
\do regression."no.such.schema"."no.such.operator"
\dO regression."no.such.schema"."no.such.collation"
\dT regression."no.such.schema"."no.such.data.type"

-- again, but with dotted database and dotted schema qualifications.
\dt "no.such.database"."no.such.schema"."no.such.table.relation"
\da "no.such.database"."no.such.schema"."no.such.aggregate.function"
\dC "no.such.database"."no.such.schema"."no.such.cast"
\dd "no.such.database"."no.such.schema"."no.such.object.description"
\ddp "no.such.database"."no.such.schema"."no.such.default.access.privilege"
\di "no.such.database"."no.such.schema"."no.such.index.relation"
\dm "no.such.database"."no.such.schema"."no.such.materialized.view"
\dt "no.such.database"."no.such.schema"."no.such.relation"
\dv "no.such.database"."no.such.schema"."no.such.relation"
\df "no.such.database"."no.such.schema"."no.such.function"
\do "no.such.database"."no.such.schema"."no.such.operator"
\dO "no.such.database"."no.such.schema"."no.such.collation"
\dT "no.such.database"."no.such.schema"."no.such.data.type"
