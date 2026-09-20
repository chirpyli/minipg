--
-- SELECT_DISTINCT
--

--
-- awk '{print $3;}' onek.data | sort -n | uniq
--
SELECT DISTINCT two FROM tmp ORDER BY 1;

--
-- awk '{print $5;}' onek.data | sort -n | uniq
--
SELECT DISTINCT ten FROM tmp ORDER BY 1;

--
-- awk '{print $16;}' onek.data | sort -d | uniq
--
SELECT DISTINCT string4 FROM tmp ORDER BY 1;

--
-- awk '{print $3,$16,$5;}' onek.data | sort -d | uniq |
-- sort +0n -1 +1d -2 +2n -3
--
SELECT DISTINCT two, string4, ten
   FROM tmp
   ORDER BY two, string4, ten;

--
-- awk '{print $2;}' person.data |
-- awk '{if(NF!=1){print $2;}else{print;}}' - emp.data |
-- awk '{if(NF!=1){print $2;}else{print;}}' - student.data |
-- awk 'BEGIN{FS="      ";}{if(NF!=1){print $5;}else{print;}}' - stud_emp.data |
-- sort -n -r | uniq
--
SELECT DISTINCT p.age FROM person p ORDER BY age DESC;

--
-- Check mentioning same column more than once
--

EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FROM
  (SELECT DISTINCT two, four, two FROM tenk1) ss;

SELECT count(*) FROM
  (SELECT DISTINCT two, four, two FROM tenk1) ss;

--
-- Compare results between plans using sorting and plans using hash
-- aggregation. Force spilling in both cases by setting work_mem low.
--

SET work_mem='64kB';

-- Produce results with sorting.

SET enable_hashagg=FALSE;

EXPLAIN (costs off)
SELECT DISTINCT g%1000 FROM (SELECT generate_series(0,9999) AS g) AS _gs;

CREATE TABLE distinct_group_1 AS
SELECT DISTINCT g%1000 FROM (SELECT generate_series(0,9999) AS g) AS _gs;

CREATE TABLE distinct_group_2 AS
SELECT DISTINCT (g%1000)::text FROM (SELECT generate_series(0,9999) AS g) AS _gs;

SET enable_hashagg=TRUE;

-- Produce results with hash aggregation.

SET enable_sort=FALSE;

EXPLAIN (costs off)
SELECT DISTINCT g%1000 FROM (SELECT generate_series(0,9999) AS g) AS _gs;

CREATE TABLE distinct_hash_1 AS
SELECT DISTINCT g%1000 FROM (SELECT generate_series(0,9999) AS g) AS _gs;

CREATE TABLE distinct_hash_2 AS
SELECT DISTINCT (g%1000)::text FROM (SELECT generate_series(0,9999) AS g) AS _gs;

SET enable_sort=TRUE;

SET work_mem TO DEFAULT;

DROP TABLE distinct_hash_1;
DROP TABLE distinct_hash_2;
DROP TABLE distinct_group_1;
DROP TABLE distinct_group_2;

