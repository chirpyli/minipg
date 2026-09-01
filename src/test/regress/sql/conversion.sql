--
-- Test built-in conversion functions.
-- minipg: only UTF8, LATIN1 (ISO-8859-1) and SQL_ASCII are retained, so this
-- test exercises just those conversions.
--

-- Helper function to test a conversion. Uses the test_enc_conversion function
-- that was created in the create_function_0 test.
-- minipg: PL/pgSQL removed. Rewrite test_conv as a SQL function. The original
-- captured the conversion error message via plpgsql exception handling; here we
-- always use noError = true so the function does not throw. The 'error' column is
-- therefore NULL for failing inputs (the 'errorat' position is still reported).
create or replace function test_conv(
  input IN bytea,
  src_encoding IN text,
  dst_encoding IN text,

  result OUT bytea,
  errorat OUT bytea,
  error OUT text)
language sql as
$$
  SELECT result,
         substr(input, validlen + 1),
         NULL::text
  FROM (SELECT * FROM test_enc_conversion(input, src_encoding, dst_encoding, true)) t(validlen, result);
$$;


--
-- UTF-8
--
CREATE TABLE utf8_inputs (inbytes bytea, description text);
insert into utf8_inputs  values
  ('\x666f6f',          'valid, pure ASCII'),
  ('\xc3a4c3b6',       'valid, latin chars (äö)'),
  ('\x666f6fe8b1a1',   'valid, Chinese char (not representable in LATIN1)'),
  ('\x66e8b1ff6f6f',   'invalid byte sequence'),
  ('\x66006f',         'invalid, NUL byte'),
  ('\x666f6fe8b1',     'incomplete character at end');

-- Test UTF-8 verification
select description, (test_conv(inbytes, 'utf8', 'utf8')).* from utf8_inputs;
-- Test conversions from UTF-8 to LATIN1
select description, inbytes, (test_conv(inbytes, 'utf8', 'latin1')).* from utf8_inputs;


--
-- LATIN1 (ISO-8859-1)
--
CREATE TABLE latin1_inputs (inbytes bytea, description text);
insert into latin1_inputs  values
  ('\x666f6f',          'valid, pure ASCII'),
  ('\xe9',             'valid, accented char (é)'),
  ('\xe9a9',           'valid, two accented chars (é©)'),
  ('\x00',             'invalid, NUL byte'),
  ('\xe900',           'invalid, NUL byte');

-- Test LATIN1 verification
select description, inbytes, (test_conv(inbytes, 'latin1', 'latin1')).* from latin1_inputs;
-- Test conversions from LATIN1 to UTF-8
select description, inbytes, (test_conv(inbytes, 'latin1', 'utf8')).* from latin1_inputs;
