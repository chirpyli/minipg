/*-------------------------------------------------------------------------
 *
 * functioncmds.c
 *
 *	  Routines for CREATE and DROP FUNCTION commands and CREATE and DROP
 *	  CAST commands.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/functioncmds.c
 *
 * DESCRIPTION
 *	  These routines take the parse tree and pick out the
 *	  appropriate arguments/flags, and pass the results to the
 *	  corresponding "FooDefine" routines (in src/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 * NOTES
 *	  These things must be defined and committed in the following order:
 *		"create function":
 *				input/output, recv/send procedures
 *		"create type":
 *				type
 *		"create operator":
 *				operators
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/proclang.h"
#include "executor/execdesc.h"
#include "executor/executor.h"
#include "executor/functions.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "pgstat.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/*
 *	 Examine the RETURNS clause of the CREATE FUNCTION statement
 *	 and return information about it as *prorettype_p and *returnsSet.
 *
 * This is more complex than the average typename lookup because we want to
 * allow a shell type to be used, or even created if the specified return type
 * doesn't exist yet.  (Without this, there's no way to define the I/O procs
 * for a new type.)  But SQL function creation won't cope, so error out if
 * the target language is SQL.  (We do this here, not in the SQL-function
 * validator, so as not to produce a NOTICE and then an ERROR for the same
 * condition.)
 */
static void
compute_return_type(TypeName *returnType, Oid languageOid,
					Oid *prorettype_p, bool *returnsSet_p)
{
	Oid			rettype;
	Type		typtup;

	typtup = LookupTypeName(NULL, returnType, NULL, false);

	if (typtup)
	{
		if (!((Form_pg_type) GETSTRUCT(typtup))->typisdefined)
		{
			if (languageOid == SQLlanguageId)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("SQL function cannot return shell type %s",
								TypeNameToString(returnType))));
			else
				ereport(NOTICE,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("return type %s is only a shell",
								TypeNameToString(returnType))));
		}
		rettype = typeTypeId(typtup);
		ReleaseSysCache(typtup);
	}
	else
	{
		char	   *typnam = TypeNameToString(returnType);
		Oid			namespaceId;
		char	   *typname;
		ObjectAddress address;

		/*
		 * Only C-coded functions can be I/O functions.  We enforce this
		 * restriction here mainly to prevent littering the catalogs with
		 * shell types due to simple typos in user-defined function
		 * definitions.
		 */
		if (languageOid != INTERNALlanguageId &&
			languageOid != ClanguageId)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type \"%s\" does not exist", typnam)));

		/* Reject if there's typmod decoration, too */
		if (returnType->typmods != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type modifier cannot be specified for shell type \"%s\"",
							typnam)));

		/* Otherwise, go ahead and make a shell type */
		ereport(NOTICE,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" is not yet defined", typnam),
				 errdetail("Creating a shell type definition.")));
		namespaceId = QualifiedNameGetCreationNamespace(returnType->names,
														&typname);
		address = TypeShellMake(typname, namespaceId, GetUserId());
		rettype = address.objectId;
		Assert(OidIsValid(rettype));
	}

	*prorettype_p = rettype;
	*returnsSet_p = returnType->setof;
}

/*
 * Interpret the function parameter list of a CREATE FUNCTION,
 * CREATE PROCEDURE, or CREATE AGGREGATE statement.
 *
 * Input parameters:
 * parameters: list of FunctionParameter structs
 * languageOid: OID of function language (InvalidOid if it's CREATE AGGREGATE)
 * objtype: identifies type of object being created
 *
 * Results are stored into output parameters.  parameterTypes must always
 * be created, but the other arrays/lists can be NULL pointers if not needed.
 * variadicArgType is set to the variadic array type if there's a VARIADIC
 * parameter (there can be only one); or to InvalidOid if not.
 * requiredResultType is set to InvalidOid if there are no OUT parameters,
 * else it is set to the OID of the implied result type.
 */
void
interpret_function_parameter_list(ParseState *pstate,
								  List *parameters,
								  Oid languageOid,
								  ObjectType objtype,
								  oidvector **parameterTypes,
								  List **parameterTypes_list,
								  ArrayType **allParameterTypes,
								  ArrayType **parameterModes,
								  ArrayType **parameterNames,
								  List **inParameterNames_list,
								  List **parameterDefaults,
								  Oid *variadicArgType,
								  Oid *requiredResultType)
{
	int			parameterCount = list_length(parameters);
	Oid		   *inTypes;
	int			inCount = 0;
	Datum	   *allTypes;
	Datum	   *paramModes;
	Datum	   *paramNames;
	int			outCount = 0;
	int			varCount = 0;
	bool		have_names = false;
	bool		have_defaults = false;
	ListCell   *x;
	int			i;

	*variadicArgType = InvalidOid;	/* default result */
	*requiredResultType = InvalidOid;	/* default result */

	inTypes = (Oid *) palloc(parameterCount * sizeof(Oid));
	allTypes = (Datum *) palloc(parameterCount * sizeof(Datum));
	paramModes = (Datum *) palloc(parameterCount * sizeof(Datum));
	paramNames = (Datum *) palloc0(parameterCount * sizeof(Datum));
	*parameterDefaults = NIL;

	/* Scan the list and extract data into work arrays */
	i = 0;
	foreach(x, parameters)
	{
		FunctionParameter *fp = (FunctionParameter *) lfirst(x);
		TypeName   *t = fp->argType;
		FunctionParameterMode fpmode = fp->mode;
		bool		isinput = false;
		Oid			toid;
		Type		typtup;

		/* For our purposes here, a defaulted mode spec is identical to IN */
		if (fpmode == FUNC_PARAM_DEFAULT)
			fpmode = FUNC_PARAM_IN;

		typtup = LookupTypeName(NULL, t, NULL, false);
		if (typtup)
		{
			if (!((Form_pg_type) GETSTRUCT(typtup))->typisdefined)
			{
				/* As above, hard error if language is SQL */
				if (languageOid == SQLlanguageId)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("SQL function cannot accept shell type %s",
									TypeNameToString(t))));
				else
					ereport(NOTICE,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("argument type %s is only a shell",
									TypeNameToString(t))));
			}
			toid = typeTypeId(typtup);
			ReleaseSysCache(typtup);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type %s does not exist",
							TypeNameToString(t))));
			toid = InvalidOid;	/* keep compiler quiet */
		}

		if (t->setof)
		{
			if (objtype == OBJECT_PROCEDURE)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("procedures cannot accept set arguments")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("functions cannot accept set arguments")));
		}

		/* handle input parameters */
		if (fpmode != FUNC_PARAM_OUT && fpmode != FUNC_PARAM_TABLE)
		{
			/* other input parameters can't follow a VARIADIC parameter */
			if (varCount > 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("VARIADIC parameter must be the last input parameter")));
			inTypes[inCount++] = toid;
			isinput = true;
			if (parameterTypes_list)
				*parameterTypes_list = lappend_oid(*parameterTypes_list, toid);
		}

		/* handle output parameters */
		if (fpmode != FUNC_PARAM_IN && fpmode != FUNC_PARAM_VARIADIC)
		{
			if (objtype == OBJECT_PROCEDURE)
			{
				/*
				 * We disallow OUT-after-VARIADIC only for procedures.  While
				 * such a case causes no confusion in ordinary function calls,
				 * it would cause confusion in a CALL statement.
				 */
				if (varCount > 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("VARIADIC parameter must be the last parameter")));
				/* Procedures with output parameters always return RECORD */
				*requiredResultType = RECORDOID;
			}
			else if (outCount == 0) /* save first output param's type */
				*requiredResultType = toid;
			outCount++;
		}

		if (fpmode == FUNC_PARAM_VARIADIC)
		{
			*variadicArgType = toid;
			varCount++;
			/* validate variadic parameter type */
			switch (toid)
			{
				case ANYARRAYOID:
				case ANYCOMPATIBLEARRAYOID:
				case ANYOID:
					/* okay */
					break;
				default:
					if (!OidIsValid(get_element_type(toid)))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
								 errmsg("VARIADIC parameter must be an array")));
					break;
			}
		}

		allTypes[i] = ObjectIdGetDatum(toid);

		paramModes[i] = CharGetDatum(fpmode);

		if (fp->name && fp->name[0])
		{
			ListCell   *px;

			/*
			 * As of Postgres 9.0 we disallow using the same name for two
			 * input or two output function parameters.  Depending on the
			 * function's language, conflicting input and output names might
			 * be bad too, but we leave it to the PL to complain if so.
			 */
			foreach(px, parameters)
			{
				FunctionParameter *prevfp = (FunctionParameter *) lfirst(px);
				FunctionParameterMode prevfpmode;

				if (prevfp == fp)
					break;
				/* as above, default mode is IN */
				prevfpmode = prevfp->mode;
				if (prevfpmode == FUNC_PARAM_DEFAULT)
					prevfpmode = FUNC_PARAM_IN;
				/* pure in doesn't conflict with pure out */
				if ((fpmode == FUNC_PARAM_IN ||
					 fpmode == FUNC_PARAM_VARIADIC) &&
					(prevfpmode == FUNC_PARAM_OUT ||
					 prevfpmode == FUNC_PARAM_TABLE))
					continue;
				if ((prevfpmode == FUNC_PARAM_IN ||
					 prevfpmode == FUNC_PARAM_VARIADIC) &&
					(fpmode == FUNC_PARAM_OUT ||
					 fpmode == FUNC_PARAM_TABLE))
					continue;
				if (prevfp->name && prevfp->name[0] &&
					strcmp(prevfp->name, fp->name) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("parameter name \"%s\" used more than once",
									fp->name)));
			}

			paramNames[i] = CStringGetTextDatum(fp->name);
			have_names = true;
		}

		if (inParameterNames_list)
			*inParameterNames_list = lappend(*inParameterNames_list, makeString(fp->name ? fp->name : pstrdup("")));

		if (fp->defexpr)
		{
			Node	   *def;

			if (!isinput)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("only input parameters can have default values")));

			def = transformExpr(pstate, fp->defexpr,
								EXPR_KIND_FUNCTION_DEFAULT);
			def = coerce_to_specific_type(pstate, def, toid, "DEFAULT");
			assign_expr_collations(pstate, def);

			/*
			 * Make sure no variables are referred to (this is probably dead
			 * code now that add_missing_from is history).
			 */
			if (list_length(pstate->p_rtable) != 0 ||
				contain_var_clause(def))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("cannot use table references in parameter default value")));

			/*
			 * transformExpr() should have already rejected subqueries,
			 * aggregates, and window functions, based on the EXPR_KIND_ for a
			 * default expression.
			 *
			 * It can't return a set either --- but coerce_to_specific_type
			 * already checked that for us.
			 *
			 * Note: the point of these restrictions is to ensure that an
			 * expression that, on its face, hasn't got subplans, aggregates,
			 * etc cannot suddenly have them after function default arguments
			 * are inserted.
			 */

			*parameterDefaults = lappend(*parameterDefaults, def);
			have_defaults = true;
		}
		else
		{
			if (isinput && have_defaults)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("input parameters after one with a default value must also have defaults")));

			/*
			 * For procedures, we also can't allow OUT parameters after one
			 * with a default, because the same sort of confusion arises in a
			 * CALL statement.
			 */
			if (objtype == OBJECT_PROCEDURE && have_defaults)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("procedure OUT parameters cannot appear after one with a default value")));
		}

		i++;
	}

	/* Now construct the proper outputs as needed */
	*parameterTypes = buildoidvector(inTypes, inCount);

	if (outCount > 0 || varCount > 0)
	{
		*allParameterTypes = construct_array(allTypes, parameterCount, OIDOID,
											 sizeof(Oid), true, TYPALIGN_INT);
		*parameterModes = construct_array(paramModes, parameterCount, CHAROID,
										  1, true, TYPALIGN_CHAR);
		if (outCount > 1)
			*requiredResultType = RECORDOID;
		/* otherwise we set requiredResultType correctly above */
	}
	else
	{
		*allParameterTypes = NULL;
		*parameterModes = NULL;
	}

	if (have_names)
	{
		for (i = 0; i < parameterCount; i++)
		{
			if (paramNames[i] == PointerGetDatum(NULL))
				paramNames[i] = CStringGetTextDatum("");
		}
		*parameterNames = construct_array(paramNames, parameterCount, TEXTOID,
										  -1, false, TYPALIGN_INT);
	}
	else
		*parameterNames = NULL;
}


/*
 * Recognize one of the options that can be passed to both CREATE
 * FUNCTION and ALTER FUNCTION and return it via one of the out
 * parameters. Returns true if the passed option was recognized. If
 * the out parameter we were going to assign to points to non-NULL,
 * raise a duplicate-clause error.  (We don't try to detect duplicate
 * SET parameters though --- if you're redundant, the last one wins.)
 */
static bool
compute_common_attribute(ParseState *pstate,
						 bool is_procedure,
						 DefElem *defel,
						 DefElem **volatility_item,
						 DefElem **strict_item,
						 DefElem **security_item,
						 DefElem **leakproof_item,
						 List **set_items,
						 DefElem **cost_item,
						 DefElem **rows_item,
						 DefElem **support_item,
						 DefElem **parallel_item)
{
	if (strcmp(defel->defname, "volatility") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*volatility_item)
			goto duplicate_error;

		*volatility_item = defel;
	}
	else if (strcmp(defel->defname, "strict") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*strict_item)
			goto duplicate_error;

		*strict_item = defel;
	}
	else if (strcmp(defel->defname, "security") == 0)
	{
		if (*security_item)
			goto duplicate_error;

		*security_item = defel;
	}
	else if (strcmp(defel->defname, "leakproof") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*leakproof_item)
			goto duplicate_error;

		*leakproof_item = defel;
	}
	else if (strcmp(defel->defname, "set") == 0)
	{
		*set_items = lappend(*set_items, defel->arg);
	}
	else if (strcmp(defel->defname, "cost") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*cost_item)
			goto duplicate_error;

		*cost_item = defel;
	}
	else if (strcmp(defel->defname, "rows") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*rows_item)
			goto duplicate_error;

		*rows_item = defel;
	}
	else if (strcmp(defel->defname, "support") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*support_item)
			goto duplicate_error;

		*support_item = defel;
	}
	else if (strcmp(defel->defname, "parallel") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*parallel_item)
			goto duplicate_error;

		*parallel_item = defel;
	}
	else
		return false;

	/* Recognized an option */
	return true;

duplicate_error:
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("conflicting or redundant options"),
			 parser_errposition(pstate, defel->location)));
	return false;				/* keep compiler quiet */

procedure_error:
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
			 errmsg("invalid attribute in procedure definition"),
			 parser_errposition(pstate, defel->location)));
	return false;
}

static char
interpret_func_volatility(DefElem *defel)
{
	char	   *str = strVal(defel->arg);

	if (strcmp(str, "immutable") == 0)
		return PROVOLATILE_IMMUTABLE;
	else if (strcmp(str, "stable") == 0)
		return PROVOLATILE_STABLE;
	else if (strcmp(str, "volatile") == 0)
		return PROVOLATILE_VOLATILE;
	else
	{
		elog(ERROR, "invalid volatility \"%s\"", str);
		return 0;				/* keep compiler quiet */
	}
}

static char
interpret_func_parallel(DefElem *defel)
{
	char	   *str = strVal(defel->arg);

	if (strcmp(str, "safe") == 0)
		return PROPARALLEL_SAFE;
	else if (strcmp(str, "unsafe") == 0)
		return PROPARALLEL_UNSAFE;
	else if (strcmp(str, "restricted") == 0)
		return PROPARALLEL_RESTRICTED;
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("parameter \"parallel\" must be SAFE, RESTRICTED, or UNSAFE")));
		return PROPARALLEL_UNSAFE;	/* keep compiler quiet */
	}
}

/*
 * Update a proconfig value according to a list of VariableSetStmt items.
 *
 * The input and result may be NULL to signify a null entry.
 */
static ArrayType *
update_proconfig_value(ArrayType *a, List *set_items)
{
	ListCell   *l;

	foreach(l, set_items)
	{
		VariableSetStmt *sstmt = lfirst_node(VariableSetStmt, l);

		if (sstmt->kind == VAR_RESET_ALL)
			a = NULL;
		else
		{
			char	   *valuestr = ExtractSetVariableArgs(sstmt);

			if (valuestr)
				a = GUCArrayAdd(a, sstmt->name, valuestr);
			else				/* RESET */
				a = GUCArrayDelete(a, sstmt->name);
		}
	}

	return a;
}

static Oid
interpret_func_support(DefElem *defel)
{
	List	   *procName = defGetQualifiedName(defel);
	Oid			procOid;
	Oid			argList[1];

	/*
	 * Support functions always take one INTERNAL argument and return
	 * INTERNAL.
	 */
	argList[0] = INTERNALOID;

	procOid = LookupFuncName(procName, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procName, 1, NIL, argList))));

	if (get_func_rettype(procOid) != INTERNALOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("support function %s must return type %s",
						NameListToString(procName), "internal")));

	/*
	 * Someday we might want an ACL check here; but for now, we insist that
	 * you be superuser to specify a support function, so privilege on the
	 * support function is moot.
	 */

	return procOid;
}


/*
 * Dissect the list of options assembled in gram.y into function
 * attributes.
 */
static void
compute_function_attributes(ParseState *pstate,
							bool is_procedure,
							List *options,
							List **as,
							char **language,
							bool *windowfunc_p,
							char *volatility_p,
							bool *strict_p,
							bool *security_definer,
							bool *leakproof_p,
							ArrayType **proconfig,
							float4 *procost,
							float4 *prorows,
							Oid *prosupport,
							char *parallel_p)
{
	ListCell   *option;
	DefElem    *as_item = NULL;
	DefElem    *language_item = NULL;
	DefElem    *windowfunc_item = NULL;
	DefElem    *volatility_item = NULL;
	DefElem    *strict_item = NULL;
	DefElem    *security_item = NULL;
	DefElem    *leakproof_item = NULL;
	List	   *set_items = NIL;
	DefElem    *cost_item = NULL;
	DefElem    *rows_item = NULL;
	DefElem    *support_item = NULL;
	DefElem    *parallel_item = NULL;

	foreach(option, options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "as") == 0)
		{
			if (as_item)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
						 parser_errposition(pstate, defel->location)));
			as_item = defel;
		}
		else if (strcmp(defel->defname, "language") == 0)
		{
			if (language_item)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
						 parser_errposition(pstate, defel->location)));
			language_item = defel;
		}
		else if (strcmp(defel->defname, "window") == 0)
		{
			if (windowfunc_item)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
						 parser_errposition(pstate, defel->location)));
			if (is_procedure)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("invalid attribute in procedure definition"),
						 parser_errposition(pstate, defel->location)));
			windowfunc_item = defel;
		}
		else if (compute_common_attribute(pstate,
										  is_procedure,
										  defel,
										  &volatility_item,
										  &strict_item,
										  &security_item,
										  &leakproof_item,
										  &set_items,
										  &cost_item,
										  &rows_item,
										  &support_item,
										  &parallel_item))
		{
			/* recognized common option */
			continue;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	if (as_item)
		*as = (List *) as_item->arg;
	if (language_item)
		*language = strVal(language_item->arg);
	if (windowfunc_item)
		*windowfunc_p = intVal(windowfunc_item->arg);
	if (volatility_item)
		*volatility_p = interpret_func_volatility(volatility_item);
	if (strict_item)
		*strict_p = intVal(strict_item->arg);
	if (security_item)
		*security_definer = intVal(security_item->arg);
	if (leakproof_item)
		*leakproof_p = intVal(leakproof_item->arg);
	if (set_items)
		*proconfig = update_proconfig_value(NULL, set_items);
	if (cost_item)
	{
		*procost = defGetNumeric(cost_item);
		if (*procost <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("COST must be positive")));
	}
	if (rows_item)
	{
		*prorows = defGetNumeric(rows_item);
		if (*prorows <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ROWS must be positive")));
	}
	if (support_item)
		*prosupport = interpret_func_support(support_item);
	if (parallel_item)
		*parallel_p = interpret_func_parallel(parallel_item);
}


/*
 * For a dynamically linked C language object, the form of the clause is
 *
 *	   AS <object file name> [, <link symbol name> ]
 *
 * In all other cases
 *
 *	   AS <object reference, or sql code>
 */
static void
interpret_AS_clause(Oid languageOid, const char *languageName,
					char *funcname, List *as, Node *sql_body_in,
					List *parameterTypes, List *inParameterNames,
					char **prosrc_str_p, char **probin_str_p,
					Node **sql_body_out,
					const char *queryString)
{
	if (!sql_body_in && !as)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("no function body specified")));

	if (sql_body_in && as)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("duplicate function body specified")));

	if (sql_body_in && languageOid != SQLlanguageId)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("inline SQL function body only valid for language SQL")));

	*sql_body_out = NULL;

	if (languageOid == ClanguageId)
	{
		/*
		 * For "C" language, store the file name in probin and, when given,
		 * the link symbol name in prosrc.  If link symbol is omitted,
		 * substitute procedure name.  We also allow link symbol to be
		 * specified as "-", since that was the habit in PG versions before
		 * 8.4, and there might be dump files out there that don't translate
		 * that back to "omitted".
		 */
		*probin_str_p = strVal(linitial(as));
		if (list_length(as) == 1)
			*prosrc_str_p = funcname;
		else
		{
			*prosrc_str_p = strVal(lsecond(as));
			if (strcmp(*prosrc_str_p, "-") == 0)
				*prosrc_str_p = funcname;
		}
	}
	else if (sql_body_in)
	{
		SQLFunctionParseInfoPtr pinfo;

		pinfo = (SQLFunctionParseInfoPtr) palloc0(sizeof(SQLFunctionParseInfo));

		pinfo->fname = funcname;
		pinfo->nargs = list_length(parameterTypes);
		pinfo->argtypes = (Oid *) palloc(pinfo->nargs * sizeof(Oid));
		pinfo->argnames = (char **) palloc(pinfo->nargs * sizeof(char *));
		for (int i = 0; i < list_length(parameterTypes); i++)
		{
			char	   *s = strVal(list_nth(inParameterNames, i));

			pinfo->argtypes[i] = list_nth_oid(parameterTypes, i);
			if (IsPolymorphicType(pinfo->argtypes[i]))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("SQL function with unquoted function body cannot have polymorphic arguments")));

			if (s[0] != '\0')
				pinfo->argnames[i] = s;
			else
				pinfo->argnames[i] = NULL;
		}

		if (IsA(sql_body_in, List))
		{
			List	   *stmts = linitial_node(List, castNode(List, sql_body_in));
			ListCell   *lc;
			List	   *transformed_stmts = NIL;

			foreach(lc, stmts)
			{
				Node	   *stmt = lfirst(lc);
				Query	   *q;
				ParseState *pstate = make_parsestate(NULL);

				pstate->p_sourcetext = queryString;
				sql_fn_parser_setup(pstate, pinfo);
				q = transformStmt(pstate, stmt);
				if (q->commandType == CMD_UTILITY)
					ereport(ERROR,
							errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("%s is not yet supported in unquoted SQL function body",
								   GetCommandTagName(CreateCommandTag(q->utilityStmt))));
				transformed_stmts = lappend(transformed_stmts, q);
				free_parsestate(pstate);
			}

			*sql_body_out = (Node *) list_make1(transformed_stmts);
		}
		else
		{
			Query	   *q;
			ParseState *pstate = make_parsestate(NULL);

			pstate->p_sourcetext = queryString;
			sql_fn_parser_setup(pstate, pinfo);
			q = transformStmt(pstate, sql_body_in);
			if (q->commandType == CMD_UTILITY)
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("%s is not yet supported in unquoted SQL function body",
							   GetCommandTagName(CreateCommandTag(q->utilityStmt))));
			free_parsestate(pstate);

			*sql_body_out = (Node *) q;
		}

		/*
		 * We must put something in prosrc.  For the moment, just record an
		 * empty string.  It might be useful to store the original text of the
		 * CREATE FUNCTION statement --- but to make actual use of that in
		 * error reports, we'd also have to adjust readfuncs.c to not throw
		 * away node location fields when reading prosqlbody.
		 */
		*prosrc_str_p = pstrdup("");

		/* But we definitely don't need probin. */
		*probin_str_p = NULL;
	}
	else
	{
		/* Everything else wants the given string in prosrc. */
		*prosrc_str_p = strVal(linitial(as));
		*probin_str_p = NULL;

		if (list_length(as) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only one AS item needed for language \"%s\"",
							languageName)));

		if (languageOid == INTERNALlanguageId)
		{
			/*
			 * In PostgreSQL versions before 6.5, the SQL name of the created
			 * function could not be different from the internal name, and
			 * "prosrc" wasn't used.  So there is code out there that does
			 * CREATE FUNCTION xyz AS '' LANGUAGE internal. To preserve some
			 * modicum of backwards compatibility, accept an empty "prosrc"
			 * value as meaning the supplied SQL function name.
			 */
			if (strlen(*prosrc_str_p) == 0)
				*prosrc_str_p = funcname;
		}
	}
}


/*
 * CREATE FUNCTION
 */
ObjectAddress
CreateFunction(ParseState *pstate, CreateFunctionStmt *stmt)
{
	char	   *probin_str;
	char	   *prosrc_str;
	Node	   *prosqlbody;
	Oid			prorettype;
	bool		returnsSet;
	char	   *language;
	Oid			languageOid;
	Oid			languageValidator;
	char	   *funcname;
	Oid			namespaceId;
	oidvector  *parameterTypes;
	List	   *parameterTypes_list = NIL;
	ArrayType  *allParameterTypes;
	ArrayType  *parameterModes;
	ArrayType  *parameterNames;
	List	   *inParameterNames_list = NIL;
	List	   *parameterDefaults;
	Oid			variadicArgType;
	List	   *trftypes_list = NIL;
	ArrayType  *trftypes;
	Oid			requiredResultType;
	bool		isWindowFunc,
				isStrict,
				security,
				isLeakProof;
	char		volatility;
	ArrayType  *proconfig;
	float4		procost;
	float4		prorows;
	Oid			prosupport;
	HeapTuple	languageTuple;
	Form_pg_language languageStruct;
	List	   *as_clause;
	char		parallel;

	/* Convert list of names to a name and namespace */
	namespaceId = QualifiedNameGetCreationNamespace(stmt->funcname,
													&funcname);

	/* Set default attributes */
	as_clause = NIL;
	language = NULL;
	isWindowFunc = false;
	isStrict = false;
	security = false;
	isLeakProof = false;
	volatility = PROVOLATILE_VOLATILE;
	proconfig = NULL;
	procost = -1;				/* indicates not set */
	prorows = -1;				/* indicates not set */
	prosupport = InvalidOid;
	parallel = PROPARALLEL_UNSAFE;

	/* Extract non-default attributes from stmt->options list */
	compute_function_attributes(pstate,
								stmt->is_procedure,
								stmt->options,
								&as_clause, &language,
								&isWindowFunc, &volatility,
								&isStrict, &security, &isLeakProof,
								&proconfig, &procost, &prorows,
								&prosupport, &parallel);

	if (!language)
	{
		if (stmt->sql_body)
			language = "sql";
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("no language specified")));
	}

	/* Look up the language and validate permissions */
	languageTuple = SearchSysCache1(LANGNAME, PointerGetDatum(language));
	if (!HeapTupleIsValid(languageTuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("language \"%s\" does not exist", language),
				 (extension_file_exists(language) ?
				  errhint("Use CREATE EXTENSION to load the language into the database.") : 0)));

	languageStruct = (Form_pg_language) GETSTRUCT(languageTuple);
	languageOid = languageStruct->oid;

	languageValidator = languageStruct->lanvalidator;

	ReleaseSysCache(languageTuple);

	/*
	 * Convert remaining parameters of CREATE to form wanted by
	 * ProcedureCreate.
	 */
	interpret_function_parameter_list(pstate,
									  stmt->parameters,
									  languageOid,
									  stmt->is_procedure ? OBJECT_PROCEDURE : OBJECT_FUNCTION,
									  &parameterTypes,
									  &parameterTypes_list,
									  &allParameterTypes,
									  &parameterModes,
									  &parameterNames,
									  &inParameterNames_list,
									  &parameterDefaults,
									  &variadicArgType,
									  &requiredResultType);

	if (stmt->is_procedure)
	{
		Assert(!stmt->returnType);
		prorettype = requiredResultType ? requiredResultType : VOIDOID;
		returnsSet = false;
	}
	else if (stmt->returnType)
	{
		/* explicit RETURNS clause */
		compute_return_type(stmt->returnType, languageOid,
							&prorettype, &returnsSet);
		if (OidIsValid(requiredResultType) && prorettype != requiredResultType)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("function result type must be %s because of OUT parameters",
							format_type_be(requiredResultType))));
	}
	else if (OidIsValid(requiredResultType))
	{
		/* default RETURNS clause from OUT parameters */
		prorettype = requiredResultType;
		returnsSet = false;
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("function result type must be specified")));
		/* Alternative possibility: default to RETURNS VOID */
		prorettype = VOIDOID;
		returnsSet = false;
	}

	if (list_length(trftypes_list) > 0)
	{
		ListCell   *lc;
		Datum	   *arr;
		int			i;

		arr = palloc(list_length(trftypes_list) * sizeof(Datum));
		i = 0;
		foreach(lc, trftypes_list)
			arr[i++] = ObjectIdGetDatum(lfirst_oid(lc));
		trftypes = construct_array(arr, list_length(trftypes_list),
								   OIDOID, sizeof(Oid), true, TYPALIGN_INT);
	}
	else
	{
		/* store SQL NULL instead of empty array */
		trftypes = NULL;
	}

	interpret_AS_clause(languageOid, language, funcname, as_clause, stmt->sql_body,
						parameterTypes_list, inParameterNames_list,
						&prosrc_str, &probin_str, &prosqlbody,
						pstate->p_sourcetext);

	/*
	 * Set default values for COST and ROWS depending on other parameters;
	 * reject ROWS if it's not returnsSet.  NB: pg_dump knows these default
	 * values, keep it in sync if you change them.
	 */
	if (procost < 0)
	{
		/* SQL and PL-language functions are assumed more expensive */
		if (languageOid == INTERNALlanguageId ||
			languageOid == ClanguageId)
			procost = 1;
		else
			procost = 100;
	}
	if (prorows < 0)
	{
		if (returnsSet)
			prorows = 1000;
		else
			prorows = 0;		/* dummy value if not returnsSet */
	}
	else if (!returnsSet)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ROWS is not applicable when function does not return a set")));

	/*
	 * And now that we have all the parameters, and know we're permitted to do
	 * so, go ahead and create the function.
	 */
	return ProcedureCreate(funcname,
						   namespaceId,
						   stmt->replace,
						   returnsSet,
						   prorettype,
						   GetUserId(),
						   languageOid,
						   languageValidator,
						   prosrc_str,	/* converted to text later */
						   probin_str,	/* converted to text later */
						   prosqlbody,
						   stmt->is_procedure ? PROKIND_PROCEDURE : (isWindowFunc ? PROKIND_WINDOW : PROKIND_FUNCTION),
						   security,
						   isLeakProof,
						   isStrict,
						   volatility,
						   parallel,
						   parameterTypes,
						   PointerGetDatum(allParameterTypes),
						   PointerGetDatum(parameterModes),
						   PointerGetDatum(parameterNames),
						   parameterDefaults,
						   PointerGetDatum(trftypes),
						   PointerGetDatum(proconfig),
						   prosupport,
						   procost,
						   prorows);
}


/*
 * Guts of function deletion.
 *
 * Note: this is also used for aggregate deletion, since the OIDs of
 * both functions and aggregates point to pg_proc.
 */
void
RemoveFunctionById(Oid funcOid)
{
	Relation	relation;
	HeapTuple	tup;
	char		prokind;

	/*
	 * Delete the pg_proc tuple.
	 */
	relation = table_open(ProcedureRelationId, RowExclusiveLock);

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcOid));
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for function %u", funcOid);

	prokind = ((Form_pg_proc) GETSTRUCT(tup))->prokind;

	CatalogTupleDelete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(relation, RowExclusiveLock);

	/*
	 * If there's a pg_aggregate tuple, delete that too.
	 */
	if (prokind == PROKIND_AGGREGATE)
	{
		relation = table_open(AggregateRelationId, RowExclusiveLock);

		tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(funcOid));
		if (!HeapTupleIsValid(tup)) /* should not happen */
			elog(ERROR, "cache lookup failed for pg_aggregate tuple for function %u", funcOid);

		CatalogTupleDelete(relation, &tup->t_self);

		ReleaseSysCache(tup);

		table_close(relation, RowExclusiveLock);
	}
}









/*
 * Subroutine for ALTER FUNCTION/AGGREGATE SET SCHEMA/RENAME
 *
 * Is there a function with the given name and signature already in the given
 * namespace?  If so, raise an appropriate error message.
 */
void
IsThereFunctionInNamespace(const char *proname, int pronargs,
						   oidvector *proargtypes, Oid nspOid)
{
	/* check for duplicate name (more friendly than unique-index failure) */
	if (SearchSysCacheExists3(PROCNAMEARGSNSP,
							  CStringGetDatum(proname),
							  PointerGetDatum(proargtypes),
							  ObjectIdGetDatum(nspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_FUNCTION),
				 errmsg("function %s already exists in schema \"%s\"",
						funcname_signature_string(proname, pronargs,
												  NIL, proargtypes->values),
						get_namespace_name(nspOid))));
}
