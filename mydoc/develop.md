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

======================================================================
裁剪记录：bgworker 后台工作者框架 + postmaster 配套逻辑（批次C）
======================================================================
裁剪范围
--------
彻底移除可插拔后台工作者（background worker）框架及其在 postmaster 中的
配套逻辑（批次A+B 提交中保留的"bgworker 框架（批次C暂不裁）"）：

- 删除文件：postmaster/bgworker.c（1291 行）、include/postmaster/bgworker.h、
  include/postmaster/bgworker_internals.h；postmaster/Makefile 去掉 bgworker.o。
- postmaster.c：删除 BackgroundWorkerList 及调度逻辑（maybe_start_bgworkers /
  do_start_bgworker / bgworker_should_start_now / assign_backendlist_entry /
  CleanupBackgroundWorker / PostmasterMarkPIDForWorkerNotify /
  BackgroundWorkerInitializeConnection{,ByOid} / BackgroundWorker{Block,Unblock}Signals）、
  Backend 的 bgworker_notify/bkend_type 字段、BACKEND_TYPE_* 宏、
  StartWorkerNeeded/HaveCrashedWorker 唤醒变量、PMSIGNAL_BACKGROUND_WORKER_CHANGE
  信号处理、DetermineSleepTime 的 crashed-worker 扫描；SignalSomeChildren /
  CountChildren / canAcceptConnections 相应简化为无类型参数。
  （注意：background writer/bgwriter 是另一套机制，BgWriterPID 等全部保留。）
- 基础设施：ipci.c 的 BackgroundWorkerShmem{Size,Init}、pmsignal.h 枚举项、
  lwlocknames.txt 的 BackgroundWorkerLock（留编号空洞 "# 33 was ..."）、
  wait_event.{h,c} 的 BGWORKER_STARTUP/SHUTDOWN 等待事件、
  postmaster.h 的 PostmasterMarkPIDForWorkerNotify 声明。
- 进程槽位：PGPROC.isBackgroundWorker 字段、ProcGlobal.bgworkerFreeProcs 链表、
  proc.c/procarray.c/twophase.c 中相关赋值与过滤；B_BG_WORKER 后端类型枚举、
  IsBackgroundWorker 全局变量，以及 postgres.c / elog.c / pgstatfuncs.c /
  backend_status.c / miscinit.c 中的消费点。
- GUC：删除 max_worker_processes 与 max_parallel_workers（随框架失去唯一
  消费者），并同步清理 control file 字段（PG_CONTROL_VERSION 1300→1301）、
  xl_parameter_change 记录、xlog.c 参数变更与热备校验、xlogdesc.c、
  pg_controldata.c、postgresql.conf.sample；guc.c 的 check_max_worker_processes
  删除，check_maxconnections 简化；MaxBackends = MaxConnections + 1。
- 并行残件：parallel.c 的 ParallelWorkerMain（bgworker 的 InternalBGWorkers
  表是其唯一入口）及 parallel.h 声明删除；max_logical_replication_workers /
  max_sync_workers_per_subscription 死配置从 postgresql.conf.sample 清理。
- 连带清理：vacuum.c 无用 include、freelist.c 注释笔误（Bgworker→Bgwriter）、
  dsm.c / xlogutils.c / procarray.c 中的过时注释。

为什么可裁剪
------------
1. 框架在 minipg 中已无任何调用者：RegisterBackgroundWorker（静态注册）与
   RegisterDynamicBackgroundWorker（动态注册）除框架自身外零引用；并行查询、
   逻辑复制、contrib 扩展均已裁剪，运行期 worker 列表恒为空。
2. bgworker 是可插拔扩展基础设施而非数据库内核主线：事务、索引、查询执行、
   buffer 管理、辅助进程管理等核心路径均不依赖它，裁剪不影响学习价值。
3. max_worker_processes 的唯一语义是给 bgworker 预留 PGPROC 槽位，框架裁剪
   后成为死配置，故一并删除。

测试与验证
----------
- 全量干净重编译：0 error、0 warning。
- `make check-world` 全部通过（regress 66/66、isolation 23/23）。
- 净删除约 2500 行（36 个文件）。
