/*-------------------------------------------------------------------------
 *
 * postmaster.c
 *	  This program acts as a clearing house for requests to the
 *	  POSTGRES system.  Frontend programs send a startup message
 *	  to the Postmaster and the postmaster uses the info in the
 *	  message to setup a backend process.
 *
 *	  The postmaster also manages system-wide operations such as
 *	  startup and shutdown. The postmaster itself doesn't do those
 *	  operations, mind you --- it just forks off a subprocess to do them
 *	  at the right times.  It also takes care of resetting the system
 *	  if a backend crashes.
 *
 *	  The postmaster process creates the shared memory and semaphore
 *	  pools during startup, but as a rule does not touch them itself.
 *	  In particular, it is not a member of the PGPROC array of backends
 *	  and so it cannot participate in lock-manager operations.  Keeping
 *	  the postmaster away from shared memory operations makes it simpler
 *	  and more reliable.  The postmaster is almost always able to recover
 *	  from crashes of individual backends by resetting shared memory;
 *	  if it did much with shared memory then it would be prone to crashing
 *	  along with the backends.
 *
 *	  When a request message is received, we now fork() immediately.
 *	  The child process performs authentication of the request, and
 *	  then becomes a backend if successful.  This allows the auth code
 *	  to be written in a simple single-threaded style (as opposed to the
 *	  crufty "poor man's multitasking" code that used to be needed).
 *	  More importantly, it ensures that blockages in non-multithreaded
 *	  libraries like SSL or PAM cannot cause denial of service to other
 *	  clients.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/postmaster.c
 *
 * NOTES
 *
 * Initialization:
 *		The Postmaster sets up shared memory data structures
 *		for the backends.
 *
 * Synchronization:
 *		The Postmaster shares memory with the backends but should avoid
 *		touching shared memory, so as not to become stuck if a crashing
 *		backend screws up locks or shared memory.  Likewise, the Postmaster
 *		should never block on messages from frontend clients.
 *
 * Garbage Collection:
 *		The Postmaster cleans up after backends if they have an emergency
 *		exit and/or core dump.
 *
 * Error Reporting:
 *		Use write_stderr() only for reporting "interactive" errors
 *		(essentially, bogus arguments on the command line).  Once the
 *		postmaster is launched, use ereport().
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/param.h>
#include <netdb.h>
#include <limits.h>

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#ifdef HAVE_PTHREAD_IS_THREADED_NP
#include <pthread.h>
#endif

#include "access/transam.h"
#include "access/xlog.h"
#include "bootstrap/bootstrap.h"
#include "catalog/pg_control.h"
#include "common/file_perm.h"
#include "common/ip.h"
#include "common/string.h"
#include "lib/ilist.h"
#include "libpq/auth.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "pg_getopt.h"
#include "utils/backend_status.h"
#include "port/pg_bswap.h"

#include "postmaster/fork_process.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/memutils.h"
#include "utils/pidfile.h"
#include "utils/ps_status.h"
#include "utils/queryjumble.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"


/*
 * List of active backends (or child processes anyway; we don't actually
 * know whether a given child has become a backend or is still in the
 * authorization phase).  This is used mainly to keep track of how many
 * children we have and send them appropriate signals when necessary.
 *
 * Also, "dead_end" children are in it: these are children launched just for
 * the purpose of sending a friendly rejection message to a would-be client.
 * We must track them because they are attached to shared memory, but we know
 * they will never become live backends.  dead_end children are not assigned a
 * PMChildSlot.
 *
 * "Special" children such as the startup, bgwriter
 * tasks are not in this list.  They are tracked via StartupPID and other
 * pid_t variables below.  (Thus, there can't be more than one of any given
 * "special" child process type.  We use BackendList entries for any child
 * process there can be more than one of.)
 */
typedef struct bkend
{
	pid_t		pid;			/* process id of backend */
	int32		cancel_key;		/* cancel key for cancels for this backend */
	int			child_slot;		/* PMChildSlot for this backend, if any */
	bool		dead_end;		/* is it going to send an error and quit? */
	dlist_node	elem;			/* list link in BackendList */
} Backend;

static dlist_head BackendList = DLIST_STATIC_INIT(BackendList);



/* The socket number we are listening for connections on */
int			PostPortNumber;

/* The TCP listen address(es) */
char	   *ListenAddresses;

/* The socket(s) we're listening to. */
#define MAXLISTEN	64
static pgsocket ListenSocket[MAXLISTEN];

/*
 * These globals control the behavior of the postmaster in case some
 * backend dumps core.  Normally, it kills all peers of the dead backend
 * and reinitializes shared memory.  By specifying -s or -n, we can have
 * the postmaster stop (rather than kill) peers and not reinitialize
 * shared data structures.  (Reinit is currently dead code, though.)
 */
static bool Reinit = true;
static int	SendStop = false;

/* still more option variables */

int			PreAuthDelay = 0;
bool		log_hostname;		/* for ps display and logging */
bool		restart_after_crash = true;
bool		remove_temp_files_after_crash = true;

/* PIDs of special child processes; 0 when not running */
static pid_t StartupPID = 0,
			BgWriterPID = 0,
			CheckpointerPID = 0,
			WalWriterPID = 0,
			SysLoggerPID = 0;

/* Startup process's status */
typedef enum
{
	STARTUP_NOT_RUNNING,
	STARTUP_RUNNING,
	STARTUP_SIGNALED,			/* we sent it a SIGQUIT or SIGKILL */
	STARTUP_CRASHED
} StartupStatusEnum;

static StartupStatusEnum StartupStatus = STARTUP_NOT_RUNNING;

/* Startup/shutdown state */
#define			NoShutdown		0
#define			SmartShutdown	1
#define			FastShutdown	2
#define			ImmediateShutdown	3

static int	Shutdown = NoShutdown;

static bool FatalError = false; /* T if recovering from backend crash */

/*
 * We use a simple state machine to control startup, shutdown, and
 * crash recovery (which is rather like shutdown followed by startup).
 *
 * After doing all the postmaster initialization work, we enter PM_STARTUP
 * state and the startup process is launched. The startup process begins by
 * reading the control file and other preliminary initialization steps.
 * In a normal startup, or after crash recovery, the startup process exits
 * with exit code 0 and we switch to PM_RUN state.  However, archive recovery
 * is handled specially since it takes much longer and we would like to support
 * hot standby during archive recovery.
 *
 * When the startup process is ready to start archive recovery, it signals the
 * postmaster, and we switch to PM_RECOVERY state. The background writer and
 * checkpointer are launched, while the startup process continues applying WAL.
 * If Hot Standby is enabled, then, after reaching a consistent point in WAL
 * redo, startup process signals us again, and we switch to PM_HOT_STANDBY
 * state and begin accepting connections to perform read-only queries.  When
 * archive recovery is finished, the startup process exits with exit code 0
 * and we switch to PM_RUN state.
 *
 * Normal child backends can only be launched when we are in PM_RUN or
 * PM_HOT_STANDBY state.  (connsAllowed can also restrict launching.)
 * In other states we handle connection requests by launching "dead_end"
 * child processes, which will simply send the client an error message and
 * quit.  (We track these in the BackendList so that we can know when they
 * are all gone; this is important because they're still connected to shared
 * memory, and would interfere with an attempt to destroy the shmem segment,
 * possibly leading to SHMALL failure when we try to make a new one.)
 * In PM_WAIT_DEAD_END state we are waiting for all the dead_end children
 * to drain out of the system, and therefore stop accepting connection
 * requests at all until the last existing child has quit (which hopefully
 * will not be very long).
 *
 * Notice that this state variable does not distinguish *why* we entered
 * states later than PM_RUN --- Shutdown and FatalError must be consulted
 * to find that out.  FatalError is never true in PM_RECOVERY, PM_HOT_STANDBY,
 * or PM_RUN states, nor in PM_SHUTDOWN states (because we don't enter those
 * states when trying to recover from a crash).  It can be true in PM_STARTUP
 * state, because we don't clear it until we've successfully started WAL redo.
 */
typedef enum
{
	PM_INIT,					/* postmaster starting */
	PM_STARTUP,					/* waiting for startup subprocess */
	PM_RUN,						/* normal "database is alive" state */
	PM_STOP_BACKENDS,			/* need to stop remaining backends */
	PM_WAIT_BACKENDS,			/* waiting for live backends to exit */
	PM_SHUTDOWN,				/* waiting for checkpointer to do shutdown
								 * ckpt */
	PM_SHUTDOWN_2,				/* waiting for walsenders to
								 * finish */
	PM_WAIT_DEAD_END,			/* waiting for dead_end children to exit */
	PM_NO_CHILDREN				/* all important children have exited */
} PMState;

static PMState pmState = PM_INIT;

/*
 * While performing a "smart shutdown", we restrict new connections but stay
 * in PM_RUN or PM_HOT_STANDBY state until all the client backends are gone.
 * connsAllowed is a sub-state indicator showing the active restriction.
 * It is of no interest unless pmState is PM_RUN or PM_HOT_STANDBY.
 */
typedef enum
{
	ALLOW_ALL_CONNS,			/* normal not-shutting-down state */
	ALLOW_SUPERUSER_CONNS,		/* only superusers can connect */
	ALLOW_NO_CONNS				/* no new connections allowed, period */
} ConnsAllowedState;

static ConnsAllowedState connsAllowed = ALLOW_ALL_CONNS;

/* Start time of SIGKILL timeout during immediate shutdown or child crash */
/* Zero means timeout is not running */
static time_t AbortStartTime = 0;

/* Length of said timeout */
#define SIGKILL_CHILDREN_AFTER_SECS		5

static bool ReachedNormalRunning = false;	/* T if we've reached PM_RUN */

bool		ClientAuthInProgress = false;	/* T during new-client
											 * authentication */

bool		redirection_done = false;	/* stderr redirected for syslogger? */


/*
 * postmaster.c - function prototypes
 */
static void CloseServerPorts(int status, Datum arg);

static void getInstallationPaths(const char *argv0);
static void checkControlFile(void);
static Port *ConnCreate(int serverFd);
static void ConnFree(Port *port);
static void reset_shared(void);
static void SIGHUP_handler(SIGNAL_ARGS);
static void pmdie(SIGNAL_ARGS);
static void reaper(SIGNAL_ARGS);
static void sigusr1_handler(SIGNAL_ARGS);
static void process_startup_packet_die(SIGNAL_ARGS);
static void dummy_handler(SIGNAL_ARGS);
static void StartupPacketTimeoutHandler(void);
static void CleanupBackend(int pid, int exitstatus);
static void HandleChildCrash(int pid, int exitstatus, const char *procname);
static void LogChildExit(int lev, const char *procname,
						 int pid, int exitstatus);
static void PostmasterStateMachine(void);
static void BackendInitialize(Port *port);
static void BackendRun(Port *port) pg_attribute_noreturn();
static void ExitPostmaster(int status) pg_attribute_noreturn();
static int	ServerLoop(void);
static int	BackendStartup(Port *port);
static int	ProcessStartupPacket(Port *port, bool ssl_done);
static void SendNegotiateProtocolVersion(List *unrecognized_protocol_options);
static void processCancelRequest(Port *port, void *pkt);
static int	initMasks(fd_set *rmask);
static void report_fork_failure_to_client(Port *port, int errnum);
static CAC_state canAcceptConnections(void);
static bool RandomCancelKey(int32 *cancel_key);
static void signal_child(pid_t pid, int signal);
static bool SignalSomeChildren(int signal);
static void TerminateChildren(int signal);

static int	CountChildren(void);
static bool CreateOptsFile(int argc, char *argv[], char *fullprogname);
static pid_t StartChildProcess(AuxProcType type);
static void InitPostmasterDeathWatchHandle(void);

#define StartupDataBase()		StartChildProcess(StartupProcess)
#define StartBackgroundWriter() StartChildProcess(BgWriterProcess)
#define StartCheckpointer()		StartChildProcess(CheckpointerProcess)
#define StartWalWriter()		StartChildProcess(WalWriterProcess)

/* Macros to check exit status of a child process */
#define EXIT_STATUS_0(st)  ((st) == 0)
#define EXIT_STATUS_1(st)  (WIFEXITED(st) && WEXITSTATUS(st) == 1)
#define EXIT_STATUS_3(st)  (WIFEXITED(st) && WEXITSTATUS(st) == 3)

/*
 * File descriptors for pipe used to monitor if postmaster is alive.
 * First is POSTMASTER_FD_WATCH, second is POSTMASTER_FD_OWN.
 */
int			postmaster_alive_fds[2] = {-1, -1};

/*
 * Postmaster main entry point
 */
void
PostmasterMain(int argc, char *argv[])
{
	int			opt;
	int			status;
	char	   *userDoption = NULL;
	bool		listen_addr_saved = false;
	int			i;
	char	   *output_config_variable = NULL;

	InitProcessGlobals();

	PostmasterPid = MyProcPid;

	IsPostmasterEnvironment = true;

	/*
	 * We should not be creating any files or directories before we check the
	 * data directory (see checkDataDir()), but just in case set the umask to
	 * the most restrictive (owner-only) permissions.
	 *
	 * checkDataDir() will reset the umask based on the data directory
	 * permissions.
	 */
	umask(PG_MODE_MASK_OWNER);

	/*
	 * By default, palloc() requests in the postmaster will be allocated in
	 * the PostmasterContext, which is space that can be recycled by backends.
	 * Allocated data that needs to be available to backends should be
	 * allocated in TopMemoryContext.
	 */
	PostmasterContext = AllocSetContextCreate(TopMemoryContext,
											  "Postmaster",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(PostmasterContext);

	/* Initialize paths to installation files */
	getInstallationPaths(argv[0]);

	/*
	 * Set up signal handlers for the postmaster process.
	 *
	 * In the postmaster, we use pqsignal_pm() rather than pqsignal() (which
	 * is used by all child processes and client processes).  That has a
	 * couple of special behaviors:
	 *
	 * 1. Except on Windows, we tell sigaction() to block all signals for the
	 * duration of the signal handler.  This is faster than our old approach
	 * of blocking/unblocking explicitly in the signal handler, and it should
	 * also prevent excessive stack consumption if signals arrive quickly.
	 *
	 * 2. We do not set the SA_RESTART flag.  This is because signals will be
	 * blocked at all times except when ServerLoop is waiting for something to
	 * happen, and during that window, we want signals to exit the select(2)
	 * wait so that ServerLoop can respond if anything interesting happened.
	 * On some platforms, signals marked SA_RESTART would not cause the
	 * select() wait to end.
	 *
	 * Child processes will generally want SA_RESTART, so pqsignal() sets that
	 * flag.  We expect children to set up their own handlers before
	 * unblocking signals.
	 *
	 * CAUTION: when changing this list, check for side-effects on the signal
	 * handling setup of child processes.  See tcop/postgres.c,
	 * bootstrap/bootstrap.c, postmaster/bgwriter.c, postmaster/walwriter.c,
	 * postmaster/syslogger.c and
	 * postmaster/checkpointer.c.
	 */
	pqinitmask();
	PG_SETMASK(&BlockSig);

	pqsignal_pm(SIGHUP, SIGHUP_handler);	/* reread config file and have
											 * children do same */
	pqsignal_pm(SIGINT, pmdie); /* send SIGTERM and shut down */
	pqsignal_pm(SIGQUIT, pmdie);	/* send SIGQUIT and die */
	pqsignal_pm(SIGTERM, pmdie);	/* wait for children and shut down */
	pqsignal_pm(SIGALRM, SIG_IGN);	/* ignored */
	pqsignal_pm(SIGPIPE, SIG_IGN);	/* ignored */
	pqsignal_pm(SIGUSR1, sigusr1_handler);	/* message from child process */
	pqsignal_pm(SIGUSR2, dummy_handler);	/* unused, reserve for children */
	pqsignal_pm(SIGCHLD, reaper);	/* handle child termination */

#ifdef SIGURG

	/*
	 * Ignore SIGURG for now.  Child processes may change this (see
	 * InitializeLatchSupport), but they will not receive any such signals
	 * until they wait on a latch.
	 */
	pqsignal_pm(SIGURG, SIG_IGN);	/* ignored */
#endif

	/*
	 * No other place in Postgres should touch SIGTTIN/SIGTTOU handling.  We
	 * ignore those signals in a postmaster environment, so that there is no
	 * risk of a child process freezing up due to writing to stderr.  But for
	 * a standalone backend, their default handling is reasonable.  Hence, all
	 * child processes should just allow the inherited settings to stand.
	 */
#ifdef SIGTTIN
	pqsignal_pm(SIGTTIN, SIG_IGN);	/* ignored */
#endif
#ifdef SIGTTOU
	pqsignal_pm(SIGTTOU, SIG_IGN);	/* ignored */
#endif

	/* ignore SIGXFSZ, so that ulimit violations work like disk full */
#ifdef SIGXFSZ
	pqsignal_pm(SIGXFSZ, SIG_IGN);	/* ignored */
#endif

	/*
	 * Options setup
	 */
	InitializeGUCOptions();

	opterr = 1;

	/*
	 * Parse command-line options.  CAUTION: keep this in sync with
	 * tcop/postgres.c (the option sets should not conflict) and with the
	 * common help() function in main/main.c.
	 */
	while ((opt = getopt(argc, argv, "B:bc:C:D:d:EeFf:h:ijN:nOPp:r:S:T:W:-:")) != -1)
	{
		switch (opt)
		{
			case 'B':
				SetConfigOption("shared_buffers", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'b':
				/* Undocumented flag used for binary upgrades */
				IsBinaryUpgrade = true;
				break;

			case 'C':
				output_config_variable = strdup(optarg);
				break;

			case 'D':
				userDoption = strdup(optarg);
				break;

			case 'd':
				set_debug_options(atoi(optarg), PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'e':
				SetConfigOption("datestyle", "euro", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'F':
				SetConfigOption("fsync", "false", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'f':
				if (!set_plan_disabling_options(optarg, PGC_POSTMASTER, PGC_S_ARGV))
				{
					write_stderr("%s: invalid argument for option -f: \"%s\"\n",
								 progname, optarg);
					ExitPostmaster(1);
				}
				break;

			case 'h':
				SetConfigOption("listen_addresses", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'i':
				SetConfigOption("listen_addresses", "*", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'j':
				/* only used by interactive backend */
				break;

			case 'N':
				SetConfigOption("max_connections", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'n':
				/* Don't reinit shared mem after abnormal exit */
				Reinit = false;
				break;

			case 'O':
				SetConfigOption("allow_system_table_mods", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'P':
				SetConfigOption("ignore_system_indexes", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'p':
				SetConfigOption("port", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'r':
				/* only used by single-user backend */
				break;

			case 'S':
				SetConfigOption("work_mem", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;


			case 'T':

				/*
				 * In the event that some backend dumps core, send SIGSTOP,
				 * rather than SIGQUIT, to all its peers.  This lets the wily
				 * post_hacker collect core dumps from everyone.
				 */
				SendStop = true;
				break;


			case 'W':
				SetConfigOption("post_auth_delay", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'c':
			case '-':
				{
					char	   *name,
							   *value;

					ParseLongOption(optarg, &name, &value);
					if (!value)
					{
						if (opt == '-')
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("--%s requires a value",
											optarg)));
						else
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("-c %s requires a value",
											optarg)));
					}

					SetConfigOption(name, value, PGC_POSTMASTER, PGC_S_ARGV);
					free(name);
					if (value)
						free(value);
					break;
				}

			default:
				write_stderr("Try \"%s --help\" for more information.\n",
							 progname);
				ExitPostmaster(1);
		}
	}

	/*
	 * Postmaster accepts no non-option switch arguments.
	 */
	if (optind < argc)
	{
		write_stderr("%s: invalid argument: \"%s\"\n",
					 progname, argv[optind]);
		write_stderr("Try \"%s --help\" for more information.\n",
					 progname);
		ExitPostmaster(1);
	}

	/*
	 * Locate the proper configuration files and data directory, and read
	 * postgresql.conf for the first time.
	 */
	if (!SelectConfigFiles(userDoption, progname))
		ExitPostmaster(2);

	if (output_config_variable != NULL)
	{
		/*
		 * "-C guc" was specified, so print GUC's value and exit.  No extra
		 * permission check is needed because the user is reading inside the
		 * data dir.
		 */
		const char *config_val = GetConfigOption(output_config_variable,
												 false, false);

		puts(config_val ? config_val : "");
		ExitPostmaster(0);
	}

	/* Verify that DataDir looks reasonable */
	checkDataDir();

	/* Check that pg_control exists */
	checkControlFile();

	/* And switch working directory into it */
	ChangeToDataDir();

	/*
	 * Other one-time internal sanity checks can go here, if they are fast.
	 * (Put any slow processing further down, after postmaster.pid creation.)
	 */
	if (!CheckDateTokenTables())
	{
		write_stderr("%s: invalid datetoken tables, please fix\n", progname);
		ExitPostmaster(1);
	}

	/*
	 * Now that we are done processing the postmaster arguments, reset
	 * getopt(3) library so that it will work correctly in subprocesses.
	 */
	optind = 1;

	/*
	 * Create lockfile for data directory.
	 *
	 * We want to do this before we try to grab the input sockets, because the
	 * data directory interlock is more reliable than the socket-file
	 * interlock (thanks to whoever decided to put socket files in /tmp :-().
	 * For the same reason, it's best to grab the TCP socket(s) before the
	 * Unix socket(s).
	 *
	 * Also note that this internally sets up the on_proc_exit function that
	 * is responsible for removing both data directory and socket lockfiles;
	 * so it must happen before opening sockets so that at exit, the socket
	 * lockfiles go away after CloseServerPorts runs.
	 */
	CreateDataDirLockFile(true);

	/*
	 * Read the control file (for error checking and config info).
	 *
	 * Since we verify the control file's CRC, this has a useful side effect
	 * on machines where we need a run-time test for CRC support instructions.
	 * The postmaster will do the test once at startup, and then its child
	 * processes will inherit the correct function pointer and not need to
	 * repeat the test.
	 */
	LocalProcessControlFile(false);

	/*
	 * Calculate MaxBackends.
	 */
	InitializeMaxBackends();

	/*
	 * Set up shared memory and semaphores.
	 */
	reset_shared();

	/*
	 * Estimate number of openable files.  This must happen after setting up
	 * semaphores, because on some platforms semaphores count as open files.
	 */
	set_max_safe_fds();

	/*
	 * Set reference point for stack-depth checking.
	 */
	(void) set_stack_base();

	/*
	 * Initialize pipe (or process handle on Windows) that allows children to
	 * wake up from sleep on postmaster death.
	 */
	InitPostmasterDeathWatchHandle();

	/*
	 * Forcibly remove the files signaling a standby promotion request.
	 * Otherwise, the existence of those files triggers a promotion too early,
	 * whether a user wants that or not.
	 *
	 * This removal of files is usually unnecessary because they can exist
	 * only during a few moments during a standby promotion. However there is
	 * a race condition: if pg_ctl promote is executed and creates the files
	 * during a promotion, the files can stay around even after the server is
	 * brought up to be the primary.  Then, if a new standby starts by using
	 * the backup taken from the new primary, the files can exist at server
	 * startup and must be removed in order to avoid an unexpected promotion.
	 *
	 * Do the same for logrotate signal file
	 */
	RemoveLogrotateSignalFiles();

	/* Remove any outdated file holding the current log filenames. */
	if (unlink(LOG_METAINFO_DATAFILE) < 0 && errno != ENOENT)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m",
						LOG_METAINFO_DATAFILE)));

	/*
	 * Initialize input sockets.
	 *
	 * Mark them all closed, and set up an on_proc_exit function that's
	 * charged with closing the sockets again at postmaster shutdown.
	 */
	for (i = 0; i < MAXLISTEN; i++)
		ListenSocket[i] = PGINVALID_SOCKET;

	on_proc_exit(CloseServerPorts, 0);

	/*
	 * If enabled, start up syslogger collection subprocess
	 */
	SysLoggerPID = SysLogger_Start();

	/*
	 * Reset whereToSendOutput from DestDebug (its starting state) to
	 * DestNone. This stops ereport from sending log messages to stderr unless
	 * Log_destination permits.  We don't do this until the postmaster is
	 * fully launched, since startup failures may as well be reported to
	 * stderr.
	 *
	 * If we are in fact disabling logging to stderr, first emit a log message
	 * saying so, to provide a breadcrumb trail for users who may not remember
	 * that their logging is configured to go somewhere else.
	 */
	if (!(Log_destination & LOG_DESTINATION_STDERR))
		ereport(LOG,
				(errmsg("ending log output to stderr"),
				 errhint("Future log output will go to log destination \"%s\".",
						 Log_destination_string)));

	whereToSendOutput = DestNone;

	/*
	 * Report server startup in log.  While we could emit this much earlier,
	 * it seems best to do so after starting the log collector, if we intend
	 * to use one.
	 */
	ereport(LOG,
			(errmsg("starting %s", PG_VERSION_STR)));

	/*
	 * Establish input sockets.
	 */
	if (ListenAddresses)
	{
		char	   *rawstring;
		List	   *elemlist;
		ListCell   *l;
		int			success = 0;

		/* Need a modifiable copy of ListenAddresses */
		rawstring = pstrdup(ListenAddresses);

		/* Parse string into list of hostnames */
		if (!SplitGUCList(rawstring, ',', &elemlist))
		{
			/* syntax error in list */
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid list syntax in parameter \"%s\"",
							"listen_addresses")));
		}

		foreach(l, elemlist)
		{
			char	   *curhost = (char *) lfirst(l);

			if (strcmp(curhost, "*") == 0)
				status = StreamServerPort(AF_INET, NULL,
										  (unsigned short) PostPortNumber,
										  NULL,
										  ListenSocket, MAXLISTEN);
			else
				status = StreamServerPort(AF_INET, curhost,
										  (unsigned short) PostPortNumber,
										  NULL,
										  ListenSocket, MAXLISTEN);

			if (status == STATUS_OK)
			{
				success++;
				/* record the first successful host addr in lockfile */
				if (!listen_addr_saved)
				{
					AddToDataDirLockFile(LOCK_FILE_LINE_LISTEN_ADDR, curhost);
					listen_addr_saved = true;
				}
			}
			else
				ereport(WARNING,
						(errmsg("could not create listen socket for \"%s\"",
								curhost)));
		}

		if (!success && elemlist != NIL)
			ereport(FATAL,
					(errmsg("could not create any TCP/IP sockets")));

		list_free(elemlist);
		pfree(rawstring);
	}

	/*
	 * check that we have some socket to listen on
	 */
	if (ListenSocket[0] == PGINVALID_SOCKET)
		ereport(FATAL,
				(errmsg("no socket created for listening")));

	/*
	 * If no valid TCP ports, write an empty line for listen address,
	 * indicating the Unix socket must be used.  Note that this line is not
	 * added to the lock file until there is a socket backing it.
	 */
	if (!listen_addr_saved)
		AddToDataDirLockFile(LOCK_FILE_LINE_LISTEN_ADDR, "");

	/*
	 * Record postmaster options.  We delay this till now to avoid recording
	 * bogus options (eg, unusable port number).
	 */
	if (!CreateOptsFile(argc, argv, my_exec_path))
		ExitPostmaster(1);

	/*
	 * Remove old temporary files.  At this point there can be no other
	 * Postgres processes running in this directory, so this should be safe.
	 */
	RemovePgTempFiles();

	/*
	 * Remember postmaster startup time
	 */
	PgStartTime = GetCurrentTimestamp();

	/*
	 * Report postmaster status in the postmaster.pid file, to allow pg_ctl to
	 * see what's happening.
	 */
	AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STARTING);

	/*
	 * We're ready to rock and roll...
	 */
	StartupPID = StartupDataBase();
	Assert(StartupPID != 0);
	StartupStatus = STARTUP_RUNNING;
	pmState = PM_STARTUP;

	status = ServerLoop();

	/*
	 * ServerLoop probably shouldn't ever return, but if it does, close down.
	 */
	ExitPostmaster(status != STATUS_OK);

	abort();					/* not reached */
}


/*
 * on_proc_exit callback to close server's listen sockets
 */
static void
CloseServerPorts(int status, Datum arg)
{
	int			i;

	/*
	 * First, explicitly close all the socket FDs.  We used to just let this
	 * happen implicitly at postmaster exit, but it's better to close them
	 * before we remove the postmaster.pid lockfile; otherwise there's a race
	 * condition if a new postmaster wants to re-use the TCP port number.
	 */
	for (i = 0; i < MAXLISTEN; i++)
	{
		if (ListenSocket[i] != PGINVALID_SOCKET)
		{
			StreamClose(ListenSocket[i]);
			ListenSocket[i] = PGINVALID_SOCKET;
		}
	}

	/*
	 * Next, remove any filesystem entries for Unix sockets.  To avoid race
	 * conditions against incoming postmasters, this must happen after closing
	 * the sockets and before removing lock files.
	 */
	RemoveSocketFiles();

	/*
	 * We don't do anything about socket lock files here; those will be
	 * removed in a later on_proc_exit callback.
	 */
}

/*
 * Compute and check the directory paths to files that are part of the
 * installation (as deduced from the postgres executable's own location)
 */
static void
getInstallationPaths(const char *argv0)
{
	DIR		   *pdir;

	/* Locate the postgres executable itself */
	if (find_my_exec(argv0, my_exec_path) < 0)
		ereport(FATAL,
				(errmsg("%s: could not locate my own executable path", argv0)));

	/*
	 * Locate the pkglib directory --- this has to be set early in case we try
	 * to load any modules from it in response to postgresql.conf entries.
	 */
	get_pkglib_path(my_exec_path, pkglib_path);

	/*
	 * Verify that there's a readable directory there; otherwise the Postgres
	 * installation is incomplete or corrupt.  (A typical cause of this
	 * failure is that the postgres executable has been moved or hardlinked to
	 * some directory that's not a sibling of the installation lib/
	 * directory.)
	 */
	pdir = AllocateDir(pkglib_path);
	if (pdir == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						pkglib_path),
				 errhint("This may indicate an incomplete PostgreSQL installation, or that the file \"%s\" has been moved away from its proper location.",
						 my_exec_path)));
	FreeDir(pdir);

	/*
	 * XXX is it worth similarly checking the share/ directory?  If the lib/
	 * directory is there, then share/ probably is too.
	 */
}

/*
 * Check that pg_control exists in the correct location in the data directory.
 *
 * No attempt is made to validate the contents of pg_control here.  This is
 * just a sanity check to see if we are looking at a real data directory.
 */
static void
checkControlFile(void)
{
	char		path[MAXPGPATH];
	FILE	   *fp;

	snprintf(path, sizeof(path), "%s/global/pg_control", DataDir);

	fp = AllocateFile(path, PG_BINARY_R);
	if (fp == NULL)
	{
		write_stderr("%s: could not find the database system\n"
					 "Expected to find it in the directory \"%s\",\n"
					 "but could not open file \"%s\": %s\n",
					 progname, DataDir, path, strerror(errno));
		ExitPostmaster(2);
	}
	FreeFile(fp);
}

/*
 * Determine how long should we let ServerLoop sleep.
 *
 * In normal conditions we wait at most one minute, to ensure that the other
 * background tasks handled by ServerLoop get done even when no requests are
 * arriving.  However, if there are background workers waiting to be started,
 * we don't actually sleep so that they are quickly serviced.  Other exception
 * cases are as shown in the code.
 */
static void
DetermineSleepTime(struct timeval *timeout)
{
	if (AbortStartTime != 0)
	{
		/* time left to abort; clamp to 0 in case it already expired */
		timeout->tv_sec = SIGKILL_CHILDREN_AFTER_SECS -
			(time(NULL) - AbortStartTime);
		timeout->tv_sec = Max(timeout->tv_sec, 0);
		timeout->tv_usec = 0;
	}
	else
	{
		timeout->tv_sec = 60;
		timeout->tv_usec = 0;
	}
}

/*
 * Main idle loop of postmaster
 *
 * NB: Needs to be called with signals blocked
 */
static int
ServerLoop(void)
{
	fd_set		readmask;
	int			nSockets;
	time_t		last_lockfile_recheck_time,
				last_touch_time;

	last_lockfile_recheck_time = last_touch_time = time(NULL);

	nSockets = initMasks(&readmask);

	for (;;)
	{
		fd_set		rmask;
		int			selres;
		time_t		now;

		/*
		 * Wait for a connection request to arrive.
		 *
		 * We block all signals except while sleeping. That makes it safe for
		 * signal handlers, which again block all signals while executing, to
		 * do nontrivial work.
		 *
		 * If we are in PM_WAIT_DEAD_END state, then we don't want to accept
		 * any new connections, so we don't call select(), and just sleep.
		 */
		memcpy((char *) &rmask, (char *) &readmask, sizeof(fd_set));

		if (pmState == PM_WAIT_DEAD_END)
		{
			PG_SETMASK(&UnBlockSig);

			pg_usleep(100000L); /* 100 msec seems reasonable */
			selres = 0;

			PG_SETMASK(&BlockSig);
		}
		else
		{
			/* must set timeout each time; some OSes change it! */
			struct timeval timeout;

			/* Needs to run with blocked signals! */
			DetermineSleepTime(&timeout);

			PG_SETMASK(&UnBlockSig);

			selres = select(nSockets, &rmask, NULL, NULL, &timeout);

			PG_SETMASK(&BlockSig);
		}

		/* Now check the select() result */
		if (selres < 0)
		{
			if (errno != EINTR && errno != EWOULDBLOCK)
			{
				ereport(LOG,
						(errcode_for_socket_access(),
						 errmsg("select() failed in postmaster: %m")));
				return STATUS_ERROR;
			}
		}

		/*
		 * New connection pending on any of our sockets? If so, fork a child
		 * process to deal with it.
		 */
		if (selres > 0)
		{
			int			i;

			for (i = 0; i < MAXLISTEN; i++)
			{
				if (ListenSocket[i] == PGINVALID_SOCKET)
					break;
				if (FD_ISSET(ListenSocket[i], &rmask))
				{
					Port	   *port;

					port = ConnCreate(ListenSocket[i]);
					if (port)
					{
						BackendStartup(port);

						/*
						 * We no longer need the open socket or port structure
						 * in this process
						 */
						StreamClose(port->sock);
						ConnFree(port);
					}
				}
			}
		}

		/* If we have lost the log collector, try to start a new one */
		if (SysLoggerPID == 0 && Logging_collector)
			SysLoggerPID = SysLogger_Start();

		/*
		 * If no background writer process is running, and we are not in a
		 * state that prevents it, start one.  It doesn't matter if this
		 * fails, we'll just try again later.  Likewise for the checkpointer.
		 */
		if (pmState == PM_RUN)
		{
			if (CheckpointerPID == 0)
				CheckpointerPID = StartCheckpointer();
			if (BgWriterPID == 0)
				BgWriterPID = StartBackgroundWriter();
		}

		/*
		 * Likewise, if we have lost the walwriter process, try to start a new
		 * one.  But this is needed only in normal operation (else we cannot
		 * be writing any new WAL).
		 */
		if (WalWriterPID == 0 && pmState == PM_RUN)
			WalWriterPID = StartWalWriter();

		/*
		 * Lastly, check to see if it's time to do some things that we don't
		 * want to do every single time through the loop, because they're a
		 * bit expensive.  Note that there's up to a minute of slop in when
		 * these tasks will be performed, since DetermineSleepTime() will let
		 * us sleep at most that long; except for SIGKILL timeout which has
		 * special-case logic there.
		 */
		now = time(NULL);

		/*
		 * If we already sent SIGQUIT to children and they are slow to shut
		 * down, it's time to send them SIGKILL.  This doesn't happen
		 * normally, but under certain conditions backends can get stuck while
		 * shutting down.  This is a last measure to get them unwedged.
		 *
		 * Note we also do this during recovery from a process crash.
		 */
		if ((Shutdown >= ImmediateShutdown || (FatalError && !SendStop)) &&
			AbortStartTime != 0 &&
			(now - AbortStartTime) >= SIGKILL_CHILDREN_AFTER_SECS)
		{
			/* We were gentle with them before. Not anymore */
			ereport(LOG,
					(errmsg("issuing SIGKILL to recalcitrant children")));
			TerminateChildren(SIGKILL);
			/* reset flag so we don't SIGKILL again */
			AbortStartTime = 0;
		}

		/*
		 * Once a minute, verify that postmaster.pid hasn't been removed or
		 * overwritten.  If it has, we force a shutdown.  This avoids having
		 * postmasters and child processes hanging around after their database
		 * is gone, and maybe causing problems if a new database cluster is
		 * created in the same place.  It also provides some protection
		 * against a DBA foolishly removing postmaster.pid and manually
		 * starting a new postmaster.  Data corruption is likely to ensue from
		 * that anyway, but we can minimize the damage by aborting ASAP.
		 */
		if (now - last_lockfile_recheck_time >= 1 * SECS_PER_MINUTE)
		{
			if (!RecheckDataDirLockFile())
			{
				ereport(LOG,
						(errmsg("performing immediate shutdown because data directory lock file is invalid")));
				kill(MyProcPid, SIGQUIT);
			}
			last_lockfile_recheck_time = now;
		}

		/*
		 * Touch Unix socket and lock files every 58 minutes, to ensure that
		 * they are not removed by overzealous /tmp-cleaning tasks.  We assume
		 * no one runs cleaners with cutoff times of less than an hour ...
		 */
		if (now - last_touch_time >= 58 * SECS_PER_MINUTE)
		{
			TouchSocketFiles();
			TouchSocketLockFiles();
			last_touch_time = now;
		}
	}
}

/*
 * Initialise the masks for select() for the ports we are listening on.
 * Return the number of sockets to listen on.
 */
static int
initMasks(fd_set *rmask)
{
	int			maxsock = -1;
	int			i;

	FD_ZERO(rmask);

	for (i = 0; i < MAXLISTEN; i++)
	{
		int			fd = ListenSocket[i];

		if (fd == PGINVALID_SOCKET)
			break;
		FD_SET(fd, rmask);

		if (fd > maxsock)
			maxsock = fd;
	}

	return maxsock + 1;
}

/*
 * postgres启动包：
 * Startup包是客户端连接到PostgreSQL服务器时发送的第一个包，用于通知服务器客户端的协议版本和请求的数据库等信息，
 * 它是libpq协议中唯一一个没有消息类型标识符的包，直接以4字节的开头表示包的长度（4字节）。（因为第一个包的特殊性，可以没有消息类型）。
 * 如果是非SSL连接，它的格式如下：
+---------+---------+-----------------------+-------------------+---+
| Length  | Protocol| Key/Value String Pairs| Terminator (\0)   |
| (4字节) | (4字节) | (变长，以\0结尾的C字符串)| (1字节)           |
+---------+---------+-----------------------+-------------------+---+
* 如果是SSL连接，则格式不同，第一个包是一个极小的SSL探测包（8 字节的魔数探测包（00000008 + 1234,5679）），需要进行SSL握手。
* 其中Key/Value String Pairs是一个变长的字符串对列表，每个字符串对以\0结尾。先发参数名，再发参数值。都已\0结尾。
* 必需包含的参数：
	- user: 连接数据库的用户名
	- database： 要连接的数据库名，如果不发，默认等于用户名。
* 可选参数：
	- application_name: 应用名称，例如walreceiver等。
*/

/*
 * Read a client's startup packet and do something according to it.
 *
 * Returns STATUS_OK or STATUS_ERROR, or might call ereport(FATAL) and
 * not return at all.
 *
 * (Note that ereport(FATAL) stuff is sent to the client, so only use it
 * if that's what you want.  Return STATUS_ERROR if you don't want to
 * send anything to the client, which would typically be appropriate
 * if we detect a communications failure.)
 *
 * Set ssl_done when negotiation of an encrypted layer (TLS) is completed.
 */
static int
ProcessStartupPacket(Port *port, bool ssl_done)
{
	int32		len;
	char	   *buf;
	ProtocolVersion proto;
	MemoryContext oldcontext;

	pq_startmsgread();

	/*
	 * Grab the first byte of the length word separately, so that we can tell
	 * whether we have no data at all or an incomplete packet.  (This might
	 * sound inefficient, but it's not really, because of buffering in
	 * pqcomm.c.)
	 */
	if (pq_getbytes((char *) &len, 1) == EOF)
	{
		/*
		 * If we get no data at all, don't clutter the log with a complaint;
		 * such cases often occur for legitimate reasons.   Service-monitoring software also often just opens and
		 * closes a connection without sending anything.  (So do port
		 * scanners, which may be less benign, but it's not really our job to
		 * notice those.)
		 */
		return STATUS_ERROR;
	}

	if (pq_getbytes(((char *) &len) + 1, 3) == EOF)
	{
		/* Got a partial length word, so bleat about that */
		if (!ssl_done)
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("incomplete startup packet")));
		return STATUS_ERROR;
	}

	len = pg_ntoh32(len);
	len -= 4;

	if (len < (int32) sizeof(ProtocolVersion) ||
		len > MAX_STARTUP_PACKET_LENGTH)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid length of startup packet")));
		return STATUS_ERROR;
	}

	/*
	 * Allocate space to hold the startup packet, plus one extra byte that's
	 * initialized to be zero.  This ensures we will have null termination of
	 * all strings inside the packet.
	 */
	buf = palloc(len + 1);
	buf[len] = '\0';

	if (pq_getbytes(buf, len) == EOF)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("incomplete startup packet")));
		return STATUS_ERROR;
	}
	pq_endmsgread();

	/*
	 * The first field is either a protocol version number or a special
	 * request code.
	 */
	port->proto = proto = pg_ntoh32(*((ProtocolVersion *) buf));

	if (proto == CANCEL_REQUEST_CODE)
	{
		if (len != sizeof(CancelRequestPacket))
		{
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid length of startup packet")));
			return STATUS_ERROR;
		}
		processCancelRequest(port, buf);
		/* Not really an error, but we don't want to proceed further */
		return STATUS_ERROR;
	}

	/* Could add additional special packet types here */

	/*
	 * Set FrontendProtocol now so that ereport() knows what format to send if
	 * we fail during startup.
	 */
	FrontendProtocol = proto;

	/* Check that the major protocol version is in range. */
	if (PG_PROTOCOL_MAJOR(proto) < PG_PROTOCOL_MAJOR(PG_PROTOCOL_EARLIEST) ||
		PG_PROTOCOL_MAJOR(proto) > PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST))
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unsupported frontend protocol %u.%u: server supports %u.0 to %u.%u",
						PG_PROTOCOL_MAJOR(proto), PG_PROTOCOL_MINOR(proto),
						PG_PROTOCOL_MAJOR(PG_PROTOCOL_EARLIEST),
						PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST),
						PG_PROTOCOL_MINOR(PG_PROTOCOL_LATEST))));

	/*
	 * Now fetch parameters out of startup packet and save them into the Port
	 * structure.  All data structures attached to the Port struct must be
	 * allocated in TopMemoryContext so that they will remain available in a
	 * running backend (even after PostmasterContext is destroyed).  We need
	 * not worry about leaking this storage on failure, since we aren't in the
	 * postmaster process anymore.
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	/* Handle protocol version 3 startup packet */
	{
		int32		offset = sizeof(ProtocolVersion);
		List	   *unrecognized_protocol_options = NIL;

		/*
		 * Scan packet body for name/option pairs.  We can assume any string
		 * beginning within the packet body is null-terminated, thanks to
		 * zeroing extra byte above.
		 */
		port->guc_options = NIL;

		while (offset < len)
		{
			char	   *nameptr = buf + offset;
			int32		valoffset;
			char	   *valptr;

			if (*nameptr == '\0')
				break;			/* found packet terminator */
			valoffset = offset + strlen(nameptr) + 1;
			if (valoffset >= len)
				break;			/* missing value, will complain below */
			valptr = buf + valoffset;

			if (strcmp(nameptr, "database") == 0)
				port->database_name = pstrdup(valptr);
			else if (strcmp(nameptr, "user") == 0)
				port->user_name = pstrdup(valptr);
			else if (strcmp(nameptr, "options") == 0)
				port->cmdline_options = pstrdup(valptr);
			else if (strncmp(nameptr, "_pq_.", 5) == 0)
			{
				/*
				 * Any option beginning with _pq_. is reserved for use as a
				 * protocol-level option, but at present no such options are
				 * defined.
				 */
				unrecognized_protocol_options =
					lappend(unrecognized_protocol_options, pstrdup(nameptr));
			}
			else
			{
				/* Assume it's a generic GUC option */
				port->guc_options = lappend(port->guc_options,
											pstrdup(nameptr));
				port->guc_options = lappend(port->guc_options,
											pstrdup(valptr));

				/*
				 * Copy application_name to port if we come across it.  This
				 * is done so we can log the application_name in the
				 * connection authorization message.  Note that the GUC would
				 * be used but we haven't gone through GUC setup yet.
				 */
				if (strcmp(nameptr, "application_name") == 0)
				{
					char	   *tmp_app_name = pstrdup(valptr);

					pg_clean_ascii(tmp_app_name);

					port->application_name = tmp_app_name;
				}
			}
			offset = valoffset + strlen(valptr) + 1;
		}

		/*
		 * If we didn't find a packet terminator exactly at the end of the
		 * given packet length, complain.
		 */
		if (offset != len - 1)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid startup packet layout: expected terminator as last byte")));

		/*
		 * If the client requested a newer protocol version or if the client
		 * requested any protocol options we didn't recognize, let them know
		 * the newest minor protocol version we do support and the names of
		 * any unrecognized options.
		 */
		if (PG_PROTOCOL_MINOR(proto) > PG_PROTOCOL_MINOR(PG_PROTOCOL_LATEST) ||
			unrecognized_protocol_options != NIL)
			SendNegotiateProtocolVersion(unrecognized_protocol_options);
	}

	/* Check a user name was given. */
	if (port->user_name == NULL || port->user_name[0] == '\0')
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
				 errmsg("no PostgreSQL user name specified in startup packet")));

	/* The database defaults to the user name. */
	if (port->database_name == NULL || port->database_name[0] == '\0')
		port->database_name = pstrdup(port->user_name);

	/*
	 * Truncate given database and user names to length of a Postgres name.
	 * This avoids lookup failures when overlength names are given.
	 */
	if (strlen(port->database_name) >= NAMEDATALEN)
		port->database_name[NAMEDATALEN - 1] = '\0';
	if (strlen(port->user_name) >= NAMEDATALEN)
		port->user_name[NAMEDATALEN - 1] = '\0';

	MyBackendType = B_BACKEND;

	/*
	 * Done putting stuff in TopMemoryContext.
	 */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * If we're going to reject the connection due to database state, say so
	 * now instead of wasting cycles on an authentication exchange. (This also
	 * allows a pg_ping utility to be written.)
	 */
	switch (port->canAcceptConnections)
	{
		case CAC_STARTUP:
			ereport(FATAL,
					(errcode(ERRCODE_CANNOT_CONNECT_NOW),
					 errmsg("the database system is starting up")));
			break;
		case CAC_NOTCONSISTENT:
			if (EnableHotStandby)
				ereport(FATAL,
						(errcode(ERRCODE_CANNOT_CONNECT_NOW),
						 errmsg("the database system is not yet accepting connections"),
						 errdetail("Consistent recovery state has not been yet reached.")));
			else
				ereport(FATAL,
						(errcode(ERRCODE_CANNOT_CONNECT_NOW),
						 errmsg("the database system is not accepting connections"),
						 errdetail("Hot standby mode is disabled.")));
			break;
		case CAC_SHUTDOWN:
			ereport(FATAL,
					(errcode(ERRCODE_CANNOT_CONNECT_NOW),
					 errmsg("the database system is shutting down")));
			break;
		case CAC_RECOVERY:
			ereport(FATAL,
					(errcode(ERRCODE_CANNOT_CONNECT_NOW),
					 errmsg("the database system is in recovery mode")));
			break;
		case CAC_TOOMANY:
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("sorry, too many clients already")));
			break;
		case CAC_SUPERUSER:
			/* OK for now, will check in InitPostgres */
			break;
		case CAC_OK:
			break;
	}

	return STATUS_OK;
}

/*
 * Send a NegotiateProtocolVersion to the client.  This lets the client know
 * that they have requested a newer minor protocol version than we are able
 * to speak.  We'll speak the highest version we know about; the client can,
 * of course, abandon the connection if that's a problem.
 *
 * We also include in the response a list of protocol options we didn't
 * understand.  This allows clients to include optional parameters that might
 * be present either in newer protocol versions or third-party protocol
 * extensions without fear of having to reconnect if those options are not
 * understood, while at the same time making certain that the client is aware
 * of which options were actually accepted.
 */
static void
SendNegotiateProtocolVersion(List *unrecognized_protocol_options)
{
	StringInfoData buf;
	ListCell   *lc;

	pq_beginmessage(&buf, 'v'); /* NegotiateProtocolVersion */
	pq_sendint32(&buf, PG_PROTOCOL_LATEST);
	pq_sendint32(&buf, list_length(unrecognized_protocol_options));
	foreach(lc, unrecognized_protocol_options)
		pq_sendstring(&buf, lfirst(lc));
	pq_endmessage(&buf);

	/* no need to flush, some other message will follow */
}

/*
 * The client has sent a cancel request packet, not a normal
 * start-a-new-connection packet.  Perform the necessary processing.
 * Nothing is sent back to the client.
 */
static void
processCancelRequest(Port *port, void *pkt)
{
	CancelRequestPacket *canc = (CancelRequestPacket *) pkt;
	int			backendPID;
	int32		cancelAuthCode;
	Backend    *bp;

	dlist_iter	iter;

	backendPID = (int) pg_ntoh32(canc->backendPID);
	cancelAuthCode = (int32) pg_ntoh32(canc->cancelAuthCode);

	/*
	 * See if we have a matching backend.
	 */
	dlist_foreach(iter, &BackendList)
	{
		bp = dlist_container(Backend, elem, iter.cur);
		if (bp->pid == backendPID)
		{
			if (bp->cancel_key == cancelAuthCode)
			{
				/* Found a match; signal that backend to cancel current op */
				ereport(DEBUG2,
						(errmsg_internal("processing cancel request: sending SIGINT to process %d",
										 backendPID)));
				signal_child(bp->pid, SIGINT);
			}
			else
				/* Right PID, wrong key: no way, Jose */
				ereport(LOG,
						(errmsg("wrong key in cancel request for process %d",
								backendPID)));
			return;
		}
	}

	/* No matching backend */
	ereport(LOG,
			(errmsg("PID %d in cancel request did not match any process",
					backendPID)));
}

/*
 * canAcceptConnections --- check to see if database state allows connections
 * of the specified type.  (Note that we don't yet know whether a NORMAL
 * connection might turn into a walsender.)
 */
static CAC_state
canAcceptConnections(void)
{
	CAC_state	result = CAC_OK;

	/*
	 * Can't start backends when in startup/shutdown/inconsistent recovery
	 * state.
	 */
	if (pmState != PM_RUN)
	{
		if (Shutdown > NoShutdown)
			return CAC_SHUTDOWN;	/* shutdown is pending */
		else if (!FatalError && pmState == PM_STARTUP)
			return CAC_STARTUP; /* normal startup */
		else
			return CAC_RECOVERY;	/* else must be crash recovery */
	}

	/*
	 * "Smart shutdown" restrictions are applied only to normal connections.
	 * When only superusers can connect, we return CAC_SUPERUSER to indicate
	 * that superuserness must be checked later.  Note that neither CAC_OK
	 * nor CAC_SUPERUSER can safely be returned until we have checked for
	 * too many children.
	 */
	if (connsAllowed != ALLOW_ALL_CONNS)
	{
		if (connsAllowed == ALLOW_SUPERUSER_CONNS)
			result = CAC_SUPERUSER; /* allow superusers only */
		else
			return CAC_SHUTDOWN;	/* shutdown is pending */
	}

	/*
	 * Don't start too many children.
	 *
	 * We allow more connections here than we can have backends because some
	 * might still be authenticating; they might fail auth, or some existing
	 * backend might exit before the auth cycle is completed.  The exact
	 * MaxBackends limit is enforced when a new backend tries to join the
	 * shared-inval backend array.
	 *
	 * The limit here must match the sizes of the per-child-process arrays;
	 * see comments for MaxLivePostmasterChildren().
	 */
	if (CountChildren() >= MaxLivePostmasterChildren())
		result = CAC_TOOMANY;

	return result;
}


/*
 * ConnCreate -- create a local connection data structure
 *
 * Returns NULL on failure, other than out-of-memory which is fatal.
 */
static Port *
ConnCreate(int serverFd)
{
	Port	   *port;

	if (!(port = (Port *) calloc(1, sizeof(Port))))
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		ExitPostmaster(1);
	}

	if (StreamConnection(serverFd, port) != STATUS_OK)
	{
		if (port->sock != PGINVALID_SOCKET)
			StreamClose(port->sock);
		ConnFree(port);
		return NULL;
	}

	return port;
}


/*
 * ConnFree -- free a local connection data structure
 *
 * Caller has already closed the socket if any, so there's not much
 * to do here.
 */
static void
ConnFree(Port *conn)
{
	free(conn);
}


/*
 * ClosePostmasterPorts -- close all the postmaster's open sockets
 *
 * This is called during child process startup to release file descriptors
 * that are not needed by that child process.  The postmaster still has
 * them open, of course.
 *
 * Note: we pass am_syslogger as a boolean because we don't want to set
 * the global variable yet when this is called.
 */
void
ClosePostmasterPorts(bool am_syslogger)
{
	int			i;

	/*
	 * Close the write end of postmaster death watch pipe. It's important to
	 * do this as early as possible, so that if postmaster dies, others won't
	 * think that it's still running because we're holding the pipe open.
	 */
	if (close(postmaster_alive_fds[POSTMASTER_FD_OWN]) != 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg_internal("could not close postmaster death monitoring pipe in child process: %m")));
	postmaster_alive_fds[POSTMASTER_FD_OWN] = -1;
	/* Notify fd.c that we released one pipe FD. */
	ReleaseExternalFD();

	/*
	 * Close the postmaster's listen sockets.  These aren't tracked by fd.c,
	 * so we don't call ReleaseExternalFD() here.
	 */
	for (i = 0; i < MAXLISTEN; i++)
	{
		if (ListenSocket[i] != PGINVALID_SOCKET)
		{
			StreamClose(ListenSocket[i]);
			ListenSocket[i] = PGINVALID_SOCKET;
		}
	}

	/*
	 * If using syslogger, close the read side of the pipe.  We don't bother
	 * tracking this in fd.c, either.
	 */
	if (!am_syslogger)
	{
		if (syslogPipe[0] >= 0)
			close(syslogPipe[0]);
		syslogPipe[0] = -1;
	}

}


/*
 * InitProcessGlobals -- set MyStartTime[stamp], random seeds
 *
 * Called early in the postmaster and every backend.
 */
void
InitProcessGlobals(void)
{
	unsigned int rseed;

	MyStartTimestamp = GetCurrentTimestamp();
	MyStartTime = timestamptz_to_time_t(MyStartTimestamp);

	/*
	 * Set a different seed for random() in every process.  We want something
	 * unpredictable, so if possible, use high-quality random bits for the
	 * seed.  Otherwise, fall back to a seed based on timestamp and PID.
	 */
	if (!pg_strong_random(&rseed, sizeof(rseed)))
	{
		/*
		 * Since PIDs and timestamps tend to change more frequently in their
		 * least significant bits, shift the timestamp left to allow a larger
		 * total number of seeds in a given time period.  Since that would
		 * leave only 20 bits of the timestamp that cycle every ~1 second,
		 * also mix in some higher bits.
		 */
		rseed = ((uint64) MyProcPid) ^
			((uint64) MyStartTimestamp << 12) ^
			((uint64) MyStartTimestamp >> 20);
	}
	srandom(rseed);
}


/*
 * reset_shared -- reset shared memory and semaphores
 */
static void
reset_shared(void)
{
	/*
	 * Create or re-create shared memory and semaphores.
	 *
	 * Note: in each "cycle of life" we will normally assign the same IPC keys
	 * (if using SysV shmem and/or semas).  This helps ensure that we will
	 * clean up dead IPC objects if the postmaster crashes and is restarted.
	 */
	CreateSharedMemoryAndSemaphores();
}


/*
 * SIGHUP -- reread config files, and tell children to do same
 */
static void
SIGHUP_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/*
	 * We rely on the signal mechanism to have blocked all signals.
	 */

	if (Shutdown <= SmartShutdown)
	{
		ereport(LOG,
				(errmsg("received SIGHUP, reloading configuration files")));
		ProcessConfigFile(PGC_SIGHUP);
		SignalSomeChildren(SIGHUP);
		if (StartupPID != 0)
			signal_child(StartupPID, SIGHUP);
		if (BgWriterPID != 0)
			signal_child(BgWriterPID, SIGHUP);
		if (CheckpointerPID != 0)
			signal_child(CheckpointerPID, SIGHUP);
		if (WalWriterPID != 0)
			signal_child(WalWriterPID, SIGHUP);
		if (SysLoggerPID != 0)
			signal_child(SysLoggerPID, SIGHUP);

		/* minipg: pg_hba.conf / pg_ident.conf 已移除，无需重载认证配置文件 */
	}

	errno = save_errno;
}


/*
 * pmdie -- signal handler for processing various postmaster signals.
 */
static void
pmdie(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/*
	 * We rely on the signal mechanism to have blocked all signals.
	 */

	ereport(DEBUG2,
			(errmsg_internal("postmaster received signal %d",
							 postgres_signal_arg)));

	switch (postgres_signal_arg)
	{
		case SIGTERM:

			/*
			 * Smart Shutdown:
			 *
			 * Wait for children to end their work, then shut down.
			 */
			if (Shutdown >= SmartShutdown)
				break;
			Shutdown = SmartShutdown;
			ereport(LOG,
					(errmsg("received smart shutdown request")));

			/* Report status */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);

			/*
			 * If we reached normal running, we have to wait for any online
			 * backup mode to end; otherwise go straight to waiting for client
			 * backends to exit.  (The difference is that in the former state,
			 * we'll still let in new superuser clients, so that somebody can
			 * end the online backup mode.)  If already in PM_STOP_BACKENDS or
			 * a later state, do not change it.
			 */
			if (pmState == PM_RUN)
				connsAllowed = ALLOW_SUPERUSER_CONNS;
			else if (pmState == PM_STARTUP)
			{
				/* There should be no clients, so proceed to stop children */
				pmState = PM_STOP_BACKENDS;
			}

			/*
			 * Now wait for online backup mode to end and backends to exit. If
			 * that is already the case, PostmasterStateMachine will take the
			 * next step.
			 */
			PostmasterStateMachine();
			break;

		case SIGINT:

			/*
			 * Fast Shutdown:
			 *
			 * Abort all children with SIGTERM (rollback active transactions
			 * and exit) and shut down when they are gone.
			 */
			if (Shutdown >= FastShutdown)
				break;
			Shutdown = FastShutdown;
			ereport(LOG,
					(errmsg("received fast shutdown request")));

			/* Report status */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);

			if (pmState == PM_STARTUP)
			{
				/* Just shut down background processes silently */
				pmState = PM_STOP_BACKENDS;
			}
			else if (pmState == PM_RUN)
			{
				/* Report that we're about to zap live client sessions */
				ereport(LOG,
						(errmsg("aborting any active transactions")));
				pmState = PM_STOP_BACKENDS;
			}

			/*
			 * PostmasterStateMachine will issue any necessary signals, or
			 * take the next step if no child processes need to be killed.
			 */
			PostmasterStateMachine();
			break;

		case SIGQUIT:

			/*
			 * Immediate Shutdown:
			 *
			 * abort all children with SIGQUIT, wait for them to exit,
			 * terminate remaining ones with SIGKILL, then exit without
			 * attempt to properly shut down the data base system.
			 */
			if (Shutdown >= ImmediateShutdown)
				break;
			Shutdown = ImmediateShutdown;
			ereport(LOG,
					(errmsg("received immediate shutdown request")));

			/* Report status */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);

			/* tell children to shut down ASAP */
			SetQuitSignalReason(PMQUIT_FOR_STOP);
			TerminateChildren(SIGQUIT);
			pmState = PM_WAIT_BACKENDS;

			/* set stopwatch for them to die */
			AbortStartTime = time(NULL);

			/*
			 * Now wait for backends to exit.  If there are none,
			 * PostmasterStateMachine will take the next step.
			 */
			PostmasterStateMachine();
			break;
	}

	errno = save_errno;
}

/*
 * Reaper -- signal handler to cleanup after a child process dies.
 */
static void
reaper(SIGNAL_ARGS)
{
	int			save_errno = errno;
	int			pid;			/* process id of dead child process */
	int			exitstatus;		/* its exit status */

	/*
	 * We rely on the signal mechanism to have blocked all signals.
	 */

	ereport(DEBUG4,
			(errmsg_internal("reaping dead processes")));

	while ((pid = waitpid(-1, &exitstatus, WNOHANG)) > 0)
	{
		/*
		 * Check if this child was a startup process.
		 */
		if (pid == StartupPID)
		{
			StartupPID = 0;

			/*
			 * Startup process exited in response to a shutdown request (or it
			 * completed normally regardless of the shutdown request).
			 */
			if (Shutdown > NoShutdown &&
				(EXIT_STATUS_0(exitstatus) || EXIT_STATUS_1(exitstatus)))
			{
				StartupStatus = STARTUP_NOT_RUNNING;
				pmState = PM_WAIT_BACKENDS;
				/* PostmasterStateMachine logic does the rest */
				continue;
			}

			if (EXIT_STATUS_3(exitstatus))
			{
				ereport(LOG,
						(errmsg("shutdown at recovery target")));
				StartupStatus = STARTUP_NOT_RUNNING;
				Shutdown = Max(Shutdown, SmartShutdown);
				TerminateChildren(SIGTERM);
				pmState = PM_WAIT_BACKENDS;
				/* PostmasterStateMachine logic does the rest */
				continue;
			}

			/*
			 * Unexpected exit of startup process (including FATAL exit)
			 * during PM_STARTUP is treated as catastrophic. There are no
			 * other processes running yet, so we can just exit.
			 */
			if (pmState == PM_STARTUP &&
				StartupStatus != STARTUP_SIGNALED &&
				!EXIT_STATUS_0(exitstatus))
			{
				LogChildExit(LOG, _("startup process"),
							 pid, exitstatus);
				ereport(LOG,
						(errmsg("aborting startup due to startup process failure")));
				ExitPostmaster(1);
			}

			/*
			 * After PM_STARTUP, any unexpected exit (including FATAL exit) of
			 * the startup process is catastrophic, so kill other children,
			 * and set StartupStatus so we don't try to reinitialize after
			 * they're gone.  Exception: if StartupStatus is STARTUP_SIGNALED,
			 * then we previously sent the startup process a SIGQUIT; so
			 * that's probably the reason it died, and we do want to try to
			 * restart in that case.
			 *
			 * This stanza also handles the case where we sent a SIGQUIT
			 * during PM_STARTUP due to some dead_end child crashing: in that
			 * situation, if the startup process dies on the SIGQUIT, we need
			 * to transition to PM_WAIT_BACKENDS state which will allow
			 * PostmasterStateMachine to restart the startup process.  (On the
			 * other hand, the startup process might complete normally, if we
			 * were too late with the SIGQUIT.  In that case we'll fall
			 * through and commence normal operations.)
			 */
			if (!EXIT_STATUS_0(exitstatus))
			{
				if (StartupStatus == STARTUP_SIGNALED)
				{
					StartupStatus = STARTUP_NOT_RUNNING;
					if (pmState == PM_STARTUP)
						pmState = PM_WAIT_BACKENDS;
				}
				else
					StartupStatus = STARTUP_CRASHED;
				HandleChildCrash(pid, exitstatus,
								 _("startup process"));
				continue;
			}

			/*
			 * Startup succeeded, commence normal operations
			 */
			StartupStatus = STARTUP_NOT_RUNNING;
			FatalError = false;
			AbortStartTime = 0;
			ReachedNormalRunning = true;
			pmState = PM_RUN;
			connsAllowed = ALLOW_ALL_CONNS;

			/*
			 * Crank up the background tasks, if we didn't do that already
			 * when we entered consistent recovery state.  It doesn't matter
			 * if this fails, we'll just try again later.
			 */
			if (CheckpointerPID == 0)
				CheckpointerPID = StartCheckpointer();
			if (BgWriterPID == 0)
				BgWriterPID = StartBackgroundWriter();
			if (WalWriterPID == 0)
				WalWriterPID = StartWalWriter();

			ereport(LOG,
					(errmsg("database system is ready to accept connections")));

			/* Report status */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_READY);

			continue;
		}

		/*
		 * Was it the bgwriter?  Normal exit can be ignored; we'll start a new
		 * one at the next iteration of the postmaster's main loop, if
		 * necessary.  Any other exit condition is treated as a crash.
		 */
		if (pid == BgWriterPID)
		{
			BgWriterPID = 0;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("background writer process"));
			continue;
		}

		/*
		 * Was it the checkpointer?
		 */
		if (pid == CheckpointerPID)
		{
			CheckpointerPID = 0;
			if (EXIT_STATUS_0(exitstatus) && pmState == PM_SHUTDOWN)
			{
				/*
				 * OK, we saw normal exit of the checkpointer after it's been
				 * told to shut down.  We expect that it wrote a shutdown
				 * checkpoint.  (If for some reason it didn't, recovery will
				 * occur on next postmaster start.)
				 *
				 * At this point we should have no normal backend children
				 * left (else we'd not be in PM_SHUTDOWN state) but we might
				 * have dead_end children to wait for.
				 *
				 * If we have walsender
				 * processes, tell them to send any remaining WAL and quit.
				 */
				Assert(Shutdown > NoShutdown);

				/*
				 * Waken walsenders for the last time. No regular backends
				 * should be around anymore.
				 */
				SignalSomeChildren(SIGUSR2);

				pmState = PM_SHUTDOWN_2;
			}
			else
			{
				/*
				 * Any unexpected exit of the checkpointer (including FATAL
				 * exit) is treated as a crash.
				 */
				HandleChildCrash(pid, exitstatus,
								 _("checkpointer process"));
			}

			continue;
		}

		/*
		 * Was it the wal writer?  Normal exit can be ignored; we'll start a
		 * new one at the next iteration of the postmaster's main loop, if
		 * necessary.  Any other exit condition is treated as a crash.
		 */
		if (pid == WalWriterPID)
		{
			WalWriterPID = 0;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("WAL writer process"));
			continue;
		}

		/* Was it the system logger?  If so, try to start a new one */
		if (pid == SysLoggerPID)
		{
			SysLoggerPID = 0;
			/* for safety's sake, launch new logger *first* */
			SysLoggerPID = SysLogger_Start();
			if (!EXIT_STATUS_0(exitstatus))
				LogChildExit(LOG, _("system logger process"),
							 pid, exitstatus);
			continue;
		}

		/*
		 * Else do standard backend child cleanup.
		 */
		CleanupBackend(pid, exitstatus);
	}							/* loop over pending child-death reports */

	/*
	 * After cleaning out the SIGCHLD queue, see if we have any state changes
	 * or actions to make.
	 */
	PostmasterStateMachine();

	/* Done with signal handler */

	errno = save_errno;
}

/*
 * CleanupBackend -- cleanup after terminated backend.
 *
 * Remove all local state associated with backend.
 */
static void
CleanupBackend(int pid,
			   int exitstatus)	/* child's exit status. */
{
	dlist_mutable_iter iter;

	LogChildExit(DEBUG2, _("server process"), pid, exitstatus);

	/*
	 * If a backend dies in an ugly way then we must signal all other backends
	 * to quickdie.  If exit status is zero (normal) or one (FATAL exit), we
	 * assume everything is all right and proceed to remove the backend from
	 * the active backend list.
	 */

	if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
	{
		HandleChildCrash(pid, exitstatus, _("server process"));
		return;
	}

	dlist_foreach_modify(iter, &BackendList)
	{
		Backend    *bp = dlist_container(Backend, elem, iter.cur);

		if (bp->pid == pid)
		{
			if (!bp->dead_end)
			{
				if (!ReleasePostmasterChildSlot(bp->child_slot))
				{
					/*
					 * Uh-oh, the child failed to clean itself up.  Treat as a
					 * crash after all.
					 */
					HandleChildCrash(pid, exitstatus, _("server process"));
				return;
			}
		}
			dlist_delete(iter.cur);
			free(bp);
			break;
		}
	}
}

/*
 * HandleChildCrash -- cleanup after failed backend, bgwriter, checkpointer,
 * walwriter or syslogger.
 *
 * The objectives here are to clean up our local state about the child
 * process, and to signal all other remaining children to quickdie.
 */
static void
HandleChildCrash(int pid, int exitstatus, const char *procname)
{
	dlist_mutable_iter iter;
	Backend    *bp;
	bool		take_action;

	/*
	 * We only log messages and send signals if this is the first process
	 * crash and we're not doing an immediate shutdown; otherwise, we're only
	 * here to update postmaster's idea of live processes.  If we have already
	 * signaled children, nonzero exit status is to be expected, so don't
	 * clutter log.
	 */
	take_action = !FatalError && Shutdown != ImmediateShutdown;

	if (take_action)
	{
		LogChildExit(LOG, procname, pid, exitstatus);
		ereport(LOG,
				(errmsg("terminating any other active server processes")));
		SetQuitSignalReason(PMQUIT_FOR_CRASH);
	}

	/* Process regular backends */
	dlist_foreach_modify(iter, &BackendList)
	{
		bp = dlist_container(Backend, elem, iter.cur);

		if (bp->pid == pid)
		{
			/*
			 * Found entry for freshly-dead backend, so remove it.
			 */
			if (!bp->dead_end)
			{
				(void) ReleasePostmasterChildSlot(bp->child_slot);
			}
			dlist_delete(iter.cur);
			free(bp);
			/* Keep looping so we can signal remaining backends */
		}
		else
		{
			/*
			 * This backend is still alive.  Unless we did so already, tell it
			 * to commit hara-kiri.
			 *
			 * SIGQUIT is the special signal that says exit without proc_exit
			 * and let the user know what's going on. But if SendStop is set
			 * (-T on command line), then we send SIGSTOP instead, so that we
			 * can get core dumps from all backends by hand.
			 *
			 * We could exclude dead_end children here, but at least in the
			 * SIGSTOP case it seems better to include them.
			 */
			if (take_action)
			{
				ereport(DEBUG2,
						(errmsg_internal("sending %s to process %d",
										 (SendStop ? "SIGSTOP" : "SIGQUIT"),
										 (int) bp->pid)));
				signal_child(bp->pid, (SendStop ? SIGSTOP : SIGQUIT));
			}
		}
	}

	/* Take care of the startup process too */
	if (pid == StartupPID)
	{
		StartupPID = 0;
		/* Caller adjusts StartupStatus, so don't touch it here */
	}
	else if (StartupPID != 0 && take_action)
	{
		ereport(DEBUG2,
				(errmsg_internal("sending %s to process %d",
								 (SendStop ? "SIGSTOP" : "SIGQUIT"),
								 (int) StartupPID)));
		signal_child(StartupPID, (SendStop ? SIGSTOP : SIGQUIT));
		StartupStatus = STARTUP_SIGNALED;
	}

	/* Take care of the bgwriter too */
	if (pid == BgWriterPID)
		BgWriterPID = 0;
	else if (BgWriterPID != 0 && take_action)
	{
		ereport(DEBUG2,
				(errmsg_internal("sending %s to process %d",
								 (SendStop ? "SIGSTOP" : "SIGQUIT"),
								 (int) BgWriterPID)));
		signal_child(BgWriterPID, (SendStop ? SIGSTOP : SIGQUIT));
	}

	/* Take care of the checkpointer too */
	if (pid == CheckpointerPID)
		CheckpointerPID = 0;
	else if (CheckpointerPID != 0 && take_action)
	{
		ereport(DEBUG2,
				(errmsg_internal("sending %s to process %d",
								 (SendStop ? "SIGSTOP" : "SIGQUIT"),
								 (int) CheckpointerPID)));
		signal_child(CheckpointerPID, (SendStop ? SIGSTOP : SIGQUIT));
	}

	/* Take care of the walwriter too */
	if (pid == WalWriterPID)
		WalWriterPID = 0;
	else if (WalWriterPID != 0 && take_action)
	{
		ereport(DEBUG2,
				(errmsg_internal("sending %s to process %d",
								 (SendStop ? "SIGSTOP" : "SIGQUIT"),
								 (int) WalWriterPID)));
		signal_child(WalWriterPID, (SendStop ? SIGSTOP : SIGQUIT));
	}

	/* We do NOT restart the syslogger */

	if (Shutdown != ImmediateShutdown)
		FatalError = true;

	/* We now transit into a state of waiting for children to die */
	if (pmState == PM_RUN ||
		pmState == PM_STOP_BACKENDS ||
		pmState == PM_SHUTDOWN)
		pmState = PM_WAIT_BACKENDS;

	/*
	 * .. and if this doesn't happen quickly enough, now the clock is ticking
	 * for us to kill them without mercy.
	 */
	if (AbortStartTime == 0)
		AbortStartTime = time(NULL);
}

/*
 * Log the death of a child process.
 */
static void
LogChildExit(int lev, const char *procname, int pid, int exitstatus)
{
	/*
	 * size of activity_buffer is arbitrary, but set equal to default
	 * track_activity_query_size
	 */
	char		activity_buffer[1024];
	const char *activity = NULL;

	if (!EXIT_STATUS_0(exitstatus))
		activity = pgstat_get_crashed_backend_activity(pid,
													   activity_buffer,
													   sizeof(activity_buffer));

	if (WIFEXITED(exitstatus))
		ereport(lev,

		/*------
		  translator: %s is a noun phrase describing a child process, such as
		  "server process" */
				(errmsg("%s (PID %d) exited with exit code %d",
						procname, pid, WEXITSTATUS(exitstatus)),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
	else if (WIFSIGNALED(exitstatus))
	{
		ereport(lev,

		/*------
		  translator: %s is a noun phrase describing a child process, such as
		  "server process" */
				(errmsg("%s (PID %d) was terminated by signal %d: %s",
						procname, pid, WTERMSIG(exitstatus),
						pg_strsignal(WTERMSIG(exitstatus))),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
	}
	else
		ereport(lev,

		/*------
		  translator: %s is a noun phrase describing a child process, such as
		  "server process" */
				(errmsg("%s (PID %d) exited with unrecognized status %d",
						procname, pid, exitstatus),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
}

/*
 * Advance the postmaster's state machine and take actions as appropriate
 *
 * This is common code for pmdie(), reaper() and sigusr1_handler(), which
 * receive the signals that might mean we need to change state.
 */
static void
PostmasterStateMachine(void)
{
	/* If we're doing a smart shutdown, try to advance that state. */
	if (pmState == PM_RUN)
	{
		if (connsAllowed == ALLOW_SUPERUSER_CONNS)
		{
			/*
			 * ALLOW_SUPERUSER_CONNS state ends as soon as online backup mode
			 * is not active.
			 */
			if (!BackupInProgress())
				connsAllowed = ALLOW_NO_CONNS;
		}

		if (connsAllowed == ALLOW_NO_CONNS)
		{
			/*
			 * ALLOW_NO_CONNS state ends when we have no normal client
			 * backends running.  Then we're ready to stop other children.
			 */
			if (CountChildren() == 0)
				pmState = PM_STOP_BACKENDS;
		}
	}

	/*
	 * If we're ready to do so, signal child processes to shut down.  (This
	 * isn't a persistent state, but treating it as a distinct pmState allows
	 * us to share this code across multiple shutdown code paths.)
	 */
	if (pmState == PM_STOP_BACKENDS)
	{
		/* Signal all backend children */
		SignalSomeChildren(SIGTERM);
		/* and the bgwriter too */
		if (BgWriterPID != 0)
			signal_child(BgWriterPID, SIGTERM);
		/* and the walwriter too */
		if (WalWriterPID != 0)
			signal_child(WalWriterPID, SIGTERM);
		/* If we're in recovery, also stop startup and walreceiver procs */
		if (StartupPID != 0)
			signal_child(StartupPID, SIGTERM);
		/* checkpointer, stats, and syslogger may continue for now */

		/* Now transition to PM_WAIT_BACKENDS state to wait for them to die */
		pmState = PM_WAIT_BACKENDS;
	}

	/*
	 * If we are in a state-machine state that implies waiting for backends to
	 * exit, see if they're all gone, and change state if so.
	 */
	if (pmState == PM_WAIT_BACKENDS)
	{
		/*
		 * PM_WAIT_BACKENDS state ends when we have no regular backends,
		 * and no walwriter or bgwriter.  If we are
		 * doing crash recovery or an immediate shutdown then we expect the
		 * checkpointer to exit as well, otherwise not. The stats and
		 * syslogger processes are disregarded since they are not connected to
		 * shared memory; we also disregard dead_end children here.
		 */
		if (CountChildren() == 0 &&
			StartupPID == 0 &&
			BgWriterPID == 0 &&
			(CheckpointerPID == 0 ||
			 (!FatalError && Shutdown < ImmediateShutdown)) &&
			WalWriterPID == 0)
		{
			if (Shutdown >= ImmediateShutdown || FatalError)
			{
				/*
				 * Start waiting for dead_end children to die.  This state
				 * change causes ServerLoop to stop creating new ones.
				 */
				pmState = PM_WAIT_DEAD_END;

				/*
				 * We already SIGQUIT'd the stats processes, if
				 * any, when we started immediate shutdown or entered
				 * FatalError state.
				 */
			}
			else
			{
				/*
				 * If we get here, we are proceeding with normal shutdown. All
				 * the regular children are gone, and it's time to tell the
				 * checkpointer to do a shutdown checkpoint.
				 */
				Assert(Shutdown > NoShutdown);
				/* Start the checkpointer if not running */
				if (CheckpointerPID == 0)
					CheckpointerPID = StartCheckpointer();
				/* And tell it to shut down */
				if (CheckpointerPID != 0)
				{
					signal_child(CheckpointerPID, SIGUSR2);
					pmState = PM_SHUTDOWN;
				}
				else
				{
					/*
					 * If we failed to fork a checkpointer, just shut down.
					 * Any required cleanup will happen at next restart. We
					 * set FatalError so that an "abnormal shutdown" message
					 * gets logged when we exit.
					 */
					FatalError = true;
					pmState = PM_WAIT_DEAD_END;

					/* Kill the walsenders too */
					SignalSomeChildren(SIGQUIT);
				}
			}
		}
	}

	if (pmState == PM_SHUTDOWN_2)
	{
		/*
		 * PM_SHUTDOWN_2 state ends when there's no other children than
		 * dead_end children left. There shouldn't be any regular backends
		 * left by now anyway; what we're really waiting for is walsenders.
		 */
		if (CountChildren() == 0)
		{
			pmState = PM_WAIT_DEAD_END;
		}
	}

	if (pmState == PM_WAIT_DEAD_END)
	{
		/*
		 * PM_WAIT_DEAD_END state ends when the BackendList is entirely empty
		 * (ie, no dead_end children remain).
		 *
		 * The reason we wait for this is to protect against a new
		 * postmaster starting conflicting subprocesses; this isn't an
		 * ironclad protection, but it at least helps in the
		 * shutdown-and-immediately-restart scenario.  Note that they have
		 * already been sent appropriate shutdown signals, either during a
		 * normal state transition leading up to PM_WAIT_DEAD_END, or during
		 * FatalError processing.
		 */
		if (dlist_is_empty(&BackendList))
		{
			/* These other guys should be dead already */
			Assert(StartupPID == 0);
			Assert(BgWriterPID == 0);
			Assert(CheckpointerPID == 0);
			Assert(WalWriterPID == 0);
			/* syslogger is not considered here */
			pmState = PM_NO_CHILDREN;
		}
	}

	/*
	 * If we've been told to shut down, we exit as soon as there are no
	 * remaining children.  If there was a crash, cleanup will occur at the
	 * next startup.  (Before PostgreSQL 8.3, we tried to recover from the
	 * crash before exiting, but that seems unwise if we are quitting because
	 * we got SIGTERM from init --- there may well not be time for recovery
	 * before init decides to SIGKILL us.)
	 *
	 * Note that the syslogger continues to run.  It will exit when it sees
	 * EOF on its input pipe, which happens when there are no more upstream
	 * processes.
	 */
	if (Shutdown > NoShutdown && pmState == PM_NO_CHILDREN)
	{
		if (FatalError)
		{
			ereport(LOG, (errmsg("abnormal database system shutdown")));
			ExitPostmaster(1);
		}
		else
		{
			/*
			 * Terminate exclusive backup mode to avoid recovery after a clean
			 * fast shutdown.  Since an exclusive backup can only be taken
			 * during normal running (and not, for example, while running
			 * under Hot Standby) it only makes sense to do this if we reached
			 * normal running. If we're still in recovery, the backup file is
			 * one we're recovering *from*, and we must keep it around so that
			 * recovery restarts from the right place.
			 */
			if (ReachedNormalRunning)
				CancelBackup();

			/*
			 * Normal exit from the postmaster is here.  We don't need to log
			 * anything here, since the UnlinkLockFiles proc_exit callback
			 * will do so, and that should be the last user-visible action.
			 */
			ExitPostmaster(0);
		}
	}

	/*
	 * If the startup process failed, or the user does not want an automatic
	 * restart after backend crashes, wait for all non-syslogger children to
	 * exit, and then exit postmaster.  We don't try to reinitialize when the
	 * startup process fails, because more than likely it will just fail again
	 * and we will keep trying forever.
	 */
	if (pmState == PM_NO_CHILDREN)
	{
		if (StartupStatus == STARTUP_CRASHED)
		{
			ereport(LOG,
					(errmsg("shutting down due to startup process failure")));
			ExitPostmaster(1);
		}
		if (!restart_after_crash)
		{
			ereport(LOG,
					(errmsg("shutting down because restart_after_crash is off")));
			ExitPostmaster(1);
		}
	}

	/*
	 * If we need to recover from a crash, wait for all non-syslogger children
	 * to exit, then reset shmem and StartupDataBase.
	 */
	if (FatalError && pmState == PM_NO_CHILDREN)
	{
		ereport(LOG,
				(errmsg("all server processes terminated; reinitializing")));

		/* remove leftover temporary files after a crash */
		if (remove_temp_files_after_crash)
			RemovePgTempFiles();

		shmem_exit(1);

		/* re-read control file into local memory */
		LocalProcessControlFile(true);

		reset_shared();

		StartupPID = StartupDataBase();
		Assert(StartupPID != 0);
		StartupStatus = STARTUP_RUNNING;
		pmState = PM_STARTUP;
		/* crash recovery started, reset SIGKILL flag */
		AbortStartTime = 0;
	}
}


/*
 * Send a signal to a postmaster child process
 *
 * On systems that have setsid(), each child process sets itself up as a
 * process group leader.  For signals that are generally interpreted in the
 * appropriate fashion, we signal the entire process group not just the
 * direct child process.  This allows us to, for example, SIGQUIT a blocked
 * archive_recovery script, or SIGINT a script being run by a backend via
 * system().
 *
 * There is a race condition for recently-forked children: they might not
 * have executed setsid() yet.  So we signal the child directly as well as
 * the group.  We assume such a child will handle the signal before trying
 * to spawn any grandchild processes.  We also assume that signaling the
 * child twice will not cause any problems.
 */
static void
signal_child(pid_t pid, int signal)
{
	if (kill(pid, signal) < 0)
		elog(DEBUG3, "kill(%ld,%d) failed: %m", (long) pid, signal);
#ifdef HAVE_SETSID
	switch (signal)
	{
		case SIGINT:
		case SIGTERM:
		case SIGQUIT:
		case SIGSTOP:
		case SIGKILL:
			if (kill(-pid, signal) < 0)
				elog(DEBUG3, "kill(%ld,%d) failed: %m", (long) (-pid), signal);
			break;
		default:
			break;
	}
#endif
}

/*
 * Send a signal to all backend children (but NOT special children;
 * dead_end children are never signaled, either).
 */
static bool
SignalSomeChildren(int signal)
{
	dlist_iter	iter;
	bool		signaled = false;

	dlist_foreach(iter, &BackendList)
	{
		Backend    *bp = dlist_container(Backend, elem, iter.cur);

		if (bp->dead_end)
			continue;

		ereport(DEBUG4,
				(errmsg_internal("sending signal %d to process %d",
								 signal, (int) bp->pid)));
		signal_child(bp->pid, signal);
		signaled = true;
	}
	return signaled;
}

/*
 * Send a termination signal to children.  This considers all of our children
 * processes, except syslogger and dead_end backends.
 */
static void
TerminateChildren(int signal)
{
	SignalSomeChildren(signal);
	if (StartupPID != 0)
	{
		signal_child(StartupPID, signal);
		if (signal == SIGQUIT || signal == SIGKILL)
			StartupStatus = STARTUP_SIGNALED;
	}
	if (BgWriterPID != 0)
		signal_child(BgWriterPID, signal);
	if (CheckpointerPID != 0)
		signal_child(CheckpointerPID, signal);
	if (WalWriterPID != 0)
		signal_child(WalWriterPID, signal);
}

/*
 * BackendStartup -- start backend process
 *
 * returns: STATUS_ERROR if the fork failed, STATUS_OK otherwise.
 */
static int
BackendStartup(Port *port)
{
	Backend    *bn;				/* for backend cleanup */
	pid_t		pid;

	/*
	 * Create backend data structure.  Better before the fork() so we can
	 * handle failure cleanly.
	 */
	bn = (Backend *) malloc(sizeof(Backend));
	if (!bn)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return STATUS_ERROR;
	}

	/*
	 * Compute the cancel key that will be assigned to this backend. The
	 * backend will have its own copy in the forked-off process' value of
	 * MyCancelKey, so that it can transmit the key to the frontend.
	 */
	if (!RandomCancelKey(&MyCancelKey))
	{
		free(bn);
		ereport(LOG,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate random cancel key")));
		return STATUS_ERROR;
	}

	bn->cancel_key = MyCancelKey;

	/* Pass down canAcceptConnections state */
	port->canAcceptConnections = canAcceptConnections();
	bn->dead_end = (port->canAcceptConnections != CAC_OK &&
					port->canAcceptConnections != CAC_SUPERUSER);

	/*
	 * Unless it's a dead_end child, assign it a child slot number
	 */
	if (!bn->dead_end)
		bn->child_slot = MyPMChildSlot = AssignPostmasterChildSlot();
	else
		bn->child_slot = 0;

	pid = fork_process();
	if (pid == 0)				/* child */
	{
		free(bn);

		/* Detangle from postmaster */
		InitPostmasterChild();

		/* Close the postmaster's sockets */
		ClosePostmasterPorts(false);

		/* Perform additional initialization and collect startup packet */
		BackendInitialize(port);

		/* And run the backend */
		BackendRun(port);
	}

	if (pid < 0)
	{
		/* in parent, fork failed */
		int			save_errno = errno;

		if (!bn->dead_end)
			(void) ReleasePostmasterChildSlot(bn->child_slot);
		free(bn);
		errno = save_errno;
		ereport(LOG,
				(errmsg("could not fork new process for connection: %m")));
		report_fork_failure_to_client(port, save_errno);
		return STATUS_ERROR;
	}

	/* in parent, successful fork */
	ereport(DEBUG2,
			(errmsg_internal("forked new backend, pid=%d socket=%d",
							 (int) pid, (int) port->sock)));

	/*
	 * Everything's been successful, it's safe to add this backend to our list
	 * of backends.
	 */
	bn->pid = pid;
	dlist_push_head(&BackendList, &bn->elem);

	return STATUS_OK;
}

/*
 * Try to report backend fork() failure to client before we close the
 * connection.  Since we do not care to risk blocking the postmaster on
 * this connection, we set the connection to non-blocking and try only once.
 *
 * This is grungy special-purpose code; we cannot use backend libpq since
 * it's not up and running.
 */
static void
report_fork_failure_to_client(Port *port, int errnum)
{
	char		buffer[1000];
	int			rc;

	/* Format the error message packet (always V2 protocol) */
	snprintf(buffer, sizeof(buffer), "E%s%s\n",
			 _("could not fork new process for connection: "),
			 strerror(errnum));

	/* Set port to non-blocking.  Don't do send() if this fails */
	if (!pg_set_noblock(port->sock))
		return;

	/* We'll retry after EINTR, but ignore all other failures */
	do
	{
		rc = send(port->sock, buffer, strlen(buffer) + 1, 0);
	} while (rc < 0 && errno == EINTR);
}


/*
 * BackendInitialize -- initialize an interactive (postmaster-child)
 *				backend process, and collect the client's startup packet.
 *
 * returns: nothing.  Will not return at all if there's any failure.
 *
 * Note: this code does not depend on having any access to shared memory.
 * Indeed, our approach to SIGTERM/timeout handling *requires* that
 * shared memory not have been touched yet; see comments within.
 */
static void
BackendInitialize(Port *port)
{
	int			status;
	int			ret;
	char		remote_host[NI_MAXHOST];
	char		remote_port[NI_MAXSERV];
	StringInfoData ps_data;

	/* Save port etc. for ps status */
	MyProcPort = port;

	/* Tell fd.c about the long-lived FD associated with the port */
	ReserveExternalFD();

	/*
	 * PreAuthDelay is a debugging aid for investigating problems in the
	 * authentication cycle: it can be set in postgresql.conf to allow time to
	 * attach to the newly-forked backend with a debugger.  (See also
	 * PostAuthDelay, which we allow clients to pass through PGOPTIONS, but it
	 * is not honored until after authentication.)
	 */
	if (PreAuthDelay > 0)
		pg_usleep(PreAuthDelay * 1000000L);

	/* This flag will remain set until InitPostgres finishes authentication */
	ClientAuthInProgress = true;	/* limit visibility of log messages */

	/* set these to empty in case they are needed before we set them up */
	port->remote_host = "";
	port->remote_port = "";

	/*
	 * Initialize libpq and enable reporting of ereport errors to the client.
	 * Must do this now because authentication uses libpq to send messages.
	 */
	pq_init();					/* initialize libpq to talk to client */
	whereToSendOutput = DestRemote; /* now safe to ereport to client */

	/*
	 * We arrange to do _exit(1) if we receive SIGTERM or timeout while trying
	 * to collect the startup packet; while SIGQUIT results in _exit(2).
	 * Otherwise the postmaster cannot shutdown the database FAST or IMMED
	 * cleanly if a buggy client fails to send the packet promptly.
	 *
	 * Exiting with _exit(1) is only possible because we have not yet touched
	 * shared memory; therefore no outside-the-process state needs to get
	 * cleaned up.
	 */
	pqsignal(SIGTERM, process_startup_packet_die);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	InitializeTimeouts();		/* establishes SIGALRM handler */
	PG_SETMASK(&StartupBlockSig);

	/*
	 * Get the remote host name and port for logging and status display.
	 */
	remote_host[0] = '\0';
	remote_port[0] = '\0';
	if ((ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								  remote_host, sizeof(remote_host),
								  remote_port, sizeof(remote_port),
								  (log_hostname ? 0 : NI_NUMERICHOST) | NI_NUMERICSERV)) != 0)
		ereport(WARNING,
				(errmsg_internal("pg_getnameinfo_all() failed: %s",
								 gai_strerror(ret))));

	/*
	 * Save remote_host and remote_port in port structure (after this, they
	 * will appear in log_line_prefix data for log messages).
	 */
	port->remote_host = strdup(remote_host);
	port->remote_port = strdup(remote_port);

	/*
	 * If we did a reverse lookup to name, we might as well save the results
	 * rather than possibly repeating the lookup during authentication.
	 *
	 * Note that we don't want to specify NI_NAMEREQD above, because then we'd
	 * get nothing useful for a client without an rDNS entry.  Therefore, we
	 * must check whether we got a numeric IPv4 or IPv6 address, and not save
	 * it into remote_hostname if so.  (This test is conservative and might
	 * sometimes classify a hostname as numeric, but an error in that
	 * direction is safe; it only results in a possible extra lookup.)
	 */
	if (log_hostname &&
		ret == 0 &&
		strspn(remote_host, "0123456789.") < strlen(remote_host) &&
		strspn(remote_host, "0123456789ABCDEFabcdef:") < strlen(remote_host))
		port->remote_hostname = strdup(remote_host);

	/*
	 * Ready to begin client interaction.  We will give up and _exit(1) after
	 * a time delay, so that a broken client can't hog a connection
	 * indefinitely.  PreAuthDelay and any DNS interactions above don't count
	 * against the time limit.

	 * Note: because PostgresMain will call InitializeTimeouts again, the
	 * registration of STARTUP_PACKET_TIMEOUT will be lost.  This is okay
	 * since we never use it again after this function.
	 */
	RegisterTimeout(STARTUP_PACKET_TIMEOUT, StartupPacketTimeoutHandler);

	/*
	 * Receive the startup packet (which might turn out to be a cancel request
	 * packet).
	 */
	status = ProcessStartupPacket(port, false);

	/*
	 * Disable the timeout, and prevent SIGTERM again.
	 */
	disable_timeout(STARTUP_PACKET_TIMEOUT, false);
	PG_SETMASK(&BlockSig);

	/*
	 * As a safety check that nothing in startup has yet performed
	 * shared-memory modifications that would need to be undone if we had
	 * exited through SIGTERM or timeout above, check that no on_shmem_exit
	 * handlers have been registered yet.  (This isn't terribly bulletproof,
	 * since someone might misuse an on_proc_exit handler for shmem cleanup,
	 * but it's a cheap and helpful check.  We cannot disallow on_proc_exit
	 * handlers unfortunately, since pq_init() already registered one.)
	 */
	check_on_shmem_exit_lists_are_empty();

	/*
	 * Stop here if it was bad or a cancel packet.  ProcessStartupPacket
	 * already did any appropriate error reporting.
	 */
	if (status != STATUS_OK)
		proc_exit(0);

	/*
	 * Now that we have the user and database name, we can set the process
	 * title for ps.  It's good to do this as early as possible in startup.
	 */
	initStringInfo(&ps_data);
	appendStringInfo(&ps_data, "%s ", port->user_name);
	appendStringInfo(&ps_data, "%s ", port->database_name);
	appendStringInfo(&ps_data, "%s", port->remote_host);
	if (port->remote_port[0] != '\0')
		appendStringInfo(&ps_data, "(%s)", port->remote_port);

	init_ps_display(ps_data.data);
	pfree(ps_data.data);

	set_ps_display("initializing");
}


/*
 * BackendRun -- set up the backend's argument list and invoke PostgresMain()
 *
 * returns:
 *		Doesn't return at all.
 */
static void
BackendRun(Port *port)
{
	char	   *av[2];
	const int	ac = 1;

	av[0] = "postgres";
	av[1] = NULL;

	/*
	 * Make sure we aren't in PostmasterContext anymore.  (We can't delete it
	 * just yet, though, because InitPostgres will need the HBA data.)
	 */
	MemoryContextSwitchTo(TopMemoryContext);

	PostgresMain(ac, av, port->database_name, port->user_name);
}




/*
 * ExitPostmaster -- cleanup
 *
 * Do NOT call exit() directly --- always go through here!
 */
static void
ExitPostmaster(int status)
{
	/* should cleanup shared memory and kill all backends */

	/*
	 * Not sure of the semantics here.  When the Postmaster dies, should the
	 * backends all be killed? probably not.
	 *
	 * MUST		-- vadim 05-10-1999
	 */

	proc_exit(status);
}

/*
 * sigusr1_handler - handle signal conditions from child processes
 */
static void
sigusr1_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/* Tell syslogger to rotate logfile if requested */
	if (SysLoggerPID != 0)
	{
		if (CheckLogrotateSignal())
		{
			signal_child(SysLoggerPID, SIGUSR1);
			RemoveLogrotateSignalFiles();
		}
		else if (CheckPostmasterSignal(PMSIGNAL_ROTATE_LOGFILE))
		{
			signal_child(SysLoggerPID, SIGUSR1);
		}
	}

	/*
	 * Try to advance postmaster's state machine, if a child requests it.
	 *
	 * Be careful about the order of this action relative to sigusr1_handler's
	 * other actions.  Generally, this should be after other actions, in case
	 * they have effects PostmasterStateMachine would need to know about.
	 * However, we should do it before the CheckPromoteSignal step, which
	 * cannot have any (immediate) effect on the state machine, but does
	 * depend on what state we're in now.
	 */
	if (CheckPostmasterSignal(PMSIGNAL_ADVANCE_STATE_MACHINE))
	{
		PostmasterStateMachine();
	}

	errno = save_errno;
}

/*
 * SIGTERM while processing startup packet.
 *
 * Running proc_exit() from a signal handler would be quite unsafe.
 * However, since we have not yet touched shared memory, we can just
 * pull the plug and exit without running any atexit handlers.
 *
 * One might be tempted to try to send a message, or log one, indicating
 * why we are disconnecting.  However, that would be quite unsafe in itself.
 * Also, it seems undesirable to provide clues about the database's state
 * to a client that has not yet completed authentication, or even sent us
 * a startup packet.
 */
static void
process_startup_packet_die(SIGNAL_ARGS)
{
	_exit(1);
}

/*
 * Dummy signal handler
 *
 * We use this for signals that we don't actually use in the postmaster,
 * but we do use in backends.  If we were to SIG_IGN such signals in the
 * postmaster, then a newly started backend might drop a signal that arrives
 * before it's able to reconfigure its signal processing.  (See notes in
 * tcop/postgres.c.)
 */
static void
dummy_handler(SIGNAL_ARGS)
{
}

/*
 * Timeout while processing startup packet.
 * As for process_startup_packet_die(), we exit via _exit(1).
 */
static void
StartupPacketTimeoutHandler(void)
{
	_exit(1);
}


/*
 * Generate a random cancel key.
 */
static bool
RandomCancelKey(int32 *cancel_key)
{
	return pg_strong_random(cancel_key, sizeof(int32));
}

/*
 * Count up number of child processes (dead_end children are always
 * excluded).
 */
static int
CountChildren(void)
{
	dlist_iter	iter;
	int			cnt = 0;

	dlist_foreach(iter, &BackendList)
	{
		Backend    *bp = dlist_container(Backend, elem, iter.cur);

		if (bp->dead_end)
			continue;

		cnt++;
	}
	return cnt;
}


/*
 * StartChildProcess -- start an auxiliary process for the postmaster
 *
 * "type" determines what kind of child will be started.  All child types
 * initially go to AuxiliaryProcessMain, which will handle common setup.
 *
 * Return value of StartChildProcess is subprocess' PID, or 0 if failed
 * to start subprocess.
 */
static pid_t
StartChildProcess(AuxProcType type)
{
	pid_t		pid;
	char	   *av[10];
	int			ac = 0;
	char		typebuf[32];

	/*
	 * Set up command-line arguments for subprocess
	 */
	av[ac++] = "postgres";

	snprintf(typebuf, sizeof(typebuf), "-x%d", type);
	av[ac++] = typebuf;

	av[ac] = NULL;
	Assert(ac < lengthof(av));

	pid = fork_process();

	if (pid == 0)				/* child */
	{
		InitPostmasterChild();

		/* Close the postmaster's sockets */
		ClosePostmasterPorts(false);

		/* Release postmaster's working memory context */
		MemoryContextSwitchTo(TopMemoryContext);
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;

		AuxiliaryProcessMain(ac, av);	/* does not return */
	}

	if (pid < 0)
	{
		/* in parent, fork failed */
		int			save_errno = errno;

		errno = save_errno;
		switch (type)
		{
			case StartupProcess:
				ereport(LOG,
						(errmsg("could not fork startup process: %m")));
				break;
			case BgWriterProcess:
				ereport(LOG,
						(errmsg("could not fork background writer process: %m")));
				break;
			case CheckpointerProcess:
				ereport(LOG,
						(errmsg("could not fork checkpointer process: %m")));
				break;
			case WalWriterProcess:
				ereport(LOG,
						(errmsg("could not fork WAL writer process: %m")));
				break;
			default:
				ereport(LOG,
						(errmsg("could not fork process: %m")));
				break;
		}

		/*
		 * fork failure is fatal during startup, but there's no need to choke
		 * immediately if starting other child types fails.
		 */
		if (type == StartupProcess)
			ExitPostmaster(1);
		return 0;
	}

	/*
	 * in parent, successful fork
	 */
	return pid;
}



/*
 * Create the opts file
 */
static bool
CreateOptsFile(int argc, char *argv[], char *fullprogname)
{
	FILE	   *fp;
	int			i;

#define OPTS_FILE	"postmaster.opts"

	if ((fp = fopen(OPTS_FILE, "w")) == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", OPTS_FILE)));
		return false;
	}

	fprintf(fp, "%s", fullprogname);
	for (i = 1; i < argc; i++)
		fprintf(fp, " \"%s\"", argv[i]);
	fputs("\n", fp);

	if (fclose(fp))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", OPTS_FILE)));
		return false;
	}

	return true;
}


/*
 * MaxLivePostmasterChildren
 *
 * This reports the number of entries needed in per-child-process arrays
 * (the PMChildFlags array).
 * These arrays include regular backends, but not special children nor
 * dead_end children.
 * This allows the arrays to have a fixed maximum size, to wit the same
 * too-many-children limit enforced by canAcceptConnections().  The exact value
 * isn't too critical as long as it's more than MaxBackends.
 */
int
MaxLivePostmasterChildren(void)
{
	return 2 * MaxBackends;
}




/*
 * Initialize one and only handle for monitoring postmaster death.
 *
 * Called once in the postmaster, so that child processes can subsequently
 * monitor if their parent is dead.
 */
static void
InitPostmasterDeathWatchHandle(void)
{
	/*
	 * Create a pipe. Postmaster holds the write end of the pipe open
	 * (POSTMASTER_FD_OWN), and children hold the read end. Children can pass
	 * the read file descriptor to select() to wake up in case postmaster
	 * dies, or check for postmaster death with a (read() == 0). Children must
	 * close the write end as soon as possible after forking, because EOF
	 * won't be signaled in the read end until all processes have closed the
	 * write fd. That is taken care of in ClosePostmasterPorts().
	 */
	Assert(MyProcPid == PostmasterPid);
	if (pipe(postmaster_alive_fds) < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg_internal("could not create pipe to monitor postmaster death: %m")));

	/* Notify fd.c that we've eaten two FDs for the pipe. */
	ReserveExternalFD();
	ReserveExternalFD();

	/*
	 * Set O_NONBLOCK to allow testing for the fd's presence with a read()
	 * call.
	 */
	if (fcntl(postmaster_alive_fds[POSTMASTER_FD_WATCH], F_SETFL, O_NONBLOCK) == -1)
		ereport(FATAL,
				(errcode_for_socket_access(),
				 errmsg_internal("could not set postmaster death monitoring pipe to nonblocking mode: %m")));
}
