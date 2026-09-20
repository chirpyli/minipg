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

static bool describeOneTableDetails(const char *schemaname,
									const char *relationname,
									const char *oid,
									bool verbose);
static void add_tablespace_footer(printTableContent *const cont, char relkind,
								  Oid tablespace, const bool newline);
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
				isindexkey_col = -1,
				indexdef_col = -1,
				attstorage_col = -1,
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
		Oid			tablespace;
		char	   *reloptions;
		char		relpersistence;
		char	   *relam;
	}			tableinfo;

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
						  "SELECT c.relkind, c.relhasindex, c.relhasrules, "
						  "false AS relrowsecurity, false AS relforcerowsecurity, "
						  "false AS relhasoids, %s, c.reltablespace, "
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
						  "SELECT c.relkind, c.relhasindex, c.relhasrules, "
						  "false AS relrowsecurity, false AS relforcerowsecurity, "
						  "false AS relhasoids, %s, c.reltablespace, "
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
						  "SELECT c.relkind, c.relhasindex, c.relhasrules, "
						  "false AS relrowsecurity, false AS relforcerowsecurity, "
						  "false AS relhasoids, %s, c.reltablespace, "
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
						  "SELECT c.relkind, c.relhasindex, c.relhasrules, "
						  ""
						  "%s, c.reltablespace, "
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
						  "SELECT c.relkind, c.relhasindex, c.relhasrules, "
						  ""
						  "%s, c.reltablespace, "
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
						  "SELECT c.relkind, c.relhasindex, c.relhasrules, "
						  ""
						  "%s, c.reltablespace, "
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
						  "SELECT c.relkind, c.relhasindex, c.relhasrules, "
						  ""
						  "%s, c.reltablespace\n"
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
						  "SELECT relkind, relhasindex, relhasrules, "
						  "reltriggers <> 0, false, false, relhasoids, "
						  "%s, reltablespace\n"
						  "FROM pg_catalog.pg_class WHERE oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(reloptions, E', ')" : "''"),
						  oid);
	}
	else if (pset.sversion >= 80000)
	{
		printfPQExpBuffer(&buf,
						  "SELECT relkind, relhasindex, relhasrules, "
						  "reltriggers <> 0, false, false, relhasoids, "
						  "'', reltablespace\n"
						  "FROM pg_catalog.pg_class WHERE oid = '%s';",
						  oid);
	}
	else
	{
		printfPQExpBuffer(&buf,
						  "SELECT relkind, relhasindex, relhasrules, "
						  "reltriggers <> 0, false, false, relhasoids, "
						  "'', ''\n"
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

	tableinfo.relkind = *(PQgetvalue(res, 0, 0));
	tableinfo.hasindex = strcmp(PQgetvalue(res, 0, 1), "t") == 0;
	tableinfo.hasrules = strcmp(PQgetvalue(res, 0, 2), "t") == 0;
	tableinfo.hasoids = strcmp(PQgetvalue(res, 0, 5), "t") == 0;
	tableinfo.reloptions = (pset.sversion >= 80200) ?
		pg_strdup(PQgetvalue(res, 0, 6)) : NULL;
	tableinfo.tablespace = (pset.sversion >= 80000) ?
		atooid(PQgetvalue(res, 0, 7)) : 0;
	tableinfo.relpersistence = (pset.sversion >= 90100) ?
		*(PQgetvalue(res, 0, 8)) : 0;
	if (pset.sversion >= 120000)
		tableinfo.relam = PQgetisnull(res, 0, 9) ?
			(char *) NULL : pg_strdup(PQgetvalue(res, 0, 9));
	else
		tableinfo.relam = NULL;
	PQclear(res);
	res = NULL;

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

	if (tableinfo.relkind == RELKIND_INDEX)
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

		/* stats target, if relevant to relkind */
		if (tableinfo.relkind == RELKIND_RELATION ||
			tableinfo.relkind == RELKIND_INDEX)
		{
		appendPQExpBufferStr(&buf, ",\n  CASE WHEN a.attstattarget=-1 THEN NULL ELSE a.attstattarget END AS attstattarget");
			attstattarget_col = cols++;
		}

		/*
		 * minipg: COMMENTS 功能已裁剪，列注释查询（col_description）不再可用。
		 */
	if (tableinfo.relkind == RELKIND_RELATION ||
		tableinfo.relkind == RELKIND_VIEW ||
		tableinfo.relkind == RELKIND_COMPOSITE_TYPE)
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
			printfPQExpBuffer(&title, _("Table \"%s.%s\""),
							  schemaname, relationname);
			break;
		case RELKIND_VIEW:
			printfPQExpBuffer(&title, _("View \"%s.%s\""),
							  schemaname, relationname);
			break;
		case RELKIND_INDEX:
			printfPQExpBuffer(&title, _("Index \"%s.%s\""),
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
	if (isindexkey_col >= 0)
		headers[cols++] = gettext_noop("Key?");
	if (indexdef_col >= 0)
		headers[cols++] = gettext_noop("Definition");
	if (attstorage_col >= 0)
		headers[cols++] = gettext_noop("Storage");
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

	if (tableinfo.relkind == RELKIND_INDEX)
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

			if (strcmp(indisvalid, "t") != 0)
				appendPQExpBufferStr(&tmpbuf, _(", invalid"));

			printTableAddFooter(&cont, tmpbuf.data);

			add_tablespace_footer(&cont, tableinfo.relkind,
								  tableinfo.tablespace, true);
		}

		PQclear(result);
	}
	/* If you add relkinds here, see also "Finish printing..." stanza below */
	else if (tableinfo.relkind == RELKIND_RELATION ||
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
		/* Footer information about a view */
		printTableAddFooter(&cont, _("View definition:"));
		printTableAddFooter(&cont, view_def);
	}


	/*
	 * Finish printing the footer information about a table.
	 */
	if (tableinfo.relkind == RELKIND_RELATION ||
		tableinfo.relkind == RELKIND_TOASTVALUE)
	{
		PGresult   *result;
		int			tuples;

		/* print tables inherited from */
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
		 * print child tables
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

		if (!verbose)
		{
			/* print the number of child tables, if any */
			if (tuples > 0)
			{
				printfPQExpBuffer(&buf, _("Number of child tables: %d (Use \\d+ to list them.)"), tuples);
				printTableAddFooter(&cont, buf.data);
			}
		}
		else
		{
			/* display the list of child tables */
			const char *ct = _("Child tables");
			int			ctw = pg_wcswidth(ct, strlen(ct), pset.encoding);

			for (i = 0; i < tuples; i++)
			{
				if (i == 0)
					printfPQExpBuffer(&buf, "%s: %s",
									  ct, PQgetvalue(result, i, 0));
				else
					printfPQExpBuffer(&buf, "%*s  %s",
									  ctw, "", PQgetvalue(result, i, 0));
				if (!PQgetisnull(result, i, 3))
					appendPQExpBuffer(&buf, " %s", PQgetvalue(result, i, 3));
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
					  " END as \"%s\"",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("table"),
					  gettext_noop("view"),
					  gettext_noop("index"),
					  gettext_noop("special"),
					  gettext_noop("TOAST table"),
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
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_RELATION) ",");
		/* with 'S' or a pattern, allow 't' to match TOAST tables too */
		if (showSystem || pattern)
			appendPQExpBufferStr(&buf, CppAsString2(RELKIND_TOASTVALUE) ",");
	}
	if (showViews)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_VIEW) ",");
	if (showIndexes)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_INDEX) ",");
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

