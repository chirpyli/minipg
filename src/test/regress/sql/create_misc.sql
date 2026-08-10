--
-- CREATE_MISC
--

-- CLASS POPULATION
--	(any resemblance to real life is purely coincidental)
--

INSERT INTO tenk2 SELECT * FROM tenk1;

CREATE TABLE onek2 (
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
INSERT INTO onek2 SELECT * FROM onek;

INSERT INTO fast_emp4000 SELECT * FROM slow_emp4000;

CREATE TABLE Bprime (
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
INSERT INTO Bprime
	SELECT * FROM tenk1
	WHERE unique2 < 1000;

INSERT INTO hobbies_r (name, person)
   SELECT 'posthacking', p.name
   FROM person p
   WHERE p.name = 'mike' or p.name = 'jeff';

INSERT INTO hobbies_r (name, person)
   SELECT 'basketball', p.name
   FROM person p
   WHERE p.name = 'joe' or p.name = 'sally';

INSERT INTO hobbies_r (name) VALUES ('skywalking');

INSERT INTO equipment_r (name, hobby) VALUES ('advil', 'posthacking');

INSERT INTO equipment_r (name, hobby) VALUES ('peet''s coffee', 'posthacking');

INSERT INTO equipment_r (name, hobby) VALUES ('hightops', 'basketball');

INSERT INTO equipment_r (name, hobby) VALUES ('guts', 'skywalking');

INSERT INTO city VALUES
('Podunk', '(1,2),(3,4)', '100,127,1000'),
('Gotham', '(1000,34),(1100,334)', '123456,127,-1000,6789');
TABLE city;

CREATE TABLE ramp (
	name		text,
	thepath 	text
);
INSERT INTO ramp
	SELECT * FROM road
	WHERE name LIKE '%Ramp';

-- 注：原 create_misc 中的 person* 继承展开、ihighway/shighway(a_star/b_star/c_star
-- /d_star/e_star/f_star) 等均为 INHERITS 表继承层级，随 minipg 移除 INHERITS 语法一并
-- 删除；这些继承表无其余回归测试引用，故不再创建。另外 minipg 已移除 polygon 类型、
-- CREATE TABLE AS / SELECT INTO 语法，故改用显式建表 + INSERT SELECT 的形式改写。

--
-- for internal portal (cursor) tests
--
CREATE TABLE iportaltest (
	i		int4,
	d		float4,
	p		text
);

INSERT INTO iportaltest (i, d, p)
   VALUES (1, 3.567, '(3.0,1.0),(4.0,2.0)');

INSERT INTO iportaltest (i, d, p)
   VALUES (2, 89.05, '(4.0,2.0),(3.0,1.0)');
