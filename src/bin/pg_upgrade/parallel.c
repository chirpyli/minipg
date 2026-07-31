/*
 *	parallel.c
 *
 *	multi-process support
 *
 *	Copyright (c) 2010-2021, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/parallel.c
 */

#include "postgres_fe.h"

#include <sys/wait.h>

#include "pg_upgrade.h"

static int	parallel_jobs;

/*
 *	parallel_exec_prog
 *
 *	This has the same API as exec_prog, except it does parallel execution,
 *	and therefore must throw errors and doesn't return an error status.
 */
void
parallel_exec_prog(const char *log_file, const char *opt_log_file,
				   const char *fmt,...)
{
	va_list		args;
	char		cmd[MAX_STRING];

	pid_t		child;

	va_start(args, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, args);
	va_end(args);

	if (user_opts.jobs <= 1)
		/* exit_on_error must be true to allow jobs */
		exec_prog(log_file, opt_log_file, true, true, "%s", cmd);
	else
	{
		/* parallel */
		/* harvest any dead children */
		while (reap_child(false) == true)
			;

		/* must we wait for a dead child? */
		if (parallel_jobs >= user_opts.jobs)
			reap_child(true);

		/* set this before we start the job */
		parallel_jobs++;

		/* Ensure stdio state is quiesced before forking */
		fflush(NULL);

		child = fork();
		if (child == 0)
			/* use _exit to skip atexit() functions */
			_exit(!exec_prog(log_file, opt_log_file, true, true, "%s", cmd));
		else if (child < 0)
			/* fork failed */
			pg_fatal("could not create worker process: %s\n", strerror(errno));
	}
}


/*
 *	parallel_transfer_all_new_dbs
 *
 *	This has the same API as transfer_all_new_dbs, except it does parallel execution
 *	by transferring multiple tablespaces in parallel
 */
void
parallel_transfer_all_new_dbs(DbInfoArr *old_db_arr, DbInfoArr *new_db_arr,
							  char *old_pgdata, char *new_pgdata,
							  char *old_tablespace)
{
	pid_t		child;

	if (user_opts.jobs <= 1)
		transfer_all_new_dbs(old_db_arr, new_db_arr, old_pgdata, new_pgdata, NULL);
	else
	{
		/* parallel */
		/* harvest any dead children */
		while (reap_child(false) == true)
			;

		/* must we wait for a dead child? */
		if (parallel_jobs >= user_opts.jobs)
			reap_child(true);

		/* set this before we start the job */
		parallel_jobs++;

		/* Ensure stdio state is quiesced before forking */
		fflush(NULL);

		child = fork();
		if (child == 0)
		{
			transfer_all_new_dbs(old_db_arr, new_db_arr, old_pgdata, new_pgdata,
								 old_tablespace);
			/* if we take another exit path, it will be non-zero */
			/* use _exit to skip atexit() functions */
			_exit(0);
		}
		else if (child < 0)
			/* fork failed */
			pg_fatal("could not create worker process: %s\n", strerror(errno));
	}
}


/*
 *	collect status from a completed worker child
 */
bool
reap_child(bool wait_for_child)
{
	int			work_status;
	pid_t		child;

	if (user_opts.jobs <= 1 || parallel_jobs == 0)
		return false;

	child = waitpid(-1, &work_status, wait_for_child ? 0 : WNOHANG);
	if (child == (pid_t) -1)
		pg_fatal("%s() failed: %s\n", "waitpid", strerror(errno));
	if (child == 0)
		return false;			/* no children, or no dead children */
	if (work_status != 0)
		pg_fatal("child process exited abnormally: status %d\n", work_status);

	/* do this after job has been removed */
	parallel_jobs--;

	return true;
}
