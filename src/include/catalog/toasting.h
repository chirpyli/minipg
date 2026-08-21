/*-------------------------------------------------------------------------
 *
 * toasting.h
 *	  This file provides some definitions to support creation of toast tables
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/toasting.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TOASTING_H
#define TOASTING_H

#include "storage/lock.h"

/*
 * toasting.c prototypes
 */
extern void NewRelationCreateToastTable(Oid relOid);
extern void NewHeapCreateToastTable(Oid relOid,
									LOCKMODE lockmode, Oid OIDOldToast);
extern void AlterTableCreateToastTable(Oid relOid,
									   LOCKMODE lockmode);
extern void BootstrapToastTable(char *relName,
								Oid toastOid, Oid toastIndexOid);

#endif							/* TOASTING_H */
