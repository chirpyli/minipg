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


CREATE OR REPLACE FUNCTION lpad(text, integer)
 RETURNS text
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN lpad($1, $2, ' ');

CREATE OR REPLACE FUNCTION rpad(text, integer)
 RETURNS text
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN rpad($1, $2, ' ');

CREATE OR REPLACE FUNCTION bit_length(bit)
 RETURNS integer
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN length($1);

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

CREATE OR REPLACE FUNCTION log(numeric)
 RETURNS numeric
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN log(10, $1);

CREATE OR REPLACE FUNCTION log10(numeric)
 RETURNS numeric
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN log(10, $1);

CREATE OR REPLACE FUNCTION round(numeric)
 RETURNS numeric
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN round($1, 0);

CREATE OR REPLACE FUNCTION trunc(numeric)
 RETURNS numeric
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN trunc($1, 0);

CREATE OR REPLACE FUNCTION numeric_pl_pg_lsn(numeric, pg_lsn)
 RETURNS pg_lsn
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

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

CREATE OR REPLACE FUNCTION pg_sleep_for(interval)
 RETURNS void
 LANGUAGE sql
 PARALLEL SAFE STRICT COST 1
RETURN pg_sleep(extract(epoch from clock_timestamp() + $1) -
                extract(epoch from clock_timestamp()));

CREATE OR REPLACE FUNCTION pg_sleep_until(timestamptz)
 RETURNS void
 LANGUAGE sql
 PARALLEL SAFE STRICT COST 1
RETURN pg_sleep(extract(epoch from $1) -
                extract(epoch from clock_timestamp()));

CREATE OR REPLACE FUNCTION pg_relation_size(regclass)
 RETURNS bigint
 LANGUAGE sql
 PARALLEL SAFE STRICT COST 1
RETURN pg_relation_size($1, 'main');

CREATE OR REPLACE FUNCTION obj_description(oid, name)
 RETURNS text
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT
BEGIN ATOMIC
select description from pg_description
  where objoid = $1 and
    classoid = (select oid from pg_class where relname = $2 and
                relnamespace = 'pg_catalog'::regnamespace) and
    objsubid = 0;
END;

CREATE OR REPLACE FUNCTION shobj_description(oid, name)
 RETURNS text
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT
BEGIN ATOMIC
select description from pg_shdescription
  where objoid = $1 and
    classoid = (select oid from pg_class where relname = $2 and
                relnamespace = 'pg_catalog'::regnamespace);
END;

CREATE OR REPLACE FUNCTION obj_description(oid)
 RETURNS text
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT
BEGIN ATOMIC
select description from pg_description where objoid = $1 and objsubid = 0;
END;

CREATE OR REPLACE FUNCTION col_description(oid, integer)
 RETURNS text
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT
BEGIN ATOMIC
select description from pg_description
  where objoid = $1 and classoid = 'pg_class'::regclass and objsubid = $2;
END;


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


-- default normalization form is NFC, per SQL standard
CREATE OR REPLACE FUNCTION
  "normalize"(text, text DEFAULT 'NFC')
RETURNS text
LANGUAGE internal
STRICT IMMUTABLE PARALLEL SAFE
AS 'unicode_normalize_func';

CREATE OR REPLACE FUNCTION
  is_normalized(text, text DEFAULT 'NFC')
RETURNS boolean
LANGUAGE internal
STRICT IMMUTABLE PARALLEL SAFE
AS 'unicode_is_normalized';

--
-- The default permissions for functions mean that anyone can execute them.
-- A number of functions shouldn't be executable by just anyone, but rather
-- than use explicit 'superuser()' checks in those functions, we use the GRANT
-- system to REVOKE access to those functions at initdb time.  Administrators
-- can later change who can access these functions, or leave them as only
-- available to superuser / cluster owner, if they choose.
--

















































--
-- We also set up some things as accessible to standard roles.
--








