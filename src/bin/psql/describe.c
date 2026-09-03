/*
 * psql - the PostgreSQL interactive terminal
 *
 * Support for the various \d ("describe") commands.  Note that the current
 * expectation is that all functions in this file will succeed when working
 * with servers of versions 7.4 and up.  It's okay to omit irrelevant
 * information for an old server, but not to fail outright.
 *
 * Copyright (c) 2000-2021, PostgreSQL Global Development Group
 *
 * src/bin/psql/describe.c
 */
#include "postgres_fe.h"

#include <ctype.h>

#include "catalog/pg_am.h"
#include "catalog/pg_attribute_d.h"
#include "catalog/pg_cast_d.h"
#include "catalog/pg_class_d.h"
#include "common.h"
#include "common/logging.h"
#include "describe.h"
#include "fe_utils/mbprint.h"
#include "fe_utils/print.h"
#include "fe_utils/string_utils.h"
#include "settings.h"
#include "variables.h"

static const char *map_typename_pattern(const char *pattern);
static bool describeOneTableDetails(const char *schemaname,
									const char *relationname,
									const char *oid,
									bool verbose);
static void add_tablespace_footer(printTableContent *const cont, char relkind,
								  Oid tablespace, const bool newline);
static bool listTSParsersVerbose(const char *pattern);
static bool describeOneTSParser(const char *oid, const char *nspname,
								const char *prsname);
static bool listTSConfigsVerbose(const char *pattern);
static bool describeOneTSConfig(const char *oid, const char *nspname,
								const char *cfgname,
								const char *pnspname, const char *prsname);
static bool listOneExtensionContents(const char *extname, const char *oid);
static bool validateSQLNamePattern(PQExpBuffer buf, const char *pattern,
								   bool have_where, bool force_escape,
								   const char *schemavar, const char *namevar,
								   const char *altnamevar,
								   const char *visibilityrule,
								   bool *added_clause, int maxparts);


/*----------------
 * Handlers for various slash commands displaying some sort of list
 * of things in the database.
 *
 * Note: try to format the queries to look nice in -E output.
 *----------------
 */


/*
 * \da
 * Takes an optional regexp to select particular aggregates
 */
bool
describeAggregates(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  p.proname AS \"%s\",\n"
					  "  pg_catalog.format_type(p.prorettype, NULL) AS \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Result data type"));

	if (pset.sversion >= 80400)
		appendPQExpBuffer(&buf,
						  "  CASE WHEN p.pronargs = 0\n"
						  "    THEN CAST('*' AS pg_catalog.text)\n"
						  "    ELSE pg_catalog.pg_get_function_arguments(p.oid)\n"
						  "  END AS \"%s\",\n",
						  gettext_noop("Argument data types"));
	else if (pset.sversion >= 80200)
		appendPQExpBuffer(&buf,
						  "  CASE WHEN p.pronargs = 0\n"
						  "    THEN CAST('*' AS pg_catalog.text)\n"
						  "    ELSE\n"
						  "    pg_catalog.array_to_string(ARRAY(\n"
						  "      SELECT\n"
						  "        pg_catalog.format_type(p.proargtypes[s.i], NULL)\n"
						  "      FROM\n"
						  "        pg_catalog.generate_series(0, pg_catalog.array_upper(p.proargtypes, 1)) AS s(i)\n"
						  "    ), ', ')\n"
						  "  END AS \"%s\",\n",
						  gettext_noop("Argument data types"));
	else
		appendPQExpBuffer(&buf,
						  "  pg_catalog.format_type(p.proargtypes[0], NULL) AS \"%s\",\n",
						  gettext_noop("Argument data types"));

	if (pset.sversion >= 110000)
		appendPQExpBuffer(&buf,
						  "  pg_catalog.obj_description(p.oid, 'pg_proc') as \"%s\"\n"
						  "FROM pg_catalog.pg_proc p\n"
						  "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace\n"
						  "WHERE p.prokind = 'a'\n",
						  gettext_noop("Description"));
	else
		appendPQExpBuffer(&buf,
						  "  pg_catalog.obj_description(p.oid, 'pg_proc') as \"%s\"\n"
						  "FROM pg_catalog.pg_proc p\n"
						  "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace\n"
						  "WHERE p.proisagg\n",
						  gettext_noop("Description"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
							  "n.nspname", "p.proname", NULL,
							  "pg_catalog.pg_function_is_visible(p.oid)",
							  NULL, 3))
		return false;

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 4;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of aggregate functions");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dA
 * Takes an optional regexp to select particular access methods
 */
bool
describeAccessMethods(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, true, false, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT amname AS \"%s\",\n"
					  "  CASE amtype"
					  " WHEN 'i' THEN '%s'"
					  " WHEN 't' THEN '%s'"
					  " END AS \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Index"),
					  gettext_noop("Table"),
					  gettext_noop("Type"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  ",\n  amhandler AS \"%s\",\n"
						  "  pg_catalog.obj_description(oid, 'pg_am') AS \"%s\"",
						  gettext_noop("Handler"),
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_am\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "amname", NULL,
								NULL,
								NULL, 1))
		return false;

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of access methods");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \df
 * Takes an optional regexp to select particular functions.
 *
 * As with \d, you can specify the kinds of functions you want:
 *
 * a for aggregates
 * n for normal
 * p for procedure
 * t for trigger
 * w for window
 *
 * and you can mix and match these in any order.
 */
bool
describeFunctions(const char *functypes, const char *func_pattern,
				  char **arg_patterns, int num_arg_patterns,
				  bool verbose, bool showSystem)
{
	bool		showAggregate = strchr(functypes, 'a') != NULL;
	bool		showNormal = strchr(functypes, 'n') != NULL;
	bool		showProcedure = strchr(functypes, 'p') != NULL;
	bool		showTrigger = strchr(functypes, 't') != NULL;
	bool		showWindow = strchr(functypes, 'w') != NULL;
	bool		have_where;
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, false, false, true, true, true, false, true, false, false, false, false};

	/* No "Parallel" column before 9.6 */
	static const bool translate_columns_pre_96[] = {false, false, false, false, true, true, false, true, false, false, false, false};

	if (strlen(functypes) != strspn(functypes, "anptwS+"))
	{
		pg_log_error("\\df only takes [anptwS+] as options");
		return true;
	}

	if (!showAggregate && !showNormal && !showProcedure && !showTrigger && !showWindow)
	{
		showAggregate = showNormal = showTrigger = true;
		if (pset.sversion >= 110000)
			showProcedure = true;
		if (pset.sversion >= 80400)
			showWindow = true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  p.proname as \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"));

	if (pset.sversion >= 110000)
		appendPQExpBuffer(&buf,
						  "  pg_catalog.pg_get_function_result(p.oid) as \"%s\",\n"
						  "  pg_catalog.pg_get_function_arguments(p.oid) as \"%s\",\n"
						  " CASE p.prokind\n"
						  "  WHEN 'a' THEN '%s'\n"
						  "  WHEN 'w' THEN '%s'\n"
						  "  WHEN 'p' THEN '%s'\n"
						  "  ELSE '%s'\n"
						  " END as \"%s\"",
						  gettext_noop("Result data type"),
						  gettext_noop("Argument data types"),
		/* translator: "agg" is short for "aggregate" */
						  gettext_noop("agg"),
						  gettext_noop("window"),
						  gettext_noop("proc"),
						  gettext_noop("func"),
						  gettext_noop("Type"));
	else if (pset.sversion >= 80400)
		appendPQExpBuffer(&buf,
						  "  pg_catalog.pg_get_function_result(p.oid) as \"%s\",\n"
						  "  pg_catalog.pg_get_function_arguments(p.oid) as \"%s\",\n"
						  " CASE\n"
						  "  WHEN p.proisagg THEN '%s'\n"
						  "  WHEN p.proiswindow THEN '%s'\n"
						  "  WHEN p.prorettype = 'pg_catalog.trigger'::pg_catalog.regtype THEN '%s'\n"
						  "  ELSE '%s'\n"
						  " END as \"%s\"",
						  gettext_noop("Result data type"),
						  gettext_noop("Argument data types"),
		/* translator: "agg" is short for "aggregate" */
						  gettext_noop("agg"),
						  gettext_noop("window"),
						  gettext_noop("trigger"),
						  gettext_noop("func"),
						  gettext_noop("Type"));
	else if (pset.sversion >= 80100)
		appendPQExpBuffer(&buf,
						  "  CASE WHEN p.proretset THEN 'SETOF ' ELSE '' END ||\n"
						  "  pg_catalog.format_type(p.prorettype, NULL) as \"%s\",\n"
						  "  CASE WHEN proallargtypes IS NOT NULL THEN\n"
						  "    pg_catalog.array_to_string(ARRAY(\n"
						  "      SELECT\n"
						  "        CASE\n"
						  "          WHEN p.proargmodes[s.i] = 'i' THEN ''\n"
						  "          WHEN p.proargmodes[s.i] = 'o' THEN 'OUT '\n"
						  "          WHEN p.proargmodes[s.i] = 'b' THEN 'INOUT '\n"
						  "          WHEN p.proargmodes[s.i] = 'v' THEN 'VARIADIC '\n"
						  "        END ||\n"
						  "        CASE\n"
						  "          WHEN COALESCE(p.proargnames[s.i], '') = '' THEN ''\n"
						  "          ELSE p.proargnames[s.i] || ' '\n"
						  "        END ||\n"
						  "        pg_catalog.format_type(p.proallargtypes[s.i], NULL)\n"
						  "      FROM\n"
						  "        pg_catalog.generate_series(1, pg_catalog.array_upper(p.proallargtypes, 1)) AS s(i)\n"
						  "    ), ', ')\n"
						  "  ELSE\n"
						  "    pg_catalog.array_to_string(ARRAY(\n"
						  "      SELECT\n"
						  "        CASE\n"
						  "          WHEN COALESCE(p.proargnames[s.i+1], '') = '' THEN ''\n"
						  "          ELSE p.proargnames[s.i+1] || ' '\n"
						  "          END ||\n"
						  "        pg_catalog.format_type(p.proargtypes[s.i], NULL)\n"
						  "      FROM\n"
						  "        pg_catalog.generate_series(0, pg_catalog.array_upper(p.proargtypes, 1)) AS s(i)\n"
						  "    ), ', ')\n"
						  "  END AS \"%s\",\n"
						  "  CASE\n"
						  "    WHEN p.proisagg THEN '%s'\n"
						  "    WHEN p.prorettype = 'pg_catalog.trigger'::pg_catalog.regtype THEN '%s'\n"
						  "    ELSE '%s'\n"
						  "  END AS \"%s\"",
						  gettext_noop("Result data type"),
						  gettext_noop("Argument data types"),
		/* translator: "agg" is short for "aggregate" */
						  gettext_noop("agg"),
						  gettext_noop("trigger"),
						  gettext_noop("func"),
						  gettext_noop("Type"));
	else
		appendPQExpBuffer(&buf,
						  "  CASE WHEN p.proretset THEN 'SETOF ' ELSE '' END ||\n"
						  "  pg_catalog.format_type(p.prorettype, NULL) as \"%s\",\n"
						  "  pg_catalog.oidvectortypes(p.proargtypes) as \"%s\",\n"
						  "  CASE\n"
						  "    WHEN p.proisagg THEN '%s'\n"
						  "    WHEN p.prorettype = 'pg_catalog.trigger'::pg_catalog.regtype THEN '%s'\n"
						  "    ELSE '%s'\n"
						  "  END AS \"%s\"",
						  gettext_noop("Result data type"),
						  gettext_noop("Argument data types"),
		/* translator: "agg" is short for "aggregate" */
						  gettext_noop("agg"),
						  gettext_noop("trigger"),
						  gettext_noop("func"),
						  gettext_noop("Type"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  ",\n CASE\n"
						  "  WHEN p.provolatile = 'i' THEN '%s'\n"
						  "  WHEN p.provolatile = 's' THEN '%s'\n"
						  "  WHEN p.provolatile = 'v' THEN '%s'\n"
						  " END as \"%s\"",
						  gettext_noop("immutable"),
						  gettext_noop("stable"),
						  gettext_noop("volatile"),
						  gettext_noop("Volatility"));
		if (pset.sversion >= 90600)
			appendPQExpBuffer(&buf,
							  ",\n CASE\n"
							  "  WHEN p.proparallel = 'r' THEN '%s'\n"
							  "  WHEN p.proparallel = 's' THEN '%s'\n"
							  "  WHEN p.proparallel = 'u' THEN '%s'\n"
							  " END as \"%s\"",
							  gettext_noop("restricted"),
							  gettext_noop("safe"),
							  gettext_noop("unsafe"),
							  gettext_noop("Parallel"));
		appendPQExpBuffer(&buf,
						  ",\n CASE WHEN prosecdef THEN '%s' ELSE '%s' END AS \"%s\"",
						  gettext_noop("definer"),
						  gettext_noop("invoker"),
						  gettext_noop("Security"));
		appendPQExpBuffer(&buf,
						  ",\n l.lanname as \"%s\"",
						  gettext_noop("Language"));
		if (pset.sversion >= 140000)
			appendPQExpBuffer(&buf,
							  ",\n COALESCE(pg_catalog.pg_get_function_sqlbody(p.oid), p.prosrc) as \"%s\"",
							  gettext_noop("Source code"));
		else
			appendPQExpBuffer(&buf,
							  ",\n p.prosrc as \"%s\"",
							  gettext_noop("Source code"));
		appendPQExpBuffer(&buf,
						  ",\n pg_catalog.obj_description(p.oid, 'pg_proc') as \"%s\"",
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_proc p"
						 "\n     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace\n");

	for (int i = 0; i < num_arg_patterns; i++)
	{
		appendPQExpBuffer(&buf,
						  "     LEFT JOIN pg_catalog.pg_type t%d ON t%d.oid = p.proargtypes[%d]\n"
						  "     LEFT JOIN pg_catalog.pg_namespace nt%d ON nt%d.oid = t%d.typnamespace\n",
						  i, i, i, i, i, i);
	}

	if (verbose)
		appendPQExpBufferStr(&buf,
							 "     LEFT JOIN pg_catalog.pg_language l ON l.oid = p.prolang\n");

	have_where = false;

	/* filter by function type, if requested */
	if (showNormal && showAggregate && showProcedure && showTrigger && showWindow)
		 /* Do nothing */ ;
	else if (showNormal)
	{
		if (!showAggregate)
		{
			if (have_where)
				appendPQExpBufferStr(&buf, "      AND ");
			else
			{
				appendPQExpBufferStr(&buf, "WHERE ");
				have_where = true;
			}
			if (pset.sversion >= 110000)
				appendPQExpBufferStr(&buf, "p.prokind <> 'a'\n");
			else
				appendPQExpBufferStr(&buf, "NOT p.proisagg\n");
		}
		if (!showProcedure && pset.sversion >= 110000)
		{
			if (have_where)
				appendPQExpBufferStr(&buf, "      AND ");
			else
			{
				appendPQExpBufferStr(&buf, "WHERE ");
				have_where = true;
			}
			appendPQExpBufferStr(&buf, "p.prokind <> 'p'\n");
		}
		if (!showTrigger)
		{
			if (have_where)
				appendPQExpBufferStr(&buf, "      AND ");
			else
			{
				appendPQExpBufferStr(&buf, "WHERE ");
				have_where = true;
			}
			appendPQExpBufferStr(&buf, "p.prorettype <> 'pg_catalog.trigger'::pg_catalog.regtype\n");
		}
		if (!showWindow && pset.sversion >= 80400)
		{
			if (have_where)
				appendPQExpBufferStr(&buf, "      AND ");
			else
			{
				appendPQExpBufferStr(&buf, "WHERE ");
				have_where = true;
			}
			if (pset.sversion >= 110000)
				appendPQExpBufferStr(&buf, "p.prokind <> 'w'\n");
			else
				appendPQExpBufferStr(&buf, "NOT p.proiswindow\n");
		}
	}
	else
	{
		bool		needs_or = false;

		appendPQExpBufferStr(&buf, "WHERE (\n       ");
		have_where = true;
		/* Note: at least one of these must be true ... */
		if (showAggregate)
		{
			if (pset.sversion >= 110000)
				appendPQExpBufferStr(&buf, "p.prokind = 'a'\n");
			else
				appendPQExpBufferStr(&buf, "p.proisagg\n");
			needs_or = true;
		}
		if (showTrigger)
		{
			if (needs_or)
				appendPQExpBufferStr(&buf, "       OR ");
			appendPQExpBufferStr(&buf,
								 "p.prorettype = 'pg_catalog.trigger'::pg_catalog.regtype\n");
			needs_or = true;
		}
		if (showProcedure)
		{
			if (needs_or)
				appendPQExpBufferStr(&buf, "       OR ");
			appendPQExpBufferStr(&buf, "p.prokind = 'p'\n");
			needs_or = true;
		}
		if (showWindow)
		{
			if (needs_or)
				appendPQExpBufferStr(&buf, "       OR ");
			if (pset.sversion >= 110000)
				appendPQExpBufferStr(&buf, "p.prokind = 'w'\n");
			else
				appendPQExpBufferStr(&buf, "p.proiswindow\n");
			needs_or = true;
		}
		appendPQExpBufferStr(&buf, "      )\n");
	}

	if (!validateSQLNamePattern(&buf, func_pattern, have_where, false,
								"n.nspname", "p.proname", NULL,
								"pg_catalog.pg_function_is_visible(p.oid)",
								NULL, 3))
		return false;

	for (int i = 0; i < num_arg_patterns; i++)
	{
		if (strcmp(arg_patterns[i], "-") != 0)
		{
			/*
			 * Match type-name patterns against either internal or external
			 * name, like \dT.  Unlike \dT, there seems no reason to
			 * discriminate against arrays or composite types.
			 */
			char		nspname[64];
			char		typname[64];
			char		ft[64];
			char		tiv[64];

			snprintf(nspname, sizeof(nspname), "nt%d.nspname", i);
			snprintf(typname, sizeof(typname), "t%d.typname", i);
			snprintf(ft, sizeof(ft),
					 "pg_catalog.format_type(t%d.oid, NULL)", i);
			snprintf(tiv, sizeof(tiv),
					 "pg_catalog.pg_type_is_visible(t%d.oid)", i);
			if (!validateSQLNamePattern(&buf,
										map_typename_pattern(arg_patterns[i]),
										true, false,
										nspname, typname, ft, tiv,
										NULL, 3))
				return false;
		}
		else
		{
			/* "-" pattern specifies no such parameter */
			appendPQExpBuffer(&buf, "  AND t%d.typname IS NULL\n", i);
		}
	}

	if (!showSystem && !func_pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 4;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of functions");
	myopt.translate_header = true;
	if (pset.sversion >= 90600)
	{
		myopt.translate_columns = translate_columns;
		myopt.n_translate_columns = lengthof(translate_columns);
	}
	else
	{
		myopt.translate_columns = translate_columns_pre_96;
		myopt.n_translate_columns = lengthof(translate_columns_pre_96);
	}

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}



/*
 * \dT
 * describe types
 */
bool
describeTypes(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  pg_catalog.format_type(t.oid, NULL) AS \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"));
	if (verbose)
		appendPQExpBuffer(&buf,
						  "  t.typname AS \"%s\",\n"
						  "  CASE WHEN t.typrelid != 0\n"
						  "      THEN CAST('tuple' AS pg_catalog.text)\n"
						  "    WHEN t.typlen < 0\n"
						  "      THEN CAST('var' AS pg_catalog.text)\n"
						  "    ELSE CAST(t.typlen AS pg_catalog.text)\n"
						  "  END AS \"%s\",\n",
						  gettext_noop("Internal name"),
						  gettext_noop("Size"));
	if (verbose && pset.sversion >= 90200)
	{
		appendPQExpBufferStr(&buf, "\n  ");
	}

	appendPQExpBuffer(&buf,
					  "  pg_catalog.obj_description(t.oid, 'pg_type') as \"%s\"\n",
					  gettext_noop("Description"));

	appendPQExpBufferStr(&buf, "FROM pg_catalog.pg_type t\n"
						 "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = t.typnamespace\n");

	/*
	 * do not include complex types (typrelid!=0) unless they are standalone
	 * composite types
	 */
	appendPQExpBufferStr(&buf, "WHERE (t.typrelid = 0 ");
	appendPQExpBufferStr(&buf, "OR (SELECT c.relkind = " CppAsString2(RELKIND_COMPOSITE_TYPE)
						 " FROM pg_catalog.pg_class c "
						 "WHERE c.oid = t.typrelid))\n");

	/*
	 * do not include array types unless the pattern contains [] (before 8.3
	 * we have to use the assumption that their names start with underscore)
	 */
	if (pattern == NULL || strstr(pattern, "[]") == NULL)
	{
		if (pset.sversion >= 80300)
			appendPQExpBufferStr(&buf, "  AND NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type el WHERE el.oid = t.typelem AND el.typarray = t.oid)\n");
		else
			appendPQExpBufferStr(&buf, "  AND t.typname NOT LIKE '\\_%' ESCAPE '\\'\n");
	}

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	/* Match name pattern against either internal or external name */
	if (!validateSQLNamePattern(&buf, map_typename_pattern(pattern),
								true, false,
								"n.nspname", "t.typname",
								"pg_catalog.format_type(t.oid, NULL)",
								"pg_catalog.pg_type_is_visible(t.oid)",
								NULL, 3))
		return false;

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of data types");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * Map some variant type names accepted by the backend grammar into
 * canonical type names.
 *
 * Helper for \dT and other functions that take typename patterns.
 * This doesn't completely mask the fact that these names are special;
 * for example, a pattern of "dec*" won't magically match "numeric".
 * But it goes a long way to reduce the surprise factor.
 */
static const char *
map_typename_pattern(const char *pattern)
{
	static const char *const typename_map[] = {
		/*
		 * These names are accepted by gram.y, although they are neither the
		 * "real" name seen in pg_type nor the canonical name printed by
		 * format_type().
		 */
		"decimal", "numeric",
		"float", "double precision",
		"int", "integer",

		/*
		 * We also have to map the array names for cases where the canonical
		 * name is different from what pg_type says.
		 */
		"bool[]", "boolean[]",
		"decimal[]", "numeric[]",
		"float[]", "double precision[]",
		"float4[]", "real[]",
		"float8[]", "double precision[]",
		"int[]", "integer[]",
		"int2[]", "smallint[]",
		"int4[]", "integer[]",
		"int8[]", "bigint[]",
		"time[]", "time without time zone[]",
		"timestamp[]", "timestamp without time zone[]",
		"timestamptz[]", "timestamp with time zone[]",
		"varbit[]", "bit varying[]",
		"varchar[]", "character varying[]",
		NULL
	};

	if (pattern == NULL)
		return NULL;
	for (int i = 0; typename_map[i] != NULL; i += 2)
	{
		if (pg_strcasecmp(pattern, typename_map[i]) == 0)
			return typename_map[i + 1];
	}
	return pattern;
}


/*
 * \do
 * Describe operators
 */
bool
describeOperators(const char *oper_pattern,
				  char **arg_patterns, int num_arg_patterns,
				  bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	/*
	 * Note: before Postgres 9.1, we did not assign comments to any built-in
	 * operators, preferring to let the comment on the underlying function
	 * suffice.  The coalesce() on the obj_description() calls below supports
	 * this convention by providing a fallback lookup of a comment on the
	 * operator's function.  As of 9.1 there is a policy that every built-in
	 * operator should have a comment; so the coalesce() is no longer
	 * necessary so far as built-in operators are concerned.  We keep it
	 * anyway, for now, because (1) third-party modules may still be following
	 * the old convention, and (2) we'd need to do it anyway when talking to a
	 * pre-9.1 server.
	 *
	 * The support for postfix operators in this query is dead code as of
	 * Postgres 14, but we need to keep it for as long as we support talking
	 * to pre-v14 servers.
	 */

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  o.oprname AS \"%s\",\n"
					  "  CASE WHEN o.oprkind='l' THEN NULL ELSE pg_catalog.format_type(o.oprleft, NULL) END AS \"%s\",\n"
					  "  CASE WHEN o.oprkind='r' THEN NULL ELSE pg_catalog.format_type(o.oprright, NULL) END AS \"%s\",\n"
					  "  pg_catalog.format_type(o.oprresult, NULL) AS \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Left arg type"),
					  gettext_noop("Right arg type"),
					  gettext_noop("Result type"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  "  o.oprcode AS \"%s\",\n",
						  gettext_noop("Function"));

	appendPQExpBuffer(&buf,
					  "  coalesce(pg_catalog.obj_description(o.oid, 'pg_operator'),\n"
					  "           pg_catalog.obj_description(o.oprcode, 'pg_proc')) AS \"%s\"\n"
					  "FROM pg_catalog.pg_operator o\n"
					  "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = o.oprnamespace\n",
					  gettext_noop("Description"));

	if (num_arg_patterns >= 2)
	{
		num_arg_patterns = 2;	/* ignore any additional arguments */
		appendPQExpBufferStr(&buf,
							 "     LEFT JOIN pg_catalog.pg_type t0 ON t0.oid = o.oprleft\n"
							 "     LEFT JOIN pg_catalog.pg_namespace nt0 ON nt0.oid = t0.typnamespace\n"
							 "     LEFT JOIN pg_catalog.pg_type t1 ON t1.oid = o.oprright\n"
							 "     LEFT JOIN pg_catalog.pg_namespace nt1 ON nt1.oid = t1.typnamespace\n");
	}
	else if (num_arg_patterns == 1)
	{
		appendPQExpBufferStr(&buf,
							 "     LEFT JOIN pg_catalog.pg_type t0 ON t0.oid = o.oprright\n"
							 "     LEFT JOIN pg_catalog.pg_namespace nt0 ON nt0.oid = t0.typnamespace\n");
	}

	if (!showSystem && !oper_pattern)
		appendPQExpBufferStr(&buf, "WHERE n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, oper_pattern,
								!showSystem && !oper_pattern, true,
								"n.nspname", "o.oprname", NULL,
								"pg_catalog.pg_operator_is_visible(o.oid)",
								NULL, 3))
		return false;

	if (num_arg_patterns == 1)
		appendPQExpBufferStr(&buf, "  AND o.oprleft = 0\n");

	for (int i = 0; i < num_arg_patterns; i++)
	{
		if (strcmp(arg_patterns[i], "-") != 0)
		{
			/*
			 * Match type-name patterns against either internal or external
			 * name, like \dT.  Unlike \dT, there seems no reason to
			 * discriminate against arrays or composite types.
			 */
			char		nspname[64];
			char		typname[64];
			char		ft[64];
			char		tiv[64];

			snprintf(nspname, sizeof(nspname), "nt%d.nspname", i);
			snprintf(typname, sizeof(typname), "t%d.typname", i);
			snprintf(ft, sizeof(ft),
					 "pg_catalog.format_type(t%d.oid, NULL)", i);
			snprintf(tiv, sizeof(tiv),
					 "pg_catalog.pg_type_is_visible(t%d.oid)", i);
			if (!validateSQLNamePattern(&buf,
										map_typename_pattern(arg_patterns[i]),
										true, false,
										nspname, typname, ft, tiv,
										NULL, 3))
				return false;
		}
		else
		{
			/* "-" pattern specifies no such parameter */
			appendPQExpBuffer(&buf, "  AND t%d.typname IS NULL\n", i);
		}
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 3, 4;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of operators");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * listAllDbs
 *
 * for \l, \list, and -l switch
 */
bool
listAllDbs(const char *pattern, bool verbose)
{
	PGresult   *res;
	PQExpBufferData buf;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT d.datname as \"%s\",\n"
					  "       pg_catalog.pg_encoding_to_char(d.encoding) as \"%s\",\n",
					  gettext_noop("Name"),
					  gettext_noop("Encoding"));
	if (pset.sversion >= 80400)
		appendPQExpBuffer(&buf,
						  "       d.datcollate as \"%s\",\n"
						  "       d.datctype as \"%s\"\n",
						  gettext_noop("Collate"),
						  gettext_noop("Ctype"));
	appendPQExpBufferStr(&buf, "       ");
	if (verbose && pset.sversion >= 80200)
	{
		appendPQExpBuffer(&buf,
						  ",\n       CASE WHEN pg_catalog.has_database_privilege(d.datname, 'CONNECT')\n"
						  "          %s"
						  "            THEN pg_catalog.pg_size_pretty(pg_catalog.pg_database_size(d.datname))\n"
						  "            ELSE 'No Access'\n"
						  "       END as \"%s\"",
						  pset.sversion >= 100000 ? "OR pg_catalog.pg_has_role('pg_read_all_stats', 'USAGE')\n" : "",
						  gettext_noop("Size"));
	}
	if (verbose && pset.sversion >= 80000)
		appendPQExpBuffer(&buf,
						  ",\n       t.spcname as \"%s\"",
						  gettext_noop("Tablespace"));
	if (verbose && pset.sversion >= 80200)
		appendPQExpBuffer(&buf,
						  ",\n       pg_catalog.shobj_description(d.oid, 'pg_database') as \"%s\"",
						  gettext_noop("Description"));
	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_database d\n");
	if (verbose && pset.sversion >= 80000)
		appendPQExpBufferStr(&buf,
							 "  JOIN pg_catalog.pg_tablespace t on d.dattablespace = t.oid\n");

	if (pattern)
		if (!validateSQLNamePattern(&buf, pattern, false, false,
									NULL, "d.datname", NULL, NULL,
									NULL, 1))
			return false;

	appendPQExpBufferStr(&buf, "ORDER BY 1;");
	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of databases");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * Get object comments
 *
 * \dd [foo]
 *
 * Note: This command only lists comments for object types which do not have
 * their comments displayed by their own backslash commands. The following
 * types of objects will be displayed: constraint, operator class,
 * operator family, rule, and trigger.
 *
 */
bool
objectDescription(const char *pattern, bool showSystem)
{
	/* minipg: COMMENT ON / 对象注释功能已被裁剪，\dd 命令不再提供任何内容 */
	return false;
}


/*
 * describeTableDetails (for \d)
 *
 * This routine finds the tables to be displayed, and calls
 * describeOneTableDetails for each one.
 *
 * verbose: if true, this is \d+
 */
bool
describeTableDetails(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	int			i;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT c.oid,\n"
					  "  n.nspname,\n"
					  "  c.relname\n"
					  "FROM pg_catalog.pg_class c\n"
					  "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, !showSystem && !pattern, false,
								"n.nspname", "c.relname", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
		return false;

	appendPQExpBufferStr(&buf, "ORDER BY 2, 3;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
		{
			if (pattern)
				pg_log_error("Did not find any relation named \"%s\".",
							 pattern);
			else
				pg_log_error("Did not find any relations.");
		}
		PQclear(res);
		return false;
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *oid;
		const char *nspname;
		const char *relname;

		oid = PQgetvalue(res, i, 0);
		nspname = PQgetvalue(res, i, 1);
		relname = PQgetvalue(res, i, 2);

		if (!describeOneTableDetails(nspname, relname, oid, verbose))
		{
			PQclear(res);
			return false;
		}
		if (cancel_pressed)
		{
			PQclear(res);
			return false;
		}
	}

	PQclear(res);
	return true;
}

/*
 * describeOneTableDetails (for \d)
 *
 * Unfortunately, the information presented here is so complicated that it
 * cannot be done in a single query. So we have to assemble the printed table
 * by hand and pass it to the underlying printTable() function.
 */
static bool
describeOneTableDetails(const char *schemaname,
						const char *relationname,
						const char *oid,
						bool verbose)
{
	bool		retval = false;
	PQExpBufferData buf;
	PGresult   *res = NULL;
	printTableOpt myopt = pset.popt.topt;
	printTableContent cont;
	bool		printTableInitialized = false;
	int			i;
	char	   *view_def = NULL;
	char	   *headers[12];
	PQExpBufferData title;
	PQExpBufferData tmpbuf;
	int			cols;
	int			attname_col = -1,	/* column indexes in "res" */
				atttype_col = -1,
				attrdef_col = -1,
				attnotnull_col = -1,
				isindexkey_col = -1,
				indexdef_col = -1,
				attstorage_col = -1,
				attcompression_col = -1,
				attstattarget_col = -1,
				attdescr_col = -1;
	int			numrows;
	struct
	{
		int16		checks;
		char		relkind;
		bool		hasindex;
		bool		hasrules;
		bool		hasoids;
		bool		ispartition;
		Oid			tablespace;
		char	   *reloptions;
		char		relpersistence;
		char	   *relam;
	}			tableinfo;
	bool		show_column_details = false;

	myopt.default_footer = false;
	/* This output looks confusing in expanded mode. */
	myopt.expanded = false;

	initPQExpBuffer(&buf);
	initPQExpBuffer(&title);
	initPQExpBuffer(&tmpbuf);

	/* Get general table info */
	if (pset.sversion >= 120000)
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "false AS relrowsecurity, false AS relforcerowsecurity, "
						  "false AS relhasoids, false AS relispartition, %s, c.reltablespace, "
						  "c.relpersistence, am.amname\n"
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "LEFT JOIN pg_catalog.pg_am am ON (c.relam = am.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else if (pset.sversion >= 100000)
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "false AS relrowsecurity, false AS relforcerowsecurity, "
						  "false AS relhasoids, false AS relispartition, %s, c.reltablespace, "
						  "c.relpersistence\n"
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else if (pset.sversion >= 90500)
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "false AS relrowsecurity, false AS relforcerowsecurity, "
						  "false AS relhasoids, false as relispartition, %s, c.reltablespace, "
						  "c.relpersistence\n"
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else if (pset.sversion >= 90400)
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  ""
						  "false as relispartition, %s, c.reltablespace, "
						  "c.relpersistence\n"
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else if (pset.sversion >= 90100)
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  ""
						  "false as relispartition, %s, c.reltablespace, "
						  "c.relpersistence\n"
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else if (pset.sversion >= 90000)
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  ""
						  "false as relispartition, %s, c.reltablespace, "
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else if (pset.sversion >= 80400)
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  ""
						  "false as relispartition, %s, c.reltablespace\n"
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else if (pset.sversion >= 80200)
	{
		printfPQExpBuffer(&buf,
						  "SELECT relchecks, relkind, relhasindex, relhasrules, "
						  "reltriggers <> 0, false, false, relhasoids, "
						  "false as relispartition, %s, reltablespace\n"
						  "FROM pg_catalog.pg_class WHERE oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(reloptions, E', ')" : "''"),
						  oid);
	}
	else if (pset.sversion >= 80000)
	{
		printfPQExpBuffer(&buf,
						  "SELECT relchecks, relkind, relhasindex, relhasrules, "
						  "reltriggers <> 0, false, false, relhasoids, "
						  "false as relispartition, '', reltablespace\n"
						  "FROM pg_catalog.pg_class WHERE oid = '%s';",
						  oid);
	}
	else
	{
		printfPQExpBuffer(&buf,
						  "SELECT relchecks, relkind, relhasindex, relhasrules, "
						  "reltriggers <> 0, false, false, relhasoids, "
						  "false as relispartition, '', ''\n"
						  "FROM pg_catalog.pg_class WHERE oid = '%s';",
						  oid);
	}

	res = PSQLexec(buf.data);
	if (!res)
		goto error_return;

	/* Did we get anything? */
	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
			pg_log_error("Did not find any relation with OID %s.", oid);
		goto error_return;
	}

	tableinfo.checks = atoi(PQgetvalue(res, 0, 0));
	tableinfo.relkind = *(PQgetvalue(res, 0, 1));
	tableinfo.hasindex = strcmp(PQgetvalue(res, 0, 2), "t") == 0;
	tableinfo.hasrules = strcmp(PQgetvalue(res, 0, 3), "t") == 0;
	tableinfo.hasoids = strcmp(PQgetvalue(res, 0, 6), "t") == 0;
	tableinfo.ispartition = strcmp(PQgetvalue(res, 0, 7), "t") == 0;
	tableinfo.reloptions = (pset.sversion >= 80200) ?
		pg_strdup(PQgetvalue(res, 0, 8)) : NULL;
	tableinfo.tablespace = (pset.sversion >= 80000) ?
		atooid(PQgetvalue(res, 0, 9)) : 0;
	tableinfo.relpersistence = (pset.sversion >= 90100) ?
		*(PQgetvalue(res, 0, 10)) : 0;
	if (pset.sversion >= 120000)
		tableinfo.relam = PQgetisnull(res, 0, 11) ?
			(char *) NULL : pg_strdup(PQgetvalue(res, 0, 11));
	else
		tableinfo.relam = NULL;
	PQclear(res);
	res = NULL;

	/* Identify whether we should print collation, nullable, default vals */
	if (tableinfo.relkind == RELKIND_RELATION ||
		tableinfo.relkind == RELKIND_VIEW ||
		tableinfo.relkind == RELKIND_COMPOSITE_TYPE ||
		tableinfo.relkind == 'p')
		show_column_details = true;

	/*
	 * Get per-column info
	 *
	 * Since the set of query columns we need varies depending on relkind and
	 * server version, we compute all the column numbers on-the-fly.  Column
	 * number variables for columns not fetched are left as -1; this avoids
	 * duplicative test logic below.
	 */
	cols = 0;
	printfPQExpBuffer(&buf, "SELECT a.attname");
	attname_col = cols++;
	appendPQExpBufferStr(&buf, ",\n  pg_catalog.format_type(a.atttypid, a.atttypmod)");
	atttype_col = cols++;

	if (show_column_details)
	{
		/* use "pretty" mode for expression to avoid excessive parentheses */
		appendPQExpBufferStr(&buf,
							 ",\n  (SELECT pg_catalog.pg_get_expr(d.adbin, d.adrelid, true)"
							 "\n   FROM pg_catalog.pg_attrdef d"
							 "\n   WHERE d.adrelid = a.attrelid AND d.adnum = a.attnum AND a.atthasdef)"
							 ",\n  a.attnotnull");
		attrdef_col = cols++;
		attnotnull_col = cols++;
	}
	if (tableinfo.relkind == RELKIND_INDEX ||
		tableinfo.relkind == 'I')
	{
		if (pset.sversion >= 110000)
		{
			appendPQExpBuffer(&buf, ",\n  CASE WHEN a.attnum <= (SELECT i.indnkeyatts FROM pg_catalog.pg_index i WHERE i.indexrelid = '%s') THEN '%s' ELSE '%s' END AS is_key",
							  oid,
							  gettext_noop("yes"),
							  gettext_noop("no"));
			isindexkey_col = cols++;
		}
	appendPQExpBufferStr(&buf, ",\n  pg_catalog.pg_get_indexdef(a.attrelid, a.attnum, TRUE) AS indexdef");
	indexdef_col = cols++;
	}
	if (verbose)
	{
		appendPQExpBufferStr(&buf, ",\n  a.attstorage");
		attstorage_col = cols++;

		/* compression info, if relevant to relkind */
		if (pset.sversion >= 140000 &&
			!pset.hide_compression &&
			(tableinfo.relkind == RELKIND_RELATION ||
			 tableinfo.relkind == 'p'))
		{
			appendPQExpBufferStr(&buf, ",\n  a.attcompression AS attcompression");
			attcompression_col = cols++;
		}

		/* stats target, if relevant to relkind */
		if (tableinfo.relkind == RELKIND_RELATION ||
			tableinfo.relkind == RELKIND_INDEX ||
			tableinfo.relkind == 'I' ||
			tableinfo.relkind == 'p')
		{
		appendPQExpBufferStr(&buf, ",\n  CASE WHEN a.attstattarget=-1 THEN NULL ELSE a.attstattarget END AS attstattarget");
			attstattarget_col = cols++;
		}

		/*
		 * minipg: COMMENTS 功能已裁剪，列注释查询（col_description）不再可用。
		 */
	if (tableinfo.relkind == RELKIND_RELATION ||
		tableinfo.relkind == RELKIND_VIEW ||
		tableinfo.relkind == RELKIND_COMPOSITE_TYPE ||
		tableinfo.relkind == 'p')
	{
			appendPQExpBufferStr(&buf, ",\n  pg_catalog.col_description(a.attrelid, a.attnum)");
			attdescr_col = cols++;
		}
	}

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_attribute a");
	appendPQExpBuffer(&buf, "\nWHERE a.attrelid = '%s' AND a.attnum > 0 AND NOT a.attisdropped", oid);
	appendPQExpBufferStr(&buf, "\nORDER BY a.attnum;");

	res = PSQLexec(buf.data);
	if (!res)
		goto error_return;
	numrows = PQntuples(res);

	/* Make title */
	switch (tableinfo.relkind)
	{
		case RELKIND_RELATION:
			if (tableinfo.relpersistence == 'u')
				printfPQExpBuffer(&title, _("Unlogged table \"%s.%s\""),
								  schemaname, relationname);
			else
				printfPQExpBuffer(&title, _("Table \"%s.%s\""),
								  schemaname, relationname);
			break;
		case RELKIND_VIEW:
			printfPQExpBuffer(&title, _("View \"%s.%s\""),
							  schemaname, relationname);
			break;
		case RELKIND_INDEX:
			if (tableinfo.relpersistence == 'u')
				printfPQExpBuffer(&title, _("Unlogged index \"%s.%s\""),
								  schemaname, relationname);
			else
				printfPQExpBuffer(&title, _("Index \"%s.%s\""),
								  schemaname, relationname);
			break;
		case 'I':
			if (tableinfo.relpersistence == 'u')
				printfPQExpBuffer(&title, _("Unlogged partitioned index \"%s.%s\""),
								  schemaname, relationname);
			else
				printfPQExpBuffer(&title, _("Partitioned index \"%s.%s\""),
								  schemaname, relationname);
			break;
		case 's':
			/* not used as of 8.2, but keep it for backwards compatibility */
			printfPQExpBuffer(&title, _("Special relation \"%s.%s\""),
							  schemaname, relationname);
			break;
		case RELKIND_TOASTVALUE:
			printfPQExpBuffer(&title, _("TOAST table \"%s.%s\""),
							  schemaname, relationname);
			break;
		case RELKIND_COMPOSITE_TYPE:
			printfPQExpBuffer(&title, _("Composite type \"%s.%s\""),
							  schemaname, relationname);
			break;
		case 'p':
			if (tableinfo.relpersistence == 'u')
				printfPQExpBuffer(&title, _("Unlogged partitioned table \"%s.%s\""),
								  schemaname, relationname);
			else
				printfPQExpBuffer(&title, _("Partitioned table \"%s.%s\""),
								  schemaname, relationname);
			break;
		default:
			/* untranslated unknown relkind */
			printfPQExpBuffer(&title, "?%c? \"%s.%s\"",
							  tableinfo.relkind, schemaname, relationname);
			break;
	}

	/* Fill headers[] with the names of the columns we will output */
	cols = 0;
	headers[cols++] = gettext_noop("Column");
	headers[cols++] = gettext_noop("Type");
	if (show_column_details)
	{
		headers[cols++] = gettext_noop("Nullable");
		headers[cols++] = gettext_noop("Default");
	}
	if (isindexkey_col >= 0)
		headers[cols++] = gettext_noop("Key?");
	if (indexdef_col >= 0)
		headers[cols++] = gettext_noop("Definition");
	if (attstorage_col >= 0)
		headers[cols++] = gettext_noop("Storage");
	if (attcompression_col >= 0)
		headers[cols++] = gettext_noop("Compression");
	if (attstattarget_col >= 0)
		headers[cols++] = gettext_noop("Stats target");
	if (attdescr_col >= 0)
		headers[cols++] = gettext_noop("Description");

	Assert(cols <= lengthof(headers));

	printTableInit(&cont, &myopt, title.data, cols, numrows);
	printTableInitialized = true;

	for (i = 0; i < cols; i++)
		printTableAddHeader(&cont, headers[i], true, 'l');

	/* Generate table cells to be printed */
	for (i = 0; i < numrows; i++)
	{
		/* Column */
		printTableAddCell(&cont, PQgetvalue(res, i, attname_col), false, false);

		/* Type */
		printTableAddCell(&cont, PQgetvalue(res, i, atttype_col), false, false);

		/* Nullable, Default */
		if (show_column_details)
		{
			printTableAddCell(&cont,
							  strcmp(PQgetvalue(res, i, attnotnull_col), "t") == 0 ? "not null" : "",
							  false, false);

			printTableAddCell(&cont, PQgetvalue(res, i, attrdef_col), false, false);
		}

		/* Info for index columns */
		if (isindexkey_col >= 0)
			printTableAddCell(&cont, PQgetvalue(res, i, isindexkey_col), true, false);
		if (indexdef_col >= 0)
			printTableAddCell(&cont, PQgetvalue(res, i, indexdef_col), false, false);

		/* Storage mode, if relevant */
		if (attstorage_col >= 0)
		{
			char	   *storage = PQgetvalue(res, i, attstorage_col);

			/* these strings are literal in our syntax, so not translated. */
			printTableAddCell(&cont, (storage[0] == 'p' ? "plain" :
									  (storage[0] == 'm' ? "main" :
									   (storage[0] == 'x' ? "extended" :
										(storage[0] == 'e' ? "external" :
										 "???")))),
							  false, false);
		}

		/* Column compression, if relevant */
		if (attcompression_col >= 0)
		{
			char	   *compression = PQgetvalue(res, i, attcompression_col);

			/* these strings are literal in our syntax, so not translated. */
			printTableAddCell(&cont, (compression[0] == 'p' ? "pglz" :
									  (compression[0] == 'l' ? "lz4" :
									   (compression[0] == '\0' ? "" :
										"???"))),
							  false, false);
		}

		/* Statistics target, if the relkind supports this feature */
		if (attstattarget_col >= 0)
			printTableAddCell(&cont, PQgetvalue(res, i, attstattarget_col),
							  false, false);

		/* Column comments, if the relkind supports this feature */
		if (attdescr_col >= 0)
			printTableAddCell(&cont, PQgetvalue(res, i, attdescr_col),
							  false, false);
	}

	/* Make footers */

	if (tableinfo.ispartition)
	{
		/* Footer information for a partition child table */
		PGresult   *result;

		printfPQExpBuffer(&buf,
						  "SELECT inhparent::pg_catalog.regclass,\n  ");

		appendPQExpBuffer(&buf,
						  pset.sversion >= 140000 ? "inhdetachpending" :
						  "false as inhdetachpending");

		/* If verbose, also request the partition constraint definition */
		if (verbose)
			appendPQExpBufferStr(&buf,
								 ",\n  pg_catalog.pg_get_partition_constraintdef(c.oid)");
		appendPQExpBuffer(&buf,
						  "\nFROM pg_catalog.pg_class c"
						  "\nWHERE c.oid = '%s' AND false;", oid);
		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;

		if (PQntuples(result) > 0)
		{
			char	   *parent_name = PQgetvalue(result, 0, 0);
			char	   *partdef = PQgetvalue(result, 0, 1);
			char	   *detached = PQgetvalue(result, 0, 2);

			printfPQExpBuffer(&tmpbuf, _("Partition of: %s %s%s"), parent_name,
							  partdef,
							  strcmp(detached, "t") == 0 ? " DETACH PENDING" : "");
			printTableAddFooter(&cont, tmpbuf.data);

			if (verbose)
			{
				char	   *partconstraintdef = NULL;

				if (!PQgetisnull(result, 0, 3))
					partconstraintdef = PQgetvalue(result, 0, 3);
				/* If there isn't any constraint, show that explicitly */
				if (partconstraintdef == NULL || partconstraintdef[0] == '\0')
					printfPQExpBuffer(&tmpbuf, _("No partition constraint"));
				else
					printfPQExpBuffer(&tmpbuf, _("Partition constraint: %s"),
									  partconstraintdef);
				printTableAddFooter(&cont, tmpbuf.data);
			}
		}
		PQclear(result);
	}

	if (tableinfo.relkind == 'p')
	{
		/* Footer information for a partitioned table (partitioning parent) */
		PGresult   *result;

		printfPQExpBuffer(&buf,
						  "SELECT pg_catalog.pg_get_partkeydef('%s'::pg_catalog.oid);",
						  oid);
		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;

		if (PQntuples(result) == 1)
		{
			char	   *partkeydef = PQgetvalue(result, 0, 0);

			printfPQExpBuffer(&tmpbuf, _("Partition key: %s"), partkeydef);
			printTableAddFooter(&cont, tmpbuf.data);
		}
		PQclear(result);
	}

	if (tableinfo.relkind == RELKIND_TOASTVALUE)
	{
		/* For a TOAST table, print name of owning table */
		PGresult   *result;

		printfPQExpBuffer(&buf,
						  "SELECT n.nspname, c.relname\n"
						  "FROM pg_catalog.pg_class c"
						  " JOIN pg_catalog.pg_namespace n"
						  " ON n.oid = c.relnamespace\n"
						  "WHERE reltoastrelid = '%s';", oid);
		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;

		if (PQntuples(result) == 1)
		{
			char	   *schemaname = PQgetvalue(result, 0, 0);
			char	   *relname = PQgetvalue(result, 0, 1);

			printfPQExpBuffer(&tmpbuf, _("Owning table: \"%s.%s\""),
							  schemaname, relname);
			printTableAddFooter(&cont, tmpbuf.data);
		}
		PQclear(result);
	}

	if (tableinfo.relkind == RELKIND_INDEX ||
		tableinfo.relkind == 'I')
	{
		/* Footer information about an index */
		PGresult   *result;

		printfPQExpBuffer(&buf,
						  "SELECT i.indisunique, i.indisprimary, i.indisclustered, ");
		if (pset.sversion >= 80200)
			appendPQExpBufferStr(&buf, "i.indisvalid,\n");
		else
			appendPQExpBufferStr(&buf, "true AS indisvalid,\n");

		appendPQExpBuffer(&buf, "  a.amname, c2.relname, "
						  "pg_catalog.pg_get_expr(i.indpred, i.indrelid, true)\n"
						  "FROM pg_catalog.pg_index i, pg_catalog.pg_class c, pg_catalog.pg_class c2, pg_catalog.pg_am a\n"
						  "WHERE i.indexrelid = c.oid AND c.oid = '%s' AND c.relam = a.oid\n"
						  "AND i.indrelid = c2.oid;",
						  oid);

		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;
		else if (PQntuples(result) != 1)
		{
			PQclear(result);
			goto error_return;
		}
		else
		{
			char	   *indisunique = PQgetvalue(result, 0, 0);
			char	   *indisprimary = PQgetvalue(result, 0, 1);
			char	   *indisclustered = PQgetvalue(result, 0, 2);
			char	   *indisvalid = PQgetvalue(result, 0, 3);
			char	   *indamname = PQgetvalue(result, 0, 4);
			char	   *indtable = PQgetvalue(result, 0, 5);
			char	   *indpred = PQgetvalue(result, 0, 6);

			if (strcmp(indisprimary, "t") == 0)
				printfPQExpBuffer(&tmpbuf, _("primary key, "));
			else if (strcmp(indisunique, "t") == 0)
				printfPQExpBuffer(&tmpbuf, _("unique, "));
			else
				resetPQExpBuffer(&tmpbuf);
			appendPQExpBuffer(&tmpbuf, "%s, ", indamname);

			/* we assume here that index and table are in same schema */
			appendPQExpBuffer(&tmpbuf, _("for table \"%s.%s\""),
							  schemaname, indtable);

			if (strlen(indpred))
				appendPQExpBuffer(&tmpbuf, _(", predicate (%s)"), indpred);

			if (strcmp(indisclustered, "t") == 0)
				appendPQExpBufferStr(&tmpbuf, _(", clustered"));

			if (strcmp(indisvalid, "t") != 0)
				appendPQExpBufferStr(&tmpbuf, _(", invalid"));

			printTableAddFooter(&cont, tmpbuf.data);

			/*
			 * If it's a partitioned index, we'll print the tablespace below
			 */
			if (tableinfo.relkind == RELKIND_INDEX)
				add_tablespace_footer(&cont, tableinfo.relkind,
									  tableinfo.tablespace, true);
		}

		PQclear(result);
	}
	/* If you add relkinds here, see also "Finish printing..." stanza below */
	else if (tableinfo.relkind == RELKIND_RELATION ||
			 tableinfo.relkind == 'p' ||
			 tableinfo.relkind == 'I' ||
			 tableinfo.relkind == RELKIND_TOASTVALUE)
	{
		/* Footer information about a table */
		PGresult   *result = NULL;
		int			tuples = 0;

		/* print indexes */
		if (tableinfo.hasindex)
		{
			printfPQExpBuffer(&buf,
							  "SELECT c2.relname, i.indisprimary, i.indisunique, i.indisclustered, ");
			if (pset.sversion >= 80200)
				appendPQExpBufferStr(&buf, "i.indisvalid, ");
			else
				appendPQExpBufferStr(&buf, "true as indisvalid, ");
			appendPQExpBufferStr(&buf, "pg_catalog.pg_get_indexdef(i.indexrelid, 0, true),\n  ");
			if (pset.sversion >= 90000)
				appendPQExpBufferStr(&buf,
									 "pg_catalog.pg_get_constraintdef(con.oid, true), "
									 "contype");
			else
				appendPQExpBufferStr(&buf,
									 "null AS constraintdef, null AS contype");
			if (pset.sversion >= 80000)
				appendPQExpBufferStr(&buf, ", c2.reltablespace");
			appendPQExpBufferStr(&buf,
								 "\nFROM pg_catalog.pg_class c, pg_catalog.pg_class c2, pg_catalog.pg_index i\n");
			if (pset.sversion >= 90000)
				appendPQExpBufferStr(&buf,
									 "  LEFT JOIN pg_catalog.pg_constraint con ON (conrelid = i.indrelid AND conindid = i.indexrelid AND contype IN ('p','u','x'))\n");
			appendPQExpBuffer(&buf,
							  "WHERE c.oid = '%s' AND c.oid = i.indrelid AND i.indexrelid = c2.oid\n"
							  "ORDER BY i.indisprimary DESC, c2.relname;",
							  oid);
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				printTableAddFooter(&cont, _("Indexes:"));
				for (i = 0; i < tuples; i++)
				{
					/* untranslated index name */
					printfPQExpBuffer(&buf, "    \"%s\"",
									  PQgetvalue(result, i, 0));

					/* If exclusion constraint, print the constraintdef */
					if (strcmp(PQgetvalue(result, i, 7), "x") == 0)
					{
						appendPQExpBuffer(&buf, " %s",
										  PQgetvalue(result, i, 6));
					}
					else
					{
						const char *indexdef;
						const char *usingpos;

						/* Label as primary key or unique (but not both) */
						if (strcmp(PQgetvalue(result, i, 1), "t") == 0)
							appendPQExpBufferStr(&buf, " PRIMARY KEY,");
						else if (strcmp(PQgetvalue(result, i, 2), "t") == 0)
						{
							if (strcmp(PQgetvalue(result, i, 7), "u") == 0)
								appendPQExpBufferStr(&buf, " UNIQUE CONSTRAINT,");
							else
								appendPQExpBufferStr(&buf, " UNIQUE,");
						}

						/* Everything after "USING" is echoed verbatim */
						indexdef = PQgetvalue(result, i, 5);
						usingpos = strstr(indexdef, " USING ");
						if (usingpos)
							indexdef = usingpos + 7;
						appendPQExpBuffer(&buf, " %s", indexdef);
					}

					/* Add these for all cases */
					if (strcmp(PQgetvalue(result, i, 3), "t") == 0)
						appendPQExpBufferStr(&buf, " CLUSTER");

					if (strcmp(PQgetvalue(result, i, 4), "t") != 0)
						appendPQExpBufferStr(&buf, " INVALID");

					printTableAddFooter(&cont, buf.data);

					/* Print tablespace of the index on the same line */
					if (pset.sversion >= 80000)
						add_tablespace_footer(&cont, RELKIND_INDEX,
											  atooid(PQgetvalue(result, i, 8)),
											  false);
				}
			}
			PQclear(result);
		}

		/* print table (and column) check constraints */
		if (tableinfo.checks)
		{
			printfPQExpBuffer(&buf,
							  "SELECT r.conname, "
							  "pg_catalog.pg_get_constraintdef(r.oid, true)\n"
							  "FROM pg_catalog.pg_constraint r\n"
							  "WHERE r.conrelid = '%s' AND r.contype = 'c'\n"
							  "ORDER BY 1;",
							  oid);
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				printTableAddFooter(&cont, _("Check constraints:"));
				for (i = 0; i < tuples; i++)
				{
					/* untranslated constraint name and def */
					printfPQExpBuffer(&buf, "    \"%s\" %s",
									  PQgetvalue(result, i, 0),
									  PQgetvalue(result, i, 1));

					printTableAddFooter(&cont, buf.data);
				}
			}
			PQclear(result);
		}



		/* print rules */
		if (tableinfo.hasrules)
		{
			if (pset.sversion >= 80300)
			{
				printfPQExpBuffer(&buf,
								  "SELECT r.rulename, trim(trailing ';' from pg_catalog.pg_get_ruledef(r.oid, true)), "
								  "ev_enabled\n"
								  "FROM pg_catalog.pg_rewrite r\n"
								  "WHERE r.ev_class = '%s' ORDER BY 1;",
								  oid);
			}
			else
			{
				printfPQExpBuffer(&buf,
								  "SELECT r.rulename, trim(trailing ';' from pg_catalog.pg_get_ruledef(r.oid, true)), "
								  "'O' AS ev_enabled\n"
								  "FROM pg_catalog.pg_rewrite r\n"
								  "WHERE r.ev_class = '%s' ORDER BY 1;",
								  oid);
			}
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				bool		have_heading;
				int			category;

				for (category = 0; category < 4; category++)
				{
					have_heading = false;

					for (i = 0; i < tuples; i++)
					{
						const char *ruledef;
						bool		list_rule = false;

						switch (category)
						{
							case 0:
								if (*PQgetvalue(result, i, 2) == 'O')
									list_rule = true;
								break;
							case 1:
								if (*PQgetvalue(result, i, 2) == 'D')
									list_rule = true;
								break;
							case 2:
								if (*PQgetvalue(result, i, 2) == 'A')
									list_rule = true;
								break;
							case 3:
								if (*PQgetvalue(result, i, 2) == 'R')
									list_rule = true;
								break;
						}
						if (!list_rule)
							continue;

						if (!have_heading)
						{
							switch (category)
							{
								case 0:
									printfPQExpBuffer(&buf, _("Rules:"));
									break;
								case 1:
									printfPQExpBuffer(&buf, _("Disabled rules:"));
									break;
								case 2:
									printfPQExpBuffer(&buf, _("Rules firing always:"));
									break;
								case 3:
									printfPQExpBuffer(&buf, _("Rules firing on replica only:"));
									break;
							}
							printTableAddFooter(&cont, buf.data);
							have_heading = true;
						}

						/* Everything after "CREATE RULE" is echoed verbatim */
						ruledef = PQgetvalue(result, i, 1);
						ruledef += 12;
						printfPQExpBuffer(&buf, "    %s", ruledef);
						printTableAddFooter(&cont, buf.data);
					}
				}
			}
			PQclear(result);
		}

		/* print any publications */
		if (pset.sversion >= 100000)
		{
			printfPQExpBuffer(&buf,
							  "SELECT pubname\n"
							  "FROM pg_catalog.pg_publication p\n"
							  "JOIN pg_catalog.pg_publication_rel pr ON p.oid = pr.prpubid\n"
							  "WHERE pr.prrelid = '%s'\n"
							  "UNION ALL\n"
							  "SELECT pubname\n"
							  "FROM pg_catalog.pg_publication p\n"
							  "WHERE p.puballtables AND pg_catalog.pg_relation_is_publishable('%s')\n"
							  "ORDER BY 1;",
							  oid, oid);

			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
				printTableAddFooter(&cont, _("Publications:"));

			/* Might be an empty set - that's ok */
			for (i = 0; i < tuples; i++)
			{
				printfPQExpBuffer(&buf, "    \"%s\"",
								  PQgetvalue(result, i, 0));

				printTableAddFooter(&cont, buf.data);
			}
			PQclear(result);
		}
	}

	/* Get view_def if table is a view or materialized view */
	if (tableinfo.relkind == RELKIND_VIEW && verbose)
	{
		PGresult   *result;

		printfPQExpBuffer(&buf,
						  "SELECT pg_catalog.pg_get_viewdef('%s'::pg_catalog.oid, true);",
						  oid);
		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;

		if (PQntuples(result) > 0)
			view_def = pg_strdup(PQgetvalue(result, 0, 0));

		PQclear(result);
	}

	if (view_def)
	{
		PGresult   *result = NULL;

		/* Footer information about a view */
		printTableAddFooter(&cont, _("View definition:"));
		printTableAddFooter(&cont, view_def);

		/* print rules */
		if (tableinfo.hasrules)
		{
			printfPQExpBuffer(&buf,
							  "SELECT r.rulename, trim(trailing ';' from pg_catalog.pg_get_ruledef(r.oid, true))\n"
							  "FROM pg_catalog.pg_rewrite r\n"
							  "WHERE r.ev_class = '%s' AND r.rulename != '_RETURN' ORDER BY 1;",
							  oid);
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;

			if (PQntuples(result) > 0)
			{
				printTableAddFooter(&cont, _("Rules:"));
				for (i = 0; i < PQntuples(result); i++)
				{
					const char *ruledef;

					/* Everything after "CREATE RULE" is echoed verbatim */
					ruledef = PQgetvalue(result, i, 1);
					ruledef += 12;

					printfPQExpBuffer(&buf, " %s", ruledef);
					printTableAddFooter(&cont, buf.data);
				}
			}
			PQclear(result);
		}
	}


	/*
	 * Finish printing the footer information about a table.
	 */
	if (tableinfo.relkind == RELKIND_RELATION ||
		tableinfo.relkind == 'p' ||
		tableinfo.relkind == 'I' ||
		tableinfo.relkind == RELKIND_TOASTVALUE)
	{
		bool		is_partitioned;
		PGresult   *result;
		int			tuples;

		/* simplify some repeated tests below */
		is_partitioned = (tableinfo.relkind == 'p' ||
						  tableinfo.relkind == 'I');


		/* print tables inherited from (exclude partitioned parents) */
		printfPQExpBuffer(&buf,
						  "SELECT c.oid::pg_catalog.regclass\n"
						  "FROM pg_catalog.pg_class c\n"
						  "WHERE c.oid = '%s' AND false\n"
						  "ORDER BY c.oid::pg_catalog.regclass::pg_catalog.text;",
						  oid);

		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;
		else
		{
			const char *s = _("Inherits");
			int			sw = pg_wcswidth(s, strlen(s), pset.encoding);

			tuples = PQntuples(result);

			for (i = 0; i < tuples; i++)
			{
				if (i == 0)
					printfPQExpBuffer(&buf, "%s: %s",
									  s, PQgetvalue(result, i, 0));
				else
					printfPQExpBuffer(&buf, "%*s  %s",
									  sw, "", PQgetvalue(result, i, 0));
				if (i < tuples - 1)
					appendPQExpBufferChar(&buf, ',');

				printTableAddFooter(&cont, buf.data);
			}

			PQclear(result);
		}

		/*
		 * print child tables (with additional info if partitions)
		 *
		 * NOTE: minipg has removed the pg_inherits system catalog (inheritance
		 * and partitioning are no longer supported), so there can never be any
		 * child tables.  We emit a query that returns no rows without
		 * referencing pg_inherits, keeping \d on relations safe.
		 */
		printfPQExpBuffer(&buf,
						  "SELECT c.oid::pg_catalog.regclass, c.relkind,"
						  " false AS inhdetachpending,"
						  " NULL\n"
						  "FROM pg_catalog.pg_class c\n"
						  "WHERE c.oid = '%s' AND false\n"
						  "ORDER BY c.oid::pg_catalog.regclass::pg_catalog.text;",
						  oid);

		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;
		tuples = PQntuples(result);

		/*
		 * For a partitioned table with no partitions, always print the number
		 * of partitions as zero, even when verbose output is expected.
		 * Otherwise, we will not print "Partitions" section for a partitioned
		 * table without any partitions.
		 */
		if (is_partitioned && tuples == 0)
		{
			printfPQExpBuffer(&buf, _("Number of partitions: %d"), tuples);
			printTableAddFooter(&cont, buf.data);
		}
		else if (!verbose)
		{
			/* print the number of child tables, if any */
			if (tuples > 0)
			{
				if (is_partitioned)
					printfPQExpBuffer(&buf, _("Number of partitions: %d (Use \\d+ to list them.)"), tuples);
				else
					printfPQExpBuffer(&buf, _("Number of child tables: %d (Use \\d+ to list them.)"), tuples);
				printTableAddFooter(&cont, buf.data);
			}
		}
		else
		{
			/* display the list of child tables */
			const char *ct = is_partitioned ? _("Partitions") : _("Child tables");
			int			ctw = pg_wcswidth(ct, strlen(ct), pset.encoding);

			for (i = 0; i < tuples; i++)
			{
				char		child_relkind = *PQgetvalue(result, i, 1);

				if (i == 0)
					printfPQExpBuffer(&buf, "%s: %s",
									  ct, PQgetvalue(result, i, 0));
				else
					printfPQExpBuffer(&buf, "%*s  %s",
									  ctw, "", PQgetvalue(result, i, 0));
				if (!PQgetisnull(result, i, 3))
					appendPQExpBuffer(&buf, " %s", PQgetvalue(result, i, 3));
				if (child_relkind == 'p' ||
					child_relkind == 'I')
					appendPQExpBufferStr(&buf, ", PARTITIONED");
				if (strcmp(PQgetvalue(result, i, 2), "t") == 0)
					appendPQExpBufferStr(&buf, " (DETACH PENDING)");
				if (i < tuples - 1)
					appendPQExpBufferChar(&buf, ',');

				printTableAddFooter(&cont, buf.data);
			}
		}
		PQclear(result);

		/* Table type */

			/* OIDs, if verbose */
		if (verbose && tableinfo.hasoids)
			printTableAddFooter(&cont, _("Has OIDs: yes"));

		/* Tablespace info */
		add_tablespace_footer(&cont, tableinfo.relkind, tableinfo.tablespace,
							  true);

		/* Access method info */
		if (verbose && tableinfo.relam != NULL && !pset.hide_tableam)
		{
			printfPQExpBuffer(&buf, _("Access method: %s"), tableinfo.relam);
			printTableAddFooter(&cont, buf.data);
		}
	}

	/* reloptions, if verbose */
	if (verbose &&
		tableinfo.reloptions && tableinfo.reloptions[0] != '\0')
	{
		const char *t = _("Options");

		printfPQExpBuffer(&buf, "%s: %s", t, tableinfo.reloptions);
		printTableAddFooter(&cont, buf.data);
	}

	printTable(&cont, pset.queryFout, false, pset.logfile);

	retval = true;

error_return:

	/* clean up */
	if (printTableInitialized)
		printTableCleanup(&cont);
	termPQExpBuffer(&buf);
	termPQExpBuffer(&title);
	termPQExpBuffer(&tmpbuf);

	if (view_def)
		free(view_def);

	if (res)
		PQclear(res);

	return retval;
}

/*
 * Add a tablespace description to a footer.  If 'newline' is true, it is added
 * in a new line; otherwise it's appended to the current value of the last
 * footer.
 */
static void
add_tablespace_footer(printTableContent *const cont, char relkind,
					  Oid tablespace, const bool newline)
{
	/* relkinds for which we support tablespaces */
	if (relkind == RELKIND_RELATION ||
		relkind == RELKIND_INDEX ||
		relkind == 'p' ||
		relkind == 'I' ||
		relkind == RELKIND_TOASTVALUE)
	{
		/*
		 * We ignore the database default tablespace so that users not using
		 * tablespaces don't need to know about them.  This case also covers
		 * pre-8.0 servers, for which tablespace will always be 0.
		 */
		if (tablespace != 0)
		{
			PGresult   *result = NULL;
			PQExpBufferData buf;

			initPQExpBuffer(&buf);
			printfPQExpBuffer(&buf,
							  "SELECT spcname FROM pg_catalog.pg_tablespace\n"
							  "WHERE oid = '%u';", tablespace);
			result = PSQLexec(buf.data);
			if (!result)
			{
				termPQExpBuffer(&buf);
				return;
			}
			/* Should always be the case, but.... */
			if (PQntuples(result) > 0)
			{
				if (newline)
				{
					/* Add the tablespace as a new footer */
					printfPQExpBuffer(&buf, _("Tablespace: \"%s\""),
									  PQgetvalue(result, 0, 0));
					printTableAddFooter(cont, buf.data);
				}
				else
				{
					/* Append the tablespace to the latest footer */
					printfPQExpBuffer(&buf, "%s", cont->footer->data);

					/*-------
					   translator: before this string there's an index description like
					   '"foo_pkey" PRIMARY KEY, btree (a)' */
					appendPQExpBuffer(&buf, _(", tablespace \"%s\""),
									  PQgetvalue(result, 0, 0));
					printTableSetFooter(cont, buf.data);
				}
			}
			PQclear(result);
			termPQExpBuffer(&buf);
		}
	}
}

/*
 * listTables()
 *
 * handler for \dt, \di, etc.
 *
 * tabtypes is an array of characters, specifying what info is desired:
 * t - tables
 * i - indexes
 * v - views
 * m - materialized views
 * (any order of the above is fine)
 */
bool
listTables(const char *tabtypes, const char *pattern, bool verbose, bool showSystem)
{
	bool		showTables = strchr(tabtypes, 't') != NULL;
	bool		showIndexes = strchr(tabtypes, 'i') != NULL;
	bool		showViews = strchr(tabtypes, 'v') != NULL;
	bool		showMatViews = strchr(tabtypes, 'm') != NULL;
	bool		showSeq = strchr(tabtypes, 's') != NULL;

	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	int			cols_so_far;
	bool		translate_columns[] = {false, false, true, false, false, false, false, false, false};

	/* If tabtypes is empty, we default to \dtvms (but see also command.c) */
	if (!(showTables || showIndexes || showViews || showMatViews || showSeq))
		showTables = showViews = showMatViews = showSeq = true;

	initPQExpBuffer(&buf);

	/*
	 * Note: as of Pg 8.2, we no longer use relkind 's' (special), but we keep
	 * it here for backwards compatibility.
	 */
	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  c.relname as \"%s\",\n"
					  "  CASE c.relkind"
					  " WHEN " CppAsString2(RELKIND_RELATION) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_VIEW) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_INDEX) " THEN '%s'"
					  " WHEN 's' THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_TOASTVALUE) " THEN '%s'"
					  " WHEN " "'p'" " THEN '%s'"
					  " WHEN " "'I'" " THEN '%s'"
					  " END as \"%s\"",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("table"),
					  gettext_noop("view"),
					  gettext_noop("index"),
					  gettext_noop("special"),
					  gettext_noop("TOAST table"),
					  gettext_noop("partitioned table"),
					  gettext_noop("partitioned index"),
					  gettext_noop("Type"));
	cols_so_far = 3;

	if (showIndexes)
	{
		appendPQExpBuffer(&buf,
						  ",\n  c2.relname as \"%s\"",
						  gettext_noop("Table"));
		cols_so_far++;
	}

	if (verbose)
	{
		/*
		 * Show whether a relation is permanent, temporary, or unlogged.  Like
		 * describeOneTableDetails(), we consider that persistence emerged in
		 * v9.1, even though related concepts existed before.
		 */
		if (pset.sversion >= 90100)
		{
			appendPQExpBuffer(&buf,
							  ",\n  CASE c.relpersistence WHEN 'p' THEN '%s' WHEN 't' THEN '%s' WHEN 'u' THEN '%s' END as \"%s\"",
							  gettext_noop("permanent"),
							  gettext_noop("temporary"),
							  gettext_noop("unlogged"),
							  gettext_noop("Persistence"));
			translate_columns[cols_so_far] = true;
		}

		/*
		 * We don't bother to count cols_so_far below here, as there's no need
		 * to; this might change with future additions to the output columns.
		 */

		/*
		 * Access methods exist for tables, materialized views and indexes.
		 * This has been introduced in PostgreSQL 12 for tables.
		 */
		if (pset.sversion >= 120000 && !pset.hide_tableam &&
			(showTables || showMatViews || showIndexes))
			appendPQExpBuffer(&buf,
							  ",\n  am.amname as \"%s\"",
							  gettext_noop("Access method"));

		/*
		 * As of PostgreSQL 9.0, use pg_table_size() to show a more accurate
		 * size of a table, including FSM, VM and TOAST tables.
		 */
		if (pset.sversion >= 90000)
			appendPQExpBuffer(&buf,
							  ",\n  pg_catalog.pg_size_pretty(pg_catalog.pg_table_size(c.oid)) as \"%s\"",
							  gettext_noop("Size"));
		else if (pset.sversion >= 80100)
			appendPQExpBuffer(&buf,
							  ",\n  pg_catalog.pg_size_pretty(pg_catalog.pg_relation_size(c.oid)) as \"%s\"",
							  gettext_noop("Size"));

		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.obj_description(c.oid, 'pg_class') as \"%s\"",
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_class c"
						 "\n     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace");

	if (pset.sversion >= 120000 && !pset.hide_tableam &&
		(showTables || showMatViews || showIndexes))
		appendPQExpBufferStr(&buf,
							 "\n     LEFT JOIN pg_catalog.pg_am am ON am.oid = c.relam");

	if (showIndexes)
		appendPQExpBufferStr(&buf,
							 "\n     LEFT JOIN pg_catalog.pg_index i ON i.indexrelid = c.oid"
							 "\n     LEFT JOIN pg_catalog.pg_class c2 ON i.indrelid = c2.oid");

	appendPQExpBufferStr(&buf, "\nWHERE c.relkind IN (");
	if (showTables)
	{
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_RELATION) ","
							 "'p'" ",");
		/* with 'S' or a pattern, allow 't' to match TOAST tables too */
		if (showSystem || pattern)
			appendPQExpBufferStr(&buf, CppAsString2(RELKIND_TOASTVALUE) ",");
	}
	if (showViews)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_VIEW) ",");
	if (showIndexes)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_INDEX) ","
							 "'I'" ",");
	if (showSystem || pattern)
		appendPQExpBufferStr(&buf, "'s',"); /* was RELKIND_SPECIAL */

	appendPQExpBufferStr(&buf, "''");	/* dummy */
	appendPQExpBufferStr(&buf, ")\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname NOT LIKE 'pg_toast%'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "c.relname", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
		return false;

	appendPQExpBufferStr(&buf, "ORDER BY 1,2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	/*
	 * Most functions in this file are content to print an empty table when
	 * there are no matching objects.  We intentionally deviate from that
	 * here, but only in !quiet mode, for historical reasons.
	 */
	if (PQntuples(res) == 0 && !pset.quiet)
	{
		if (pattern)
			pg_log_error("Did not find any relation named \"%s\".",
						 pattern);
		else
			pg_log_error("Did not find any relations.");
	}
	else
	{
		myopt.nullPrint = NULL;
		myopt.title = _("List of relations");
		myopt.translate_header = true;
		myopt.translate_columns = translate_columns;
		myopt.n_translate_columns = lengthof(translate_columns);

		printQuery(res, &myopt, pset.queryFout, false, pset.logfile);
	}

	PQclear(res);
	return true;
}

/*
 * \dL
 *
 * Describes languages.
 */
bool
listLanguages(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT l.lanname AS \"%s\"\n",
					  gettext_noop("Name"));
	appendPQExpBuffer(&buf,
					  ",\n       l.lanpltrusted AS \"%s\"",
					  gettext_noop("Trusted"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  ",\n       NOT l.lanispl AS \"%s\",\n"
						  "       l.lanplcallfoid::pg_catalog.regprocedure AS \"%s\",\n"
						  "       l.lanvalidator::pg_catalog.regprocedure AS \"%s\",\n       ",
						  gettext_noop("Internal language"),
						  gettext_noop("Call handler"),
						  gettext_noop("Validator"));
		if (pset.sversion >= 90000)
			appendPQExpBuffer(&buf, "l.laninline::pg_catalog.regprocedure AS \"%s\",\n       ",
							  gettext_noop("Inline handler"));
	}

	appendPQExpBuffer(&buf,
					  "\nFROM pg_catalog.pg_language l\n");

	if (pattern)
		if (!validateSQLNamePattern(&buf, pattern, false, false,
									NULL, "l.lanname", NULL, NULL,
									NULL, 2))
			return false;

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE l.lanplcallfoid != 0\n");


	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of languages");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * \dC
 *
 * Describes casts.
 */
bool
listCasts(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, false, true, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT pg_catalog.format_type(castsource, NULL) AS \"%s\",\n"
					  "       pg_catalog.format_type(casttarget, NULL) AS \"%s\",\n",
					  gettext_noop("Source type"),
					  gettext_noop("Target type"));

	/*
	 * We don't attempt to localize '(binary coercible)' or '(with inout)',
	 * because there's too much risk of gettext translating a function name
	 * that happens to match some string in the PO database.
	 */
	if (pset.sversion >= 80400)
		appendPQExpBuffer(&buf,
						  "       CASE WHEN c.castmethod = '%c' THEN '(binary coercible)'\n"
						  "            WHEN c.castmethod = '%c' THEN '(with inout)'\n"
						  "            ELSE p.proname\n"
						  "       END AS \"%s\",\n",
						  COERCION_METHOD_BINARY,
						  COERCION_METHOD_INOUT,
						  gettext_noop("Function"));
	else
		appendPQExpBuffer(&buf,
						  "       CASE WHEN c.castfunc = 0 THEN '(binary coercible)'\n"
						  "            ELSE p.proname\n"
						  "       END AS \"%s\",\n",
						  gettext_noop("Function"));

	appendPQExpBuffer(&buf,
					  "       CASE WHEN c.castcontext = '%c' THEN '%s'\n"
					  "            WHEN c.castcontext = '%c' THEN '%s'\n"
					  "            ELSE '%s'\n"
					  "       END AS \"%s\"",
					  COERCION_CODE_EXPLICIT,
					  gettext_noop("no"),
					  COERCION_CODE_ASSIGNMENT,
					  gettext_noop("in assignment"),
					  gettext_noop("yes"),
					  gettext_noop("Implicit?"));

	/*
	 * We need a left join to pg_proc for binary casts; the others are just
	 * paranoia.
	 */
	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_cast c LEFT JOIN pg_catalog.pg_proc p\n"
						 "     ON c.castfunc = p.oid\n"
						 "     LEFT JOIN pg_catalog.pg_type ts\n"
						 "     ON c.castsource = ts.oid\n"
						 "     LEFT JOIN pg_catalog.pg_namespace ns\n"
						 "     ON ns.oid = ts.typnamespace\n"
						 "     LEFT JOIN pg_catalog.pg_type tt\n"
						 "     ON c.casttarget = tt.oid\n"
						 "     LEFT JOIN pg_catalog.pg_namespace nt\n"
						 "     ON nt.oid = tt.typnamespace\n");

	appendPQExpBufferStr(&buf, "WHERE ( (true");

	/*
	 * Match name pattern against either internal or external name of either
	 * castsource or casttarget
	 */
	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"ns.nspname", "ts.typname",
								"pg_catalog.format_type(ts.oid, NULL)",
								"pg_catalog.pg_type_is_visible(ts.oid)",
								NULL, 3))
		return false;

	appendPQExpBufferStr(&buf, ") OR (true");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"nt.nspname", "tt.typname",
								"pg_catalog.format_type(tt.oid, NULL)",
								"pg_catalog.pg_type_is_visible(tt.oid)",
								NULL, 3))
		return false;

	appendPQExpBufferStr(&buf, ") )\nORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of casts");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dO
 *
 * Describes collations.
 */
bool
listCollations(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, false, false, false, true, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname AS \"%s\",\n"
					  "       c.collname AS \"%s\",\n"
					  "       c.collcollate AS \"%s\",\n"
					  "       c.collctype AS \"%s\"",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Collate"),
					  gettext_noop("Ctype"));

	if (pset.sversion >= 100000)
		appendPQExpBuffer(&buf,
						  ",\n       CASE c.collprovider WHEN 'd' THEN 'default' WHEN 'c' THEN 'libc' END AS \"%s\"",
						  gettext_noop("Provider"));
	else
		appendPQExpBuffer(&buf,
						  ",\n       'libc' AS \"%s\"",
						  gettext_noop("Provider"));

	if (pset.sversion >= 120000)
		appendPQExpBuffer(&buf,
						  ",\n       CASE WHEN c.collisdeterministic THEN '%s' ELSE '%s' END AS \"%s\"",
						  gettext_noop("yes"), gettext_noop("no"),
						  gettext_noop("Deterministic?"));
	else
		appendPQExpBuffer(&buf,
						  ",\n       '%s' AS \"%s\"",
						  gettext_noop("yes"),
						  gettext_noop("Deterministic?"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n       pg_catalog.obj_description(c.oid, 'pg_collation') AS \"%s\"",
						  gettext_noop("Description"));

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_collation c, pg_catalog.pg_namespace n\n"
						 "WHERE n.oid = c.collnamespace\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	/*
	 * Hide collations that aren't usable in the current database's encoding.
	 * If you think to change this, note that pg_collation_is_visible rejects
	 * unusable collations, so you will need to hack name pattern processing
	 * somehow to avoid inconsistent behavior.
	 */
	appendPQExpBufferStr(&buf, "      AND c.collencoding IN (-1, pg_catalog.pg_char_to_encoding(pg_catalog.getdatabaseencoding()))\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "c.collname", NULL,
								"pg_catalog.pg_collation_is_visible(c.oid)",
								NULL, 3))
		return false;

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of collations");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dn
 *
 * Describes schemas (namespaces)
 */
bool
listSchemas(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT n.nspname AS \"%s\"",
					  gettext_noop("Name"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.obj_description(n.oid, 'pg_namespace') AS \"%s\"",
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_namespace n\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf,
							 "WHERE n.nspname NOT LIKE 'pg\\_%' ESCAPE '\\' AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern,
								!showSystem && !pattern, false,
								NULL, "n.nspname", NULL,
								NULL,
								NULL, 2))
		return false;

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of schemas");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * \dFp
 * list text search parsers
 */
bool
listTSParsers(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	if (verbose)
		return listTSParsersVerbose(pattern);

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  n.nspname as \"%s\",\n"
					  "  p.prsname as \"%s\",\n"
					  "  pg_catalog.obj_description(p.oid, 'pg_ts_parser') as \"%s\"\n"
					  "FROM pg_catalog.pg_ts_parser p\n"
					  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.prsnamespace\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Description")
		);

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "p.prsname", NULL,
								"pg_catalog.pg_ts_parser_is_visible(p.oid)",
								NULL, 3))
		return false;

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of text search parsers");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * full description of parsers
 */
static bool
listTSParsersVerbose(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	int			i;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT p.oid,\n"
					  "  n.nspname,\n"
					  "  p.prsname\n"
					  "FROM pg_catalog.pg_ts_parser p\n"
					  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.prsnamespace\n"
		);

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "p.prsname", NULL,
								"pg_catalog.pg_ts_parser_is_visible(p.oid)",
								NULL, 3))
		return false;

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
		{
			if (pattern)
				pg_log_error("Did not find any text search parser named \"%s\".",
							 pattern);
			else
				pg_log_error("Did not find any text search parsers.");
		}
		PQclear(res);
		return false;
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *oid;
		const char *nspname = NULL;
		const char *prsname;

		oid = PQgetvalue(res, i, 0);
		if (!PQgetisnull(res, i, 1))
			nspname = PQgetvalue(res, i, 1);
		prsname = PQgetvalue(res, i, 2);

		if (!describeOneTSParser(oid, nspname, prsname))
		{
			PQclear(res);
			return false;
		}

		if (cancel_pressed)
		{
			PQclear(res);
			return false;
		}
	}

	PQclear(res);
	return true;
}

static bool
describeOneTSParser(const char *oid, const char *nspname, const char *prsname)
{
	PQExpBufferData buf;
	PGresult   *res;
	PQExpBufferData title;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {true, false, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT '%s' AS \"%s\",\n"
					  "   p.prsstart::pg_catalog.regproc AS \"%s\",\n"
					  "   pg_catalog.obj_description(p.prsstart, 'pg_proc') as \"%s\"\n"
					  " FROM pg_catalog.pg_ts_parser p\n"
					  " WHERE p.oid = '%s'\n"
					  "UNION ALL\n"
					  "SELECT '%s',\n"
					  "   p.prstoken::pg_catalog.regproc,\n"
					  "   pg_catalog.obj_description(p.prstoken, 'pg_proc')\n"
					  " FROM pg_catalog.pg_ts_parser p\n"
					  " WHERE p.oid = '%s'\n"
					  "UNION ALL\n"
					  "SELECT '%s',\n"
					  "   p.prsend::pg_catalog.regproc,\n"
					  "   pg_catalog.obj_description(p.prsend, 'pg_proc')\n"
					  " FROM pg_catalog.pg_ts_parser p\n"
					  " WHERE p.oid = '%s'\n"
					  "UNION ALL\n"
					  "SELECT '%s',\n"
					  "   p.prsheadline::pg_catalog.regproc,\n"
					  "   pg_catalog.obj_description(p.prsheadline, 'pg_proc')\n"
					  " FROM pg_catalog.pg_ts_parser p\n"
					  " WHERE p.oid = '%s'\n"
					  "UNION ALL\n"
					  "SELECT '%s',\n"
					  "   p.prslextype::pg_catalog.regproc,\n"
					  "   pg_catalog.obj_description(p.prslextype, 'pg_proc')\n"
					  " FROM pg_catalog.pg_ts_parser p\n"
					  " WHERE p.oid = '%s';",
					  gettext_noop("Start parse"),
					  gettext_noop("Method"),
					  gettext_noop("Function"),
					  gettext_noop("Description"),
					  oid,
					  gettext_noop("Get next token"),
					  oid,
					  gettext_noop("End parse"),
					  oid,
					  gettext_noop("Get headline"),
					  oid,
					  gettext_noop("Get token types"),
					  oid);

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	initPQExpBuffer(&title);
	if (nspname)
		printfPQExpBuffer(&title, _("Text search parser \"%s.%s\""),
						  nspname, prsname);
	else
		printfPQExpBuffer(&title, _("Text search parser \"%s\""), prsname);
	myopt.title = title.data;
	myopt.footers = NULL;
	myopt.topt.default_footer = false;
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT t.alias as \"%s\",\n"
					  "  t.description as \"%s\"\n"
					  "FROM pg_catalog.ts_token_type( '%s'::pg_catalog.oid ) as t\n"
					  "ORDER BY 1;",
					  gettext_noop("Token name"),
					  gettext_noop("Description"),
					  oid);

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	if (nspname)
		printfPQExpBuffer(&title, _("Token types for parser \"%s.%s\""),
						  nspname, prsname);
	else
		printfPQExpBuffer(&title, _("Token types for parser \"%s\""), prsname);
	myopt.title = title.data;
	myopt.footers = NULL;
	myopt.topt.default_footer = true;
	myopt.translate_header = true;
	myopt.translate_columns = NULL;
	myopt.n_translate_columns = 0;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&title);
	PQclear(res);
	return true;
}


/*
 * \dFd
 * list text search dictionaries
 */
bool
listTSDictionaries(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  n.nspname as \"%s\",\n"
					  "  d.dictname as \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  "  ( SELECT COALESCE(nt.nspname, '(null)')::pg_catalog.text || '.' || t.tmplname FROM\n"
						  "    pg_catalog.pg_ts_template t\n"
						  "    LEFT JOIN pg_catalog.pg_namespace nt ON nt.oid = t.tmplnamespace\n"
						  "    WHERE d.dicttemplate = t.oid ) AS  \"%s\",\n"
						  "  d.dictinitoption as \"%s\",\n",
						  gettext_noop("Template"),
						  gettext_noop("Init options"));
	}

	appendPQExpBuffer(&buf,
					  "  pg_catalog.obj_description(d.oid, 'pg_ts_dict') as \"%s\"\n",
					  gettext_noop("Description"));

	appendPQExpBufferStr(&buf, "FROM pg_catalog.pg_ts_dict d\n"
						 "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = d.dictnamespace\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "d.dictname", NULL,
								"pg_catalog.pg_ts_dict_is_visible(d.oid)",
								NULL, 3))
		return false;

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of text search dictionaries");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * \dFt
 * list text search templates
 */
bool
listTSTemplates(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	if (verbose)
		printfPQExpBuffer(&buf,
						  "SELECT\n"
						  "  n.nspname AS \"%s\",\n"
						  "  t.tmplname AS \"%s\",\n"
						  "  t.tmplinit::pg_catalog.regproc AS \"%s\",\n"
						  "  t.tmpllexize::pg_catalog.regproc AS \"%s\",\n"
						  "  pg_catalog.obj_description(t.oid, 'pg_ts_template') AS \"%s\"\n",
						  gettext_noop("Schema"),
						  gettext_noop("Name"),
						  gettext_noop("Init"),
						  gettext_noop("Lexize"),
						  gettext_noop("Description"));
	else
		printfPQExpBuffer(&buf,
						  "SELECT\n"
						  "  n.nspname AS \"%s\",\n"
						  "  t.tmplname AS \"%s\",\n"
						  "  pg_catalog.obj_description(t.oid, 'pg_ts_template') AS \"%s\"\n",
						  gettext_noop("Schema"),
						  gettext_noop("Name"),
						  gettext_noop("Description"));

	appendPQExpBufferStr(&buf, "FROM pg_catalog.pg_ts_template t\n"
						 "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = t.tmplnamespace\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "t.tmplname", NULL,
								"pg_catalog.pg_ts_template_is_visible(t.oid)",
								NULL, 3))
		return false;

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of text search templates");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * \dF
 * list text search configurations
 */
bool
listTSConfigs(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	if (verbose)
		return listTSConfigsVerbose(pattern);

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "   n.nspname as \"%s\",\n"
					  "   c.cfgname as \"%s\",\n"
					  "   pg_catalog.obj_description(c.oid, 'pg_ts_config') as \"%s\"\n"
					  "FROM pg_catalog.pg_ts_config c\n"
					  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.cfgnamespace\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Description")
		);

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "c.cfgname", NULL,
								"pg_catalog.pg_ts_config_is_visible(c.oid)",
								NULL, 3))
		return false;

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of text search configurations");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

static bool
listTSConfigsVerbose(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	int			i;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT c.oid, c.cfgname,\n"
					  "   n.nspname,\n"
					  "   p.prsname,\n"
					  "   np.nspname as pnspname\n"
					  "FROM pg_catalog.pg_ts_config c\n"
					  "   LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.cfgnamespace,\n"
					  " pg_catalog.pg_ts_parser p\n"
					  "   LEFT JOIN pg_catalog.pg_namespace np ON np.oid = p.prsnamespace\n"
					  "WHERE  p.oid = c.cfgparser\n"
		);

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "c.cfgname", NULL,
								"pg_catalog.pg_ts_config_is_visible(c.oid)",
								NULL, 3))
		return false;

	appendPQExpBufferStr(&buf, "ORDER BY 3, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
		{
			if (pattern)
				pg_log_error("Did not find any text search configuration named \"%s\".",
							 pattern);
			else
				pg_log_error("Did not find any text search configurations.");
		}
		PQclear(res);
		return false;
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *oid;
		const char *cfgname;
		const char *nspname = NULL;
		const char *prsname;
		const char *pnspname = NULL;

		oid = PQgetvalue(res, i, 0);
		cfgname = PQgetvalue(res, i, 1);
		if (!PQgetisnull(res, i, 2))
			nspname = PQgetvalue(res, i, 2);
		prsname = PQgetvalue(res, i, 3);
		if (!PQgetisnull(res, i, 4))
			pnspname = PQgetvalue(res, i, 4);

		if (!describeOneTSConfig(oid, nspname, cfgname, pnspname, prsname))
		{
			PQclear(res);
			return false;
		}

		if (cancel_pressed)
		{
			PQclear(res);
			return false;
		}
	}

	PQclear(res);
	return true;
}

static bool
describeOneTSConfig(const char *oid, const char *nspname, const char *cfgname,
					const char *pnspname, const char *prsname)
{
	PQExpBufferData buf,
				title;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  ( SELECT t.alias FROM\n"
					  "    pg_catalog.ts_token_type(c.cfgparser) AS t\n"
					  "    WHERE t.tokid = m.maptokentype ) AS \"%s\",\n"
					  "  pg_catalog.btrim(\n"
					  "    ARRAY( SELECT mm.mapdict::pg_catalog.regdictionary\n"
					  "           FROM pg_catalog.pg_ts_config_map AS mm\n"
					  "           WHERE mm.mapcfg = m.mapcfg AND mm.maptokentype = m.maptokentype\n"
					  "           ORDER BY mapcfg, maptokentype, mapseqno\n"
					  "    ) :: pg_catalog.text,\n"
					  "  '{}') AS \"%s\"\n"
					  "FROM pg_catalog.pg_ts_config AS c, pg_catalog.pg_ts_config_map AS m\n"
					  "WHERE c.oid = '%s' AND m.mapcfg = c.oid\n"
					  "GROUP BY m.mapcfg, m.maptokentype, c.cfgparser\n"
					  "ORDER BY 1;",
					  gettext_noop("Token"),
					  gettext_noop("Dictionaries"),
					  oid);

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	initPQExpBuffer(&title);

	if (nspname)
		appendPQExpBuffer(&title, _("Text search configuration \"%s.%s\""),
						  nspname, cfgname);
	else
		appendPQExpBuffer(&title, _("Text search configuration \"%s\""),
						  cfgname);

	if (pnspname)
		appendPQExpBuffer(&title, _("\nParser: \"%s.%s\""),
						  pnspname, prsname);
	else
		appendPQExpBuffer(&title, _("\nParser: \"%s\""),
						  prsname);

	myopt.nullPrint = NULL;
	myopt.title = title.data;
	myopt.footers = NULL;
	myopt.topt.default_footer = false;
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&title);

	PQclear(res);
	return true;
}



/*
 * \dx
 *
 * Briefly describes installed extensions.
 */
bool
listExtensions(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;


	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT e.extname AS \"%s\", "
					  "e.extversion AS \"%s\", n.nspname AS \"%s\"\n"
					  "FROM pg_catalog.pg_extension e "
					  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = e.extnamespace\n",
					  gettext_noop("Name"),
					  gettext_noop("Version"),
					  gettext_noop("Schema"));

	if (!validateSQLNamePattern(&buf, pattern,
								false, false,
								NULL, "e.extname", NULL,
								NULL,
								NULL, 1))
		return false;

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of installed extensions");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dx+
 *
 * List contents of installed extensions.
 */
bool
listExtensionContents(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	int			i;


	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT e.extname, e.oid\n"
					  "FROM pg_catalog.pg_extension e\n");

	if (!validateSQLNamePattern(&buf, pattern,
								false, false,
								NULL, "e.extname", NULL,
								NULL,
								NULL, 1))
		return false;

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
		{
			if (pattern)
				pg_log_error("Did not find any extension named \"%s\".",
							 pattern);
			else
				pg_log_error("Did not find any extensions.");
		}
		PQclear(res);
		return false;
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *extname;
		const char *oid;

		extname = PQgetvalue(res, i, 0);
		oid = PQgetvalue(res, i, 1);

		if (!listOneExtensionContents(extname, oid))
		{
			PQclear(res);
			return false;
		}
		if (cancel_pressed)
		{
			PQclear(res);
			return false;
		}
	}

	PQclear(res);
	return true;
}

static bool
listOneExtensionContents(const char *extname, const char *oid)
{
	PQExpBufferData buf;
	PGresult   *res;
	PQExpBufferData title;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT pg_catalog.pg_describe_object(classid, objid, 0) AS \"%s\"\n"
					  "FROM pg_catalog.pg_depend\n"
					  "WHERE refclassid = 'pg_catalog.pg_extension'::pg_catalog.regclass AND refobjid = '%s' AND deptype = 'e'\n"
					  "ORDER BY 1;",
					  gettext_noop("Object description"),
					  oid);

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	initPQExpBuffer(&title);
	printfPQExpBuffer(&title, _("Objects in extension \"%s\""), extname);
	myopt.title = title.data;
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&title);
	PQclear(res);
	return true;
}

/*
 * validateSQLNamePattern
 *
 * Wrapper around string_utils's processSQLNamePattern which also checks the
 * pattern's validity.  In addition to that function's parameters, takes a
 * 'maxparts' parameter specifying the maximum number of dotted names the
 * pattern is allowed to have, and a 'added_clause' parameter that returns by
 * reference whether a clause was added to 'buf'.  Returns whether the pattern
 * passed validation, after logging any errors.
 */
static bool
validateSQLNamePattern(PQExpBuffer buf, const char *pattern, bool have_where,
					   bool force_escape, const char *schemavar,
					   const char *namevar, const char *altnamevar,
					   const char *visibilityrule, bool *added_clause,
					   int maxparts)
{
	PQExpBufferData	dbbuf;
	int			dotcnt;
	bool		added;

	initPQExpBuffer(&dbbuf);
	added = processSQLNamePattern(pset.db, buf, pattern, have_where, force_escape,
								  schemavar, namevar, altnamevar,
								  visibilityrule, &dbbuf, &dotcnt);
	if (added_clause != NULL)
		*added_clause = added;

	if (dotcnt >= maxparts)
	{
		pg_log_error("improper qualified name (too many dotted names): %s",
					 pattern);
		termPQExpBuffer(&dbbuf);
		return false;
	}

	if (maxparts > 1 && dotcnt == maxparts-1)
	{
		if (PQdb(pset.db) == NULL)
		{
			pg_log_error("You are currently not connected to a database.");
			return false;
		}
		if (strcmp(PQdb(pset.db), dbbuf.data) != 0)
		{
			pg_log_error("cross-database references are not implemented: %s",
						 pattern);
			return false;
		}
	}
	return true;
}

/*
 * \dAc
 * Lists operator classes
 *
 * Takes optional regexps to filter by index access method and input data type.
 */
bool
listOperatorClasses(const char *access_method_pattern,
					const char *type_pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	bool		have_where = false;
	static const bool translate_columns[] = {false, false, false, false, false, false, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  am.amname AS \"%s\",\n"
					  "  pg_catalog.format_type(c.opcintype, NULL) AS \"%s\",\n"
					  "  CASE\n"
					  "    WHEN c.opckeytype <> 0 AND c.opckeytype <> c.opcintype\n"
					  "    THEN pg_catalog.format_type(c.opckeytype, NULL)\n"
					  "    ELSE NULL\n"
					  "  END AS \"%s\",\n"
					  "  CASE\n"
					  "    WHEN pg_catalog.pg_opclass_is_visible(c.oid)\n"
					  "    THEN pg_catalog.format('%%I', c.opcname)\n"
					  "    ELSE pg_catalog.format('%%I.%%I', n.nspname, c.opcname)\n"
					  "  END AS \"%s\",\n"
					  "  (CASE WHEN c.opcdefault\n"
					  "    THEN '%s'\n"
					  "    ELSE '%s'\n"
					  "  END) AS \"%s\"",
					  gettext_noop("AM"),
					  gettext_noop("Input type"),
					  gettext_noop("Storage type"),
					  gettext_noop("Operator class"),
					  gettext_noop("yes"),
					  gettext_noop("no"),
					  gettext_noop("Default?"));
	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n  CASE\n"
						  "    WHEN pg_catalog.pg_opfamily_is_visible(of.oid)\n"
						  "    THEN pg_catalog.format('%%I', of.opfname)\n"
						  "    ELSE pg_catalog.format('%%I.%%I', ofn.nspname, of.opfname)\n"
						  "  END AS \"%s\"\n",
						  gettext_noop("Operator family"));
	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_opclass c\n"
						 "  LEFT JOIN pg_catalog.pg_am am on am.oid = c.opcmethod\n"
						 "  LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.opcnamespace\n"
						 "  LEFT JOIN pg_catalog.pg_type t ON t.oid = c.opcintype\n"
						 "  LEFT JOIN pg_catalog.pg_namespace tn ON tn.oid = t.typnamespace\n");
	if (verbose)
		appendPQExpBufferStr(&buf,
							 "  LEFT JOIN pg_catalog.pg_opfamily of ON of.oid = c.opcfamily\n"
							 "  LEFT JOIN pg_catalog.pg_namespace ofn ON ofn.oid = of.opfnamespace\n");

	if (access_method_pattern)
		if (!validateSQLNamePattern(&buf, access_method_pattern,
									false, false, NULL, "am.amname", NULL, NULL,
									&have_where, 1))
			return false;
	if (type_pattern)
	{
		/* Match type name pattern against either internal or external name */
		if (!validateSQLNamePattern(&buf, type_pattern, have_where, false,
									"tn.nspname", "t.typname",
									"pg_catalog.format_type(t.oid, NULL)",
									"pg_catalog.pg_type_is_visible(t.oid)",
									NULL, 3))
			return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 4;");
	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of operator classes");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

