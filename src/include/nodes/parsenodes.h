/*-------------------------------------------------------------------------
 *
 * parsenodes.h
 *	  definitions for parse tree nodes
 *
 * Many of the node types used in parsetrees include a "location" field.
 * This is a byte (not character) offset in the original source text, to be
 * used for positioning an error cursor when there is an error related to
 * the node.  Access to the original source text is needed to make use of
 * the location.  At the topmost (statement) level, we also provide a
 * statement length, likewise measured in bytes, for convenience in
 * identifying statement boundaries in multi-statement source strings.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/parsenodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSENODES_H
#define PARSENODES_H

#include "nodes/bitmapset.h"
#include "nodes/lockoptions.h"
#include "nodes/primnodes.h"
#include "nodes/value.h"


typedef enum OverridingKind
{
	OVERRIDING_NOT_SET = 0,
	OVERRIDING_USER_VALUE,
	OVERRIDING_SYSTEM_VALUE
} OverridingKind;

/* Possible sources of a Query */
typedef enum QuerySource
{
	QSRC_ORIGINAL,				/* original parsetree (explicit query) */
	QSRC_PARSER,				/* added by parse analysis (now unused) */
	QSRC_INSTEAD_RULE,			/* added by unconditional INSTEAD rule */
	QSRC_QUAL_INSTEAD_RULE,		/* added by conditional INSTEAD rule */
	QSRC_NON_INSTEAD_RULE		/* added by non-INSTEAD rule */
} QuerySource;

/* Sort ordering options for ORDER BY and CREATE INDEX */
typedef enum SortByDir
{
	SORTBY_DEFAULT,
	SORTBY_ASC,
	SORTBY_DESC,
	SORTBY_USING				/* not allowed in CREATE INDEX ... */
} SortByDir;

typedef enum SortByNulls
{
	SORTBY_NULLS_DEFAULT,
	SORTBY_NULLS_FIRST,
	SORTBY_NULLS_LAST
} SortByNulls;

/* Options for [ ALL | DISTINCT ] */
typedef enum SetQuantifier
{
	SET_QUANTIFIER_DEFAULT,
	SET_QUANTIFIER_ALL,
	SET_QUANTIFIER_DISTINCT
} SetQuantifier;

/*
 * Grantable rights are encoded so that we can OR them together in a bitmask.
 * AclMode is defined as uint32 and holds these privilege bits.
 */
typedef uint32 AclMode;			/* a bitmask of privilege bits */

#define ACL_INSERT		(1<<0)	/* for relations */
#define ACL_SELECT		(1<<1)
#define ACL_UPDATE		(1<<2)
#define ACL_DELETE		(1<<3)
#define ACL_TRUNCATE	(1<<4)
#define ACL_REFERENCES	(1<<5)
#define ACL_TRIGGER		(1<<6)
#define ACL_EXECUTE		(1<<7)	/* for functions */
#define ACL_USAGE		(1<<8)	/* for languages, namespaces, FDWs, and
								 * servers */
#define ACL_CREATE		(1<<9)	/* for namespaces and databases */
#define ACL_CREATE_TEMP (1<<10) /* for databases */
#define ACL_CONNECT		(1<<11) /* for databases */
#define N_ACL_RIGHTS	12		/* 1 plus the last 1<<x */
#define ACL_NO_RIGHTS	0
/* Currently, SELECT ... FOR [KEY] UPDATE/SHARE requires UPDATE privileges */
#define ACL_SELECT_FOR_UPDATE	ACL_UPDATE


/*****************************************************************************
 *	Query Tree
 *****************************************************************************/

/*
 * Query -
 *	  Parse analysis turns all statements into a Query tree
 *	  for further processing by the rewriter and planner.
 *
 *	  Utility statements (i.e. non-optimizable statements) have the
 *	  utilityStmt field set, and the rest of the Query is mostly dummy.
 *
 *	  Planning converts a Query tree into a Plan tree headed by a PlannedStmt
 *	  node --- the Query structure is not used by the executor.
 */
typedef struct Query
{
	NodeTag		type;

	CmdType		commandType;	/* select|insert|update|delete|utility */

	QuerySource querySource;	/* where did I come from? */

	uint64		queryId;		/* query identifier (can be set by plugins) */

	bool		canSetTag;		/* do I set the command result tag? */

	Node	   *utilityStmt;	/* non-null if commandType == CMD_UTILITY */

	int			resultRelation; /* rtable index of target relation for
								 * INSERT/UPDATE/DELETE; 0 for SELECT */

	bool		hasAggs;		/* has aggregates in tlist or havingQual */
	bool		hasTargetSRFs;	/* has set-returning functions in tlist */
	bool		hasSubLinks;	/* has subquery SubLink */
	bool		hasDistinctOn;	/* distinctClause is from DISTINCT ON */
	bool		hasForUpdate;	/* FOR [KEY] UPDATE/SHARE was specified */

	List	   *rtable;			/* list of range table entries */
	FromExpr   *jointree;		/* table join tree (FROM and WHERE clauses) */

	List	   *targetList;		/* target list (of TargetEntry) */

	OverridingKind override;	/* OVERRIDING clause */

	OnConflictExpr *onConflict; /* ON CONFLICT DO [NOTHING | UPDATE] */

	List	   *returningList;	/* return-values list (of TargetEntry) */

	List	   *groupClause;	/* a list of SortGroupClause's */
	bool		groupDistinct;	/* is the group by clause distinct? */

	List	   *groupingSets;	/* a list of GroupingSet's if present */

	Node	   *havingQual;		/* qualifications applied to groups */

	List	   *distinctClause; /* a list of SortGroupClause's */

	List	   *sortClause;		/* a list of SortGroupClause's */

	Node	   *limitOffset;	/* # of result tuples to skip (int8 expr) */
	Node	   *limitCount;		/* # of result tuples to return (int8 expr) */
	LimitOption limitOption;	/* limit type */

	List	   *rowMarks;		/* a list of RowMarkClause's */

	List	   *constraintDeps; /* a list of pg_constraint OIDs that the query
								 * depends on to be semantically valid */

	List	   *withCheckOptions;	/* a list of WithCheckOption's (added
									 * during rewrite) */

	/*
	 * The following two fields identify the portion of the source text string
	 * containing this query.  They are typically only populated in top-level
	 * Queries, not in sub-queries.  When not set, they might both be zero, or
	 * both be -1 meaning "unknown".
	 */
	int			stmt_location;	/* start location, or -1 if unknown */
	int			stmt_len;		/* length in bytes; 0 means "rest of string" */
} Query;


/****************************************************************************
 *	Supporting data structures for Parse Trees
 *
 *	Most of these node types appear in raw parsetrees output by the grammar,
 *	and get transformed to something else by the analyzer.  A few of them
 *	are used as-is in transformed querytrees.
 ****************************************************************************/

/*
 * TypeName - specifies a type in definitions
 *
 * For TypeName structures generated internally, it is often easier to
 * specify the type by OID than by name.  If "names" is NIL then the
 * actual type OID is given by typeOid, otherwise typeOid is unused.
 * Similarly, if "typmods" is NIL then the actual typmod is expected to
 * be prespecified in typemod, otherwise typemod is unused.
 *
 * If pct_type is true, then names is actually a field name and we look up
 * the type of that field.  Otherwise (the normal case), names is a type
 * name possibly qualified with schema and database name.
 */
typedef struct TypeName
{
	NodeTag		type;
	List	   *names;			/* qualified name (list of Value strings) */
	Oid			typeOid;		/* type identified by OID */
	bool		setof;			/* is a set? */
	bool		pct_type;		/* %TYPE specified? */
	List	   *typmods;		/* type modifier expression(s) */
	int32		typemod;		/* prespecified type modifier */
	List	   *arrayBounds;	/* array bounds */
	int			location;		/* token location, or -1 if unknown */
} TypeName;

/*
 * ColumnRef - specifies a reference to a column, or possibly a whole tuple
 *
 * The "fields" list must be nonempty.  It can contain string Value nodes
 * (representing names) and A_Star nodes (representing occurrence of a '*').
 * Currently, A_Star must appear only as the last list element --- the grammar
 * is responsible for enforcing this!
 *
 * Note: any container subscripting or selection of fields from composite columns
 * is represented by an A_Indirection node above the ColumnRef.  However,
 * for simplicity in the normal case, initial field selection from a table
 * name is represented within ColumnRef and not by adding A_Indirection.
 */
typedef struct ColumnRef
{
	NodeTag		type;
	List	   *fields;			/* field names (Value strings) or A_Star */
	int			location;		/* token location, or -1 if unknown */
} ColumnRef;

/*
 * ParamRef - specifies a $n parameter reference
 */
typedef struct ParamRef
{
	NodeTag		type;
	int			number;			/* the number of the parameter */
	int			location;		/* token location, or -1 if unknown */
} ParamRef;

/*
 * A_Expr - infix, prefix, and postfix expressions
 */
typedef enum A_Expr_Kind
{
	AEXPR_OP,					/* normal operator */
	AEXPR_OP_ANY,				/* scalar op ANY (array) */
	AEXPR_OP_ALL,				/* scalar op ALL (array) */
	AEXPR_DISTINCT,				/* IS DISTINCT FROM - name must be "=" */
	AEXPR_NOT_DISTINCT,			/* IS NOT DISTINCT FROM - name must be "=" */
	AEXPR_NULLIF,				/* NULLIF - name must be "=" */
	AEXPR_IN,					/* [NOT] IN - name must be "=" or "<>" */
	AEXPR_LIKE,					/* [NOT] LIKE - name must be "~~" or "!~~" */
	AEXPR_ILIKE,				/* [NOT] ILIKE - name must be "~~*" or "!~~*" */
	AEXPR_BETWEEN,				/* name must be "BETWEEN" */
	AEXPR_NOT_BETWEEN,			/* name must be "NOT BETWEEN" */
	AEXPR_BETWEEN_SYM,			/* name must be "BETWEEN SYMMETRIC" */
	AEXPR_NOT_BETWEEN_SYM		/* name must be "NOT BETWEEN SYMMETRIC" */
} A_Expr_Kind;

typedef struct A_Expr
{
	NodeTag		type;
	A_Expr_Kind kind;			/* see above */
	List	   *name;			/* possibly-qualified name of operator */
	Node	   *lexpr;			/* left argument, or NULL if none */
	Node	   *rexpr;			/* right argument, or NULL if none */
	int			location;		/* token location, or -1 if unknown */
} A_Expr;

/*
 * A_Const - a literal constant
 */
typedef struct A_Const
{
	NodeTag		type;
	Value		val;			/* value (includes type info, see value.h) */
	int			location;		/* token location, or -1 if unknown */
} A_Const;

/*
 * TypeCast - a CAST expression
 */
typedef struct TypeCast
{
	NodeTag		type;
	Node	   *arg;			/* the expression being casted */
	TypeName   *typeName;		/* the target type */
	int			location;		/* token location, or -1 if unknown */
} TypeCast;

/*
 * RoleSpec - a role name or one of a few special values.
 */
typedef enum RoleSpecType
{
	ROLESPEC_CSTRING,			/* role name is stored as a C string */
	ROLESPEC_CURRENT_ROLE,		/* role spec is CURRENT_ROLE */
	ROLESPEC_CURRENT_USER,		/* role spec is CURRENT_USER */
	ROLESPEC_SESSION_USER,		/* role spec is SESSION_USER */
	ROLESPEC_PUBLIC				/* role name is "public" */
} RoleSpecType;

typedef struct RoleSpec
{
	NodeTag		type;
	RoleSpecType roletype;		/* Type of this rolespec */
	char	   *rolename;		/* filled only for ROLESPEC_CSTRING */
	int			location;		/* token location, or -1 if unknown */
} RoleSpec;

/*
 * FuncCall - a function or aggregate invocation
 *
 * agg_order (if not NIL) indicates we saw 'foo(... ORDER BY ...)', or if
 * agg_within_group is true, it was 'foo(...) WITHIN GROUP (ORDER BY ...)'.
 * agg_star indicates we saw a 'foo(*)' construct, while agg_distinct
 * indicates we saw 'foo(DISTINCT ...)'.  In any of these cases, the
 * construct *must* be an aggregate call.  Otherwise, it might be either an
 * aggregate or some other kind of function.  However, if FILTER or OVER is
 * present it had better be an aggregate or window function.
 *
 * Normally, you'd initialize this via makeFuncCall() and then only change the
 * parts of the struct its defaults don't match afterwards, as needed.
 */
typedef struct FuncCall
{
	NodeTag		type;
	List	   *funcname;		/* qualified name of function */
	List	   *args;			/* the arguments (list of exprs) */
	List	   *agg_order;		/* ORDER BY (list of SortBy) */
	Node	   *agg_filter;		/* FILTER clause, if any */
	bool		agg_within_group;	/* ORDER BY appeared in WITHIN GROUP */
	bool		agg_star;		/* argument was really '*' */
	bool		agg_distinct;	/* arguments were labeled DISTINCT */
	bool		func_variadic;	/* last argument was labeled VARIADIC */
	CoercionForm funcformat;	/* how to display this node */
	int			location;		/* token location, or -1 if unknown */
} FuncCall;

/*
 * A_Star - '*' representing all columns of a table or compound field
 *
 * This can appear within ColumnRef.fields, A_Indirection.indirection, and
 * ResTarget.indirection lists.
 */
typedef struct A_Star
{
	NodeTag		type;
} A_Star;

/*
 * A_Indices - array subscript or slice bounds ([idx] or [lidx:uidx])
 *
 * In slice case, either or both of lidx and uidx can be NULL (omitted).
 * In non-slice case, uidx holds the single subscript and lidx is always NULL.
 */
typedef struct A_Indices
{
	NodeTag		type;
	bool		is_slice;		/* true if slice (i.e., colon present) */
	Node	   *lidx;			/* slice lower bound, if any */
	Node	   *uidx;			/* subscript, or slice upper bound if any */
} A_Indices;

/*
 * A_Indirection - select a field and/or array element from an expression
 *
 * The indirection list can contain A_Indices nodes (representing
 * subscripting), string Value nodes (representing field selection --- the
 * string value is the name of the field to select), and A_Star nodes
 * (representing selection of all fields of a composite type).
 * For example, a complex selection operation like
 *				(foo).field1[42][7].field2
 * would be represented with a single A_Indirection node having a 4-element
 * indirection list.
 *
 * Currently, A_Star must appear only as the last list element --- the grammar
 * is responsible for enforcing this!
 */
typedef struct A_Indirection
{
	NodeTag		type;
	Node	   *arg;			/* the thing being selected from */
	List	   *indirection;	/* subscripts and/or field names and/or * */
} A_Indirection;

/*
 * A_ArrayExpr - an ARRAY[] construct
 */
typedef struct A_ArrayExpr
{
	NodeTag		type;
	List	   *elements;		/* array element expressions */
	int			location;		/* token location, or -1 if unknown */
} A_ArrayExpr;

/*
 * ResTarget -
 *	  result target (used in target list of pre-transformed parse trees)
 *
 * In a SELECT target list, 'name' is the column label from an
 * 'AS ColumnLabel' clause, or NULL if there was none, and 'val' is the
 * value expression itself.  The 'indirection' field is not used.
 *
 * INSERT uses ResTarget in its target-column-names list.  Here, 'name' is
 * the name of the destination column, 'indirection' stores any subscripts
 * attached to the destination, and 'val' is not used.
 *
 * In an UPDATE target list, 'name' is the name of the destination column,
 * 'indirection' stores any subscripts attached to the destination, and
 * 'val' is the expression to assign.
 *
 * See A_Indirection for more info about what can appear in 'indirection'.
 */
typedef struct ResTarget
{
	NodeTag		type;
	char	   *name;			/* column name or NULL */
	List	   *indirection;	/* subscripts, field names, and '*', or NIL */
	Node	   *val;			/* the value expression to compute or assign */
	int			location;		/* token location, or -1 if unknown */
} ResTarget;

/*
 * MultiAssignRef - element of a row source expression for UPDATE
 *
 * In an UPDATE target list, when we have SET (a,b,c) = row-valued-expression,
 * we generate separate ResTarget items for each of a,b,c.  Their "val" trees
 * are MultiAssignRef nodes numbered 1..n, linking to a common copy of the
 * row-valued-expression (which parse analysis will process only once, when
 * handling the MultiAssignRef with colno=1).
 */
typedef struct MultiAssignRef
{
	NodeTag		type;
	Node	   *source;			/* the row-valued expression */
	int			colno;			/* column number for this target (1..n) */
	int			ncolumns;		/* number of targets in the construct */
} MultiAssignRef;

/*
 * SortBy - for ORDER BY clause
 */
typedef struct SortBy
{
	NodeTag		type;
	Node	   *node;			/* expression to sort on */
	SortByDir	sortby_dir;		/* ASC/DESC/USING/default */
	SortByNulls sortby_nulls;	/* NULLS FIRST/LAST */
	List	   *useOp;			/* name of op to use, if SORTBY_USING */
	int			location;		/* operator location, or -1 if none/unknown */
} SortBy;

/*
 * RangeSubselect - subquery appearing in a FROM clause
 */
typedef struct RangeSubselect
{
	NodeTag		type;
	bool		lateral;		/* does it have LATERAL prefix? */
	Node	   *subquery;		/* the untransformed sub-select clause */
	Alias	   *alias;			/* table alias & optional column aliases */
} RangeSubselect;

/*
 * RangeFunction - function call appearing in a FROM clause
 *
 * functions is a List because we use this to represent the construct
 * ROWS FROM(func1(...), func2(...), ...).  Each element of this list is a
 * two-element sublist, the first element being the untransformed function
 * call tree, and the second element being a possibly-empty list of ColumnDef
 * nodes representing any columndef list attached to that function within the
 * ROWS FROM() syntax.
 *
 * alias and coldeflist represent any alias and/or columndef list attached
 * at the top level.  (We disallow coldeflist appearing both here and
 * per-function, but that's checked in parse analysis, not by the grammar.)
 */
typedef struct RangeFunction
{
	NodeTag		type;
	bool		lateral;		/* does it have LATERAL prefix? */
	bool		ordinality;		/* does it have WITH ORDINALITY suffix? */
	bool		is_rowsfrom;	/* is result of ROWS FROM() syntax? */
	List	   *functions;		/* per-function information, see above */
	Alias	   *alias;			/* table alias & optional column aliases */
	List	   *coldeflist;		/* list of ColumnDef nodes to describe result
								 * of function returning RECORD */
} RangeFunction;

/*
 * RangeTableSample - TABLESAMPLE appearing in a raw FROM clause
 *
 * This node, appearing only in raw parse trees, represents
 *		<relation> TABLESAMPLE <method> (<params>) REPEATABLE (<num>)
 * Currently, the <relation> can only be a RangeVar, but we might in future
 * allow RangeSubselect and other options.  Note that the RangeTableSample
 * is wrapped around the node representing the <relation>, rather than being
 * a subfield of it.
 */
typedef struct RangeTableSample
{
	NodeTag		type;
	Node	   *relation;		/* relation to be sampled */
	List	   *method;			/* sampling method name (possibly qualified) */
	List	   *args;			/* argument(s) for sampling method */
	Node	   *repeatable;		/* REPEATABLE expression, or NULL if none */
	int			location;		/* method name location, or -1 if unknown */
} RangeTableSample;

/*
 * ColumnDef - column definition (used in various creates)
 *
 * If the column has a default value, we may have the value expression
 * in either "raw" form (an untransformed parse tree) or "cooked" form
 * (a post-parse-analysis, executable expression tree), depending on
 * how this ColumnDef node was created (by parsing, or by inheritance
 * from an existing relation).  We should never have both in the same node!
 *
 * The constraints list may contain a CONSTR_DEFAULT item in a raw
 * parsetree produced by gram.y, but transformCreateStmt will remove
 * the item and set raw_default instead.  CONSTR_DEFAULT items
 * should not appear in any subsequent processing.
 */
typedef struct ColumnDef
{
	NodeTag		type;
	char	   *colname;		/* name of column */
	TypeName   *typeName;		/* type of column */
	char	   *compression;	/* compression method for column */
	int			inhcount;		/* number of times column is inherited */
	bool		is_local;		/* column has local (non-inherited) def'n */
	bool		is_not_null;	/* NOT NULL constraint specified? */
	bool		is_from_type;	/* column definition came from table type */
	char		storage;		/* attstorage setting, or 0 for default */
	Node	   *raw_default;	/* default value (untransformed parse tree) */
	Node	   *cooked_default; /* default value (transformed expr tree) */
	char		identity;		/* attidentity setting */
	RangeVar   *identitySequence;	/* to store identity sequence name for
									 * ALTER TABLE ... ADD COLUMN */
	char		generated;		/* attgenerated setting */
	List	   *constraints;	/* other constraints on column */
	int			location;		/* parse location, or -1 if none/unknown */
} ColumnDef;

/*
 * IndexElem - index parameters (used in CREATE INDEX, and in ON CONFLICT)
 *
 * For a plain index attribute, 'name' is the name of the table column to
 * index, and 'expr' is NULL.  For an index expression, 'name' is NULL and
 * 'expr' is the expression tree.
 */
typedef struct IndexElem
{
	NodeTag		type;
	char	   *name;			/* name of attribute to index, or NULL */
	Node	   *expr;			/* expression to index, or NULL */
	char	   *indexcolname;	/* name for index column; NULL = default */
	List	   *opclass;		/* name of desired opclass; NIL = default */
	List	   *opclassopts;	/* opclass-specific options, or NIL */
	SortByDir	ordering;		/* ASC/DESC/default */
	SortByNulls nulls_ordering; /* FIRST/LAST/default */
} IndexElem;

/*
 * DefElem - a generic "name = value" option definition
 *
 * In some contexts the name can be qualified.  Also, certain SQL commands
 * allow a SET/ADD/DROP action to be attached to option settings, so it's
 * convenient to carry a field for that too.  (Note: currently, it is our
 * practice that the grammar allows namespace and action only in statements
 * where they are relevant; C code can just ignore those fields in other
 * statements.)
 */
typedef enum DefElemAction
{
	DEFELEM_UNSPEC,				/* no action given */
	DEFELEM_SET,
	DEFELEM_ADD,
	DEFELEM_DROP
} DefElemAction;

typedef struct DefElem
{
	NodeTag		type;
	char	   *defnamespace;	/* NULL if unqualified name */
	char	   *defname;
	Node	   *arg;			/* a (Value *) or a (TypeName *) */
	DefElemAction defaction;	/* unspecified action, or SET/ADD/DROP */
	int			location;		/* token location, or -1 if unknown */
} DefElem;

/*
 * LockingClause - raw representation of FOR [NO KEY] UPDATE/[KEY] SHARE
 *		options
 *
 * Note: lockedRels == NIL means "all relations in query".  Otherwise it
 * is a list of RangeVar nodes.  (We use RangeVar mainly because it carries
 * a location field --- currently, parse analysis insists on unqualified
 * names in LockingClause.)
 */
typedef struct LockingClause
{
	NodeTag		type;
	List	   *lockedRels;		/* FOR [KEY] UPDATE/SHARE relations */
	LockClauseStrength strength;
	LockWaitPolicy waitPolicy;	/* NOWAIT and SKIP LOCKED */
} LockingClause;

/****************************************************************************
 *	Nodes for a Query tree
 ****************************************************************************/

/*--------------------
 * RangeTblEntry -
 *	  A range table is a List of RangeTblEntry nodes.
 *
 *	  A range table entry may represent a plain relation, a sub-select in
 *	  FROM, or the result of a JOIN clause.  (Only explicit JOIN syntax
 *	  produces an RTE, not the implicit join resulting from multiple FROM
 *	  items.  This is because we only need the RTE to deal with SQL features
 *	  like outer joins and join-output-column aliasing.)  Other special
 *	  RTE types also exist, as indicated by RTEKind.
 *
 *	  Note that we consider RTE_RELATION to cover anything that has a pg_class
 *	  entry.  relkind distinguishes the sub-cases.
 *
 *	  alias is an Alias node representing the AS alias-clause attached to the
 *	  FROM expression, or NULL if no clause.
 *
 *	  eref is the table reference name and column reference names (either
 *	  real or aliases).  Note that system columns (OID etc) are not included
 *	  in the column list.
 *	  eref->aliasname is required to be present, and should generally be used
 *	  to identify the RTE for error messages etc.
 *
 *	  In RELATION RTEs, the colnames in both alias and eref are indexed by
 *	  physical attribute number; this means there must be colname entries for
 *	  dropped columns.  When building an RTE we insert empty strings ("") for
 *	  dropped columns.  Note however that a stored rule may have nonempty
 *	  colnames for columns dropped since the rule was created (and for that
 *	  matter the colnames might be out of date due to column renamings).
 *	  The same comments apply to FUNCTION RTEs when a function's return type
 *	  is a named composite type.
 *
 *	  In JOIN RTEs, the colnames in both alias and eref are one-to-one with
 *	  joinaliasvars entries.  A JOIN RTE will omit columns of its inputs when
 *	  those columns are known to be dropped at parse time.  Again, however,
 *	  a stored rule might contain entries for columns dropped since the rule
 *	  was created.  (This is only possible for columns not actually referenced
 *	  in the rule.)  When loading a stored rule, we replace the joinaliasvars
 *	  items for any such columns with null pointers.  (We can't simply delete
 *	  them from the joinaliasvars list, because that would affect the attnums
 *	  of Vars referencing the rest of the list.)
 *
 *	  inh is true for relation references that should be expanded to include
 *	  inheritance children, if the rel has any.  This *must* be false for
 *	  RTEs other than RTE_RELATION entries.
 *
 *	  inFromCl marks those range variables that are listed in the FROM clause.
 *	  It's false for RTEs that are added to a query behind the scenes, such
 *	  as the NEW and OLD variables for a rule, or the subqueries of a UNION.
 *	  This flag is not used during parsing (except in transformLockingClause,
 *	  q.v.); the parser now uses a separate "namespace" data structure to
 *	  control visibility.  But it is needed by ruleutils.c to determine
 *	  whether RTEs should be shown in decompiled queries.
 *
 *	  For SELECT/INSERT/UPDATE permissions, if the user doesn't have
 *	  table-wide permissions then it is sufficient to have the permissions
 *	  on all columns identified in selectedCols (for SELECT) and/or
 *	  insertedCols and/or updatedCols (INSERT with ON CONFLICT DO UPDATE may
 *	  have all 3).  selectedCols, insertedCols and updatedCols are bitmapsets,
 *	  which cannot have negative integer members, so we subtract
 *	  FirstLowInvalidHeapAttributeNumber from column numbers before storing
 *	  them in these fields.  A whole-row Var reference is represented by
 *	  setting the bit for InvalidAttrNumber.
 *
 *	  updatedCols is also used in some other places, for example, to determine
 *	  which triggers to fire and in FDWs to know which changed columns they
 *	  need to ship off.
 *
 *	  extraUpdatedCols is no longer used or maintained; it's always empty.
 *--------------------
 */
typedef enum RTEKind
{
	RTE_RELATION,				/* ordinary relation reference */
	RTE_SUBQUERY,				/* subquery in FROM */
	RTE_JOIN,					/* join */
	RTE_FUNCTION,				/* function in FROM */
	RTE_VALUES,					/* VALUES (<exprlist>), (<exprlist>), ... */
	RTE_NAMEDTUPLESTORE,		/* tuplestore, e.g. for AFTER triggers */
	RTE_RESULT					/* RTE represents an empty FROM clause; such
								 * RTEs are added by the planner, they're not
								 * present during parsing or rewriting */
} RTEKind;

typedef struct RangeTblEntry
{
	NodeTag		type;

	RTEKind		rtekind;		/* see above */

	/*
	 * XXX the fields applicable to only some rte kinds should be merged into
	 * a union.  I didn't do this yet because the diffs would impact a lot of
	 * code that is being actively worked on.  FIXME someday.
	 */

	/*
	 * Fields valid for a plain relation RTE (else zero):
	 *
	 * As a special case, relid can also be set in RTE_SUBQUERY RTEs.  This
	 * happens when an RTE_RELATION RTE for a view is transformed to an
	 * RTE_SUBQUERY during rewriting.  We keep the relid because it is useful
	 * during planning, cf makeWholeRowVar.  (It will not be passed on to the
	 * executor, however.)
	 *
	 * As a special case, RTE_NAMEDTUPLESTORE can also set relid to indicate
	 * that the tuple format of the tuplestore is the same as the referenced
	 * relation.  This allows plans referencing AFTER trigger transition
	 * tables to be invalidated if the underlying table is altered.
	 *
	 * rellockmode is really LOCKMODE, but it's declared int to avoid having
	 * to include lock-related headers here.  It must be RowExclusiveLock if
	 * the RTE is an INSERT/UPDATE/DELETE target, else RowShareLock if the RTE
	 * is a SELECT FOR UPDATE/FOR SHARE target, else AccessShareLock.
	 *
	 * Note: in some cases, rule expansion may result in RTEs that are marked
	 * with RowExclusiveLock even though they are not the target of the
	 * current query; this happens if a DO ALSO rule simply scans the original
	 * target table.  We leave such RTEs with their original lockmode so as to
	 * avoid getting an additional, lesser lock.
	 */
	Oid			relid;			/* OID of the relation */
	char		relkind;		/* relation kind (see pg_class.relkind) */
	int			rellockmode;	/* lock level that query requires on the rel */
	struct TableSampleClause *tablesample;	/* sampling info, or NULL */

	/*
	 * Fields valid for a subquery RTE (else NULL):
	 */
	Query	   *subquery;		/* the sub-query */
	bool		security_barrier;	/* is from security_barrier view? */

	/*
	 * Fields valid for a join RTE (else NULL/zero):
	 *
	 * joinaliasvars is a list of (usually) Vars corresponding to the columns
	 * of the join result.  An alias Var referencing column K of the join
	 * result can be replaced by the K'th element of joinaliasvars --- but to
	 * simplify the task of reverse-listing aliases correctly, we do not do
	 * that until planning time.  In detail: an element of joinaliasvars can
	 * be a Var of one of the join's input relations, or such a Var with an
	 * implicit coercion to the join's output column type, or a COALESCE
	 * expression containing the two input column Vars (possibly coerced).
	 * Elements beyond the first joinmergedcols entries are always just Vars,
	 * and are never referenced from elsewhere in the query (that is, join
	 * alias Vars are generated only for merged columns).  We keep these
	 * entries only because they're needed in expandRTE() and similar code.
	 *
	 * Within a Query loaded from a stored rule, it is possible for non-merged
	 * joinaliasvars items to be null pointers, which are placeholders for
	 * (necessarily unreferenced) columns dropped since the rule was made.
	 * Also, once planning begins, joinaliasvars items can be almost anything,
	 * as a result of subquery-flattening substitutions.
	 *
	 * joinleftcols is an integer list of physical column numbers of the left
	 * join input rel that are included in the join; likewise joinrighttcols
	 * for the right join input rel.  (Which rels those are can be determined
	 * from the associated JoinExpr.)  If the join is USING/NATURAL, then the
	 * first joinmergedcols entries in each list identify the merged columns.
	 * The merged columns come first in the join output, then remaining
	 * columns of the left input, then remaining columns of the right.
	 *
	 * Note that input columns could have been dropped after creation of a
	 * stored rule, if they are not referenced in the query (in particular,
	 * merged columns could not be dropped); this is not accounted for in
	 * joinleftcols/joinrighttcols.
	 */
	JoinType	jointype;		/* type of join */
	int			joinmergedcols; /* number of merged (JOIN USING) columns */
	List	   *joinaliasvars;	/* list of alias-var expansions */
	List	   *joinleftcols;	/* left-side input column numbers */
	List	   *joinrightcols;	/* right-side input column numbers */

	/*
	 * join_using_alias is an alias clause attached directly to JOIN/USING. It
	 * is different from the alias field (below) in that it does not hide the
	 * range variables of the tables being joined.
	 */
	Alias	   *join_using_alias;

	/*
	 * Fields valid for a function RTE (else NIL/zero):
	 *
	 * When funcordinality is true, the eref->colnames list includes an alias
	 * for the ordinality column.  The ordinality column is otherwise
	 * implicit, and must be accounted for "by hand" in places such as
	 * expandRTE().
	 */
	List	   *functions;		/* list of RangeTblFunction nodes */
	bool		funcordinality; /* is this called WITH ORDINALITY? */

	/*
	 * Fields valid for a values RTE (else NIL):
	 */
	List	   *values_lists;	/* list of expression lists */

	/*
	 * Fields valid for VALUES and ENR RTEs (else NIL):
	 *
	 * For VALUES RTEs, storing these explicitly saves having to re-determine
	 * the info by scanning the values_lists. For ENRs, we store the types
	 * explicitly here (we could get the information from the catalogs if
	 * 'relid' was supplied, but we'd still need these for TupleDesc-based
	 * ENRs, so we might as well always store the type info here).
	 *
	 * For ENRs only, we have to consider the possibility of dropped columns.
	 * A dropped column is included in these lists, but it will have zeroes in
	 * all three lists (as well as an empty-string entry in eref).  Testing
	 * for zero coltype is the standard way to detect a dropped column.
	 */
	List	   *coltypes;		/* OID list of column type OIDs */
	List	   *coltypmods;		/* integer list of column typmods */
	List	   *colcollations;	/* OID list of column collation OIDs */

	/*
	 * Fields valid for ENR RTEs (else NULL/zero):
	 */
	char	   *enrname;		/* name of ephemeral named relation */
	double		enrtuples;		/* estimated or actual from caller */

	/*
	 * Fields valid in all RTEs:
	 */
	Alias	   *alias;			/* user-written alias clause, if any */
	Alias	   *eref;			/* expanded reference names */
	bool		lateral;		/* subquery, function, or values is LATERAL? */
	bool		inFromCl;		/* present in FROM clause? */
	Bitmapset  *selectedCols;	/* columns needing SELECT permission */
	Bitmapset  *insertedCols;	/* columns needing INSERT permission */
	Bitmapset  *updatedCols;	/* columns needing UPDATE permission */
	Bitmapset  *extraUpdatedCols;	/* generated columns being updated */
} RangeTblEntry;

/*
 * RangeTblFunction -
 *	  RangeTblEntry subsidiary data for one function in a FUNCTION RTE.
 *
 * If the function had a column definition list (required for an
 * otherwise-unspecified RECORD result), funccolnames lists the names given
 * in the definition list, funccoltypes lists their declared column types,
 * funccoltypmods lists their typmods, funccolcollations their collations.
 * Otherwise, those fields are NIL.
 *
 * Notice we don't attempt to store info about the results of functions
 * returning named composite types, because those can change from time to
 * time.  We do however remember how many columns we thought the type had
 * (including dropped columns!), so that we can successfully ignore any
 * columns added after the query was parsed.
 */
typedef struct RangeTblFunction
{
	NodeTag		type;

	Node	   *funcexpr;		/* expression tree for func call */
	int			funccolcount;	/* number of columns it contributes to RTE */
	/* These fields record the contents of a column definition list, if any: */
	List	   *funccolnames;	/* column names (list of String) */
	List	   *funccoltypes;	/* OID list of column type OIDs */
	List	   *funccoltypmods; /* integer list of column typmods */
	List	   *funccolcollations;	/* OID list of column collation OIDs */
	/* This is set during planning for use by the executor: */
	Bitmapset  *funcparams;		/* PARAM_EXEC Param IDs affecting this func */
} RangeTblFunction;

/*
 * TableSampleClause - TABLESAMPLE appearing in a transformed FROM clause
 *
 * Unlike RangeTableSample, this is a subnode of the relevant RangeTblEntry.
 */
typedef struct TableSampleClause
{
	NodeTag		type;
	Oid			tsmhandler;		/* OID of the tablesample handler function */
	List	   *args;			/* tablesample argument expression(s) */
	Expr	   *repeatable;		/* REPEATABLE expression, or NULL if none */
} TableSampleClause;

/*
 * WithCheckOption -
 *		representation of WITH CHECK OPTION checks to be applied to new tuples
 *		when inserting/updating an auto-updatable view, or RLS WITH CHECK
 *		policies to be applied when inserting/updating a relation with RLS.
 */
typedef enum WCOKind
{
	WCO_VIEW_CHECK,				/* WCO on an auto-updatable view */
	WCO_RLS_INSERT_CHECK,		/* RLS INSERT WITH CHECK policy */
	WCO_RLS_UPDATE_CHECK,		/* RLS UPDATE WITH CHECK policy */
	WCO_RLS_CONFLICT_CHECK		/* RLS ON CONFLICT DO UPDATE USING policy */
} WCOKind;

typedef struct WithCheckOption
{
	NodeTag		type;
	WCOKind		kind;			/* kind of WCO */
	char	   *relname;		/* name of relation that specified the WCO */
	char	   *polname;		/* name of RLS policy being checked */
	Node	   *qual;			/* constraint qual to check */
	bool		cascaded;		/* true for a cascaded WCO on a view */
} WithCheckOption;

/*
 * SortGroupClause -
 *		representation of ORDER BY, GROUP BY, PARTITION BY,
 *		DISTINCT, DISTINCT ON items
 *
 * You might think that ORDER BY is only interested in defining ordering,
 * and GROUP/DISTINCT are only interested in defining equality.  However,
 * one way to implement grouping is to sort and then apply a "uniq"-like
 * filter.  So it's also interesting to keep track of possible sort operators
 * for GROUP/DISTINCT, and in particular to try to sort for the grouping
 * in a way that will also yield a requested ORDER BY ordering.  So we need
 * to be able to compare ORDER BY and GROUP/DISTINCT lists, which motivates
 * the decision to give them the same representation.
 *
 * tleSortGroupRef must match ressortgroupref of exactly one entry of the
 *		query's targetlist; that is the expression to be sorted or grouped by.
 * eqop is the OID of the equality operator.
 * sortop is the OID of the ordering operator (a "<" or ">" operator),
 *		or InvalidOid if not available.
 * nulls_first means about what you'd expect.  If sortop is InvalidOid
 *		then nulls_first is meaningless and should be set to false.
 * hashable is true if eqop is hashable (note this condition also depends
 *		on the datatype of the input expression).
 *
 * In an ORDER BY item, all fields must be valid.  (The eqop isn't essential
 * here, but it's cheap to get it along with the sortop, and requiring it
 * to be valid eases comparisons to grouping items.)  Note that this isn't
 * actually enough information to determine an ordering: if the sortop is
 * collation-sensitive, a collation OID is needed too.  We don't store the
 * collation in SortGroupClause because it's not available at the time the
 * parser builds the SortGroupClause; instead, consult the exposed collation
 * of the referenced targetlist expression to find out what it is.
 *
 * In a grouping item, eqop must be valid.  If the eqop is a btree equality
 * operator, then sortop should be set to a compatible ordering operator.
 * We prefer to set eqop/sortop/nulls_first to match any ORDER BY item that
 * the query presents for the same tlist item.  If there is none, we just
 * use the default ordering op for the datatype.
 *
 * If the tlist item's type has a hash opclass but no btree opclass, then
 * we will set eqop to the hash equality operator, sortop to InvalidOid,
 * and nulls_first to false.  A grouping item of this kind can only be
 * implemented by hashing, and of course it'll never match an ORDER BY item.
 *
 * The hashable flag is provided since we generally have the requisite
 * information readily available when the SortGroupClause is constructed,
 * and it's relatively expensive to get it again later.  Note there is no
 * need for a "sortable" flag since OidIsValid(sortop) serves the purpose.
 *
 * A query might have both ORDER BY and DISTINCT (or DISTINCT ON) clauses.
 * In SELECT DISTINCT, the distinctClause list is as long or longer than the
 * sortClause list, while in SELECT DISTINCT ON it's typically shorter.
 * The two lists must match up to the end of the shorter one --- the parser
 * rearranges the distinctClause if necessary to make this true.  (This
 * restriction ensures that only one sort step is needed to both satisfy the
 * ORDER BY and set up for the Unique step.  This is semantically necessary
 * for DISTINCT ON, and presents no real drawback for DISTINCT.)
 */
typedef struct SortGroupClause
{
	NodeTag		type;
	Index		tleSortGroupRef;	/* reference into targetlist */
	Oid			eqop;			/* the equality operator ('=' op) */
	Oid			sortop;			/* the ordering operator ('<' op), or 0 */
	bool		nulls_first;	/* do NULLs come before normal values? */
	bool		hashable;		/* can eqop be implemented by hashing? */
} SortGroupClause;

/*
 * GroupingSet -
 *		representation of CUBE, ROLLUP and GROUPING SETS clauses
 *
 * In a Query with grouping sets, the groupClause contains a flat list of
 * SortGroupClause nodes for each distinct expression used.  The actual
 * structure of the GROUP BY clause is given by the groupingSets tree.
 *
 * In the raw parser output, GroupingSet nodes (of all types except SIMPLE
 * which is not used) are potentially mixed in with the expressions in the
 * groupClause of the SelectStmt.  (An expression can't contain a GroupingSet,
 * but a list may mix GroupingSet and expression nodes.)  At this stage, the
 * content of each node is a list of expressions, some of which may be RowExprs
 * which represent sublists rather than actual row constructors, and nested
 * GroupingSet nodes where legal in the grammar.  The structure directly
 * reflects the query syntax.
 *
 * In parse analysis, the transformed expressions are used to build the tlist
 * and groupClause list (of SortGroupClause nodes), and the groupingSets tree
 * is eventually reduced to a fixed format:
 *
 * EMPTY nodes represent (), and obviously have no content
 *
 * SIMPLE nodes represent a list of one or more expressions to be treated as an
 * atom by the enclosing structure; the content is an integer list of
 * ressortgroupref values (see SortGroupClause)
 *
 * CUBE and ROLLUP nodes contain a list of one or more SIMPLE nodes.
 *
 * SETS nodes contain a list of EMPTY, SIMPLE, CUBE or ROLLUP nodes, but after
 * parse analysis they cannot contain more SETS nodes; enough of the syntactic
 * transforms of the spec have been applied that we no longer have arbitrarily
 * deep nesting (though we still preserve the use of cube/rollup).
 *
 * Note that if the groupingSets tree contains no SIMPLE nodes (only EMPTY
 * nodes at the leaves), then the groupClause will be empty, but this is still
 * an aggregation query (similar to using aggs or HAVING without GROUP BY).
 *
 * As an example, the following clause:
 *
 * GROUP BY GROUPING SETS ((a,b), CUBE(c,(d,e)))
 *
 * looks like this after raw parsing:
 *
 * SETS( RowExpr(a,b) , CUBE( c, RowExpr(d,e) ) )
 *
 * and parse analysis converts it to:
 *
 * SETS( SIMPLE(1,2), CUBE( SIMPLE(3), SIMPLE(4,5) ) )
 */
typedef enum
{
	GROUPING_SET_EMPTY,
	GROUPING_SET_SIMPLE,
	GROUPING_SET_ROLLUP,
	GROUPING_SET_CUBE,
	GROUPING_SET_SETS
} GroupingSetKind;

typedef struct GroupingSet
{
	NodeTag		type;
	GroupingSetKind kind;
	List	   *content;
	int			location;
} GroupingSet;

/*
 * RowMarkClause -
 *	   parser output representation of FOR [KEY] UPDATE/SHARE clauses
 *
 * Query.rowMarks contains a separate RowMarkClause node for each relation
 * identified as a FOR [KEY] UPDATE/SHARE target.  If one of these clauses
 * is applied to a subquery, we generate RowMarkClauses for all normal and
 * subquery rels in the subquery, but they are marked pushedDown = true to
 * distinguish them from clauses that were explicitly written at this query
 * level.  Also, Query.hasForUpdate tells whether there were explicit FOR
 * UPDATE/SHARE/KEY SHARE clauses in the current query level.
 */
typedef struct RowMarkClause
{
	NodeTag		type;
	Index		rti;			/* range table index of target relation */
	LockClauseStrength strength;
	LockWaitPolicy waitPolicy;	/* NOWAIT and SKIP LOCKED */
	bool		pushedDown;		/* pushed down from higher query level? */
} RowMarkClause;

/*
 * InferClause -
 *		ON CONFLICT unique index inference clause
 *
 * Note: InferClause does not propagate into the Query representation.
 */
typedef struct InferClause
{
	NodeTag		type;
	List	   *indexElems;		/* IndexElems to infer unique index */
	Node	   *whereClause;	/* qualification (partial-index predicate) */
	char	   *conname;		/* Constraint name, or NULL if unnamed */
	int			location;		/* token location, or -1 if unknown */
} InferClause;

/*
 * OnConflictClause -
 *		representation of ON CONFLICT clause
 *
 * Note: OnConflictClause does not propagate into the Query representation.
 */
typedef struct OnConflictClause
{
	NodeTag		type;
	OnConflictAction action;	/* DO NOTHING or UPDATE? */
	InferClause *infer;			/* Optional index inference clause */
	List	   *targetList;		/* the target list (of ResTarget) */
	Node	   *whereClause;	/* qualifications */
	int			location;		/* token location, or -1 if unknown */
} OnConflictClause;

/*****************************************************************************
 *		Raw Grammar Output Statements
 *****************************************************************************/

/*
 *		RawStmt --- container for any one statement's raw parse tree
 *
 * Parse analysis converts a raw parse tree headed by a RawStmt node into
 * an analyzed statement headed by a Query node.  For optimizable statements,
 * the conversion is complex.  For utility statements, the parser usually just
 * transfers the raw parse tree (sans RawStmt) into the utilityStmt field of
 * the Query node, and all the useful work happens at execution time.
 *
 * stmt_location/stmt_len identify the portion of the source text string
 * containing this raw statement (useful for multi-statement strings).
 */
typedef struct RawStmt
{
	NodeTag		type;
	Node	   *stmt;			/* raw parse tree */
	int			stmt_location;	/* start location, or -1 if unknown */
	int			stmt_len;		/* length in bytes; 0 means "rest of string" */
} RawStmt;

/*****************************************************************************
 *		Optimizable Statements
 *****************************************************************************/

/* ----------------------
 *		Insert Statement
 *
 * The source expression is represented by SelectStmt for both the
 * SELECT and VALUES cases.  If selectStmt is NULL, then the query
 * is INSERT ... DEFAULT VALUES.
 * ----------------------
 */
typedef struct InsertStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* relation to insert into */
	List	   *cols;			/* optional: names of the target columns */
	Node	   *selectStmt;		/* the source SELECT/VALUES, or NULL */
	OnConflictClause *onConflictClause; /* ON CONFLICT clause */
	List	   *returningList;	/* list of expressions to return */
	OverridingKind override;	/* OVERRIDING clause */
} InsertStmt;

/* ----------------------
 *		Delete Statement
 * ----------------------
 */
typedef struct DeleteStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* relation to delete from */
	List	   *usingClause;	/* optional using clause for more tables */
	Node	   *whereClause;	/* qualifications */
	List	   *returningList;	/* list of expressions to return */
} DeleteStmt;

/* ----------------------
 *		Update Statement
 * ----------------------
 */
typedef struct UpdateStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* relation to update */
	List	   *targetList;		/* the target list (of ResTarget) */
	Node	   *whereClause;	/* qualifications */
	List	   *fromClause;		/* optional from clause for more tables */
	List	   *returningList;	/* list of expressions to return */
} UpdateStmt;

/* ----------------------
 *		Select Statement
 *
 * A "simple" SELECT is represented in the output of gram.y by a single
 * SelectStmt node; so is a VALUES construct.  A query containing set
 * operators (UNION, INTERSECT, EXCEPT) is represented by a tree of SelectStmt
 * nodes, in which the leaf nodes are component SELECTs and the internal nodes
 * represent UNION, INTERSECT, or EXCEPT operators.  Using the same node
 * type for both leaf and internal nodes allows gram.y to stick ORDER BY,
 * LIMIT, etc, clause values into a SELECT statement without worrying
 * whether it is a simple or compound SELECT.
 * ----------------------
 */
typedef struct SelectStmt
{
	NodeTag		type;

	/*
	 * These fields are used only in "leaf" SelectStmts.
	 */
	List	   *distinctClause; /* NULL, list of DISTINCT ON exprs, or
								 * lcons(NIL,NIL) for all (SELECT DISTINCT) */
	List	   *targetList;		/* the target list (of ResTarget) */
	List	   *fromClause;		/* the FROM clause */
	Node	   *whereClause;	/* WHERE qualification */
	List	   *groupClause;	/* GROUP BY clauses */
	bool		groupDistinct;	/* Is this GROUP BY DISTINCT? */
	Node	   *havingClause;	/* HAVING conditional-expression */

	/*
	 * In a "leaf" node representing a VALUES list, the above fields are all
	 * null, and instead this field is set.  Note that the elements of the
	 * sublists are just expressions, without ResTarget decoration. Also note
	 * that a list element can be DEFAULT (represented as a SetToDefault
	 * node), regardless of the context of the VALUES list. It's up to parse
	 * analysis to reject that where not valid.
	 */
	List	   *valuesLists;	/* untransformed list of expression lists */

	/*
	 * These fields are used in both "leaf" SelectStmts and upper-level
	 * SelectStmts.
	 */
	List	   *sortClause;		/* sort clause (a list of SortBy's) */
	Node	   *limitOffset;	/* # of result tuples to skip */
	Node	   *limitCount;		/* # of result tuples to return */
	LimitOption limitOption;	/* limit type */
	List	   *lockingClause;	/* FOR UPDATE (list of LockingClause's) */

	/* Eventually add fields for CORRESPONDING spec here */
} SelectStmt;


/*****************************************************************************
 *		Other Statements (no optimizations required)
 *
 *		These are not touched by parser/analyze.c except to put them into
 *		the utilityStmt field of a Query.  This is eventually passed to
 *		ProcessUtility (by-passing rewriting and planning).  Some of the
 *		statements do need attention from parse analysis, and this is
 *		done by routines in parser/parse_utilcmd.c after ProcessUtility
 *		receives the command for execution.
 *		EXPLAIN and CREATE TABLE AS are special cases:
 *		they contain optimizable statements, which get processed normally
 *		by parser/analyze.c.
 *****************************************************************************/

/*
 * When a command can act on several kinds of objects with only one
 * parse structure required, use these constants to designate the
 * object type.  Note that commands typically don't support all the types.
 */

typedef enum ObjectType
{
	OBJECT_ACCESS_METHOD,
	OBJECT_AMOP,
	OBJECT_AMPROC,
	OBJECT_ATTRIBUTE,			/* type's attribute, when distinct from column */
	OBJECT_CAST,
	OBJECT_COLUMN,
	OBJECT_COLLATION,
	OBJECT_DATABASE,
	OBJECT_DEFAULT,
	OBJECT_EXTENSION,
	OBJECT_FUNCTION,
	OBJECT_INDEX,
	OBJECT_LANGUAGE,
	OBJECT_OPCLASS,
	OBJECT_OPERATOR,
	OBJECT_OPFAMILY,
	OBJECT_PROCEDURE,
	OBJECT_PUBLICATION,
	OBJECT_PUBLICATION_REL,
	OBJECT_ROUTINE,
	OBJECT_RULE,
	OBJECT_SCHEMA,
	OBJECT_TABCONSTRAINT,
	OBJECT_TABLE,
	OBJECT_TYPE,
	OBJECT_VIEW
} ObjectType;

/* ----------------------
 *		Create Schema Statement
 *
 * NOTE: the schemaElts list contains raw parsetrees for component statements
 * of the schema, such as CREATE TABLE, GRANT, etc.  These are analyzed and
 * executed after the schema itself is created.
 * ----------------------
 */
typedef struct CreateSchemaStmt
{
	NodeTag		type;
	char	   *schemaname;		/* the name of the schema to create */
	List	   *schemaElts;		/* schema components (list of parsenodes) */
	bool		if_not_exists;	/* just do nothing if schema already exists? */
} CreateSchemaStmt;

typedef enum DropBehavior
{
	DROP_RESTRICT,				/* drop fails if any dependent objects */
	DROP_CASCADE				/* remove dependent objects too */
} DropBehavior;

/* ----------------------
 *	Alter Table
 * ----------------------
 */
typedef struct AlterTableStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* table to work on */
	List	   *cmds;			/* list of subcommands */
	ObjectType	objtype;		/* type of object */
	bool		missing_ok;		/* skip error if table missing */
} AlterTableStmt;


typedef enum AlterTableType
{
	AT_AddColumn,				/* add column */
	AT_AddColumnRecurse,		/* internal to commands/tablecmds.c */
	AT_AddColumnToView,			/* implicitly via CREATE OR REPLACE VIEW */
	AT_ColumnDefault,			/* alter column default */
	AT_CookedColumnDefault,		/* add a pre-cooked column default */
	AT_DropNotNull,				/* alter column drop not null */
	AT_SetNotNull,				/* alter column set not null */
	AT_DropExpression,			/* alter column drop expression */
	AT_CheckNotNull,			/* check column is already marked not null */
	AT_SetStatistics,			/* alter column set statistics */
	AT_SetOptions,				/* alter column set ( options ) */
	AT_ResetOptions,			/* alter column reset ( options ) */
	AT_SetStorage,				/* alter column set storage */
	AT_SetCompression,			/* alter column set compression */
	AT_DropColumn,				/* drop column */
	AT_DropColumnRecurse,		/* internal to commands/tablecmds.c */
	AT_AddIndex,				/* add index */
	AT_ReAddIndex,				/* internal to commands/tablecmds.c */
	AT_AddConstraint,			/* add constraint */
	AT_AddConstraintRecurse,	/* internal to commands/tablecmds.c */
	AT_ReAddConstraint,			/* internal to commands/tablecmds.c */
	AT_AlterConstraint,			/* alter constraint */
	AT_ValidateConstraint,		/* validate constraint */
	AT_ValidateConstraintRecurse,	/* internal to commands/tablecmds.c */
	AT_AddIndexConstraint,		/* add constraint using existing index */
	AT_DropConstraint,			/* drop constraint */
	AT_DropConstraintRecurse,	/* internal to commands/tablecmds.c */
	AT_AlterColumnType,			/* alter column type */
	AT_AlterColumnGenericOptions,	/* alter column OPTIONS (...) */
	AT_ChangeOwner,				/* change owner */
	AT_ClusterOn,				/* CLUSTER ON */
	AT_DropCluster,				/* SET WITHOUT CLUSTER */
	AT_DropOids,				/* SET WITHOUT OIDS */
	AT_SetRelOptions,			/* SET (...) -- AM specific parameters */
	AT_ResetRelOptions,			/* RESET (...) -- AM specific parameters */
	AT_ReplaceRelOptions,		/* replace reloption list in its entirety */
	AT_EnableRule,				/* ENABLE RULE name */
	AT_EnableAlwaysRule,		/* ENABLE ALWAYS RULE name */
	AT_EnableReplicaRule,		/* ENABLE REPLICA RULE name */
	AT_DisableRule,				/* DISABLE RULE name */
	AT_GenericOptions,			/* OPTIONS (...) */
	AT_AddIdentity,				/* ADD IDENTITY */
	AT_SetIdentity,				/* SET identity column options */
	AT_DropIdentity,			/* DROP IDENTITY */
	AT_ReAddStatistics			/* internal to commands/tablecmds.c */
} AlterTableType;

typedef struct AlterTableCmd	/* one subcommand of an ALTER TABLE */
{
	NodeTag		type;
	AlterTableType subtype;		/* Type of table alteration to apply */
	char	   *name;			/* column, constraint, or trigger to act on,
								 * or tablespace */
	int16		num;			/* attribute number for columns referenced by
								 * number */
	RoleSpec   *newowner;
	Node	   *def;			/* definition of new column, index,
								 * constraint, or parent table */
	DropBehavior behavior;		/* RESTRICT or CASCADE for DROP cases */
	bool		missing_ok;		/* skip error if missing? */
	bool		recurse;		/* exec-time recursion */
} AlterTableCmd;


/* ----------------------
 *	Alter Table
 *
 * The fields are used in different ways by the different variants of
 * this command.
 */


/*
 * ObjectWithArgs represents an operator name plus parameter identification.
 *
 * objargs includes only the types of the input parameters of the object.
 * In some contexts, that will be all we have, and it's enough to look up
 * objects according to the traditional Postgres rules (i.e., when only input
 * arguments matter).
 *
 * Some grammar productions can set args_unspecified = true instead of
 * providing parameter info.  In this case, lookup will succeed only if
 * the object name is unique.  Note that otherwise, NIL parameter lists
 * mean zero arguments.
 */
typedef struct ObjectWithArgs
{
	NodeTag		type;
	List	   *objname;		/* qualified name of function/operator */
	List	   *objargs;		/* list of Typename nodes (input args only) */
	bool		args_unspecified;	/* argument list was omitted? */
} ObjectWithArgs;

/* ----------------------
 * SET Statement (includes RESET)
 *
 * "SET var TO DEFAULT" and "RESET var" are semantically equivalent, but we
 * preserve the distinction in VariableSetKind for CreateCommandTag().
 * ----------------------
 */
typedef enum
{
	VAR_SET_VALUE,				/* SET var = value */
	VAR_SET_DEFAULT,			/* SET var TO DEFAULT */
	VAR_SET_CURRENT,			/* SET var FROM CURRENT */
	VAR_SET_MULTI,				/* special case for SET TRANSACTION ... */
	VAR_RESET,					/* RESET var */
	VAR_RESET_ALL				/* RESET ALL */
} VariableSetKind;

typedef struct VariableSetStmt
{
	NodeTag		type;
	VariableSetKind kind;
	char	   *name;			/* variable to be set */
	List	   *args;			/* List of A_Const nodes */
	bool		is_local;		/* SET LOCAL? */
} VariableSetStmt;

/* ----------------------
 * Show Statement
 * ----------------------
 */
typedef struct VariableShowStmt
{
	NodeTag		type;
	char	   *name;
} VariableShowStmt;

/* ----------------------
 *		Create Table Statement
 *
 * NOTE: in the raw gram.y output, ColumnDef and Constraint nodes are
 * intermixed in tableElts, and constraints is NIL.  After parse analysis,
 * tableElts contains just ColumnDefs, and constraints contains just
 * Constraint nodes (in fact, only CONSTR_CHECK nodes, in the present
 * implementation).
 * ----------------------
 */

typedef struct CreateStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* relation to create */
	List	   *tableElts;		/* column definitions (list of ColumnDef) */
	List	   *constraints;	/* constraints (list of Constraint nodes) */
	OnCommitAction oncommit;	/* what do we do at COMMIT? */
	bool		if_not_exists;	/* just do nothing if it already exists? */
} CreateStmt;

/* ----------
 * Definitions for constraints in CreateStmt
 *
 * Note that column defaults are treated as a type of constraint,
 * even though that's a bit odd semantically.
 *
 * For constraints that use expressions (CONSTR_CHECK, CONSTR_DEFAULT)
 * we may have the expression in either "raw" form (an untransformed
 * parse tree) or "cooked" form (the nodeToString representation of
 * an executable expression tree), depending on how this Constraint
 * node was created (by parsing, or by inheritance from an existing
 * relation).  We should never have both in the same node!
 * ----------
 */

typedef enum ConstrType			/* types of constraints */
{
	CONSTR_NULL,				/* not standard SQL, but a lot of people
								 * expect it */
	CONSTR_NOTNULL,
	CONSTR_DEFAULT,
	CONSTR_CHECK,
	CONSTR_PRIMARY,
	CONSTR_UNIQUE
} ConstrType;

typedef struct Constraint
{
	NodeTag		type;
	ConstrType	contype;		/* see above */

	/* Fields used for most/all constraint types: */
	char	   *conname;		/* Constraint name, or NULL if unnamed */
	int			location;		/* token location, or -1 if unknown */

	/* Fields used for constraints with expressions (CHECK and DEFAULT): */
	bool		is_no_inherit;	/* is constraint non-inheritable? */
	Node	   *raw_expr;		/* expr, as untransformed parse tree */
	char	   *cooked_expr;	/* expr, as nodeToString representation */
	char		generated_when; /* ALWAYS or BY DEFAULT */

	/* Fields used for unique constraints (UNIQUE and PRIMARY KEY): */
	List	   *keys;			/* String nodes naming referenced key
								 * column(s) */
	List	   *including;		/* String nodes naming referenced nonkey
								 * column(s) */

	/* Fields used for index constraints (UNIQUE, PRIMARY KEY): */
	List	   *options;		/* options from WITH clause */
	char	   *indexname;		/* existing index to use; otherwise NULL */
	/* These could be, but currently are not, used for UNIQUE/PKEY: */
	char	   *access_method;	/* index access method; NULL for default */
	Node	   *where_clause;	/* partial index predicate */

	/* Fields used for constraints that allow a NOT VALID specification */
	bool		skip_validation;	/* skip validation of existing rows? */
	bool		initially_valid;	/* mark the new constraint as valid? */
} Constraint;

/* ----------------------
 *		Create/Alter Extension Statements
 * ----------------------
 */

typedef struct CreateExtensionStmt
{
	NodeTag		type;
	char	   *extname;
	bool		if_not_exists;	/* just do nothing if it already exists? */
	List	   *options;		/* List of DefElem nodes */
} CreateExtensionStmt;

/* ----------------------
 *		Drop Table|Sequence|View|Index|Type|Domain|Conversion|Schema Statement
 * ----------------------
 */

typedef struct DropStmt
{
	NodeTag		type;
	List	   *objects;		/* list of names */
	ObjectType	removeType;		/* object type */
	DropBehavior behavior;		/* RESTRICT or CASCADE behavior */
	bool		missing_ok;		/* skip error if object is missing? */
	bool		concurrent;		/* drop index concurrently? */
} DropStmt;

/* ----------------------
 *				Truncate Table Statement
 * ----------------------
 */
typedef struct TruncateStmt
{
	NodeTag		type;
	List	   *relations;		/* relations (RangeVars) to be truncated */
	bool		restart_seqs;	/* restart owned sequences? */
	DropBehavior behavior;		/* RESTRICT or CASCADE behavior */
} TruncateStmt;

/* ----------------------
 *		Planner-control flags
 *
 * These option bits are passed down to the planner.  They do not
 * correspond to any SQL grammar.
 * ----------------------
 */
#define CURSOR_OPT_GENERIC_PLAN 0x0200	/* force use of generic plan */
#define CURSOR_OPT_CUSTOM_PLAN	0x0400	/* force use of custom plan */
#define CURSOR_OPT_PARALLEL_OK	0x0800	/* parallel mode OK */

/* ----------------------
 *		Create Index Statement
 *
 * This represents creation of an index and/or an associated constraint.
 * If isconstraint is true, we should create a pg_constraint entry along
 * with the index.  But if indexOid isn't InvalidOid, we are not creating an
 * index, just a UNIQUE/PKEY constraint using an existing index.  isconstraint
 * must always be true in this case, and the fields describing the index
 * properties are empty.
 * ----------------------
 */
typedef struct IndexStmt
{
	NodeTag		type;
	char	   *idxname;		/* name of new index, or NULL for default */
	RangeVar   *relation;		/* relation to build index on */
	char	   *accessMethod;	/* name of access method (eg. btree) */
	List	   *indexParams;	/* columns to index: a list of IndexElem */
	List	   *indexIncludingParams;	/* additional columns to index: a list
										 * of IndexElem */
	List	   *options;		/* WITH clause options: a list of DefElem */
	Node	   *whereClause;	/* qualification (partial-index predicate) */
	Oid			indexOid;		/* OID of an existing index, if any */
	Oid			oldNode;		/* relfilenode of existing storage, if any */
	SubTransactionId oldCreateSubid;	/* rd_createSubid of oldNode */
	SubTransactionId oldFirstRelfilenodeSubid;	/* rd_firstRelfilenodeSubid of
												 * oldNode */
	bool		unique;			/* is index unique? */
	bool		primary;		/* is index a primary key? */
	bool		isconstraint;	/* is it for a pkey/unique constraint? */
	bool		transformed;	/* true when transformIndexStmt is finished */
	bool		concurrent;		/* should this be a concurrent index build? */
	bool		if_not_exists;	/* just do nothing if index already exists? */
} IndexStmt;


typedef enum FunctionParameterMode
{
	/* the assigned enum values appear in pg_proc, don't change 'em! */
	FUNC_PARAM_IN = 'i',		/* input only */
	FUNC_PARAM_OUT = 'o',		/* output only */
	FUNC_PARAM_INOUT = 'b',		/* both */
	FUNC_PARAM_VARIADIC = 'v',	/* variadic (always input) */
	FUNC_PARAM_TABLE = 't',		/* table function output column */
	/* this is not used in pg_proc: */
	FUNC_PARAM_DEFAULT = 'd'	/* default; effectively same as IN */
} FunctionParameterMode;

/* ----------------------
 * ALTER object DEPENDS ON EXTENSION extname
 * ----------------------
 */
/* ----------------------
 *		ALTER object SET SCHEMA Statement
 * ----------------------
 */
typedef struct AlterObjectSchemaStmt
{
	NodeTag		type;
	ObjectType	objectType;		/* OBJECT_TABLE, OBJECT_TYPE, etc */
	RangeVar   *relation;		/* in case it's a table */
	Node	   *object;			/* in case it's some other object */
	char	   *newschema;		/* the new schema */
	bool		missing_ok;		/* skip error if missing? */
} AlterObjectSchemaStmt;

/* ----------------------
 *		Create Rule Statement
 * ----------------------
 */
typedef struct RuleStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* relation the rule is for */
	char	   *rulename;		/* name of the rule */
	Node	   *whereClause;	/* qualifications */
	CmdType		event;			/* SELECT, INSERT, etc */
	bool		instead;		/* is a 'do instead'? */
	List	   *actions;		/* the action statements */
	bool		replace;		/* OR REPLACE */
} RuleStmt;

/* ----------------------
 *		{Begin|Commit|Rollback} Transaction Statement
 * ----------------------
 */
typedef enum TransactionStmtKind
{
	TRANS_STMT_BEGIN,
	TRANS_STMT_START,			/* semantically identical to BEGIN */
	TRANS_STMT_COMMIT,
	TRANS_STMT_ROLLBACK,
	TRANS_STMT_SAVEPOINT,
	TRANS_STMT_RELEASE,
	TRANS_STMT_ROLLBACK_TO,
	TRANS_STMT_PREPARE,
	TRANS_STMT_COMMIT_PREPARED,
	TRANS_STMT_ROLLBACK_PREPARED
} TransactionStmtKind;

typedef struct TransactionStmt
{
	NodeTag		type;
	TransactionStmtKind kind;	/* see above */
	List	   *options;		/* for BEGIN/START commands */
	char	   *savepoint_name; /* for savepoint commands */
	char	   *gid;			/* for two-phase-commit related commands */
	bool		chain;			/* AND CHAIN option */
} TransactionStmt;

/* ----------------------
 *		Create View Statement
 * ----------------------
 */
typedef enum ViewCheckOption
{
	NO_CHECK_OPTION,
	LOCAL_CHECK_OPTION,
	CASCADED_CHECK_OPTION
} ViewCheckOption;

typedef struct ViewStmt
{
	NodeTag		type;
	RangeVar   *view;			/* the view to be created */
	List	   *aliases;		/* target column names */
	Node	   *query;			/* the SELECT query (as a raw parse tree) */
	bool		replace;		/* replace an existing view? */
	List	   *options;		/* options from WITH clause */
	ViewCheckOption withCheckOption;	/* WITH CHECK OPTION */
} ViewStmt;

/* ----------------------
 *		Createdb Statement
 * ----------------------
 */
typedef struct CreatedbStmt
{
	NodeTag		type;
	char	   *dbname;			/* name of database to create */
	List	   *options;		/* List of DefElem nodes */
} CreatedbStmt;

/* ----------------------
 *		Dropdb Statement
 * ----------------------
 */
typedef struct DropdbStmt
{
	NodeTag		type;
	char	   *dbname;			/* database to drop */
	bool		missing_ok;		/* skip error if db is missing? */
	List	   *options;		/* currently only FORCE is supported */
} DropdbStmt;

/* ----------------------
 *		Cluster Statement (support pbrown's cluster index implementation)
 * ----------------------
 */
typedef struct ClusterStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* relation being indexed, or NULL if all */
	char	   *indexname;		/* original index defined */
	List	   *params;			/* list of DefElem nodes */
} ClusterStmt;

/* ----------------------
 *		Vacuum and Analyze Statements
 *
 * Even though these are nominally two statements, it's convenient to use
 * just one node type for both.
 * ----------------------
 */
typedef struct VacuumStmt
{
	NodeTag		type;
	List	   *options;		/* list of DefElem nodes */
	List	   *rels;			/* list of VacuumRelation, or NIL for all */
	bool		is_vacuumcmd;	/* true for VACUUM, false for ANALYZE */
} VacuumStmt;

/*
 * Info about a single target table of VACUUM/ANALYZE.
 *
 * If the OID field is set, it always identifies the table to process.
 * Then the relation field can be NULL; if it isn't, it's used only to report
 * failure to open/lock the relation.
 */
typedef struct VacuumRelation
{
	NodeTag		type;
	RangeVar   *relation;		/* table name to process, or NULL */
	Oid			oid;			/* table's OID; InvalidOid if not looked up */
	List	   *va_cols;		/* list of column names, or NIL for all */
} VacuumRelation;

/* ----------------------
 *		Explain Statement
 *
 * The "query" field is initially a raw parse tree, and is converted to a
 * Query node during parse analysis.  Note that rewriting and planning
 * of the query are always postponed until execution.
 * ----------------------
 */
typedef struct ExplainStmt
{
	NodeTag		type;
	Node	   *query;			/* the query (see comments above) */
	List	   *options;		/* list of DefElem nodes */
} ExplainStmt;

/* ----------------------
 * Checkpoint Statement
 * ----------------------
 */
typedef struct CheckPointStmt
{
	NodeTag		type;
} CheckPointStmt;

/* ----------------------
 * Discard Statement
 * ----------------------
 */

typedef enum DiscardMode
{
	DISCARD_ALL,
	DISCARD_PLANS,
	DISCARD_TEMP
} DiscardMode;

typedef struct DiscardStmt
{
	NodeTag		type;
	DiscardMode target;
} DiscardStmt;

/* ----------------------
 *		REINDEX Statement
 * ----------------------
 */
typedef enum ReindexObjectType
{
	REINDEX_OBJECT_INDEX,		/* index */
	REINDEX_OBJECT_TABLE,		/* table or materialized view */
	REINDEX_OBJECT_SCHEMA,		/* schema */
	REINDEX_OBJECT_SYSTEM,		/* system catalogs */
	REINDEX_OBJECT_DATABASE		/* database */
} ReindexObjectType;

typedef struct ReindexStmt
{
	NodeTag		type;
	ReindexObjectType kind;		/* REINDEX_OBJECT_INDEX, REINDEX_OBJECT_TABLE,
								 * etc. */
	RangeVar   *relation;		/* Table or index to reindex */
	const char *name;			/* name of database to reindex */
	List	   *params;			/* list of DefElem nodes */
} ReindexStmt;


#endif						/* PARSENODES_H */
