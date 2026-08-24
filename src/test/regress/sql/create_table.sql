--
-- CREATE_TABLE
--

--
-- CLASS DEFINITIONS
--
CREATE TABLE hobbies_r (
	name		text,
	person 		text
);

CREATE TABLE equipment_r (
	name 		text,
	hobby		text
);

CREATE TABLE onek (
	unique1		int4,
	unique2		int4,
	two			int4,
	four		int4,
	ten			int4,
	twenty		int4,
	hundred		int4,
	thousand	int4,
	twothousand	int4,
	fivethous	int4,
	tenthous	int4,
	odd			int4,
	even		int4,
	stringu1	name,
	stringu2	name,
	string4		name
);

CREATE TABLE tenk1 (
	unique1		int4,
	unique2		int4,
	two			int4,
	four		int4,
	ten			int4,
	twenty		int4,
	hundred		int4,
	thousand	int4,
	twothousand	int4,
	fivethous	int4,
	tenthous	int4,
	odd			int4,
	even		int4,
	stringu1	name,
	stringu2	name,
	string4		name
);

CREATE TABLE tenk2 (
	unique1 	int4,
	unique2 	int4,
	two 	 	int4,
	four 		int4,
	ten			int4,
	twenty 		int4,
	hundred 	int4,
	thousand 	int4,
	twothousand int4,
	fivethous 	int4,
	tenthous	int4,
	odd			int4,
	even		int4,
	stringu1	name,
	stringu2	name,
	string4		name
);


CREATE TABLE person (
	name 		text,
	age			int4,
	location 	text
);


-- minipg: city_budget 原为 create_type.sql 定义的基础类型(element=int4)，
-- CREATE TYPE 基础形式已裁剪，budget 列改用 text
CREATE TABLE city (
	name		name,
	location 	text,
	budget 		text
);

CREATE TABLE dept (
	dname		name,
	mgrname 	text
);

CREATE TABLE slow_emp4000 (
	home_base	 text
);

CREATE TABLE fast_emp4000 (
	home_base	 text
);

CREATE TABLE road (
	name		text,
	thepath 	text
);

CREATE TABLE real_city (
	pop			int4,
	cname		text,
	outline 	text
);

CREATE TABLE aggtest (
	a 			int2,
	b			float4
);

CREATE TABLE hash_i4_heap (
	seqno 		int4,
	random 		int4
);

CREATE TABLE hash_name_heap (
	seqno 		int4,
	random 		name
);

CREATE TABLE hash_txt_heap (
	seqno 		int4,
	random 		text
);

CREATE TABLE hash_f8_heap (
	seqno		int4,
	random 		float8
);

-- don't include the hash_ovfl_heap stuff in the distribution
-- the data set is too large for what it's worth
--
-- CREATE TABLE hash_ovfl_heap (
--	x			int4,
--	y			int4
-- );

CREATE TABLE bt_i4_heap (
	seqno 		int4,
	random 		int4
);

CREATE TABLE bt_name_heap (
	seqno 		name,
	random 		int4
);

CREATE TABLE bt_txt_heap (
	seqno 		text,
	random 		int4
);

CREATE TABLE bt_f8_heap (
	seqno 		float8,
	random 		int4
);

CREATE TABLE array_op_test (
	seqno		int4,
	i			int4[],
	t			text[]
);

CREATE TABLE array_index_op_test (
	seqno		int4,
	i			int4[],
	t			text[]
);

CREATE TABLE testjsonb (
       j text
);

CREATE TABLE unknowntab (
	u unknown    -- fail
);

CREATE TYPE unknown_comptype AS (
	u unknown    -- fail
);

CREATE TABLE unlogged2 (a int primary key);			-- OK
SELECT relname, relkind, relpersistence FROM pg_class WHERE relname LIKE 'unlogged%' ORDER BY relname;
REINDEX INDEX unlogged2_pkey;
SELECT relname, relkind, relpersistence FROM pg_class WHERE relname LIKE 'unlogged%' ORDER BY relname;
DROP TABLE unlogged2;

-- create an extra wide table to test for issues related to that
-- (temporarily hide query, to avoid the long CREATE TABLE stmt)
\set ECHO none
SELECT 'CREATE TABLE extra_wide_table(firstc text, '|| array_to_string(array_agg('c'||i||' bool'),',')||', lastc text);'
FROM generate_series(1, 1100) g(i)
\gexec
\set ECHO all
INSERT INTO extra_wide_table(firstc, lastc) VALUES('first col', 'last col');
SELECT firstc, lastc FROM extra_wide_table;

-- check that tables with oids cannot be created anymore
CREATE TABLE withoid() WITH OIDS;
CREATE TABLE withoid() WITH (oids);
CREATE TABLE withoid() WITH (oids = true);
DROP TABLE withoid;

-- but explicitly not adding oids is still supported
CREATE TABLE withoutoid() WITHOUT OIDS; DROP TABLE withoutoid;
CREATE TABLE withoutoid() WITH (oids = false); DROP TABLE withoutoid;

-- temporary tables are ignored by pg_filenode_relation().
CREATE TABLE relation_filenode_check(c1 int);
SELECT relpersistence,
  pg_filenode_relation (reltablespace, pg_relation_filenode(oid))
  FROM pg_class
  WHERE relname = 'relation_filenode_check';
DROP TABLE relation_filenode_check;

-- check restriction with default expressions
-- invalid use of column reference in default expressions
CREATE TABLE default_expr_column (id int DEFAULT (id));
CREATE TABLE default_expr_column (id int DEFAULT (bar.id));
CREATE TABLE default_expr_agg_column (id int DEFAULT (avg(id)));
-- invalid column definition
CREATE TABLE default_expr_non_column (a int DEFAULT (avg(non_existent)));
-- invalid use of aggregate
CREATE TABLE default_expr_agg (a int DEFAULT (avg(1)));
-- invalid use of subquery
CREATE TABLE default_expr_agg (a int DEFAULT (select 1));
-- invalid use of set-returning function
CREATE TABLE default_expr_agg (a int DEFAULT (generate_series(1,3)));

-- Verify that subtransaction rollback restores rd_createSubid.
BEGIN;
CREATE TABLE remember_create_subid (c int);
SAVEPOINT q; DROP TABLE remember_create_subid; ROLLBACK TO q;
COMMIT;
DROP TABLE remember_create_subid;

-- Verify that subtransaction rollback restores rd_firstRelfilenodeSubid.
CREATE TABLE remember_node_subid (c int);
BEGIN;
ALTER TABLE remember_node_subid ALTER c TYPE bigint;
SAVEPOINT q; DROP TABLE remember_node_subid; ROLLBACK TO q;
COMMIT;
DROP TABLE remember_node_subid;

-- 加载标准回归测试数据（原由 copy.sql/input/copy.source 负责，因 INHERITS 继承
-- 相关测试(create_table 继承链、copy、misc、create_function_2)已随继承功能裁剪而
-- 移除，此处改为在 create_table 内直接加载非继承标准表数据，供其余回归测试使用）
-- 注：SQL COPY 命令与 psql \copy 元命令已随 COPY 功能裁剪而移除，故改用等价的
-- INSERT 脚本(data/load_*.sql，由 data/*.data 经 COPY 文本格式转换生成)以加载数据
\set ECHO none
\i data/load_aggtest.sql
\i data/load_onek.sql
\i data/load_tenk1.sql
\i data/load_slow_emp4000.sql
\i data/load_person.sql
\i data/load_dept.sql
\i data/load_real_city.sql
\set ECHO all
