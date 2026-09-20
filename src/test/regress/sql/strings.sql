--
-- STRINGS
-- Test various data entry syntaxes.
--

-- SQL string continuation syntax
-- E021-03 character string literals
SELECT 'first line'
' - next line'
	' - third line'
	AS "Three lines to one";

-- illegal string continuation syntax
SELECT 'first line'
' - next line' /* this comment is not allowed here */
' - third line'
	AS "Illegal comment within continuation";

-- check the Unicode escape cases in E-style literals
SELECT E'd\u0061t\U00000061' AS "data";
SELECT E'a\\b' AS "a\b";
SELECT E'wrong: \u061';
SELECT E'wrong: \U0061';
SELECT E'wrong: \udb99';
SELECT E'wrong: \udb99xy';
SELECT E'wrong: \udb99\\';
SELECT E'wrong: \udb99\u0061';
SELECT E'wrong: \U0000db99\U00000061';
SELECT E'wrong: \U002FFFFF';

-- bytea
SET bytea_output TO hex;
SELECT E'\\xDeAdBeEf'::bytea;
SELECT E'\\x De Ad Be Ef '::bytea;
SELECT E'\\xDeAdBeE'::bytea;
SELECT E'\\xDeAdBeEx'::bytea;
SELECT E'\\xDe00BeEf'::bytea;
SELECT E'DeAdBeEf'::bytea;
SELECT E'De\\000dBeEf'::bytea;
SELECT E'De\123dBeEf'::bytea;
SELECT E'De\\123dBeEf'::bytea;
SELECT E'De\\678dBeEf'::bytea;

SET bytea_output TO escape;
SELECT E'\\xDeAdBeEf'::bytea;
SELECT E'\\x De Ad Be Ef '::bytea;
SELECT E'\\xDe00BeEf'::bytea;
SELECT E'DeAdBeEf'::bytea;
SELECT E'De\\000dBeEf'::bytea;
SELECT E'De\\123dBeEf'::bytea;

--
-- test conversions between various string types
-- E021-10 implicit casting among the character data types
--

SELECT CAST(f1 AS text) AS "text(char)" FROM CHAR_TBL;

SELECT CAST(f1 AS text) AS "text(varchar)" FROM VARCHAR_TBL;

SELECT CAST(name 'namefield' AS text) AS "text(name)";

-- since this is an explicit cast, it should truncate w/o error:
SELECT CAST(f1 AS char(10)) AS "char(text)" FROM TEXT_TBL;
-- note: implicit-cast case is tested in char.sql

SELECT CAST(f1 AS char(20)) AS "char(text)" FROM TEXT_TBL;

SELECT CAST(f1 AS char(10)) AS "char(varchar)" FROM VARCHAR_TBL;

SELECT CAST(name 'namefield' AS char(10)) AS "char(name)";

SELECT CAST(f1 AS varchar) AS "varchar(text)" FROM TEXT_TBL;

SELECT CAST(f1 AS varchar) AS "varchar(char)" FROM CHAR_TBL;

SELECT CAST(name 'namefield' AS varchar) AS "varchar(name)";

--
-- test SQL string functions
-- E### and T### are feature reference numbers from SQL99
--

-- E021-09 trim function
SELECT btrim('  bunch o blanks  ') = 'bunch o blanks' AS "bunch o blanks";

SELECT ltrim('  bunch o blanks  ') = 'bunch o blanks  ' AS "bunch o blanks  ";

SELECT rtrim('  bunch o blanks  ') = '  bunch o blanks' AS "  bunch o blanks";

SELECT btrim('xxxxxsome Xsxxxxx', 'x') = 'some Xs' AS "some Xs";

-- E021-06 substring expression
SELECT substring('1234567890', 3) = '34567890' AS "34567890";

SELECT substring('1234567890', 4, 3) = '456' AS "456";

-- test overflow cases
SELECT substring('string', 2, 2147483646) AS "tring";
SELECT substring('string', -10, 2147483646) AS "string";
SELECT substring('string', -10, -2147483646) AS "error";


-- E021-11 position expression
SELECT position('1234567890', '4') = '4' AS "4";

SELECT position('1234567890', '5') = '5' AS "5";

-- T312 character overlay function
SELECT overlay('abcdef', '45', 4) AS "abc45f";

SELECT overlay('yabadoo', 'daba', 5) AS "yabadaba";

SELECT overlay('yabadoo', 'daba', 5, 0) AS "yabadabadoo";

SELECT overlay('babosa', 'ubb', 2, 4) AS "bubba";

--
-- test LIKE
-- Be sure to form every test as a LIKE/NOT LIKE pair.
--

-- simplest examples
-- E061-04 like predicate
SELECT 'hawkeye' LIKE 'h%' AS "true";
SELECT 'hawkeye' NOT LIKE 'h%' AS "false";

SELECT 'hawkeye' LIKE 'H%' AS "false";
SELECT 'hawkeye' NOT LIKE 'H%' AS "true";

SELECT 'hawkeye' LIKE 'indio%' AS "false";
SELECT 'hawkeye' NOT LIKE 'indio%' AS "true";

SELECT 'hawkeye' LIKE 'h%eye' AS "true";
SELECT 'hawkeye' NOT LIKE 'h%eye' AS "false";

SELECT 'indio' LIKE '_ndio' AS "true";
SELECT 'indio' NOT LIKE '_ndio' AS "false";

SELECT 'indio' LIKE 'in__o' AS "true";
SELECT 'indio' NOT LIKE 'in__o' AS "false";

SELECT 'indio' LIKE 'in_o' AS "false";
SELECT 'indio' NOT LIKE 'in_o' AS "true";

SELECT 'abc'::name LIKE '_b_' AS "true";
SELECT 'abc'::name NOT LIKE '_b_' AS "false";

SELECT 'abc'::bytea LIKE '_b_'::bytea AS "true";
SELECT 'abc'::bytea NOT LIKE '_b_'::bytea AS "false";



--
-- test ~~* (case-insensitive LIKE)
-- Be sure to form every test as an ILIKE/NOT ~~* pair.
--

SELECT 'hawkeye' ~~* 'h%' AS "true";
SELECT 'hawkeye' !~~* 'h%' AS "false";

SELECT 'hawkeye' ~~* 'H%' AS "true";
SELECT 'hawkeye' !~~* 'H%' AS "false";

SELECT 'hawkeye' ~~* 'H%Eye' AS "true";
SELECT 'hawkeye' !~~* 'H%Eye' AS "false";

SELECT 'Hawkeye' ~~* 'h%' AS "true";
SELECT 'Hawkeye' !~~* 'h%' AS "false";

SELECT 'ABC'::name ~~* '_b_' AS "true";
SELECT 'ABC'::name !~~* '_b_' AS "false";

--
-- test %/_ combination cases, cf bugs #4821 and #5478
--

SELECT 'foo' LIKE '_%' as t, 'f' LIKE '_%' as t, '' LIKE '_%' as f;
SELECT 'foo' LIKE '%_' as t, 'f' LIKE '%_' as t, '' LIKE '%_' as f;

SELECT 'foo' LIKE '__%' as t, 'foo' LIKE '___%' as t, 'foo' LIKE '____%' as f;
SELECT 'foo' LIKE '%__' as t, 'foo' LIKE '%___' as t, 'foo' LIKE '%____' as f;

SELECT 'jack' LIKE '%____%' AS t;


--
-- basic tests of LIKE with indexes
--

CREATE TABLE texttest (a text PRIMARY KEY, b int);
SELECT * FROM texttest WHERE a LIKE '%1%';

CREATE TABLE byteatest (a bytea PRIMARY KEY, b int);
SELECT * FROM byteatest WHERE a LIKE '%1%';

DROP TABLE texttest, byteatest;


--
-- test implicit type conversion
--

-- E021-07 character concatenation
SELECT 'unknown' || ' and unknown' AS "Concat unknown types";

SELECT text 'text' || ' and unknown' AS "Concat text to unknown type";

SELECT char(20) 'characters' || ' and text' AS "Concat char to unknown type";

SELECT text 'text' || char(20) ' and characters' AS "Concat text to char";

SELECT text 'text' || varchar ' and varchar' AS "Concat text to varchar";

--
-- test substr with toasted text values
--
CREATE TABLE toasttest(f1 text);

insert into toasttest values(repeat('1234567890',10000));
insert into toasttest values(repeat('1234567890',10000));

--
-- Ensure that some values are uncompressed, to test the faster substring
-- operation used in that case
--
alter table toasttest alter column f1 set storage external;
insert into toasttest values(repeat('1234567890',10000));
insert into toasttest values(repeat('1234567890',10000));

-- If the starting position is zero or less, then return from the start of the string
-- adjusting the length to be consistent with the "negative start" per SQL.
SELECT substr(f1, -1, 5) from toasttest;

-- If the length is less than zero, an ERROR is thrown.
SELECT substr(f1, 5, -1) from toasttest;

-- If no third argument (length) is provided, the length to the end of the
-- string is assumed.
SELECT substr(f1, 99995) from toasttest;

-- If start plus length is > string length, the result is truncated to
-- string length
SELECT substr(f1, 99995, 10) from toasttest;

TRUNCATE TABLE toasttest;
INSERT INTO toasttest values (repeat('1234567890',300));
INSERT INTO toasttest values (repeat('1234567890',300));
INSERT INTO toasttest values (repeat('1234567890',300));
INSERT INTO toasttest values (repeat('1234567890',300));
-- expect >0 blocks
SELECT pg_relation_size(reltoastrelid) = 0 AS is_empty
  FROM pg_class where relname = 'toasttest';

TRUNCATE TABLE toasttest;
INSERT INTO toasttest values (repeat('1234567890',300));
INSERT INTO toasttest values (repeat('1234567890',300));
INSERT INTO toasttest values (repeat('1234567890',300));
INSERT INTO toasttest values (repeat('1234567890',300));
-- expect >0 blocks (reloptions removed, toast_tuple_target default used)
SELECT pg_relation_size(reltoastrelid) = 0 AS is_empty
  FROM pg_class where relname = 'toasttest';

DROP TABLE toasttest;

--
-- test substr with toasted bytea values
--
CREATE TABLE toasttest(f1 bytea);

insert into toasttest values(decode(repeat('1234567890',10000),'escape'));
insert into toasttest values(decode(repeat('1234567890',10000),'escape'));

--
-- Ensure that some values are uncompressed, to test the faster substring
-- operation used in that case
--
alter table toasttest alter column f1 set storage external;
insert into toasttest values(decode(repeat('1234567890',10000),'escape'));
insert into toasttest values(decode(repeat('1234567890',10000),'escape'));

-- If the starting position is zero or less, then return from the start of the string
-- adjusting the length to be consistent with the "negative start" per SQL.
SELECT substr(f1, -1, 5) from toasttest;

-- If the length is less than zero, an ERROR is thrown.
SELECT substr(f1, 5, -1) from toasttest;

-- If no third argument (length) is provided, the length to the end of the
-- string is assumed.
SELECT substr(f1, 99995) from toasttest;

-- If start plus length is > string length, the result is truncated to
-- string length
SELECT substr(f1, 99995, 10) from toasttest;

DROP TABLE toasttest;

-- test internally compressing datums

-- this tests compressing a datum to a very small size which exercises a
-- corner case in packed-varlena handling: even though small, the compressed
-- datum must be given a 4-byte header because there are no bits to indicate
-- compression in a 1-byte header

CREATE TABLE toasttest (c char(4096));
INSERT INTO toasttest VALUES('x');
SELECT length(c), c::text FROM toasttest;
SELECT c FROM toasttest;
DROP TABLE toasttest;

--
-- test length
--

SELECT length('abcdef') AS "length_6";

--
-- test strpos
--

SELECT strpos('abcdef', 'cd') AS "pos_3";

SELECT strpos('abcdef', 'xy') AS "pos_0";

SELECT strpos('abcdef', '') AS "pos_1";

SELECT strpos('', 'xy') AS "pos_0";

SELECT strpos('', '') AS "pos_1";

--
-- test replace
--
SELECT replace('abcdef', 'de', '45') AS "abc45f";

SELECT replace('yabadabadoo', 'ba', '123') AS "ya123da123doo";

SELECT replace('yabadoo', 'bad', '') AS "yaoo";

--
-- test split_part
--
select split_part('','@',1) AS "empty string";

select split_part('','@',-1) AS "empty string";

select split_part('joeuser@mydatabase','',1) AS "joeuser@mydatabase";

select split_part('joeuser@mydatabase','',2) AS "empty string";

select split_part('joeuser@mydatabase','',-1) AS "joeuser@mydatabase";

select split_part('joeuser@mydatabase','',-2) AS "empty string";

select split_part('joeuser@mydatabase','@',0) AS "an error";

select split_part('joeuser@mydatabase','@@',1) AS "joeuser@mydatabase";

select split_part('joeuser@mydatabase','@@',2) AS "empty string";

select split_part('joeuser@mydatabase','@',1) AS "joeuser";

select split_part('joeuser@mydatabase','@',2) AS "mydatabase";

select split_part('joeuser@mydatabase','@',3) AS "empty string";

select split_part('@joeuser@mydatabase@','@',2) AS "joeuser";

select split_part('joeuser@mydatabase','@',-1) AS "mydatabase";

select split_part('joeuser@mydatabase','@',-2) AS "joeuser";

select split_part('joeuser@mydatabase','@',-3) AS "empty string";

select split_part('@joeuser@mydatabase@','@',-2) AS "mydatabase";

--
-- test to_hex
--
select to_hex(256*256*256 - 1) AS "ffffff";

select to_hex(256::bigint*256::bigint*256::bigint*256::bigint - 1) AS "ffffffff";

SET bytea_output TO hex;

--
-- encode/decode
--
SELECT encode('\x1234567890abcdef00', 'hex');
SELECT decode('1234567890abcdef00', 'hex');
SELECT encode(('\x' || repeat('1234567890abcdef0001', 7))::bytea, 'base64');
SELECT decode(encode(('\x' || repeat('1234567890abcdef0001', 7))::bytea,
                     'base64'), 'base64');
SELECT encode('\x1234567890abcdef00', 'escape');
SELECT decode(encode('\x1234567890abcdef00', 'escape'), 'escape');

--
-- get_bit/set_bit etc
--
SELECT get_bit('\x1234567890abcdef00'::bytea, 43);
SELECT get_bit('\x1234567890abcdef00'::bytea, 99);  -- error
SELECT set_bit('\x1234567890abcdef00'::bytea, 43, 0);
SELECT set_bit('\x1234567890abcdef00'::bytea, 99, 0);  -- error
SELECT get_byte('\x1234567890abcdef00'::bytea, 3);
SELECT get_byte('\x1234567890abcdef00'::bytea, 99);  -- error
SELECT set_byte('\x1234567890abcdef00'::bytea, 7, 11);
SELECT set_byte('\x1234567890abcdef00'::bytea, 99, 11);  -- error

--
-- test behavior of escape_string_warning and standard_conforming_strings options
--
set escape_string_warning = off;
set standard_conforming_strings = off;

show escape_string_warning;
show standard_conforming_strings;

set escape_string_warning = on;
set standard_conforming_strings = on;

show escape_string_warning;
show standard_conforming_strings;

select 'a\bcd' as f1, 'a\b''cd' as f2, 'a\b''''cd' as f3, 'abcd\'   as f4, 'ab\''cd' as f5, '\\' as f6;

set standard_conforming_strings = off;

select 'a\\bcd' as f1, 'a\\b\'cd' as f2, 'a\\b\'''cd' as f3, 'abcd\\'   as f4, 'ab\\\'cd' as f5, '\\\\' as f6;

set escape_string_warning = off;
set standard_conforming_strings = on;

select 'a\bcd' as f1, 'a\b''cd' as f2, 'a\b''''cd' as f3, 'abcd\'   as f4, 'ab\''cd' as f5, '\\' as f6;

set standard_conforming_strings = off;

select 'a\\bcd' as f1, 'a\\b\'cd' as f2, 'a\\b\'''cd' as f3, 'abcd\\'   as f4, 'ab\\\'cd' as f5, '\\\\' as f6;

reset standard_conforming_strings;


--
-- Additional string functions
--
SET bytea_output TO escape;

SELECT initcap('hi THOMAS');

SELECT ltrim('zzzytrim', 'xyz');

SELECT translate('', '14', 'ax');
SELECT translate('12345', '14', 'ax');
SELECT translate('12345', '134', 'a');

SELECT ascii('x');
SELECT ascii('');

SELECT chr(65);
SELECT chr(0);

SELECT repeat('Pg', 4);
SELECT repeat('Pg', -4);

SELECT substring('1234567890'::bytea, 3) "34567890";
SELECT substring('1234567890'::bytea, 4, 3) AS "456";
SELECT substring('string'::bytea, 2, 2147483646) AS "tring";
SELECT substring('string'::bytea, -10, 2147483646) AS "string";
SELECT substring('string'::bytea, -10, -2147483646) AS "error";

SELECT btrim(E'\\000Tom\\000'::bytea, E'\\000'::bytea);
SELECT ltrim(E'\\000Tom\\000'::bytea, E'\\000'::bytea);
SELECT rtrim(E'\\000Tom\\000'::bytea, E'\\000'::bytea);
SELECT btrim(E'\\000trim\\000'::bytea, E'\\000'::bytea);
SELECT btrim(''::bytea, E'\\000'::bytea);
SELECT btrim(E'\\000trim\\000'::bytea, ''::bytea);
SELECT encode(overlay(E'Th\\000omas'::bytea, E'Th\\001omas'::bytea, 2),'escape');
SELECT encode(overlay(E'Th\\000omas'::bytea, E'\\002\\003'::bytea, 8),'escape');
SELECT encode(overlay(E'Th\\000omas'::bytea, E'\\002\\003'::bytea, 5, 3),'escape');

SELECT bit_count('\x1234567890'::bytea);

SELECT unistr('\0064at\+0000610');
SELECT unistr('d\u0061t\U000000610');
SELECT unistr('a\\b');
-- errors:
SELECT unistr('wrong: \db99');
SELECT unistr('wrong: \db99\0061');
SELECT unistr('wrong: \+00db99\+000061');
SELECT unistr('wrong: \+2FFFFF');
SELECT unistr('wrong: \udb99\u0061');
SELECT unistr('wrong: \U0000db99\U00000061');
SELECT unistr('wrong: \U002FFFFF');
SELECT unistr('wrong: \xyz');
