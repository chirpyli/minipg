/*-------------------------------------------------------------------------
 *
 * htup.h
 *	  POSTGRES heap tuple definitions.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/htup.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HTUP_H
#define HTUP_H

#include "storage/itemptr.h"

/* typedefs and forward declarations for structs defined in htup_details.h */

typedef struct HeapTupleHeaderData HeapTupleHeaderData;

typedef HeapTupleHeaderData *HeapTupleHeader;

typedef struct MinimalTupleData MinimalTupleData;

typedef MinimalTupleData *MinimalTuple;


/*
 * HeapTupleData 是一个指向 tuple 的内存数据结构。
 *
 * 该数据结构有几种使用方式：
 *
 * * 指向 disk buffer 中的 tuple：t_data 直接指向 buffer 内部
 *	 （代码最好已经在该 buffer 上持有了 pin，但这并不会体现在
 *	 HeapTupleData 本身中）。
 *
 * * 指向空：t_data 为 NULL。在某些函数中用作失败指示。
 *
 * * 作为 palloc'd tuple 的一部分：HeapTupleData 自身与 tuple
 *	 共同构成单个 palloc'd chunk。t_data 指向紧随 HeapTupleData 结构体
 *	 之后的内存位置（偏移量为 HEAPTUPLESIZE）。
 *	 这是 heap_form_tuple 及相关例程的输出格式。
 *
 * * 单独分配的 tuple：t_data 指向一个 palloc'd chunk，且该 chunk
 *	 与 HeapTupleData 不相邻。（这种情况已被废弃，因为很难与第 1 种
 *	 情况区分。它只应用于代码明确知道第 1 种情况不会出现的有限场景。）
 *
 * * 单独分配的 minimal tuple：t_data 指向 MinimalTuple 起始位置之前
 *	 MINIMAL_TUPLE_OFFSET 字节处。与前一种情况一样，通过检查无法
 *	 与第 1 种情况区分；负责建立或销毁此表示的代码必须清楚它在做什么。
 *
 * 除指向空的情况外，t_len 应始终有效。
 * 如果 HeapTupleData 指向 disk buffer，或表示磁盘上 tuple 的副本，
 * 则 t_self 与 t_tableOid 应有效。在人工构造的 tuple 中，
 * 它们应被显式置为无效。
 */
typedef struct HeapTupleData
{
	uint32		t_len;			/* length of *t_data */
	ItemPointerData t_self;		/* SelfItemPointer */
	Oid			t_tableOid;		/* table the tuple came from */
#define FIELDNO_HEAPTUPLEDATA_DATA 3
	HeapTupleHeader t_data;		/* -> tuple header and data */
} HeapTupleData;

typedef HeapTupleData *HeapTuple;

#define HEAPTUPLESIZE	MAXALIGN(sizeof(HeapTupleData))

/*
 * Accessor macros to be used with HeapTuple pointers.
 */
#define HeapTupleIsValid(tuple) PointerIsValid(tuple)

/* HeapTupleHeader functions implemented in utils/time/combocid.c */
extern CommandId HeapTupleHeaderGetCmin(HeapTupleHeader tup);
extern CommandId HeapTupleHeaderGetCmax(HeapTupleHeader tup);
extern void HeapTupleHeaderAdjustCmax(HeapTupleHeader tup,
									  CommandId *cmax, bool *iscombo);

/* Prototype for HeapTupleHeader accessors in heapam.c */
extern TransactionId HeapTupleGetUpdateXid(HeapTupleHeader tuple);

#endif							/* HTUP_H */
