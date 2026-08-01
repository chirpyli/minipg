/*-------------------------------------------------------------------------
 *
 * pg_ctl --- start/stops/restarts the PostgreSQL server
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 * src/bin/pg_ctl/pg_ctl.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/time.h>
#include <sys/resource.h>
#endif

#include "catalog/pg_control.h"
#include "common/controldata_utils.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "common/string.h"
#include "getopt_long.h"
#include "utils/pidfile.h"

/* PID can be negative for standalone backend */
typedef long pgpid_t;


typedef enum
{
	SMART_MODE,
	FAST_MODE,
	IMMEDIATE_MODE
} ShutdownMode;

typedef enum
{
	POSTMASTER_READY,
	POSTMASTER_STILL_STARTING,
	POSTMASTER_FAILED
} WaitPMResult;

typedef enum
{
	NO_COMMAND = 0,
	INIT_COMMAND,
	START_COMMAND,
	STOP_COMMAND,
	RESTART_COMMAND,
	RELOAD_COMMAND,
	STATUS_COMMAND,
	PROMOTE_COMMAND,
	LOGROTATE_COMMAND,
	KILL_COMMAND
} CtlCommand;

#define DEFAULT_WAIT	60

#define USEC_PER_SEC	1000000

#define WAITS_PER_SEC	10		/* should divide USEC_PER_SEC evenly */

static bool do_wait = true;
static int	wait_seconds = DEFAULT_WAIT;
static bool wait_seconds_arg = false;
static bool silent_mode = false;
static ShutdownMode shutdown_mode = FAST_MODE;
static int	sig = SIGINT;		/* default */
static CtlCommand ctl_command = NO_COMMAND;
static char *pg_data = NULL;
static char *pg_config = NULL;
static char *pgdata_opt = NULL;
static char *post_opts = NULL;
static const char *progname;
static char *log_file = NULL;
static char *exec_path = NULL;
static char *argv0 = NULL;
static bool allow_core_files = false;
static time_t start_time;

static char postopts_file[MAXPGPATH];
static char version_file[MAXPGPATH];
static char pid_file[MAXPGPATH];
static char backup_file[MAXPGPATH];
static char promote_file[MAXPGPATH];
static char logrotate_file[MAXPGPATH];

static volatile pgpid_t postmasterPID = -1;


static void write_stderr(const char *fmt,...) pg_attribute_printf(1, 2);
static void do_advice(void);
static void do_help(void);
static void set_mode(char *modeopt);
static void set_sig(char *signame);
static void do_init(void);
static void do_start(void);
static void do_stop(void);
static void do_restart(void);
static void do_reload(void);
static void do_status(void);
static void do_promote(void);
static void do_logrotate(void);
static void do_kill(pgpid_t pid);
static void print_msg(const char *msg);
static void adjust_data_dir(void);

static pgpid_t get_pgpid(bool is_status_request);
static char **readfile(const char *path, int *numlines);
static void free_readfile(char **optlines);
static pgpid_t start_postmaster(void);
static void read_post_opts(void);

static WaitPMResult wait_for_postmaster_start(pgpid_t pm_pid, bool do_checkpoint);
static bool wait_for_postmaster_stop(void);
static bool wait_for_postmaster_promote(void);
static bool postmaster_is_alive(pid_t pid);

#if defined(HAVE_GETRLIMIT) && defined(RLIMIT_CORE)
static void unlimit_core_size(void);
#endif

static DBState get_control_dbstate(void);


/*
 * Write errors to stderr (or by equal means when stderr is
 * not available).
 */
static void
write_stderr(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	/* On Unix, we just fprintf to stderr */
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/*
 * Given an already-localized string, print it to stdout unless the
 * user has specified that no messages should be printed.
 */
static void
print_msg(const char *msg)
{
	if (!silent_mode)
	{
		fputs(msg, stdout);
		fflush(stdout);
	}
}

static pgpid_t
get_pgpid(bool is_status_request)
{
	FILE	   *pidf;
	long		pid;
	struct stat statbuf;

	if (stat(pg_data, &statbuf) != 0)
	{
		if (errno == ENOENT)
			write_stderr(_("%s: directory \"%s\" does not exist\n"), progname,
						 pg_data);
		else
			write_stderr(_("%s: could not access directory \"%s\": %s\n"), progname,
						 pg_data, strerror(errno));

		/*
		 * The Linux Standard Base Core Specification 3.1 says this should
		 * return '4, program or service status is unknown'
		 * https://refspecs.linuxbase.org/LSB_3.1.0/LSB-Core-generic/LSB-Core-generic/iniscrptact.html
		 */
		exit(is_status_request ? 4 : 1);
	}

	if (stat(version_file, &statbuf) != 0 && errno == ENOENT)
	{
		write_stderr(_("%s: directory \"%s\" is not a database cluster directory\n"),
					 progname, pg_data);
		exit(is_status_request ? 4 : 1);
	}

	pidf = fopen(pid_file, "r");
	if (pidf == NULL)
	{
		/* No pid file, not an error on startup */
		if (errno == ENOENT)
			return 0;
		else
		{
			write_stderr(_("%s: could not open PID file \"%s\": %s\n"),
						 progname, pid_file, strerror(errno));
			exit(1);
		}
	}
	if (fscanf(pidf, "%ld", &pid) != 1)
	{
		/* Is the file empty? */
		if (ftell(pidf) == 0 && feof(pidf))
			write_stderr(_("%s: the PID file \"%s\" is empty\n"),
						 progname, pid_file);
		else
			write_stderr(_("%s: invalid data in PID file \"%s\"\n"),
						 progname, pid_file);
		exit(1);
	}
	fclose(pidf);
	return (pgpid_t) pid;
}


/*
 * get the lines from a text file - return NULL if file can't be opened
 *
 * Trailing newlines are deleted from the lines (this is a change from pre-v10)
 *
 * *numlines is set to the number of line pointers returned; there is
 * also an additional NULL pointer after the last real line.
 */
static char **
readfile(const char *path, int *numlines)
{
	int			fd;
	int			nlines;
	char	  **result;
	char	   *buffer;
	char	   *linebegin;
	int			i;
	int			n;
	int			len;
	struct stat statbuf;

	*numlines = 0;				/* in case of failure or empty file */

	/*
	 * Slurp the file into memory.
	 *
	 * The file can change concurrently, so we read the whole file into memory
	 * with a single read() call. That's not guaranteed to get an atomic
	 * snapshot, but in practice, for a small file, it's close enough for the
	 * current use.
	 */
	fd = open(path, O_RDONLY | PG_BINARY, 0);
	if (fd < 0)
		return NULL;
	if (fstat(fd, &statbuf) < 0)
	{
		close(fd);
		return NULL;
	}
	if (statbuf.st_size == 0)
	{
		/* empty file */
		close(fd);
		result = (char **) pg_malloc(sizeof(char *));
		*result = NULL;
		return result;
	}
	buffer = pg_malloc(statbuf.st_size + 1);

	len = read(fd, buffer, statbuf.st_size + 1);
	close(fd);
	if (len != statbuf.st_size)
	{
		/* oops, the file size changed between fstat and read */
		free(buffer);
		return NULL;
	}

	/*
	 * Count newlines. We expect there to be a newline after each full line,
	 * including one at the end of file. If there isn't a newline at the end,
	 * any characters after the last newline will be ignored.
	 */
	nlines = 0;
	for (i = 0; i < len; i++)
	{
		if (buffer[i] == '\n')
			nlines++;
	}

	/* set up the result buffer */
	result = (char **) pg_malloc((nlines + 1) * sizeof(char *));
	*numlines = nlines;

	/* now split the buffer into lines */
	linebegin = buffer;
	n = 0;
	for (i = 0; i < len; i++)
	{
		if (buffer[i] == '\n')
		{
			int			slen = &buffer[i] - linebegin;
			char	   *linebuf = pg_malloc(slen + 1);

			memcpy(linebuf, linebegin, slen);
			/* we already dropped the \n, but get rid of any \r too */
			if (slen > 0 && linebuf[slen - 1] == '\r')
				slen--;
			linebuf[slen] = '\0';
			result[n++] = linebuf;
			linebegin = &buffer[i + 1];
		}
	}
	result[n] = NULL;

	free(buffer);

	return result;
}


/*
 * Free memory allocated for optlines through readfile()
 */
static void
free_readfile(char **optlines)
{
	char	   *curr_line = NULL;
	int			i = 0;

	if (!optlines)
		return;

	while ((curr_line = optlines[i++]))
		free(curr_line);

	free(optlines);
}

/*
 * start/test/stop routines
 */

/*
 * Start the postmaster and return its PID.
 *
 * Currently, on Windows what we return is the PID of the shell process
 * that launched the postmaster (and, we trust, is waiting for it to exit).
 * So the PID is usable for "is the postmaster still running" checks,
 * but cannot be compared directly to postmaster.pid.
 *
 * On Windows, we also save aside a handle to the shell process in
 * "postmasterProcess", which the caller should close when done with it.
 */
static pgpid_t
start_postmaster(void)
{
	char	   *cmd;

	pgpid_t		pm_pid;

	/* Flush stdio channels just before fork, to avoid double-output problems */
	fflush(stdout);
	fflush(stderr);

	pm_pid = fork();
	if (pm_pid < 0)
	{
		/* fork failed */
		write_stderr(_("%s: could not start server: %s\n"),
					 progname, strerror(errno));
		exit(1);
	}
	if (pm_pid > 0)
	{
		/* fork succeeded, in parent */
		return pm_pid;
	}

	/* fork succeeded, in child */

	/*
	 * If possible, detach the postmaster process from the launching process
	 * group and make it a group leader, so that it doesn't get signaled along
	 * with the current group that launched it.
	 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
	{
		write_stderr(_("%s: could not start server due to setsid() failure: %s\n"),
					 progname, strerror(errno));
		exit(1);
	}
#endif

	/*
	 * Since there might be quotes to handle here, it is easier simply to pass
	 * everything to a shell to process them.  Use exec so that the postmaster
	 * has the same PID as the current child process.
	 */
	if (log_file != NULL)
		cmd = psprintf("exec \"%s\" %s%s < \"%s\" >> \"%s\" 2>&1",
					   exec_path, pgdata_opt, post_opts,
					   DEVNULL, log_file);
	else
		cmd = psprintf("exec \"%s\" %s%s < \"%s\" 2>&1",
					   exec_path, pgdata_opt, post_opts, DEVNULL);

	(void) execl("/bin/sh", "/bin/sh", "-c", cmd, (char *) NULL);

	/* exec failed */
	write_stderr(_("%s: could not start server: %s\n"),
				 progname, strerror(errno));
	exit(1);

	return 0;					/* keep dumb compilers quiet */
}



/*
 * Wait for the postmaster to become ready.
 *
 * On Unix, pm_pid is the PID of the just-launched postmaster.  On Windows,
 * it may be the PID of an ancestor shell process, so we can't check the
 * contents of postmaster.pid quite as carefully.
 *
 * On Windows, the static variable postmasterProcess is an implicit argument
 * to this routine; it contains a handle to the postmaster process or an
 * ancestor shell process thereof.
 *
 * Note that the checkpoint parameter enables a Windows service control
 * manager checkpoint, it's got nothing to do with database checkpoints!!
 */
static WaitPMResult
wait_for_postmaster_start(pgpid_t pm_pid, bool do_checkpoint)
{
	int			i;

	for (i = 0; i < wait_seconds * WAITS_PER_SEC; i++)
	{
		char	  **optlines;
		int			numlines;

		/*
		 * Try to read the postmaster.pid file.  If it's not valid, or if the
		 * status line isn't there yet, just keep waiting.
		 */
		if ((optlines = readfile(pid_file, &numlines)) != NULL &&
			numlines >= LOCK_FILE_LINE_PM_STATUS)
		{
			/* File is complete enough for us, parse it */
			pgpid_t		pmpid;
			time_t		pmstart;

			/*
			 * Make sanity checks.  If it's for the wrong PID, or the recorded
			 * start time is before pg_ctl started, then either we are looking
			 * at the wrong data directory, or this is a pre-existing pidfile
			 * that hasn't (yet?) been overwritten by our child postmaster.
			 * Allow 2 seconds slop for possible cross-process clock skew.
			 */
			pmpid = atol(optlines[LOCK_FILE_LINE_PID - 1]);
			pmstart = atoll(optlines[LOCK_FILE_LINE_START_TIME - 1]);
			if (pmstart >= start_time - 2 &&
				pmpid == pm_pid
				)
			{
				/*
				 * OK, seems to be a valid pidfile from our child.  Check the
				 * status line (this assumes a v10 or later server).
				 */
				char	   *pmstatus = optlines[LOCK_FILE_LINE_PM_STATUS - 1];

				if (strcmp(pmstatus, PM_STATUS_READY) == 0 ||
					strcmp(pmstatus, PM_STATUS_STANDBY) == 0)
				{
					/* postmaster is done starting up */
					free_readfile(optlines);
					return POSTMASTER_READY;
				}
			}
		}

		/*
		 * Free the results of readfile.
		 *
		 * This is safe to call even if optlines is NULL.
		 */
		free_readfile(optlines);

		/*
		 * Check whether the child postmaster process is still alive.  This
		 * lets us exit early if the postmaster fails during startup.
		 *
		 * On Windows, we may be checking the postmaster's parent shell, but
		 * that's fine for this purpose.
		 */
		{
			int			exitstatus;

			if (waitpid((pid_t) pm_pid, &exitstatus, WNOHANG) == (pid_t) pm_pid)
				return POSTMASTER_FAILED;
		}

		/* Startup still in process; wait, printing a dot once per second */
		if (i % WAITS_PER_SEC == 0)
		{
			print_msg(".");
		}

		pg_usleep(USEC_PER_SEC / WAITS_PER_SEC);
	}

	/* out of patience; report that postmaster is still starting up */
	return POSTMASTER_STILL_STARTING;
}


/*
 * Wait for the postmaster to stop.
 *
 * Returns true if the postmaster stopped cleanly (i.e., removed its pidfile).
 * Returns false if the postmaster dies uncleanly, or if we time out.
 */
static bool
wait_for_postmaster_stop(void)
{
	int			cnt;

	for (cnt = 0; cnt < wait_seconds * WAITS_PER_SEC; cnt++)
	{
		pgpid_t		pid;

		if ((pid = get_pgpid(false)) == 0)
			return true;		/* pid file is gone */

		if (kill((pid_t) pid, 0) != 0)
		{
			/*
			 * Postmaster seems to have died.  Check the pid file once more to
			 * avoid a race condition, but give up waiting.
			 */
			if (get_pgpid(false) == 0)
				return true;	/* pid file is gone */
			return false;		/* postmaster died untimely */
		}

		if (cnt % WAITS_PER_SEC == 0)
			print_msg(".");
		pg_usleep(USEC_PER_SEC / WAITS_PER_SEC);
	}
	return false;				/* timeout reached */
}


/*
 * Wait for the postmaster to promote.
 *
 * Returns true on success, else false.
 * To avoid waiting uselessly, we check for postmaster death here too.
 */
static bool
wait_for_postmaster_promote(void)
{
	int			cnt;

	for (cnt = 0; cnt < wait_seconds * WAITS_PER_SEC; cnt++)
	{
		pgpid_t		pid;
		DBState		state;

		if ((pid = get_pgpid(false)) == 0)
			return false;		/* pid file is gone */
		if (kill((pid_t) pid, 0) != 0)
			return false;		/* postmaster died */

		state = get_control_dbstate();
		if (state == DB_IN_PRODUCTION)
			return true;		/* successful promotion */

		if (cnt % WAITS_PER_SEC == 0)
			print_msg(".");
		pg_usleep(USEC_PER_SEC / WAITS_PER_SEC);
	}
	return false;				/* timeout reached */
}


#if defined(HAVE_GETRLIMIT) && defined(RLIMIT_CORE)
static void
unlimit_core_size(void)
{
	struct rlimit lim;

	getrlimit(RLIMIT_CORE, &lim);
	if (lim.rlim_max == 0)
	{
		write_stderr(_("%s: cannot set core file size limit; disallowed by hard limit\n"),
					 progname);
		return;
	}
	else if (lim.rlim_max == RLIM_INFINITY || lim.rlim_cur < lim.rlim_max)
	{
		lim.rlim_cur = lim.rlim_max;
		setrlimit(RLIMIT_CORE, &lim);
	}
}
#endif

static void
read_post_opts(void)
{
	if (post_opts == NULL)
	{
		post_opts = "";			/* default */
		if (ctl_command == RESTART_COMMAND)
		{
			char	  **optlines;
			int			numlines;

			optlines = readfile(postopts_file, &numlines);
			if (optlines == NULL)
			{
				write_stderr(_("%s: could not read file \"%s\"\n"), progname, postopts_file);
				exit(1);
			}
			else if (numlines != 1)
			{
				write_stderr(_("%s: option file \"%s\" must have exactly one line\n"),
							 progname, postopts_file);
				exit(1);
			}
			else
			{
				char	   *optline;
				char	   *arg1;

				optline = optlines[0];

				/*
				 * Are we at the first option, as defined by space and
				 * double-quote?
				 */
				if ((arg1 = strstr(optline, " \"")) != NULL)
				{
					*arg1 = '\0';	/* terminate so we get only program name */
					post_opts = pg_strdup(arg1 + 1);	/* point past whitespace */
				}
				if (exec_path == NULL)
					exec_path = pg_strdup(optline);
			}

			/* Free the results of readfile. */
			free_readfile(optlines);
		}
	}
}

/*
 * SIGINT signal handler used while waiting for postmaster to start up.
 * Forwards the SIGINT to the postmaster process, asking it to shut down,
 * before terminating pg_ctl itself. This way, if the user hits CTRL-C while
 * waiting for the server to start up, the server launch is aborted.
 */
static void
trap_sigint_during_startup(int sig)
{
	if (postmasterPID != -1)
	{
		if (kill(postmasterPID, SIGINT) != 0)
			write_stderr(_("%s: could not send stop signal (PID: %ld): %s\n"),
						 progname, (pgpid_t) postmasterPID, strerror(errno));
	}

	/*
	 * Clear the signal handler, and send the signal again, to terminate the
	 * process as normal.
	 */
	pqsignal(SIGINT, SIG_DFL);
	raise(SIGINT);
}

static char *
find_other_exec_or_die(const char *argv0, const char *target, const char *versionstr)
{
	int			ret;
	char	   *found_path;

	found_path = pg_malloc(MAXPGPATH);

	if ((ret = find_other_exec(argv0, target, versionstr, found_path)) < 0)
	{
		char		full_path[MAXPGPATH];

		if (find_my_exec(argv0, full_path) < 0)
			strlcpy(full_path, progname, sizeof(full_path));

		if (ret == -1)
			write_stderr(_("The program \"%s\" is needed by %s but was not found in the\n"
						   "same directory as \"%s\".\n"
						   "Check your installation.\n"),
						 target, progname, full_path);
		else
			write_stderr(_("The program \"%s\" was found by \"%s\"\n"
						   "but was not the same version as %s.\n"
						   "Check your installation.\n"),
						 target, full_path, progname);
		exit(1);
	}

	return found_path;
}

static void
do_init(void)
{
	char	   *cmd;

	if (exec_path == NULL)
		exec_path = find_other_exec_or_die(argv0, "initdb", "initdb (PostgreSQL) " PG_VERSION "\n");

	if (pgdata_opt == NULL)
		pgdata_opt = "";

	if (post_opts == NULL)
		post_opts = "";

	if (!silent_mode)
		cmd = psprintf("\"%s\" %s%s",
					   exec_path, pgdata_opt, post_opts);
	else
		cmd = psprintf("\"%s\" %s%s > \"%s\"",
					   exec_path, pgdata_opt, post_opts, DEVNULL);

	if (system(cmd) != 0)
	{
		write_stderr(_("%s: database system initialization failed\n"), progname);
		exit(1);
	}
}

static void
do_start(void)
{
	pgpid_t		old_pid = 0;
	pgpid_t		pm_pid;

	if (ctl_command != RESTART_COMMAND)
	{
		old_pid = get_pgpid(false);
		if (old_pid != 0)
			write_stderr(_("%s: another server might be running; "
						   "trying to start server anyway\n"),
						 progname);
	}

	read_post_opts();

	/* No -D or -D already added during server start */
	if (ctl_command == RESTART_COMMAND || pgdata_opt == NULL)
		pgdata_opt = "";

	if (exec_path == NULL)
		exec_path = find_other_exec_or_die(argv0, "postgres", PG_BACKEND_VERSIONSTR);

#if defined(HAVE_GETRLIMIT) && defined(RLIMIT_CORE)
	if (allow_core_files)
		unlimit_core_size();
#endif

	/*
	 * If possible, tell the postmaster our parent shell's PID (see the
	 * comments in CreateLockFile() for motivation).
	 */
	{
		char		env_var[32];

		snprintf(env_var, sizeof(env_var), "%d", (int) getppid());
		setenv("PG_GRANDPARENT_PID", env_var, 1);
	}

	pm_pid = start_postmaster();

	if (do_wait)
	{
		/*
		 * If the user interrupts the startup (e.g. with CTRL-C), we'd like to
		 * abort the server launch.  Install a signal handler that will
		 * forward SIGINT to the postmaster process, while we wait.
		 *
		 * (We don't bother to reset the signal handler after the launch, as
		 * we're about to exit, anyway.)
		 */
		postmasterPID = pm_pid;
		pqsignal(SIGINT, trap_sigint_during_startup);

		print_msg(_("waiting for server to start..."));

		switch (wait_for_postmaster_start(pm_pid, false))
		{
			case POSTMASTER_READY:
				print_msg(_(" done\n"));
				print_msg(_("server started\n"));
				break;
			case POSTMASTER_STILL_STARTING:
				print_msg(_(" stopped waiting\n"));
				write_stderr(_("%s: server did not start in time\n"),
							 progname);
				exit(1);
				break;
			case POSTMASTER_FAILED:
				print_msg(_(" stopped waiting\n"));
				write_stderr(_("%s: could not start server\n"
							   "Examine the log output.\n"),
							 progname);
				exit(1);
				break;
		}
	}
	else
		print_msg(_("server starting\n"));
}


static void
do_stop(void)
{
	pgpid_t		pid;
	struct stat statbuf;

	pid = get_pgpid(false);

	if (pid == 0)				/* no pid file */
	{
		write_stderr(_("%s: PID file \"%s\" does not exist\n"), progname, pid_file);
		write_stderr(_("Is server running?\n"));
		exit(1);
	}
	else if (pid < 0)			/* standalone backend, not postmaster */
	{
		pid = -pid;
		write_stderr(_("%s: cannot stop server; "
					   "single-user server is running (PID: %ld)\n"),
					 progname, pid);
		exit(1);
	}

	if (kill((pid_t) pid, sig) != 0)
	{
		write_stderr(_("%s: could not send stop signal (PID: %ld): %s\n"), progname, pid,
					 strerror(errno));
		exit(1);
	}

	if (!do_wait)
	{
		print_msg(_("server shutting down\n"));
		return;
	}
	else
	{
		/*
		 * If backup_label exists, an online backup is running. Warn the user
		 * that smart shutdown will wait for it to finish. However, if the
		 * server is in archive recovery, we're recovering from an online
		 * backup instead of performing one.
		 */
		if (shutdown_mode == SMART_MODE &&
			stat(backup_file, &statbuf) == 0 &&
			get_control_dbstate() != DB_IN_ARCHIVE_RECOVERY)
		{
			print_msg(_("WARNING: online backup mode is active\n"
						"Shutdown will not complete until pg_stop_backup() is called.\n\n"));
		}

		print_msg(_("waiting for server to shut down..."));

		if (!wait_for_postmaster_stop())
		{
			print_msg(_(" failed\n"));

			write_stderr(_("%s: server does not shut down\n"), progname);
			if (shutdown_mode == SMART_MODE)
				write_stderr(_("HINT: The \"-m fast\" option immediately disconnects sessions rather than\n"
							   "waiting for session-initiated disconnection.\n"));
			exit(1);
		}
		print_msg(_(" done\n"));

		print_msg(_("server stopped\n"));
	}
}


/*
 *	restart/reload routines
 */

static void
do_restart(void)
{
	pgpid_t		pid;
	struct stat statbuf;

	pid = get_pgpid(false);

	if (pid == 0)				/* no pid file */
	{
		write_stderr(_("%s: PID file \"%s\" does not exist\n"),
					 progname, pid_file);
		write_stderr(_("Is server running?\n"));
		write_stderr(_("trying to start server anyway\n"));
		do_start();
		return;
	}
	else if (pid < 0)			/* standalone backend, not postmaster */
	{
		pid = -pid;
		if (postmaster_is_alive((pid_t) pid))
		{
			write_stderr(_("%s: cannot restart server; "
						   "single-user server is running (PID: %ld)\n"),
						 progname, pid);
			write_stderr(_("Please terminate the single-user server and try again.\n"));
			exit(1);
		}
	}

	if (postmaster_is_alive((pid_t) pid))
	{
		if (kill((pid_t) pid, sig) != 0)
		{
			write_stderr(_("%s: could not send stop signal (PID: %ld): %s\n"), progname, pid,
						 strerror(errno));
			exit(1);
		}

		/*
		 * If backup_label exists, an online backup is running. Warn the user
		 * that smart shutdown will wait for it to finish. However, if the
		 * server is in archive recovery, we're recovering from an online
		 * backup instead of performing one.
		 */
		if (shutdown_mode == SMART_MODE &&
			stat(backup_file, &statbuf) == 0 &&
			get_control_dbstate() != DB_IN_ARCHIVE_RECOVERY)
		{
			print_msg(_("WARNING: online backup mode is active\n"
						"Shutdown will not complete until pg_stop_backup() is called.\n\n"));
		}

		print_msg(_("waiting for server to shut down..."));

		/* always wait for restart */
		if (!wait_for_postmaster_stop())
		{
			print_msg(_(" failed\n"));

			write_stderr(_("%s: server does not shut down\n"), progname);
			if (shutdown_mode == SMART_MODE)
				write_stderr(_("HINT: The \"-m fast\" option immediately disconnects sessions rather than\n"
							   "waiting for session-initiated disconnection.\n"));
			exit(1);
		}

		print_msg(_(" done\n"));
		print_msg(_("server stopped\n"));
	}
	else
	{
		write_stderr(_("%s: old server process (PID: %ld) seems to be gone\n"),
					 progname, pid);
		write_stderr(_("starting server anyway\n"));
	}

	do_start();
}

static void
do_reload(void)
{
	pgpid_t		pid;

	pid = get_pgpid(false);
	if (pid == 0)				/* no pid file */
	{
		write_stderr(_("%s: PID file \"%s\" does not exist\n"), progname, pid_file);
		write_stderr(_("Is server running?\n"));
		exit(1);
	}
	else if (pid < 0)			/* standalone backend, not postmaster */
	{
		pid = -pid;
		write_stderr(_("%s: cannot reload server; "
					   "single-user server is running (PID: %ld)\n"),
					 progname, pid);
		write_stderr(_("Please terminate the single-user server and try again.\n"));
		exit(1);
	}

	if (kill((pid_t) pid, sig) != 0)
	{
		write_stderr(_("%s: could not send reload signal (PID: %ld): %s\n"),
					 progname, pid, strerror(errno));
		exit(1);
	}

	print_msg(_("server signaled\n"));
}


/*
 * promote
 */

static void
do_promote(void)
{
	FILE	   *prmfile;
	pgpid_t		pid;

	pid = get_pgpid(false);

	if (pid == 0)				/* no pid file */
	{
		write_stderr(_("%s: PID file \"%s\" does not exist\n"), progname, pid_file);
		write_stderr(_("Is server running?\n"));
		exit(1);
	}
	else if (pid < 0)			/* standalone backend, not postmaster */
	{
		pid = -pid;
		write_stderr(_("%s: cannot promote server; "
					   "single-user server is running (PID: %ld)\n"),
					 progname, pid);
		exit(1);
	}

	if (get_control_dbstate() != DB_IN_ARCHIVE_RECOVERY)
	{
		write_stderr(_("%s: cannot promote server; "
					   "server is not in standby mode\n"),
					 progname);
		exit(1);
	}

	snprintf(promote_file, MAXPGPATH, "%s/promote", pg_data);

	if ((prmfile = fopen(promote_file, "w")) == NULL)
	{
		write_stderr(_("%s: could not create promote signal file \"%s\": %s\n"),
					 progname, promote_file, strerror(errno));
		exit(1);
	}
	if (fclose(prmfile))
	{
		write_stderr(_("%s: could not write promote signal file \"%s\": %s\n"),
					 progname, promote_file, strerror(errno));
		exit(1);
	}

	sig = SIGUSR1;
	if (kill((pid_t) pid, sig) != 0)
	{
		write_stderr(_("%s: could not send promote signal (PID: %ld): %s\n"),
					 progname, pid, strerror(errno));
		if (unlink(promote_file) != 0)
			write_stderr(_("%s: could not remove promote signal file \"%s\": %s\n"),
						 progname, promote_file, strerror(errno));
		exit(1);
	}

	if (do_wait)
	{
		print_msg(_("waiting for server to promote..."));
		if (wait_for_postmaster_promote())
		{
			print_msg(_(" done\n"));
			print_msg(_("server promoted\n"));
		}
		else
		{
			print_msg(_(" stopped waiting\n"));
			write_stderr(_("%s: server did not promote in time\n"),
						 progname);
			exit(1);
		}
	}
	else
		print_msg(_("server promoting\n"));
}

/*
 * log rotate
 */

static void
do_logrotate(void)
{
	FILE	   *logrotatefile;
	pgpid_t		pid;

	pid = get_pgpid(false);

	if (pid == 0)				/* no pid file */
	{
		write_stderr(_("%s: PID file \"%s\" does not exist\n"), progname, pid_file);
		write_stderr(_("Is server running?\n"));
		exit(1);
	}
	else if (pid < 0)			/* standalone backend, not postmaster */
	{
		pid = -pid;
		write_stderr(_("%s: cannot rotate log file; "
					   "single-user server is running (PID: %ld)\n"),
					 progname, pid);
		exit(1);
	}

	snprintf(logrotate_file, MAXPGPATH, "%s/logrotate", pg_data);

	if ((logrotatefile = fopen(logrotate_file, "w")) == NULL)
	{
		write_stderr(_("%s: could not create log rotation signal file \"%s\": %s\n"),
					 progname, logrotate_file, strerror(errno));
		exit(1);
	}
	if (fclose(logrotatefile))
	{
		write_stderr(_("%s: could not write log rotation signal file \"%s\": %s\n"),
					 progname, logrotate_file, strerror(errno));
		exit(1);
	}

	sig = SIGUSR1;
	if (kill((pid_t) pid, sig) != 0)
	{
		write_stderr(_("%s: could not send log rotation signal (PID: %ld): %s\n"),
					 progname, pid, strerror(errno));
		if (unlink(logrotate_file) != 0)
			write_stderr(_("%s: could not remove log rotation signal file \"%s\": %s\n"),
						 progname, logrotate_file, strerror(errno));
		exit(1);
	}

	print_msg(_("server signaled to rotate log file\n"));
}


/*
 *	utility routines
 */

static bool
postmaster_is_alive(pid_t pid)
{
	/*
	 * Test to see if the process is still there.  Note that we do not
	 * consider an EPERM failure to mean that the process is still there;
	 * EPERM must mean that the given PID belongs to some other userid, and
	 * considering the permissions on $PGDATA, that means it's not the
	 * postmaster we are after.
	 *
	 * Don't believe that our own PID or parent shell's PID is the postmaster,
	 * either.  (Windows hasn't got getppid(), though.)
	 */
	if (pid == getpid())
		return false;
	if (pid == getppid())
		return false;
	if (kill(pid, 0) == 0)
		return true;
	return false;
}

static void
do_status(void)
{
	pgpid_t		pid;

	pid = get_pgpid(true);
	/* Is there a pid file? */
	if (pid != 0)
	{
		/* standalone backend? */
		if (pid < 0)
		{
			pid = -pid;
			if (postmaster_is_alive((pid_t) pid))
			{
				printf(_("%s: single-user server is running (PID: %ld)\n"),
					   progname, pid);
				return;
			}
		}
		else
			/* must be a postmaster */
		{
			if (postmaster_is_alive((pid_t) pid))
			{
				char	  **optlines;
				char	  **curr_line;
				int			numlines;

				printf(_("%s: server is running (PID: %ld)\n"),
					   progname, pid);

				optlines = readfile(postopts_file, &numlines);
				if (optlines != NULL)
				{
					for (curr_line = optlines; *curr_line != NULL; curr_line++)
						puts(*curr_line);

					/* Free the results of readfile */
					free_readfile(optlines);
				}
				return;
			}
		}
	}
	printf(_("%s: no server running\n"), progname);

	/*
	 * The Linux Standard Base Core Specification 3.1 says this should return
	 * '3, program is not running'
	 * https://refspecs.linuxbase.org/LSB_3.1.0/LSB-Core-generic/LSB-Core-generic/iniscrptact.html
	 */
	exit(3);
}



static void
do_kill(pgpid_t pid)
{
	if (kill((pid_t) pid, sig) != 0)
	{
		write_stderr(_("%s: could not send signal %d (PID: %ld): %s\n"),
					 progname, sig, pid, strerror(errno));
		exit(1);
	}
}


static void
do_advice(void)
{
	write_stderr(_("Try \"%s --help\" for more information.\n"), progname);
}



static void
do_help(void)
{
	printf(_("%s is a utility to initialize, start, stop, or control a PostgreSQL server.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s init[db]   [-D DATADIR] [-s] [-o OPTIONS]\n"), progname);
	printf(_("  %s start      [-D DATADIR] [-l FILENAME] [-W] [-t SECS] [-s]\n"
			 "                    [-o OPTIONS] [-p PATH] [-c]\n"), progname);
	printf(_("  %s stop       [-D DATADIR] [-m SHUTDOWN-MODE] [-W] [-t SECS] [-s]\n"), progname);
	printf(_("  %s restart    [-D DATADIR] [-m SHUTDOWN-MODE] [-W] [-t SECS] [-s]\n"
			 "                    [-o OPTIONS] [-c]\n"), progname);
	printf(_("  %s reload     [-D DATADIR] [-s]\n"), progname);
	printf(_("  %s status     [-D DATADIR]\n"), progname);
	printf(_("  %s promote    [-D DATADIR] [-W] [-t SECS] [-s]\n"), progname);
	printf(_("  %s logrotate  [-D DATADIR] [-s]\n"), progname);
	printf(_("  %s kill       SIGNALNAME PID\n"), progname);

	printf(_("\nCommon options:\n"));
	printf(_("  -D, --pgdata=DATADIR   location of the database storage area\n"));
	printf(_("  -s, --silent           only print errors, no informational messages\n"));
	printf(_("  -t, --timeout=SECS     seconds to wait when using -w option\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("  -w, --wait             wait until operation completes (default)\n"));
	printf(_("  -W, --no-wait          do not wait until operation completes\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("If the -D option is omitted, the environment variable PGDATA is used.\n"));

	printf(_("\nOptions for start or restart:\n"));
#if defined(HAVE_GETRLIMIT) && defined(RLIMIT_CORE)
	printf(_("  -c, --core-files       allow postgres to produce core files\n"));
#else
	printf(_("  -c, --core-files       not applicable on this platform\n"));
#endif
	printf(_("  -l, --log=FILENAME     write (or append) server log to FILENAME\n"));
	printf(_("  -o, --options=OPTIONS  command line options to pass to postgres\n"
			 "                         (PostgreSQL server executable) or initdb\n"));
	printf(_("  -p PATH-TO-POSTGRES    normally not necessary\n"));
	printf(_("\nOptions for stop or restart:\n"));
	printf(_("  -m, --mode=MODE        MODE can be \"smart\", \"fast\", or \"immediate\"\n"));

	printf(_("\nShutdown modes are:\n"));
	printf(_("  smart       quit after all clients have disconnected\n"));
	printf(_("  fast        quit directly, with proper shutdown (default)\n"));
	printf(_("  immediate   quit without complete shutdown; will lead to recovery on restart\n"));

	printf(_("\nAllowed signal names for kill:\n"));
	printf("  ABRT HUP INT KILL QUIT TERM USR1 USR2\n");

	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}



static void
set_mode(char *modeopt)
{
	if (strcmp(modeopt, "s") == 0 || strcmp(modeopt, "smart") == 0)
	{
		shutdown_mode = SMART_MODE;
		sig = SIGTERM;
	}
	else if (strcmp(modeopt, "f") == 0 || strcmp(modeopt, "fast") == 0)
	{
		shutdown_mode = FAST_MODE;
		sig = SIGINT;
	}
	else if (strcmp(modeopt, "i") == 0 || strcmp(modeopt, "immediate") == 0)
	{
		shutdown_mode = IMMEDIATE_MODE;
		sig = SIGQUIT;
	}
	else
	{
		write_stderr(_("%s: unrecognized shutdown mode \"%s\"\n"), progname, modeopt);
		do_advice();
		exit(1);
	}
}



static void
set_sig(char *signame)
{
	if (strcmp(signame, "HUP") == 0)
		sig = SIGHUP;
	else if (strcmp(signame, "INT") == 0)
		sig = SIGINT;
	else if (strcmp(signame, "QUIT") == 0)
		sig = SIGQUIT;
	else if (strcmp(signame, "ABRT") == 0)
		sig = SIGABRT;
	else if (strcmp(signame, "KILL") == 0)
		sig = SIGKILL;
	else if (strcmp(signame, "TERM") == 0)
		sig = SIGTERM;
	else if (strcmp(signame, "USR1") == 0)
		sig = SIGUSR1;
	else if (strcmp(signame, "USR2") == 0)
		sig = SIGUSR2;
	else
	{
		write_stderr(_("%s: unrecognized signal name \"%s\"\n"), progname, signame);
		do_advice();
		exit(1);
	}
}



/*
 * adjust_data_dir
 *
 * If a configuration-only directory was specified, find the real data dir.
 */
static void
adjust_data_dir(void)
{
	char		filename[MAXPGPATH];
	char	   *my_exec_path,
			   *cmd;
	FILE	   *fd;

	/* do nothing if we're working without knowledge of data dir */
	if (pg_config == NULL)
		return;

	/* If there is no postgresql.conf, it can't be a config-only dir */
	snprintf(filename, sizeof(filename), "%s/postgresql.conf", pg_config);
	if ((fd = fopen(filename, "r")) == NULL)
		return;
	fclose(fd);

	/* If PG_VERSION exists, it can't be a config-only dir */
	snprintf(filename, sizeof(filename), "%s/PG_VERSION", pg_config);
	if ((fd = fopen(filename, "r")) != NULL)
	{
		fclose(fd);
		return;
	}

	/* Must be a configuration directory, so find the data directory */

	/* we use a private my_exec_path to avoid interfering with later uses */
	if (exec_path == NULL)
		my_exec_path = find_other_exec_or_die(argv0, "postgres", PG_BACKEND_VERSIONSTR);
	else
		my_exec_path = pg_strdup(exec_path);

	/* it's important for -C to be the first option, see main.c */
	cmd = psprintf("\"%s\" -C data_directory %s%s",
				   my_exec_path,
				   pgdata_opt ? pgdata_opt : "",
				   post_opts ? post_opts : "");

	fd = popen(cmd, "r");
	if (fd == NULL || fgets(filename, sizeof(filename), fd) == NULL)
	{
		write_stderr(_("%s: could not determine the data directory using command \"%s\"\n"), progname, cmd);
		exit(1);
	}
	pclose(fd);
	free(my_exec_path);

	/* strip trailing newline and carriage return */
	(void) pg_strip_crlf(filename);

	free(pg_data);
	pg_data = pg_strdup(filename);
	canonicalize_path(pg_data);
}


static DBState
get_control_dbstate(void)
{
	DBState		ret;
	bool		crc_ok;
	ControlFileData *control_file_data = get_controlfile(pg_data, &crc_ok);

	if (!crc_ok)
	{
		write_stderr(_("%s: control file appears to be corrupt\n"), progname);
		exit(1);
	}

	ret = control_file_data->state;
	pfree(control_file_data);
	return ret;
}


int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},
		{"log", required_argument, NULL, 'l'},
		{"mode", required_argument, NULL, 'm'},
		{"pgdata", required_argument, NULL, 'D'},
		{"options", required_argument, NULL, 'o'},
		{"silent", no_argument, NULL, 's'},
		{"timeout", required_argument, NULL, 't'},
		{"core-files", no_argument, NULL, 'c'},
		{"wait", no_argument, NULL, 'w'},
		{"no-wait", no_argument, NULL, 'W'},
		{NULL, 0, NULL, 0}
	};

	char	   *env_wait;
	int			option_index;
	int			c;
	pgpid_t		killproc = 0;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_ctl"));
	start_time = time(NULL);

	/*
	 * save argv[0] so do_start() can look for the postmaster if necessary. we
	 * don't look for postmaster here because in many cases we won't need it.
	 */
	argv0 = argv[0];

	/* Set restrictive mode mask until PGDATA permissions are checked */
	umask(PG_MODE_MASK_OWNER);

	/* support --help and --version even if invoked as root */
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			do_help();
			exit(0);
		}
		else if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_ctl (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	/*
	 * Disallow running as root, to forestall any possible security holes.
	 */
	if (geteuid() == 0)
	{
		write_stderr(_("%s: cannot be run as root\n"
					   "Please log in (using, e.g., \"su\") as the "
					   "(unprivileged) user that will\n"
					   "own the server process.\n"),
					 progname);
		exit(1);
	}

	env_wait = getenv("PGCTLTIMEOUT");
	if (env_wait != NULL)
		wait_seconds = atoi(env_wait);

	/*
	 * 'Action' can be before or after args so loop over both. Some
	 * getopt_long() implementations will reorder argv[] to place all flags
	 * first (GNU?), but we don't rely on it. Our /port version doesn't do
	 * that.
	 */
	optind = 1;

	/* process command-line options */
	while (optind < argc)
	{
		while ((c = getopt_long(argc, argv, "cD:l:m:o:p:sS:t:wW",
								long_options, &option_index)) != -1)
		{
			switch (c)
			{
			case 'D':
				{
					char	   *pgdata_D;

					pgdata_D = pg_strdup(optarg);
					canonicalize_path(pgdata_D);
					setenv("PGDATA", pgdata_D, 1);

					/*
					 * We could pass PGDATA just in an environment
					 * variable but we do -D too for clearer postmaster
					 * 'ps' display
					 */
					pgdata_opt = psprintf("-D \"%s\" ", pgdata_D);
					free(pgdata_D);
					break;
				}
			case 'e':
				write_stderr(_("%s: -e option not supported on this platform\n"),
							 progname);
				exit(1);
				break;
			case 'l':
				log_file = pg_strdup(optarg);
				break;
			case 'm':
				set_mode(optarg);
				break;
			case 'N':
				write_stderr(_("%s: -N option not supported on this platform\n"),
							 progname);
				exit(1);
				break;
			case 'o':
					/* append option? */
					if (!post_opts)
						post_opts = pg_strdup(optarg);
					else
					{
						char	   *old_post_opts = post_opts;

						post_opts = psprintf("%s %s", old_post_opts, optarg);
						free(old_post_opts);
					}
					break;
				case 'p':
					exec_path = pg_strdup(optarg);
					break;
			case 'P':
				write_stderr(_("%s: -P option not supported on this platform\n"),
							 progname);
				exit(1);
				break;
			case 's':
				silent_mode = true;
				break;
				case 'S':
					write_stderr(_("%s: -S option not supported on this platform\n"),
								 progname);
					exit(1);
					break;
				case 't':
					wait_seconds = atoi(optarg);
					wait_seconds_arg = true;
					break;
			case 'U':
				write_stderr(_("%s: -U option not supported on this platform\n"),
							 progname);
				exit(1);
				break;
			case 'w':
				do_wait = true;
				break;
				case 'W':
					do_wait = false;
					break;
				case 'c':
					allow_core_files = true;
					break;
				default:
					/* getopt_long already issued a suitable error message */
					do_advice();
					exit(1);
			}
		}

		/* Process an action */
		if (optind < argc)
		{
			if (ctl_command != NO_COMMAND)
			{
				write_stderr(_("%s: too many command-line arguments (first is \"%s\")\n"), progname, argv[optind]);
				do_advice();
				exit(1);
			}

			if (strcmp(argv[optind], "init") == 0
				|| strcmp(argv[optind], "initdb") == 0)
				ctl_command = INIT_COMMAND;
			else if (strcmp(argv[optind], "start") == 0)
				ctl_command = START_COMMAND;
			else if (strcmp(argv[optind], "stop") == 0)
				ctl_command = STOP_COMMAND;
			else if (strcmp(argv[optind], "restart") == 0)
				ctl_command = RESTART_COMMAND;
			else if (strcmp(argv[optind], "reload") == 0)
				ctl_command = RELOAD_COMMAND;
			else if (strcmp(argv[optind], "status") == 0)
				ctl_command = STATUS_COMMAND;
			else if (strcmp(argv[optind], "promote") == 0)
				ctl_command = PROMOTE_COMMAND;
			else if (strcmp(argv[optind], "logrotate") == 0)
				ctl_command = LOGROTATE_COMMAND;
			else if (strcmp(argv[optind], "kill") == 0)
			{
				if (argc - optind < 3)
				{
					write_stderr(_("%s: missing arguments for kill mode\n"), progname);
					do_advice();
					exit(1);
				}
				ctl_command = KILL_COMMAND;
				set_sig(argv[++optind]);
				killproc = atol(argv[++optind]);
			}
			else
			{
				write_stderr(_("%s: unrecognized operation mode \"%s\"\n"), progname, argv[optind]);
				do_advice();
				exit(1);
			}
			optind++;
		}
	}

	if (ctl_command == NO_COMMAND)
	{
		write_stderr(_("%s: no operation specified\n"), progname);
		do_advice();
		exit(1);
	}

	/* Note we put any -D switch into the env var above */
	pg_config = getenv("PGDATA");
	if (pg_config)
	{
		pg_config = pg_strdup(pg_config);
		canonicalize_path(pg_config);
		pg_data = pg_strdup(pg_config);
	}

	/* -D might point at config-only directory; if so find the real PGDATA */
	adjust_data_dir();

	/* Complain if -D needed and not provided */
	if (pg_config == NULL &&
		ctl_command != KILL_COMMAND)
	{
		write_stderr(_("%s: no database directory specified and environment variable PGDATA unset\n"),
					 progname);
		do_advice();
		exit(1);
	}

	if (ctl_command == RELOAD_COMMAND)
	{
		sig = SIGHUP;
		do_wait = false;
	}

	if (pg_data)
	{
		snprintf(postopts_file, MAXPGPATH, "%s/postmaster.opts", pg_data);
		snprintf(version_file, MAXPGPATH, "%s/PG_VERSION", pg_data);
		snprintf(pid_file, MAXPGPATH, "%s/postmaster.pid", pg_data);
		snprintf(backup_file, MAXPGPATH, "%s/backup_label", pg_data);

		/*
		 * Set mask based on PGDATA permissions,
		 *
		 * Don't error here if the data directory cannot be stat'd. This is
		 * handled differently based on the command and we don't want to
		 * interfere with that logic.
		 */
		if (GetDataDirectoryCreatePerm(pg_data))
			umask(pg_mode_mask);
	}

	switch (ctl_command)
	{
		case INIT_COMMAND:
			do_init();
			break;
		case STATUS_COMMAND:
			do_status();
			break;
		case START_COMMAND:
			do_start();
			break;
		case STOP_COMMAND:
			do_stop();
			break;
		case RESTART_COMMAND:
			do_restart();
			break;
		case RELOAD_COMMAND:
			do_reload();
			break;
		case PROMOTE_COMMAND:
			do_promote();
			break;
		case LOGROTATE_COMMAND:
			do_logrotate();
			break;
		case KILL_COMMAND:
			do_kill(killproc);
			break;
		default:
			break;
	}

	exit(0);
}
