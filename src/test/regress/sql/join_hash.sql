--
-- exercises for the hash join code
--

begin;

set local min_parallel_table_scan_size = 0;
set local parallel_setup_cost = 0;
set local enable_hashjoin = on;


-- Verify that hash key expressions reference the correct
-- nodes. Hashjoin's hashkeys need to reference its outer plan, Hash's
-- need to reference Hash's outer plan (which is below HashJoin's
-- inner plan). It's not trivial to verify that the references are
-- correct (we don't display the hashkeys themselves), but if the
-- hashkeys contain subplan references, those will be displayed. Force
-- subplans to appear just about everywhere.
--
-- Bug report:
-- https://www.postgresql.org/message-id/CAPpHfdvGVegF_TKKRiBrSmatJL2dR9uwFCuR%2BteQ_8tEXU8mxg%40mail.gmail.com
--
BEGIN;
SET LOCAL enable_sort = OFF; -- avoid mergejoins
SET LOCAL from_collapse_limit = 1; -- allows easy changing of join order

CREATE TABLE hjtest_1 (a text, b int, id int, c bool);
CREATE TABLE hjtest_2 (a bool, id int, b text, c int);

INSERT INTO hjtest_1(a, b, id, c) VALUES ('text', 2, 1, false); -- matches
INSERT INTO hjtest_1(a, b, id, c) VALUES ('text', 1, 2, false); -- fails id join condition
INSERT INTO hjtest_1(a, b, id, c) VALUES ('text', 20, 1, false); -- fails < 50
INSERT INTO hjtest_1(a, b, id, c) VALUES ('text', 1, 1, false); -- fails (SELECT hjtest_1.b * 5) = (SELECT hjtest_2.c*5)

INSERT INTO hjtest_2(a, id, b, c) VALUES (true, 1, 'another', 2); -- matches
INSERT INTO hjtest_2(a, id, b, c) VALUES (true, 3, 'another', 7); -- fails id join condition
INSERT INTO hjtest_2(a, id, b, c) VALUES (true, 1, 'another', 90);  -- fails < 55
INSERT INTO hjtest_2(a, id, b, c) VALUES (true, 1, 'another', 3); -- fails (SELECT hjtest_1.b * 5) = (SELECT hjtest_2.c*5)
INSERT INTO hjtest_2(a, id, b, c) VALUES (true, 1, 'text', 1); --  fails hjtest_1.a <> hjtest_2.b;

EXPLAIN (COSTS OFF, VERBOSE)
SELECT hjtest_1.a a1, hjtest_2.a a2,hjtest_1.tableoid::regclass t1, hjtest_2.tableoid::regclass t2
FROM hjtest_1, hjtest_2
WHERE
    hjtest_1.id = (SELECT 1 WHERE hjtest_2.id = 1)
    AND (SELECT hjtest_1.b * 5) = (SELECT hjtest_2.c*5)
    AND (SELECT hjtest_1.b * 5) < 50
    AND (SELECT hjtest_2.c * 5) < 55
    AND hjtest_1.a <> hjtest_2.b;

SELECT hjtest_1.a a1, hjtest_2.a a2,hjtest_1.tableoid::regclass t1, hjtest_2.tableoid::regclass t2
FROM hjtest_1, hjtest_2
WHERE
    hjtest_1.id = (SELECT 1 WHERE hjtest_2.id = 1)
    AND (SELECT hjtest_1.b * 5) = (SELECT hjtest_2.c*5)
    AND (SELECT hjtest_1.b * 5) < 50
    AND (SELECT hjtest_2.c * 5) < 55
    AND hjtest_1.a <> hjtest_2.b;

EXPLAIN (COSTS OFF, VERBOSE)
SELECT hjtest_1.a a1, hjtest_2.a a2,hjtest_1.tableoid::regclass t1, hjtest_2.tableoid::regclass t2
FROM hjtest_2, hjtest_1
WHERE
    hjtest_1.id = (SELECT 1 WHERE hjtest_2.id = 1)
    AND (SELECT hjtest_1.b * 5) = (SELECT hjtest_2.c*5)
    AND (SELECT hjtest_1.b * 5) < 50
    AND (SELECT hjtest_2.c * 5) < 55
    AND hjtest_1.a <> hjtest_2.b;

SELECT hjtest_1.a a1, hjtest_2.a a2,hjtest_1.tableoid::regclass t1, hjtest_2.tableoid::regclass t2
FROM hjtest_2, hjtest_1
WHERE
    hjtest_1.id = (SELECT 1 WHERE hjtest_2.id = 1)
    AND (SELECT hjtest_1.b * 5) = (SELECT hjtest_2.c*5)
    AND (SELECT hjtest_1.b * 5) < 50
    AND (SELECT hjtest_2.c * 5) < 55
    AND hjtest_1.a <> hjtest_2.b;

ROLLBACK;

-- Verify that we behave sanely when the inner hash keys contain parameters
-- (that is, outer or lateral references).  This situation has to defeat
-- re-use of the inner hash table across rescans.
begin;
set local enable_hashjoin = on;

explain (costs off)
select i8.q2, ss.* from
int8_tbl i8,
lateral (select t1.fivethous, i4.f1 from tenk1 t1 join int4_tbl i4
         on t1.fivethous = i4.f1+i8.q2 order by 1,2) ss;

select i8.q2, ss.* from
int8_tbl i8,
lateral (select t1.fivethous, i4.f1 from tenk1 t1 join int4_tbl i4
         on t1.fivethous = i4.f1+i8.q2 order by 1,2) ss;

rollback;
