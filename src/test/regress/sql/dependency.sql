--
-- DEPENDENCIES
--


CREATE TABLE deptest (f1 serial primary key, f2 text);


-- can't drop neither because they have privileges somewhere

-- if we revoke the privileges we can drop the group

-- can't drop the user if we revoke the privileges partially

-- now we are OK to drop him

-- we are OK too if we drop the privileges all at once

-- can't drop the owner of an object
-- the error message detail here would include a pg_toast_nnn name that
-- is not constant, so suppress it
\set VERBOSITY terse
\set VERBOSITY default

-- if we drop the object, we can drop the user too
DROP TABLE deptest;

-- Test DROP OWNED
-- permission denied
DROP OWNED BY regress_dep_user1;
DROP OWNED BY regress_dep_user0, regress_dep_user2;
-- this one is allowed
DROP OWNED BY regress_dep_user0;

CREATE TABLE deptest1 (f1 int unique);

CREATE TABLE deptest (a serial primary key, b text);
\z deptest1

DROP OWNED BY regress_dep_user1;
-- all grants revoked
\z deptest1
-- table was dropped
\d deptest

-- Test REASSIGN OWNED

CREATE SCHEMA deptest;
CREATE TABLE deptest (a serial primary key, b text);
CREATE FUNCTION deptest_func() RETURNS void LANGUAGE plpgsql
  AS $$ BEGIN END; $$;
CREATE TYPE deptest_enum AS ENUM ('red');
CREATE TYPE deptest_range AS RANGE (SUBTYPE = int4);

CREATE TABLE deptest2 (f1 int);
-- make a serial column the hard way
CREATE SEQUENCE ss1;
ALTER TABLE deptest2 ALTER f1 SET DEFAULT nextval('ss1');
ALTER SEQUENCE ss1 OWNED BY deptest2.f1;

-- When reassigning ownership of a composite type, its pg_class entry
-- should match
CREATE TYPE deptest_t AS (a int);
SELECT typowner = relowner
FROM pg_type JOIN pg_class c ON typrelid = c.oid WHERE typname = 'deptest_t';

\dt deptest

SELECT typowner = relowner
FROM pg_type JOIN pg_class c ON typrelid = c.oid WHERE typname = 'deptest_t';

-- doesn't work: grant still exists
DROP OWNED BY regress_dep_user1;

DROP OWNED BY regress_dep_user2, regress_dep_user0;
