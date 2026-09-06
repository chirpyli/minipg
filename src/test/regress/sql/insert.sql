--
-- insert with DEFAULT in the target_list
--
create table inserttest (col1 int4, col2 int4, col3 text default 'testing');
insert into inserttest (col1, col2, col3) values (DEFAULT, DEFAULT, DEFAULT);
insert into inserttest (col2, col3) values (3, DEFAULT);
insert into inserttest (col1, col2, col3) values (DEFAULT, 5, DEFAULT);
insert into inserttest values (DEFAULT, 5, 'test');
insert into inserttest values (DEFAULT, 7);

select * from inserttest;

--
-- insert with similar expression / target_list values (all fail)
--
insert into inserttest (col1, col2, col3) values (DEFAULT, DEFAULT);
insert into inserttest (col1, col2, col3) values (1, 2);
insert into inserttest (col1) values (1, 2);
insert into inserttest (col1) values (DEFAULT, DEFAULT);

select * from inserttest;

--
-- VALUES test
--
insert into inserttest values(10, 20, '40'), (-1, 2, DEFAULT),
    ((select 2), (select i from (values(3)) as foo (i)), 'values are fun!');

select * from inserttest;

--
-- TOASTed value test
--
insert into inserttest values(30, 50, repeat('x', 10000));

select col1, col2, char_length(col3) from inserttest;

drop table inserttest;

--
-- tuple larger than page
--
CREATE TABLE large_tuple_test (a int, b text);
ALTER TABLE large_tuple_test ALTER COLUMN b SET STORAGE plain;

-- create page w/ free space in range [nearlyEmptyFreeSpace, MaxHeapTupleSize)
INSERT INTO large_tuple_test (select 1, NULL);

-- should still fit on the page
INSERT INTO large_tuple_test (select 2, repeat('a', 1000));
SELECT pg_size_pretty(pg_relation_size('large_tuple_test'::regclass, 'main'));

-- add small record to the second page
INSERT INTO large_tuple_test (select 3, NULL);

-- now this tuple won't fit on the second page, but the insert should
-- still succeed by extending the relation
INSERT INTO large_tuple_test (select 4, repeat('a', 8126));

DROP TABLE large_tuple_test;

