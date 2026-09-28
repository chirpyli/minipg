/*-------------------------------------------------------------------------
 *
 * main.c
 *	  Stub main() routine for the postgres executable.
 *
 * This does some essential startup tasks for any incarnation of postgres
 * (postmaster, standalone backend, standalone bootstrap process, or a
 * separately exec'd child of a postmaster) and then dispatches to the
 * proper FooMain() routine for the incarnation.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/main/main.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "bootstrap/bootstrap.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "postmaster/postmaster.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"


const char *progname;


static void startup_hacks(const char *progname);
static void help(const char *progname);

/*
 * Any Postgres server process begins execution here.
 */
int
main(int argc, char *argv[])
{
	/*
	 * If supported on the current platform, set up a handler to be called if
	 * the backend/postmaster crashes with a fatal signal or exception.
	 */
	progname = get_progname(argv[0]);

	/*
	 * Platform-specific startup hacks
	 */
	startup_hacks(progname);

	/*
	 * Remember the physical location of the initially given argv[] array for
	 * possible use by ps display.  On some platforms, the argv[] storage must
	 * be overwritten in order to set the process title for ps. In such cases
	 * save_ps_display_args makes and returns a new copy of the argv[] array.
	 *
	 * save_ps_display_args may also move the environment strings to make
	 * extra room. Therefore this should be done as early as possible during
	 * startup, to avoid entanglements with code that might save a getenv()
	 * result pointer.
	 */
	argv = save_ps_display_args(argc, argv);

	/*
	 * Fire up essential subsystems: error and memory management
	 *
	 * Code after this point is allowed to use elog/ereport, though
	 * localization of messages may not work right away, and messages won't go
	 * anywhere but stderr until GUC settings get loaded.
	 */
	MyProcPid = getpid();
	MemoryContextInit();

	/*
	 * Set up the application-specific service directory (PGSYSCONFDIR).
	 *
	 * minipg 已裁剪 locale 与 collation 概念：进程不再吸收环境 locale，
	 * 一律使用 C locale，因此这里只保留服务目录的设置。
	 */
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("postgres"));

	/*
	 * Catch standard options before doing much else, in particular before we
	 * insist on not being root.
	 */
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			fputs(PG_BACKEND_VERSIONSTR, stdout);
			exit(0);
		}
	}

	/*
	 * Dispatch to one of various subprograms depending on first argument.
	 */

	if (argc > 1 && strcmp(argv[1], "--boot") == 0)
		AuxiliaryProcessMain(argc, argv);	/* does not return */
	else if (argc > 1 && strcmp(argv[1], "--single") == 0)
		PostgresMain(argc, argv,
					 NULL,		/* no dbname */
					 strdup("postgres"));	/* does not return */
	else
		PostmasterMain(argc, argv); /* does not return */
	abort();					/* should not get here */
}



/*
 * 在此放置平台相关的启动补丁代码。这里是放置那些必须在任何新的
 * 服务器进程启动早期执行的代码的合适位置。注意，当 fork 出后端进程
 * 或 sub-bootstrap 进程时，这段代码不会被执行。
 *
 * XXX 这里之所以需要代码，本身就证明相关平台太笨，无法在无需帮助的
 * 情况下提供标准的 C 执行环境。如有可能，请避免在此添加更多内容。
 */
static void
startup_hacks(const char *progname)
{
	/*
	 * Initialize dummy_spinlock, in case we are on a platform where we have
	 * to use the fallback implementation of pg_memory_barrier().
	 */
	SpinLockInit(&dummy_spinlock);
}


/*
 * Help display should match the options accepted by PostmasterMain()
 * and PostgresMain().
 *
 * XXX On Windows, non-ASCII localizations of these messages only display
 * correctly if the console output code page covers the necessary characters.
 * Messages emitted in write_console() do not exhibit this problem.
 */
static void
help(const char *progname)
{
	printf(_("%s is the PostgreSQL server.\n\n"), progname);
	printf(_("Usage:\n  %s [OPTION]...\n\n"), progname);
	printf(_("Options:\n"));
	printf(_("  -B NBUFFERS        number of shared buffers\n"));
	printf(_("  -c NAME=VALUE      set run-time parameter\n"));
	printf(_("  -C NAME            print value of run-time parameter, then exit\n"));
	printf(_("  -d 1-5             debugging level\n"));
	printf(_("  -D DATADIR         database directory\n"));
	printf(_("  -e                 use European date input format (DMY)\n"));
	printf(_("  -F                 turn fsync off\n"));
	printf(_("  -h HOSTNAME        host name or IP address to listen on\n"));
	printf(_("  -i                 enable TCP/IP connections\n"));
	printf(_("  -N MAX-CONNECT     maximum number of allowed connections\n"));
	printf(_("  -p PORT            port number to listen on\n"));
	printf(_("  -S WORK-MEM        set amount of memory for sorts (in kB)\n"));
	printf(_("  -V, --version      output version information, then exit\n"));
	printf(_("  --NAME=VALUE       set run-time parameter\n"));
	printf(_("  -?, --help         show this help, then exit\n"));

	printf(_("\nDeveloper options:\n"));
	printf(_("  -f s|i|o|b|t|n|m|h forbid use of some plan types\n"));
	printf(_("  -n                 do not reinitialize shared memory after abnormal exit\n"));
	printf(_("  -O                 allow system table structure changes\n"));
	printf(_("  -P                 disable system indexes\n"));
	printf(_("  -T                 send SIGSTOP to all backend processes if one dies\n"));
	printf(_("  -W NUM             wait NUM seconds to allow attach from a debugger\n"));

	printf(_("\nOptions for single-user mode:\n"));
	printf(_("  --single           selects single-user mode (must be first argument)\n"));
	printf(_("  DBNAME             database name (defaults to user name)\n"));
	printf(_("  -d 0-5             override debugging level\n"));
	printf(_("  -E                 echo statement before execution\n"));
	printf(_("  -j                 do not use newline as interactive query delimiter\n"));
	printf(_("  -r FILENAME        send stdout and stderr to given file\n"));

	printf(_("\nOptions for bootstrapping mode:\n"));
	printf(_("  --boot             selects bootstrapping mode (must be first argument)\n"));
	printf(_("  DBNAME             database name (mandatory argument in bootstrapping mode)\n"));
	printf(_("  -r FILENAME        send stdout and stderr to given file\n"));
	printf(_("  -x NUM             internal use\n"));

	printf(_("\nPlease read the documentation for the complete list of run-time\n"
			 "configuration settings and how to set them on the command line or in\n"
			 "the configuration file.\n\n"
			 "Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}
