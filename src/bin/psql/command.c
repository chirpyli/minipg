/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2021, PostgreSQL Global Development Group
 *
 * src/bin/psql/command.c
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <time.h>
#include <pwd.h>
#include <utime.h>
#include <sys/stat.h>			/* for stat() */
#include <fcntl.h>				/* open() flags */
#include <unistd.h>				/* for geteuid(), getpid(), stat() */

#include "catalog/pg_class_d.h"
#include "command.h"
#include "common.h"
#include "common/logging.h"
#include "common/string.h"
#include "describe.h"
#include "fe_utils/cancel.h"
#include "fe_utils/print.h"
#include "fe_utils/string_utils.h"
#include "input.h"
#include "libpq-fe.h"
#include "libpq/pqcomm.h"
#include "mainloop.h"
#include "pqexpbuffer.h"
#include "psqlscanslash.h"
#include "settings.h"
#include "variables.h"

/* local function declarations */
static backslashResult exec_command(const char *cmd,
									PsqlScanState scan_state,
									ConditionalStack cstack,
									PQExpBuffer query_buf,
									PQExpBuffer previous_buf);
static backslashResult exec_command_a(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_connect(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_d(PsqlScanState scan_state, bool active_branch,
									  const char *cmd);
static backslashResult exec_command_echo(PsqlScanState scan_state, bool active_branch,
										 const char *cmd);
static backslashResult exec_command_elif(PsqlScanState scan_state, ConditionalStack cstack,
										 PQExpBuffer query_buf);
static backslashResult exec_command_else(PsqlScanState scan_state, ConditionalStack cstack,
										 PQExpBuffer query_buf);
static backslashResult exec_command_endif(PsqlScanState scan_state, ConditionalStack cstack,
										  PQExpBuffer query_buf);
static backslashResult exec_command_g(PsqlScanState scan_state, bool active_branch,
									  const char *cmd);
static backslashResult exec_command_gset(PsqlScanState scan_state, bool active_branch);
static backslashResult process_command_g_options(char *first_option,
												 PsqlScanState scan_state,
												 bool active_branch,
												 const char *cmd);
static backslashResult exec_command_include(PsqlScanState scan_state, bool active_branch,
											const char *cmd);
static backslashResult exec_command_if(PsqlScanState scan_state, ConditionalStack cstack,
									   PQExpBuffer query_buf);
static backslashResult exec_command_list(PsqlScanState scan_state, bool active_branch,
										 const char *cmd);
static backslashResult exec_command_pset(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_quit(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_set(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_t(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_unset(PsqlScanState scan_state, bool active_branch,
										  const char *cmd);
static backslashResult exec_command_x(PsqlScanState scan_state, bool active_branch);
static char *read_connect_arg(PsqlScanState scan_state);
static PQExpBuffer gather_boolean_expression(PsqlScanState scan_state);
static bool is_true_boolean_expression(PsqlScanState scan_state, const char *name);
static void ignore_boolean_expression(PsqlScanState scan_state);
static void ignore_slash_options(PsqlScanState scan_state);
static bool is_branching_command(const char *cmd);
static void save_query_text_state(PsqlScanState scan_state, ConditionalStack cstack,
								  PQExpBuffer query_buf);
static void discard_query_text(PsqlScanState scan_state, ConditionalStack cstack,
							   PQExpBuffer query_buf);
static bool copy_previous_query(PQExpBuffer query_buf, PQExpBuffer previous_buf);
static bool do_connect(enum trivalue reuse_previous_specification,
					   char *dbname, char *user, char *host, char *port);

static bool printPsetInfo(const char *param, printQueryOpt *popt);
static char *pset_value_string(const char *param, printQueryOpt *popt);



/*----------
 * HandleSlashCmds:
 *
 * Handles all the different commands that start with '\'.
 * Ordinarily called by MainLoop().
 *
 * scan_state is a lexer working state that is set to continue scanning
 * just after the '\'.  The lexer is advanced past the command and all
 * arguments on return.
 *
 * cstack is the current \if stack state.  This will be examined, and
 * possibly modified by conditional commands.
 *
 * query_buf contains the query-so-far, which may be modified by
 * execution of the backslash command (for example, \r clears it).
 *
 * previous_buf contains the query most recently sent to the server
 * (empty if none yet).  This should not be modified here, but some
 * commands copy its content into query_buf.
 *
 * query_buf and previous_buf will be NULL when executing a "-c"
 * command-line option.
 *
 * Returns a status code indicating what action is desired, see command.h.
 *----------
 */

backslashResult
HandleSlashCmds(PsqlScanState scan_state,
				ConditionalStack cstack,
				PQExpBuffer query_buf,
				PQExpBuffer previous_buf)
{
	backslashResult status;
	char	   *cmd;
	char	   *arg;

	Assert(scan_state != NULL);
	Assert(cstack != NULL);

	/* Parse off the command name */
	cmd = psql_scan_slash_command(scan_state);

	/*
	 * And try to execute it.
	 *
	 */
	status = exec_command(cmd, scan_state, cstack, query_buf, previous_buf);

	if (status == PSQL_CMD_UNKNOWN)
	{
		pg_log_error("invalid command \\%s", cmd);
		if (pset.cur_cmd_interactive)
			pg_log_info("Try \\? for help.");
		status = PSQL_CMD_ERROR;
	}

	if (status != PSQL_CMD_ERROR)
	{
		/*
		 * Eat any remaining arguments after a valid command.  We want to
		 * suppress evaluation of backticks in this situation, so transiently
		 * push an inactive conditional-stack entry.
		 */
		bool		active_branch = conditional_active(cstack);

		conditional_stack_push(cstack, IFSTATE_IGNORED);
		while ((arg = psql_scan_slash_option(scan_state,
											 OT_NORMAL, NULL, false)))
		{
			if (active_branch)
				pg_log_warning("\\%s: extra argument \"%s\" ignored", cmd, arg);
			free(arg);
		}
		conditional_stack_pop(cstack);
	}
	else
	{
		/* silently throw away rest of line after an erroneous command */
		while ((arg = psql_scan_slash_option(scan_state,
											 OT_WHOLE_LINE, NULL, false)))
			free(arg);
	}

	/* if there is a trailing \\, swallow it */
	psql_scan_slash_command_end(scan_state);

	free(cmd);

	/* some commands write to queryFout, so make sure output is sent */
	fflush(pset.queryFout);

	return status;
}


/*
 * Subroutine to actually try to execute a backslash command.
 *
 * The typical "success" result code is PSQL_CMD_SKIP_LINE, although some
 * commands return something else.  Failure results are PSQL_CMD_ERROR,
 * unless PSQL_CMD_UNKNOWN is more appropriate.
 */
static backslashResult
exec_command(const char *cmd,
			 PsqlScanState scan_state,
			 ConditionalStack cstack,
			 PQExpBuffer query_buf,
			 PQExpBuffer previous_buf)
{
	backslashResult status;
	bool		active_branch = conditional_active(cstack);

	/*
	 * In interactive mode, warn when we're ignoring a command within a false
	 * \if-branch.  But we continue on, so as to parse and discard the right
	 * amount of parameter text.  Each individual backslash command subroutine
	 * is responsible for doing nothing after discarding appropriate
	 * arguments, if !active_branch.
	 */
	if (pset.cur_cmd_interactive && !active_branch &&
		!is_branching_command(cmd))
	{
		pg_log_warning("\\%s command ignored; use \\endif or Ctrl-C to exit current \\if block",
					   cmd);
	}

	if (strcmp(cmd, "a") == 0)
		status = exec_command_a(scan_state, active_branch);
	else if (strcmp(cmd, "c") == 0 || strcmp(cmd, "connect") == 0)
		status = exec_command_connect(scan_state, active_branch);
	else if (cmd[0] == 'd')
		status = exec_command_d(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "echo") == 0 || strcmp(cmd, "qecho") == 0 ||
			 strcmp(cmd, "warn") == 0)
		status = exec_command_echo(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "elif") == 0)
		status = exec_command_elif(scan_state, cstack, query_buf);
	else if (strcmp(cmd, "else") == 0)
		status = exec_command_else(scan_state, cstack, query_buf);
	else if (strcmp(cmd, "endif") == 0)
		status = exec_command_endif(scan_state, cstack, query_buf);
	else if (strcmp(cmd, "g") == 0 || strcmp(cmd, "gx") == 0)
		status = exec_command_g(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "gset") == 0)
		status = exec_command_gset(scan_state, active_branch);
	else if (strcmp(cmd, "i") == 0 || strcmp(cmd, "include") == 0 ||
			 strcmp(cmd, "ir") == 0 || strcmp(cmd, "include_relative") == 0)
		status = exec_command_include(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "if") == 0)
		status = exec_command_if(scan_state, cstack, query_buf);
	else if (strcmp(cmd, "l") == 0 || strcmp(cmd, "list") == 0 ||
			 strcmp(cmd, "l+") == 0 || strcmp(cmd, "list+") == 0)
		status = exec_command_list(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "pset") == 0)
		status = exec_command_pset(scan_state, active_branch);
	else if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0)
		status = exec_command_quit(scan_state, active_branch);
	else if (strcmp(cmd, "set") == 0)
		status = exec_command_set(scan_state, active_branch);
	else if (strcmp(cmd, "t") == 0)
		status = exec_command_t(scan_state, active_branch);
	else if (strcmp(cmd, "unset") == 0)
		status = exec_command_unset(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "x") == 0)
		status = exec_command_x(scan_state, active_branch);
	else
		status = PSQL_CMD_UNKNOWN;

	/*
	 * All the commands that return PSQL_CMD_SEND want to execute previous_buf
	 * if query_buf is empty.  For convenience we implement that here, not in
	 * the individual command subroutines.
	 */
	if (status == PSQL_CMD_SEND)
		(void) copy_previous_query(query_buf, previous_buf);

	return status;
}


/*
 * \a -- toggle field alignment
 *
 * This makes little sense but we keep it around.
 */
static backslashResult
exec_command_a(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		if (pset.popt.topt.format != PRINT_ALIGNED)
			success = do_pset("format", "aligned", &pset.popt, pset.quiet);
		else
			success = do_pset("format", "unaligned", &pset.popt, pset.quiet);
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \C -- override table title (formerly change HTML caption)
 */
static backslashResult
exec_command_connect(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		static const char prefix[] = "-reuse-previous=";
		char	   *opt1,
				   *opt2,
				   *opt3,
				   *opt4;
		enum trivalue reuse_previous = TRI_DEFAULT;

		opt1 = read_connect_arg(scan_state);
		if (opt1 != NULL && strncmp(opt1, prefix, sizeof(prefix) - 1) == 0)
		{
			bool		on_off;

			success = ParseVariableBool(opt1 + sizeof(prefix) - 1,
										"-reuse-previous",
										&on_off);
			if (success)
			{
				reuse_previous = on_off ? TRI_YES : TRI_NO;
				free(opt1);
				opt1 = read_connect_arg(scan_state);
			}
		}

		if (success)			/* give up if reuse_previous was invalid */
		{
			opt2 = read_connect_arg(scan_state);
			opt3 = read_connect_arg(scan_state);
			opt4 = read_connect_arg(scan_state);

			success = do_connect(reuse_previous, opt1, opt2, opt3, opt4);

			free(opt2);
			free(opt3);
			free(opt4);
		}
		free(opt1);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \cd -- change directory
 */
static backslashResult
exec_command_d(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;
	bool		success = true;

	if (active_branch)
	{
		char	   *pattern;
		bool		show_verbose,
					show_system;

		/* We don't do SQLID reduction on the pattern yet */
		pattern = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, true);

		show_verbose = strchr(cmd, '+') ? true : false;
		show_system = strchr(cmd, 'S') ? true : false;

		switch (cmd[1])
		{
			case '\0':
			case '+':
			case 'S':
				if (pattern)
					success = describeTableDetails(pattern, show_verbose, show_system);
				else
					/* standard listing of interesting things */
					success = listTables("tvms", NULL, show_verbose, show_system);
				break;
			case 't':
			case 'v':
			case 'm':
			case 'i':
			case 's':
				success = listTables(&cmd[1], pattern, show_verbose, show_system);
				break;
		default:
				status = PSQL_CMD_UNKNOWN;
		}

		if (pattern)
			free(pattern);
	}
	else
		ignore_slash_options(scan_state);

	if (!success)
		status = PSQL_CMD_ERROR;

	return status;
}

static backslashResult
exec_command_echo(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	if (active_branch)
	{
		char	   *value;
		char		quoted;
		bool		no_newline = false;
		bool		first = true;
		FILE	   *fout;

		if (strcmp(cmd, "qecho") == 0)
			fout = pset.queryFout;
		else if (strcmp(cmd, "warn") == 0)
			fout = stderr;
		else
			fout = stdout;

		while ((value = psql_scan_slash_option(scan_state,
											   OT_NORMAL, &quoted, false)))
		{
			if (first && !no_newline && !quoted && strcmp(value, "-n") == 0)
				no_newline = true;
			else
			{
				if (first)
					first = false;
				else
					fputc(' ', fout);
				fputs(value, fout);
			}
			free(value);
		}
		if (!no_newline)
			fputs("\n", fout);
	}
	else
		ignore_slash_options(scan_state);

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \encoding -- set/show client side encoding
 */
static backslashResult
exec_command_g(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;
	char	   *fname;

	/*
	 * Because the option processing for this is fairly complicated, we do it
	 * and then decide whether the branch is active.
	 */
	fname = psql_scan_slash_option(scan_state,
								   OT_FILEPIPE, NULL, false);

	if (fname && fname[0] == '(')
	{
		/* Consume pset options through trailing ')' ... */
		status = process_command_g_options(fname + 1, scan_state,
										   active_branch, cmd);
		free(fname);
		/* ... and again attempt to scan the filename. */
		fname = psql_scan_slash_option(scan_state,
									   OT_FILEPIPE, NULL, false);
	}

	if (status == PSQL_CMD_SKIP_LINE && active_branch)
	{
		if (!fname)
			pset.gfname = NULL;
		else
		{
			expand_tilde(&fname);
			pset.gfname = pg_strdup(fname);
		}
		if (strcmp(cmd, "gx") == 0)
		{
			/* save settings if not done already, then force expanded=on */
			if (pset.gsavepopt == NULL)
				pset.gsavepopt = savePsetInfo(&pset.popt);
			pset.popt.topt.expanded = 1;
		}
		status = PSQL_CMD_SEND;
	}

	free(fname);

	return status;
}

/*
 * Process parenthesized pset options for \g
 *
 * Note: okay to modify first_option, but not to free it; caller does that
 */
static backslashResult
process_command_g_options(char *first_option, PsqlScanState scan_state,
						  bool active_branch, const char *cmd)
{
	bool		success = true;
	bool		found_r_paren = false;

	do
	{
		char	   *option;
		size_t		optlen;

		/* If not first time through, collect a new option */
		if (first_option)
			option = first_option;
		else
		{
			option = psql_scan_slash_option(scan_state,
											OT_NORMAL, NULL, false);
			if (!option)
			{
				if (active_branch)
				{
					pg_log_error("\\%s: missing right parenthesis", cmd);
					success = false;
				}
				break;
			}
		}

		/* Check for terminating right paren, and remove it from string */
		optlen = strlen(option);
		if (optlen > 0 && option[optlen - 1] == ')')
		{
			option[--optlen] = '\0';
			found_r_paren = true;
		}

		/* If there was anything besides parentheses, parse/execute it */
		if (optlen > 0)
		{
			/* We can have either "name" or "name=value" */
			char	   *valptr = strchr(option, '=');

			if (valptr)
				*valptr++ = '\0';
			if (active_branch)
			{
				/* save settings if not done already, then apply option */
				if (pset.gsavepopt == NULL)
					pset.gsavepopt = savePsetInfo(&pset.popt);
				success &= do_pset(option, valptr, &pset.popt, true);
			}
		}

		/* Clean up after this option.  We should not free first_option. */
		if (first_option)
			first_option = NULL;
		else
			free(option);
	} while (!found_r_paren);

	/* If we failed after already changing some options, undo side-effects */
	if (!success && active_branch && pset.gsavepopt)
	{
		restorePsetInfo(&pset.popt, pset.gsavepopt);
		pset.gsavepopt = NULL;
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \gset [prefix] -- send query and store result into variables
 */
static backslashResult
exec_command_gset(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		char	   *prefix = psql_scan_slash_option(scan_state,
												   OT_NORMAL, NULL, false);

		if (prefix)
			pset.gset_prefix = prefix;
		else
		{
			/* we must set a non-NULL prefix to trigger storing */
			pset.gset_prefix = pg_strdup("");
		}
		/* gset_prefix is freed later */
		status = PSQL_CMD_SEND;
	}
	else
		ignore_slash_options(scan_state);

	return status;
}

/*
 * \i and \ir -- include a file
 */
static backslashResult
exec_command_include(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *fname = psql_scan_slash_option(scan_state,
												   OT_NORMAL, NULL, true);

		if (!fname)
		{
			pg_log_error("\\%s: missing required argument", cmd);
			success = false;
		}
		else
		{
			bool		include_relative;

			include_relative = (strcmp(cmd, "ir") == 0
								|| strcmp(cmd, "include_relative") == 0);
			expand_tilde(&fname);
			success = (process_file(fname, include_relative) == EXIT_SUCCESS);
			free(fname);
		}
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \if <expr> -- beginning of an \if..\endif block
 *
 * <expr> is parsed as a boolean expression.  Invalid expressions will emit a
 * warning and be treated as false.  Statements that follow a false expression
 * will be parsed but ignored.  Note that in the case where an \if statement
 * is itself within an inactive section of a block, then the entire inner
 * \if..\endif block will be parsed but ignored.
 */
static backslashResult
exec_command_if(PsqlScanState scan_state, ConditionalStack cstack,
				PQExpBuffer query_buf)
{
	if (conditional_active(cstack))
	{
		/*
		 * First, push a new active stack entry; this ensures that the lexer
		 * will perform variable substitution and backtick evaluation while
		 * scanning the expression.  (That should happen anyway, since we know
		 * we're in an active outer branch, but let's be sure.)
		 */
		conditional_stack_push(cstack, IFSTATE_TRUE);

		/* Remember current query state in case we need to restore later */
		save_query_text_state(scan_state, cstack, query_buf);

		/*
		 * Evaluate the expression; if it's false, change to inactive state.
		 */
		if (!is_true_boolean_expression(scan_state, "\\if expression"))
			conditional_stack_poke(cstack, IFSTATE_FALSE);
	}
	else
	{
		/*
		 * We're within an inactive outer branch, so this entire \if block
		 * will be ignored.  We don't want to evaluate the expression, so push
		 * the "ignored" stack state before scanning it.
		 */
		conditional_stack_push(cstack, IFSTATE_IGNORED);

		/* Remember current query state in case we need to restore later */
		save_query_text_state(scan_state, cstack, query_buf);

		ignore_boolean_expression(scan_state);
	}

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \elif <expr> -- alternative branch in an \if..\endif block
 *
 * <expr> is evaluated the same as in \if <expr>.
 */
static backslashResult
exec_command_elif(PsqlScanState scan_state, ConditionalStack cstack,
				  PQExpBuffer query_buf)
{
	bool		success = true;

	switch (conditional_stack_peek(cstack))
	{
		case IFSTATE_TRUE:

			/*
			 * Just finished active branch of this \if block.  Update saved
			 * state so we will keep whatever data was put in query_buf by the
			 * active branch.
			 */
			save_query_text_state(scan_state, cstack, query_buf);

			/*
			 * Discard \elif expression and ignore the rest until \endif.
			 * Switch state before reading expression to ensure proper lexer
			 * behavior.
			 */
			conditional_stack_poke(cstack, IFSTATE_IGNORED);
			ignore_boolean_expression(scan_state);
			break;
		case IFSTATE_FALSE:

			/*
			 * Discard any query text added by the just-skipped branch.
			 */
			discard_query_text(scan_state, cstack, query_buf);

			/*
			 * Have not yet found a true expression in this \if block, so this
			 * might be the first.  We have to change state before examining
			 * the expression, or the lexer won't do the right thing.
			 */
			conditional_stack_poke(cstack, IFSTATE_TRUE);
			if (!is_true_boolean_expression(scan_state, "\\elif expression"))
				conditional_stack_poke(cstack, IFSTATE_FALSE);
			break;
		case IFSTATE_IGNORED:

			/*
			 * Discard any query text added by the just-skipped branch.
			 */
			discard_query_text(scan_state, cstack, query_buf);

			/*
			 * Skip expression and move on.  Either the \if block already had
			 * an active section, or whole block is being skipped.
			 */
			ignore_boolean_expression(scan_state);
			break;
		case IFSTATE_ELSE_TRUE:
		case IFSTATE_ELSE_FALSE:
			pg_log_error("\\elif: cannot occur after \\else");
			success = false;
			break;
		case IFSTATE_NONE:
			/* no \if to elif from */
			pg_log_error("\\elif: no matching \\if");
			success = false;
			break;
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \else -- final alternative in an \if..\endif block
 *
 * Statements within an \else branch will only be executed if
 * all previous \if and \elif expressions evaluated to false
 * and the block was not itself being ignored.
 */
static backslashResult
exec_command_else(PsqlScanState scan_state, ConditionalStack cstack,
				  PQExpBuffer query_buf)
{
	bool		success = true;

	switch (conditional_stack_peek(cstack))
	{
		case IFSTATE_TRUE:

			/*
			 * Just finished active branch of this \if block.  Update saved
			 * state so we will keep whatever data was put in query_buf by the
			 * active branch.
			 */
			save_query_text_state(scan_state, cstack, query_buf);

			/* Now skip the \else branch */
			conditional_stack_poke(cstack, IFSTATE_ELSE_FALSE);
			break;
		case IFSTATE_FALSE:

			/*
			 * Discard any query text added by the just-skipped branch.
			 */
			discard_query_text(scan_state, cstack, query_buf);

			/*
			 * We've not found any true \if or \elif expression, so execute
			 * the \else branch.
			 */
			conditional_stack_poke(cstack, IFSTATE_ELSE_TRUE);
			break;
		case IFSTATE_IGNORED:

			/*
			 * Discard any query text added by the just-skipped branch.
			 */
			discard_query_text(scan_state, cstack, query_buf);

			/*
			 * Either we previously processed the active branch of this \if,
			 * or the whole \if block is being skipped.  Either way, skip the
			 * \else branch.
			 */
			conditional_stack_poke(cstack, IFSTATE_ELSE_FALSE);
			break;
		case IFSTATE_ELSE_TRUE:
		case IFSTATE_ELSE_FALSE:
			pg_log_error("\\else: cannot occur after \\else");
			success = false;
			break;
		case IFSTATE_NONE:
			/* no \if to else from */
			pg_log_error("\\else: no matching \\if");
			success = false;
			break;
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \endif -- ends an \if...\endif block
 */
static backslashResult
exec_command_endif(PsqlScanState scan_state, ConditionalStack cstack,
				   PQExpBuffer query_buf)
{
	bool		success = true;

	switch (conditional_stack_peek(cstack))
	{
		case IFSTATE_TRUE:
		case IFSTATE_ELSE_TRUE:
			/* Close the \if block, keeping the query text */
			success = conditional_stack_pop(cstack);
			Assert(success);
			break;
		case IFSTATE_FALSE:
		case IFSTATE_IGNORED:
		case IFSTATE_ELSE_FALSE:

			/*
			 * Discard any query text added by the just-skipped branch.
			 */
			discard_query_text(scan_state, cstack, query_buf);

			/* Close the \if block */
			success = conditional_stack_pop(cstack);
			Assert(success);
			break;
		case IFSTATE_NONE:
			/* no \if to end */
			pg_log_error("\\endif: no matching \\if");
			success = false;
			break;
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \l -- list databases
 */
static backslashResult
exec_command_list(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *pattern;
		bool		show_verbose;

		pattern = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, true);

		show_verbose = strchr(cmd, '+') ? true : false;

		success = listAllDbs(pattern, show_verbose);

		if (pattern)
			free(pattern);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \o -- set query output
 */
static backslashResult
exec_command_pset(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt0 = psql_scan_slash_option(scan_state,
												  OT_NORMAL, NULL, false);
		char	   *opt1 = psql_scan_slash_option(scan_state,
												  OT_NORMAL, NULL, false);

		if (!opt0)
		{
			/* list all variables */

			int			i;
			static const char *const my_list[] = {
				"border", "columns", "csv_fieldsep", "expanded", "fieldsep",
				"fieldsep_zero", "footer", "format", "linestyle", "null",
				"numericlocale", "pager", "pager_min_lines",
				"recordsep", "recordsep_zero",
				"title", "tuples_only",
				"unicode_border_linestyle",
				"unicode_column_linestyle",
				"unicode_header_linestyle",
				NULL
			};

			for (i = 0; my_list[i] != NULL; i++)
			{
				char	   *val = pset_value_string(my_list[i], &pset.popt);

				printf("%-24s %s\n", my_list[i], val);
				free(val);
			}

			success = true;
		}
		else
			success = do_pset(opt0, opt1, &pset.popt, pset.quiet);

		free(opt0);
		free(opt1);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \q or \quit -- exit psql
 */
static backslashResult
exec_command_quit(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
		status = PSQL_CMD_TERMINATE;

	return status;
}

/*
 * \r -- reset (clear) the query buffer
 */
static backslashResult
exec_command_set(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt0 = psql_scan_slash_option(scan_state,
												  OT_NORMAL, NULL, false);

		if (!opt0)
		{
			/* list all variables */
			PrintVariables(pset.vars);
			success = true;
		}
		else
		{
			/*
			 * Set variable to the concatenation of the arguments.
			 */
			char	   *newval;
			char	   *opt;

			opt = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, false);
			newval = pg_strdup(opt ? opt : "");
			free(opt);

			while ((opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, false)))
			{
				newval = pg_realloc(newval, strlen(newval) + strlen(opt) + 1);
				strcat(newval, opt);
				free(opt);
			}

			if (!SetVariable(pset.vars, opt0, newval))
				success = false;

			free(newval);
		}
		free(opt0);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \setenv -- set environment variable
 */
static backslashResult
exec_command_t(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, true);

		success = do_pset("tuples_only", opt, &pset.popt, pset.quiet);
		free(opt);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \timing -- enable/disable timing of queries
 */
static backslashResult
exec_command_unset(PsqlScanState scan_state, bool active_branch,
				   const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, false);

		if (!opt)
		{
			pg_log_error("\\%s: missing required argument", cmd);
			success = false;
		}
		else if (!SetVariable(pset.vars, opt, NULL))
			success = false;

		free(opt);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \w -- write query buffer to file
 */
static backslashResult
exec_command_x(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, true);

		success = do_pset("expanded", opt, &pset.popt, pset.quiet);
		free(opt);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}


/*
 * \! -- execute shell command
 */
static char *
read_connect_arg(PsqlScanState scan_state)
{
	char	   *result;
	char		quote;

	/*
	 * Ideally we should treat the arguments as SQL identifiers.  But for
	 * backwards compatibility with 7.2 and older pg_dump files, we have to
	 * take unquoted arguments verbatim (don't downcase them). For now,
	 * double-quoted arguments may be stripped of double quotes (as if SQL
	 * identifiers).  By 7.4 or so, pg_dump files can be expected to
	 * double-quote all mixed-case \connect arguments, and then we can get rid
	 * of OT_SQLIDHACK.
	 */
	result = psql_scan_slash_option(scan_state, OT_SQLIDHACK, &quote, true);

	if (!result)
		return NULL;

	if (quote)
		return result;

	if (*result == '\0' || strcmp(result, "-") == 0)
	{
		free(result);
		return NULL;
	}

	return result;
}

/*
 * Read a boolean expression, return it as a PQExpBuffer string.
 *
 * Note: anything more or less than one token will certainly fail to be
 * parsed by ParseVariableBool, so we don't worry about complaining here.
 * This routine's return data structure will need to be rethought anyway
 * to support likely future extensions such as "\if defined VARNAME".
 */
static PQExpBuffer
gather_boolean_expression(PsqlScanState scan_state)
{
	PQExpBuffer exp_buf = createPQExpBuffer();
	int			num_options = 0;
	char	   *value;

	/* collect all arguments for the conditional command into exp_buf */
	while ((value = psql_scan_slash_option(scan_state,
										   OT_NORMAL, NULL, false)) != NULL)
	{
		/* add spaces between tokens */
		if (num_options > 0)
			appendPQExpBufferChar(exp_buf, ' ');
		appendPQExpBufferStr(exp_buf, value);
		num_options++;
		free(value);
	}

	return exp_buf;
}

/*
 * Read a boolean expression, return true if the expression
 * was a valid boolean expression that evaluated to true.
 * Otherwise return false.
 *
 * Note: conditional stack's top state must be active, else lexer will
 * fail to expand variables and backticks.
 */
static bool
is_true_boolean_expression(PsqlScanState scan_state, const char *name)
{
	PQExpBuffer buf = gather_boolean_expression(scan_state);
	bool		value = false;
	bool		success = ParseVariableBool(buf->data, name, &value);

	destroyPQExpBuffer(buf);
	return success && value;
}

/*
 * Read a boolean expression, but do nothing with it.
 *
 * Note: conditional stack's top state must be INACTIVE, else lexer will
 * expand variables and backticks, which we do not want here.
 */
static void
ignore_boolean_expression(PsqlScanState scan_state)
{
	PQExpBuffer buf = gather_boolean_expression(scan_state);

	destroyPQExpBuffer(buf);
}

/*
 * Read and discard "normal" slash command options.
 *
 * This should be used for inactive-branch processing of any slash command
 * that eats one or more OT_NORMAL, OT_SQLID, or OT_SQLIDHACK parameters.
 * We don't need to worry about exactly how many it would eat, since the
 * cleanup logic in HandleSlashCmds would silently discard any extras anyway.
 */
static void
ignore_slash_options(PsqlScanState scan_state)
{
	char	   *arg;

	while ((arg = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, false)) != NULL)
		free(arg);
}

/*
 * Read and discard FILEPIPE slash command argument.
 *
 * This *MUST* be used for inactive-branch processing of any slash command
 * that takes an OT_FILEPIPE option.  Otherwise we might consume a different
 * amount of option text in active and inactive cases.
 */
static bool
is_branching_command(const char *cmd)
{
	return (strcmp(cmd, "if") == 0 ||
			strcmp(cmd, "elif") == 0 ||
			strcmp(cmd, "else") == 0 ||
			strcmp(cmd, "endif") == 0);
}

/*
 * Prepare to possibly restore query buffer to its current state
 * (cf. discard_query_text).
 *
 * We need to remember the length of the query buffer, and the lexer's
 * notion of the parenthesis nesting depth.
 */
static void
save_query_text_state(PsqlScanState scan_state, ConditionalStack cstack,
					  PQExpBuffer query_buf)
{
	if (query_buf)
		conditional_stack_set_query_len(cstack, query_buf->len);
	conditional_stack_set_paren_depth(cstack,
									  psql_scan_get_paren_depth(scan_state));
}

/*
 * Discard any query text absorbed during an inactive conditional branch.
 *
 * We must discard data that was appended to query_buf during an inactive
 * \if branch.  We don't have to do anything there if there's no query_buf.
 *
 * Also, reset the lexer state to the same paren depth there was before.
 * (The rest of its state doesn't need attention, since we could not be
 * inside a comment or literal or partial token.)
 */
static void
discard_query_text(PsqlScanState scan_state, ConditionalStack cstack,
				   PQExpBuffer query_buf)
{
	if (query_buf)
	{
		int			new_len = conditional_stack_get_query_len(cstack);

		Assert(new_len >= 0 && new_len <= query_buf->len);
		query_buf->len = new_len;
		query_buf->data[new_len] = '\0';
	}
	psql_scan_set_paren_depth(scan_state,
							  conditional_stack_get_paren_depth(cstack));
}

/*
 * If query_buf is empty, copy previous_buf into it.
 *
 * This is used by various slash commands for which re-execution of a
 * previous query is a common usage.  For convenience, we allow the
 * case of query_buf == NULL (and do nothing).
 *
 * Returns "true" if the previous query was copied into the query
 * buffer, else "false".
 */
static bool
copy_previous_query(PQExpBuffer query_buf, PQExpBuffer previous_buf)
{
	if (query_buf && query_buf->len == 0)
	{
		appendPQExpBufferStr(query_buf, previous_buf->data);
		return true;
	}
	return false;
}

/*
 * Ask the user for a password; 'username' is the username the
 * password is for, if one has been explicitly specified. Returns a
 * malloc'd string.
 */
static char *
prompt_for_password(const char *username)
{
	char	   *result;

	if (username == NULL || username[0] == '\0')
		result = simple_prompt("Password: ", false);
	else
	{
		char	   *prompt_text;

		prompt_text = psprintf(_("Password for user %s: "), username);
		result = simple_prompt(prompt_text, false);
		free(prompt_text);
	}
	return result;
}

static bool
param_is_newly_set(const char *old_val, const char *new_val)
{
	if (new_val == NULL)
		return false;

	if (old_val == NULL || strcmp(old_val, new_val) != 0)
		return true;

	return false;
}

/*
 * do_connect -- handler for \connect
 *
 * Connects to a database with given parameters.  If we are told to re-use
 * parameters, parameters from the previous connection are used where the
 * command's own options do not supply a value.  Otherwise, libpq defaults
 * are used.
 *
 * In interactive mode, if connection fails with the given parameters,
 * the old connection will be kept.
 */
static bool
do_connect(enum trivalue reuse_previous_specification,
		   char *dbname, char *user, char *host, char *port)
{
	PGconn	   *o_conn = pset.db,
			   *n_conn = NULL;
	PQconninfoOption *cinfo;
	int			nconnopts = 0;
	bool		same_host = false;
	char	   *password = NULL;
	char	   *client_encoding;
	bool		success = true;
	bool		keep_password = true;
	bool		has_connection_string;
	bool		reuse_previous;

	has_connection_string = dbname ?
		recognized_connection_string(dbname) : false;

	/* Complain if we have additional arguments after a connection string. */
	if (has_connection_string && (user || host || port))
	{
		pg_log_error("Do not give user, host, or port separately when using a connection string");
		return false;
	}

	switch (reuse_previous_specification)
	{
		case TRI_YES:
			reuse_previous = true;
			break;
		case TRI_NO:
			reuse_previous = false;
			break;
		default:
			reuse_previous = !has_connection_string;
			break;
	}

	/*
	 * If we intend to re-use connection parameters, collect them out of the
	 * old connection, then replace individual values as necessary.  (We may
	 * need to resort to looking at pset.dead_conn, if the connection died
	 * previously.)  Otherwise, obtain a PQconninfoOption array containing
	 * libpq's defaults, and modify that.  Note this function assumes that
	 * PQconninfo, PQconndefaults, and PQconninfoParse will all produce arrays
	 * containing the same options in the same order.
	 */
	if (reuse_previous)
	{
		if (o_conn)
			cinfo = PQconninfo(o_conn);
		else if (pset.dead_conn)
			cinfo = PQconninfo(pset.dead_conn);
		else
		{
			/* This is reachable after a non-interactive \connect failure */
			pg_log_error("No database connection exists to re-use parameters from");
			return false;
		}
	}
	else
		cinfo = PQconndefaults();

	if (cinfo)
	{
		if (has_connection_string)
		{
			/* Parse the connstring and insert values into cinfo */
			PQconninfoOption *replcinfo;
			char	   *errmsg;

			replcinfo = PQconninfoParse(dbname, &errmsg);
			if (replcinfo)
			{
				PQconninfoOption *ci;
				PQconninfoOption *replci;
				bool		have_password = false;

				for (ci = cinfo, replci = replcinfo;
					 ci->keyword && replci->keyword;
					 ci++, replci++)
				{
					Assert(strcmp(ci->keyword, replci->keyword) == 0);
					/* Insert value from connstring if one was provided */
					if (replci->val)
					{
						/*
						 * We know that both val strings were allocated by
						 * libpq, so the least messy way to avoid memory leaks
						 * is to swap them.
						 */
						char	   *swap = replci->val;

						replci->val = ci->val;
						ci->val = swap;

						/*
						 * Check whether connstring provides options affecting
						 * password re-use.  While any change in user, host,
						 * hostaddr, or port causes us to ignore the old
						 * connection's password, we don't force that for
						 * dbname, since passwords aren't database-specific.
						 */
						if (replci->val == NULL ||
							strcmp(ci->val, replci->val) != 0)
						{
							if (strcmp(replci->keyword, "user") == 0 ||
								strcmp(replci->keyword, "host") == 0 ||
								strcmp(replci->keyword, "hostaddr") == 0 ||
								strcmp(replci->keyword, "port") == 0)
								keep_password = false;
						}
						/* Also note whether connstring contains a password. */
						if (strcmp(replci->keyword, "password") == 0)
							have_password = true;
					}
					else if (!reuse_previous)
					{
						/*
						 * When we have a connstring and are not re-using
						 * parameters, swap *all* entries, even those not set
						 * by the connstring.  This avoids absorbing
						 * environment-dependent defaults from the result of
						 * PQconndefaults().  We don't want to do that because
						 * they'd override service-file entries if the
						 * connstring specifies a service parameter, whereas
						 * the priority should be the other way around.  libpq
						 * can certainly recompute any defaults we don't pass
						 * here.  (In this situation, it's a bit wasteful to
						 * have called PQconndefaults() at all, but not doing
						 * so would require yet another major code path here.)
						 */
						replci->val = ci->val;
						ci->val = NULL;
					}
				}
				Assert(ci->keyword == NULL && replci->keyword == NULL);

				/* While here, determine how many option slots there are */
				nconnopts = ci - cinfo;

				PQconninfoFree(replcinfo);

				/*
				 * If the connstring contains a password, tell the loop below
				 * that we may use it, regardless of other settings (i.e.,
				 * cinfo's password is no longer an "old" password).
				 */
				if (have_password)
					keep_password = true;

				/* Don't let code below try to inject dbname into params. */
				dbname = NULL;
			}
			else
			{
				/* PQconninfoParse failed */
				if (errmsg)
				{
					pg_log_error("%s", errmsg);
					PQfreemem(errmsg);
				}
				else
					pg_log_error("out of memory");
				success = false;
			}
		}
		else
		{
			/*
			 * If dbname isn't a connection string, then we'll inject it and
			 * the other parameters into the keyword array below.  (We can't
			 * easily insert them into the cinfo array because of memory
			 * management issues: PQconninfoFree would misbehave on Windows.)
			 * However, to avoid dependencies on the order in which parameters
			 * appear in the array, make a preliminary scan to set
			 * keep_password and same_host correctly.
			 *
			 * While any change in user, host, or port causes us to ignore the
			 * old connection's password, we don't force that for dbname,
			 * since passwords aren't database-specific.
			 */
			PQconninfoOption *ci;

			for (ci = cinfo; ci->keyword; ci++)
			{
				if (user && strcmp(ci->keyword, "user") == 0)
				{
					if (!(ci->val && strcmp(user, ci->val) == 0))
						keep_password = false;
				}
				else if (host && strcmp(ci->keyword, "host") == 0)
				{
					if (ci->val && strcmp(host, ci->val) == 0)
						same_host = true;
					else
						keep_password = false;
				}
				else if (port && strcmp(ci->keyword, "port") == 0)
				{
					if (!(ci->val && strcmp(port, ci->val) == 0))
						keep_password = false;
				}
			}

			/* While here, determine how many option slots there are */
			nconnopts = ci - cinfo;
		}
	}
	else
	{
		/* We failed to create the cinfo structure */
		pg_log_error("out of memory");
		success = false;
	}

	/*
	 * If the user asked to be prompted for a password, ask for one now. If
	 * not, use the password from the old connection, provided the username
	 * etc have not changed. Otherwise, try to connect without a password
	 * first, and then ask for a password if needed.
	 *
	 * XXX: this behavior leads to spurious connection attempts recorded in
	 * the postmaster's log.  But libpq offers no API that would let us obtain
	 * a password and then continue with the first connection attempt.
	 */
	/*
	 * Consider whether to force client_encoding to "auto" (overriding
	 * anything in the connection string).  We do so if we have a terminal
	 * connection and there is no PGCLIENTENCODING environment setting.
	 */
	if (pset.notty || getenv("PGCLIENTENCODING"))
		client_encoding = NULL;
	else
		client_encoding = "auto";

	/* Loop till we have a connection or fail, which we might've already */
	while (success)
	{
		const char **keywords = pg_malloc((nconnopts + 1) * sizeof(*keywords));
		const char **values = pg_malloc((nconnopts + 1) * sizeof(*values));
		int			paramnum = 0;
		PQconninfoOption *ci;

		/*
		 * Copy non-default settings into the PQconnectdbParams parameter
		 * arrays; but inject any values specified old-style, as well as any
		 * interactively-obtained password, and a couple of fields we want to
		 * set forcibly.
		 *
		 * If you change this code, see also the initial-connection code in
		 * main().
		 */
		for (ci = cinfo; ci->keyword; ci++)
		{
			keywords[paramnum] = ci->keyword;

			if (dbname && strcmp(ci->keyword, "dbname") == 0)
				values[paramnum++] = dbname;
			else if (user && strcmp(ci->keyword, "user") == 0)
				values[paramnum++] = user;
			else if (host && strcmp(ci->keyword, "host") == 0)
				values[paramnum++] = host;
			else if (host && !same_host && strcmp(ci->keyword, "hostaddr") == 0)
			{
				/* If we're changing the host value, drop any old hostaddr */
				values[paramnum++] = NULL;
			}
			else if (port && strcmp(ci->keyword, "port") == 0)
				values[paramnum++] = port;
			/* If !keep_password, we unconditionally drop old password */
			else if ((password || !keep_password) &&
					 strcmp(ci->keyword, "password") == 0)
				values[paramnum++] = password;
			else if (strcmp(ci->keyword, "fallback_application_name") == 0)
				values[paramnum++] = pset.progname;
			else if (client_encoding &&
					 strcmp(ci->keyword, "client_encoding") == 0)
				values[paramnum++] = client_encoding;
			else if (ci->val)
				values[paramnum++] = ci->val;
			/* else, don't bother making libpq parse this keyword */
		}
		/* add array terminator */
		keywords[paramnum] = NULL;
		values[paramnum] = NULL;

		/* Note we do not want libpq to re-expand the dbname parameter */
		n_conn = PQconnectdbParams(keywords, values, false);

		pg_free(keywords);
		pg_free(values);

		if (PQstatus(n_conn) == CONNECTION_OK)
			break;

		/*
		 * Connection attempt failed; either retry the connection attempt with
		 * a new password, or give up.
		 */
		if (!password && PQconnectionNeedsPassword(n_conn))
		{
			/*
			 * Prompt for password using the username we actually connected
			 * with --- it might've come out of "dbname" rather than "user".
			 */
			password = prompt_for_password(PQuser(n_conn));
			PQfinish(n_conn);
			n_conn = NULL;
			continue;
		}

		/*
		 * We'll report the error below ... unless n_conn is NULL, indicating
		 * that libpq didn't have enough memory to make a PGconn.
		 */
		if (n_conn == NULL)
			pg_log_error("out of memory");

		success = false;
	}							/* end retry loop */

	/* Release locally allocated data, whether we succeeded or not */
	if (password)
		pg_free(password);
	if (cinfo)
		PQconninfoFree(cinfo);

	if (!success)
	{
		/*
		 * Failed to connect to the database. In interactive mode, keep the
		 * previous connection to the DB; in scripting mode, close our
		 * previous connection as well.
		 */
		if (pset.cur_cmd_interactive)
		{
			if (n_conn)
			{
				pg_log_info("%s", PQerrorMessage(n_conn));
				PQfinish(n_conn);
			}

			/* pset.db is left unmodified */
			if (o_conn)
				pg_log_info("Previous connection kept");
		}
		else
		{
			if (n_conn)
			{
				pg_log_error("\\connect: %s", PQerrorMessage(n_conn));
				PQfinish(n_conn);
			}

			if (o_conn)
			{
				/*
				 * Transition to having no connection.
				 *
				 * Unlike CheckConnection(), we close the old connection
				 * immediately to prevent its parameters from being re-used.
				 * This is so that a script cannot accidentally reuse
				 * parameters it did not expect to.  Otherwise, the state
				 * cleanup should be the same as in CheckConnection().
				 */
				PQfinish(o_conn);
				pset.db = NULL;
				ResetCancelConn();
				UnsyncVariables();
			}

			/* On the same reasoning, release any dead_conn to prevent reuse */
			if (pset.dead_conn)
			{
				PQfinish(pset.dead_conn);
				pset.dead_conn = NULL;
			}
		}

		return false;
	}

	/*
	 * Replace the old connection with the new one, and update
	 * connection-dependent variables.  Keep the resynchronization logic in
	 * sync with CheckConnection().
	 */
	PQsetNoticeProcessor(n_conn, NoticeProcessor, NULL);
	pset.db = n_conn;
	SyncVariables();
	connection_warnings(false); /* Must be after SyncVariables */

	/* Tell the user about the new connection */
	if (!pset.quiet)
	{
		if (!o_conn ||
			param_is_newly_set(PQhost(o_conn), PQhost(pset.db)) ||
			param_is_newly_set(PQport(o_conn), PQport(pset.db)))
		{
			char	   *host = PQhost(pset.db);
			char	   *hostaddr = PQhostaddr(pset.db);

			if (is_unixsock_path(host))
			{
				/* hostaddr overrides host */
				if (hostaddr && *hostaddr)
					printf(_("You are now connected to database \"%s\" as user \"%s\" on address \"%s\" at port \"%s\".\n"),
						   PQdb(pset.db), PQuser(pset.db), hostaddr, PQport(pset.db));
				else
					printf(_("You are now connected to database \"%s\" as user \"%s\" via socket in \"%s\" at port \"%s\".\n"),
						   PQdb(pset.db), PQuser(pset.db), host, PQport(pset.db));
			}
			else
			{
				if (hostaddr && *hostaddr && strcmp(host, hostaddr) != 0)
					printf(_("You are now connected to database \"%s\" as user \"%s\" on host \"%s\" (address \"%s\") at port \"%s\".\n"),
						   PQdb(pset.db), PQuser(pset.db), host, hostaddr, PQport(pset.db));
				else
					printf(_("You are now connected to database \"%s\" as user \"%s\" on host \"%s\" at port \"%s\".\n"),
						   PQdb(pset.db), PQuser(pset.db), host, PQport(pset.db));
			}
		}
		else
			printf(_("You are now connected to database \"%s\" as user \"%s\".\n"),
				   PQdb(pset.db), PQuser(pset.db));
	}

	/* Drop no-longer-needed connection(s) */
	if (o_conn)
		PQfinish(o_conn);
	if (pset.dead_conn)
	{
		PQfinish(pset.dead_conn);
		pset.dead_conn = NULL;
	}

	return true;
}


void
connection_warnings(bool in_startup)
{
	if (!pset.quiet && !pset.notty)
	{
		int			client_ver = PG_VERSION_NUM;
		char		sverbuf[32];

		if (pset.sversion != client_ver)
		{
			const char *server_version;

			/* Try to get full text form, might include "devel" etc */
			server_version = PQparameterStatus(pset.db, "server_version");
			/* Otherwise fall back on pset.sversion */
			if (!server_version)
			{
				formatPGVersionNumber(pset.sversion, true,
									  sverbuf, sizeof(sverbuf));
				server_version = sverbuf;
			}

			printf(_("%s (%s, server %s)\n"),
				   pset.progname, PG_VERSION, server_version);
		}
		/* For version match, only print psql banner on startup. */
		else if (in_startup)
			printf("%s (%s)\n", pset.progname, PG_VERSION);
	}
}



/*
 * SyncVariables
 *
 * Make psql's internal variables agree with connection state upon
 * establishing a new connection.
 */
void
SyncVariables(void)
{
	char		vbuf[32];
	const char *server_version;

	/* get stuff from connection */
	pset.encoding = PQclientEncoding(pset.db);
	pset.popt.topt.encoding = pset.encoding;
	pset.sversion = PQserverVersion(pset.db);

	setFmtEncoding(pset.encoding);

	SetVariable(pset.vars, "DBNAME", PQdb(pset.db));
	SetVariable(pset.vars, "USER", PQuser(pset.db));
	SetVariable(pset.vars, "HOST", PQhost(pset.db));
	SetVariable(pset.vars, "PORT", PQport(pset.db));
	SetVariable(pset.vars, "ENCODING", pg_encoding_to_char(pset.encoding));

	/* this bit should match connection_warnings(): */
	/* Try to get full text form of version, might include "devel" etc */
	server_version = PQparameterStatus(pset.db, "server_version");
	/* Otherwise fall back on pset.sversion */
	if (!server_version)
	{
		formatPGVersionNumber(pset.sversion, true, vbuf, sizeof(vbuf));
		server_version = vbuf;
	}
	SetVariable(pset.vars, "SERVER_VERSION_NAME", server_version);

	snprintf(vbuf, sizeof(vbuf), "%d", pset.sversion);
	SetVariable(pset.vars, "SERVER_VERSION_NUM", vbuf);

	/* send stuff to it, too */
	PQsetErrorVerbosity(pset.db, pset.verbosity);
	PQsetErrorContextVisibility(pset.db, pset.show_context);
}

/*
 * UnsyncVariables
 *
 * Clear variables that should be not be set when there is no connection.
 */
void
UnsyncVariables(void)
{
	SetVariable(pset.vars, "DBNAME", NULL);
	SetVariable(pset.vars, "USER", NULL);
	SetVariable(pset.vars, "HOST", NULL);
	SetVariable(pset.vars, "PORT", NULL);
	SetVariable(pset.vars, "ENCODING", NULL);
	SetVariable(pset.vars, "SERVER_VERSION_NAME", NULL);
	SetVariable(pset.vars, "SERVER_VERSION_NUM", NULL);
}


/*
 * helper for do_edit(): actually invoke the editor
 *
 * Returns true on success, false if we failed to invoke the editor or
 * it returned nonzero status.  (An error message is printed for failed-
 * to-invoke cases, but not if the editor returns nonzero status.)
 */
/*
 * process_file - execute a file of psql commands (used by \i and psqlrc)
 */
int
process_file(char *filename, bool use_relative_path)
{
	FILE	   *fd;
	int			result;
	char	   *oldfilename;
	char		relpath[MAXPGPATH];

	if (!filename)
	{
		fd = stdin;
		filename = NULL;
	}
	else if (strcmp(filename, "-") != 0)
	{
		canonicalize_path_enc(filename, pset.encoding);

		/*
		 * If we were asked to resolve the pathname relative to the location
		 * of the currently executing script, and there is one, and this is a
		 * relative pathname, then prepend all but the last pathname component
		 * of the current script to this pathname.
		 */
		if (use_relative_path && pset.inputfile &&
			!is_absolute_path(filename) && !has_drive_prefix(filename))
		{
			strlcpy(relpath, pset.inputfile, sizeof(relpath));
			get_parent_directory(relpath);
			join_path_components(relpath, relpath, filename);
			canonicalize_path_enc(relpath, pset.encoding);

			filename = relpath;
		}

		fd = fopen(filename, PG_BINARY_R);

		if (!fd)
		{
			pg_log_error("%s: %m", filename);
			return EXIT_FAILURE;
		}
	}
	else
	{
		fd = stdin;
		filename = "<stdin>";	/* for future error messages */
	}

	oldfilename = pset.inputfile;
	pset.inputfile = filename;

	pg_logging_config(pset.inputfile ? 0 : PG_LOG_FLAG_TERSE);

	result = MainLoop(fd);

	if (fd != stdin)
		fclose(fd);

	pset.inputfile = oldfilename;

	pg_logging_config(pset.inputfile ? 0 : PG_LOG_FLAG_TERSE);

	return result;
}

static const char *
_align2string(enum printFormat in)
{
	switch (in)
	{
		case PRINT_NOTHING:
			return "nothing";
			break;
		case PRINT_ALIGNED:
			return "aligned";
			break;
		case PRINT_CSV:
			return "csv";
			break;
		case PRINT_UNALIGNED:
			return "unaligned";
			break;
		case PRINT_WRAPPED:
			return "wrapped";
			break;
	}
	return "unknown";
}

/*
 * Parse entered Unicode linestyle.  If ok, update *linestyle and return
 * true, else return false.
 */
static bool
set_unicode_line_style(const char *value, size_t vallen,
					   unicode_linestyle *linestyle)
{
	if (pg_strncasecmp("single", value, vallen) == 0)
		*linestyle = UNICODE_LINESTYLE_SINGLE;
	else if (pg_strncasecmp("double", value, vallen) == 0)
		*linestyle = UNICODE_LINESTYLE_DOUBLE;
	else
		return false;
	return true;
}

static const char *
_unicode_linestyle2string(int linestyle)
{
	switch (linestyle)
	{
		case UNICODE_LINESTYLE_SINGLE:
			return "single";
			break;
		case UNICODE_LINESTYLE_DOUBLE:
			return "double";
			break;
	}
	return "unknown";
}

/*
 * do_pset
 *
 * Performs the assignment "param = value", where value could be NULL;
 * for some params that has an effect such as inversion, for others
 * it does nothing.
 *
 * Adjusts the state of the formatting options at *popt.  (In practice that
 * is always pset.popt, but maybe someday it could be different.)
 *
 * If successful and quiet is false, then invokes printPsetInfo() to report
 * the change.
 *
 * Returns true if successful, else false (eg for invalid param or value).
 */
bool
do_pset(const char *param, const char *value, printQueryOpt *popt, bool quiet)
{
	size_t		vallen = 0;

	Assert(param != NULL);

	if (value)
		vallen = strlen(value);

	/* set format */
	if (strcmp(param, "format") == 0)
	{
		static const struct fmt
		{
			const char *name;
			enum printFormat number;
		}			formats[] =
		{
			/* remember to update error message below when adding more */
			{"aligned", PRINT_ALIGNED},
			{"csv", PRINT_CSV},
			{"unaligned", PRINT_UNALIGNED},
			{"wrapped", PRINT_WRAPPED}
		};

		if (!value)
			;
		else
		{
			int			match_pos = -1;

			for (int i = 0; i < lengthof(formats); i++)
			{
				if (pg_strncasecmp(formats[i].name, value, vallen) == 0)
				{
					if (match_pos < 0)
						match_pos = i;
					else
					{
						pg_log_error("\\pset: ambiguous abbreviation \"%s\" matches both \"%s\" and \"%s\"",
									 value,
									 formats[match_pos].name, formats[i].name);
						return false;
					}
				}
			}
			if (match_pos >= 0)
				popt->topt.format = formats[match_pos].number;
			else
			{
				pg_log_error("\\pset: allowed formats are aligned, csv, unaligned, wrapped");
				return false;
			}
		}
	}

	/* set table line style */
	else if (strcmp(param, "linestyle") == 0)
	{
		if (!value)
			;
		else if (pg_strncasecmp("ascii", value, vallen) == 0)
			popt->topt.line_style = &pg_asciiformat;
		else if (pg_strncasecmp("old-ascii", value, vallen) == 0)
			popt->topt.line_style = &pg_asciiformat_old;
		else if (pg_strncasecmp("unicode", value, vallen) == 0)
			popt->topt.line_style = &pg_utf8format;
		else
		{
			pg_log_error("\\pset: allowed line styles are ascii, old-ascii, unicode");
			return false;
		}
	}

	/* set unicode border line style */
	else if (strcmp(param, "unicode_border_linestyle") == 0)
	{
		if (!value)
			;
		else if (set_unicode_line_style(value, vallen,
										&popt->topt.unicode_border_linestyle))
			refresh_utf8format(&(popt->topt));
		else
		{
			pg_log_error("\\pset: allowed Unicode border line styles are single, double");
			return false;
		}
	}

	/* set unicode column line style */
	else if (strcmp(param, "unicode_column_linestyle") == 0)
	{
		if (!value)
			;
		else if (set_unicode_line_style(value, vallen,
										&popt->topt.unicode_column_linestyle))
			refresh_utf8format(&(popt->topt));
		else
		{
			pg_log_error("\\pset: allowed Unicode column line styles are single, double");
			return false;
		}
	}

	/* set unicode header line style */
	else if (strcmp(param, "unicode_header_linestyle") == 0)
	{
		if (!value)
			;
		else if (set_unicode_line_style(value, vallen,
										&popt->topt.unicode_header_linestyle))
			refresh_utf8format(&(popt->topt));
		else
		{
			pg_log_error("\\pset: allowed Unicode header line styles are single, double");
			return false;
		}
	}

	/* set border style/width */
	else if (strcmp(param, "border") == 0)
	{
		if (value)
			popt->topt.border = atoi(value);
	}

	/* set expanded/vertical mode */
	else if (strcmp(param, "x") == 0 ||
			 strcmp(param, "expanded") == 0 ||
			 strcmp(param, "vertical") == 0)
	{
		if (value && pg_strcasecmp(value, "auto") == 0)
			popt->topt.expanded = 2;
		else if (value)
		{
			bool		on_off;

			if (ParseVariableBool(value, NULL, &on_off))
				popt->topt.expanded = on_off ? 1 : 0;
			else
			{
				PsqlVarEnumError(param, value, "on, off, auto");
				return false;
			}
		}
		else
			popt->topt.expanded = !popt->topt.expanded;
	}

	/* field separator for CSV format */
	else if (strcmp(param, "csv_fieldsep") == 0)
	{
		if (value)
		{
			/* CSV separator has to be a one-byte character */
			if (strlen(value) != 1)
			{
				pg_log_error("\\pset: csv_fieldsep must be a single one-byte character");
				return false;
			}
			if (value[0] == '"' || value[0] == '\n' || value[0] == '\r')
			{
				pg_log_error("\\pset: csv_fieldsep cannot be a double quote, a newline, or a carriage return");
				return false;
			}
			popt->topt.csvFieldSep[0] = value[0];
		}
	}

	/* locale-aware numeric output */
	else if (strcmp(param, "numericlocale") == 0)
	{
		if (value)
			return ParseVariableBool(value, param, &popt->topt.numericLocale);
		else
			popt->topt.numericLocale = !popt->topt.numericLocale;
	}

	/* null display */
	else if (strcmp(param, "null") == 0)
	{
		if (value)
		{
			free(popt->nullPrint);
			popt->nullPrint = pg_strdup(value);
		}
	}

	/* field separator for unaligned text */
	else if (strcmp(param, "fieldsep") == 0)
	{
		if (value)
		{
			free(popt->topt.fieldSep.separator);
			popt->topt.fieldSep.separator = pg_strdup(value);
			popt->topt.fieldSep.separator_zero = false;
		}
	}

	else if (strcmp(param, "fieldsep_zero") == 0)
	{
		free(popt->topt.fieldSep.separator);
		popt->topt.fieldSep.separator = NULL;
		popt->topt.fieldSep.separator_zero = true;
	}

	/* record separator for unaligned text */
	else if (strcmp(param, "recordsep") == 0)
	{
		if (value)
		{
			free(popt->topt.recordSep.separator);
			popt->topt.recordSep.separator = pg_strdup(value);
			popt->topt.recordSep.separator_zero = false;
		}
	}

	else if (strcmp(param, "recordsep_zero") == 0)
	{
		free(popt->topt.recordSep.separator);
		popt->topt.recordSep.separator = NULL;
		popt->topt.recordSep.separator_zero = true;
	}

	/* toggle between full and tuples-only format */
	else if (strcmp(param, "t") == 0 || strcmp(param, "tuples_only") == 0)
	{
		if (value)
			return ParseVariableBool(value, param, &popt->topt.tuples_only);
		else
			popt->topt.tuples_only = !popt->topt.tuples_only;
	}

	/* set title override */
	else if (strcmp(param, "C") == 0 || strcmp(param, "title") == 0)
	{
		free(popt->title);
		if (!value)
			popt->title = NULL;
		else
			popt->title = pg_strdup(value);
	}

	/* toggle use of pager */
	else if (strcmp(param, "pager") == 0)
	{
		if (value && pg_strcasecmp(value, "always") == 0)
			popt->topt.pager = 2;
		else if (value)
		{
			bool		on_off;

			if (!ParseVariableBool(value, NULL, &on_off))
			{
				PsqlVarEnumError(param, value, "on, off, always");
				return false;
			}
			popt->topt.pager = on_off ? 1 : 0;
		}
		else if (popt->topt.pager == 1)
			popt->topt.pager = 0;
		else
			popt->topt.pager = 1;
	}

	/* set minimum lines for pager use */
	else if (strcmp(param, "pager_min_lines") == 0)
	{
		if (value)
			popt->topt.pager_min_lines = atoi(value);
	}

	/* disable "(x rows)" footer */
	else if (strcmp(param, "footer") == 0)
	{
		if (value)
			return ParseVariableBool(value, param, &popt->topt.default_footer);
		else
			popt->topt.default_footer = !popt->topt.default_footer;
	}

	/* set border style/width */
	else if (strcmp(param, "columns") == 0)
	{
		if (value)
			popt->topt.columns = atoi(value);
	}
	else
	{
		pg_log_error("\\pset: unknown option: %s", param);
		return false;
	}

	if (!quiet)
		printPsetInfo(param, &pset.popt);

	return true;
}

/*
 * printPsetInfo: print the state of the "param" formatting parameter in popt.
 */
static bool
printPsetInfo(const char *param, printQueryOpt *popt)
{
	Assert(param != NULL);

	/* show border style/width */
	if (strcmp(param, "border") == 0)
		printf(_("Border style is %d.\n"), popt->topt.border);

	/* show the target width for the wrapped format */
	else if (strcmp(param, "columns") == 0)
	{
		if (!popt->topt.columns)
			printf(_("Target width is unset.\n"));
		else
			printf(_("Target width is %d.\n"), popt->topt.columns);
	}

	/* show expanded/vertical mode */
	else if (strcmp(param, "x") == 0 || strcmp(param, "expanded") == 0 || strcmp(param, "vertical") == 0)
	{
		if (popt->topt.expanded == 1)
			printf(_("Expanded display is on.\n"));
		else if (popt->topt.expanded == 2)
			printf(_("Expanded display is used automatically.\n"));
		else
			printf(_("Expanded display is off.\n"));
	}

	/* show field separator for CSV format */
	else if (strcmp(param, "csv_fieldsep") == 0)
	{
		printf(_("Field separator for CSV is \"%s\".\n"),
			   popt->topt.csvFieldSep);
	}

	/* show field separator for unaligned text */
	else if (strcmp(param, "fieldsep") == 0)
	{
		if (popt->topt.fieldSep.separator_zero)
			printf(_("Field separator is zero byte.\n"));
		else
			printf(_("Field separator is \"%s\".\n"),
				   popt->topt.fieldSep.separator);
	}

	else if (strcmp(param, "fieldsep_zero") == 0)
	{
		printf(_("Field separator is zero byte.\n"));
	}

	/* show disable "(x rows)" footer */
	else if (strcmp(param, "footer") == 0)
	{
		if (popt->topt.default_footer)
			printf(_("Default footer is on.\n"));
		else
			printf(_("Default footer is off.\n"));
	}

	/* show format */
	else if (strcmp(param, "format") == 0)
	{
		printf(_("Output format is %s.\n"), _align2string(popt->topt.format));
	}

	/* show table line style */
	else if (strcmp(param, "linestyle") == 0)
	{
		printf(_("Line style is %s.\n"),
			   get_line_style(&popt->topt)->name);
	}

	/* show null display */
	else if (strcmp(param, "null") == 0)
	{
		printf(_("Null display is \"%s\".\n"),
			   popt->nullPrint ? popt->nullPrint : "");
	}

	/* show locale-aware numeric output */
	else if (strcmp(param, "numericlocale") == 0)
	{
		if (popt->topt.numericLocale)
			printf(_("Locale-adjusted numeric output is on.\n"));
		else
			printf(_("Locale-adjusted numeric output is off.\n"));
	}

	/* show toggle use of pager */
	else if (strcmp(param, "pager") == 0)
	{
		if (popt->topt.pager == 1)
			printf(_("Pager is used for long output.\n"));
		else if (popt->topt.pager == 2)
			printf(_("Pager is always used.\n"));
		else
			printf(_("Pager usage is off.\n"));
	}

	/* show minimum lines for pager use */
	else if (strcmp(param, "pager_min_lines") == 0)
	{
		printf(ngettext("Pager won't be used for less than %d line.\n",
						"Pager won't be used for less than %d lines.\n",
						popt->topt.pager_min_lines),
			   popt->topt.pager_min_lines);
	}

	/* show record separator for unaligned text */
	else if (strcmp(param, "recordsep") == 0)
	{
		if (popt->topt.recordSep.separator_zero)
			printf(_("Record separator is zero byte.\n"));
		else if (strcmp(popt->topt.recordSep.separator, "\n") == 0)
			printf(_("Record separator is <newline>.\n"));
		else
			printf(_("Record separator is \"%s\".\n"),
				   popt->topt.recordSep.separator);
	}

	else if (strcmp(param, "recordsep_zero") == 0)
	{
		printf(_("Record separator is zero byte.\n"));
	}

	/* show title override */
	else if (strcmp(param, "C") == 0 || strcmp(param, "title") == 0)
	{
		if (popt->title)
			printf(_("Title is \"%s\".\n"), popt->title);
		else
			printf(_("Title is unset.\n"));
	}

	/* show toggle between full and tuples-only format */
	else if (strcmp(param, "t") == 0 || strcmp(param, "tuples_only") == 0)
	{
		if (popt->topt.tuples_only)
			printf(_("Tuples only is on.\n"));
		else
			printf(_("Tuples only is off.\n"));
	}

	/* Unicode style formatting */
	else if (strcmp(param, "unicode_border_linestyle") == 0)
	{
		printf(_("Unicode border line style is \"%s\".\n"),
			   _unicode_linestyle2string(popt->topt.unicode_border_linestyle));
	}

	else if (strcmp(param, "unicode_column_linestyle") == 0)
	{
		printf(_("Unicode column line style is \"%s\".\n"),
			   _unicode_linestyle2string(popt->topt.unicode_column_linestyle));
	}

	else if (strcmp(param, "unicode_header_linestyle") == 0)
	{
		printf(_("Unicode header line style is \"%s\".\n"),
			   _unicode_linestyle2string(popt->topt.unicode_header_linestyle));
	}

	else
	{
		pg_log_error("\\pset: unknown option: %s", param);
		return false;
	}

	return true;
}

/*
 * savePsetInfo: make a malloc'd copy of the data in *popt.
 *
 * Possibly this should be somewhere else, but it's a bit specific to psql.
 */
printQueryOpt *
savePsetInfo(const printQueryOpt *popt)
{
	printQueryOpt *save;

	save = (printQueryOpt *) pg_malloc(sizeof(printQueryOpt));

	/* Flat-copy all the scalar fields, then duplicate sub-structures. */
	memcpy(save, popt, sizeof(printQueryOpt));

	/* topt.line_style points to const data that need not be duplicated */
	if (popt->topt.fieldSep.separator)
		save->topt.fieldSep.separator = pg_strdup(popt->topt.fieldSep.separator);
	if (popt->topt.recordSep.separator)
		save->topt.recordSep.separator = pg_strdup(popt->topt.recordSep.separator);
	if (popt->nullPrint)
		save->nullPrint = pg_strdup(popt->nullPrint);
	if (popt->title)
		save->title = pg_strdup(popt->title);

	/*
	 * footers and translate_columns are never set in psql's print settings,
	 * so we needn't write code to duplicate them.
	 */
	Assert(popt->footers == NULL);
	Assert(popt->translate_columns == NULL);

	return save;
}

/*
 * restorePsetInfo: restore *popt from the previously-saved copy *save,
 * then free *save.
 */
void
restorePsetInfo(printQueryOpt *popt, printQueryOpt *save)
{
	/* Free all the old data we're about to overwrite the pointers to. */

	/* topt.line_style points to const data that need not be duplicated */
	if (popt->topt.fieldSep.separator)
		free(popt->topt.fieldSep.separator);
	if (popt->topt.recordSep.separator)
		free(popt->topt.recordSep.separator);
	if (popt->nullPrint)
		free(popt->nullPrint);
	if (popt->title)
		free(popt->title);

	/*
	 * footers and translate_columns are never set in psql's print settings,
	 * so we needn't write code to duplicate them.
	 */
	Assert(popt->footers == NULL);
	Assert(popt->translate_columns == NULL);

	/* Now we may flat-copy all the fields, including pointers. */
	memcpy(popt, save, sizeof(printQueryOpt));

	/* Lastly, free "save" ... but its sub-structures now belong to popt. */
	free(save);
}

static const char *
pset_bool_string(bool val)
{
	return val ? "on" : "off";
}


static char *
pset_quoted_string(const char *str)
{
	char	   *ret = pg_malloc(strlen(str) * 2 + 3);
	char	   *r = ret;

	*r++ = '\'';

	for (; *str; str++)
	{
		if (*str == '\n')
		{
			*r++ = '\\';
			*r++ = 'n';
		}
		else if (*str == '\'')
		{
			*r++ = '\\';
			*r++ = '\'';
		}
		else
			*r++ = *str;
	}

	*r++ = '\'';
	*r = '\0';

	return ret;
}


/*
 * Return a malloc'ed string for the \pset value.
 *
 * Note that for some string parameters, print.c distinguishes between unset
 * and empty string, but for others it doesn't.  This function should produce
 * output that produces the correct setting when fed back into \pset.
 */
static char *
pset_value_string(const char *param, printQueryOpt *popt)
{
	Assert(param != NULL);

	if (strcmp(param, "border") == 0)
		return psprintf("%d", popt->topt.border);
	else if (strcmp(param, "columns") == 0)
		return psprintf("%d", popt->topt.columns);
	else if (strcmp(param, "csv_fieldsep") == 0)
		return pset_quoted_string(popt->topt.csvFieldSep);
	else if (strcmp(param, "expanded") == 0)
		return pstrdup(popt->topt.expanded == 2
					   ? "auto"
					   : pset_bool_string(popt->topt.expanded));
	else if (strcmp(param, "fieldsep") == 0)
		return pset_quoted_string(popt->topt.fieldSep.separator
								  ? popt->topt.fieldSep.separator
								  : "");
	else if (strcmp(param, "fieldsep_zero") == 0)
		return pstrdup(pset_bool_string(popt->topt.fieldSep.separator_zero));
	else if (strcmp(param, "footer") == 0)
		return pstrdup(pset_bool_string(popt->topt.default_footer));
	else if (strcmp(param, "format") == 0)
		return psprintf("%s", _align2string(popt->topt.format));
	else if (strcmp(param, "linestyle") == 0)
		return psprintf("%s", get_line_style(&popt->topt)->name);
	else if (strcmp(param, "null") == 0)
		return pset_quoted_string(popt->nullPrint
								  ? popt->nullPrint
								  : "");
	else if (strcmp(param, "numericlocale") == 0)
		return pstrdup(pset_bool_string(popt->topt.numericLocale));
	else if (strcmp(param, "pager") == 0)
		return psprintf("%d", popt->topt.pager);
	else if (strcmp(param, "pager_min_lines") == 0)
		return psprintf("%d", popt->topt.pager_min_lines);
	else if (strcmp(param, "recordsep") == 0)
		return pset_quoted_string(popt->topt.recordSep.separator
								  ? popt->topt.recordSep.separator
								  : "");
	else if (strcmp(param, "recordsep_zero") == 0)
		return pstrdup(pset_bool_string(popt->topt.recordSep.separator_zero));
	else if (strcmp(param, "title") == 0)
		return popt->title ? pset_quoted_string(popt->title) : pstrdup("");
	else if (strcmp(param, "tuples_only") == 0)
		return pstrdup(pset_bool_string(popt->topt.tuples_only));
	else if (strcmp(param, "unicode_border_linestyle") == 0)
		return pstrdup(_unicode_linestyle2string(popt->topt.unicode_border_linestyle));
	else if (strcmp(param, "unicode_column_linestyle") == 0)
		return pstrdup(_unicode_linestyle2string(popt->topt.unicode_column_linestyle));
	else if (strcmp(param, "unicode_header_linestyle") == 0)
		return pstrdup(_unicode_linestyle2string(popt->topt.unicode_header_linestyle));
	else
		return pstrdup("ERROR");
}



#define DEFAULT_SHELL "/bin/sh"

