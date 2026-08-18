/*-------------------------------------------------------------------------
 *
 * intagg.c
 *	 从 numeric.c 中提取的整数聚合实现（numeric 类型被裁剪后）。
 *	 整数 sum/avg 使用 int128 累加状态，状态以 _int8 数组（{sum, count}）表示。
 *
 * 注意：本文件不依赖 numeric 类型。
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/utils/adt/intagg.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/int8.h"
#include "common/int.h"
#include "libpq/pqformat.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "access/tupmacs.h"

/*
 * 整数求和/求平均的状态：_int8 数组 [sum, count]。
 * sum 为 int128 累加（拆成两个 int64 以无损表示），count 为参与计数。
 */

/* sum(int2) / sum(int4) 的普通转移函数：直接累加为 int8 */
Datum
int2_sum(PG_FUNCTION_ARGS)
{
	int64		sum = PG_GETARG_INT64(0);
	int32		newval = PG_GETARG_INT32(1);
	int64		result;

	if (unlikely(pg_add_s64_overflow(sum, (int64) newval, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int4_sum(PG_FUNCTION_ARGS)
{
	int64		sum = PG_GETARG_INT64(0);
	int32		newval = PG_GETARG_INT32(1);
	int64		result;

	if (unlikely(pg_add_s64_overflow(sum, (int64) newval, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

/* 平均的转移函数：_int8 状态以 3 元素 int64 数组表示 {sum_lo, sum_hi, count} */
static ArrayType *
int_avg_accum_common(ArrayType *transarray, int64 newval)
{
	int128		sum;
	int64		count;
	int64		newarr[3];
	ArrayType  *res;

	if (transarray == NULL ||
		ARR_HASNULL(transarray) ||
		ARR_DIMS(transarray)[0] != 3)
	{
		/* 新状态 */
		sum = (int128) newval;
		count = 1;
	}
	else
	{
		int64   *arr = (int64 *) ARR_DATA_PTR(transarray);

		sum = ((int128) arr[1] << 64) | ((int128) (uint64) arr[0]);
		count = arr[2] + 1;
		sum = sum + (int128) newval;
	}

	newarr[0] = (int64) (sum & 0xFFFFFFFFFFFFFFFFULL);
	newarr[1] = (int64) (sum >> 64);
	newarr[2] = count;
	res = construct_array((Datum *) newarr, 3, INT8OID,
						  sizeof(int64), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);
	return res;
}

Datum
int2_avg_accum(PG_FUNCTION_ARGS)
{
	ArrayType   *transarray = PG_ARGISNULL(0) ? NULL : PG_GETARG_ARRAYTYPE_P(0);
	int32		newval = PG_GETARG_INT32(1);

	PG_RETURN_ARRAYTYPE_P(int_avg_accum_common(transarray, (int64) newval));
}

Datum
int4_avg_accum(PG_FUNCTION_ARGS)
{
	ArrayType   *transarray = PG_ARGISNULL(0) ? NULL : PG_GETARG_ARRAYTYPE_P(0);
	int32		newval = PG_GETARG_INT32(1);

	PG_RETURN_ARRAYTYPE_P(int_avg_accum_common(transarray, (int64) newval));
}

Datum
int8_avg_accum(PG_FUNCTION_ARGS)
{
	ArrayType   *transarray = PG_ARGISNULL(0) ? NULL : PG_GETARG_ARRAYTYPE_P(0);
	int64		newval = PG_GETARG_INT64(1);

	PG_RETURN_ARRAYTYPE_P(int_avg_accum_common(transarray, newval));
}

/* 逆转移函数（用于可移动聚合） */
static ArrayType *
int_avg_accum_inv_common(ArrayType *transarray, int64 newval)
{
	int64	   *arr;
	int128		sum;
	int64		count;

	if (ARR_HASNULL(transarray) ||
		ARR_DIMS(transarray)[0] != 3)
		elog(ERROR, "expected 3-element int8 array");

	arr = (int64 *) ARR_DATA_PTR(transarray);
	sum = ((int128) arr[1] << 64) | ((int128) (uint64) arr[0]);
	count = arr[2];
	sum = sum - (int128) newval;
	count = count - 1;
	{
		int64		newarr[3];
		ArrayType  *res;

		newarr[0] = (int64) (sum & 0xFFFFFFFFFFFFFFFFULL);
		newarr[1] = (int64) (sum >> 64);
		newarr[2] = count;
		res = construct_array((Datum *) newarr, 3, INT8OID,
							  sizeof(int64), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);
		return res;
	}
}

Datum
int2_avg_accum_inv(PG_FUNCTION_ARGS)
{
	ArrayType   *transarray = PG_GETARG_ARRAYTYPE_P(0);
	int32		newval = PG_GETARG_INT32(1);

	PG_RETURN_ARRAYTYPE_P(int_avg_accum_inv_common(transarray, (int64) newval));
}

Datum
int4_avg_accum_inv(PG_FUNCTION_ARGS)
{
	ArrayType   *transarray = PG_GETARG_ARRAYTYPE_P(0);
	int32		newval = PG_GETARG_INT32(1);

	PG_RETURN_ARRAYTYPE_P(int_avg_accum_inv_common(transarray, (int64) newval));
}

Datum
int8_avg_accum_inv(PG_FUNCTION_ARGS)
{
	ArrayType   *transarray = PG_GETARG_ARRAYTYPE_P(0);
	int64		newval = PG_GETARG_INT64(1);

	PG_RETURN_ARRAYTYPE_P(int_avg_accum_inv_common(transarray, newval));
}

/* sum 的终函数：从状态返回 int8 求和结果 */
Datum
int2int4_sum(PG_FUNCTION_ARGS)
{
	ArrayType   *transarray = PG_GETARG_ARRAYTYPE_P(0);
	int64	   *arr;

	if (ARR_HASNULL(transarray) ||
		ARR_DIMS(transarray)[0] != 3)
		elog(ERROR, "expected 3-element int8 array");

	arr = (int64 *) ARR_DATA_PTR(transarray);
	PG_RETURN_INT64((int64) (arr[0]));	/* 低 64 位即 sum 截断 */
}

/* avg 的终函数：返回 float8 平均值 */
Datum
int8_avg(PG_FUNCTION_ARGS)
{
	ArrayType   *transarray = PG_GETARG_ARRAYTYPE_P(0);
	int64	   *arr;
	int128		sum;
	int64		count;

	if (ARR_HASNULL(transarray) ||
		ARR_DIMS(transarray)[0] != 3)
		elog(ERROR, "expected 3-element int8 array");

	arr = (int64 *) ARR_DATA_PTR(transarray);
	sum = ((int128) arr[1] << 64) | ((int128) (uint64) arr[0]);
	count = arr[2];
	if (count == 0)
		PG_RETURN_NULL();
	PG_RETURN_FLOAT8((float8) sum / (float8) count);
}

/* 组合函数：合并两个状态 */
Datum
int4_avg_combine(PG_FUNCTION_ARGS)
{
	ArrayType   *transarray1 = PG_ARGISNULL(0) ? NULL : PG_GETARG_ARRAYTYPE_P(0);
	ArrayType   *transarray2 = PG_ARGISNULL(1) ? NULL : PG_GETARG_ARRAYTYPE_P(1);
	int128		sum1 = 0, sum2 = 0;
	int64		count1 = 0, count2 = 0;

	if (transarray1 && !(ARR_HASNULL(transarray1) || ARR_DIMS(transarray1)[0] != 3))
	{
		int64   *a = (int64 *) ARR_DATA_PTR(transarray1);
		sum1 = ((int128) a[1] << 64) | ((int128) (uint64) a[0]);
		count1 = a[2];
	}
	if (transarray2 && !(ARR_HASNULL(transarray2) || ARR_DIMS(transarray2)[0] != 3))
	{
		int64   *a = (int64 *) ARR_DATA_PTR(transarray2);
		sum2 = ((int128) a[1] << 64) | ((int128) (uint64) a[0]);
		count2 = a[2];
	}

	{
		int128		sum = sum1 + sum2;
		int64		count = count1 + count2;
		int64		newarr[3];
		ArrayType  *res;

		newarr[0] = (int64) (sum & 0xFFFFFFFFFFFFFFFFULL);
		newarr[1] = (int64) (sum >> 64);
		newarr[2] = count;
		res = construct_array((Datum *) newarr, 3, INT8OID,
							  sizeof(int64), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);
		PG_RETURN_ARRAYTYPE_P(res);
	}
}
