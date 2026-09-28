/*-------------------------------------------------------------------------
 *
 * timestamp_core.h
 *	  内核计时核心：内部时间表示与工具函数的声明。
 *
 * minipg 已裁剪 SQL 层的 date/time/timestamp/timestamptz 类型，但事务、WAL、
 * 锁、超时、快照、进程管理与日志仍以 TimestampTz（自 2000-01-01 UTC 起的
 * 微秒数）或 pg_time_t 记录墙钟时间。本头文件提供这部分内核基础设施的接口。
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/timestamp_core.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TIMESTAMP_CORE_H
#define TIMESTAMP_CORE_H

#include "datatype/timestamp.h"
#include "fmgr.h"
#include "pgtime.h"

/*
 * The only output style left after cropping the DateStyle GUC.  It is kept as
 * a symbolic name for EncodeDateTime's callers.
 */
#define USE_ISO_DATES		1

/*
 * Working buffer size for internal timestamp printing.  Longer outputs will
 * overrun buffers, so this must suffice for all possible output.
 */
#define MAXDATELEN		128

/*
 * TMODULO()
 * Like FMODULO(), but work on the timestamp datatype (now always int64).
 * We assume that int64 follows the C99 semantics for division (negative
 * quotients truncate towards zero).
 */
#define TMODULO(t,q,u) \
do { \
	(q) = ((t) / (u)); \
	if ((q) != 0) (t) -= ((q) * (u)); \
} while(0)

#define TimestampTzPlusMilliseconds(tz,ms) ((tz) + ((ms) * (int64) 1000))


/* Set at postmaster start */
extern TimestampTz PgStartTime;

/* Set at configuration reload */
extern TimestampTz PgReloadTime;


/* Internal routines (not fmgr-callable) */

extern TimestampTz GetCurrentTimestamp(void);
extern void TimestampDifference(TimestampTz start_time, TimestampTz stop_time,
								long *secs, int *microsecs);
extern long TimestampDifferenceMilliseconds(TimestampTz start_time,
											TimestampTz stop_time);
extern bool TimestampDifferenceExceeds(TimestampTz start_time,
									   TimestampTz stop_time,
									   int msec);

extern TimestampTz time_t_to_timestamptz(pg_time_t tm);
extern pg_time_t timestamptz_to_time_t(TimestampTz t);

extern const char *timestamptz_to_str(TimestampTz t);

extern int	timestamp2tm(Timestamp dt, int *tzp, struct pg_tm *tm,
						 fsec_t *fsec, const char **tzn, pg_tz *attimezone);
extern void dt2time(Timestamp dt, int *hour, int *min, int *sec, fsec_t *fsec);

extern void EncodeSpecialTimestamp(Timestamp dt, char *str);
extern void EncodeDateTime(struct pg_tm *tm, fsec_t fsec, bool print_tz,
						   int tz, const char *tzn, int style, char *str);

extern void j2date(int jd, int *year, int *month, int *day);


/* SQL-visible monitoring accessors (int8 microseconds since 2000-01-01) */

extern Datum pg_postmaster_start_time(PG_FUNCTION_ARGS);
extern Datum pg_conf_load_time(PG_FUNCTION_ARGS);

#endif							/* TIMESTAMP_CORE_H */
