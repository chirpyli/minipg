--
-- FLOAT4
--

CREATE TABLE FLOAT4_TBL (f1  float4);

INSERT INTO FLOAT4_TBL(f1) VALUES ('    0.0');
INSERT INTO FLOAT4_TBL(f1) VALUES ('1004.30   ');
INSERT INTO FLOAT4_TBL(f1) VALUES ('     -34.84    ');
INSERT INTO FLOAT4_TBL(f1) VALUES ('1.2345678901234e+20');
INSERT INTO FLOAT4_TBL(f1) VALUES ('1.2345678901234e-20');

-- test for over and under flow
INSERT INTO FLOAT4_TBL(f1) VALUES ('10e70');
INSERT INTO FLOAT4_TBL(f1) VALUES ('-10e70');
INSERT INTO FLOAT4_TBL(f1) VALUES ('10e-70');
INSERT INTO FLOAT4_TBL(f1) VALUES ('-10e-70');

INSERT INTO FLOAT4_TBL(f1) VALUES ('10e70'::float8);
INSERT INTO FLOAT4_TBL(f1) VALUES ('-10e70'::float8);
INSERT INTO FLOAT4_TBL(f1) VALUES ('10e-70'::float8);
INSERT INTO FLOAT4_TBL(f1) VALUES ('-10e-70'::float8);

INSERT INTO FLOAT4_TBL(f1) VALUES ('10e400');
INSERT INTO FLOAT4_TBL(f1) VALUES ('-10e400');
INSERT INTO FLOAT4_TBL(f1) VALUES ('10e-400');
INSERT INTO FLOAT4_TBL(f1) VALUES ('-10e-400');

-- bad input
INSERT INTO FLOAT4_TBL(f1) VALUES ('');
INSERT INTO FLOAT4_TBL(f1) VALUES ('       ');
INSERT INTO FLOAT4_TBL(f1) VALUES ('xyz');
INSERT INTO FLOAT4_TBL(f1) VALUES ('5.0.0');
INSERT INTO FLOAT4_TBL(f1) VALUES ('5 . 0');
INSERT INTO FLOAT4_TBL(f1) VALUES ('5.   0');
INSERT INTO FLOAT4_TBL(f1) VALUES ('     - 3.0');
INSERT INTO FLOAT4_TBL(f1) VALUES ('123            5');

-- special inputs
SELECT 'NaN'::float4;
SELECT 'nan'::float4;
SELECT '   NAN  '::float4;
SELECT 'infinity'::float4;
SELECT '          -INFINiTY   '::float4;
-- bad special inputs
SELECT 'N A N'::float4;
SELECT 'NaN x'::float4;
SELECT ' INFINITY    x'::float4;

SELECT 'Infinity'::float4 + 100.0;
SELECT 'Infinity'::float4 / 'Infinity'::float4;
SELECT '42'::float4 / 'Infinity'::float4;
SELECT 'nan'::float4 / 'nan'::float4;
SELECT 'nan'::float4 / '0'::float4;
SELECT 'nan'::int8::float4;

SELECT * FROM FLOAT4_TBL;

SELECT f.* FROM FLOAT4_TBL f WHERE f.f1 <> '1004.3';

SELECT f.* FROM FLOAT4_TBL f WHERE f.f1 = '1004.3';

SELECT f.* FROM FLOAT4_TBL f WHERE '1004.3' > f.f1;

SELECT f.* FROM FLOAT4_TBL f WHERE  f.f1 < '1004.3';

SELECT f.* FROM FLOAT4_TBL f WHERE '1004.3' >= f.f1;

SELECT f.* FROM FLOAT4_TBL f WHERE  f.f1 <= '1004.3';

SELECT f.f1, f.f1 * '-10' AS x FROM FLOAT4_TBL f
   WHERE f.f1 > '0.0';

SELECT f.f1, f.f1 + '-10' AS x FROM FLOAT4_TBL f
   WHERE f.f1 > '0.0';

SELECT f.f1, f.f1 / '-10' AS x FROM FLOAT4_TBL f
   WHERE f.f1 > '0.0';

SELECT f.f1, f.f1 - '-10' AS x FROM FLOAT4_TBL f
   WHERE f.f1 > '0.0';

-- test divide by zero
SELECT f.f1 / '0.0' from FLOAT4_TBL f;

SELECT * FROM FLOAT4_TBL;

-- test the unary float4abs operator
SELECT f.f1, @f.f1 AS abs_f1 FROM FLOAT4_TBL f;

UPDATE FLOAT4_TBL
   SET f1 = FLOAT4_TBL.f1 * '-1'
   WHERE FLOAT4_TBL.f1 > '0.0';

SELECT * FROM FLOAT4_TBL;

-- test edge-case coercions to integer
SELECT '32767.4'::float4::int2;
SELECT '32767.6'::float4::int2;
SELECT '-32768.4'::float4::int2;
SELECT '-32768.6'::float4::int2;
SELECT '2147483520'::float4::int4;
SELECT '2147483647'::float4::int4;
SELECT '-2147483648.5'::float4::int4;
SELECT '-2147483900'::float4::int4;
SELECT '9223369837831520256'::float4::int8;
SELECT '9223372036854775807'::float4::int8;
SELECT '-9223372036854775808.5'::float4::int8;
SELECT '-9223380000000000000'::float4::int8;

-- Test for correct input rounding in edge cases.
-- These lists are from Paxson 1991, excluding subnormals and
-- inputs of over 9 sig. digits.

SELECT float4send('5e-20'::float4);
SELECT float4send('67e14'::float4);
SELECT float4send('985e15'::float4);
SELECT float4send('55895e-16'::float4);
SELECT float4send('7038531e-32'::float4);
SELECT float4send('702990899e-20'::float4);

SELECT float4send('3e-23'::float4);
SELECT float4send('57e18'::float4);
SELECT float4send('789e-35'::float4);
SELECT float4send('2539e-18'::float4);
SELECT float4send('76173e28'::float4);
SELECT float4send('887745e-11'::float4);
SELECT float4send('5382571e-37'::float4);
SELECT float4send('82381273e-35'::float4);
SELECT float4send('750486563e-38'::float4);

-- Test that the smallest possible normalized input value inputs
-- correctly, either in 9-significant-digit or shortest-decimal
-- format.
--
-- exact val is             1.1754943508...
-- shortest val is          1.1754944000
-- midpoint to next val is  1.1754944208...

SELECT float4send('1.17549435e-38'::float4);
SELECT float4send('1.1754944e-38'::float4);

-- test output (and round-trip safety) of various values.
-- To ensure we're testing what we think we're testing, start with
-- float values specified by bit patterns (as a useful side effect,
-- this means we'll fail on non-IEEE platforms).

create type xfloat4;
create function xfloat4in(cstring) returns xfloat4 immutable strict
  language internal as 'int4in';
create function xfloat4out(xfloat4) returns cstring immutable strict
  language internal as 'int4out';
create type xfloat4 (input = xfloat4in, output = xfloat4out, like = float4);
create cast (xfloat4 as float4) without function;
create cast (float4 as xfloat4) without function;
create cast (xfloat4 as integer) without function;
create cast (integer as xfloat4) without function;

-- float4: seeeeeee emmmmmmm mmmmmmmm mmmmmmmm

-- we don't care to assume the platform's strtod() handles subnormals
-- correctly; those are "use at your own risk". However we do test
-- subnormal outputs, since those are under our control.


-- clean up, lest opr_sanity complain
drop type xfloat4 cascade;
