/*-------------------------------------------------------------------------
 *
 * startup.c
 *
 * The Startup process initialises the server and performs any recovery
 * actions that have been specified. Notice that there is no "main loop"
 * since the Startup process ends as soon as initialisation is complete.
 * (in standby mode, one can think of the replay loop as a main loop,
 * though.)
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/startup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/xlog.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/interrupt.h"
#include "postmaster/startup.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/procsignal.h"
#include "utils/guc.h"
#include "utils/timeout.h"


#ifndef USE_POSTMASTER_DEATH_SIGNAL
/*
 * On systems that need to make a system call to find out if the postmaster has
 * gone away, we'll do so only every Nth call to HandleStartupProcInterrupts().
 * This only affects how long it takes us to detect the condition while we're
 * busy replaying WAL.  Latch waits and similar which should react immediately
 * through the usual techniques.
 */
#define POSTMASTER_POLL_RATE_LIMIT 1024
#endif

/*
 * Flags set by interrupt handlers for later service in the redo loop.
 */
static volatile sig_atomic_t got_SIGHUP = false;
static volatile sig_atomic_t shutdown_requested = false;

/* Signal handlers */
static void StartupProcSigHupHandler(SIGNAL_ARGS);

/* Callbacks */


/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */

/* SIGHUP: set flag to re-read config file at next convenient time */
static void
StartupProcSigHupHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_SIGHUP = true;
	WakeupRecovery();

	errno = save_errno;
}

/* SIGTERM: set flag to abort redo and exit */
static void
StartupProcShutdownHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	shutdown_requested = true;
	WakeupRecovery();

	errno = save_errno;
}

/*
 * Re-read the config file.
 *
 * Streaming replication has been removed, so there is no walreceiver to
 * restart on connection/slot changes; we just reload configuration.
 */
static void
StartupRereadConfig(void)
{
	ProcessConfigFile(PGC_SIGHUP);
}

/* Handle various signals that might be sent to the startup process */
void
HandleStartupProcInterrupts(void)
{
#ifdef POSTMASTER_POLL_RATE_LIMIT
	static uint32 postmaster_poll_count = 0;
#endif

	/*
	 * Process any requests or signals received recently.
	 */
	if (got_SIGHUP)
	{
		got_SIGHUP = false;
		StartupRereadConfig();
	}

	/*
	 * Check if we were requested to exit without finishing recovery.
	 */
	if (shutdown_requested)
		proc_exit(1);

	/*
	 * Emergency bailout if postmaster has died.  This is to avoid the
	 * necessity for manual cleanup of all postmaster children.  Do this less
	 * frequently on systems for which we don't have signals to make that
	 * cheap.
	 */
	if (IsUnderPostmaster &&
#ifdef POSTMASTER_POLL_RATE_LIMIT
		postmaster_poll_count++ % POSTMASTER_POLL_RATE_LIMIT == 0 &&
#endif
		!PostmasterIsAlive())
		exit(1);

	/* Process barrier events */
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();
}


/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */
/* ----------------------------------
 *	Startup Process main entry point
 * ----------------------------------
 */
void
StartupProcessMain(void)
{
	/*
	 * Properly accept or ignore signals the postmaster might send us.
	 */
	pqsignal(SIGHUP, StartupProcSigHupHandler); /* reload config file */
	pqsignal(SIGINT, SIG_IGN);	/* ignore query cancel */
	pqsignal(SIGTERM, StartupProcShutdownHandler);	/* request shutdown */
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	InitializeTimeouts();		/* establishes SIGALRM handler */
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 */
	PG_SETMASK(&UnBlockSig);

	/*
	 * Do what we came for.
	 */
	StartupXLOG();

	/*
	 * Exit normally. Exit code 0 tells postmaster that we completed recovery
	 * successfully.
	 */
	proc_exit(0);
}


