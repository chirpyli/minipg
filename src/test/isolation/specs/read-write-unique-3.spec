# Read-write-unique test.
# From bug report 9301.

setup
{
  CREATE TABLE test (
    key   integer UNIQUE,
    val   text
  );
}

teardown
{
  DROP TABLE test;
}

session s1
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step rw1 { INSERT INTO test (key, val) SELECT 1, '1' WHERE NOT EXISTS (SELECT key FROM test WHERE key = 1); }
step c1 { COMMIT; }

session s2
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step rw2 { INSERT INTO test (key, val) SELECT 1, '2' WHERE NOT EXISTS (SELECT key FROM test WHERE key = 1); }
step c2 { COMMIT; }

permutation rw1 rw2 c1 c2
