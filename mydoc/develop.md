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

======================================================================
重要：本项目 configure 未启用 --enable-depend，无 .deps 依赖跟踪
======================================================================
`build.sh` 使用 `./configure --prefix=... --enable-debug`（没有 --enable-depend），
因此 **修改任何头文件后 `make` 不会自动重编译受影响的 .o**，增量构建会得到
"新旧混编" 的二进制，症状可能是莫名其妙的段错误（例如旧版 nbtsort.o 里
tuplesort_begin_index_btree 还是隐式声明，返回值被截断成 32 位）。

结论：**改动头文件后必须 `make clean && make -j`，再执行 make check-world**。
（`make clean` 末尾删除 tmp_install 时可能被安全策略拦截，手工 `rm -rf tmp_install`
后重跑即可。）


======================================================================
裁剪记录：contrib 扩展目录彻底裁剪
======================================================================
裁剪范围
--------
- 删除 contrib/pageinspect、pg_buffercache、pg_freespacemap、pg_visibility
  （源码 + sql + expected + control，共 4 个目录）。
- contrib/Makefile 的 SUBDIRS 维持为空并补注说明；contrib/README 改写为裁剪说明。
- 保留 contrib/contrib-global.mk：src/test/modules/test_misc/Makefile 仍引用它。

为什么可裁剪
------------
扩展的安装脚本本质是 CREATE FUNCTION / CREATE AGGREGATE，而 CREATE FUNCTION
DDL 已在 FUNCTION/PROCEDURE 裁剪中移除，这些扩展无法安装；contrib 也不参与构建，
源码目录属于纯残留。

测试与验证
----------
- 全量干净重编译：0 error、0 warning；make check-world 通过（66/66、23/23）。


======================================================================
裁剪记录：陈旧回归测试资产清理
======================================================================
裁剪范围
--------
- 删除 sql/random.sql + expected/random.out：用例使用 CREATE TABLE AS（已裁剪），
  在当前语法下无法运行。
- 删除 sql/hs_standby_{check,allowed,disallowed}.sql 与对应 expected：
  依赖已删除的 hs_primary_setup.sql（主库预置脚本）与 src/test/recovery 驱动，
  且用例本身使用已裁剪的 GRANT/REVOKE。
- 删除 standby_schedule 与 GNUmakefile 的 standbycheck 目标。
- 删除 GNUmakefile 的 bigtest/bigcheck 目标：唯一用例 numeric_big 随 numeric
  数据类型裁剪而消失。

为什么可裁剪
------------
这些资产在当前裁剪后的语法/基础设施下**无法执行**（不是"少覆盖"，而是"跑不起来"），
留着只会误导；hot standby 本身的代码路径仍完整保留。

测试与验证
----------
- make check-world：regress 66/66、isolation 23/23。


======================================================================
裁剪记录：interval 类型残留与 date/time 死代码
======================================================================
裁剪范围
--------
- datetime.c：删除死函数 ClearPgTm()；删除 DecodeInterval / DecodeISO8601Interval /
  EncodeInterval 三处悬空注释块；删除 DecodeTime() 的 range 形参（两个调用点恒传
  INTERVAL_FULL_RANGE，`range == INTERVAL_MASK(MINUTE)|INTERVAL_MASK(SECOND)`
  的 mm:ss 分支永不可达）及 INTERVAL_FULL_RANGE/INTERVAL_MASK 两处调用；
  删除 DateTimeParseError() 中无生产者的 DTERR_INTERVAL_OVERFLOW 分支。
- timestamp.c：删除 11 处悬空注释块（interval_justify_*、timestamp_pl_interval、
  timestamptz_pl_interval、interval_abs 说明、interval_accum/avg、
  timestamp/timestamptz_age、timestamp_bin、interval_trunc、timestamp_izone、
  generate_series_timestamp/timestamptz）。
- date.c：删除 7 处悬空注释块（date_pl_interval、date_mi_interval、time_interval、
  interval_time、time_mi_time、time_pl_interval、time_mi_interval）。
- datetime.h：删除 DAGO 字符串宏、AGO/ABS_BEFORE/ABS_AFTER/ISODATE/WEEK/DECADE/
  CENTURY/MILLENNIUM/JULIAN 字段类型、DTK_AGO/DTK_DELTA、DTERR_INTERVAL_OVERFLOW；
  deltatktbl 中删除仅供 interval 使用的 "@" / "ago" 词条。
- timestamp.h：删除 INTERVAL_RANGE_MASK/INTERVAL_FULL_PRECISION/
  INTERVAL_PRECISION_MASK/INTERVAL_TYPMOD/INTERVAL_PRECISION/INTERVAL_RANGE/
  TIMESTAMP_MASK 等无引用宏；文件头注释改写。

为什么可裁剪
------------
interval 数据类型已裁（pg_type.dat 无 interval、pg_proc.dat 无 interval 函数），
上列注释/常量/形参均已无任何消费者；DecodeTime 的 range 形参恒为同一取值，
其唯一分支不可达。

测试与验证
----------
- 全量干净重编译：0 error、0 warning；make check-world 通过（66/66、23/23）。


======================================================================
裁剪记录：extended-stats 与 logical-replication origin 残留
======================================================================
裁剪范围
--------
- 删除 src/include/statistics/extended_stats_internal.h（连同 statistics/ 目录）：
  其中仅度量的 StdAnalyzeData / ScalarItem 在生产代码中只有 analyze.c 使用，
  已内联到 analyze.c 顶部；MultiSortSupport/SortItem/multi_sort_* /
  compare_*_simple 等扩展统计专用声明无任何引用，一并消失。
- 删除 src/include/replication/origin.h（连同 replication/ 目录）：
  InvalidRepOriginId 迁移到 access/xlogdefs.h（RepOriginId 定义处），
  DoNotReplicateId 无引用直接删除；5 处 #include 清理。
- src/include/Makefile 的 SUBDIRS 去掉 replication（statistics 原本不在其中）。
- 优化器：删除 use_extended_stats 形参链路（optimizer.h 的
  clause_selectivity_ext / clauselist_selectivity_ext 声明、
  clausesel.c 的 *_ext 变体、estimatedclauses 死变量），合并回
  clause_selectivity / clauselist_selectivity / clauselist_selectivity_or。

为什么可裁剪
------------
扩展统计（CREATE STATISTICS / pg_statistic_ext）已裁，优化器里没有任何
statext_* 消费者，use_extended_stats 一路传递但永远不产生效果；
estimatedclauses 只被读取、从不被写入。

测试与验证
----------
- 全量干净重编译：0 error、0 warning；make check-world 通过（66/66、23/23）。


======================================================================
裁剪记录：commands 空壳文件与全树头文件死声明
======================================================================
裁剪范围
--------
- opclasscmds.c：删除无调用者的 IsThereOpClassInNamespace /
  IsThereOpFamilyInNamespace；include 收敛到实际所需 8 个；文件头注释改写。
- typecmds.c / operatorcmds.c / amcmds.c：删除各自的悬空注释（AlterType、
  DefineFoo 描述等），文件头注释改为如实描述"只保留删除/命名空间/查找辅助"。
- cluster.c 及其调用链（VACUUM FULL 简化，重要）：
  * 删除退化代码块 `if (recheck) { { relation_close(); goto out; } ... }`——
    属主检查被早先裁剪清空后，该块变成"recheck 为真就静默什么都不做"的隐患；
    连同 CLUOPT_RECHECK 标志一起删除。
  * 删除 check_index_is_clusterable() / mark_index_clustered()（CLUSTER 专属，已无调用者）。
  * cluster_rel() / rebuild_relation() / copy_table_data() 去掉 indexOid 形参
    （唯一调用者 vacuum.c 恒传 InvalidOid），删除 indexscan / use_sort /
    "clustering ... using index scan" 等不可达分支；表重写固定走顺序扫描。
  * 删除 plan_cluster_use_sort()（planner.c 112 行，已无调用者）与 optimizer.h 声明。
  * tablecmds.c：删除 RememberClusterOnForRebuilding()（其输出 clusterOnIndex
    从不被消费）与 AlteredTableInfo.clusterOnIndex 字段；
    删除 get_index_isclustered()（lsyscache.c/.h，随上面删除变为无引用）。
- 头文件死声明清理（声明在全树无任何定义）：access/xlog.h 的
  CalculateMaxmumSafeLSN、StartupRequestWalReceiverRestart；
  access/xlogreader.h 的 LocalXLogReaderRoutine；
  catalog/pg_constraint.h 的 ConstraintSetParentConstraint；
  commands/defrem.h 的 StatisticsGetRelation、serialize_deflist、
  deserialize_deflist、IsThereOpClass/OpFamilyInNamespace；
  commands/explain.h 的 ExplainQueryText；
  commands/schemacmds.h 的 AlterSchemaOwner(_oid)；
  commands/typecmds.h 的 AlterTypeOwner/_oid/AlterTypeOwnerInternal；
  executor/executor.h 的 ExecGetTriggerResultRel（并修正 execnodes.h 引用它的注释）；
  executor/node{Agg,Sort,IncrementalSort}.h 的 *RetrieveInstrumentation；
  optimizer/cost.h 的 cost_recursive_union；optimizer/plancat.h 的
  infer_arbiter_indexes；optimizer/subselect.h 的 SS_process_ctes；
  utils/builtins.h 的 pg_inet_cidr_ntop、pg_inet_net_pton、network_scan_first/last、
  numeric_round、numeric_float8_no_overflow；
  utils/sortsupport.h 的 PrepareSortSupportFromGistIndexRel；
  utils/tuplesort.h 的 tuplesort_begin_index_gist。

为什么可裁剪
------------
1. 上列函数/声明对应的语法（CLUSTER、ALTER TYPE OWNER、CREATE OPERATOR CLASS、
   GiST、inet/cidr、numeric、触发器、继承、ON CONFLICT、递归 UNION、CTE、
   并行查询）均已在前序批次裁剪，声明成为纯残留。
2. cluster.c 的 indexOid 形参自 CLUSTER 裁剪后恒为 InvalidOid，其分支全部不可达；
   recheck 块更是被裁出"静默跳过 VACUUM FULL"的退化行为，必须清除。
3. 说明：这些文件（amcmds/operatorcmds/portalcmds/typecmds/opclasscmds/alter/
   tablespace/cluster）里剩下的都是**活代码**（被 objectaddress/删除流程/ALTER SET
   SCHEMA/VACUUM FULL 调用），因此保留文件、只清死代码；不把它们合并进其它文件，
   以保持与上游 PG 的文件结构对应关系，便于对照学习。

测试与验证
----------
- 全量干净重编译：0 error、0 warning；make check-world 通过（66/66、23/23）。


======================================================================
裁剪记录：DSM 动态共享内存 + 并行排序基础设施（本批次最大项）
======================================================================
裁剪范围
--------
- 删除文件：storage/ipc/dsm.c、dsm_impl.c、include/storage/dsm.h、dsm_impl.h、
  storage/file/sharedfileset.c、include/storage/sharedfileset.h。
- buffile.c/.h：删除共享 BufFile 全部实现（BufFileCreateShared/OpenShared/
  DeleteShared/ExportShared/TruncateShared、BufFileSize、BufFileAppend、
  SharedSegmentName、MakeNewSharedSegment）与 fileset/name 字段。
- logtape.c/.h：删除 ltsConcatWorkerTapes、TapeShare、LogicalTape.offsetBlockNumber、
  LogicalTapeSet.nHoleBlocks；LogicalTapeSetCreate 去掉
  (shared, fileset, worker) 三个形参、LogicalTapeFreeze 去掉 share 形参。
- tuplesort.c/.h：删除 Sharedsort、SortCoordinate/SortCoordinateData、
  PARALLEL_SORT/SERIAL/WORKER/LEADER 宏、state 的 worker/shared/nParticipants 字段、
  worker_get_identifier/worker_freeze_result_tape/worker_nomergeruns/
  leader_takeover_tapes、tuplesort_estimate_shared/initialize_shared/attach_shared、
  TRACE 探针中的 parallel 参数、各 elog 中的 worker %d 前缀；
  5 个 tuplesort_begin_* 去掉 coordinate 形参（10 个调用点同步修改）。
- 连带清理：heapam_handler.c（tuplesort_begin_cluster 调用）、hashsort.c、
  nbtsort.c（含 coordinate/coordinate2 变量）、nodeAgg.c（含
  LogicalTapeSetCreate 调用）、nodeSort.c、nodeIncrementalSort.c、catalog/index.c；
  inittapes 去掉已无调用者的 mergeruns 形参。
- DSM 消费者清理：ipc.c（dsm_backend_shutdown/reset_on_dsm_detach）、
  ipci.c（dsm_estimate_size/dsm_shmem_init/dsm_postmaster_startup）、
  sysv_shmem.c（dsm_cleanup_using_control_segment、hdr->dsm_control）、
  pgstat.c/syslogger.c（dsm_detach_all）、
  resowner.c/.h（ResourceOwner{Enlarge,Remember,Forget}DSM 与 dsmarr 数组）、
  PGShmemHeader.dsm_control 字段、heapam.h/typcache.h/guc.c/planner.c 的无用 include。
- GUC/工具连带：删除 dynamic_shared_memory_type 与 min_dynamic_shared_memory
  两个 GUC（含 postgresql.conf.sample 注释、dynamic_shared_memory_options 声明）、
  wait_event 的 WAIT_EVENT_DSM_FILL_ZERO_WRITE、initdb 的
  choose_dsm_implementation() 与 pg_dynshmem 子目录创建。

为什么可裁剪
------------
1. 并行查询已裁：tuplesort_initialize_shared/attach_shared 全树零调用者，
   SharedFileSet 的唯一消费者是 tuplesort 的并行排序路径（已不可达），
   dsm_create/dsm_attach/dsm_segment_address 等对外接口同样零调用者。
   因此整条 dsm → sharedfileset → buffile/logtape/tuplesort 的共享链路全部是死代码。
2. DSM 只服务并行查询/并行建索引/并行 vacuum，不是数据库内核主线（事务、索引、
   查询执行、buffer 管理均不依赖），学习价值低。
3. PGShmemHeader.dsm_control 不在磁盘上（仅共享内存中的头），删除安全。

测试与验证
----------
- 全量干净重编译：0 error、0 warning；make check-world 通过（regress 66/66、
  isolation 23/23）。
- 踩坑记录：本批次曾因缺少依赖跟踪（见文首）导致增量构建出"旧 nbtsort.o + 新
  tuplesort.h"的混编二进制，initdb 在建索引时段错误（返回值被截断为 32 位）；
  执行 make clean 全量重编译后消失。这也验证了必须全量重编译。


======================================================================
裁剪记录：libpq 扩展协议不一致的处理（部分裁剪 + 待定项）
======================================================================
背景
----
minipg 后端只实现简单查询协议：tcop/postgres.c 的 PostgresMain 主循环只处理
'Q'（简单查询）、EOF、'X'（终止），exec_parse_message / exec_bind_message /
exec_execute_message 全树零命中；而 libpq 仍导出 PQexecParams / PQprepare /
PQexecPrepared / PQdescribe* 等走 Parse/Bind/Execute 的 API，调用会被后端 FATAL。

处理
----
- 删除 src/test/examples/testlibpq3.c 与 testlibpq3.sql：该示例的唯一主题就是
  PQexecParams（行外参数 + 二进制结果格式），在后端不可用；并在
  src/test/examples/Makefile 的 PROGS 中移除（examples 本就不参与默认构建）。
- 其余两个示例（testlibpq.c、testlibpq4.c）与 psql 都只用简单协议，正常。
- src/test/isolation/isolationtester.c 早已绕过该限制（把查询拆成 prefix/suffix），
  isolation 23/23 通过。

待定项（需要决策，未在本批次处理）
--------------------------------
libpq 侧的参数化 API 与 fe-protocol3.c 中 '1'/'2'/'3'/'t'/'n'/'s' 等扩展协议
响应分支（约 600~900 行，涉及 libpq-fe.h 公开符号与连接状态机）尚未删除，原因是
两条路线各有代价，需明确取舍：
  (a) 裁掉 libpq 参数化 API：彻底，但改动公开 API/ABI，且 minipg 的 libpq 与上游
      差异变大，不利于对照学习；
  (b) 恢复后端扩展协议：能力更完整，但要恢复 Parse/Bind/Execute 与参数机制，
      与"只保留核心"的目标冲突。
建议：若要继续裁剪，按 (a) 作为独立批次，范围 = fe-exec.c 的
PQsendQueryGuts/PQsendQueryParams/PQsendPrepare/PQsendQueryPrepared/PQexecParams/
PQprepare/PQexecPrepared/PQdescribePrepared/PQdescribePortal/PQsendDescribe*、
libpq-fe.h 对应声明、libpq-int.h 的 cmd_queue 与 CONNSTATE_PARSE/BIND/EXECUTE、
fe-protocol3.c 的 '1'/'2'/'3'/'t'/'n'/'s' 分支；每步需全量重编译 + check-world。
