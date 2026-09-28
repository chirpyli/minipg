/*-------------------------------------------------------------------------
 *
 * proclist_types.h
 *		doubly-linked lists of pgprocnos
 *
 * See proclist.h for functions that operate on these types.
 *
 * Portions Copyright (c) 2016-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/storage/proclist_types.h
 *-------------------------------------------------------------------------
 */

#ifndef PROCLIST_TYPES_H
#define PROCLIST_TYPES_H

/*
 * 进程双向链表中的一个节点。链接字段保存下一个和上一个进程的、从 0 开始的
 * PGPROC 索引；对于最后一个节点的 next 链接和第一个节点的 prev 链接，
 * 其值为 INVALID_PGPROCNO。当前不在任何链表中的节点应当满足
 * next == prev == 0；而对于处于链表中的节点，这不可能是合法状态，
 * 因为我们不允许出现环形结构。
 */
typedef struct proclist_node
{
	int			next;			/* pgprocno of the next PGPROC */
	int			prev;			/* pgprocno of the prev PGPROC */
} proclist_node;

/*
 * 以 pgprocno 标识的 PGPROC 双向链表的头部。
 * 空链表用 head == tail == INVALID_PGPROCNO 表示。
 */
typedef struct proclist_head
{
	int			head;			/* pgprocno of the head PGPROC */
	int			tail;			/* pgprocno of the tail PGPROC */
} proclist_head;

/*
 * List iterator allowing some modifications while iterating.
 */
typedef struct proclist_mutable_iter
{
	int			cur;			/* pgprocno of the current PGPROC */
	int			next;			/* pgprocno of the next PGPROC */
} proclist_mutable_iter;

#endif							/* PROCLIST_TYPES_H */
