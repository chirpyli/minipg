/*-------------------------------------------------------------------------
 *
 * snapshot.h
 *	  POSTGRES snapshot definition
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/snapshot.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include "access/htup.h"
#include "access/xlogdefs.h"
#include "datatype/timestamp.h"
#include "lib/pairingheap.h"
#include "storage/buf.h"


/*
 * 各种快照类型。我们使用 SnapshotData 结构来表示"常规"（MVCC）快照
 * 以及具有非 MVCC 语义的"特殊"快照。某个快照的具体语义由其类型编码。
 *
 * 每种快照类型的行为都应在其枚举值旁加以说明，最好使用不针对某个
 * 具体表访问方法（table AM）的表述。
 *
 * 之所以采用快照类型（而不是像过去那样使用回调函数），是因为这样
 * 就可以让同一个快照服务于不同的表访问方法，而不必为每个 AM 各写
 * 一个回调。
 */
typedef enum SnapshotType
{
	/*-------------------------------------------------------------------------
	 * 当且仅当元组对给定的 MVCC 快照有效时，该元组才是可见的。
	 *
	 * 这里我们考虑以下影响：
	 * - 在该快照生成时刻已提交的所有事务
	 * - 本事务之前执行的命令
	 *
	 * 不包括：
	 * - 快照中显示为正在进行的事务
	 * - 在快照生成之后才启动的事务
	 * - 当前命令所做的修改
	 * -------------------------------------------------------------------------
	 */
	SNAPSHOT_MVCC = 0,

	/*-------------------------------------------------------------------------
	 * 当且仅当元组"对自身"有效时，该元组才是可见的。
	 *
	 * 这里我们考虑以下影响：
	 * - 所有已提交的事务（以当前时刻为准）
	 * - 本事务之前执行的命令
	 * - 当前命令所做的修改
	 *
	 * 不包括：
	 * - 正在进行的事务（以当前时刻为准）
	 * -------------------------------------------------------------------------
	 */
	SNAPSHOT_SELF,

	/*
	 * 任何元组都可见。
	 */
	SNAPSHOT_ANY,

	/*
	 * 当且仅当元组作为 TOAST 行有效时，该元组才是可见的。
	 */
	SNAPSHOT_TOAST,

	/*-------------------------------------------------------------------------
	 * 当且仅当元组在计入未结束事务的影响后仍然有效时，该元组才是可见的。
	 *
	 * 这里我们考虑以下影响：
	 * - 所有已提交和正在进行的事务（以当前时刻为准）
	 * - 本事务之前执行的命令
	 * - 当前命令所做的修改
	 *
	 * 就本事务以及已提交/已回滚事务的影响而言，这基本上与
	 * SNAPSHOT_SELF 相同。不过，它还会计入其他仍在进行中的事务的影响。
	 *
	 * 一个特殊的技巧是：当使用这种类型的快照来判断元组可见性时，传入的
	 * snapshot 结构会被用作输出参数，返回影响该元组的并发事务的 xid。
	 * 如果元组的 xmin 来自另一个仍在进行中的事务，则 snapshot->xmin 被
	 * 设为该元组的 xmin；如果元组的 xmin 是已提交有效的、已提交但已死的
	 * 或本事务自身的 xid，则设为 InvalidTransactionId。snapshot->xmax 与
	 * 元组的 xmax 的关系同理。另请参见 InitDirtySnapshot()。
	 * -------------------------------------------------------------------------
	 */
	 SNAPSHOT_DIRTY,

	/*
	 * 当且仅当元组可能对某个事务可见时，该元组才是可见的；如果它肯定
	 * 对所有人都已死亡，即可以被 vacuum 回收，则为 false。
	 *
	 * 在进行可见性检查时，snapshot->min 必须已被设置为要使用的 xmin 水位线
	 * （xmin horizon）。
	 */
	SNAPSHOT_NON_VACUUMABLE
} SnapshotType;

typedef struct SnapshotData *Snapshot;

#define InvalidSnapshot		((Snapshot) NULL)

/*
 * 用于表示所有可能类型的快照的结构体。
 *
 * 快照有以下几种不同的类型：
 * * MVCC 快照
 * * 在恢复期间（Hot-Standby 模式下）取得的 MVCC 快照
 * * 传给 HeapTupleSatisfiesDirty() 的快照
 * * 传给 HeapTupleSatisfiesNonVacuumable() 的快照
 * * 用于 SatisfiesAny、Toast、Self 的快照，这些快照不会访问任何成员。
 *
 * TODO: 使用 NodeTag 来拆分这个结构体大概是个好主意，就像解析器和执行器
 * 节点那样处理，为每种不同类型的快照定义一种类型，以避免各个字段的含义
 * 被过度复用。
 */
typedef struct SnapshotData
{
	SnapshotType snapshot_type; /* 快照类型 */

	/*
	 * 其余字段只用于 MVCC 快照，在特殊快照中通常都只是零值。
	 * （不过 xmin 和 xmax 会被 HeapTupleSatisfiesDirty 特殊使用，
	 * xmin 还会被 HeapTupleSatisfiesNonVacuumable 特殊使用。）
	 *
	 * MVCC 快照永远看不到 XID >= xmax 的效果。除了快照中列出的那些之外，
	 * 它能看见所有更老 XID 的效果。存储 xmin 是一种优化，用于在大多数
	 * 元组上避免搜索 XID 数组。
	 */
	TransactionId xmin;			/* 所有 XID < xmin 对我可见 */
	TransactionId xmax;			/* 所有 XID >= xmax 对我不可见 */

	/*
	 * 对于普通 MVCC 快照，这里包含所有正在进行中的事务 ID，除非该快照是
	 * 在恢复期间取得的，此时它为空。
	 *
	 * 注意：xip[] 中的所有 ID 都满足 xmin <= xip[i] < xmax
	 */
	TransactionId *xip;
	uint32		xcnt;			/* xip[] 中的事务 ID 个数 */

	/*
	 * 对于非历史（non-historic）MVCC 快照，这里包含正在进行中的子事务 ID
	 * （如果是恢复期间取得的，还包含其他正在进行中的事务）。对于历史
	 * 快照（historic snapshot），它包含分配给被重放事务的*所有* xid，
	 * 包括顶层 xid。
	 *
	 * 注意：subxip[] 中的所有 ID 都 >= xmin，但我们不费心过滤掉那些
	 * >= xmax 的 ID
	 */
	TransactionId *subxip;
	int32		subxcnt;		/* subxip[] 中的事务 ID 个数 */
	bool		suboverflowed;	/* subxip 数组是否已溢出？ */

	bool		takenDuringRecovery;	/* 是否为恢复期间形成的快照？ */
	bool		copied;			/* 若为静态快照则为 false */

	CommandId	curcid;			/* 在本事务中，CID < curcid 的可见 */

	/*
	 * 对于 SNAPSHOT_NON_VACUUMABLE（希望将来还能用于更多类型），
	 * 它用于判断某行是否可以被 vacuum 回收。
	 */
	struct GlobalVisState *vistest;

	/*
	 * 记账信息，由快照管理器使用
	 */
	uint32		active_count;	/* ActiveSnapshot 栈上的引用计数 */
	uint32		regd_count;		/* RegisteredSnapshots 上的引用计数 */
	pairingheap_node ph_node;	/* RegisteredSnapshots 堆中的链接 */

	TimestampTz whenTaken;		/* 取得快照时的时间戳 */
	XLogRecPtr	lsn;			/* 取得快照时在 WAL 流中的位置 */

	/*
	 * GetSnapshotData() 构建该快照时的事务完成计数。当自上次
	 * GetSnapshotData() 以来没有任何事务完成时，它可以用来避免重新计算
	 * 静态快照。
	 */
	uint64		snapXactCompletionCount;
} SnapshotData;

#endif							/* SNAPSHOT_H */
