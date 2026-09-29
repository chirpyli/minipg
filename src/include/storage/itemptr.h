/*-------------------------------------------------------------------------
 *
 * itemptr.h
 *	  POSTGRES disk item pointer definitions.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/itemptr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ITEMPTR_H
#define ITEMPTR_H

#include "storage/block.h"
#include "storage/off.h"

/*
 * ItemPointer：
 *
 * 这是一个指向已知文件某个 disk page 中某个 item 的指针
 * （例如，从 index 到其父表的一条 cross-link）。
 * ip_blkid 告诉我们哪个 block，ip_posid 告诉我们想要 linp
 * (ItemIdData) 数组中的哪一项。
 *
 * 注意：由于磁盘上每个 tuple header 和 index tuple header 中都有一个
 * item pointer，因此不把空间浪费在结构体 padding bytes 上非常重要。
 * 该 struct 被设计为六个字节长（它包含三个 int16 字段），但少数编译器
 * 会在未强制处理时将其填充到八个字节。我们在可能的情况下施加适当的
 * "说服"。如果你的编译器无法配合，你将浪费大量空间。
 */
typedef struct ItemPointerData
{
	BlockIdData ip_blkid;
	OffsetNumber ip_posid;
}

/* If compiler understands packed and aligned pragmas, use those */
#if defined(pg_attribute_packed) && defined(pg_attribute_aligned)
			pg_attribute_packed()
			pg_attribute_aligned(2)
#endif
ItemPointerData;

typedef ItemPointerData *ItemPointer;

/* ----------------
 *		special values used in heap tuples (t_ctid)
 * ----------------
 */

/* ----------------
 *		support macros
 * ----------------
 */

/*
 * ItemPointerIsValid
 *		True iff the disk item pointer is not NULL.
 */
#define ItemPointerIsValid(pointer) \
	((bool) (PointerIsValid(pointer) && ((pointer)->ip_posid != 0)))

/*
 * ItemPointerGetBlockNumberNoCheck
 *		Returns the block number of a disk item pointer.
 */
#define ItemPointerGetBlockNumberNoCheck(pointer) \
( \
	BlockIdGetBlockNumber(&(pointer)->ip_blkid) \
)

/*
 * ItemPointerGetBlockNumber
 *		As above, but verifies that the item pointer looks valid.
 */
#define ItemPointerGetBlockNumber(pointer) \
( \
	AssertMacro(ItemPointerIsValid(pointer)), \
	ItemPointerGetBlockNumberNoCheck(pointer) \
)

/*
 * ItemPointerGetOffsetNumberNoCheck
 *		Returns the offset number of a disk item pointer.
 */
#define ItemPointerGetOffsetNumberNoCheck(pointer) \
( \
	(pointer)->ip_posid \
)

/*
 * ItemPointerGetOffsetNumber
 *		As above, but verifies that the item pointer looks valid.
 */
#define ItemPointerGetOffsetNumber(pointer) \
( \
	AssertMacro(ItemPointerIsValid(pointer)), \
	ItemPointerGetOffsetNumberNoCheck(pointer) \
)

/*
 * ItemPointerSet
 *		Sets a disk item pointer to the specified block and offset.
 */
#define ItemPointerSet(pointer, blockNumber, offNum) \
( \
	AssertMacro(PointerIsValid(pointer)), \
	BlockIdSet(&((pointer)->ip_blkid), blockNumber), \
	(pointer)->ip_posid = offNum \
)

/*
 * ItemPointerSetBlockNumber
 *		Sets a disk item pointer to the specified block.
 */
#define ItemPointerSetBlockNumber(pointer, blockNumber) \
( \
	AssertMacro(PointerIsValid(pointer)), \
	BlockIdSet(&((pointer)->ip_blkid), blockNumber) \
)

/*
 * ItemPointerSetOffsetNumber
 *		Sets a disk item pointer to the specified offset.
 */
#define ItemPointerSetOffsetNumber(pointer, offsetNumber) \
( \
	AssertMacro(PointerIsValid(pointer)), \
	(pointer)->ip_posid = (offsetNumber) \
)

/*
 * ItemPointerCopy
 *		Copies the contents of one disk item pointer to another.
 *
 * Should there ever be padding in an ItemPointer this would need to be handled
 * differently as it's used as hash key.
 */
#define ItemPointerCopy(fromPointer, toPointer) \
( \
	AssertMacro(PointerIsValid(toPointer)), \
	AssertMacro(PointerIsValid(fromPointer)), \
	*(toPointer) = *(fromPointer) \
)

/*
 * ItemPointerSetInvalid
 *		Sets a disk item pointer to be invalid.
 */
#define ItemPointerSetInvalid(pointer) \
( \
	AssertMacro(PointerIsValid(pointer)), \
	BlockIdSet(&((pointer)->ip_blkid), InvalidBlockNumber), \
	(pointer)->ip_posid = InvalidOffsetNumber \
)

/* ----------------
 *		externs
 * ----------------
 */

extern bool ItemPointerEquals(ItemPointer pointer1, ItemPointer pointer2);
extern int32 ItemPointerCompare(ItemPointer arg1, ItemPointer arg2);
extern void ItemPointerInc(ItemPointer pointer);
extern void ItemPointerDec(ItemPointer pointer);

#endif							/* ITEMPTR_H */
