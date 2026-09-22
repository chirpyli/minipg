/*-------------------------------------------------------------------------
 *
 * ilist.c
 *	  support for integrated/inline doubly- and singly- linked lists
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/lib/ilist.c
 *
 * NOTES
 *	  This file only contains functions that are too big to be considered
 *	  for inlining.  See ilist.h for most of the goodies.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/ilist.h"

#ifdef ILIST_DEBUG
/*
 * Verify integrity of a doubly linked list
 */
void
dlist_check(dlist_head *head)
{
	dlist_node *cur;

	if (head == NULL)
		elog(ERROR, "doubly linked list head address is NULL");

	if (head->head.next == NULL && head->head.prev == NULL)
		return;					/* OK, initialized as zeroes */

	/* iterate in forward direction */
	for (cur = head->head.next; cur != &head->head; cur = cur->next)
	{
		if (cur == NULL ||
			cur->next == NULL ||
			cur->prev == NULL ||
			cur->prev->next != cur ||
			cur->next->prev != cur)
			elog(ERROR, "doubly linked list is corrupted");
	}

	/* iterate in backward direction */
	for (cur = head->head.prev; cur != &head->head; cur = cur->prev)
	{
		if (cur == NULL ||
			cur->next == NULL ||
			cur->prev == NULL ||
			cur->prev->next != cur ||
			cur->next->prev != cur)
			elog(ERROR, "doubly linked list is corrupted");
	}
}

/*
 * Verify integrity of a singly linked list
 */
void
slist_check(slist_head *head)
{
	slist_node *cur;

	if (head == NULL)
		elog(ERROR, "singly linked list head address is NULL");

	/*
	 * there isn't much we can test in a singly linked list except that it
	 * actually ends sometime, i.e. hasn't introduced a cycle or similar
	 */
	for (cur = head->head.next; cur != NULL; cur = cur->next)
		;
}

#endif							/* ILIST_DEBUG */
