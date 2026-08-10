CREATE EXTENSION pgstattuple;

--
-- It's difficult to come up with platform-independent test cases for
-- the pgstattuple functions, but the results for empty tables and
-- indexes should be that.
--

create table test (a int primary key, b int[]);

select * from pgstattuple('test');
select * from pgstattuple('test'::text);
select * from pgstattuple('test'::name);
select * from pgstattuple('test'::regclass);
select pgstattuple(oid) from pg_class where relname = 'test';
select pgstattuple(relname) from pg_class where relname = 'test';

select version, tree_level,
    index_size / current_setting('block_size')::int as index_size,
    root_block_no, internal_pages, leaf_pages, empty_pages, deleted_pages,
    avg_leaf_density, leaf_fragmentation
    from pgstatindex('test_pkey');
select version, tree_level,
    index_size / current_setting('block_size')::int as index_size,
    root_block_no, internal_pages, leaf_pages, empty_pages, deleted_pages,
    avg_leaf_density, leaf_fragmentation
    from pgstatindex('test_pkey'::text);
select version, tree_level,
    index_size / current_setting('block_size')::int as index_size,
    root_block_no, internal_pages, leaf_pages, empty_pages, deleted_pages,
    avg_leaf_density, leaf_fragmentation
    from pgstatindex('test_pkey'::name);
select version, tree_level,
    index_size / current_setting('block_size')::int as index_size,
    root_block_no, internal_pages, leaf_pages, empty_pages, deleted_pages,
    avg_leaf_density, leaf_fragmentation
    from pgstatindex('test_pkey'::regclass);

select pg_relpages('test');
select pg_relpages('test_pkey');
select pg_relpages('test_pkey'::text);
select pg_relpages('test_pkey'::name);
select pg_relpages('test_pkey'::regclass);
select pg_relpages(oid) from pg_class where relname = 'test_pkey';
select pg_relpages(relname) from pg_class where relname = 'test_pkey';



create index test_hashidx on test using hash (b);

select * from pgstathashindex('test_hashidx');

-- these should error with the wrong type
select pgstathashindex('test_pkey');


select pgstatindex('test_hashidx');

-- 分区功能已在 minipg 中裁剪（不支持 CREATE TABLE ... PARTITION BY），
-- 因此不再测试 pgstattuple 系列函数对分区表/分区索引的报错场景。

create view test_view as select 1;
-- these should all fail
select pgstattuple('test_view');
select pgstattuple_approx('test_view');
select pg_relpages('test_view');
select pgstatindex('test_view');
select pgstathashindex('test_view');

create foreign data wrapper dummy;
create server dummy_server foreign data wrapper dummy;
create foreign table test_foreign_table () server dummy_server;
-- these should all fail
select pgstattuple('test_foreign_table');
select pgstattuple_approx('test_foreign_table');
select pg_relpages('test_foreign_table');
select pgstatindex('test_foreign_table');
select pgstathashindex('test_foreign_table');

-- 分区功能已在 minipg 中裁剪，不再测试分区子表及其索引的 pgstattuple 调用。

-- toast tables should work
select pgstattuple((select reltoastrelid from pg_class where relname = 'test'));
select pgstattuple_approx((select reltoastrelid from pg_class where relname = 'test'));
select pg_relpages((select reltoastrelid from pg_class where relname = 'test'));

-- these should work for sequences
create sequence test_sequence;
select count(*) from pgstattuple('test_sequence');
select pg_relpages('test_sequence');

-- these should fail for sequences
select pgstatindex('test_sequence');
select pgstathashindex('test_sequence');
select pgstattuple_approx('test_sequence');

drop sequence test_sequence;
drop view test_view;
drop foreign table test_foreign_table;
drop server dummy_server;
drop foreign data wrapper dummy;
