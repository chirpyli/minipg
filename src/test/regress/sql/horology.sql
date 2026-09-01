--
-- HOROLOGY
--

SHOW TimeZone;  -- Many of these tests depend on the prevailing settings
SHOW DateStyle;

--
-- Test various input formats
--
SELECT timestamp with time zone '20011227 040506+08';
SELECT timestamp with time zone '20011227 040506-08';
SELECT timestamp with time zone '20011227 040506.789+08';
SELECT timestamp with time zone '20011227 040506.789-08';
SELECT timestamp with time zone '20011227T040506+08';
SELECT timestamp with time zone '20011227T040506-08';
SELECT timestamp with time zone '20011227T040506.789+08';
SELECT timestamp with time zone '20011227T040506.789-08';
SELECT timestamp with time zone '2001-12-27 04:05:06.789-08';
SELECT timestamp with time zone '2001.12.27 04:05:06.789-08';
SELECT timestamp with time zone '2001/12/27 04:05:06.789-08';
SELECT timestamp with time zone '12/27/2001 04:05:06.789-08';
-- should fail in mdy mode:
SELECT timestamp with time zone '27/12/2001 04:05:06.789-08';
set datestyle to dmy;
SELECT timestamp with time zone '27/12/2001 04:05:06.789-08';
reset datestyle;
SELECT timestamp with time zone 'Y2001M12D27H04M05S06.789+08';
SELECT timestamp with time zone 'Y2001M12D27H04M05S06.789-08';
SELECT timestamp with time zone 'Y2001M12D27H04MM05S06.789+08';
SELECT timestamp with time zone 'Y2001M12D27H04MM05S06.789-08';
SELECT timestamp with time zone 'J2452271+08';
SELECT timestamp with time zone 'J2452271-08';
SELECT timestamp with time zone 'J2452271.5+08';
SELECT timestamp with time zone 'J2452271.5-08';
SELECT timestamp with time zone 'J2452271 04:05:06+08';
SELECT timestamp with time zone 'J2452271 04:05:06-08';
SELECT timestamp with time zone 'J2452271T040506+08';
SELECT timestamp with time zone 'J2452271T040506-08';
SELECT timestamp with time zone 'J2452271T040506.789+08';
SELECT timestamp with time zone 'J2452271T040506.789-08';
-- German/European-style dates with periods as delimiters
SELECT timestamp with time zone '12.27.2001 04:05:06.789+08';
SELECT timestamp with time zone '12.27.2001 04:05:06.789-08';
SET DateStyle = 'German';
SELECT timestamp with time zone '27.12.2001 04:05:06.789+08';
SELECT timestamp with time zone '27.12.2001 04:05:06.789-08';
SET DateStyle = 'ISO';
-- As of 7.4, allow time without time zone having a time zone specified
SELECT time without time zone '040506.789+08';
SELECT time without time zone '040506.789-08';
SELECT time without time zone 'T040506.789+08';
SELECT time without time zone 'T040506.789-08';
SELECT time with time zone '040506.789+08';
SELECT time with time zone '040506.789-08';
SELECT time with time zone 'T040506.789+08';
SELECT time with time zone 'T040506.789-08';
SELECT time with time zone 'T040506.789 +08';
SELECT time with time zone 'T040506.789 -08';
SET DateStyle = 'Postgres, MDY';
-- Check Julian dates BC
SELECT date 'J1520447' AS "Confucius' Birthday";
SELECT date 'J0' AS "Julian Epoch";

--
-- date, time arithmetic
--

SELECT date '1981-02-03' + time '04:05:06' AS "Date + Time";
SELECT date '1991-02-03' + time with time zone '04:05:06 PST' AS "Date + Time PST";
SELECT date '2001-02-03' + time with time zone '04:05:06 UTC' AS "Date + Time UTC";
SELECT date '1991-02-03' + interval '2 years' AS "Add Two Years";
SELECT date '2001-12-13' - interval '2 years' AS "Subtract Two Years";
-- subtract time from date should not make sense; use interval instead
SELECT date '1991-02-03' - time '04:05:06' AS "Subtract Time";
SELECT date '1991-02-03' - time with time zone '04:05:06 UTC' AS "Subtract Time UTC";

--
-- timestamp, interval arithmetic
--

SELECT timestamp without time zone '1996-03-01' - interval '1 second' AS "Feb 29";
SELECT timestamp without time zone '1999-03-01' - interval '1 second' AS "Feb 28";
SELECT timestamp without time zone '2000-03-01' - interval '1 second' AS "Feb 29";
SELECT timestamp without time zone '1999-12-01' + interval '1 month - 1 second' AS "Dec 31";
SELECT timestamp without time zone 'Jan 1, 4713 BC' + interval '106000000 days' AS "Feb 23, 285506";
SELECT timestamp without time zone 'Jan 1, 4713 BC' + interval '107000000 days' AS "Jan 20, 288244";
SELECT timestamp without time zone 'Jan 1, 4713 BC' + interval '109203489 days' AS "Dec 31, 294276";
SELECT timestamp without time zone '2000-01-01' - interval '2483590 days' AS "out of range";
SELECT timestamp without time zone '12/31/294276' - timestamp without time zone '12/23/1999' AS "106751991 Days";

-- Shorthand values
-- Not directly usable for regression testing since these are not constants.
-- So, just try to test parser and hope for the best - thomas 97/04/26
SELECT (timestamp without time zone 'today' = (timestamp without time zone 'yesterday' + interval '1 day')) as "True";
SELECT (timestamp without time zone 'today' = (timestamp without time zone 'tomorrow' - interval '1 day')) as "True";
SELECT (timestamp without time zone 'today 10:30' = (timestamp without time zone 'yesterday' + interval '1 day 10 hr 30 min')) as "True";
SELECT (timestamp without time zone '10:30 today' = (timestamp without time zone 'yesterday' + interval '1 day 10 hr 30 min')) as "True";
SELECT (timestamp without time zone 'tomorrow' = (timestamp without time zone 'yesterday' + interval '2 days')) as "True";
SELECT (timestamp without time zone 'tomorrow 16:00:00' = (timestamp without time zone 'today' + interval '1 day 16 hours')) as "True";
SELECT (timestamp without time zone '16:00:00 tomorrow' = (timestamp without time zone 'today' + interval '1 day 16 hours')) as "True";
SELECT (timestamp without time zone 'yesterday 12:34:56' = (timestamp without time zone 'tomorrow' - interval '2 days - 12:34:56')) as "True";
SELECT (timestamp without time zone '12:34:56 yesterday' = (timestamp without time zone 'tomorrow' - interval '2 days - 12:34:56')) as "True";
SELECT (timestamp without time zone 'tomorrow' > 'now') as "True";

-- Convert from date and time to timestamp
-- This test used to be timestamp(date,time) but no longer allowed by grammar
-- to enable support for SQL99 timestamp type syntax.
SELECT date '1994-01-01' + time '11:00' AS "Jan_01_1994_11am";
SELECT date '1994-01-01' + time '10:00' AS "Jan_01_1994_10am";

SELECT d1 + interval '1 year' AS one_year FROM TIMESTAMP_TBL;
SELECT d1 - interval '1 year' AS one_year FROM TIMESTAMP_TBL;

SELECT timestamp with time zone '1996-03-01' - interval '1 second' AS "Feb 29";
SELECT timestamp with time zone '1999-03-01' - interval '1 second' AS "Feb 28";
SELECT timestamp with time zone '2000-03-01' - interval '1 second' AS "Feb 29";
SELECT timestamp with time zone '1999-12-01' + interval '1 month - 1 second' AS "Dec 31";
SELECT timestamp with time zone '2000-01-01' - interval '2483590 days' AS "out of range";

SELECT (timestamp with time zone 'today' = (timestamp with time zone 'yesterday' + interval '1 day')) as "True";
SELECT (timestamp with time zone 'today' = (timestamp with time zone 'tomorrow' - interval '1 day')) as "True";
SELECT (timestamp with time zone 'tomorrow' = (timestamp with time zone 'yesterday' + interval '2 days')) as "True";
SELECT (timestamp with time zone 'tomorrow' > 'now') as "True";

-- timestamp with time zone, interval arithmetic around DST change
-- (just for fun, let's use an intentionally nonstandard POSIX zone spec)
SET TIME ZONE 'CST7CDT,M4.1.0,M10.5.0';
SELECT timestamp with time zone '2005-04-02 12:00-07' + interval '1 day' as "Apr 3, 12:00";
SELECT timestamp with time zone '2005-04-02 12:00-07' + interval '24 hours' as "Apr 3, 13:00";
SELECT timestamp with time zone '2005-04-03 12:00-06' - interval '1 day' as "Apr 2, 12:00";
SELECT timestamp with time zone '2005-04-03 12:00-06' - interval '24 hours' as "Apr 2, 11:00";
RESET TIME ZONE;


SELECT timestamptz(date '1994-01-01', time '11:00') AS "Jan_01_1994_10am";
SELECT timestamptz(date '1994-01-01', time '10:00') AS "Jan_01_1994_9am";
SELECT timestamptz(date '1994-01-01', time with time zone '11:00-8') AS "Jan_01_1994_11am";
SELECT timestamptz(date '1994-01-01', time with time zone '10:00-8') AS "Jan_01_1994_10am";
SELECT timestamptz(date '1994-01-01', time with time zone '11:00-5') AS "Jan_01_1994_8am";

SELECT d1 + interval '1 year' AS one_year FROM TIMESTAMPTZ_TBL;
SELECT d1 - interval '1 year' AS one_year FROM TIMESTAMPTZ_TBL;

--
-- time, interval arithmetic
--

SELECT CAST(time '01:02' AS interval) AS "+01:02";
SELECT CAST(interval '02:03' AS time) AS "02:03:00";
SELECT time '01:30' + interval '02:01' AS "03:31:00";
SELECT time '01:30' - interval '02:01' AS "23:29:00";
SELECT time '02:30' + interval '36:01' AS "14:31:00";
SELECT time '03:30' + interval '1 month 04:01' AS "07:31:00";
SELECT CAST(time with time zone '01:02-08' AS interval) AS "+00:01";
SELECT CAST(interval '02:03' AS time with time zone) AS "02:03:00-08";
SELECT time with time zone '01:30-08' - interval '02:01' AS "23:29:00-08";
SELECT time with time zone '02:30-08' + interval '36:01' AS "14:31:00-08";

-- These two tests cannot be used because they default to current timezone,
-- which may be either -08 or -07 depending on the time of year.
-- SELECT time with time zone '01:30' + interval '02:01' AS "03:31:00-08";
-- SELECT time with time zone '03:30' + interval '1 month 04:01' AS "07:31:00-08";
-- Try the following two tests instead, as a poor substitute

SELECT CAST(CAST(date 'today' + time with time zone '05:30'
            + interval '02:01' AS time with time zone) AS time) AS "07:31:00";

SELECT CAST(cast(date 'today' + time with time zone '03:30'
  + interval '1 month 04:01' as timestamp without time zone) AS time) AS "07:31:00";

SELECT t.d1 AS t, i.f1 AS i, t.d1 + i.f1 AS "add", t.d1 - i.f1 AS "subtract"
  FROM TIMESTAMP_TBL t, INTERVAL_TBL i
  WHERE t.d1 BETWEEN '1990-01-01' AND '2001-01-01'
    AND i.f1 BETWEEN '00:00' AND '23:00'
  ORDER BY 1,2;

SELECT t.f1 AS t, i.f1 AS i, t.f1 + i.f1 AS "add", t.f1 - i.f1 AS "subtract"
  FROM TIME_TBL t, INTERVAL_TBL i
  ORDER BY 1,2;

-- SQL9x OVERLAPS operator
-- test with time zone
SELECT (timestamp with time zone '2000-11-27', timestamp with time zone '2000-11-28')
  OVERLAPS (timestamp with time zone '2000-11-27 12:00', timestamp with time zone '2000-11-30') AS "True";

SELECT (timestamp with time zone '2000-11-26', timestamp with time zone '2000-11-27')
  OVERLAPS (timestamp with time zone '2000-11-27 12:00', timestamp with time zone '2000-11-30') AS "False";

SELECT (timestamp with time zone '2000-11-27', timestamp with time zone '2000-11-28')
  OVERLAPS (timestamp with time zone '2000-11-27 12:00', interval '1 day') AS "True";

SELECT (timestamp with time zone '2000-11-27', interval '12 hours')
  OVERLAPS (timestamp with time zone '2000-11-27 12:00', timestamp with time zone '2000-11-30') AS "False";

SELECT (timestamp with time zone '2000-11-27', interval '12 hours')
  OVERLAPS (timestamp with time zone '2000-11-27', interval '12 hours') AS "True";

SELECT (timestamp with time zone '2000-11-27', interval '12 hours')
  OVERLAPS (timestamp with time zone '2000-11-27 12:00', interval '12 hours') AS "False";

-- test without time zone
SELECT (timestamp without time zone '2000-11-27', timestamp without time zone '2000-11-28')
  OVERLAPS (timestamp without time zone '2000-11-27 12:00', timestamp without time zone '2000-11-30') AS "True";

SELECT (timestamp without time zone '2000-11-26', timestamp without time zone '2000-11-27')
  OVERLAPS (timestamp without time zone '2000-11-27 12:00', timestamp without time zone '2000-11-30') AS "False";

SELECT (timestamp without time zone '2000-11-27', timestamp without time zone '2000-11-28')
  OVERLAPS (timestamp without time zone '2000-11-27 12:00', interval '1 day') AS "True";

SELECT (timestamp without time zone '2000-11-27', interval '12 hours')
  OVERLAPS (timestamp without time zone '2000-11-27 12:00', timestamp without time zone '2000-11-30') AS "False";

SELECT (timestamp without time zone '2000-11-27', interval '12 hours')
  OVERLAPS (timestamp without time zone '2000-11-27', interval '12 hours') AS "True";

SELECT (timestamp without time zone '2000-11-27', interval '12 hours')
  OVERLAPS (timestamp without time zone '2000-11-27 12:00', interval '12 hours') AS "False";

-- test time and interval
SELECT (time '00:00', time '01:00')
  OVERLAPS (time '00:30', time '01:30') AS "True";

SELECT (time '00:00', interval '1 hour')
  OVERLAPS (time '00:30', interval '1 hour') AS "True";

SELECT (time '00:00', interval '1 hour')
  OVERLAPS (time '01:30', interval '1 hour') AS "False";

-- SQL99 seems to want this to be false (and we conform to the spec).
-- istm that this *should* return true, on the theory that time
-- intervals can wrap around the day boundary - thomas 2001-09-25
SELECT (time '00:00', interval '1 hour')
  OVERLAPS (time '01:30', interval '1 day') AS "False";

CREATE TABLE TEMP_TIMESTAMP (f1 timestamp with time zone);

-- get some candidate input values

INSERT INTO TEMP_TIMESTAMP (f1)
  SELECT d1 FROM TIMESTAMP_TBL
  WHERE d1 BETWEEN '13-jun-1957' AND '1-jan-1997'
   OR d1 BETWEEN '1-jan-1999' AND '1-jan-2010';

SELECT f1 AS "timestamp"
  FROM TEMP_TIMESTAMP
  ORDER BY "timestamp";

SELECT d.f1 AS "timestamp", t.f1 AS "interval", d.f1 + t.f1 AS plus
  FROM TEMP_TIMESTAMP d, INTERVAL_TBL t
  ORDER BY plus, "timestamp", "interval";

SELECT d.f1 AS "timestamp", t.f1 AS "interval", d.f1 - t.f1 AS minus
  FROM TEMP_TIMESTAMP d, INTERVAL_TBL t
  WHERE isfinite(d.f1)
  ORDER BY minus, "timestamp", "interval";

SELECT d.f1 AS "timestamp",
   timestamp with time zone '1980-01-06 00:00 GMT' AS gpstime_zero,
   d.f1 - timestamp with time zone '1980-01-06 00:00 GMT' AS difference
  FROM TEMP_TIMESTAMP d
  ORDER BY difference;

SELECT d1.f1 AS timestamp1, d2.f1 AS timestamp2, d1.f1 - d2.f1 AS difference
  FROM TEMP_TIMESTAMP d1, TEMP_TIMESTAMP d2
  ORDER BY timestamp1, timestamp2, difference;

--
-- Conversions
--

SELECT f1 AS "timestamp", date(f1) AS date
  FROM TEMP_TIMESTAMP
  WHERE f1 <> timestamp 'now'
  ORDER BY date, "timestamp";

DROP TABLE TEMP_TIMESTAMP;

--
-- Comparisons between datetime types, especially overflow cases
---

SELECT '2202020-10-05'::date::timestamp;  -- fail
SELECT '2202020-10-05'::date > '2020-10-05'::timestamp as t;
SELECT '2020-10-05'::timestamp > '2202020-10-05'::date as f;

SELECT '2202020-10-05'::date::timestamptz;  -- fail
SELECT '2202020-10-05'::date > '2020-10-05'::timestamptz as t;
SELECT '2020-10-05'::timestamptz > '2202020-10-05'::date as f;

-- This conversion may work depending on timezone
SELECT '4714-11-24 BC'::date::timestamptz;
SET TimeZone = 'UTC-2';
SELECT '4714-11-24 BC'::date::timestamptz;  -- fail

SELECT '4714-11-24 BC'::date < '2020-10-05'::timestamptz as t;
SELECT '2020-10-05'::timestamptz >= '4714-11-24 BC'::date as t;

SELECT '4714-11-24 BC'::timestamp < '2020-10-05'::timestamptz as t;
SELECT '2020-10-05'::timestamptz >= '4714-11-24 BC'::timestamp as t;

RESET TimeZone;

--
-- Formats
--

SET DateStyle TO 'US,Postgres';

SHOW DateStyle;

SELECT d1 AS us_postgres FROM TIMESTAMP_TBL;

SET DateStyle TO 'US,ISO';

SELECT d1 AS us_iso FROM TIMESTAMP_TBL;

SET DateStyle TO 'US,SQL';

SHOW DateStyle;

SELECT d1 AS us_sql FROM TIMESTAMP_TBL;

SET DateStyle TO 'European,Postgres';

SHOW DateStyle;

INSERT INTO TIMESTAMP_TBL VALUES('13/06/1957');

SELECT count(*) as one FROM TIMESTAMP_TBL WHERE d1 = 'Jun 13 1957';

SELECT d1 AS european_postgres FROM TIMESTAMP_TBL;

SET DateStyle TO 'European,ISO';

SHOW DateStyle;

SELECT d1 AS european_iso FROM TIMESTAMP_TBL;

SET DateStyle TO 'European,SQL';

SHOW DateStyle;

SELECT d1 AS european_sql FROM TIMESTAMP_TBL;

RESET DateStyle;


--
-- Check behavior with SQL-style fixed-GMT-offset time zone (cf bug #8572)
--

SET TIME ZONE 'Etc/GMT+5';
SET TIME ZONE '-1.5';

SHOW TIME ZONE;

SELECT '2012-12-12 12:00'::timestamptz;
SELECT '2012-12-12 12:00 Etc/GMT+5'::timestamptz;


RESET TIME ZONE;
