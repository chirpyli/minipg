--
-- Tests for psql features that aren't closely connected to any
-- specific server features.
--
-- This is a trimmed version covering only the commands retained in
-- minipg's psql: \c \g \gx \d \dt \l \x \q plus the test-suite helpers
-- \i \set \unset \pset \a \t \if \echo and the special result variables.
--

-- \set / \unset / \echo
\set foo bar
\echo :foo
\set num 42
\echo :num
\unset foo
\unset num

-- \pset / \a / \t
\pset format aligned
\pset null '(null)'
\a
\t

-- \g / \gx
SELECT 1 AS one, 2 AS two \g
\gx
SELECT 3 AS three, 4 AS four \gx
\g

-- \x expanded display
\x on
SELECT 1 AS a, 2 AS b;
\x off

-- special result variables
SELECT 1 / 0;
\echo :ERROR
\echo :SQLSTATE
SELECT 1;
\echo :ROW_COUNT

-- \if / \endif
\if true
  \echo branch_taken
\endif
\if false
  \echo should_not_appear
\endif

-- \i include a script file
\i data/psql_incl.sql

-- describe / list on a temp table
CREATE TABLE psql_test (id int, val text);
\d psql_test
\dt psql_test
DROP TABLE psql_test;

-- list databases
\l
