/*-------------------------------------------------------------------------
 *
 * pqsignal.h
 *	  Backend signal(2) support (see also src/port/pqsignal.c)
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/pqsignal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQSIGNAL_H
#define PQSIGNAL_H

#include <signal.h>

#define PG_SETMASK(mask)	sigprocmask(SIG_SETMASK, mask, NULL)

extern sigset_t UnBlockSig,
			BlockSig,
			StartupBlockSig;

extern void pqinitmask(void);

/* pqsigfunc is declared in src/include/port.h */
extern pqsigfunc pqsignal_pm(int signo, pqsigfunc func);

#endif							/* PQSIGNAL_H */
