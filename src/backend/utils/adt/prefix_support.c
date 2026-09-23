/*-------------------------------------------------------------------------
 *
 * prefix_support.c
 *	  Selectivity estimation for the ^@ (starts-with) operator.
 *
 * This file supports the "^@" operator on text columns, which tests whether
 * the left operand begins with the right operand (starts_with()).  The
 * operator is not indexable by itself, but an accurate selectivity estimate
 * still matters when the planner has to choose between index and sequential
 * plans for other clauses of the query.
 *
 * The estimate is derived from the column's histogram and most-common-values
 * statistics, treating the comparison value as a fixed prefix: "variable ^@
 * 'foo'" is estimated as the selectivity of "variable >= 'foo' AND variable
 * < 'fop'".
 *
 * Note: the LIKE/regex pattern support that used to live in this file (and
 * the pattern-prefix index-condition machinery it fed) has been trimmed from
 * this project.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/prefix_support.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/selfuncs.h"
#include "utils/varlena.h"


static double patternsel_common(PlannerInfo *root,
								Oid oprid,
								Oid opfuncid,
								List *args,
								int varRelid,
								Oid collation);
static Selectivity prefix_selectivity(PlannerInfo *root,
									  VariableStatData *vardata,
									  Oid eqopr, Oid ltopr, Oid geopr,
									  Oid collation,
									  Const *prefixcon);
static Const *make_greater_string(const Const *str_const, FmgrInfo *ltproc,
								  Oid collation);
static Const *string_to_const(const char *str);


/*
 *		prefixsel			- Selectivity of starts-with operator.
 */
Datum
prefixsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	Oid			collation = PG_GET_COLLATION();

	PG_RETURN_FLOAT8(patternsel_common(root,
									   operator,
									   InvalidOid,
									   args,
									   varRelid,
									   collation));
}

/*
 *		prefixjoinsel			- Join selectivity of starts-with operator
 */
Datum
prefixjoinsel(PG_FUNCTION_ARGS)
{
	/* For the moment we just punt. */
	PG_RETURN_FLOAT8(DEFAULT_MATCH_SEL);
}

/*
 * patternsel_common - Selectivity of the starts-with operator.
 *
 * The right-hand side is a fixed prefix, so we can use the column's
 * histogram to estimate how many values fall in the range of strings
 * beginning with that prefix, and use the MCV list to get exact answers
 * for the most common values.
 *
 * Note that oprid and/or opfuncid should be for the positive-match operator
 * (the starts-with operator has no negator).
 */
static double
patternsel_common(PlannerInfo *root,
				  Oid oprid,
				  Oid opfuncid,
				  List *args,
				  int varRelid,
				  Oid collation)
{
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Datum		constval;
	Oid			consttype;
	Oid			vartype;
	Oid			eqopr = TextEqualOperator;
	Oid			ltopr = TextLessOperator;
	Oid			geopr = TextGreaterEqualOperator;
	Const	   *prefix;
	double		nullfrac = 0.0;
	double		result;

	result = DEFAULT_MATCH_SEL;

	/*
	 * If expression is not variable op constant, then punt and return the
	 * default estimate.
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		return result;
	if (!varonleft || !IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		return result;
	}

	/*
	 * If the constant is NULL, assume operator is strict and return zero, ie,
	 * operator will never return TRUE.
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		return 0.0;
	}
	constval = ((Const *) other)->constvalue;
	consttype = ((Const *) other)->consttype;

	/*
	 * The right-hand const is type text for the starts-with operator.  We do
	 * not expect to see binary-compatible types here, since const-folding
	 * should have relabeled the const to exactly match the operator's
	 * declared type.
	 */
	if (consttype != TEXTOID)
	{
		ReleaseVariableStats(vardata);
		return result;
	}

	/*
	 * Similarly, the exposed type of the left-hand side should be text.  (Do
	 * not look at vardata.atttype, which might be something binary-compatible
	 * but different.)
	 */
	vartype = vardata.vartype;
	if (vartype != TEXTOID)
	{
		ReleaseVariableStats(vardata);
		return result;
	}

	/*
	 * Grab the nullfrac for use below.
	 */
	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		nullfrac = stats->stanullfrac;
	}

	/*
	 * The whole comparison value is a fixed prefix here, so use the constant
	 * as-is when estimating the selectivity of the prefix.
	 */
	prefix = (Const *) other;

	{
		Selectivity selec;
		int			hist_size;
		FmgrInfo	opproc;
		double		mcv_selec,
					sumcommon;

		/* Try to use the histogram entries to get selectivity */
		if (!OidIsValid(opfuncid))
			opfuncid = get_opcode(oprid);
		fmgr_info(opfuncid, &opproc);

		selec = histogram_selectivity(&vardata, &opproc, collation,
									  constval, true,
									  10, 1, &hist_size);

		/* If not at least 100 entries, use the heuristic method */
		if (hist_size < 100)
		{
			Selectivity heursel;
			Selectivity prefixsel;

			prefixsel = prefix_selectivity(root, &vardata,
										   eqopr, ltopr, geopr,
										   collation,
										   prefix);
			heursel = prefixsel;

			if (selec < 0)		/* fewer than 10 histogram entries? */
				selec = heursel;
			else
			{
				/*
				 * For histogram sizes from 10 to 100, we combine the
				 * histogram and heuristic selectivities, putting increasingly
				 * more trust in the histogram for larger sizes.
				 */
				double		hist_weight = hist_size / 100.0;

				selec = selec * hist_weight + heursel * (1.0 - hist_weight);
			}
		}

		/* In any case, don't believe extremely small or large estimates. */
		if (selec < 0.0001)
			selec = 0.0001;
		else if (selec > 0.9999)
			selec = 0.9999;

		/*
		 * If we have most-common-values info, add up the fractions of the MCV
		 * entries that satisfy MCV ^@ PREFIX.  These fractions contribute
		 * directly to the result selectivity.  Also add up the total fraction
		 * represented by MCV entries.
		 */
		mcv_selec = mcv_selectivity(&vardata, &opproc, collation,
									constval, true,
									&sumcommon);

		/*
		 * Now merge the results from the MCV and histogram calculations,
		 * realizing that the histogram covers only the non-null values that
		 * are not listed in MCV.
		 */
		selec *= 1.0 - nullfrac - sumcommon;
		selec += mcv_selec;
		result = selec;
	}

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(result);

	ReleaseVariableStats(vardata);

	return result;
}

/*
 * Estimate the selectivity of a fixed prefix for a pattern match.
 *
 * A fixed prefix "foo" is estimated as the selectivity of the expression
 * "variable >= 'foo' AND variable < 'fop'".
 *
 * The selectivity estimate is with respect to the portion of the column
 * population represented by the histogram --- the caller must fold this
 * together with info about MCVs and NULLs.
 *
 * We use the given comparison operators and collation to do the estimation.
 * The given variable and Const must be of the associated datatype(s).
 *
 * XXX Note: we make use of the upper bound to estimate operator selectivity
 * even if the locale is such that we cannot rely on the upper-bound string.
 * The selectivity only needs to be approximately right anyway, so it seems
 * more useful to use the upper-bound code than not.
 */
static Selectivity
prefix_selectivity(PlannerInfo *root, VariableStatData *vardata,
				   Oid eqopr, Oid ltopr, Oid geopr,
				   Oid collation,
				   Const *prefixcon)
{
	Selectivity prefixsel;
	FmgrInfo	opproc;
	Const	   *greaterstrcon;
	Selectivity eq_sel;

	/* Estimate the selectivity of "x >= prefix" */
	fmgr_info(get_opcode(geopr), &opproc);

	prefixsel = ineq_histogram_selectivity(root, vardata,
										   geopr, &opproc, true, true,
										   collation,
										   prefixcon->constvalue,
										   prefixcon->consttype);

	if (prefixsel < 0.0)
	{
		/* No histogram is present ... return a suitable default estimate */
		return DEFAULT_MATCH_SEL;
	}

	/*
	 * If we can create a string larger than the prefix, say "x < greaterstr".
	 */
	fmgr_info(get_opcode(ltopr), &opproc);
	greaterstrcon = make_greater_string(prefixcon, &opproc, collation);
	if (greaterstrcon)
	{
		Selectivity topsel;

		topsel = ineq_histogram_selectivity(root, vardata,
											ltopr, &opproc, false, false,
											collation,
											greaterstrcon->constvalue,
											greaterstrcon->consttype);

		/* ineq_histogram_selectivity worked before, it shouldn't fail now */
		Assert(topsel >= 0.0);

		/*
		 * Merge the two selectivities in the same way as for a range query
		 * (see clauselist_selectivity()).  Note that we don't need to worry
		 * about double-exclusion of nulls, since ineq_histogram_selectivity
		 * doesn't count those anyway.
		 */
		prefixsel = topsel + prefixsel - 1.0;
	}

	/*
	 * If the prefix is long then the two bounding values might be too close
	 * together for the histogram to distinguish them usefully, resulting in a
	 * zero estimate (plus or minus roundoff error). To avoid returning a
	 * ridiculously small estimate, compute the estimated selectivity for
	 * "variable = 'foo'", and clamp to that. (Obviously, the resultant
	 * estimate should be at least that.)
	 *
	 * We apply this even if we couldn't make a greater string.  That case
	 * suggests that the prefix is near the maximum possible, and thus
	 * probably off the end of the histogram, and thus we probably got a very
	 * small estimate from the >= condition; so we still need to clamp.
	 */
	eq_sel = var_eq_const(vardata, eqopr, collation, prefixcon->constvalue,
						  false, true, false);

	prefixsel = Max(prefixsel, eq_sel);

	return prefixsel;
}

/*
 * Try to generate a string greater than the given string or any
 * string it is a prefix of.  If successful, return a palloc'd string
 * in the form of a Const node; else return NULL.
 *
 * The caller must provide the appropriate "less than" comparison function
 * for testing the strings, along with the collation to use.
 *
 * The key requirement here is that given a prefix string, say "foo",
 * we must be able to generate another string "fop" that is greater than
 * all strings "foobar" starting with "foo".  We can test that we have
 * generated a string greater than the prefix string, but in non-C collations
 * that is not a bulletproof guarantee that an extension of the string might
 * not sort after it; an example is that "foo " is less than "foo!", but it
 * is not clear that a "dictionary" sort ordering will consider "foo!" less
 * than "foo bar".  CAUTION: Therefore, this function should be used only for
 * estimation purposes when working in a non-C collation.
 *
 * To try to catch most cases where an extended string might otherwise sort
 * before the result value, we determine which of the strings "Z", "z", "y",
 * and "9" is seen as largest by the collation, and append that to the given
 * prefix before trying to find a string that compares as larger.
 *
 * To search for a greater string, we repeatedly "increment" the rightmost
 * character, using an encoding-specific character incrementer function.
 * When it's no longer possible to increment the last character, we truncate
 * off that character and start incrementing the next-to-rightmost.
 * For example, if "z" were the last character in the sort order, then we
 * could produce "foo" as a string greater than "fonz".
 *
 * This could be rather slow in the worst case, but in most cases we
 * won't have to try more than one or two strings before succeeding.
 *
 * Note that it's important for the character incrementer not to be too anal
 * about producing every possible character code, since in some cases the only
 * way to get a larger string is to increment a previous character position.
 * So we don't want to spend too much time trying every possible character
 * code at the last position.  A good rule of thumb is to be sure that we
 * don't try more than 256*K values for a K-byte character (and definitely
 * not 256^K, which is what an exhaustive search would approach).
 */
static Const *
make_greater_string(const Const *str_const, FmgrInfo *ltproc, Oid collation)
{
	char	   *workstr;
	int			len;
	Datum		cmpstr;
	char	   *cmptxt = NULL;
	mbcharacter_incrementer charinc;

	/*
	 * Get a modifiable copy of the prefix string in C-string format, and set
	 * up the string we will compare to as a Datum.  In C locale this can just
	 * be the given prefix string, otherwise we need to add a suffix.
	 */
	workstr = TextDatumGetCString(str_const->constvalue);
	len = strlen(workstr);
	if (lc_collate_is_c(collation) || len == 0)
		cmpstr = str_const->constvalue;
	else
	{
		/* If first time through, determine the suffix to use */
		static char suffixchar = 0;
		static Oid	suffixcollation = 0;

		if (!suffixchar || suffixcollation != collation)
		{
			char	   *best;

			best = "Z";
			if (varstr_cmp(best, 1, "z", 1, collation) < 0)
				best = "z";
			if (varstr_cmp(best, 1, "y", 1, collation) < 0)
				best = "y";
			if (varstr_cmp(best, 1, "9", 1, collation) < 0)
				best = "9";
			suffixchar = *best;
			suffixcollation = collation;
		}

		/* And build the string to compare to */
		cmptxt = palloc(VARHDRSZ + len + 1);
		SET_VARSIZE(cmptxt, VARHDRSZ + len + 1);
		memcpy(VARDATA(cmptxt), workstr, len);
		*(VARDATA(cmptxt) + len) = suffixchar;
		cmpstr = PointerGetDatum(cmptxt);
	}

	/* Select the character-incrementer function for the database encoding */
	charinc = pg_database_encoding_character_incrementer();

	/* And search ... */
	while (len > 0)
	{
		int			charlen;
		unsigned char *lastchar;

		/* Identify the last character (which may be multibyte) */
		charlen = len - pg_mbcliplen(workstr, len, len - 1);
		lastchar = (unsigned char *) (workstr + len - charlen);

		/*
		 * Try to generate a larger string by incrementing the last character.
		 *
		 * Note: the incrementer function is expected to return true if it's
		 * generated a valid-per-the-encoding new character, otherwise false.
		 * The contents of the character on false return are unspecified.
		 */
		while (charinc(lastchar, charlen))
		{
			Const	   *workstr_const;

			workstr_const = string_to_const(workstr);

			if (DatumGetBool(FunctionCall2Coll(ltproc,
											   collation,
											   cmpstr,
											   workstr_const->constvalue)))
			{
				/* Successfully made a string larger than cmpstr */
				if (cmptxt)
					pfree(cmptxt);
				pfree(workstr);
				return workstr_const;
			}

			/* No good, release unusable value and try again */
			pfree(DatumGetPointer(workstr_const->constvalue));
			pfree(workstr_const);
		}

		/*
		 * No luck here, so truncate off the last character and try to
		 * increment the next one.
		 */
		len -= charlen;
		workstr[len] = '\0';
	}

	/* Failed... */
	if (cmptxt)
		pfree(cmptxt);
	pfree(workstr);

	return NULL;
}

/*
 * Generate a Const node of text type from a C string.
 */
static Const *
string_to_const(const char *str)
{
	Datum		conval = CStringGetTextDatum(str);

	return makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
					 conval, false, false);
}
