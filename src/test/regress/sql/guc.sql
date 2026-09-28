-- pg_regress should ensure that this default value applies; however
-- we can't rely on any specific default value of vacuum_cost_delay
SHOW extra_float_digits;

-- SET to some nondefault value
SET vacuum_cost_delay TO 40;
SET extra_float_digits = 0;
SHOW vacuum_cost_delay;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;

-- SET LOCAL has no effect outside of a transaction
SET LOCAL vacuum_cost_delay TO 50;
SHOW vacuum_cost_delay;
SET LOCAL extra_float_digits = 3;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;

-- SET LOCAL within a transaction that commits
BEGIN;
SET LOCAL vacuum_cost_delay TO 50;
SHOW vacuum_cost_delay;
SET LOCAL extra_float_digits = 3;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
COMMIT;
SHOW vacuum_cost_delay;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;

-- SET should be reverted after ROLLBACK
BEGIN;
SET vacuum_cost_delay TO 60;
SHOW vacuum_cost_delay;
SET extra_float_digits = 0;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
ROLLBACK;
SHOW vacuum_cost_delay;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;

-- Some tests with subtransactions
BEGIN;
SET vacuum_cost_delay TO 70;
SET extra_float_digits = 0;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
SAVEPOINT first_sp;
SET vacuum_cost_delay TO 80.1;
SHOW vacuum_cost_delay;
SET extra_float_digits = 0;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
ROLLBACK TO first_sp;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
SAVEPOINT second_sp;
SET vacuum_cost_delay TO '900us';
SET extra_float_digits = 0;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
SAVEPOINT third_sp;
SET vacuum_cost_delay TO 100;
SHOW vacuum_cost_delay;
SET extra_float_digits = 0;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
ROLLBACK TO third_sp;
SHOW vacuum_cost_delay;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
ROLLBACK TO second_sp;
SHOW vacuum_cost_delay;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
ROLLBACK;
SHOW vacuum_cost_delay;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;

-- SET LOCAL with Savepoints
BEGIN;
SHOW vacuum_cost_delay;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
SAVEPOINT sp;
SET LOCAL vacuum_cost_delay TO 30;
SHOW vacuum_cost_delay;
SET LOCAL extra_float_digits = 3;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
ROLLBACK TO sp;
SHOW vacuum_cost_delay;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
ROLLBACK;
SHOW vacuum_cost_delay;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;

-- SET LOCAL persists through RELEASE (which was not true in 8.0-8.2)
BEGIN;
SHOW vacuum_cost_delay;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
SAVEPOINT sp;
SET LOCAL vacuum_cost_delay TO 30;
SHOW vacuum_cost_delay;
SET LOCAL extra_float_digits = 3;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
RELEASE SAVEPOINT sp;
SHOW vacuum_cost_delay;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
ROLLBACK;
SHOW vacuum_cost_delay;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;

-- SET followed by SET LOCAL
BEGIN;
SET vacuum_cost_delay TO 40;
SET LOCAL vacuum_cost_delay TO 50;
SHOW vacuum_cost_delay;
SET extra_float_digits = 0;
SET LOCAL extra_float_digits = 3;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
COMMIT;
SHOW vacuum_cost_delay;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;

--
-- Test RESET.  We use extra_float_digits because its built-in default (1)
-- doesn't depend on the installation's configuration.
--
SET extra_float_digits = 0;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;
RESET extra_float_digits;
SHOW extra_float_digits;
SELECT 1.2345678901234567::float8;

-- Test some simple error cases
SET seq_page_cost TO 'NaN';
SET vacuum_cost_delay TO '10s';
SET no_such_variable TO 42;

-- Test "custom" GUCs created on the fly (which aren't really an
-- intended feature, but many people use them).
SHOW custom.my_guc;  -- error, not known yet
SET custom.my_guc = 42;
SHOW custom.my_guc;
RESET custom.my_guc;  -- this makes it go to empty, not become unknown again
SHOW custom.my_guc;
SET custom.my.qualified.guc = 'foo';
SHOW custom.my.qualified.guc;
SET custom."bad-guc" = 42;  -- disallowed because -c cannot set this name
SHOW custom."bad-guc";
SET special."weird name" = 'foo';  -- could be allowed, but we choose not to
SHOW special."weird name";

--
-- search_path should react to changes in pg_namespace
--

set search_path = foo, public, not_there_initially;
select current_schemas(false);
create schema not_there_initially;
select current_schemas(false);
drop schema not_there_initially;
select current_schemas(false);
reset search_path;

-- minipg: 函数级 GUC 设置（CREATE FUNCTION ... SET）依赖已裁剪的
-- CREATE FUNCTION，plpgsql 相关检查亦已移除，整节删除。

-- check current_setting()'s behavior with invalid setting name

select current_setting('nosuch.setting');  -- FAIL
select current_setting('nosuch.setting', false);  -- FAIL
select current_setting('nosuch.setting', true) is null;

-- after this, all three cases should yield 'nada'
set nosuch.setting = 'nada';

select current_setting('nosuch.setting');
select current_setting('nosuch.setting', false);
select current_setting('nosuch.setting', true);