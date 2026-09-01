--
-- Test hash functions
--
-- When the salt is 0, the extended hash function should produce a result
-- whose low 32 bits match the standard hash function.  When the salt is
-- not 0, we should get a different result.
--

SELECT v as value, hashint2(v)::int8 as standard,
       hashint2extended(v, 0)::int8 as extended0,
       hashint2extended(v, 1)::int8 as extended1
FROM   (VALUES (0::int2), (1::int2), (17::int2), (42::int2)) x(v)
WHERE  hashint2(v)::int8 != hashint2extended(v, 0)::int8
       OR hashint2(v)::int8 = hashint2extended(v, 1)::int8;

SELECT v as value, hashint4(v)::int8 as standard,
       hashint4extended(v, 0)::int8 as extended0,
       hashint4extended(v, 1)::int8 as extended1
FROM   (VALUES (0), (1), (17), (42), (550273), (207112489)) x(v)
WHERE  hashint4(v)::int8 != hashint4extended(v, 0)::int8
       OR hashint4(v)::int8 = hashint4extended(v, 1)::int8;

SELECT v as value, hashint8(v)::int8 as standard,
       hashint8extended(v, 0)::int8 as extended0,
       hashint8extended(v, 1)::int8 as extended1
FROM   (VALUES (0), (1), (17), (42), (550273), (207112489)) x(v)
WHERE  hashint8(v)::int8 != hashint8extended(v, 0)::int8
       OR hashint8(v)::int8 = hashint8extended(v, 1)::int8;

SELECT v as value, hashfloat4(v)::int8 as standard,
       hashfloat4extended(v, 0)::int8 as extended0,
       hashfloat4extended(v, 1)::int8 as extended1
FROM   (VALUES (0), (1), (17), (42), (550273), (207112489)) x(v)
WHERE  hashfloat4(v)::int8 != hashfloat4extended(v, 0)::int8
       OR hashfloat4(v)::int8 = hashfloat4extended(v, 1)::int8;

SELECT v as value, hashfloat8(v)::int8 as standard,
       hashfloat8extended(v, 0)::int8 as extended0,
       hashfloat8extended(v, 1)::int8 as extended1
FROM   (VALUES (0), (1), (17), (42), (550273), (207112489)) x(v)
WHERE  hashfloat8(v)::int8 != hashfloat8extended(v, 0)::int8
       OR hashfloat8(v)::int8 = hashfloat8extended(v, 1)::int8;

SELECT v as value, hashoid(v)::int8 as standard,
       hashoidextended(v, 0)::int8 as extended0,
       hashoidextended(v, 1)::int8 as extended1
FROM   (VALUES (0), (1), (17), (42), (550273), (207112489)) x(v)
WHERE  hashoid(v)::int8 != hashoidextended(v, 0)::int8
       OR hashoid(v)::int8 = hashoidextended(v, 1)::int8;

SELECT v as value, hashchar(v)::int8 as standard,
       hashcharextended(v, 0)::int8 as extended0,
       hashcharextended(v, 1)::int8 as extended1
FROM   (VALUES (NULL::"char"), ('1'), ('x'), ('X'), ('p'), ('N')) x(v)
WHERE  hashchar(v)::int8 != hashcharextended(v, 0)::int8
       OR hashchar(v)::int8 = hashcharextended(v, 1)::int8;

SELECT v as value, hashname(v)::int8 as standard,
       hashnameextended(v, 0)::int8 as extended0,
       hashnameextended(v, 1)::int8 as extended1
FROM   (VALUES (NULL), ('PostgreSQL'), ('eIpUEtqmY89'), ('AXKEJBTK'),
        ('muop28x03'), ('yi3nm0d73')) x(v)
WHERE  hashname(v)::int8 != hashnameextended(v, 0)::int8
       OR hashname(v)::int8 = hashnameextended(v, 1)::int8;

SELECT v as value, hashtext(v)::int8 as standard,
       hashtextextended(v, 0)::int8 as extended0,
       hashtextextended(v, 1)::int8 as extended1
FROM   (VALUES (NULL), ('PostgreSQL'), ('eIpUEtqmY89'), ('AXKEJBTK'),
        ('muop28x03'), ('yi3nm0d73')) x(v)
WHERE  hashtext(v)::int8 != hashtextextended(v, 0)::int8
       OR hashtext(v)::int8 = hashtextextended(v, 1)::int8;

SELECT v as value, hashoidvector(v)::int8 as standard,
       hashoidvectorextended(v, 0)::int8 as extended0,
       hashoidvectorextended(v, 1)::int8 as extended1
FROM   (VALUES (NULL::oidvector), ('0 1 2 3 4'), ('17 18 19 20'),
        ('42 43 42 45'), ('550273 550273 570274'),
        ('207112489 207112499 21512 2155 372325 1363252')) x(v)
WHERE  hashoidvector(v)::int8 != hashoidvectorextended(v, 0)::int8
       OR hashoidvector(v)::int8 = hashoidvectorextended(v, 1)::int8;

SELECT v as value, hash_numeric(v)::int8 as standard,
       hash_numeric_extended(v, 0)::int8 as extended0,
       hash_numeric_extended(v, 1)::int8 as extended1
FROM   (VALUES (0), (1.149484958), (17.149484958), (42.149484958),
        (149484958.550273), (2071124898672)) x(v)
WHERE  hash_numeric(v)::int8 != hash_numeric_extended(v, 0)::int8
       OR hash_numeric(v)::int8 = hash_numeric_extended(v, 1)::int8;

SELECT v as value, hash_array(v)::int8 as standard,
       hash_array_extended(v, 0)::int8 as extended0,
       hash_array_extended(v, 1)::int8 as extended1
FROM   (VALUES ('{0}'::int4[]), ('{0,1,2,3,4}'), ('{17,18,19,20}'),
        ('{42,34,65,98}'), ('{550273,590027, 870273}'),
        ('{207112489, 807112489}')) x(v)
WHERE  hash_array(v)::int8 != hash_array_extended(v, 0)::int8
       OR hash_array(v)::int8 = hash_array_extended(v, 1)::int8;

-- array hashing with non-hashable element type
SELECT v as value, hash_array(v)::int8 as standard
FROM   (VALUES ('{0}'::int8[])) x(v);
SELECT v as value, hash_array_extended(v, 0)::int8 as extended0
FROM   (VALUES ('{0}'::int8[])) x(v);

SELECT v as value, hashbpchar(v)::int8 as standard,
       hashbpcharextended(v, 0)::int8 as extended0,
       hashbpcharextended(v, 1)::int8 as extended1
FROM   (VALUES (NULL), ('PostgreSQL'), ('eIpUEtqmY89'), ('AXKEJBTK'),
        ('muop28x03'), ('yi3nm0d73')) x(v)
WHERE  hashbpchar(v)::int8 != hashbpcharextended(v, 0)::int8
       OR hashbpchar(v)::int8 = hashbpcharextended(v, 1)::int8;

SELECT v as value, time_hash(v)::int8 as standard,
       time_hash_extended(v, 0)::int8 as extended0,
       time_hash_extended(v, 1)::int8 as extended1
FROM   (VALUES (NULL::time), ('11:09:59'), ('1:09:59'), ('11:59:59'),
        ('7:9:59'), ('5:15:59')) x(v)
WHERE  time_hash(v)::int8 != time_hash_extended(v, 0)::int8
       OR time_hash(v)::int8 = time_hash_extended(v, 1)::int8;

SELECT v as value, interval_hash(v)::int8 as standard,
       interval_hash_extended(v, 0)::int8 as extended0,
       interval_hash_extended(v, 1)::int8 as extended1
FROM   (VALUES (NULL::interval),
        ('5 month 7 day 46 minutes'), ('1 year 7 day 46 minutes'),
        ('1 year 7 month 20 day 46 minutes'), ('5 month'),
        ('17 year 11 month 7 day 9 hours 46 minutes 5 seconds')) x(v)
WHERE  interval_hash(v)::int8 != interval_hash_extended(v, 0)::int8
       OR interval_hash(v)::int8 = interval_hash_extended(v, 1)::int8;

SELECT v as value, timestamp_hash(v)::int8 as standard,
       timestamp_hash_extended(v, 0)::int8 as extended0,
       timestamp_hash_extended(v, 1)::int8 as extended1
FROM   (VALUES (NULL::timestamp), ('2017-08-22 00:09:59.518762'),
        ('2015-08-20 00:11:52.51762-08'),
        ('2017-05-22 00:11:52.62-01'),
        ('2013-08-22 00:11:52.62+01'), ('2013-08-22 11:59:59+04')) x(v)
WHERE  timestamp_hash(v)::int8 != timestamp_hash_extended(v, 0)::int8
       OR timestamp_hash(v)::int8 = timestamp_hash_extended(v, 1)::int8;

SELECT v as value, pg_lsn_hash(v)::int8 as standard,
       pg_lsn_hash_extended(v, 0)::int8 as extended0,
       pg_lsn_hash_extended(v, 1)::int8 as extended1
FROM   (VALUES (NULL::pg_lsn), ('16/B374D84'), ('30/B374D84'),
        ('255/B374D84'), ('25/B379D90'), ('900/F37FD90')) x(v)
WHERE  pg_lsn_hash(v)::int8 != pg_lsn_hash_extended(v, 0)::int8
       OR pg_lsn_hash(v)::int8 = pg_lsn_hash_extended(v, 1)::int8;

--
-- Check special cases for specific data types
--
SELECT hashfloat4('0'::float4) = hashfloat4('-0'::float4) AS t;
SELECT hashfloat4('NaN'::float4) = hashfloat4(-'NaN'::float4) AS t;
SELECT hashfloat8('0'::float8) = hashfloat8('-0'::float8) AS t;
SELECT hashfloat8('NaN'::float8) = hashfloat8(-'NaN'::float8) AS t;
SELECT hashfloat4('NaN'::float4) = hashfloat8('NaN'::float8) AS t;
