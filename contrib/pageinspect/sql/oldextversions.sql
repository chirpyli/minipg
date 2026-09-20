-- test old extension version entry points

DROP EXTENSION pageinspect;
CREATE EXTENSION pageinspect VERSION '1.8';

CREATE TABLE test1 (a int8, b text);
INSERT INTO test1 VALUES (72057594037927937, 'text');
CREATE INDEX test1_a_idx ON test1 USING btree (a);

-- from page.sql
SELECT octet_length(get_raw_page('test1', 0)) AS main_0;
SELECT octet_length(get_raw_page('test1', 'main', 0)) AS main_0;

-- from btree.sql
-- minipg: function-in-FROM 已裁剪，改为在子查询目标列中调用集返回函数并展开记录
SELECT * FROM (SELECT (bt_page_stats('test1_a_idx', 1)).* ) AS _gs;
SELECT * FROM (SELECT (bt_page_items('test1_a_idx', 1)).* ) AS _gs;

DROP TABLE test1;
DROP EXTENSION pageinspect;
