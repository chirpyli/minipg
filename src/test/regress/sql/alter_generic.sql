--
-- Test for ALTER some_object {RENAME TO, OWNER TO, SET SCHEMA}
--

-- Clean up in case a prior regression run failed
SET client_min_messages TO 'warning';


RESET client_min_messages;


CREATE SCHEMA alt_nsp1;
CREATE SCHEMA alt_nsp2;


SET search_path = alt_nsp1, public;

--
-- Aggregate
--
CREATE AGGREGATE alt_agg1 (
  sfunc1 = int4pl, basetype = int4, stype1 = int4, initcond = 0
);
CREATE AGGREGATE alt_agg2 (
  sfunc1 = int4mi, basetype = int4, stype1 = int4, initcond = 0
);
ALTER AGGREGATE alt_agg1(int) RENAME TO alt_agg2;   -- failed (name conflict)
ALTER AGGREGATE alt_agg1(int) RENAME TO alt_agg3;   -- OK
ALTER AGGREGATE alt_agg2(int) SET SCHEMA alt_nsp2;  -- OK

CREATE AGGREGATE alt_agg1 (
  sfunc1 = int4pl, basetype = int4, stype1 = int4, initcond = 100
);
CREATE AGGREGATE alt_agg2 (
  sfunc1 = int4mi, basetype = int4, stype1 = int4, initcond = -100
);

ALTER AGGREGATE alt_agg3(int) RENAME TO alt_agg4;   -- failed (not owner)
ALTER AGGREGATE alt_agg1(int) RENAME TO alt_agg4;   -- OK
ALTER AGGREGATE alt_agg3(int) SET SCHEMA alt_nsp2;  -- failed (not owner)
ALTER AGGREGATE alt_agg2(int) SET SCHEMA alt_nsp2;  -- failed (name conflict)


SELECT n.nspname, proname, prorettype::regtype, prokind, 'postgres'::name AS rolname
  FROM pg_proc p, pg_namespace n
  WHERE p.pronamespace = n.oid
    AND n.nspname IN ('alt_nsp1', 'alt_nsp2')
  ORDER BY nspname, proname;

--
-- We would test collations here, but it's not possible because the error
-- messages tend to be nonportable.
--

--
-- Conversion
--
CREATE CONVERSION alt_conv1 FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;
CREATE CONVERSION alt_conv2 FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;

ALTER CONVERSION alt_conv1 RENAME TO alt_conv2;  -- failed (name conflict)
ALTER CONVERSION alt_conv1 RENAME TO alt_conv3;  -- OK
ALTER CONVERSION alt_conv2 SET SCHEMA alt_nsp2;  -- OK

CREATE CONVERSION alt_conv1 FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;
CREATE CONVERSION alt_conv2 FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;

ALTER CONVERSION alt_conv3 RENAME TO alt_conv4;  -- failed (not owner)
ALTER CONVERSION alt_conv1 RENAME TO alt_conv4;  -- OK
ALTER CONVERSION alt_conv3 SET SCHEMA alt_nsp2;  -- failed (not owner)
ALTER CONVERSION alt_conv2 SET SCHEMA alt_nsp2;  -- failed (name conflict)


SELECT n.nspname, c.conname, 'postgres'::name AS rolname
  FROM pg_conversion c, pg_namespace n
  WHERE c.connamespace = n.oid
    AND n.nspname IN ('alt_nsp1', 'alt_nsp2')
  ORDER BY nspname, conname;


---
--- Cleanup resources
---

DROP SCHEMA alt_nsp1 CASCADE;
DROP SCHEMA alt_nsp2 CASCADE;

