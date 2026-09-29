/*-------------------------------------------------------------------------
 *
 * pathnodes.h
 *	  Definitions for planner's internal data structures, especially Paths.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/pathnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PATHNODES_H
#define PATHNODES_H

#include "access/sdir.h"
#include "lib/stringinfo.h"
#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "storage/block.h"

/*
 * Relids
 *		Set of relation identifiers (indexes into the rangetable).
 */
typedef Bitmapset *Relids;

/*
 * When looking for a "cheapest path", this enum specifies whether we want
 * cheapest startup cost or cheapest total cost.
 */
typedef enum CostSelector
{
	STARTUP_COST, TOTAL_COST
} CostSelector;

/*
 * The cost estimate produced by cost_qual_eval() includes both a one-time
 * (startup) cost, and a per-tuple cost.
 */
typedef struct QualCost
{
	Cost		startup;		/* one-time cost */
	Cost		per_tuple;		/* per-evaluation cost */
} QualCost;

/*
 * This enum identifies the different types of "upper" (post-scan/join)
 * relations that we might deal with during planning.
 */
typedef enum UpperRelationKind
{
	UPPERREL_DISTINCT,			/* result of "SELECT DISTINCT", if any */
	UPPERREL_ORDERED,			/* result of ORDER BY, if any */
	UPPERREL_FINAL				/* result of any remaining top-level actions */
	/* NB: UPPERREL_FINAL must be last enum entry; it's used to size arrays */
} UpperRelationKind;

/*----------
 * PlannerGlobal
 *		用于规划/优化的全局信息
 *
 * PlannerGlobal 保存一次完整规划器调用（planner invocation）的状态；该状态在被
 * 规划的命令中存在的所有层级的子查询（sub-Query）之间共享。
 *----------
 */
typedef struct PlannerGlobal
{
	NodeTag		type;

	/* Param values provided to standard_planner() */
	ParamListInfo boundParams;

	List	   *subplans;		/* Plans for SubPlan nodes */

	List	   *subroots;		/* PlannerInfos for SubPlan nodes */

	Bitmapset  *rewindPlanIDs;	/* indices of subplans that require REWIND */

	List	   *finalrtable;	/* "flat" rangetable for executor */

	List	   *finalrowmarks;	/* "flat" list of PlanRowMarks */

	List	   *resultRelations;	/* "flat" list of integer RT indexes */

	List	   *appendRelations;	/* "flat" list of AppendRelInfos */

	List	   *relationOids;	/* OIDs of relations the plan depends on */

	List	   *invalItems;		/* other dependencies, as PlanInvalItems */

	List	   *paramExecTypes; /* type OIDs for PARAM_EXEC Params */

	Index		lastPHId;		/* highest PlaceHolderVar ID assigned */

	Index		lastRowMarkId;	/* highest PlanRowMark ID assigned */

	int			lastPlanNodeId; /* highest plan node ID assigned */

	bool		transientPlan;	/* redo plan when TransactionXmin changes? */

	bool		dependsOnRole;	/* is plan specific to current role? */

} PlannerGlobal;

/* macro for fetching the Plan associated with a SubPlan node */
#define planner_subplan_get_plan(root, subplan) \
	((Plan *) list_nth((root)->glob->subplans, (subplan)->plan_id - 1))


/*----------
 * PlannerInfo
 *		Per-query information for planning/optimization
 *
 * This struct is conventionally called "root" in all the planner routines.
 * It holds links to all of the planner's working state, in addition to the
 * original Query.  Note that at present the planner extensively modifies
 * the passed-in Query data structure; someday that should stop.
 *
 * For reasons explained in optimizer/optimizer.h, we define the typedef
 * either here or in that header, whichever is read first.
 *----------
 */
#ifndef HAVE_PLANNERINFO_TYPEDEF
typedef struct PlannerInfo PlannerInfo;
#define HAVE_PLANNERINFO_TYPEDEF 1
#endif

struct PlannerInfo
{
	NodeTag		type;

	Query	   *parse;			/* the Query being planned */

	PlannerGlobal *glob;		/* global info for current planner run */

	Index		query_level;	/* 1 at the outermost Query */

	PlannerInfo *parent_root;	/* NULL at outermost Query */

	/*
	 * plan_params contains the expressions that this query level needs to
	 * make available to a lower query level that is currently being planned.
	 * outer_params contains the paramIds of PARAM_EXEC Params that outer
	 * query levels will make available to this query level.
	 */
	List	   *plan_params;	/* list of PlannerParamItems, see below */
	Bitmapset  *outer_params;

	/*
	 * simple_rel_array holds pointers to "base rels" and "other rels" (see
	 * comments for RelOptInfo for more info).  It is indexed by rangetable
	 * index (so entry 0 is always wasted).  Entries can be NULL when an RTE
	 * does not correspond to a base relation, such as a join RTE or an
	 * unreferenced view RTE; or if the RelOptInfo hasn't been made yet.
	 */
	struct RelOptInfo **simple_rel_array;	/* All 1-rel RelOptInfos */
	int			simple_rel_array_size;	/* allocated size of array */

	/*
	 * simple_rte_array is the same length as simple_rel_array and holds
	 * pointers to the associated rangetable entries.  Using this is a shade
	 * faster than using rt_fetch(), mostly due to fewer indirections.
	 */
	RangeTblEntry **simple_rte_array;	/* rangetable as an array */

	/*
	 * append_rel_array is the same length as the above arrays, and holds
	 * pointers to the corresponding AppendRelInfo entry indexed by
	 * child_relid, or NULL if the rel is not an appendrel child.  The array
	 * itself is not allocated if append_rel_list is empty.
	 */
	struct AppendRelInfo **append_rel_array;

	/*
	 * all_baserels is a Relids set of all base relids (but not "other"
	 * relids) in the query; that is, the Relids identifier of the final join
	 * we need to form.  This is computed in make_one_rel, just before we
	 * start making Paths.
	 */
	Relids		all_baserels;

	/*
	 * nullable_baserels is a Relids set of base relids that are nullable by
	 * some outer join in the jointree; these are rels that are potentially
	 * nullable below the WHERE clause, SELECT targetlist, etc.  This is
	 * computed in deconstruct_jointree.
	 */
	Relids		nullable_baserels;

	/*
	 * join_rel_list is a list of all join-relation RelOptInfos we have
	 * considered in this planning run.  For small problems we just scan the
	 * list to do lookups, but when there are many join relations we build a
	 * hash table for faster lookups.  The hash table is present and valid
	 * when join_rel_hash is not NULL.  Note that we still maintain the list
	 * even when using the hash table for lookups; this simplifies life for
	 * join-search plugins.
	 */
	List	   *join_rel_list;	/* list of join-relation RelOptInfos */
	struct HTAB *join_rel_hash; /* optional hashtable for join relations */

	/*
	 * When doing a dynamic-programming-style join search, join_rel_level[k]
	 * is a list of all join-relation RelOptInfos of level k, and
	 * join_cur_level is the current level.  New join-relation RelOptInfos are
	 * automatically added to the join_rel_level[join_cur_level] list.
	 * join_rel_level is NULL if not in use.
	 */
	List	  **join_rel_level; /* lists of join-relation RelOptInfos */
	int			join_cur_level; /* index of list being extended */

	List	   *init_plans;		/* init SubPlans for query */

	List	   *eq_classes;		/* list of active EquivalenceClasses */

	bool		ec_merging_done;	/* set true once ECs are canonical */

	List	   *canon_pathkeys; /* list of "canonical" PathKeys */

	List	   *left_join_clauses;	/* list of RestrictInfos for mergejoinable
									 * outer join clauses w/nonnullable var on
									 * left */

	List	   *right_join_clauses; /* list of RestrictInfos for mergejoinable
									 * outer join clauses w/nonnullable var on
									 * right */

	List	   *full_join_clauses;	/* list of RestrictInfos for mergejoinable
									 * full join clauses */

	List	   *join_info_list; /* list of SpecialJoinInfos */

	/*
	 * all_result_relids is empty for SELECT, otherwise it contains at least
	 * parse->resultRelation.  For UPDATE/DELETE across an inheritance tree,
	 * the result rel's child relids are added.
	 * leaf_result_relids is similar except that only actual result tables
	 * are included in it.
	 */
	Relids		all_result_relids;	/* set of all result relids */
	Relids		leaf_result_relids; /* set of all leaf relids */

	List	   *append_rel_list;	/* list of AppendRelInfos */

	List	   *row_identity_vars;	/* list of RowIdentityVarInfos */

	List	   *rowMarks;		/* list of PlanRowMarks */

	List	   *placeholder_list;	/* list of PlaceHolderInfos */

	List	   *query_pathkeys; /* desired pathkeys for query_planner() */

	List	   *distinct_pathkeys;	/* distinctClause pathkeys, if any */
	List	   *sort_pathkeys;	/* sortClause pathkeys, if any */

	List	   *initial_rels;	/* RelOptInfos we are now trying to join */

	/* Use fetch_upper_rel() to get any particular upper rel */
	List	   *upper_rels[UPPERREL_FINAL + 1]; /* upper-rel RelOptInfos */

	/* Result tlists chosen by upper_planner for upper-stage processing */
	struct PathTarget *upper_targets[UPPERREL_FINAL + 1];

	/*
	 * The fully-processed targetlist is kept here.  It differs from
	 * parse->targetList in that (for INSERT) it's been reordered to match the
	 * target table, and defaults have been filled in.  Also, additional
	 * resjunk targets may be present.  preprocess_targetlist() does most of
	 * that work, but note that more resjunk targets can get added during
	 * appendrel expansion.  (Hence, upper_targets mustn't get set up till
	 * after that.)
	 */
	List	   *processed_tlist;

	/*
	 * For UPDATE, this list contains the target table's attribute numbers to
	 * which the first N entries of processed_tlist are to be assigned.  (Any
	 * additional entries in processed_tlist must be resjunk.)  DO NOT use the
	 * resnos in processed_tlist to identify the UPDATE target columns.
	 */
	List	   *update_colnos;

	/* Fields filled during create_plan() for use in setrefs.c */

	MemoryContext planner_cxt;	/* context holding PlannerInfo */

	double		total_table_pages;	/* # of pages in all non-dummy tables of
									 * query */

	double		tuple_fraction; /* tuple_fraction passed to query_planner */

	Index		qual_security_level;	/* minimum security_level for quals */
	/* Note: qual_security_level is zero if there are no securityQuals */

	bool		hasJoinRTEs;	/* true if any RTEs are RTE_JOIN kind */
	bool		hasLateralRTEs; /* true if any RTEs are marked LATERAL */
	bool		hasPseudoConstantQuals; /* true if any RestrictInfo has
									 * pseudoconstant = true */

	/* These fields are workspace for createplan.c */
	Relids		curOuterRels;	/* outer rels above current node */
	List	   *curOuterParams; /* not-yet-assigned NestLoopParams */

	/* optional private data for join_search_hook */
	void	   *join_search_private;
};


/*
 * In places where it's known that simple_rte_array[] must have been prepared
 * already, we just index into it to fetch RTEs.  In code that might be
 * executed before or after entering query_planner(), use this macro.
 */
#define planner_rt_fetch(rti, root) \
	((root)->simple_rte_array ? (root)->simple_rte_array[rti] : \
	 rt_fetch(rti, (root)->parse->rtable))

/*----------
 * RelOptInfo
 *		用于规划/优化的按关系（per-relation）信息
 *
 * 就规划而言，一个"基关系"（base rel）要么是一个普通关系（表），要么是出现在
 * 范围表（range table）中的某个子 SELECT 或函数的输出。无论哪种情况，它都由一个
 * RT 索引唯一标识。一个"连接关系"（joinrel）是两个或更多基关系的连接。一个
 * joinrel 由组成它的各基关系的 RT 索引集合来标识。我们为每个基关系和 joinrel
 * 创建 RelOptInfo 节点，并分别存储在 PlannerInfo 的 simple_rel_array 和
 * join_rel_list 中。
 *
 * 注意，对于任意给定的组成基关系集合，无论我们以何种顺序组装它们，都只有一个
 * joinrel；因此，用无序集合来标识它是正确的数据类型。
 *
 * 我们还有"其他关系"（other rels），它们与基关系类似，都只引用单个 RT 索引；
 * 但它们不属于连接树的一部分，并且被赋予不同的 RelOptKind 来标识。
 * 目前，唯一一种 otherrel 是为"追加关系"（append relation）的成员关系创建的，
 * 即继承集（inheritance set）或 UNION ALL 子查询。一个追加关系有一个父 RTE，
 * 它是一个基关系，代表整个追加关系。成员 RTE 则是 otherrel。父关系出现在查询
 * 连接树中，而成员则不然。成员 RTE 和 otherrel 用于规划追加集中各个表或子查询的
 * 扫描；随后，父基关系会获得由各成员关系的最佳路径组成的 Append 路径。（更多
 * 信息参见 AppendRelInfo 的注释。）
 *
 * 曾经我们也创建 otherrel 来表示 join RTE，用于处理连接别名 Var。目前不再需要
 * 这样做，因为所有连接别名 Var 都在 preprocess_expression 期间被展开为非别名
 * 形式。
 *
 * 还有一种 RelOptKind 用于"上层"（upper）关系，这些 RelOptInfo 描述的是扫描/连接
 * 之后的处理步骤，例如聚合。这些 RelOptInfo 中的许多字段是无意义的，但它们的
 * Path 字段始终保存着表示执行该处理步骤的各种方式的 Path。
 *
 * 最后，还有一种 RelOptKind 用于"死亡"（dead）关系，即我们已证明最终无需连接的
 * 那些基关系。
 *
 * 本数据结构的某些部分是针对各种扫描和连接机制的。为它们创建新的节点类型似乎
 * 并不值得。
 *
 *		relids - 基关系标识符集合；如果只有一个，就是基关系，
 *				如果有多个，就是连接关系
 *		rows - 应用限制子句后关系中元组的估计数量
 *			   （即，该关系对应计划的输出行数）
 *		consider_startup - 若因启动代价低而值得为该关系保留普通路径，则为 true
 *		consider_param_startup - 对于参数化路径同理
 *		reltarget - 该关系的默认 Path 输出目标列表（tlist）；通常包含我们
 *					需要从该关系输出的值所对应的 Var 和 PlaceHolderVar 节点。
 *					列表中元素的顺序无所谓，但一个 appendrel 集合的所有关系
 *					必须使用相对应的顺序。
 *					注意：在 appendrel 子关系中，可能包含从子查询提升上来的
 *					任意表达式！
 *		pathlist - Path 节点列表，每个对应一种可能有用
 *				   的生成该关系的方法
 *		ppilist - 参数化 Path 的 ParamPathInfo 节点（如果有）
 *		cheapest_startup_path - 在非参数化路径中启动代价最低的 pathlist 成员
 *			（不考虑排序）；若不存在非参数化路径则为 NULL
 *		cheapest_total_path - 在非参数化路径中总代价最低的 pathlist 成员
 *			（不考虑排序）；若不存在非参数化路径，则为参数化程度最小的
 *			路径中总代价最低的那条
 *		cheapest_unique_path - 用于缓存从该关系产生唯一（无重复）输出的
 *			最低代价路径；若尚未请求则为 NULL
 *		cheapest_parameterized_paths - 各自参数化方式下的最佳路径；
 *			始终包含 cheapest_total_path，即使它是非参数化的
 *		direct_lateral_relids - 该关系直接 LATERAL 引用的关系
 *		lateral_relids - LATERAL 所需的外部关系，以 Relids 集合表示
 *			（同时包含直接和间接的 lateral 引用）
 *
 * 如果该关系是基关系，则它会有以下字段被设置：
 *
 *		relid - RTE 索引（这与 relids 字段冗余，但为了访问方便而提供）
 *		rtekind - RTE 的 rtekind 字段的副本
 *		min_attr, max_attr - 该关系有效 AttrNumber 的范围
 *		attr_needed - 一个 bitmapset 数组，指示每个属性所需达到的最高 joinrel；
 *				若第 0 位被置位，则表示该属性作为最终 targetlist 的一部分
 *				而被需要
 *		attr_widths - 每个属性宽度估算的缓存空间；
 *					  为零表示尚未计算
 *		lateral_vars - 该关系的 lateral 交叉引用（如果有）（Var 和
 *					   PlaceHolderVar 的列表）
 *		lateral_referencers - 以 lateral 方式引用该关系的那些关系的 relid
 *				（同时包含直接和间接的 lateral 引用）
 *		indexlist - 该关系各索引的 IndexOptInfo 节点列表
 *					（若不是表则总是 NIL）
 *		pages - 该关系中的磁盘页数（若不是表则为零）
 *		tuples - 该关系中的元组数（不考虑限制条件）
 *		allvisfrac - 被标记为 all-visible 的磁盘页所占比例
 *		eclass_indexes - 提及该关系的等价类（只有当 EC 合并完成后才填充）
 *		subroot - 子查询的 PlannerInfo（若不是子查询则为 NULL）
 *		subplan_params - 要传递给子查询的 PlannerParamItem 列表
 *
 *		注意：对于子查询，tuples 和 subroot 并不会在创建 RelOptInfo 对象时
 *		立即设置；它们是在 set_subquery_pathlist 处理该对象时才被填充的。
 *
 *		对于作为 appendrel 成员的 otherrel，这些字段的填充方式与基关系完全
 *		相同，只是我们不去管 lateral_vars。
 *
 *
 * 有两个字段用于缓存连接搜索期间获得的知识，即关于该关系在与给定其他关系连接时
 * 是否可证明唯一，也就是说，它至多只有一行能与来自该连接关系的任意给定行匹配。
 * 目前我们只对基关系尝试此类证明，因而也只填充这些字段；但将来也许会把它们用于
 * 连接关系：
 *
 *		unique_for_rels - Relid 集合的列表，其中每一个都是已证明该关系对其
 *					唯一的一组其他关系
 *		non_unique_for_rels - Relid 集合的列表，其中每一个都是我们已尝试
 *					但未能证明该关系对其唯一的一组其他关系
 *
 * 以下字段是否存在取决于该关系所参与的限制条件和连接：
 *
 *		baserestrictinfo - RestrictInfo 节点列表，包含有关该关系所参与的每个
 *					非连接限定条件子句的信息（仅用于基关系）
 *		baserestrictcost - 在单个元组上求值 baserestrictinfo 子句的估计代价
 *					（仅用于基关系）
 *		baserestrict_min_security - baserestrictinfo 各子句中最小的
 *					security_level
 *		joininfo  - RestrictInfo 节点列表，包含有关该关系所参与的每个连接
 *					子句的信息（但注意这排除了可能由等价类导出的子句）
 *		has_eclass_joins - 表示等价类连接是否可能的标志
 *
 * 注意：在 RelOptInfo 中保存 restrictinfo 列表只对基关系有用，因为对于连接关系
 * 而言，被视为限制子句的子句集合会随我们选择连接哪些子关系而变化。（例如，在
 * 一个 3 基关系的连接中，关联关系 1 和 2 的子句，如果我们把 {1} 和 {2 3} 连接
 * 成 {1 2 3}，就必须被视为一个限制子句；但如果我们把 {1 2} 和 {3} 连接，那么
 * 该子句在 {1 2} 中就会是一个限制子句，不应在 {1 2 3} 这一层级上再处理一次。）
 * 因此，连接情形下的 restrictinfo 列表出现在各个 JoinPath 中（字段
 * joinrestrictinfo），而不是出现在父关系中。但 RelOptInfo 保存 joininfo 列表
 * 是可以的，因为对于给定的关系而言，无论我们如何构造它，该列表都是相同的。
 *
 * 我们把 baserestrictcost 存储在 RelOptInfo 中（对于基关系），因为我们知道
 * 至少会需要它一次（用于为顺序扫描计价），并且可能多次需要它来为索引扫描计价。
 *----------
 */

/* Bitmask of flags supported by table AMs */
#define AMFLAG_HAS_TID_RANGE (1 << 0)

typedef enum RelOptKind
{
	RELOPT_BASEREL,
	RELOPT_JOINREL,
	RELOPT_OTHER_MEMBER_REL,
	RELOPT_UPPER_REL,
	RELOPT_OTHER_UPPER_REL,
	RELOPT_DEADREL
} RelOptKind;

/*
 * Is the given relation a simple relation i.e a base or "other" member
 * relation?
 */
#define IS_SIMPLE_REL(rel) \
	((rel)->reloptkind == RELOPT_BASEREL || \
	 (rel)->reloptkind == RELOPT_OTHER_MEMBER_REL)

/* Is the given relation a join relation? */
#define IS_JOIN_REL(rel)	\
	((rel)->reloptkind == RELOPT_JOINREL)

/* Is the given relation an upper relation? */
#define IS_UPPER_REL(rel)	\
	((rel)->reloptkind == RELOPT_UPPER_REL || \
	 (rel)->reloptkind == RELOPT_OTHER_UPPER_REL)

/* Is the given relation an "other" relation? */
#define IS_OTHER_REL(rel) \
	((rel)->reloptkind == RELOPT_OTHER_MEMBER_REL || \
	 (rel)->reloptkind == RELOPT_OTHER_UPPER_REL)

typedef struct RelOptInfo
{
	NodeTag		type;

	RelOptKind	reloptkind;

	/* all relations included in this RelOptInfo */
	Relids		relids;			/* set of base relids (rangetable indexes) */

	/* size estimates generated by planner */
	double		rows;			/* estimated number of result tuples */

	/* per-relation planner control flags */
	bool		consider_startup;	/* keep cheap-startup-cost paths? */
	bool		consider_param_startup; /* ditto, for parameterized paths? */

	/* default result targetlist for Paths scanning this relation */
	struct PathTarget *reltarget;	/* list of Vars/Exprs, cost, width */

	/* materialization information */
	List	   *pathlist;		/* Path structures */
	List	   *ppilist;		/* ParamPathInfos used in pathlist */
	struct Path *cheapest_startup_path;
	struct Path *cheapest_total_path;
	struct Path *cheapest_unique_path;
	List	   *cheapest_parameterized_paths;

	/* parameterization information needed for both base rels and join rels */
	/* (see also lateral_vars and lateral_referencers) */
	Relids		direct_lateral_relids;	/* rels directly laterally referenced */
	Relids		lateral_relids; /* minimum parameterization of rel */

	/* information about a base rel (not set for join rels!) */
	Index		relid;
	Oid			reltablespace;	/* containing tablespace */
	RTEKind		rtekind;		/* RELATION, SUBQUERY, FUNCTION, etc */
	AttrNumber	min_attr;		/* smallest attrno of rel (often <0) */
	AttrNumber	max_attr;		/* largest attrno of rel */
	Relids	   *attr_needed;	/* array indexed [min_attr .. max_attr] */
	int32	   *attr_widths;	/* array indexed [min_attr .. max_attr] */
	List	   *lateral_vars;	/* LATERAL Vars and PHVs referenced by rel */
	Relids		lateral_referencers;	/* rels that reference me laterally */
	List	   *indexlist;		/* list of IndexOptInfo */
	BlockNumber pages;			/* size estimates derived from pg_class */
	double		tuples;
	double		allvisfrac;
	Bitmapset  *eclass_indexes; /* Indexes in PlannerInfo's eq_classes list of
								 * ECs that mention this rel */
	PlannerInfo *subroot;		/* if subquery */
	List	   *subplan_params; /* if subquery */
	uint32		amflags;		/* Bitmask of optional features supported by
								 * the table AM */

	/* cache space for remembering if we have proven this relation unique */
	List	   *unique_for_rels;	/* known unique for these other relid
									 * set(s) */
	List	   *non_unique_for_rels;	/* known not unique for these set(s) */

	/* used by various scans and joins: */
	List	   *baserestrictinfo;	/* RestrictInfo structures (if base rel) */
	QualCost	baserestrictcost;	/* cost of evaluating the above */
	Index		baserestrict_min_security;	/* min security_level found in
											 * baserestrictinfo */
	List	   *joininfo;		/* RestrictInfo structures for join clauses
								 * involving this rel */
	bool		has_eclass_joins;	/* T means joininfo is incomplete */

	Relids		top_parent_relids;	/* Relids of topmost parents (if "other"
									 * rel) */
} RelOptInfo;

/*
 * IndexOptInfo
 *		Per-index information for planning/optimization
 *
 *		indexkeys[] and canreturn[] each have ncolumns entries.
 *
 *		indexcollations[], opfamily[], and opcintype[] each have nkeycolumns
 *		entries.  These don't contain any information about INCLUDE columns.
 *
 *		sortopfamily[], reverse_sort[], and nulls_first[] have
 *		nkeycolumns entries, if the index is ordered; but if it is unordered,
 *		those pointers are NULL.
 *
 *		Zeroes in the indexkeys[] array indicate index columns that are
 *		expressions; there is one element in indexprs for each such column.
 *
 *		For an ordered index, reverse_sort[] and nulls_first[] describe the
 *		sort ordering of a forward indexscan; we can also consider a backward
 *		indexscan, which will generate the reverse ordering.
 *
 *		The indexprs and indpred expressions have been run through
 *		prepqual.c and eval_const_expressions() for ease of matching to
 *		WHERE clauses. indpred is in implicit-AND form.
 *
 *		indextlist is a TargetEntry list representing the index columns.
 *		It provides an equivalent base-relation Var for each simple column,
 *		and links to the matching indexprs element for each expression column.
 *
 *		While most of these fields are filled when the IndexOptInfo is created
 *		(by plancat.c), indrestrictinfo and predOK are set later, in
 *		check_index_predicates().
 */
#ifndef HAVE_INDEXOPTINFO_TYPEDEF
typedef struct IndexOptInfo IndexOptInfo;
#define HAVE_INDEXOPTINFO_TYPEDEF 1
#endif

struct IndexOptInfo
{
	NodeTag		type;

	Oid			indexoid;		/* OID of the index relation */
	Oid			reltablespace;	/* tablespace of index (not table) */
	RelOptInfo *rel;			/* back-link to index's table */

	/* index-size statistics (from pg_class and elsewhere) */
	BlockNumber pages;			/* number of disk pages in index */
	double		tuples;			/* number of index tuples in index */
	int			tree_height;	/* index tree height, or -1 if unknown */

	/* index descriptor information */
	int			ncolumns;		/* number of columns in index */
	int			nkeycolumns;	/* number of key columns in index */
	int		   *indexkeys;		/* column numbers of index's attributes both
								 * key and included columns, or 0 */
	Oid		   *indexcollations;	/* OIDs of collations of index columns */
	Oid		   *opfamily;		/* OIDs of operator families for columns */
	Oid		   *opcintype;		/* OIDs of opclass declared input data types */
	Oid		   *sortopfamily;	/* OIDs of btree opfamilies, if orderable */
	bool	   *reverse_sort;	/* is sort order descending? */
	bool	   *nulls_first;	/* do NULLs come first in the sort order? */
	bytea	  **opclassoptions; /* opclass-specific options for columns */
	bool	   *canreturn;		/* which index cols can be returned in an
								 * index-only scan? */
	Oid			relam;			/* OID of the access method (in pg_am) */

	List	   *indexprs;		/* expressions for non-simple index columns */
	List	   *indpred;		/* predicate if a partial index, else NIL */

	List	   *indextlist;		/* targetlist representing index columns */

	List	   *indrestrictinfo;	/* parent relation's baserestrictinfo
									 * list, less any conditions implied by
									 * the index's predicate (unless it's a
									 * target rel, see comments in
									 * check_index_predicates()) */

	bool		predOK;			/* true if index predicate matches query */
	bool		unique;			/* true if a unique index */
	bool		immediate;		/* is uniqueness enforced immediately? */
	bool		hypothetical;	/* true if index doesn't really exist */

	/* Remaining fields are copied from the index AM's API struct: */
	bool		amcanorderbyop; /* does AM support order by operator result? */
	bool		amoptionalkey;	/* can query omit key for the first column? */
	bool		amsearcharray;	/* can AM handle ScalarArrayOpExpr quals? */
	bool		amsearchnulls;	/* can AM search for NULL/NOT NULL entries? */
	bool		amhasgettuple;	/* does AM have amgettuple interface? */
	bool		amhasgetbitmap; /* does AM have amgetbitmap interface? */
	bool		amcanmarkpos;	/* does AM support mark/restore? */
	/* Rather than include amapi.h here, we declare amcostestimate like this */
	void		(*amcostestimate) ();	/* AM's cost estimator */
};

/*
 * EquivalenceClasses
 *
 * Whenever we can determine that a mergejoinable equality clause A = B is
 * not delayed by any outer join, we create an EquivalenceClass containing
 * the expressions A and B to record this knowledge.  If we later find another
 * equivalence B = C, we add C to the existing EquivalenceClass; this may
 * require merging two existing EquivalenceClasses.  At the end of the qual
 * distribution process, we have sets of values that are known all transitively
 * equal to each other, where "equal" is according to the rules of the btree
 * operator family(s) shown in ec_opfamilies, as well as the collation shown
 * by ec_collation.  (We restrict an EC to contain only equalities whose
 * operators belong to the same set of opfamilies.  This could probably be
 * relaxed, but for now it's not worth the trouble, since nearly all equality
 * operators belong to only one btree opclass anyway.  Similarly, we suppose
 * that all or none of the input datatypes are collatable, so that a single
 * collation value is sufficient.)
 *
 * We also use EquivalenceClasses as the base structure for PathKeys, letting
 * us represent knowledge about different sort orderings being equivalent.
 * Since every PathKey must reference an EquivalenceClass, we will end up
 * with single-member EquivalenceClasses whenever a sort key expression has
 * not been equivalenced to anything else.  It is also possible that such an
 * EquivalenceClass will contain a volatile expression ("ORDER BY random()"),
 * which is a case that can't arise otherwise since clauses containing
 * volatile functions are never considered mergejoinable.  We mark such
 * EquivalenceClasses specially to prevent them from being merged with
 * ordinary EquivalenceClasses.  Also, for volatile expressions we have
 * to be careful to match the EquivalenceClass to the correct targetlist
 * entry: consider SELECT random() AS a, random() AS b ... ORDER BY b,a.
 * So we record the SortGroupRef of the originating sort clause.
 *
 * We allow equality clauses appearing below the nullable side of an outer join
 * to form EquivalenceClasses, but these have a slightly different meaning:
 * the included values might be all NULL rather than all the same non-null
 * values.  See src/backend/optimizer/README for more on that point.
 *
 * NB: if ec_merged isn't NULL, this class has been merged into another, and
 * should be ignored in favor of using the pointed-to class.
 */
typedef struct EquivalenceClass
{
	NodeTag		type;

	List	   *ec_opfamilies;	/* btree operator family OIDs */
	Oid			ec_collation;	/* collation, if datatypes are collatable */
	List	   *ec_members;		/* list of EquivalenceMembers */
	List	   *ec_sources;		/* list of generating RestrictInfos */
	List	   *ec_derives;		/* list of derived RestrictInfos */
	Relids		ec_relids;		/* all relids appearing in ec_members, except
								 * for child members (see below) */
	bool		ec_has_const;	/* any pseudoconstants in ec_members? */
	bool		ec_has_volatile;	/* the (sole) member is a volatile expr */
	bool		ec_below_outer_join;	/* equivalence applies below an OJ */
	bool		ec_broken;		/* failed to generate needed clauses? */
	Index		ec_sortref;		/* originating sortclause label, or 0 */
	Index		ec_min_security;	/* minimum security_level in ec_sources */
	Index		ec_max_security;	/* maximum security_level in ec_sources */
	struct EquivalenceClass *ec_merged; /* set if merged into another EC */
} EquivalenceClass;

/*
 * If an EC contains a const and isn't below-outer-join, any PathKey depending
 * on it must be redundant, since there's only one possible value of the key.
 */
#define EC_MUST_BE_REDUNDANT(eclass)  \
	((eclass)->ec_has_const && !(eclass)->ec_below_outer_join)

/*
 * EquivalenceMember - one member expression of an EquivalenceClass
 *
 * em_is_child signifies that this element was built by transposing a member
 * for an appendrel parent relation to represent the corresponding expression
 * for an appendrel child.  These members are used for determining the
 * pathkeys of scans on the child relation.  An em_is_child member has no
 * impact on the properties of the EC as a
 * whole; in particular the EC's ec_relids field does NOT include the child
 * relation.  An em_is_child member should never be marked em_is_const nor
 * cause ec_has_const or ec_has_volatile to be set, either.  Thus, em_is_child
 * members are not really full-fledged members of the EC, but just reflections
 * or doppelgangers of real members.  Most operations on EquivalenceClasses
 * should ignore em_is_child members, and those that don't should test
 * em_relids to make sure they only consider relevant members.
 *
 * em_datatype is usually the same as exprType(em_expr), but can be
 * different when dealing with a binary-compatible opfamily; in particular
 * anyarray_ops would never work without this.  Use em_datatype when
 * looking up a specific btree operator to work with this expression.
 */
typedef struct EquivalenceMember
{
	NodeTag		type;

	Expr	   *em_expr;		/* the expression represented */
	Relids		em_relids;		/* all relids appearing in em_expr */
	Relids		em_nullable_relids; /* nullable by lower outer joins */
	bool		em_is_const;	/* expression is pseudoconstant? */
	bool		em_is_child;	/* derived version for a child relation? */
	Oid			em_datatype;	/* the "nominal type" used by the opfamily */
} EquivalenceMember;

/*
 * PathKeys
 *
 * The sort ordering of a path is represented by a list of PathKey nodes.
 * An empty list implies no known ordering.  Otherwise the first item
 * represents the primary sort key, the second the first secondary sort key,
 * etc.  The value being sorted is represented by linking to an
 * EquivalenceClass containing that value and including pk_opfamily among its
 * ec_opfamilies.  The EquivalenceClass tells which collation to use, too.
 * This is a convenient method because it makes it trivial to detect
 * equivalent and closely-related orderings. (See optimizer/README for more
 * information.)
 *
 * Note: pk_strategy is either BTLessStrategyNumber (for ASC) or
 * BTGreaterStrategyNumber (for DESC).  We assume that all ordering-capable
 * index types will use btree-compatible strategy numbers.
 */
typedef struct PathKey
{
	NodeTag		type;

	EquivalenceClass *pk_eclass;	/* the value that is ordered */
	Oid			pk_opfamily;	/* btree opfamily defining the ordering */
	int			pk_strategy;	/* sort direction (ASC or DESC) */
	bool		pk_nulls_first; /* do NULLs come before normal values? */
} PathKey;

/*
 * VolatileFunctionStatus -- allows nodes to cache their
 * contain_volatile_functions properties. VOLATILITY_UNKNOWN means not yet
 * determined.
 */
typedef enum VolatileFunctionStatus
{
	VOLATILITY_UNKNOWN = 0,
	VOLATILITY_VOLATILE,
	VOLATILITY_NOVOLATILE
} VolatileFunctionStatus;

/*
 * PathTarget
 *
 * This struct contains what we need to know during planning about the
 * targetlist (output columns) that a Path will compute.  Each RelOptInfo
 * includes a default PathTarget, which its individual Paths may simply
 * reference.  However, in some cases a Path may compute outputs different
 * from other Paths, and in that case we make a custom PathTarget for it.
 * For example, an indexscan might return index expressions that would
 * otherwise need to be explicitly calculated.  (Note also that "upper"
 * relations generally don't have useful default PathTargets.)
 *
 * exprs contains bare expressions; they do not have TargetEntry nodes on top,
 * though those will appear in finished Plans.
 *
 * sortgrouprefs[] is an array of the same length as exprs, containing the
 * corresponding sort/group refnos, or zeroes for expressions not referenced
 * by sort/group clauses.  If sortgrouprefs is NULL (which it generally is in
 * RelOptInfo.reltarget targets; only upper-level Paths contain this info),
 * we have not identified sort/group columns in this tlist.  This allows us to
 * deal with sort/group refnos when needed with less expense than including
 * TargetEntry nodes in the exprs list.
 */
typedef struct PathTarget
{
	NodeTag		type;
	List	   *exprs;			/* list of expressions to be computed */
	Index	   *sortgrouprefs;	/* corresponding sort/group refnos, or 0 */
	QualCost	cost;			/* cost of evaluating the expressions */
	int			width;			/* estimated avg width of result tuples */
	VolatileFunctionStatus has_volatile_expr;	/* indicates if exprs contain
												 * any volatile functions. */
} PathTarget;

/* Convenience macro to get a sort/group refno from a PathTarget */
#define get_pathtarget_sortgroupref(target, colno) \
	((target)->sortgrouprefs ? (target)->sortgrouprefs[colno] : (Index) 0)


/*
 * ParamPathInfo
 *
 * All parameterized paths for a given relation with given required outer rels
 * link to a single ParamPathInfo, which stores common information such as
 * the estimated rowcount for this parameterization.  We do this partly to
 * avoid recalculations, but mostly to ensure that the estimated rowcount
 * is in fact the same for every such path.
 *
 * Note: ppi_clauses is only used in ParamPathInfos for base relation paths;
 * in join cases it's NIL because the set of relevant clauses varies depending
 * on how the join is formed.  The relevant clauses will appear in each
 * parameterized join path's joinrestrictinfo list, instead.
 */
typedef struct ParamPathInfo
{
	NodeTag		type;

	Relids		ppi_req_outer;	/* rels supplying parameters used by path */
	double		ppi_rows;		/* estimated number of result tuples */
	List	   *ppi_clauses;	/* join clauses available from outer rels */
} ParamPathInfo;


/*
 * Type "Path" is used as-is for sequential-scan paths, as well as some other
 * simple plan types that we don't need any extra information in the path for.
 * For other path types it is the first component of a larger struct.
 *
 * "pathtype" is the NodeTag of the Plan node we could build from this Path.
 * It is partially redundant with the Path's NodeTag, but allows us to use
 * the same Path type for multiple Plan types when there is no need to
 * distinguish the Plan type during path processing.
 *
 * "parent" identifies the relation this Path scans, and "pathtarget"
 * describes the precise set of output columns the Path would compute.
 * In simple cases all Paths for a given rel share the same targetlist,
 * which we represent by having path->pathtarget equal to parent->reltarget.
 *
 * "param_info", if not NULL, links to a ParamPathInfo that identifies outer
 * relation(s) that provide parameter values to each scan of this path.
 * That means this path can only be joined to those rels by means of nestloop
 * joins with this path on the inside.  Also note that a parameterized path
 * is responsible for testing all "movable" joinclauses involving this rel
 * and the specified outer rel(s).
 *
 * "rows" is the same as parent->rows in simple paths, but in parameterized
 * paths and UniquePaths it can be less than parent->rows, reflecting the
 * fact that we've filtered by extra join conditions or removed duplicates.
 *
 * "pathkeys" is a List of PathKey nodes (see above), describing the sort
 * ordering of the path's output rows.
 */
typedef struct Path
{
	NodeTag		type;

	NodeTag		pathtype;		/* tag identifying scan/join method */

	RelOptInfo *parent;			/* the relation this path can build */
	PathTarget *pathtarget;		/* list of Vars/Exprs, cost, width */

	ParamPathInfo *param_info;	/* parameterization info, or NULL if none */

	/* estimated size/costs for path (see costsize.c for more info) */
	double		rows;			/* estimated number of result tuples */
	Cost		startup_cost;	/* cost expended before fetching any tuples */
	Cost		total_cost;		/* total cost (assuming all tuples fetched) */

	List	   *pathkeys;		/* sort ordering of path's output */
	/* pathkeys is a List of PathKey nodes; see above */
} Path;

/* Macro for extracting a path's parameterization relids; beware double eval */
#define PATH_REQ_OUTER(path)  \
	((path)->param_info ? (path)->param_info->ppi_req_outer : (Relids) NULL)

/*----------
 * IndexPath represents an index scan over a single index.
 *
 * This struct is used for both regular indexscans and index-only scans;
 * path.pathtype is T_IndexScan or T_IndexOnlyScan to show which is meant.
 *
 * 'indexinfo' is the index to be scanned.
 *
 * 'indexclauses' is a list of IndexClause nodes, each representing one
 * index-checkable restriction, with implicit AND semantics across the list.
 * An empty list implies a full index scan.
 *
 * 'indexorderbys', if not NIL, is a list of ORDER BY expressions that have
 * been found to be usable as ordering operators for an amcanorderbyop index.
 * The list must match the path's pathkeys, ie, one expression per pathkey
 * in the same order.  These are not RestrictInfos, just bare expressions,
 * since they generally won't yield booleans.  It's guaranteed that each
 * expression has the index key on the left side of the operator.
 *
 * 'indexorderbycols' is an integer list of index column numbers (zero-based)
 * of the same length as 'indexorderbys', showing which index column each
 * ORDER BY expression is meant to be used with.  (There is no restriction
 * on which index column each ORDER BY can be used with.)
 *
 * 'indexscandir' is one of:
 *		ForwardScanDirection: forward scan of an ordered index
 *		BackwardScanDirection: backward scan of an ordered index
 *		NoMovementScanDirection: scan of an unordered index, or don't care
 * (The executor doesn't care whether it gets ForwardScanDirection or
 * NoMovementScanDirection for an indexscan, but the planner wants to
 * distinguish ordered from unordered indexes for building pathkeys.)
 *
 * 'indextotalcost' and 'indexselectivity' are saved in the IndexPath so that
 * we need not recompute them when considering using the same index in a
 * bitmap index/heap scan (see BitmapHeapPath).  The costs of the IndexPath
 * itself represent the costs of an IndexScan or IndexOnlyScan plan type.
 *----------
 */
typedef struct IndexPath
{
	Path		path;
	IndexOptInfo *indexinfo;
	List	   *indexclauses;
	List	   *indexorderbys;
	List	   *indexorderbycols;
	ScanDirection indexscandir;
	Cost		indextotalcost;
	Selectivity indexselectivity;
} IndexPath;

/*
 * Each IndexClause references a RestrictInfo node from the query's WHERE
 * or JOIN conditions, and shows how that restriction can be applied to
 * the particular index.  We support both indexclauses that are directly
 * usable by the index machinery, which are typically of the form
 * "indexcol OP pseudoconstant", and those from which an indexable qual
 * can be derived.  The simplest such transformation is that a clause
 * of the form "pseudoconstant OP indexcol" can be commuted to produce an
 * indexable qual (the index machinery expects the indexcol to be on the
 * left always).  Another example is that we might be able to extract an
 * indexable range condition from a LIKE condition, as in "x LIKE 'foo%bar'"
 * giving rise to "x >= 'foo' AND x < 'fop'".  Derivation of such lossy
 * conditions is done by a planner support function attached to the
 * indexclause's top-level function or operator.
 *
 * indexquals is a list of RestrictInfos for the directly-usable index
 * conditions associated with this IndexClause.  In the simplest case
 * it's a one-element list whose member is iclause->rinfo.  Otherwise,
 * it contains one or more directly-usable indexqual conditions extracted
 * from the given clause.  The 'lossy' flag indicates whether the
 * indexquals are semantically equivalent to the original clause, or
 * represent a weaker condition.
 *
 * Normally, indexcol is the index of the single index column the clause
 * works on, and indexcols is NIL.
 *
 * An IndexPath's IndexClause list is required to be ordered by index
 * column, i.e. the indexcol values must form a nondecreasing sequence.
 * (The order of multiple clauses for the same index column is unspecified.)
 */
typedef struct IndexClause
{
	NodeTag		type;
	struct RestrictInfo *rinfo; /* original restriction or join clause */
	List	   *indexquals;		/* indexqual(s) derived from it */
	bool		lossy;			/* are indexquals a lossy version of clause? */
	AttrNumber	indexcol;		/* index column the clause uses (zero-based) */
	List	   *indexcols;		/* multiple index columns, if any */
} IndexClause;

/*
 * BitmapHeapPath represents one or more indexscans that generate TID bitmaps
 * instead of directly accessing the heap, followed by AND/OR combinations
 * to produce a single bitmap, followed by a heap scan that uses the bitmap.
 * Note that the output is always considered unordered, since it will come
 * out in physical heap order no matter what the underlying indexes did.
 *
 * The individual indexscans are represented by IndexPath nodes, and any
 * logic on top of them is represented by a tree of BitmapAndPath and
 * BitmapOrPath nodes.  Notice that we can use the same IndexPath node both
 * to represent a regular (or index-only) index scan plan, and as the child
 * of a BitmapHeapPath that represents scanning the same index using a
 * BitmapIndexScan.  The startup_cost and total_cost figures of an IndexPath
 * always represent the costs to use it as a regular (or index-only)
 * IndexScan.  The costs of a BitmapIndexScan can be computed using the
 * IndexPath's indextotalcost and indexselectivity.
 */
typedef struct BitmapHeapPath
{
	Path		path;
	Path	   *bitmapqual;		/* IndexPath, BitmapAndPath, BitmapOrPath */
} BitmapHeapPath;

/*
 * BitmapAndPath represents a BitmapAnd plan node; it can only appear as
 * part of the substructure of a BitmapHeapPath.  The Path structure is
 * a bit more heavyweight than we really need for this, but for simplicity
 * we make it a derivative of Path anyway.
 */
typedef struct BitmapAndPath
{
	Path		path;
	List	   *bitmapquals;	/* IndexPaths and BitmapOrPaths */
	Selectivity bitmapselectivity;
} BitmapAndPath;

/*
 * BitmapOrPath represents a BitmapOr plan node; it can only appear as
 * part of the substructure of a BitmapHeapPath.  The Path structure is
 * a bit more heavyweight than we really need for this, but for simplicity
 * we make it a derivative of Path anyway.
 */
typedef struct BitmapOrPath
{
	Path		path;
	List	   *bitmapquals;	/* IndexPaths and BitmapAndPaths */
	Selectivity bitmapselectivity;
} BitmapOrPath;

/*
 * TidPath represents a scan by TID
 *
 * tidquals is an implicitly OR'ed list of qual expressions of the form
 * "CTID = pseudoconstant", or "CTID = ANY(pseudoconstant_array)".
 */
typedef struct TidPath
{
	Path		path;
	List	   *tidquals;		/* qual(s) involving CTID = something */
} TidPath;

/*
 * TidRangePath represents a scan by a continguous range of TIDs
 *
 * tidrangequals is an implicitly AND'ed list of qual expressions of the form
 * "CTID relop pseudoconstant", where relop is one of >,>=,<,<=.
 */
typedef struct TidRangePath
{
	Path		path;
	List	   *tidrangequals;
} TidRangePath;

/*
 * SubqueryScanPath represents a scan of an unflattened subquery-in-FROM
 *
 * Note that the subpath comes from a different planning domain; for example
 * RTE indexes within it mean something different from those known to the
 * SubqueryScanPath.  path.parent->subroot is the planning context needed to
 * interpret the subpath.
 */
typedef struct SubqueryScanPath
{
	Path		path;
	Path	   *subpath;		/* path representing subquery execution */
} SubqueryScanPath;

/*
 * AppendPath represents an Append plan, ie, successive execution of
 * several member plans.
 *
 * Note: an AppendPath with no subpaths is a "dummy" path that is created to
 * represent the case that a relation is provably empty.  It is the only
 * form of AppendPath that this build of the code can produce.
 */
typedef struct AppendPath
{
	Path		path;
	List	   *subpaths;		/* list of component Paths */
} AppendPath;

#define IS_DUMMY_APPEND(p) \
	(IsA((p), AppendPath) && ((AppendPath *) (p))->subpaths == NIL)

/*
 * A relation that's been proven empty will have one path that is dummy
 * (but might have projection paths on top).  For historical reasons,
 * this is provided as a macro that wraps is_dummy_rel().
 */
#define IS_DUMMY_REL(r) is_dummy_rel(r)
extern bool is_dummy_rel(RelOptInfo *rel);

/*
 * ResultPath represents use of a Result plan node to compute a one-row
 * output, as needed for a FROM-less SELECT on an RTE_RESULT relation,
 * possibly filtered by a qual.
 *
 * Note that quals is a list of bare clauses, not RestrictInfos.
 */
typedef struct ResultPath
{
	Path		path;
	List	   *quals;
} ResultPath;

/*
 * MaterialPath represents use of a Material plan node, i.e., caching of
 * the output of its subpath.  This is used when the subpath is expensive
 * and needs to be scanned repeatedly, or when we need mark/restore ability
 * and the subpath doesn't have it.
 */
typedef struct MaterialPath
{
	Path		path;
	Path	   *subpath;
} MaterialPath;

/*
 * UniquePath represents elimination of distinct rows from the output of
 * its subpath.
 *
 * This can represent significantly different plans: either sort-based
 * implementation, or a no-op if the input path can be proven distinct
 * already.  The decision is sufficiently localized that it's not worth
 * having separate Path node types.  (Note: in the no-op case, we could
 * eliminate the UniquePath node entirely and just return the subpath; but
 * it's convenient to have a UniquePath in the path tree to signal upper-level
 * routines that the input is known distinct.)
 */
typedef enum
{
	UNIQUE_PATH_NOOP,			/* input is known unique already */
	UNIQUE_PATH_SORT			/* use sorting */
} UniquePathMethod;

typedef struct UniquePath
{
	Path		path;
	Path	   *subpath;
	UniquePathMethod umethod;
	List	   *in_operators;	/* equality operators of the IN clause */
	List	   *uniq_exprs;		/* expressions to be made unique */
} UniquePath;

/*
 * All join-type paths share these fields.
 */

typedef struct JoinPath
{
	Path		path;

	JoinType	jointype;

	bool		inner_unique;	/* each outer tuple provably matches no more
								 * than one inner tuple */

	Path	   *outerjoinpath;	/* path for the outer side of the join */
	Path	   *innerjoinpath;	/* path for the inner side of the join */

	List	   *joinrestrictinfo;	/* RestrictInfos to apply to join */

	/*
	 * See the notes for RelOptInfo and ParamPathInfo to understand why
	 * joinrestrictinfo is needed in JoinPath, and can't be merged into the
	 * parent RelOptInfo.
	 */
} JoinPath;

/*
 * A nested-loop path needs no special fields.
 */

typedef JoinPath NestPath;

/*
 * A mergejoin path has these fields.
 *
 * Unlike other path types, a MergePath node doesn't represent just a single
 * run-time plan node: it can represent up to four.  Aside from the MergeJoin
 * node itself, there can be a Sort node for the outer input, a Sort node
 * for the inner input, and/or a Material node for the inner input.  We could
 * represent these nodes by separate path nodes, but considering how many
 * different merge paths are investigated during a complex join problem,
 * it seems better to avoid unnecessary palloc overhead.
 *
 * path_mergeclauses lists the clauses (in the form of RestrictInfos)
 * that will be used in the merge.
 *
 * Note that the mergeclauses are a subset of the parent relation's
 * restriction-clause list.  Any join clauses that are not mergejoinable
 * appear only in the parent's restrict list, and must be checked by a
 * qpqual at execution time.
 *
 * outersortkeys (resp. innersortkeys) is NIL if the outer path
 * (resp. inner path) is already ordered appropriately for the
 * mergejoin.  If it is not NIL then it is a PathKeys list describing
 * the ordering that must be created by an explicit Sort node.
 *
 * skip_mark_restore is true if the executor need not do mark/restore calls.
 * Mark/restore overhead is usually required, but can be skipped if we know
 * that the executor need find only one match per outer tuple, and that the
 * mergeclauses are sufficient to identify a match.  In such cases the
 * executor can immediately advance the outer relation after processing a
 * match, and therefore it need never back up the inner relation.
 *
 * materialize_inner is true if a Material node should be placed atop the
 * inner input.  This may appear with or without an inner Sort step.
 */

typedef struct MergePath
{
	JoinPath	jpath;
	List	   *path_mergeclauses;	/* join clauses to be used for merge */
	List	   *outersortkeys;	/* keys for explicit sort, if any */
	List	   *innersortkeys;	/* keys for explicit sort, if any */
	bool		skip_mark_restore;	/* can executor skip mark/restore? */
	bool		materialize_inner;	/* add Materialize to inner? */
} MergePath;

/*
 * A hashjoin path has these fields.
 *
 * The remarks above for mergeclauses apply for hashclauses as well.
 *
 * Hashjoin does not care what order its inputs appear in, so we have
 * no need for sortkeys.
 */

typedef struct HashPath
{
	JoinPath	jpath;
	List	   *path_hashclauses;	/* join clauses used for hashing */
	int			num_batches;	/* number of batches expected */
	double		inner_rows_total;	/* total inner rows expected */
} HashPath;

/*
 * ProjectionPath represents a projection (that is, targetlist computation)
 *
 * Nominally, this path node represents using a Result plan node to do a
 * projection step.  However, if the input plan node supports projection,
 * we can just modify its output targetlist to do the required calculations
 * directly, and not need a Result.  In some places in the planner we can just
 * jam the desired PathTarget into the input path node (and adjust its cost
 * accordingly), so we don't need a ProjectionPath.  But in other places
 * it's necessary to not modify the input path node, so we need a separate
 * ProjectionPath node, which is marked dummy to indicate that we intend to
 * assign the work to the input plan node.  The estimated cost for the
 * ProjectionPath node will account for whether a Result will be used or not.
 */
typedef struct ProjectionPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	bool		dummypp;		/* true if no separate Result is needed */
} ProjectionPath;

/*
 * ProjectSetPath represents evaluation of a targetlist that includes
 * set-returning function(s), which will need to be implemented by a
 * ProjectSet plan node.
 */
typedef struct ProjectSetPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
} ProjectSetPath;

/*
 * SortPath represents an explicit sort step
 *
 * The sort keys are, by definition, the same as path.pathkeys.
 *
 * Note: the Sort plan node cannot project, so path.pathtarget must be the
 * same as the input's pathtarget.
 */
typedef struct SortPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
} SortPath;

/*
 * IncrementalSortPath represents an incremental sort step
 *
 * This is like a regular sort, except some leading key columns are assumed
 * to be ordered already.
 */
typedef struct IncrementalSortPath
{
	SortPath	spath;
	int			nPresortedCols; /* number of presorted columns */
} IncrementalSortPath;

/*
 * UpperUniquePath represents adjacent-duplicate removal (in presorted input)
 *
 * The columns to be compared are the first numkeys columns of the path's
 * pathkeys.  The input is presumed already sorted that way.
 */
typedef struct UpperUniquePath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	int			numkeys;		/* number of pathkey columns to compare */
} UpperUniquePath;

/*
 * ModifyTablePath represents performing INSERT/UPDATE/DELETE modifications
 *
 * We represent most things that will be in the ModifyTable plan node
 * literally, except we have a child Path not Plan.  But analysis of the
 * FDW data is deferred to createplan.c.
 */
typedef struct ModifyTablePath
{
	Path		path;
	Path	   *subpath;		/* Path producing source data */
	CmdType		operation;		/* INSERT, UPDATE, or DELETE */
	bool		canSetTag;		/* do we set the command tag/es_processed? */
	Index		nominalRelation;	/* Parent RT index for use of EXPLAIN */
	Index		rootRelation;	/* Root RT index, if partitioned/inherited */
	List	   *resultRelations;	/* integer list of RT indexes */
	List	   *updateColnosLists;	/* per-target-table update_colnos lists */
	List	   *rowMarks;		/* PlanRowMarks (non-locking only) */
	int			epqParam;		/* ID of Param for EvalPlanQual re-eval */
} ModifyTablePath;


/*
 * 限制条件（Restriction clause）信息。
 *
 * 我们为限制条件（WHERE 或 JOIN/ON 子句）的每一个 AND 子子句创建一个该结构。
 * 由于限制子句在逻辑上是 AND 连接的，我们可以用其中任意一个或任意子集来过滤
 * 元组，而无需计算其余子句。RestrictInfo 节点本身存储了优化器在选择最佳查询
 * 计划时所使用的数据。
 *
 * 如果某个限制子句只引用单个基关系，它就会出现在该基关系对应 RelOptInfo 的
 * baserestrictinfo 列表中。
 *
 * 如果某个限制子句引用多于一个基关系，它会出现在每一个 RelOptInfo 的 joininfo
 * 列表中，只要该 RelOptInfo 描述的是该子句所涉及基关系的一个真子集。joininfo
 * 列表用于驱动连接树的构建，通过挑选可能的连接候选者。然而，只有当构建出包含该
 * 子句所引用的全部基关系的连接关系（join rel）后，该子句才能真正被应用。
 *
 * 当我们构建出一个连接关系，且它包含了某个多关系限制子句所引用的全部基关系时，
 * 如果左子路径和右子路径都没有包含该子句所引用的全部基关系，我们就把它放入该
 * 连接关系各路径的 joinrestrictinfo 列表中。该子句将在该连接层级上被应用，并且
 * 不会再向上传播到连接树的其他部分。（注：曾经的"谓词迁移"（predicate migration）
 * 代码本打算根据求值代价在计划树中上下推动限制子句，但它是死代码，在可预见的
 * 将来也不太可能被复活。）
 *
 * 注意，当涉及两个以上关系时，一个多关系限制条件在连接树中到达的高度可能取决于
 * 我们使用的连接顺序。因此，这类子句不能直接关联到连接的 RelOptInfo 上，而必须
 * 按每个连接路径（per-join-path）来跟踪。
 *
 * 表示等价条件（即，可归并连接（mergejoinable）的相等条件，且未被外连接延迟）的
 * RestrictInfo 处理方式略有不同。最初我们把它们挂到由它们导出的等价类
 * （EquivalenceClass）上。当我们构建扫描路径或连接路径时，我们会遍历所有等价类，
 * 并生成派生的 RestrictInfo，表示针对该特定扫描或连接，为强制每个等价类的所有
 * 成员在该扫描或连接输出的所有行中确实相等所需检查的最小条件集合。
 *
 * 在处理外连接时，我们必须非常小心地在树中上下推动限定条件（qual clause）。
 * 外连接自身的 JOIN/ON 条件必须恰好在该连接节点处求值，除非它们是"退化"
 * （degenerate）条件——即只引用连接可空侧（nullable side）的 Var。出现在 WHERE 中
 * 或外连接之上的 JOIN 中的限定条件，只要引用了任何可空 Var，就不能下推到外连接
 * 之下。RestrictInfo 节点包含一个标志，用来表示某个限定条件是否已被下推到比其
 * 在连接树中的原始语法位置更低的层级。如果某个外连接阻止我们把限定条件下推到它
 * 的"自然"语义层级（即仅由该限定条件所使用的基关系所关联的层级），那么我们就给
 * 该限定条件标记一个 required_relids 值，其中包含的不仅仅是它实际使用到的基关系。
 * 通过假装该限定条件引用了构建该外连接所需的全部关系，我们就能阻止它在该外连接的
 * joinrel 之下被求值。当我们真正构建外连接的 joinrel 时，我们仍然需要区分那些
 * 实际属于该连接 JOIN/ON 条件的限定条件，和那些出现在树中其他位置、因未使用其他
 * 关系而被下推到该 joinrel 的限定条件。is_pushed_down 标志正是为此而设；它告诉
 * 我们：一个限定条件对于 required_relids 中列出的那组基关系而言，并不是一个
 * OUTER JOIN 限定条件。原本来自 WHERE 或 INNER JOIN 条件的子句，其 is_pushed_down
 * 标志*总是*会被置位。一个 OUTER JOIN 子句也有可能被标记为 is_pushed_down，如果
 * 我们判定它可以被下推到连接的可空侧。在这种情况下，无论它在何处求值，它都充当
 * 一个普通的过滤限定条件。（简而言之，is_pushed_down 仅对非退化的外连接条件为
 * false。也许我们应该重命名它以反映这一含义？但另见下面关于 RINFO_IS_PUSHED_DOWN
 * 的注释。）
 *
 * RestrictInfo 节点还包含一个 outerjoin_delayed 标志，如果由于该子句下方存在任何
 * 外连接而必须延迟其适用性，则该标志为 true（即，它必须被推迟到比它实际引用的
 * 关系集合更高的某个连接层级）。
 *
 * 还有一个 outer_relids 字段，除了外连接子句外均为 NULL；对于外连接子句，它是
 * 该子句所属外连接外侧（outer side）上的关系集合。（这些关系是：在参数化扫描中
 * 该子句不能应用到的关系，因为把它推入连接的外侧会导致错误的结果。）
 *
 * 还有一个 nullable_relids 字段，它是该子句所引用的、可能被该子句下方的某些外
 * 连接强制置空的关系集合。
 *
 * outerjoin_delayed = true 与 nullable_relids != NULL 有微妙差别：一个子句可能
 * 引用了一些可空关系，但却不是 outerjoin_delayed，因为它也引用了外连接的其他所有
 * 关系。一个非 outerjoin_delayed 的子句可以在任何它能被计算的地方被强制执行。
 *
 * 为了高效处理安全屏障（security-barrier）条件，我们用 security_level 字段标记
 * RestrictInfo 节点，其中较高的值标识来自较不可信来源的子句。确切语义是：一个
 * 子句不能在另一个 security_level 值更低的子句之前被求值，除非第一个子句是
 * leakproof（防泄漏）的。与外连接子句一样，这造成了一种原因：子句有时需要在连接
 * 树中比其内容所暗示的更高的位置求值；而且即使在单个计划节点内，这条规则也约束
 * 了子句的应用顺序。
 *
 * 一般而言，被引用的子句可能是任意复杂的。我们能够作为索引扫描限定条件
 * （indexscan quals）、归并连接子句或哈希连接子句处理的子句种类是有限的
 * （例如，不允许易变（volatile）函数）。每种路径的代码负责识别它能够使用的限制
 * 子句，并忽略其余部分。索引扫描、归并连接或哈希连接未实现的子句将被放入最终
 * Plan 节点的 plan qual 或 joinqual 字段中，在那里由通用的限定条件表达式求值代码
 * 来强制执行。（但如果我们能猜出它的选择性，我们仍然有权在估算结果元组数时计入
 * 它的选择性……）
 *
 * 当被引用的子句是一个 OR 子句时，我们会生成一个修改过的副本，在其中向顶层 OR/AND
 * 结构之下插入额外的 RestrictInfo 节点。这是为 OR 索引扫描处理提供便利：无论取自
 * 顶层还是取自某个 OR 子子句的索引限定条件，都会有与之关联的 RestrictInfo 节点。
 *
 * 如果某个子句看起来可能可用作归并连接或哈希连接子句，即它是一个二元操作符子句，
 * 且左、右两侧引用的 relid 集合不重叠，则 can_join 标志被置为 true。（但该操作符
 * 实际上是否可归并连接或可哈希连接，并不在此检查。）
 *
 * 如果某个子句不包含当前查询层级的任何 Var，也不包含易变函数，则 pseudoconstant
 * 标志被置为 true。这样的子句可以被提取出来，作为门控 Result 节点中的一次性限定
 * 条件使用。我们把伪常量子句与其他 RestrictInfo 一起保留在相同的列表中，以便常规
 * 的子句推动机制能把它们分配到正确的连接层级，但在代价估算和选择性估算时需要对
 * 它们做特殊处理。注意，伪常量子句永远不可能成为索引限定条件、归并连接或哈希
 * 连接子句，因此规划器的大部分部分对它并不感兴趣。
 *
 * 当归并子句由等价类生成时，可能存在若干种同样有效的强制执行连接等价性的方式，
 * 而我们只需要应用其中一种。我们通过设置 parent_ec 指向生成它的等价类来标记这类
 * 子句。在同一连接中，多个具有相同 parent_ec 的子句是冗余的。
 */

typedef struct RestrictInfo
{
	NodeTag		type;

	Expr	   *clause;			/* the represented clause of WHERE or JOIN */

	bool		is_pushed_down; /* true if clause was pushed down in level */

	bool		outerjoin_delayed;	/* true if delayed by lower outer join */

	bool		can_join;		/* see comment above */

	bool		pseudoconstant; /* see comment above */

	bool		leakproof;		/* true if known to contain no leaked Vars */

	VolatileFunctionStatus has_volatile;	/* to indicate if clause contains
											 * any volatile functions. */

	Index		security_level; /* see comment above */

	/* The set of relids (varnos) actually referenced in the clause: */
	Relids		clause_relids;

	/* The set of relids required to evaluate the clause: */
	Relids		required_relids;

	/* If an outer-join clause, the outer-side relations, else NULL: */
	Relids		outer_relids;

	/* The relids used in the clause that are nullable by lower outer joins: */
	Relids		nullable_relids;

	/* These fields are set for any binary opclause: */
	Relids		left_relids;	/* relids in left side of clause */
	Relids		right_relids;	/* relids in right side of clause */

	/* This field is NULL unless clause is an OR clause: */
	Expr	   *orclause;		/* modified clause with RestrictInfos */

	/* This field is NULL unless clause is potentially redundant: */
	EquivalenceClass *parent_ec;	/* generating EquivalenceClass */

	/* cache space for cost and selectivity */
	QualCost	eval_cost;		/* eval cost of clause; -1 if not yet set */
	Selectivity norm_selec;		/* selectivity for "normal" (JOIN_INNER)
								 * semantics; -1 if not yet set; >1 means a
								 * redundant clause */
	Selectivity outer_selec;	/* selectivity for outer join semantics; -1 if
								 * not yet set */

	/* valid if clause is mergejoinable, else NIL */
	List	   *mergeopfamilies;	/* opfamilies containing clause operator */

	/* cache space for mergeclause processing; NULL if not yet set */
	EquivalenceClass *left_ec;	/* EquivalenceClass containing lefthand */
	EquivalenceClass *right_ec; /* EquivalenceClass containing righthand */
	EquivalenceMember *left_em; /* EquivalenceMember for lefthand */
	EquivalenceMember *right_em;	/* EquivalenceMember for righthand */
	List	   *scansel_cache;	/* list of MergeScanSelCache structs */

	/* transient workspace for use while considering a specific join path */
	bool		outer_is_left;	/* T = outer var on left, F = on right */

	/* valid if clause is hashjoinable, else InvalidOid: */
	Oid			hashjoinoperator;	/* copy of clause operator */

	/* cache space for hashclause processing; -1 if not yet set */
	Selectivity left_bucketsize;	/* avg bucketsize of left side */
	Selectivity right_bucketsize;	/* avg bucketsize of right side */
	Selectivity left_mcvfreq;	/* left side's most common val's freq */
	Selectivity right_mcvfreq;	/* right side's most common val's freq */
} RestrictInfo;

/*
 * This macro embodies the correct way to test whether a RestrictInfo is
 * "pushed down" to a given outer join, that is, should be treated as a filter
 * clause rather than a join clause at that outer join.  This is certainly so
 * if is_pushed_down is true; but examining that is not sufficient anymore,
 * because outer-join clauses will get pushed down to lower outer joins when
 * we generate a path for the lower outer join that is parameterized by the
 * LHS of the upper one.  We can detect such a clause by noting that its
 * required_relids exceed the scope of the join.
 */
#define RINFO_IS_PUSHED_DOWN(rinfo, joinrelids) \
	((rinfo)->is_pushed_down || \
	 !bms_is_subset((rinfo)->required_relids, joinrelids))

/*
 * Since mergejoinscansel() is a relatively expensive function, and would
 * otherwise be invoked many times while planning a large join tree,
 * we go out of our way to cache its results.  Each mergejoinable
 * RestrictInfo carries a list of the specific sort orderings that have
 * been considered for use with it, and the resulting selectivities.
 */
typedef struct MergeScanSelCache
{
	/* Ordering details (cache lookup key) */
	Oid			opfamily;		/* btree opfamily defining the ordering */
	Oid			collation;		/* collation for the ordering */
	int			strategy;		/* sort direction (ASC or DESC) */
	bool		nulls_first;	/* do NULLs come before normal values? */
	/* Results */
	Selectivity leftstartsel;	/* first-join fraction for clause left side */
	Selectivity leftendsel;		/* last-join fraction for clause left side */
	Selectivity rightstartsel;	/* first-join fraction for clause right side */
	Selectivity rightendsel;	/* last-join fraction for clause right side */
} MergeScanSelCache;

/*
 * Placeholder node for an expression to be evaluated below the top level
 * of a plan tree.  This is used during planning to represent the contained
 * expression.  At the end of the planning process it is replaced by either
 * the contained expression or a Var referring to a lower-level evaluation of
 * the contained expression.  Typically the evaluation occurs below an outer
 * join, and Var references above the outer join might thereby yield NULL
 * instead of the expression value.
 *
 * Although the planner treats this as an expression node type, it is not
 * recognized by the parser or executor, so we declare it here rather than
 * in primnodes.h.
 */

typedef struct PlaceHolderVar
{
	Expr		xpr;
	Expr	   *phexpr;			/* the represented expression */
	Relids		phrels;			/* base relids syntactically within expr src */
	Index		phid;			/* ID for PHV (unique within planner run) */
	Index		phlevelsup;		/* > 0 if PHV belongs to outer query */
} PlaceHolderVar;

/*
 * "Special join" info.
 *
 * One-sided outer joins constrain the order of joining partially but not
 * completely.  We flatten such joins into the planner's top-level list of
 * relations to join, but record information about each outer join in a
 * SpecialJoinInfo struct.  These structs are kept in the PlannerInfo node's
 * join_info_list.
 *
 * Similarly, semijoins and antijoins created by flattening IN (subselect)
 * and EXISTS(subselect) clauses create partial constraints on join order.
 * These are likewise recorded in SpecialJoinInfo structs.
 *
 * We make SpecialJoinInfos for FULL JOINs even though there is no flexibility
 * of planning for them, because this simplifies make_join_rel()'s API.
 *
 * min_lefthand and min_righthand are the sets of base relids that must be
 * available on each side when performing the special join.  lhs_strict is
 * true if the special join's condition cannot succeed when the LHS variables
 * are all NULL (this means that an outer join can commute with upper-level
 * outer joins even if it appears in their RHS).  We don't bother to set
 * lhs_strict for FULL JOINs, however.
 *
 * It is not valid for either min_lefthand or min_righthand to be empty sets;
 * if they were, this would break the logic that enforces join order.
 *
 * syn_lefthand and syn_righthand are the sets of base relids that are
 * syntactically below this special join.  (These are needed to help compute
 * min_lefthand and min_righthand for higher joins.)
 *
 * delay_upper_joins is set true if we detect a pushed-down clause that has
 * to be evaluated after this join is formed (because it references the RHS).
 * Any outer joins that have such a clause and this join in their RHS cannot
 * commute with this join, because that would leave noplace to check the
 * pushed-down clause.  (We don't track this for FULL JOINs, either.)
 *
 * For a semijoin, we also extract the join operators and their RHS arguments
 * and set semi_operators, semi_rhs_exprs, and semi_can_btree.
 * This is done in support of possibly unique-ifying the RHS, so we don't
 * bother unless semi_can_btree can be set true.  (You might expect that this
 * information would be computed during join planning; but it's helpful to
 * have it available during planning of parameterized table scans, so we store
 * it in the SpecialJoinInfo structs.)
 *
 * jointype is never JOIN_RIGHT; a RIGHT JOIN is handled by switching
 * the inputs to make it a LEFT JOIN.  So the allowed values of jointype
 * in a join_info_list member are only LEFT, FULL, SEMI, or ANTI.
 *
 * For purposes of join selectivity estimation, we create transient
 * SpecialJoinInfo structures for regular inner joins; so it is possible
 * to have jointype == JOIN_INNER in such a structure, even though this is
 * not allowed within join_info_list.  We also create transient
 * SpecialJoinInfos with jointype == JOIN_INNER for outer joins, since for
 * cost estimation purposes it is sometimes useful to know the join size under
 * plain innerjoin semantics.  Note that lhs_strict, delay_upper_joins, and
 * of course the semi_xxx fields are not set meaningfully within such structs.
 */
#ifndef HAVE_SPECIALJOININFO_TYPEDEF
typedef struct SpecialJoinInfo SpecialJoinInfo;
#define HAVE_SPECIALJOININFO_TYPEDEF 1
#endif

struct SpecialJoinInfo
{
	NodeTag		type;
	Relids		min_lefthand;	/* base relids in minimum LHS for join */
	Relids		min_righthand;	/* base relids in minimum RHS for join */
	Relids		syn_lefthand;	/* base relids syntactically within LHS */
	Relids		syn_righthand;	/* base relids syntactically within RHS */
	JoinType	jointype;		/* always INNER, LEFT, FULL, SEMI, or ANTI */
	bool		lhs_strict;		/* joinclause is strict for some LHS rel */
	bool		delay_upper_joins;	/* can't commute with upper RHS */
	/* Remaining fields are set only for JOIN_SEMI jointype: */
	bool		semi_can_btree; /* true if semi_operators are all btree */
	List	   *semi_operators; /* OIDs of equality join operators */
	List	   *semi_rhs_exprs; /* righthand-side expressions of these ops */
};

/*
 * Append-relation info.
 *
 * When we expand an inheritable table or a UNION-ALL subselect into an
 * "append relation" (essentially, a list of child RTEs), we build an
 * AppendRelInfo for each child RTE.  The list of AppendRelInfos indicates
 * which child RTEs must be included when expanding the parent, and each node
 * carries information needed to translate between columns of the parent and
 * columns of the child.
 *
 * These structs are kept in the PlannerInfo node's append_rel_list, with
 * append_rel_array[] providing a convenient lookup method for the struct
 * associated with a particular child relid (there can be only one, though
 * parent rels may have many entries in append_rel_list).
 *
 * Note: after completion of the planner prep phase, any given RTE is an
 * append parent having entries in append_rel_list if and only if its
 * "inh" flag is set.  We clear "inh" for plain tables that turn out not
 * to have inheritance children, and (in an abuse of the original meaning
 * of the flag) we set "inh" for subquery RTEs that turn out to be
 * flattenable UNION ALL queries.  This lets us avoid useless searches
 * of append_rel_list.
 *
 * Note: the data structure assumes that append-rel members are single
 * baserels.  This is OK for inheritance, but it prevents us from pulling
 * up a UNION ALL member subquery if it contains a join.  While that could
 * be fixed with a more complex data structure, at present there's not much
 * point because no improvement in the plan could result.
 */

typedef struct AppendRelInfo
{
	NodeTag		type;

	/*
	 * These fields uniquely identify this append relationship.  There can be
	 * (in fact, always should be) multiple AppendRelInfos for the same
	 * parent_relid, but never more than one per child_relid, since a given
	 * RTE cannot be a child of more than one append parent.
	 */
	Index		parent_relid;	/* RT index of append parent rel */
	Index		child_relid;	/* RT index of append child rel */

	/*
	 * For an inheritance appendrel, the parent and child are both regular
	 * relations, and we store their rowtype OIDs here for use in translating
	 * whole-row Vars.  For a UNION-ALL appendrel, the parent and child are
	 * both subqueries with no named rowtype, and we store InvalidOid here.
	 */
	Oid			parent_reltype; /* OID of parent's composite type */
	Oid			child_reltype;	/* OID of child's composite type */

	/*
	 * The N'th element of this list is a Var or expression representing the
	 * child column corresponding to the N'th column of the parent. This is
	 * used to translate Vars referencing the parent rel into references to
	 * the child.  A list element is NULL if it corresponds to a dropped
	 * column of the parent (this is only possible for inheritance cases, not
	 * UNION ALL).  The list elements are always simple Vars for inheritance
	 * cases, but can be arbitrary expressions in UNION ALL cases.
	 *
	 * Notice we only store entries for user columns (attno > 0).  Whole-row
	 * Vars are special-cased, and system columns (attno < 0) need no special
	 * translation since their attnos are the same for all tables.
	 *
	 * Caution: the Vars have varlevelsup = 0.  Be careful to adjust as needed
	 * when copying into a subquery.
	 */
	List	   *translated_vars;	/* Expressions in the child's Vars */

	/*
	 * This array simplifies translations in the reverse direction, from
	 * child's column numbers to parent's.  The entry at [ccolno - 1] is the
	 * 1-based parent column number for child column ccolno, or zero if that
	 * child column is dropped or doesn't exist in the parent.
	 */
	int			num_child_cols; /* length of array */
	AttrNumber *parent_colnos;	/* array of parent attnos, or zeroes */

	/*
	 * We store the parent table's OID here for inheritance, or InvalidOid for
	 * UNION ALL.  This is only needed to help in generating error messages if
	 * an attempt is made to reference a dropped parent column.
	 */
	Oid			parent_reloid;	/* OID of parent relation */
} AppendRelInfo;

/*
 * Information about a row-identity "resjunk" column in UPDATE/DELETE.
 *
 * It's important for child tables in an inheritance tree to share
 * row-identity columns whenever possible, so as not to chew up too many
 * targetlist columns.  We use these structs to track which identity columns
 * have been requested.  In the finished plan, each of these will give rise
 * to one resjunk entry in the targetlist of the ModifyTable's subplan node.
 *
 * All the Vars stored in RowIdentityVarInfos must have varno ROWID_VAR, for
 * convenience of detecting duplicate requests.  We'll replace that, in the
 * final plan, with the varno of the generating rel.
 *
 * Outside this list, a Var with varno ROWID_VAR and varattno k is a reference
 * to the k-th element of the row_identity_vars list (k counting from 1).
 * We add such a reference to root->processed_tlist when creating the entry,
 * and it propagates into the plan tree from there.
 */
typedef struct RowIdentityVarInfo
{
	NodeTag		type;

	Var		   *rowidvar;		/* Var to be evaluated (but varno=ROWID_VAR) */
	int32		rowidwidth;		/* estimated average width */
	char	   *rowidname;		/* name of the resjunk column */
	Relids		rowidrels;		/* RTE indexes of target rels using this */
} RowIdentityVarInfo;

/*
 * For each distinct placeholder expression generated during planning, we
 * store a PlaceHolderInfo node in the PlannerInfo node's placeholder_list.
 * This stores info that is needed centrally rather than in each copy of the
 * PlaceHolderVar.  The phid fields identify which PlaceHolderInfo goes with
 * each PlaceHolderVar.  Note that phid is unique throughout a planner run,
 * not just within a query level --- this is so that we need not reassign ID's
 * when pulling a subquery into its parent.
 *
 * The idea is to evaluate the expression at (only) the ph_eval_at join level,
 * then allow it to bubble up like a Var until the ph_needed join level.
 * ph_needed has the same definition as attr_needed for a regular Var.
 *
 * The PlaceHolderVar's expression might contain LATERAL references to vars
 * coming from outside its syntactic scope.  If so, those rels are *not*
 * included in ph_eval_at, but they are recorded in ph_lateral.
 *
 * Notice that when ph_eval_at is a join rather than a single baserel, the
 * PlaceHolderInfo may create constraints on join order: the ph_eval_at join
 * has to be formed below any outer joins that should null the PlaceHolderVar.
 *
 * We create a PlaceHolderInfo only after determining that the PlaceHolderVar
 * is actually referenced in the plan tree, so that unreferenced placeholders
 * don't result in unnecessary constraints on join order.
 */

typedef struct PlaceHolderInfo
{
	NodeTag		type;

	Index		phid;			/* ID for PH (unique within planner run) */
	PlaceHolderVar *ph_var;		/* copy of PlaceHolderVar tree */
	Relids		ph_eval_at;		/* lowest level we can evaluate value at */
	Relids		ph_lateral;		/* relids of contained lateral refs, if any */
	Relids		ph_needed;		/* highest level the value is needed at */
	int32		ph_width;		/* estimated attribute width */
} PlaceHolderInfo;

/*
 * At runtime, PARAM_EXEC slots are used to pass values around from one plan
 * node to another.  They can be used to pass values down into subqueries (for
 * outer references in subqueries), or up out of subqueries (for the results
 * of a subplan), or from a NestLoop plan node into its inner relation (when
 * the inner scan is parameterized with values from the outer relation).
 * The planner is responsible for assigning nonconflicting PARAM_EXEC IDs to
 * the PARAM_EXEC Params it generates.
 *
 * Outer references are managed via root->plan_params, which is a list of
 * PlannerParamItems.  While planning a subquery, each parent query level's
 * plan_params contains the values required from it by the current subquery.
 * During create_plan(), we use plan_params to track values that must be
 * passed from outer to inner sides of NestLoop plan nodes.
 *
 * The item a PlannerParamItem represents can be one of three kinds:
 *
 * A Var: the slot represents a variable of this level that must be passed
 * down because subqueries have outer references to it, or must be passed
 * from a NestLoop node to its inner scan.  The varlevelsup value in the Var
 * will always be zero.
 *
 * A PlaceHolderVar: this works much like the Var case, except that the
 * entry is a PlaceHolderVar node with a contained expression.  The PHV
 * will have phlevelsup = 0, and the contained expression is adjusted
 * to match in level.
 *
 * Note: we detect duplicate Var and PlaceHolderVar parameters and coalesce
 * them into one slot.
 * The scope of duplicate-elimination only extends across the set of
 * parameters passed from one query level into a single subquery, or for
 * nestloop parameters across the set of nestloop parameters used in a single
 * query level.  So there is no possibility of a PARAM_EXEC slot being used
 * for conflicting purposes.
 *
 * In addition, PARAM_EXEC slots are assigned for Params representing outputs
 * from subplans (values that are setParam items for those subplans).  These
 * IDs need not be tracked via PlannerParamItems, since we do not need any
 * duplicate-elimination nor later processing of the represented expressions.
 * Instead, we just record the assignment of the slot number by appending to
 * root->glob->paramExecTypes.
 */
typedef struct PlannerParamItem
{
	NodeTag		type;

	Node	   *item;			/* the Var or PlaceHolderVar */
	int			paramId;		/* its assigned PARAM_EXEC slot number */
} PlannerParamItem;

/*
 * When making cost estimates for a SEMI/ANTI/inner_unique join, there are
 * some correction factors that are needed in both nestloop and hash joins
 * to account for the fact that the executor can stop scanning inner rows
 * as soon as it finds a match to the current outer row.  These numbers
 * depend only on the selected outer and inner join relations, not on the
 * particular paths used for them, so it's worthwhile to calculate them
 * just once per relation pair not once per considered path.  This struct
 * is filled by compute_semi_anti_join_factors and must be passed along
 * to the join cost estimation functions.
 *
 * outer_match_frac is the fraction of the outer tuples that are
 *		expected to have at least one match.
 * match_count is the average number of matches expected for
 *		outer tuples that have at least one match.
 */
typedef struct SemiAntiJoinFactors
{
	Selectivity outer_match_frac;
	Selectivity match_count;
} SemiAntiJoinFactors;

/*
 * Struct for extra information passed to subroutines of add_paths_to_joinrel
 *
 * restrictlist contains all of the RestrictInfo nodes for restriction
 *		clauses that apply to this join
 * mergeclause_list is a list of RestrictInfo nodes for available
 *		mergejoin clauses in this join
 * inner_unique is true if each outer tuple provably matches no more
 *		than one inner tuple
 * sjinfo is extra info about special joins for selectivity estimation
 * semifactors is as shown above (only valid for SEMI/ANTI/inner_unique joins)
 * param_source_rels are OK targets for parameterization of result paths
 */
typedef struct JoinPathExtraData
{
	List	   *restrictlist;
	List	   *mergeclause_list;
	bool		inner_unique;
	SpecialJoinInfo *sjinfo;
	SemiAntiJoinFactors semifactors;
	Relids		param_source_rels;
} JoinPathExtraData;

/*
 * For speed reasons, cost estimation for join paths is performed in two
 * phases: the first phase tries to quickly derive a lower bound for the
 * join cost, and then we check if that's sufficient to reject the path.
 * If not, we come back for a more refined cost estimate.  The first phase
 * fills a JoinCostWorkspace struct with its preliminary cost estimates
 * and possibly additional intermediate values.  The second phase takes
 * these values as inputs to avoid repeating work.
 *
 * (Ideally we'd declare this in cost.h, but it's also needed in pathnode.h,
 * so seems best to put it here.)
 */
typedef struct JoinCostWorkspace
{
	/* Preliminary cost estimates --- must not be larger than final ones! */
	Cost		startup_cost;	/* cost expended before fetching any tuples */
	Cost		total_cost;		/* total cost (assuming all tuples fetched) */

	/* Fields below here should be treated as private to costsize.c */
	Cost		run_cost;		/* non-startup cost components */

	/* private for cost_nestloop code */
	Cost		inner_run_cost; /* also used by cost_mergejoin code */
	Cost		inner_rescan_run_cost;

	/* private for cost_mergejoin code */
	double		outer_rows;
	double		inner_rows;
	double		outer_skip_rows;
	double		inner_skip_rows;

	/* private for cost_hashjoin code */
	int			numbuckets;
	int			numbatches;
	double		inner_rows_total;
} JoinCostWorkspace;

#endif							/* PATHNODES_H */
