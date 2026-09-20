fd.c 中删除临时表 请分析这段代码是否可被裁剪，临时表已被裁剪删除

======================================================================
裁剪记录：FROM 中的函数（func_table / ROWS FROM / WITH ORDINALITY）
======================================================================

裁剪范围
--------
彻底移除「FROM 中函数」这一语法族及其后端实现：

- gram.y：删除 table_ref 的 `func_table func_alias_clause` 与
  `LATERAL_P func_table ...` 产生式，删除 func_table / rowsfrom_item /
  rowsfrom_list / opt_col_def_list / opt_ordinality / TableFuncElementList /
  TableFuncElement / func_alias_clause；删除 ROWS、ORDINALITY 关键字。
- 保留：LATERAL（含 LATERAL 子查询及其优化器 lateral 框架）、SELECT 列表中
  的 SRF（ProjectSet）、func_expr_windowless（函数索引仍在使用）。
- 节点层：删除 RangeFunction、RangeTblFunction、FunctionScan、
  FunctionScanState 及 RangeTblEntry.functions/funcordinality、RTE_FUNCTION；
  同步 copy/equal/out/read/walker/mutator。
- 解析层：删除 transformRangeFunction、addRangeTableEntryForFunction、
  chooseScalarFunctionAlias、EXPR_KIND_FROM_FUNCTION，并去掉
  makeWholeRowVar 的 allowScalar 参数与 expandTupleDesc 的 offset 参数。
- 优化器：删除 set_function_pathlist/set_function_size_estimates、
  cost_functionscan、create_functionscan_path、create_functionscan_plan、
  inline_set_returning_function、pull_up_simple_function 相关
  （preprocess_function_rtes、pull_up_constant_function）及各处 RTE_FUNCTION 分支。
- 执行器：删除 nodeFunctionscan.c/.h、
  ExecInitTableFunctionResult/ExecMakeTableFunctionResult、T_FunctionScan 分发、
  Explain 的 Function Scan 分支、ExecMaterializesOutput/cost_rescan 的对应分支。
- 工具层：清理 ruleutils.c、queryjumble.c、dependency.c、rewriteHandler.c、
  print.c、makefuncs.c、subselect.c、setrefs.c 中的 RTE_FUNCTION 消费点。
- 顺带清理死代码/告警：execAmi.c 无定义的 IndexSupportsBackwardScan 原型、
  ExecSupportsBackwardScan 缺失的 default 分支、nodeFuncs.c 未处理的
  RTE_RELATION。

为什么可裁剪
------------
1. FROM 中函数只是「SRF 的一种调用写法」，SRF 本体（目标列 SRF / ProjectSet）
   完整保留，用子查询目标列照样能调用集返回函数，功能损失有限。
2. ROWS FROM / WITH ORDINALITY / 函数的列定义列表属于高级形态，学习价值中等，
   可用 VALUES 列表或子查询展开替代。
3. LATERAL 的优化器框架（lateral_relids/lateral_referencers/ph_lateral 等）
   牵连 20+ 文件，故本次保留 LATERAL，避免高风险大重构。

连带影响（必须同步处理）
------------------------
- 系统视图：pg_locks、pg_settings、pg_file_settings、pg_config、
  pg_shmem_allocations、pg_backend_memory_contexts、pg_prepared_xacts、
  pg_available_extensions(_versions)、pg_stat_slru、pg_stat_archiver、
  pg_stat_wal、pg_stat_activity、pg_stat_progress_* 原本依赖 FROM 函数，
  在 system_views.sql 中改写为
  `FROM (SELECT (func(...)).* ) AS x` 形式以保住这些视图。
- psql：describe.c 的 `\d+` 内部 SQL 使用 `FROM pg_catalog.unnest(...)`，
  同样改写为子查询展开形式。
- contrib（未纳入构建）与 isolation/regress 用例中的 FROM 函数用法一并改写。

测试与验证
----------
- regress：改写 20 个用例（arrays/btree_index/hash_index/bitmapops/
  select_distinct/subselect/text/int8/float8/pg_lsn/timestamp/timestamptz/
  mvcc/index_including/tidrangescan/vacuum_parallel/tsrf/create_view/
  opr_sanity/misc_functions），并同步 expected；`make check-world` 全部通过
  （regress 68/68、isolation 26/26）。
- 全量干净重编译：0 error、0 warning。
