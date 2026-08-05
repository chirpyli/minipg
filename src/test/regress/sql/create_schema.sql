--
-- CREATE_SCHEMA
--

-- Schema creation with elements.


-- Cases where schema creation fails as objects are qualified with a schema
-- that does not match with what's expected.
-- This checks all the object types that include schema qualifications.
CREATE SCHEMA
  CREATE SEQUENCE schema_not_existing.seq;
CREATE SCHEMA
  CREATE TABLE schema_not_existing.tab (id int);
CREATE SCHEMA
  CREATE VIEW schema_not_existing.view AS SELECT 1;
CREATE SCHEMA
  CREATE INDEX ON schema_not_existing.tab (id);
CREATE SCHEMA
  CREATE TRIGGER schema_trig BEFORE INSERT ON schema_not_existing.tab
  EXECUTE FUNCTION schema_trig.no_func();
-- Again, with a role specification and no schema names.
CREATE SCHEMA
  CREATE SEQUENCE schema_not_existing.seq;
CREATE SCHEMA
  CREATE TABLE schema_not_existing.tab (id int);
CREATE SCHEMA
  CREATE VIEW schema_not_existing.view AS SELECT 1;
CREATE SCHEMA
  CREATE INDEX ON schema_not_existing.tab (id);
CREATE SCHEMA
  CREATE TRIGGER schema_trig BEFORE INSERT ON schema_not_existing.tab
  EXECUTE FUNCTION schema_trig.no_func();
-- Again, with a schema name and a role specification.
CREATE SCHEMA regress_schema_1
  CREATE SEQUENCE schema_not_existing.seq;
CREATE SCHEMA regress_schema_1
  CREATE TABLE schema_not_existing.tab (id int);
CREATE SCHEMA regress_schema_1
  CREATE VIEW schema_not_existing.view AS SELECT 1;
CREATE SCHEMA regress_schema_1
  CREATE INDEX ON schema_not_existing.tab (id);
CREATE SCHEMA regress_schema_1
  CREATE TRIGGER schema_trig BEFORE INSERT ON schema_not_existing.tab
  EXECUTE FUNCTION schema_trig.no_func();

-- Cases where the schema creation succeeds.
-- The schema created matches the role name.
CREATE SCHEMA
  CREATE TABLE regress_create_schema_role.tab (id int);
\d regress_create_schema_role.tab
DROP SCHEMA regress_create_schema_role CASCADE;
-- Again, with a different role specification and no schema names.
CREATE SCHEMA
  CREATE TABLE regress_create_schema_role.tab (id int);
\d regress_create_schema_role.tab
DROP SCHEMA regress_create_schema_role CASCADE;
-- Again, with a schema name and a role specification.
CREATE SCHEMA regress_schema_1
  CREATE TABLE regress_schema_1.tab (id int);
\d regress_schema_1.tab
DROP SCHEMA regress_schema_1 CASCADE;

-- Clean up
