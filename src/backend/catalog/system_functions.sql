/*
 * PostgreSQL System Functions
 *
 * Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 * src/backend/catalog/system_functions.sql
 *
 * This file redefines certain built-in functions that it's impractical
 * to fully define in pg_proc.dat.  In most cases that's because they use
 * SQL-standard function bodies and/or default expressions.  The node
 * tree representations of those are too unreadable, platform-dependent,
 * and changeable to want to deal with them manually.  Hence, we put stub
 * definitions of such functions into pg_proc.dat and then replace them
 * here.  The stub definitions would be unnecessary were it not that we'd
 * like these functions to have stable OIDs, the same as other built-in
 * functions.
 *
 * This file also takes care of adjusting privileges for those functions
 * that should not have the default public-EXECUTE privileges.  (However,
 * a small number of functions that exist mainly to underlie system views
 * are dealt with in system_views.sql, instead.)
 *
 * Note: this file is read in single-user -j mode, which means that the
 * command terminator is semicolon-newline-newline; whenever the backend
 * sees that, it stops and executes what it's got.  If you write a lot of
 * statements without empty lines between, they'll all get quoted to you
 * in any error message about one of them, so don't do that.  Also, you
 * cannot write a semicolon immediately followed by an empty line in a
 * string literal (including a function body!) or a multiline comment.
 */


CREATE OR REPLACE FUNCTION bit_length(bytea)
 RETURNS integer
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN octet_length($1) * 8;

CREATE OR REPLACE FUNCTION bit_length(text)
RETURNS integer
LANGUAGE sql
IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN octet_length($1) * 8;

CREATE OR REPLACE FUNCTION age(timestamptz)
 RETURNS interval
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT COST 1
RETURN age(cast(current_date as timestamptz), $1);

CREATE OR REPLACE FUNCTION age(timestamp)
 RETURNS interval
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT COST 1
RETURN age(cast(current_date as timestamp), $1);

CREATE OR REPLACE FUNCTION date_part(text, date)
 RETURNS double precision
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN date_part($1, cast($2 as timestamp));

CREATE OR REPLACE FUNCTION timestamptz(date, time)
 RETURNS timestamptz
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT COST 1
RETURN cast(($1 + $2) as timestamptz);

CREATE OR REPLACE FUNCTION timedate_pl(time, date)
 RETURNS timestamp
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION interval_pl_time(interval, time)
 RETURNS time
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION interval_pl_date(interval, date)
 RETURNS timestamp
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION interval_pl_timestamp(interval, timestamp)
 RETURNS timestamp
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION interval_pl_timestamptz(interval, timestamptz)
 RETURNS timestamptz
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION integer_pl_date(integer, date)
 RETURNS date
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION "overlaps"(timestamptz, timestamptz,
  timestamptz, interval)
 RETURNS boolean
 LANGUAGE sql
 STABLE PARALLEL SAFE COST 1
RETURN ($1, $2) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(timestamptz, interval,
  timestamptz, interval)
 RETURNS boolean
 LANGUAGE sql
 STABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(timestamptz, interval,
  timestamptz, timestamptz)
 RETURNS boolean
 LANGUAGE sql
 STABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, $4);

CREATE OR REPLACE FUNCTION "overlaps"(timestamp, timestamp,
  timestamp, interval)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, $2) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(timestamp, interval,
  timestamp, timestamp)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, $4);

CREATE OR REPLACE FUNCTION "overlaps"(timestamp, interval,
  timestamp, interval)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(time, interval,
  time, interval)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(time, time,
  time, interval)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, $2) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(time, interval,
  time, time)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, $4);


CREATE OR REPLACE FUNCTION pg_relation_size(regclass)
 RETURNS bigint
 LANGUAGE sql
 PARALLEL SAFE STRICT COST 1
RETURN pg_relation_size($1, 'main');


CREATE OR REPLACE FUNCTION
  pg_start_backup(label text, fast boolean DEFAULT false, exclusive boolean DEFAULT true)
  RETURNS pg_lsn STRICT VOLATILE LANGUAGE internal AS 'pg_start_backup'
  PARALLEL RESTRICTED;

CREATE OR REPLACE FUNCTION pg_stop_backup (
        exclusive boolean, wait_for_archive boolean DEFAULT true,
        OUT lsn pg_lsn, OUT labelfile text, OUT spcmapfile text)
  RETURNS SETOF record STRICT VOLATILE LANGUAGE internal as 'pg_stop_backup_v2'
  PARALLEL RESTRICTED;

CREATE OR REPLACE FUNCTION
  pg_promote(wait boolean DEFAULT true, wait_seconds integer DEFAULT 60)
  RETURNS boolean STRICT VOLATILE LANGUAGE INTERNAL AS 'pg_promote'
  PARALLEL SAFE;

CREATE OR REPLACE FUNCTION
  pg_terminate_backend(pid integer, timeout int8 DEFAULT 0)
  RETURNS boolean STRICT VOLATILE LANGUAGE INTERNAL AS 'pg_terminate_backend'
  PARALLEL SAFE;



CREATE OR REPLACE FUNCTION
  make_interval(years int4 DEFAULT 0, months int4 DEFAULT 0, weeks int4 DEFAULT 0,
                days int4 DEFAULT 0, hours int4 DEFAULT 0, mins int4 DEFAULT 0,
                secs double precision DEFAULT 0.0)
RETURNS interval
LANGUAGE INTERNAL
STRICT IMMUTABLE PARALLEL SAFE
AS 'make_interval';















































--
-- We also set up some things as accessible to standard roles.
--










-- minipg: COMMENT ON 功能已裁剪，pg_description/pg_shdescription 已删除。
-- 以下描述函数保留签名以兼容 psql \d+ 等客户端查询，但一律返回 NULL。
CREATE OR REPLACE FUNCTION obj_description(oid, name)
 RETURNS text
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT COST 1
RETURN NULL::text;

CREATE OR REPLACE FUNCTION obj_description(oid)
 RETURNS text
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT COST 1
RETURN NULL::text;

CREATE OR REPLACE FUNCTION col_description(oid, integer)
 RETURNS text
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT COST 1
RETURN NULL::text;

CREATE OR REPLACE FUNCTION shobj_description(oid, name)
 RETURNS text
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT COST 1
RETURN NULL::text;
