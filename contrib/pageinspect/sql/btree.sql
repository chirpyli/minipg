CREATE TABLE test1 (a int8, b int4);
INSERT INTO test1 VALUES (72057594037927937, 0);
CREATE INDEX test1_a_idx ON test1 USING btree (a);

\x

-- minipg: function-in-FROM 已裁剪，改为在子查询目标列中调用集返回函数并展开记录
SELECT * FROM (SELECT (bt_metap('test1_a_idx')).* ) AS _gs;

SELECT * FROM (SELECT (bt_page_stats('test1_a_idx', -1)).* ) AS _gs;
SELECT * FROM (SELECT (bt_page_stats('test1_a_idx', 0)).* ) AS _gs;
SELECT * FROM (SELECT (bt_page_stats('test1_a_idx', 1)).* ) AS _gs;
SELECT * FROM (SELECT (bt_page_stats('test1_a_idx', 2)).* ) AS _gs;

SELECT * FROM (SELECT (bt_page_items('test1_a_idx', -1)).* ) AS _gs;
SELECT * FROM (SELECT (bt_page_items('test1_a_idx', 0)).* ) AS _gs;
SELECT * FROM (SELECT (bt_page_items('test1_a_idx', 1)).* ) AS _gs;
SELECT * FROM (SELECT (bt_page_items('test1_a_idx', 2)).* ) AS _gs;

SELECT * FROM (SELECT (bt_page_items(get_raw_page('test1_a_idx', -1))).* ) AS _gs;
SELECT * FROM (SELECT (bt_page_items(get_raw_page('test1_a_idx', 0))).* ) AS _gs;
SELECT * FROM (SELECT (bt_page_items(get_raw_page('test1_a_idx', 1))).* ) AS _gs;
SELECT * FROM (SELECT (bt_page_items(get_raw_page('test1_a_idx', 2))).* ) AS _gs;

-- Failure when using a non-btree index.
CREATE INDEX test1_a_hash ON test1 USING hash(a);
SELECT bt_metap('test1_a_hash');
SELECT bt_page_stats('test1_a_hash', 0);
SELECT bt_page_items('test1_a_hash', 0);
SELECT bt_page_items(get_raw_page('test1_a_hash', 0));

-- Several failure modes.
-- Suppress the DETAIL message, to allow the tests to work across various
-- page sizes and architectures.
\set VERBOSITY terse
-- invalid page size
SELECT * FROM (SELECT (bt_page_items('aaa'::bytea)).* ) AS _gs;
-- invalid special area size
SELECT * FROM (SELECT (bt_page_items(get_raw_page('test1', 0))).* ) AS _gs;
\set VERBOSITY default

-- Tests with all-zero pages.
SHOW block_size \gset
SELECT * FROM (SELECT (bt_page_items(decode(repeat('00', :block_size), 'hex'))).* ) AS _gs;

DROP TABLE test1;
