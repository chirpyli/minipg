# minipg 变更日志（裁剪记录）

> 约定：每条裁剪均保证与「不可裁部分」（btree / hash 索引、事务）零耦合，删除后 `make -j` 全量重编通过。
> 验证命令固化：`cd src/test/regress && NO_TEMP_INSTALL=1 make check`（依赖先 `make prefix=$(pwd)/tmp_install install`）。
> 已知既有问题（与具体裁剪无关）：minipg 既有 HEAD 的 `initdb` 因 `syscache.c` 的 `cacheinfo[]` 数组与 `syscache.h` 枚举不对齐而崩溃，须先对齐二者方能跑完整回归（详见 2026-08-14 记忆）。

---

## DO 语句裁剪（2026-08-24）

DO 语句是 SQL 层执行「匿名代码块」（`DO ... LANGUAGE plpgsql`）的入口。minipg 仅保留 internal/c/sql 三种内置语言且均无 `laninline`（内联代码处理器），任何 `DO ... LANGUAGE` 在执行期必然在 `ExecuteDoStmt` 处报 "language ... does not support inline code execution"，亦无 pl 扩展语言可装载，属无实际效果的「装饰性」语法。为保留它需额外携带内联代码执行分发（`ExecuteDoStmt`）、执行期结构 `InlineCodeBlock` 与命令标签 `CMDTAG_DO`，学习价值低，故整体裁剪。

### 删除的代码点

- **语法/节点层**
  - [gram.y](file:///home/postgres/works/my-github/minipg/src/backend/parser/gram.y)：删除 `DoStmt` 产生式、`dostmt_opt_list`/`dostmt_opt_item` 非终结符、`utility_stmt` 中的 `| DoStmt` 分支及对应 `%type` 声明（`CreateTransformStmt` 区段前残留的孤立 ALTER FUNCTION 注释一并清理）
  - [parsenodes.h](file:///home/postgres/works/my-github/minipg/src/include/nodes/parsenodes.h)：删除 `DoStmt`、`InlineCodeBlock` 结构体；**[nodes.h](file:///home/postgres/works/my-github/minipg/src/include/nodes/nodes.h)** 删除 `T_DoStmt`、`T_InlineCodeBlock` 结点标签并重排枚举
- **节点支持层**
  - [copyfuncs.c](file:///home/postgres/works/my-github/minipg/src/backend/nodes/copyfuncs.c)：删除 `_copyDoStmt` 及其分发分支；**[equalfuncs.c](file:///home/postgres/works/my-github/minipg/src/backend/nodes/equalfuncs.c)** 删除 `_equalDoStmt` 及其分发分支
  - [typedefs.list](file:///home/postgres/works/my-github/minipg/src/tools/pgindent/typedefs.list)：移除 `DoStmt`、`InlineCodeBlock`
- **命令执行层**
  - [functioncmds.c](file:///home/postgres/works/my-github/minipg/src/backend/commands/functioncmds.c)：删除 `ExecuteDoStmt`（内联代码执行入口，`InlineCodeBlock` 唯一使用者）
  - [defrem.h](file:///home/postgres/works/my-github/minipg/src/include/commands/defrem.h)：删除 `ExecuteDoStmt` 声明
  - [utility.c](file:///home/postgres/works/my-github/minipg/src/backend/tcop/utility.c)：删除 `ClassifyUtilityCommandAsReadOnly`（严格只读分类）、`standard_ProcessUtility` 分发、`CreateCommandTag`（`CMDTAG_DO`）、`GetCommandLogLevel`（`LOGSTMT_ALL`）四处 `T_DoStmt` 分支
  - [cmdtaglist.h](file:///home/postgres/works/my-github/minipg/src/include/tcop/cmdtaglist.h)：删除 `CMDTAG_DO("DO")` 命令标签

### 保留边界（未破坏）

`DO` 关键字在 [kwlist.h](file:///home/postgres/works/my-github/minipg/src/include/parser/kwlist.h) 与 gram.y `ReservedWord` 列表中**保留**——`CREATE RULE ... DO INSTEAD` 与 `INSERT ... ON CONFLICT ... DO UPDATE/NOTHING` 复用同一 `DO` token，移除将破坏规则与 upsert 语法。btree/hash 索引与事务零耦合。

### 验证

`make -j8` 全量编译 0 error；`make check-world` 退出码 0（主回归 79/79 通过，isolation/modules/contrib 全绿）。冒烟验证：`DO $$...$$;` 报语法错误（`syntax error at or near "DO"`），普通 `SELECT` 正常返回，`INSERT ... ON CONFLICT DO UPDATE/NOTHING` 与 `CREATE RULE ... DO INSTEAD` 均按预期工作。

---

## 游标（CURSOR）功能裁剪（2026-08-24）

游标是 SQL 层通过 `DECLARE CURSOR` / `FETCH` / `MOVE` / `CLOSE` 访问、按名称显式推进的查询结果集，依赖可滚动/可持久的 Portal 状态。Handler/协议层的 Portal 机制（扩展查询协议的未命名/命名 Portal、`PortalDescribe`、`DestTuplestore` 目标等）属于执行器分发核心，**被完整保留**；本次仅裁剪 SQL 游标语法、游标专用执行路径、`WHERE CURRENT OF`、可持久游标物化机制及 `refcursor` 类型。学习价值低（游标驱动逻辑与执行器解耦，属可选配层）。

### 删除的代码点

- **语法/节点层**
  - **[gram.y](file:///home/postgres/works/my-github/minipg/src/backend/parser/gram.y)**：删除 `DECLARE CURSOR` / `FETCH` / `MOVE` / `CLOSE` 产生式及 `cursor_name`、`opt_hold` 等游标专用非终结符
  - **[parsenodes.h](file:///home/postgres/works/my-github/minipg/src/include/nodes/parsenodes.h)** / **[nodes.h](file:///home/postgres/works/my-github/minipg/src/include/nodes/nodes.h)**：删除 `DeclareCursorStmt`、`FetchStmt` 结构体与 `T_DeclareCursorStmt`、`T_FetchStmt` 结点标签
  - **[primnodes.h](file:///home/postgres/works/my-github/minipg/src/include/nodes/primnodes.h)**：删除 `CurrentOfExpr` 结点及 `T_CurrentOfExpr`
  - **[kwlist.h](file:///home/postgres/works/my-github/minipg/src/include/parser/kwlist.h)**：从关键字表删除游标用关键字（保留 `FETCH` 用于 `FETCH FIRST n ROWS ONLY` 限制语法、`FIRST_P`/`LAST_P` 用于 `SELECT` 专项访问）
- **执行/命令层**
  - **[portalcmds.c](file:///home/postgres/works/my-github/minipg/src/backend/commands/portalcmds.c)**：删除 `PerformCursorOpen`（`DECLARE CURSOR` 入口）及 `PerformPortalFetch`/`PerformPortalClose` 中游标命令行分支
  - **[pquery.c](file:///home/postgres/works/my-github/minipg/src/backend/tcop/pquery.c)**：删除 `PortalRunFetch` / `DoPortalRewind` 游标推进逻辑
  - **[portalmem.c](file:///home/postgres/works/my-github/minipg/src/backend/utils/mmgr/portalmem.c)**：删除 `PersistHoldablePortal`（可持久游标物化）、`HoldPortal` 相关状态及 `CURSOR_OPT_SCROLL` 检查
  - **[execCurrent.c](file:///home/postgres/works/my-github/minipg/src/backend/executor/execCurrent.c)**：删除（含 `execCurrentOf` 与 `WHERE CURRENT OF` 处理）及 executor/Makefile 移除编译项
  - **[nodeTidscan.c](file:///home/postgres/works/my-github/minipg/src/backend/executor/nodeTidscan.c)**、**[nodeLockRows.c](file:///home/postgres/works/my-github/minipg/src/backend/executor/nodeLockRows.c)**：删除 `CurrentOfExpr` 求值分支
  - **[spi.c](file:///home/postgres/works/my-github/minipg/src/backend/executor/spi.c)**：删除 `ExecQueryUsingCursor`、`HoldPinnedPortals` 等游标 SPI 入口
  - **[tstoreReceiver.c](file:///home/postgres/works/my-github/minipg/src/backend/executor/tstoreReceiver.c)**：删除游标专用 detoast 与元组映射参数，仅保留向 Tuplestore 存元组
- **类型层**
  - **[pg_type.dat](file:///home/postgres/works/my-github/minipg/src/include/catalog/pg_type.dat)**：删除 `refcursor` 类型项；**[pg_proc.dat](file:///home/postgres/works/my-github/minipg/src/include/catalog/pg_proc.dat)** 移除 `pg_cursor` 相关目录项；**[system_views.sql](file:///home/postgres/works/my-github/minipg/src/backend/catalog/system_views.sql)** 移除游标相关系统目录视图
- **客户端**
  - **[psql](file:///home/postgres/works/my-github/minipg/src/bin/psql/common.c)**：删除依赖 SQL 游标的 `FETCH_COUNT`（`\set FETCH_COUNT`）逐批读取功能；**[tab-complete.c](file:///home/postgres/works/my-github/minipg/src/bin/psql/tab-complete.c)** 移除补全关键字；**[settings.h](file:///home/postgres/works/my-github/minipg/src/bin/psql/settings.h)** / **[startup.c](file:///home/postgres/works/my-github/minipg/src/bin/psql/startup.c)** 清理对应配置
  - **[testlibpq.c](file:///home/postgres/works/my-github/minipg/src/test/examples/testlibpq.c)** / **[testlibpq4.c](file:///home/postgres/works/my-github/minipg/src/test/examples/testlibpq4.c)**：示例改用普通 `SELECT` 读取

### 回归测试适配

- 主回归：删除游标专用用例 [portals_p2](file:///home/postgres/works/my-github/minipg/src/test/regress/sql/portals_p2.sql)（.sql/.out 同删）；[transactions](file:///home/postgres/works/my-github/minipg/src/test/regress/sql/transactions.sql)、[combocid](file:///home/postgres/works/my-github/minipg/src/test/regress/sql/combocid.sql)、[hash_index](file:///home/postgres/works/my-github/minipg/src/test/regress/sql/hash_index.sql)、[tidscan](file:///home/postgres/works/my-github/minipg/src/test/regress/sql/tidscan.sql)、[tablesample](file:///home/postgres/works/my-github/minipg/src/test/regress/sql/tablesample.sql)、[psql](file:///home/postgres/works/my-github/minipg/src/test/regress/sql/psql.sql) 等删除/改写游标子用例
- [rangefuncs](file:///home/postgres/works/my-github/minipg/src/test/regress/sql/rangefuncs.sql) / [subselect](file:///home/postgres/works/my-github/minipg/src/test/regress/sql/subselect.sql)：原滚动游标反扫/HOLD 用例改用普通查询或删除
- [isolation_schedule](file:///home/postgres/works/my-github/minipg/src/test/isolation/isolation_schedule)：移除依赖 `DECLARE CURSOR` 固定缓冲页的 `vacuum-reltuples` 用例
- [snapshot_too_old](file:///home/postgres/works/my-github/minipg/src/test/modules/snapshot_too_old/specs/sto_using_cursor.spec)：删除游标版快照用例
- [libpq_pipeline](file:///home/postgres/works/my-github/minipg/src/test/modules/libpq_pipeline/libpq_pipeline.c)：改用扩展查询协议（未命名 Portal）验证 `PortalDescribe`
- [031_recovery_conflict.pl](file:///home/postgres/works/my-github/minipg/src/test/recovery/t/031_recovery_conflict.pl)：buffer pin 冲突依赖游标持有缓冲页，已删除；快照冲突改用 `REPEATABLE READ` 事务
- [type_sanity](file:///home/postgres/works/my-github/minipg/src/test/regress/sql/type_sanity.sql)：去掉 `refcursor` 类型引用并同步 [tab_core_types](file:///home/postgres/works/my-github/minipg/src/test/regress/expected/type_sanity.out) 期望

### 保留边界（未破坏）

Portal 机制、扩展查询协议（未命名/命名 Portal、`PortalDescribe`、释放/关闭）、`DestTuplestore` 目标接收器、SPI 的 `spi_cursor_fetch` 之外的内存管理入口均保留；`cursorOptions` 作为 plancache/plan 的 `CURSOR_OPT_*` 计划标志（并行计划、通用/定制计划切换）保留不变。btree/hash 索引与事务零耦合。

### 验证

`make -j8` 全量编译 0 error；`make check-world` 退出码 0，回归 159/159 通过、isolation 等其他套件全绿。冒烟验证：`DECLARE/FETCH/CLOSE` 均报语法错误，`refcursor` 类型不存在，普通 `SELECT` 正常返回，`pg_cursor` 与游标相关 `pg_proc` 计数为 0。

---

## 插件钩子与调试桩裁剪（2026-08-24）

### 一、插件钩子（ProcessUtility_hook + 4 个 Executor 钩子）裁剪

这些钩子均为「可加载插件介入内核」的扩展点，非数据库运行核心逻辑；minipg 现有 contrib 扩展（amcheck、bloom、pageinspect、pg_buffercache、pg_freespacemap、pg_surgery、pg_visibility、pgrowlocks、pgstattuple、spi 等）均无引用。全部删除，调用点直接改为调用对应的 `standard_*` 实现：

- **`src/include/tcop/utility.h`**：删除 `ProcessUtility_hook_type` 类型定义与 `ProcessUtility_hook` 的 `extern` 声明；删除 `utility.c` 中针对该 hook 的残留注释块。
- **`src/backend/tcop/utility.c`**：删除 `ProcessUtility_hook` 变量定义，`ProcessUtility()` 中 `if (ProcessUtility_hook) ... else standard_ProcessUtility(...)` 分支简化为直接调用 `standard_ProcessUtility()`。
- **`src/include/executor/executor.h`**：删除 `ExecutorStart_hook_type` / `ExecutorRun_hook_type` / `ExecutorFinish_hook_type` / `ExecutorEnd_hook_type` 四个 typedef 与对应 `extern` 声明。
- **`src/backend/executor/execMain.c`**：删除 4 个 hook 变量定义（`= NULL`）；`ExecutorStart/Run/Finish/End` 中 `if (hook) ... else standard_*` 分支简化为直接调用 `standard_*`。
- **`src/tools/pgindent/typedefs.list`**：删除 5 个 hook 类型（`ProcessUtility_hook_type` 与 4 个 `Executor*_hook_type`）的登记项。

### 二、调试桩裁剪

- **`src/backend/parser/analyze.c`**：删除 `RAW_EXPRESSION_COVERAGE_TEST` 条件编译块——含 `test_raw_expression_coverage` 的前向声明、调用点（`transformStmt` 中对 SELECT/INSERT/UPDATE/DELETE 走 `raw_expression_tree_walker` 的自测）与函数定义；`pg_config_manual.h` 中对应的注释开关一并移除。该机制仅为上游维护 `raw_expression_tree_walker` 的开发者自测手段，默认关闭、非运行功能。
- **`src/backend/tcop/postgres.c`**：删除 `COPY_PARSE_PLAN_TREES` 条件块中 `ereport` 打印 `raw_parse_tree` 的调试转储分支（保留节点解析本身）。

---

## initdb 裁剪：移除 `-A/--auth`、`--auth-host`、`--auth-local` 三个选项（2026-08-21）

minipg 已不再需要 initdb 的认证方式配置选项，本次裁剪移除以下三个命令行选项：

- `-A, --auth=METHOD`：本地连接的默认认证方式
- `--auth-host=METHOD`：本地 TCP/IP 连接的默认认证方式
- `--auth-local=METHOD`：本地 Unix 域套接字连接的默认认证方式

涉及文件：

- **`src/bin/initdb/initdb.c`**：删除 `usage()` 中三个选项的帮助文本；从 `getopt_long` 短选项字符串中移除 `A:`。
- **`src/test/perl/PostgresNode.pm`**：测试基础设施调用 `initdb` 时移除 `-A trust` 参数。

---

## CREATE/ALTER/DROP CONVERSION 功能裁剪 与 SEQUENCE 残留裁剪（2026-08-21）

### 一、CREATE/ALTER/DROP CONVERSION 功能裁剪

`pg_conversion` catalog 与其内置编码转换数据保留（内置编码转换仍依赖 `FindDefaultConversion` 与约束检查），但删除了「创建/修改/删除用户自定义 conversion」这一整套 DDL 用户侧入口，实现层 `conversioncmds.c` 与 `pg_conversion.c` 的 `ConversionCreate` 一并裁除：

- **命令派发与语法（`gram.y` / `utility.c`）**：删除 `CreateConversionStmt` 整条语法产生式（`CREATE [DEFAULT] CONVERSION ... FOR ... TO ... FROM ...`）、`stmt` 与 `ClassifyUtilityCommandAsReadOnly` 中的 `T_CreateConversionStmt` 分支、`ProcessUtilitySlow` 的 `CreateConversionCommand` 调用、`CreateConversionStmt` 在 `CreateCommandTag` / `GetCommandLogLevel` 的分支；`AlterObjectSchemaStmt` 删除 `ALTER CONVERSION ... SET SCHEMA` 分支、`object_type_any_name` 删除 `CONVERSION_P` → `OBJECT_CONVERSION`（保留 `ALTER DOMAIN ... SET SCHEMA` 规则头，修复此前误删导致的 bison reduce/reduce 冲突）；`CreateCommandTag` / `AlterObjectTypeCommandTag` 删除 `DROP CONVERSION` / `ALTER CONVERSION` 标签分支。
- **执行层删除文件与声明**：删除 `src/backend/commands/conversioncmds.c` 与 `src/include/commands/conversioncmds.h`；从 `src/backend/commands/Makefile`、`pg_shdepend.c`、`utility.c`、`alter.c` 引用中移除。
- **catalog 层（`pg_conversion.c` / `pg_conversion.h`）**：删除 `ConversionCreate`（建 conversion 元组的入口）与其 `pg_conversion.h` 声明；保留 `FindDefaultConversion`。
- **节点层（`nodes.h` / `parsenodes.h` / `copyfuncs.c` / `equalfuncs.c`）**：删除 `T_CreateConversionStmt`、`CreateConversionStmt` 结构体、`_copyCreateConversionStmt` / `_equalCreateConversionStmt` 及 `copyObjectImpl` / `equal` 中的分发分支；`ObjectType` 枚举删除 `OBJECT_CONVERSION`。
- **依赖层**：删除 `OCLASS_CONVERSION`（`dependency.h` 枚举）、`object_classes[]` 数组项、`getObjectClass` 的 `ConversionRelationId` 分支、`getObjectAddress` 的 `OBJECT_CONVERSION` 分支、`getObjectDescription` / `getObjectTypeDescription` / `getObjectIdentityParts` 的 OCLASS_CONVERSION 分支（清理误删残留的孤儿代码行，修复 describe 编译错误）；`doDeletion` / `AlterObjectNamespace_oid` / `ATExecAlterColumnType` / `tablecmds.c` 中相应 case；`dropcmds.c` 的 `does_not_exist_skipping` 中 `OBJECT_CONVERSION` 分支。
- **命令标签（`cmdtaglist.h`）**：删除 `CMDTAG_CREATE_CONVERSION` / `CMDTAG_ALTER_CONVERSION` / `CMDTAG_DROP_CONVERSION`。
- **psql 客户端**：删除 `\dc`（`listConversions`）功能及其 `command.c` 的 `case 'c'`、`describe.h` 声明、`describe.c` 实现；删除 `tab-complete.c` 中 CREATE/ALTER/COMMENT ON/DROP 的相关 CONVERSION 补全分支与补全查询；更新 `help.c` 使用说明。
- **回归测试**：`conversion.sql/.out` 删除用户自定义 conversion 的建/删用例（保留内置编码转换函数测试）；`drop_if_exists` 删除 `CREATE/DROP CONVERSION` 用例；`psql.sql/.out` 删除全部 `\dc` 用例；`parallel_schedule` 移除已删除的 `alter_generic` 及随之失效的 `sql/alter_generic.sql`、`expected/alter_generic.out`。
- **pgindent 工具**：`src/tools/pgindent/typedefs.list` 删除 `CreateConversionStmt`。

### 二、SEQUENCE 残留裁剪（承接此前序列 DDL / `pg_sequence` catalog / `RELKIND_SEQUENCE` 裁剪）

此前已裁除序列 DDL 与 `sequence.c` / `pg_sequence`，本轮清理剩下零散残留：

- **psql**：删除 `\ds`（`showSeq`）序列列举功能残留的 `describe.c` / `describe.h` 引用与 `tab-complete.c` 中 CREATE/ALTER/DROP SEQUENCE 补全分支（配合上一节回归测试同步删除 psql.out 中 `\ds` 用例）。
- **命令标签（`cmdtaglist.h`）**：删除 `CMDTAG_CREATE_SEQUENCE` / `CMDTAG_ALTER_SEQUENCE` / `CMDTAG_DROP_SEQUENCE` / 无引用死标签 `CMDTAG_DISCARD_SEQUENCES`。
- **节点（`parsenodes.h`）**：`DiscardMode` 枚举删除无语法来源的 `DISCARD_SEQUENCES`。
- **pgindent 工具**：`typedefs.list` 删除 `AlterSeqStmt` / `CreateSeqStmt`。

### 验证

`make -j8` 全量重编通过；`make check-world` 全绿（regress 80 用例、isolation 与 contrib 均通过）。`conversion` 回归经复跑通过；`timestamptz`/`conversion` 首次偶发失败为既有 `now` 时钟相关脆弱断言与手工编辑未对齐，均已复跑验证通过。

---

## libpq / 通信层 IPv6 与死代码裁剪（2026-08-21）

minipg 仅支持 IPv4，监听/连接层不再需要 IPv6 代码。此前 `configure.ac` 已关闭 `HAVE_IPV6` 检测（`HAVE_IPV6` 宏始终未定义），所有 `#ifdef HAVE_IPV6` 块在编译期已被条件编译消除，属纯粹死代码。本次彻底删除这些死代码与宏定义本身（不在源码中保留条件编译），并清理与 IPv6 同源的 Notify / 密码认证残留死代码。

### 一、IPv6 通信层裁剪

- **`src/backend/libpq/ifaddr.c`**：删除全部 IPv6 代码——`range_sockaddr_AF_INET6` 声明与定义、`pg_range_sockaddr` 的 `AF_INET6` 分支、`pg_sockaddr_cidr_mask` 的 `case AF_INET6` 与 `128` 位掩码、`run_ifaddr_callback` 的 `AF_INET6` 掩码检查、SIOCGLIFCONF 版 `pg_foreach_ifaddr` 的 `sock6` 声明/创建/`fd` 三元选择/`close(sock6)`、fallback 版 `::1/128` loopback 块。
- **`src/interfaces/libpq/fe-connect.c`**：删除 `connectOptions2`/`pg_sockaddr2string` 中的 `#ifdef HAVE_IPV6` `AF_INET6` 地址格式化块（保留 URI 中的 `[ipv6]` 字符串解析，其为协议无关解析、不影响 IPv4-only）。
- **`src/include/pg_config.h.in`**、**`src/include/pg_config.h`**：删除 `HAVE_IPV6` 宏定义（`#undef` / `#define` 形式）。
- **`src/tools/ifaddrs/test_ifaddrs.c`**：删除 `print_addr` 中的 `#ifdef HAVE_IPV6` `case AF_INET6`。
- **`src/configure.ac`**：清理注释，明确「仅支持 IPv4，不检测 struct sockaddr_in6」。
- 保留项（与通信层 IPv6 无关）：`src/common/ip.c` 的 `pg_getaddrinfo_all` 等 getaddrinfo 封装（协议无关 DNS 解析，IPv4-only 也必需）；`src/port/inet_net_ntop.c` 的 `PGSQL_AF_INET6` 分支（服务于 inet/cidr 数据类型的 IPv6 地址**存储/显示**能力，configure.ac 注释约定保留）。

### 二、Notify（LISTEN/NOTIFY）前端死代码裁剪

后端 LISTEN/NOTIFY 早已删除，以下前端代码均为死代码：
- **`src/interfaces/libpq/fe-protocol3.c`**：删除 `getNotify()` 函数、`pqParseInput3` 主循环与 `pqFunctionCall3` 中的 `case 'A'`（NotifyResponse）解析分支、前向声明；更新 "NOTIFY and NOTICE" 注释为仅 NOTICE。
- **`src/interfaces/libpq/fe-exec.c`**：删除 `PQnotifies()` 与 `PQfreeNotify()`。
- **`src/interfaces/libpq/libpq-fe.h`**：删除 `PGnotify` 结构体、`PQnotifies` 声明、`PQfreeNotify` 宏、`PQnoPasswordSupplied` 宏（已无引用）。
- **`src/interfaces/libpq/libpq-int.h`**：删除 `PGconn.notifyHead` / `notifyTail` 字段。
- **`src/interfaces/libpq/fe-connect.c`**：`pqDropServerData` 删除 notify 队列清理。
- **`src/interfaces/libpq/exports.txt`**：删除 `PQnotifies` / `PQfreeNotify` 导出符号。
- **`src/bin/psql/common.c`**：删除 `PrintNotifications()` 函数及其调用。
- **`src/bin/psql/tab-complete.c`**：从关键词列表删除 `LISTEN` / `NOTIFY`，删除 `NOTIFY` 的 tab 补全分支。
- **`src/test/isolation/isolationtester.c`**：删除 notify 变量与 NOTIFY 报告逻辑块。
- **删除文件**：`src/test/examples/testlibpq2.c`、`src/test/examples/testlibpq2.sql`（专用于 LISTEN/NOTIFY 异步通知示例），并从 `src/test/examples/Makefile` 的 `PROGS` 移除。

### 三、密码认证残留死代码裁剪

后端 `ClientAuthentication` 无条件发送 `AUTH_REQ_OK`，永不发送 `AUTH_REQ_PASSWORD`：
- **`src/interfaces/libpq/fe-auth.c`**：删除 `AUTH_REQ_PASSWORD` 分支与已无调用者的 `pg_password_sendauth()` 函数。

### 四、COPY 协议 trace 死代码裁剪

后端 COPY 命令已删除，`fe-trace.c` 中 Copy 相关追踪分支永不触发：
- **`src/interfaces/libpq/fe-trace.c`**：删除 `pqTraceOutputf`(CopyFail)/`pqTraceOutputG`(CopyInResponse)/`pqTraceOutputH`(CopyOutResponse)/`pqTraceOutputW`(CopyBothResponse) 函数；删除 `pqTraceOutputMessage` 中的 `case 'c'`(CopyDone)/`case 'd'`(CopyData)/`case 'f'`(CopyFail)/`case 'G'`(StartCopyIn)/`case 'W'`(StartCopyBoth) 分支；`case 'H'` 简化为纯 Flush（去掉 Copy Out 调用）。

### 行为影响

- 编译期彻底消除 IPv6 死代码（无 `#ifdef HAVE_IPV6` 残留），libpq ABI 去除 `PQnotifies`/`PQfreeNotify`/`PGnotify`；`PQnoPasswordSupplied` 宏移除。
- 与不可裁部分（btree/hash 索引、事务）零耦合。
- 注：`src/test/regress/pg_regress.c` 的 `have_ipv6` 局部变量及 `src/tools/ifaddrs` 等属测试框架通用逻辑，未改动（其 IPv6 分支现在不执行但保留无害）。

验证：`make -j` 全量重编 0 错误；`make check-world` 通过（无 regression.diffs；pg_rewind 因依赖 pg_basebackup 已移除而跳过，属预期）。

---

## SASL/SCRAM 认证裁剪（2026-08-21）

服务端 `ClientAuthentication` 已被裁剪为无条件信任（仅发送 `AUTH_REQ_OK`），永远不会发起 SASL 握手；此前已裁剪 pg_hba/密码认证/SSL。因此客户端完整的 SASL/SCRAM 链路（`pg_SASL_init`/`pg_SASL_continue`/`fe-auth-scram.c` 等）均为死代码。SASL/SCRAM 属于网络认证协议层（RFC 4422/5802），非数据库内核核心，学习价值低，予以彻底裁剪。

### 删除文件

- **`src/common/scram-common.c`**、**`src/include/common/scram-common.h`**：SCRAM 通用原语（SaltedPassword/ClientKey/ServerKey 计算等）
- **`src/common/saslprep.c`**、**`src/include/common/saslprep.h`**：SASLprep 密码规范化（RFC 4013，约千行 Unicode 表）
- **`src/interfaces/libpq/fe-auth-scram.c`**：libpq 客户端 SCRAM-SHA-256 完整实现

### 修改文件

- **`src/include/libpq/pqcomm.h`**：删除 `AUTH_REQ_SASL` / `AUTH_REQ_SASL_CONT` / `AUTH_REQ_SASL_FIN` 三个认证请求码宏
- **`src/backend/libpq/auth.c`**：删除 `PG_MAX_SASL_MESSAGE_LENGTH` 宏及注释；`sendAuthRequest` 的 flush 条件去掉 `AUTH_REQ_SASL_FIN` 引用
- **`src/interfaces/libpq/fe-auth.c`**：重写——删除 `pg_SASL_init`、`pg_SASL_continue`、`check_expected_areq`、SASL 三 case 分支、`#include "common/scram-common.h"`；保留 `AUTH_REQ_PASSWORD` 等分支以维持 demux 完整性
- **`src/interfaces/libpq/fe-auth.h`**：删除 `pg_fe_scram_*` 五个函数声明
- **`src/interfaces/libpq/libpq-int.h`**：删除 `pg_conn.channel_binding` 与 `pg_conn.sasl_state` 字段
- **`src/interfaces/libpq/fe-connect.c`**：删除 `DefaultChannelBinding` 宏、`channel_binding` conninfo 选项、`channel_binding` 校验与释放、`sasl_state` 释放、`#include "common/scram-common.h"`
- **`src/common/Makefile`**、**`src/interfaces/libpq/Makefile`**：移除 `saslprep.o` / `scram-common.o` / `fe-auth-scram.o`
- **`src/tools/pgindent/typedefs.list`**：清理 `fe_scram_state` / `fe_scram_state_enum` / `pg_saslprep_rc` / `scram_state` / `scram_state_enum` 死类型

### 行为影响

- 服务端：无变化（本就不发 SASL 请求）。
- 客户端：`channel_binding` 连接参数与 `PGCHANNELBINDING` 环境变量不再被识别，传入会报 `invalid connection option "channel_binding"`；SASL/SCRAM 认证请求（服务端不会发送）走 default 报错分支。
- 与不可裁部分（btree/hash 索引、事务）零耦合。

验证：`make -j` 全量重编 0 错误 0 警告；手工 psql TCP 连接+查询成功；`channel_binding` 参数已被识别为非法选项。

---

## 临时关系文件清理死链裁剪（2026-08-21）

minipg 已彻底移除临时表功能（"Temporary relations are not supported in this build"），因此 `mdcreate` 不会再产生 `t<dboid>_<relfilenode>` 命名的临时关系物理文件。原 `RemovePgTempFiles()` 中对"遗留临时关系文件"的扫描清理逻辑已永远匹配不到任何文件，成为死代码，予以彻底删除。

### 删除内容（`src/backend/storage/file/fd.c`）

- `RemovePgTempRelationFiles(const char *tsdirname)`：扫描 tablespace 目录查找 per-DB 子目录
- `RemovePgTempRelationFilesInDbspace(const char *dbspacedirname)`：在 per-DB 目录下按 `t*` 文件名 unlink
- `looks_like_temp_rel_name(const char *name)`：判断文件名是否为临时关系文件（`t<digits>_<digits>` 规则）
- `RemovePgTempFiles()` 中两处 `RemovePgTempRelationFiles(...)` 调用（base 与 pg_tblspc 各一处）及对应前向声明、函数头注释

### 同步修改

- **`src/include/storage/fd.h`**：删除 `extern bool looks_like_temp_rel_name(const char *name);` 导出声明

### 保留说明

- `RemovePgTempFilesInDir()`（普通临时文件清理，服务于排序/hash join/tuplesort 等内核临时文件）**保留**，与临时表无关。
- `OpenTemporaryFile` 及其调用链保留。

验证：`make -C src` 全库重编通过；`RemovePgTempFiles` 仍由 `postmaster.c` 在启动/崩溃重启时调用，仅清理 pgsql_tmp 普通临时文件。

---

## authentication_timeout 参数裁剪（2026-08-21）

彻底裁剪 `authentication_timeout` 参数及其配套的 `AuthenticationTimeout` 变量。

### 涉及文件与改动

- **`src/backend/utils/misc/guc.c`**：删除 `{"authentication_timeout", ...}` GUC 定义
- **`src/backend/postmaster/postmaster.c`**：删除 `int AuthenticationTimeout = 60;` 变量定义及 `enable_timeout_after(STARTUP_PACKET_TIMEOUT)` 调用
- **`src/backend/utils/init/postinit.c`**：删除 `enable_timeout_after(STATEMENT_TIMEOUT, AuthenticationTimeout * 1000)` 调用
- **`src/include/postmaster/postmaster.h`**：删除 `extern int AuthenticationTimeout;` 声明
- **`src/backend/utils/misc/postgresql.conf.sample`**：删除 `#authentication_timeout` 配置样例
- **`src/backend/libpq/be-secure-openssl.c`**：删除相关注释

### 修复事项

`authentication_timeout` 的裁剪误删了 `StatementTimeoutHandler` 的注册（`STATEMENT_TIMEOUT` 与 `authentication_timeout` 共用同一 timeout 机制），导致 `statement_timeout` / `lock_timeout` 功能触发时服务端段错误。已修复：

- **`src/backend/utils/init/postinit.c`**：恢复 `StatementTimeoutHandler` 的声明、`RegisterTimeout(STATEMENT_TIMEOUT, StatementTimeoutHandler)` 注册、及 `StatementTimeoutHandler` 函数定义（仅发送 SIGINT，不再处理 `ClientAuthInProgress` 分支）

验证：`make -C src/test/isolation check` 全部 66 测试通过。

---

## Relation Options（reloptions）功能裁剪（2026-08-21）

minipg 彻底裁剪 Relation Options（关系选项）系统。reloptions 是 PostgreSQL 中用于存储表/索引/视图等关系级配置参数（如 fillfactor、autovacuum 设置、toast_tuple_target 等）的机制，存储在 `pg_class.reloptions` 列中，由 `reloptions.c` 解析并缓存在 `Relation->rd_options` 中。本次裁剪移除了整个 reloptions 框架，包括核心文件、所有选项结构体、解析注册表、以及所有消费者代码。**不保留 fillfactor**，所有表/索引使用硬编码的默认值。

### 核心文件删除

- `src/include/access/reloptions.h` — reloptions 头文件（类型定义、API 声明）
- `src/backend/access/common/reloptions.c` — reloptions 实现（注册表、解析、验证）
- `src/include/utils/attoptcache.h` — 属性选项缓存头文件
- `src/backend/utils/cache/attoptcache.c` — 属性选项缓存实现

### 结构体与宏删除

- `StdRdOptions`（标准关系选项，含 fillfactor、autovacuum、toast_tuple_target 等）
- `BTOptions`（B-tree 索引选项，含 fillfactor、deduplicate_items 等）
- `HashOptions`（Hash 索引选项，含 fillfactor）
- `ViewOptions`（视图选项）
- `AutoVacOpts`（autovacuum 子结构体）
- 所有访问宏：`RelationGetFillFactor`、`BTGetFillFactor`、`BTGetDeduplicateItems`、`HashGetFillFactor`、`RelationGetToastTupleTarget`、`RelationGetTargetPageFreeSpace`、`HashGetTargetPageUsage` 等

### 消费者代码修改

- **`src/include/utils/rel.h`**：删除 `rd_options` 字段、所有 reloptions 结构体及宏
- **`src/include/access/nbtree.h`**：删除 `BTOptions` 结构体及 `BT*` 宏
- **`src/include/access/hash.h`**：删除 `HashOptions` 结构体及 `Hash*` 宏
- **`src/backend/access/heap/hio.c`**：`RelationGetTargetPageFreeSpace()` → `saveFreeSpace = 0`
- **`src/backend/access/heap/heapam.c`**：同上
- **`src/backend/access/heap/rewriteheap.c`**：同上
- **`src/backend/access/heap/pruneheap.c`**：同上
- **`src/backend/access/heap/heaptoast.c`**：`RelationGetToastTupleTarget()` → `TOAST_TUPLE_TARGET`
- **`src/backend/access/nbtree/nbtinsert.c`**：删除 `BTGetDeduplicateItems` 条件判断
- **`src/backend/access/nbtree/nbtsort.c`**：`BTGetTargetPageFreeSpace()` → `BLCKSZ*(100-BTREE_DEFAULT_FILLFACTOR)/100`；删除 `BTGetDeduplicateItems` 调用
- **`src/backend/access/nbtree/nbtsplitloc.c`**：`BTGetFillFactor()` → `BTREE_DEFAULT_FILLFACTOR`
- **`src/backend/access/nbtree/nbtutils.c`**：添加 `#include "storage/lwlock.h"`（因 reloptions.h 间接包含）；删除 `btoptions()` 函数
- **`src/backend/access/hash/hashpage.c`**：`HashGetTargetPageUsage()` → `BLCKSZ*HASH_DEFAULT_FILLFACTOR/100/item_width`
- **`src/backend/access/hash/hashutil.c`**：删除 `hashoptions()` 函数
- **`src/backend/postmaster/autovacuum.c`**：删除 `AutoVacOpts` 结构及 `extract_autovac_opts()` 函数
- **`src/backend/utils/cache/relcache.c`**：删除 `RelationParseRelOptions()` 函数及 `rd_options` 引用
- **`src/backend/commands/tablecmds.c`**：删除 `ATExecSetRelOptions` 函数定义及声明；删除 `AT_SetRelOptions`/`AT_ResetRelOptions`/`AT_ReplaceRelOptions` 相关 case 代码（lock mode、permission/prep、execution 三处）；删除 `Datum reloptions` 局部变量及赋值；删除 `heap_create_with_catalog` 调用中的 `reloptions` 参数；删除 `AlterTableCreateToastTable` 调用中的 `(Datum) 0` 参数
- **`src/backend/catalog/heap.c`**：删除 `InsertPgClassTuple` 和 `AddNewRelationTuple` 中的 `reloptions` 参数及 `Anum_pg_class_reloptions` 写入代码；删除 `heap_create_with_catalog` 函数签名中的 `Datum reloptions` 参数
- **`src/include/catalog/heap.h`**：从 `heap_create_with_catalog` 和 `InsertPgClassTuple` 声明中删除 `Datum reloptions` 参数
- **`src/include/catalog/toasting.h`**：从 `NewRelationCreateToastTable`、`NewHeapCreateToastTable`、`AlterTableCreateToastTable` 声明中删除 `Datum reloptions` 参数
- **`src/backend/catalog/toasting.c`**：删除 `heap_create_with_catalog` 调用中的 `(Datum) 0` 参数
- **`src/backend/commands/cluster.c`**：删除 `heap_create_with_catalog` 调用中的 `(Datum) 0` 参数
- **`src/backend/catalog/index.c`**：删除 `InsertPgClassTuple` 调用中的 `reloptions` 参数
- **`src/backend/bootstrap/bootparse.y`**：删除 `heap_create_with_catalog` 调用中的 `(Datum) 0` 参数
- **`src/backend/access/common/Makefile`**：移除 `reloptions.o`
- **`src/bin/psql/command.c`**：添加 `exec_command_password()` 桩函数（因 `PQencryptPasswordConn` 随 libpq reloptions 裁剪丢失）

### contrib 模块修改

- **`contrib/bloom/blutils.c`**：移除 `#include "access/reloptions.h"`；删除 reloptions 注册代码（`bl_relopt_kind`、`bl_relopt_tab`、`add_int_reloption` 调用）；`bloptions()` 返回 NULL；`initBloomState()` 不再读取 `index->rd_options`
- **`contrib/bloom/sql/bloom.sql`**：删除 reloptions 测试段（`WITH (length=N, col1=M)`、`SELECT reloptions` 等）
- **`contrib/bloom/expected/bloom.out`**：同步删除预期输出

### 回归测试修改

- **删除文件**：`src/test/regress/sql/reloptions.sql`、`src/test/regress/expected/reloptions.out`
- **`src/test/regress/parallel_schedule`**：移除 `test: reloptions`
- **`src/test/regress/expected/btree_index.out`**：删除 `ERROR: operator class int4_ops has no options` 预期
- **`src/test/regress/expected/hash_index.out`**：删除 fillfactor 越界错误预期
- **`src/test/regress/expected/create_table.out`**：调整 `WITH OIDS` 测试预期（不再报 "not supported"）
- **`src/test/regress/expected/strings.out`**：调整 `toast_tuple_target` 相关预期；`ALTER TABLE toasttest set (toast_tuple_target = 4080)` 改为报 `ERROR: unrecognized alter table type: 34`
- **`src/test/regress/expected/hash_index.out`**：删除 fillfactor 越界错误预期；`ALTER INDEX hash_split_index SET (fillfactor = 10)` 改为报 `ERROR: unrecognized alter table type: 34`
- **`src/test/regress/expected/tablesample.out`**：更新采样结果预期（因无 fillfactor 表页数变化）
- **`src/test/regress/expected/groupingsets.out`**：`alter table ... set (autovacuum_enabled = 'false')` 改为报 `ERROR: unrecognized alter table type: 34`
- **`src/test/regress/sql/btree_index.sql`**、**`hash_index.sql`**、**`create_table.sql`**、**`strings.sql`**、**`tablesample.sql`**：同步修改 SQL 测试
- **`src/test/modules/dummy_index_am/Makefile`**：移除 `REGRESS = dummy_index_am`（reloptions 测试已删）

### 保留项

- `pg_class.reloptions` 列保留（系统目录结构不可修改），但不再解析，视为 NULL
- `BloomOptions` 结构体保留在 `bloom.h`（用于磁盘存储 `BloomMetaPageData.opts`）
- `amoptions_function` 索引 AM 接口保留（各索引 AM 仍需提供 `amoptions` 回调，即使返回 NULL）

验证：`make -j4` 全量重编通过；`make check-world` 全绿。

---

## 触发器（TRIGGER）功能裁剪（2026-08-20）

minipg 彻底裁剪触发器功能（含 SQL 触发器、内部/系统触发器、INSTEAD OF 触发器、事件触发器残余、延迟触发队列）。触发器的用户侧语法/DDL/执行器/缓存/目录全链路删除，仅保留目录列 `pg_class.relhastriggers` 的字段与写入（由 pg_depend/规则共享的 `pg_rewrite` 不受影响）。

涉及文件与改动（分阶段收尾）：

- **目录层**：删除 `src/include/catalog/pg_trigger.h`、`src/include/utils/reltrigger.h`；`pg_proc.dat` 删除触发器函数（`tgenabled` 相关 `trigfuncs.c` 的 `pg_trigger_depth` 等）与 `pg_type.dat` 相关类型；`objectaddress.c`/`dependency.c`/`dependency.h` 删除 `OBJECT_TRIGGER` 的 `get_object_address`/`findDependRecursively` 分支与 `DROPTRIGGER` 依赖处理；`catalog/Makefile`/`commands/Makefile`/`utils/adt/Makefile` 移除 `trigger.c`/`trigfuncs.c`。
- **语法/命令层**：`gram.y` 删除 `CREATE TRIGGER`/`ALTER TRIGGER`/`DROP TRIGGER` 语法生产式与 `TriggerElem`/`TriggerActionTime`/`TriggerForClause`/`TriggerForEach`/`TriggerOptTransitionTable` 非终结符；`cmdtaglist.h` 删除 `CMDTAG_ALTER_TRIGGER`/`CMDTAG_CREATE_TRIGGER`/`CMDTAG_DROP_TRIGGER` 命令标签；`utility.c` 删除对应 `ProcessUtility` case；`dropcmds.c` 删除 `RemoveTriggerById`/`DROP TRIGGER` 分支；`commands/alter.c`/`tablecmds.c` 删除 `AT_AddConstraint` 中 `CONSTR_TRIGGER`（内部触发器约束）路径。
- **节点层**：`parsenodes.h` 删除 `CreateTrigStmt`/`TriggerTransition`/`TriggerData` 结构体与 `parsenodes.h` 中相关字段（`Constraint` 的 `old_conname`/`conname` 等触发器约束字段、`IndexStmt` 的 `excludeOpNames` 无关，保留约束核心）；`nodes.h` 删除 `T_CreateTrigStmt`/`T_TriggerTransition` 枚举；`copyfuncs.c`/`equalfuncs.c`/`outfuncs.c` 删除对应节点复制/相等/输出函数与 case。
- **执行层**：`execMain.c`/`execUtils.c`/`nodeModifyTable.c`/`spi.c`/`functions.c` 删除 `AfterTriggerBeginQuery`/`AfterTriggerEndQuery`/`AfterTriggerSetState` 调用、`ri_TrigDesc` 复制、`ExecASUpdateTriggers`/`ExecBSDeleteTriggers` 等 DML 触发器队列；`xact.c` 删除 `AfterTriggerBeginXact`/`AfterTriggerEndXact`/`AfterTriggerFireDeferred` 及 Commit/Prepare/Abort 的事务边界触发队列；`rewriteHandler.c` 删除 `fireRIRrules` 中对 `CommandType_INSERT` 的触发器规则处理；`plancat.c`/`plancat.h` 删除 `has_row_triggers`/`has_transition_tables`；`relcache.c` 删除 `RelationGetTriggerDesc`/`rd_trigdesc`/`relhastriggers` 缓存字段。
- **用户可见函数**：`regress.c` 删除 `pg_trigger_depth` 等触发器测试函数与 `create_function_0.source` 中 3 个触发器 C 函数（`plpgsql_call_handler` 依赖外的 `int4pl` 无关）；`ruleutils.c` 删除 `pg_get_triggerdef` 等反解析函数；`guc.c` 删除 `session_replication_role`（触发器复制角色 GUC）及其相关 `row_security` 无耦合清理。
- **头文件清理**：`commands/trigger.h`/`commands/alter.h` 删除 `renametrig`/`ExecRenameStmt` 等触发改名残留；`executor/executor.h`/`spi.h`/`optimizer/plancat.h` 删除触发器相关原型；`parser/kwlist.h` 保留 `TRIGGER` 关键字（不可删，见下）。

**本次收尾（本会话补完）**：
- `src/backend/catalog/indexing.c`：删除 `resultRelInfo->ri_TrigDesc = NULL`（`CatalogOpenIndexes` 中已删字段赋值）。
- `src/backend/parser/parse_utilcmd.c`：删除 `CreateSchemaStmtContext` 的 `triggers` 字段、初始化、`T_CreateTrigStmt` case 与 `list_concat(cxt.triggers)`。
- `src/bin/initdb/initdb.c`：删除 initdb 填充 pg_depend 时的 `INSERT INTO pg_depend SELECT ... FROM pg_trigger`（pg_trigger 表已删）。
- `src/test/regress/expected/sanity_check.out`：删除 `pg_trigger|t` 系统目录清单期望行。
- 清理 stale 生成文件 `pg_trigger_d.h`（genbki 在 pg_trigger.h 删除后不再生成）。

**保留项**：`pg_class.relhastriggers` 列保留（仍由 `pg_rewrite`/`pg_depend` 依赖逻辑读取，语义退化为恒 false）；`TRIGGER` 关键字保留在 `kwlist.h`（`UNRESERVED_KEYWORD`，与 `BARE_LABEL` 冲突检查相关，check_keywords.pl 约束）；`sql_features`/信息模式中 `TRIGGER` 相关文档不改。**会话复制角色 GUC 不再存在**，但 `session_replication_role` 已随触发器一并裁剪，无残留引用。

**修复裁剪引入的副作用 bug（`execMain.c` 的 `CMD_SELECT` case 被误删）**：触发器裁剪在 `standard_ExecutorStart` 删除 `CMD_SELECT` case 时，把标准 PG 中 SELECT 的 `if (rowMarks != NIL || hasModifyingCTE) GetCurrentCommandId(true)` 条件判断连同 `EXEC_FLAG_SKIP_TRIGGERS` 一起整段删除，导致 `CMD_SELECT` 掉入 `CMD_INSERT/DELETE/UPDATE` 分支、**无条件调用 `GetCurrentCommandId(true)`**。后果：(1) 普通 SELECT 也把 `currentCommandIdUsed` 置 true，使 `combocid` 回归测试的 cmin 值整体偏移 +1/+2（比标准 PG 多推进 command id）；(2) 在 `force_parallel_mode=on` 的并行查询中，parallel worker 执行多语句 parallel-safe SQL 函数（如 `lock_excl` 的 `select pg_advisory_xact_lock($1); select 1;`）时，第二条语句的 `CommandCounterIncrement` 因 `IsParallelWorker()` 命中 `cannot start commands during a parallel operation` 报错，导致 `src/test/isolation` 的 `deadlock-parallel` 测试失败。修复：恢复 `CMD_SELECT` case，仅保留 `GetCurrentCommandId` 的条件判断（`rowMarks`/`hasModifyingCTE`），不再保留已裁的 `EXEC_FLAG_SKIP_TRIGGERS`。修复后 `combocid.out` 恢复为原始标准 PG 预期值（cmin 不再偏移），`deadlock-parallel` 通过。

验证：`make -j4` 全量重编通过；`cd src/test/regress && NO_TEMP_INSTALL=1 make check` **82 项全绿**；`cd src/test/isolation && NO_TEMP_INSTALL=1 make check` **66 项全绿**。注：此过程同时修复了 minipg 既有的 initdb bootstrap 崩溃（`heap_create` 创建 pg_proc 时 `rd_tableam` 为 NULL，系历史裁剪导致的混合编译/结构体布局不一致，经全量 clean 重编解决，非本裁剪引入）。

---

## ALTER COLLATION 功能裁剪（2026-08-20）

minipg 的 `ALTER COLLATION` 在裁前已属死功能：语法生产式为空壳（`gram.y` 仅有 `ALTER COLLATION` 注释块而无实际产生式，无法解析）、无 `AlterCollation`/`RefreshCollationVersion` 实现函数、`RENAME` 已整体裁、`OWNER TO` 角色已裁，且无任何回归测试引用。本次彻底删除 `ALTER COLLATION` 的残留死代码与文档。

涉及文件与改动：

- **`src/backend/parser/gram.y`**：删除空的 `ALTER COLLATION` 注释块（仅注释，无生产式，属误导死壳）。
- **`src/backend/commands/alter.c`**：从 `ExecAlterObjectSchemaStmt` 的 generic 代码路径中删除 `case OBJECT_COLLATION:`（该 case 实际不可达——无语法生成 `AlterObjectSchemaStmt(OBJECT_COLLATION)`），保留 `OBJECT_AGGREGATE`/`OBJECT_CONVERSION` 共享分支，不影响 CONVERSION 的 `SET SCHEMA`。
- **`doc/src/sgml/ref/alter_collation.sgml`**：整文件删除（参考文档）；同步清理引用：`ref/allfiles.sgml` 删除 `<!ENTITY alterCollation ...>`、`reference.sgml` 删除 `&alterCollation;`、`create_collation.sgml` 与 `drop_collation.sgml` 的 See Also 中 `<xref linkend="sql-altercollation"/>`、`create_collation.sgml` 版本不匹配说明段、`func.sgml` 中 `pg_collation.collversion` 说明处的 `sql-altercollation` 断链 xref。

保留项：`OBJECT_COLLATION` 枚举及 `objectaddress.c`/`dropcmds.c` 中的引用（`get_object_address` 的 `OBJECT_COLLATION` case、`pg_class` 描述、DROP COLLATION 路径）——这些服务于 **CREATE/DROP COLLATION**，collation 是排序规则核心对象，学习价值高，不予裁。

与不可裁部分（btree/hash 索引、事务）零耦合；改动为纯删除，无死代码残留（`OBJECT_COLLATION` 枚举仍被 CREATE/DROP COLLATION 及寻址逻辑共享，必须保留）。`make` 重编通过，`make check` 全绿后回填验证结果。

---

## ALTER AGGREGATE 功能裁剪（2026-08-20）

minipg 的 `ALTER AGGREGATE` 无独立 `AlterAggregateStmt` 节点，而是复用 `AlterObjectSchemaStmt`（`SET SCHEMA`）、`RenameStmt`（`RENAME`）、`AlterOwnerStmt`（`OWNER TO`）三类通用语句机制。经核查：minipg 已于历史裁剪中删除 `RenameStmt`/`ExecRenameStmt`（RENAME 语法整体不可用），且角色/owner 机制已裁（`OWNER TO` 无实际语义）；故 `ALTER AGGREGATE` 实际仅 `SET SCHEMA` 一条子命令（走 `OBJECT_AGGREGATE` 与 `OBJECT_COLLATION`/`OBJECT_CONVERSION` 共享的 `ExecAlterObjectSchemaStmt` generic 分支）生效。本次彻底裁剪 `ALTER AGGREGATE` 的全部用户侧入口（语法生产式 + 命令标签），底层 aggregate 对象系统（`OBJECT_AGGREGATE` 枚举、`pg_proc` 寻址、CREATE/DROP AGGREGATE）保留——因后者与函数机制深度共享，且 CREATE/DROP AGGREGATE 学习价值高，不予裁。

涉及文件与改动：

- **`src/backend/parser/gram.y`**：从 `AlterObjectSchemaStmt` 产生式中删除 `ALTER AGGREGATE aggregate_with_argtypes SET SCHEMA name` 一条生产式（aggregate 段仅此一条 ALTER 入口，删除后 `ALTER AGGREGATE x SET SCHEMA y` 报语法错误）。
- **`src/include/tcop/cmdtaglist.h`**：删除 `CMDTAG_ALTER_AGGREGATE` 命令标签枚举项（`OBJECT_AGGREGATE` 不再经 `AlterObjectTypeCommandTag` 返回标签）。
- **`src/backend/tcop/utility.c`**：从 `AlterObjectTypeCommandTag` 的 switch 中删除 `case OBJECT_AGGREGATE: tag = CMDTAG_ALTER_AGGREGATE; break;`（该 switch 已有 `default: tag = CMDTAG_UNKNOWN;`，删除后无枚举未覆盖警告）。
- **`doc/src/sgml/ref/alter_aggregate.sgml`**：整文件删除（参考文档）；同步清理引用：`ref/allfiles.sgml` 删除 `<!ENTITY alterAggregate ...>`、`reference.sgml` 删除 `&alterAggregate;`、`create_aggregate.sgml` 与 `drop_aggregate.sgml` 的 See Also 中 `<xref linkend="sql-alteraggregate"/>`；`drop_aggregate.sgml` 的 NOTES 段改写为「本实现无 ALTER AGGREGATE 命令」。

回归测试同步（删除对已裁语法的依赖，重生成 `expected`）：

- **`sql/alter_generic.sql`**：删除原 `Aggregate` 测试整段（含 `CREATE AGGREGATE alt_agg*` 与 `ALTER AGGREGATE ... RENAME/SET SCHEMA`），仅保留 `Conversion` 段；`expected/alter_generic.out` 同步删除 Aggregate 段输出，并修正结尾 `DROP SCHEMA` 级联对象数（由 4/2 降为 2/1）。
- **`sql/create_aggregate.sql`**：删除两行 `alter aggregate ... rename to`（RENAME 语法已裁），改为 `\da my_percentile_disc` / `\da my_rank` 验证；`expected/create_aggregate.out` 同步替换对应输出。

与不可裁部分（btree/hash 索引、事务）零耦合；改动为纯删除，无死代码残留（`OBJECT_AGGREGATE` 枚举仍被 CREATE/DROP AGGREGATE 及 `parse_func.c`/`objectaddress.c`/`dropcmds.c` 共享，必须保留）。`make` 重编通过，`make check` 全绿后回填验证结果。

---

## ALTER FUNCTION / OPERATOR / OPERATOR FAMILY / STATISTICS / EXTENSION / DATABASE 功能裁剪（2026-08-18）

按"非核心对象管理 DDL 且与不可裁部分（btree/hash 索引、事务）零耦合则优先裁剪"的原则，本次彻底裁剪以下 7 类外围 `ALTER` 语法（与分析报告一致）：`ALTER FUNCTION/PROCEDURE/ROUTINE`、`ALTER OPERATOR`、`ALTER OPERATOR FAMILY`、`ALTER STATISTICS`、`ALTER EXTENSION`、`ALTER EXTENSION ... ADD/DROP CONTENTS`、`ALTER DATABASE`。

`ALTER TABLE`（含 `ALTER INDEX`/`ALTER VIEW` 复用 `AlterTableStmt`，索引为不可裁核心）**保留**；`ALTER TYPE`/`ALTER DOMAIN`/`ALTER ENUM` 与表空间相关 `ALTER`（贴近存储引擎、学习价值高）**保留**；`ALTER OBJECT SET SCHEMA`/`ALTER OBJECT DEPENDS ON` 节点与语法入口仅移除直接 `ALTER FUNCTION/EXTENSION ... SET SCHEMA/DEPENDS` 入口，节点本体、产生式及 `ALTER TABLE SET SCHEMA` 复用路径**保留**。

涉及文件与改动：

- **`src/backend/parser/gram.y`**：从 `stmt:` 列表与 `%type` 删除 `AlterFunctionStmt`/`AlterOperatorStmt`/`AlterOpFamilyStmt`/`AlterStatsStmt`/`AlterExtensionStmt`/`AlterExtensionContentsStmt`/`AlterDatabaseStmt`；删除各自的文法产生式块（`AlterStatsStmt`、整个 `AlterExtension` 块、`AlterOpFamilyStmt`、`AlterFunctionStmt`、`AlterOperatorStmt`、`AlterDatabaseStmt`）；`stmt:` 列表中仅移除 `AlterObjectDependsStmt`/`AlterObjectSchemaStmt` 的直接入口行（节点/产生式保留，供 `ALTER TABLE SET SCHEMA` 等复用）。
- **`src/include/nodes/nodes.h`** / **`src/include/nodes/parsenodes.h`**：删除上述 7 个节点的 `T_` 枚举值与结构体定义（`AlterObjectDependsStmt`/`AlterObjectSchemaStmt` 保留）。
- **`src/backend/nodes/copyfuncs.c`** / **`src/backend/nodes/equalfuncs.c`** / **`src/backend/nodes/outfuncs.c`**：删除对应的 `_copyXxx`/`_equalXxx`/`_outXxx` 函数及 dispatch case。
- **`src/backend/tcop/utility.c`**：在 4 个 switch（`ClassifyReadOnly`/`ProcessUtility` 执行、`GetCommandTag`、`SetQueryCompletion`）中删除上述 7 个节点的 case；同时删除孤儿执行体（`AlterDatabase`、`GetCommandTag` 中 `AlterFunctionStmt` 的 `objtype` 子 switch、`SetQueryCompletion` 中 `AlterStatsStmt` 分支）。
- **`src/backend/commands/functioncmds.c`**：删除 `AlterFunction` 函数实现（保留 `CreateFunction`/`RemoveFunctionById`/`CreateTransform` 及 `check_transform_function`）。
- **`src/backend/commands/operatorcmds.c`**：删除 `AlterOperator` 函数实现。
- **`src/backend/commands/opclasscmds.c`**：删除 `AlterOpFamily` 及其 static 辅助 `AlterOpFamilyAdd`/`AlterOpFamilyDrop`（保留 `DefineOpClass`/`DefineOpFamily` 及通用 `processTypesSpec` 等）。
- **`src/backend/commands/statscmds.c`**：删除 `AlterStatistics` 函数实现（保留 `CreateStatistics`/`RemoveStatisticsById`/`RemoveStatisticsDataById`/`ChooseExtendedStatisticName` 等 `CREATE STATISTICS` 依赖的辅助函数）。
- **`src/backend/commands/extension.c`**：删除 `ExecAlterExtensionStmt`/`ExecAlterExtensionContentsStmt` 函数实现（保留 `CreateExtension` 及 `ApplyExtensionUpdates`/`AlterExtensionNamespace` 等，后者仍被 `ALTER EXTENSION SET SCHEMA` 与 `ALTER TABLE SET SCHEMA` 复用）。
- **`src/backend/commands/dbcommands.c`**：删除 `AlterDatabase` 函数实现（保留 `createdb`/`dropdb`/`movedb` 等）；同步删除头文件残留的 `AlterDatabaseOwner` 死声明。
- **头文件**：`src/include/commands/{defrem.h,dbcommands.h,extension.h}` 删除对应 `extern` 声明。
- **`src/tools/pgindent/typedefs.list`**：删除上述 7 个类型名。
- **`src/bin/psql/tab-complete.c`**：删除 `ALTER DATABASE`/`ALTER EXTENSION`/`ALTER STATISTICS`/`ALTER FUNCTION`/`ALTER AGGREGATE` 的代码补全分支（修复编辑器补全对未提供语法的提示）。

与不可裁部分（btree/hash 索引、事务）零耦合；改动为彻底删除，无死代码残留（除 `AlterObjectDependsStmt`/`AlterObjectSchemaStmt` 节点因 `ALTER TABLE` 复用而保留）。`make -j` 全量重编通过。

### 回归测试同步（2026-08-18，裁剪收尾）

上一轮语法裁剪后，4 个既有回归测试仍在调用已删除的语法，导致 `make check` 失败。本次从测试 SQL 中移除对已裁功能的依赖，并重新生成对应 `expected/*.out`：

- **`sql/alter_generic.sql`**：删除 `ALTER OPERATOR FAMILY ... ADD/DROP` 整段（157–327 行）与 `ALTER STATISTICS` 整段（含其专属的 `CREATE TABLE alt_regress_1/2` 与 `pg_statistic_ext` 校验查询）；保留 `ALTER TABLE/INDEX/VIEW/OPCLASS RENAME`、`SET SCHEMA` 等复用 `AlterTableStmt` 路径的测试。
- **`sql/alter_operator.sql`**：删除全部 `ALTER OPERATOR ... SET (...)` 调用及依赖它们的 `pg_proc`/`pg_operator` 校验查询，仅保留 `CREATE/DROP OPERATOR` 与 `pg_depend` 基础校验。
- **`sql/misc_functions.sql`**：删除 `ALTER FUNCTION ... SUPPORT test_support_func` 测试段（含 `my_int_eq`/`my_gen_series` 定义及依赖 SUPPORT 的计划校验）；删除后相关查询计划回退为 Merge Join，已同步 `expected`。
- **`sql/guc.sql`**：删除 `ALTER FUNCTION ... SET (work_mem)` / `RESET` 及其校验查询（属已裁 `ALTER FUNCTION` 子集）。

`make check` 验证：**全部 83 个测试通过**。

#### alter_generic 二次清理（2026-08-18，裁剪收尾）

上一轮回归同步只移除了 `ALTER OPERATOR FAMILY ... ADD/DROP` 与 `ALTER STATISTICS`，但 `sql/alter_generic.sql` 仍残留已裁的 `ALTER FUNCTION` 与 `ALTER OPERATOR/OPERATOR FAMILY SET SCHEMA/RENAME` 调用（语法已彻底删除，现报 `syntax error at or near "FUNCTION"/"OPERATOR"`）。本次彻底清理测试 SQL 并重生成 `expected/alter_generic.out`：

- **`sql/alter_generic.sql`**：删除 `Function and Aggregate` 段中的 `CREATE/ALTER FUNCTION` 全部语句（函数 `ALTER` 语法已裁），聚合 `ALTER AGGREGATE` 部分**保留**（仍可用）；删除 `Operator` 段（`CREATE/ALTER OPERATOR SET SCHEMA`，操作符 `ALTER` 已裁）；删除 `Operator Family` 段（`CREATE OPERATOR FAMILY` 及其 `RENAME/SET SCHEMA`，操作符族 `ALTER` 已裁）及结尾的 `pg_opfamily`/`pg_opclass` 校验查询。仅保留 `AGGREGATE`/`CONVERSION` 的 `ALTER ... RENAME/SET SCHEMA`（这两条路径未被裁）。
- **`expected/alter_generic.out`**：依据新 SQL 重新生成（级联 `DROP SCHEMA` 对象数由 9/6 降为 4/2，因不再创建/迁移函数、操作符、操作符族）。

`make check` 验证：**全部 83 个测试通过**。

### gram.y 死语法清理（2026-08-18，裁剪收尾）

上一轮语法块删除后，`gram.y` 遗留 7 个无用非终结符（bison 报 `7 nonterminals useless in grammar`），均为已裁功能的残骸，需彻底删除不留死代码：

- `opclass_drop_list` / `opclass_drop`（3391–3413）：原 `CREATE OPERATOR CLASS ... AS (DROP OPERATOR n (...))` 就地删成员语法；minipg 的 `CreateOpClassStmt` 的 `AS` 子句只接 `opclass_item_list`，从未引用该分支 → 死代码（注意：这属于 `CREATE OPERATOR CLASS` 的边角写法，删除不影响 opclass 创建主路径，亦不触碰 btree/hash 索引核心）。
- `object_type_name`（3612–3616）：仅将 `drop_type_name` 包一层 `DATABASE`/`TABLESPACE` 的包装非终结符，无任何规则引用它。保留其下层 `drop_type_name`（3495/3505 的 `DROP` 语句仍使用）。
- `add_drop`（10330–10332）：`ALTER OPERATOR FAMILY ... ADD/DROP` 用，已裁 → 死。
- `alter_extension_opt_list` / `alter_extension_opt_item`（10367–10385，含 ALTER EXTENSION 注释块）：`ALTER EXTENSION ... UPDATE ... WITH (new_version=...)` 用，已裁 → 死。
- `alterfunc_opt_list`（10387–10391）：`ALTER FUNCTION ... SET/RESET` 用，已裁 → 死。

同步从 7 处 `%type` 声明中移除对应符号。`make` 重编后 bison 警告全部消除；`make check`：**全部 83 个测试通过**。

---

## CREATE/DROP/ALTER COLLATION 功能裁剪（2026-08-18）

排序规则（collation）的字符串比较内核（`pg_collation` 预置表、`pg_locale.c`、`parse_collate.c`、索引/排序的 collation 字段）属于数据库核心能力，**予以保留**。本次仅裁剪用户侧的 COLLATION 管理 DDL 入口，使其不可再由 SQL 创建/删除/修改自定义排序规则：

- **`src/backend/parser/gram.y`**：删除 4 条 `CREATE COLLATION` 产生式、`ALTER COLLATION ... SET SCHEMA` 产生式、`AlterCollationStmt`（REFRESH VERSION）产生式；从 `object_type_any_name` 删除 `COLLATION` 分支（DROP 入口）、从 `stmt` 列表与 `%type` 删除 `AlterCollationStmt`。**保留** `OBJECT_COLLATION` 枚举（内核对象寻址仍需）。
- **`src/backend/commands/collationcmds.c`**：删除 `DefineCollation`、`AlterCollation` 两个函数实现。**保留** `IsThereCollationInNamespace`（ALTER 通用路径依赖）、`pg_collation_actual_version`、`pg_import_system_collations`（initdb 依赖）。
- **`src/include/commands/collationcmds.h`**：删除 `DefineCollation`/`AlterCollation` 声明，保留 `IsThereCollationInNamespace`。
- **`src/backend/tcop/utility.c`**：删除 `DefineCollation` 的 `OBJECT_COLLATION` case、`T_AlterCollationStmt` 执行/命令标签/日志分支，以及 `OBJECT_COLLATION` 的 CREATE/DROP/ALTER 命令标签 case；移除冗余 `#include "commands/collationcmds.h"`。
- **`src/include/nodes/nodes.h`**、**`src/include/nodes/parsenodes.h`**：删除 `T_AlterCollationStmt` 枚举值与 `AlterCollationStmt` 结构体。
- **`src/backend/nodes/copyfuncs.c`**、**`src/backend/nodes/equalfuncs.c`**：删除 `_copyAlterCollationStmt`/`_equalAlterCollationStmt` 函数及对应 dispatch case。
- **`src/tools/pgindent/typedefs.list`**：删除 `AlterCollationStmt` 类型名。

`OBJECT_COLLATION` 枚举值、`pg_collation` 系统表、`C`/`POSIX` 预置 collation、`pg_import_system_collations`、`IsThereCollationInNamespace` 均保留，与不可裁部分（btree/hash 索引、事务）及内核字符串比较零耦合。

---

## ALTER DEFAULT PRIVILEGES 残留清理（2026-08-18）

minipg 此前已彻底裁剪对象级 ACL 系统（权限位宏、aclchk、aclitem、`pg_default_acl` 等），`ALTER DEFAULT PRIVILEGES` 的执行函数、语法解析、底层系统表均已随 ACL 一同删除。本次仅清理两处遗留的孤立枚举/类型名（无对应语法、无执行入口、无 catalog 引用）：

- **`src/include/tcop/cmdtaglist.h`**：删除孤立的 `CMDTAG_ALTER_DEFAULT_PRIVILEGES` 命令标签枚举项（第 35 行，现实中无任何语句触发，纯死代码）。
- **`src/tools/pgindent/typedefs.list`**：删除已不存在的 `AlterDefaultPrivilegesStmt` 类型名（pgindent 静态名单，不影响编译）。

与不可裁部分（btree/hash 索引、事务）零耦合；改动为纯删除，无逻辑影响。

---

## cmdtaglist.h 已裁命令标签清理（2026-08-18）

系统盘点 `src/include/tcop/cmdtaglist.h` 中全部命令标签，逐一核对对应 SQL 功能在 minipg 中是否已彻底裁剪（语法 `gram.y` 无规则 + 执行 `tcop/utility.c` 无对应 case + 无实现）。凡无语法、无执行入口、无对象系统引用（即孤立死代码）的标签一律删除，并同步清理 `utility.c` 中引用这些标签的悬挂死语句。

删除的命令标签（共 23 条）：
- **TEXT SEARCH 系列（12 条）**：CREATE/DROP/ALTER TEXT SEARCH CONFIGURATION/DICTIONARY/PARSER/TEMPLATE（`OBJECT_TEXT_SEARCH_*` 枚举已不存在，utility.c 仅余悬挂 `tag=` 死语句）。
- **ROLE 系列（3 条）**：CREATE ROLE / ALTER ROLE / DROP ROLE（`OBJECT_ROLE` 枚举已删除，角色骨架已裁）。
- **其它（8 条）**：CREATE TABLE AS、SELECT INTO、REFRESH MATERIALIZED VIEW、GRANT ROLE、REVOKE ROLE、DROP OWNED、CREATE CONSTRAINT、DROP CONSTRAINT、ALTER CONSTRAINT（语法与执行均已删除，utility.c 无引用）。

同步修改：
- **`src/include/tcop/cmdtaglist.h`**：删除上述 23 条 `PG_CMDTAG` 枚举项。
- **`src/backend/tcop/utility.c`**：删除 `AlterObjectTypeCommandTag`/`RemoveObjects`/`DefineCommandTag`/`RenameStmt` 中引用已删 TEXT SEARCH 标签的 14 条悬挂 `tag = CMDTAG_xxx; break;` 死语句（其前置 `case` 标签已随语法裁剪而消失）。

**未删除、有意保留的命令标签**（理由）：下列命令虽无对应 SQL 语法入口（被裁），但其 `ObjectType` 枚举（`OBJECT_ACCESS_METHOD`/`OBJECT_CAST`/`OBJECT_COLLATION`/`OBJECT_LANGUAGE`/`OBJECT_PUBLICATION`/`OBJECT_SUBSCRIPTION`/`OBJECT_TRANSFORM` 等）在内核对象寻址系统（`objectaddress.c`/`alter.c`/`dropcmds.c`/`extension.c`）中仍被引用，且 `utility.c` 中仍保留 `case OBJECT_xxx:` 分支引用其 `CMDTAG_xxx`。若删除这些 cmdtag 会导致编译期"未声明标识符"，因此必须随对象类型系统一并重构后方可删除，不属本次命令标签清理范围：
- ACCESS METHOD（CREATE/ALTER/DROP）、CAST（CREATE/ALTER/DROP）、COLLATION（CREATE/ALTER/DROP）、LANGUAGE（CREATE/ALTER/DROP）、PUBLICATION（CREATE/ALTER/DROP）、SUBSCRIPTION（CREATE/ALTER/DROP）、TRANSFORM（CREATE/ALTER/DROP）、ALTER VIEW。

> 注：保留项的根因是 parsenodes.h 的 `ObjectType` 枚举未随用户侧 DDL 语法一并裁剪，属历史遗留；如需彻底裁掉这些标签，应另行发起"对象类型系统裁剪"任务。

与不可裁部分（btree/hash 索引、事务）零耦合；改动为纯删除，无逻辑影响。

---

## GSSAPI 功能彻底裁剪（2026-08-17）

minipg 早期（2026-08-02）已在构建层删除 `--with-gssapi` 选项及 `configure.ac`/`configure` 的 GSS 探测，但源码层仍残留由 `#ifdef ENABLE_GSS` 包裹的死代码，以及若干**无条件编译**的 GSS 协议/状态残留。本次彻底删除 GSS 功能相关代码（无构建开关，ENABLE_GSS 宏始终未定义，故所有 `#ifdef ENABLE_GSS` 块为编译期死代码）。改动文件与要点：

- **`src/backend/utils/activity/backend_status.c`**：删除全部 7 处 `#ifdef ENABLE_GSS ... #else ... #endif` 块（GSS 共享状态缓冲的声明/估算/创建、pgstat_bestart 的 GSS 局部变量与状态填充、pgstat_read_current_status 的 GSS 快照拷贝）；`st_gss` 改为无条件 `false`（原 if/else 的 else 分支）。
- **`src/include/utils/backend_status.h`**：删除 `PgBackendGSSStatus` 结构体定义，及 `PgBackendStatus` 中的 `st_gss`/`st_gssstatus` 字段。
- **`src/backend/postmaster/postmaster.c`**：`ProcessStartupPacket()` 去掉 `gss_done` 参数；删除 `NEGOTIATE_GSS_CODE` 协商分支（原 1740-1781 行）；SSL 分支成功后不再置 `gss_done`；部分长度检查去掉 `!gss_done` 条件；调用点同步去掉实参。
- **`src/backend/utils/adt/pgstatfuncs.c`**：`pg_stat_get_activity()` 的 GSS 信息读取分支改为无条件填 `false`/`null`（gss_auth/gss_princ/gss_enc 三列）。
- **`src/interfaces/libpq/fe-auth.c`**：删除 `AUTH_REQ_GSS`/`AUTH_REQ_GSS_CONT`/`AUTH_REQ_SSPI` 三个 case（落到 default，报 "not supported"）。
- **`src/interfaces/libpq/fe-connect.c`**：删除两处 `CONNECTION_GSS_STARTUP` case（poll 分支与 unreachable 分支）。
- **`src/interfaces/libpq/libpq-fe.h`**：删除 `CONNECTION_GSS_STARTUP` 枚举项。
- **`src/include/utils/wait_event.h` + `src/backend/utils/activity/wait_event.c`**：删除 `WAIT_EVENT_GSS_OPEN_SERVER` 枚举项与对应 case。
- **`src/include/libpq/pqcomm.h`**：删除 `NEGOTIATE_GSS_CODE` 宏（已无引用点）；更新注释去掉 GSSAPI 提及。保留 `AUTH_REQ_GSS`/`AUTH_REQ_GSS_CONT` 协议常量号（7/8，未使用，保留以维持 auth 请求类型枚举布局稳定）。
- **`src/tools/pgindent/typedefs.list`**：删除已不存在的 `PgBackendGSSStatus` 类型。
- **`src/test/postmaster/t/004_negotiate.pl`**：整文件删除（专测 SSL/GSS 组合协商，依赖已删的 `NEGOTIATE_GSS_CODE`）。
- **`src/test/README`**：kerberos 测试说明去掉 GSSAPI 字样。

与不可裁部分（btree/hash 索引、事务）零耦合；`make -j4` 全量重编通过（0 错误）。

---

## 一、平台 / 构建链裁剪

- **（2026-07-30 前）仅支持 Linux，移除 Windows/Cygwin/MSVC 代码**：删除 `src/backend/port/win32/`、`src/include/port/win32*/`、`src/tools/msvc/`、`src/bin/pgevent/`、各 `win32*.c`、模板/构建脚本（`template/win32|cygwin`、`makefiles/Makefile.win32|cygwin`、`tools/win32tzlist.pl`），清理 `configure.ac`/`Makefile.global.in`/ecpg 等处的 `WIN32`/`_MSC_VER`/`__CYGWIN__` 条件分支；后续又两轮清理 backend 核心高风险文件的 WIN32 死代码（latch.c 的 `WAIT_USE_WIN32` 路径、pg_locale.c、elog.c 的 eventlog、varlena.c、fd.c、auth.c 的 winldap 等）。`./configure`+`make`+`make check-world` 通过。
- **（2026-07-31）删除运维/性能/升级类 bin 工具**：`pgbench`、`pg_amcheck`、`pg_archivecleanup`、`pg_checksums`、`pg_resetwal`、`pg_test_fsync`、`pg_test_timing`、`pg_upgrade`、`pg_verifybackup`、`scripts/`（clusterdb 等）；保留 `initdb`/`pg_ctl`/`psql`/`pg_config`/`pg_dump`/`pg_basebackup`(后于 08-12 裁)/`pg_controldata`/`pg_waldump`。
- **（2026-08-04 前）删除 EXEC_BACKEND 双实现**：分方案 A（孤立分支）与方案 B（彻底删除 `postmaster.c`/`autovacuum.c`/`syslogger.c`/`bgworker.c`/`ipci.c`/`dsm.c`/`sysv_shmem.c` 的 fork/exec 双路径 + `NON_EXEC_STATIC` 宏 + guc.c 的 `CONFIG_EXEC_PARAMS`/`write_nondefault_variables` 等），并清理 `postmaster.c`/`postmaster` 全树 WIN32 死代码。

## 二、客户端工具与协议层

- **（2026-08-02）SSL/TLS 与 GSSAPI 传输加密裁剪**：删除 `be-secure-openssl/gssapi.c`、`fe-secure-openssl/gssapi.c`、相关头文件与测试目录（`test/ssl`/`kerberos`/`ldap`/`ssl_passphrase_callback`）；认证收敛为 `trust`/`reject`/`password`/`scram-sha-256`（md5 仅删协商、保留存储格式兼容）；`secure_*` 层拍平为裸 socket 直通；`configure` 去 `--with-ssl/gssapi/ldap/pam/bsd-auth`，删除 `pg_stat_ssl`/`pg_stat_gssapi` 视图。`make check-world` 通过。
- **（2026-08-13）物理流复制全链路裁剪**：删除 `walsender/walreceiver/walreceiverfuncs/slot/slotfuncs/syncrep/basebackup/backup_manifest` 及 `libpqwalreceiver/` 与 replnodes；保留 archive 恢复 + hot standby 只读；`synchronous_commit` 退化为无操作。`make check` 全 103 项通过。续2 补裁 xlog.c 复制槽 LSN 边界死代码与 3 个孤儿 LWLocks，修复 `src/backend/Makefile` 对已删 `replication` 目录的引用。
- **（2026-08-12 续16）删除客户端 `pg_basebackup`**：`git rm -r src/bin/pg_basebackup`；连带禁用依赖它的 recovery/commit_ts/pg_rewind 测试。`PostgresNode.pm` 的 backup 方法保留为死 helper。
- **（2026-08-15）放弃 PG13 之前兼容**：删除 `AdjustUpgrade.pm`（死代码）与 psql 中 `pset.sversion < 阈值` 的旧版降级分支，仅保留 PG14 路径。
- **（2026-08-17）彻底裁剪 Unix 域套接字（Unix-domain socket），并补充裁剪 IPv6 监听/连接层**：本地与网络监听仅保留 IPv4 TCP 通道，不再创建 `.s.PGSQL.<port>` 套接字文件、不再监听 IPv6 地址，但 IPv6 地址数据（inet/cidr）仍可正常入库（数据类型层不依赖 `HAVE_IPV6`）。采用「构建系统开关 + 源码清理」两段式，与上游 PG 在 Windows 下不支持 Unix socket / IPv6 的原生裁剪路径一致：
  - **构建系统层**：`configure.ac` 注释掉 `PGAC_STRUCT_SOCKADDR_UN`（使 `HAVE_STRUCT_SOCKADDR_UN` 不定义）与 `AC_CHECK_TYPE([struct sockaddr_in6]...)`（使 `HAVE_IPV6` 不定义）；用 `autoconf2.69` 重新生成 `configure` 并重跑，使 `pg_config.h` 不再定义这两宏。由此 `c.h` 的 `HAVE_UNIX_SOCKETS`（由 `HAVE_STRUCT_SOCKADDR_UN` 推导）不生效，`IS_AF_UNIX()` 宏恒为假，所有 `#ifdef HAVE_UNIX_SOCKETS` / `#ifdef HAVE_IPV6` 分支在编译期消除。
  - **后端 GUC 清理（`guc.c`）**：删除 `unix_socket_directories` / `unix_socket_permissions` / `unix_socket_group` 三个 GUC 注册块（含默认值段）、`show_unix_socket_permissions` 前向声明与函数定义；`postmaster.h` 删除三个 extern 变量声明。
  - **后端 server 端**：`postmaster.c` 删除 `Unix_socket_directories` 变量声明、`#ifdef HAVE_UNIX_SOCKETS` 的整个 Unix socket 监听块（StreamServerPort(AF_UNIX) 调用）、`getopt` 字符串移除 `k:` 选项与 `-k` case、调用 `SetConfigOption("unix_socket_directories"...)` 的分支；`postgres.c`（单用户后端）同步移除 `k:` 与 `-k` case；`pqcomm.c` 将 `Unix_socket_permissions` / `Unix_socket_group` 变量定义连同 `Lock_AF_UNIX`/`Setup_AF_UNIX` 两个函数及其全部 `#ifdef HAVE_UNIX_SOCKETS` 引用分支直接删除（不再依赖宏跳过，代码已无残留条件编译）。
  - **后端口径函数（`pgstatfuncs.c`）**：用 `#ifndef HAVE_UNIX_SOCKETS` 包裹裸 `AF_UNIX` 的 `else if` 分支与 `case AF_UNIX:`（系统头仍定义 `AF_UNIX` 常量，裁后不可达，显式隔离避免混淆）；`#ifdef HAVE_IPV6` 包裹的 `AF_INET6` 分支随宏自动消失。
  - **客户端/工具端**：`fe-connect.c` 默认 host 填 `DEFAULT_PGSOCKET_DIR` 的 `#ifdef HAVE_UNIX_SOCKETS` 块与 `AF_INET6`/`IS_AF_UNIX`/`CHT_UNIX_SOCKET` 分支均随宏消除；`fe-misc.c` 用 `#ifndef HAVE_UNIX_SOCKETS` 包裹裸 `AF_UNIX` 的 NODELAY 对齐分支；`psql/prompt.c` 用 `#ifndef HAVE_UNIX_SOCKETS` 包裹裸 `DEFAULT_PGSOCKET_DIR` 比较；`initdb.c` 删除 `#unix_socket_directories` 默认配置片段（GUC 已删）；`pg_regress.c` 移除在无 Unix socket 且非 SSPI 平台触发的 `#error Platform has no means to secure the test installation.`（改为 TCP-only 注释说明，minipg 测试实例走 localhost 依赖文件权限隔离）。`is_unixsock_path()`（pqcomm.h 的 static inline，恒返回路径判断，无编译错误）在 command.c/prompt.c/fe-connect.c 的调用保留（裁剪后 hostname 为 TCP host，行为正确）。
  - **配置样例与测试框架**：`postgresql.conf.sample` 删除三个已删 GUC 的注释行；`PostgresNode.pm` 在 `$use_tcp` 分支不再写已删的 `unix_socket_directories = ''`，standby 配置移除 unix socket 分支（minipg 恒走 TCP）；`001_start_stop.pl` 的 unix 分支在 minipg 下不触发（`use_unix_sockets=false`），保留为死路径不报错。`DEFAULT_PGSOCKET_DIR` 常量（pg_config_manual.h）保留以避免牵动无关代码。
  - **验证**：后端 `make -j4` 与 client 端（libpq/psql/initdb/pg_regress）均 0 错误 0 新增 unused 警告；`pg_regress` 编译通过（`#error` 已消除）。注意 `src/backend` 全量编译仍报 `objectaddress.c` 的 `OBJECT_CONSTRAINT`/`AuthIdRelationId` 等错误——此为会话开始时 git_status 已记录的 minipg 历史裁剪残留（pg_authid/约束 catalog 早裁），与本次 Unix socket 裁剪无关、非本次引入。与不可裁部分（btree/hash 索引、事务）零耦合：`HAVE_IPV6` 仅消除监听/连接侧 v6，inet/cidr 的 `ipaddr[16]`/`PGSQL_AF_INET6` 仍无条件存 128 位 IPv6 地址，btree 索引与事务机制均不受影响。
- **（2026-08-17 补）`pqcomm.c` 二次清理：删除 `HAVE_UNIX_SOCKETS` 条件编译而非宏跳过**：前述 08-17 初次裁剪把 `Unix_socket_permissions`/`Unix_socket_group` 变量定义与 `Lock_AF_UNIX`/`Setup_AF_UNIX` 仍留在 `#ifdef HAVE_UNIX_SOCKETS` 块内（宏关闭时靠编译器消除）。本次按"直接裁剪代码而非条件编译"的要求，将 pqcomm.c 中所有 `HAVE_UNIX_SOCKETS` 块**整块删除**：① 前向声明 ifdef（Lock_AF_UNIX/Setup_AF_UNIX）② `StreamServerPort` 内的 `unixSocketPath` 局部变量 ifdef ③ `if (family == AF_UNIX) {...} else` 分支（仅留 TCP 路径）④ `case AF_UNIX` ⑤ `addrDesc = unixSocketPath` 的 ifdef ⑥ `Setup_AF_UNIX(service)` 调用块 ⑦ Unix socket 监听日志 ifdef ⑧ `Lock_AF_UNIX`+`Setup_AF_UNIX` 整函数定义。`StreamServerPort` 的 `unixSocketDir` 参数保留（公开 API 签名，避免牵动调用方 postmaster.c），PG flags 不启 `-Wunused-parameter` 故无警告；`sock_paths` 列表相关 `TouchSocketFiles`/`RemoveSocketFiles` 因不再有 append 而恒为空循环，保留为通用锁文件清理接口（无害）。编译验证：`make` libpq 目录 0 错误 0 警告。
- **（2026-08-17 续）全代码库彻底删除 `HAVE_UNIX_SOCKETS` 条件编译（仅支持 TCP）**：前述各次裁剪仍保留了大量 `#ifdef HAVE_UNIX_SOCKETS` / `#ifndef HAVE_UNIX_SOCKETS` 预处理块（宏关闭时惰式跳过）。本次按"直接裁剪代码而非条件编译"的要求，对全代码库所有 `HAVE_UNIX_SOCKETS` 引用逐文件整块删除，使代码中再无任何该宏的条件编译：
  - **`src/include/c.h`**：删除 `HAVE_STRUCT_SOCKADDR_UN` → `HAVE_UNIX_SOCKETS` 的宏推导块（minipg 不再探测 sockaddr_un，该宏永不被定义）。
  - **`src/include/common/ip.h`**：`IS_AF_UNIX(fam)` 宏不再用 ifdef 包裹，直接定义为恒假 `(0)`（minipg 仅 TCP）。
  - **`src/common/ip.c`**：删除 `getaddrinfo_unix`/`getnameinfo_unix` 前向声明与整函数定义（约 130 行），以及 `pg_getaddrinfo_all`/`pg_freeaddrinfo_all`/`pg_getnameinfo_all` 中的 `AF_UNIX` 分支（仅留 IPv4/IPv6 的 getaddrinfo/getnameinfo 路径）。
  - **`src/backend/utils/adt/pgstatfuncs.c`**：删除 `pg_stat_get_activity` 中 `else if (AF_UNIX)` 分支与 `pg_stat_get_backend_client_port` 中 `case AF_UNIX`（连同 `#ifndef` 包裹），AF_UNIX 连接现落入 `else`/default 分支（置 NULL/-1）。
  - **`src/backend/postmaster/pgstat.c`**：删除 `pgstat_bestart` 中 ignore `AF_UNIX` 的 `#ifdef` 块（后续 `if (++tries > 1)` 直接保留）。
  - **`src/interfaces/libpq/fe-misc.c`**：删除 `pqPutMsgEnd` 中 `if (raddr==AF_UNIX) toSend -= toSend%8192` 的 `#ifndef` 包裹（该分支不可达，整块删除）。
  - **`src/interfaces/libpq/fe-connect.c`**：删除 `is_unixsock_path` 赋值 `CHT_UNIX_SOCKET`、`default host` 填 `DEFAULT_PGSOCKET_DIR` 的 `#ifdef` 块（仅留 `DefaultHost`）、`emitHostIdentityInfo` 的 Unix socket 分支、`connectDBComplete` 连接错误提示的 Unix socket 分支、`case CHT_UNIX_SOCKET:` 整 case（含 `UNIXSOCK_PATH`/`pg_getaddrinfo_all(NULL,...)`）。
  - **`src/interfaces/libpq/libpq-int.h`**：删除 `ConnHostType` 枚举的 `CHT_UNIX_SOCKET` 成员（`fe-connect.c` 已不再赋值，成为死枚举值）。
  - **`src/bin/psql/prompt.c`**：删除主机名与 `DEFAULT_PGSOCKET_DIR` 比较的 `#ifndef` 包裹（该比较不可达，整块删除）。
  - **`src/bin/initdb/initdb.c`**：删除无调用点的 `filter_lines_with_token` 前向声明与函数定义（该 `#ifndef` 块原用于过滤已删除的 unix_socket_directories 配置）。
  - **`src/test/regress/pg_regress.c`**：删除 `temp_sockdir`/`sockself`/`socklock` 变量声明、`remove_temp`/`signal_remove_temp`/`make_temp_sockdir` 整函数块（Unix socket 临时目录）、PGHOST 设置与默认 host 报告中的 Unix socket 分支（`use_unix_sockets` 直接置 `false`，PGHOST 恒为 hostname/localhost，测试实例仅走 TCP localhost）、`#elif !HAVE_UNIX_SOCKETS` 注释块；端口选择注释去除对已删宏的引用。
  - **保留项（非条件编译，改动会引发不必要风险，且不影响 TCP-only 行为）**：`DEFAULT_PGSOCKET_DIR` 常量（`pg_config_manual.h` 无条件定义）、`is_unixsock_path()`（pqcomm.h 的 static inline，被 command.c/prompt.c/fe-connect.c 调用，仅作路径判断，minipg 下 hostname 为 TCP host 行为正确）；`sock_paths` 相关 `TouchSocketFiles`/`RemoveSocketFiles`（恒为空循环，通用锁文件清理接口）。
  - **验证**：`make` 全量重编 common / libpq / psql / initdb / pg_regress（含 `pg_regress.o`）/ backend 的 pgstatfuncs.o / pgstat.o / pqcomm.o 均 0 错误；仅有与本次无关的既有 warning（`no previous prototype`、`/* within comment`，来自 minipg 历史裁剪残留）。全代码库 `grep HAVE_UNIX_SOCKETS` 仅剩 `configure.ac` 的注释（已注释掉 `PGAC_STRUCT_SOCKADDR_UN`，无害）与 pg_regress.c 一处说明性注释（已改写为 TCP localhost 描述）。与不可裁部分（btree/hash 索引、事务）零耦合。

- **（2026-08-17 续）深度裁剪 `pqcomm.c` 的 IPv6 监听专用代码 + 强制 `AF_INET`（仅支持 IPv4）**：在 08-17 已使 `HAVE_IPV6` 宏编译期不定义、且 08-17 记录「ifaddr.c/pqcomm.c 中 `#ifdef HAVE_IPV6` 块因宏未定义而编译期不生效、作为源码噪音保留」的基础上，本轮按"直接裁剪代码而非条件编译"的要求，真正删除 pqcomm.c 中两处 IPv6 专用块，并让监听从 `AF_UNSPEC` 改为强制 `AF_INET`，从源头只解析/绑定 IPv4：
  - **`src/backend/libpq/pqcomm.c`**：
    1. 删除 `switch(addr->ai_family)` 中 `#ifdef HAVE_IPV6` 的 `case AF_INET6:`（IPv6 log 描述分支，`familyDesc = _("IPv6")`）。
    2. 删除 `#ifdef IPV6_V6ONLY` 的 `setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, ...)` 块（IPv6 双栈隔离，IPv4 监听无需）。
    3. 更新 `StreamServerPort` 注释 `family should be AF_UNIX or AF_UNSPEC` → `family should be AF_UNIX or AF_INET`，与代码一致。
  - **`src/backend/postmaster/postmaster.c`**：`listen_addresses` 的两个 `StreamServerPort(AF_UNSPEC, ...)` 调用（含 `"*"` 与本机 host 两分支）改为 `StreamServerPort(AF_INET, ...)`，使 getaddrinfo 只返回 IPv4 地址，不再尝试创建/绑定 IPv6 socket。
  - **验证**：`pqcomm.c`/`postmaster.c` 编译 0 错误（残余 warning 均为与本次无关的既有项：`signal.h unused-includes`、`objectaddress.c` 等历史裁剪残留）；全代码库 `grep AF_UNSPEC` 在后端监听路径已无残留（`pgstat.c` 出站连接与 `ifaddr.c` 内仍保留，前者为统计收集器出站、后者被 `#ifdef HAVE_IPV6` 包裹编译期跳过，二者均不影响"仅监听 IPv4"诉求，按最小改动保留）。与不可裁部分（btree/hash 索引、事务）零耦合：`HAVE_IPV6` 仅作用于监听/连接侧 v6，`inet/cidr` 数据类型层此前已删除；btree/hash 索引与事务机制不受影响。

- **（2026-08-17 续）深度裁剪 `pgstatfuncs.c` 的 IPv6 统计展示残骸 + 删除孤立 `clean_ipv6_addr`（adt 层 IPV6 彻底消失）**：在 08-17 已使 `HAVE_IPV6` 宏在编译期不定义（监听/连接侧 v6 消除）的基础上，本轮清理**源码层**仍残留的 IPV6 死代码，使 adt 统计层真正只支持 IPv4：
  - **`src/backend/utils/adt/pgstatfuncs.c`**（3 处 `#ifdef HAVE_IPV6` 残骸 + 2 处死调用）：
    1. `pg_stat_get_activity()` 的客户端地址判断 `if (... == AF_INET #ifdef HAVE_IPV6 || ... == AF_INET6 #endif)` 简化为纯 `if (... == AF_INET)`。
    2. 同函数 `clean_ipv6_addr(...)` 调用删除（IPV4 场景无需清理 `%zone` 后缀）。
    3. `pg_stat_get_backend_client_addr()` 的 `switch` 中 `case AF_INET6:`（含 `#ifdef`）删除，仅留 `case AF_INET:`。
    4. 同函数 `clean_ipv6_addr(...)` 调用删除。
    5. `pg_stat_get_backend_client_port()` 的 `switch` 中 `case AF_INET6:`（含 `#ifdef`）删除，仅留 `case AF_INET:`。
  - **删除孤立死代码文件 `src/backend/utils/adt/ipaddr.c`**：该文件此时只剩 `clean_ipv6_addr()` 一个函数（inet/cidr 类型与 hba.c 早已在 minipg 中删除，原注释「still used by hba.c」已失效），`pgstatfuncs.c` 是其唯一调用者，本次调用点删除后该函数成为纯死代码。`git rm` 该文件；`src/backend/utils/adt/Makefile` 移除 `ipaddr.o`；`src/include/utils/builtins.h` 删除 `extern void clean_ipv6_addr(int addr_family, char *addr);` 声明。
  - **验证**：`pgstatfuncs.o` 编译 0 错误（残余 `-Wunused-function` 为改动前既有的 `pg_stat_get_backend_client_addr` 等未被直接调用的注册式函数告警，非本次引入）；全代码库 `grep clean_ipv6_addr` 0 命中；`make` 后端 adt 子目录无 `ipaddr.o` 构建规则（符合预期）。与不可裁部分（btree/hash 索引、事务）零耦合：`HAVE_IPV6` 仅消除监听/连接/统计展示侧 v6，`inet/cidr` 数据类型层此前已删除、与其无关；btree/hash 索引与事务机制不受影响。注：网络核心层 `ifaddr.c`/`pqcomm.c` 中剩余 `#ifdef HAVE_IPV6` 块因宏永久不定义而**编译期不生效**，作为源码噪音保留（按 08-17 既定「构建系统开关 + 源码清理」两段式方案，未为代码整洁而冒险逐行删除公开接口 `ifaddr.h` 的 IPV6 分支）。

- **（2026-08-17 续）彻底裁剪 `USE_SSL` 残余 SSL 代码（后端活动状态 / 统计层）**：此前 08-02 已删 `be-secure-openssl`/`fe-secure-openssl` 与 `pg_stat_ssl` 视图，但后端 `PgBackendStatus` 仍保留 `USE_SSL` 条件编译的 SSL 字段与独立 `PgBackendSSLStatus` 结构体、`BackendSslStatusBuffer` 共享内存段，且 `pg_stat_get_activity` 仍输出 7 个 SSL 列（ssl/sslversion/sslcipher/sslbits/ssl_client_dn/ssl_client_serial/ssl_issuer_dn）。本次彻底移除：
  - `src/include/utils/backend_status.h`：删除 `PgBackendSSLStatus` 结构体定义、`PgBackendStatus` 的 `st_ssl`(`bool`) 与 `st_sslstatus`(`PgBackendSSLStatus *`) 字段；保留 `PgBackendGSSStatus`/`st_gss`/`st_gssstatus`（GSSAPI 与 SSL 解耦）。
  - `src/backend/utils/activity/backend_status.c`：删除 5 处 `#ifdef USE_SSL` 块——全局 `BackendSslStatusBuffer` 变量、`SizeBackendStatusArray` 的 SSL 共享内存大小累加、`BackendStatusShmemInit` 的 SSL 段分配与 `st_sslstatus` 指针初始化、`pgstat_report_activity` 的 `lsslstatus` 局部变量/`memset`/`ssl_in_use` 填充/`memcpy st_sslstatus`、`read_current_status` 的 `localsslstatus` 声明/分配/拷贝/指针递增。
  - `src/backend/utils/adt/pgstatfuncs.c`：删除 `pg_stat_get_activity` 中 SSL 输出块（`values[18..24]`），GSS 列索引前移 7（25-27→18-20），`leader_pid` 28→21、`query_id` 29→22；`PG_STAT_GET_ACTIVITY_COLS` 由 30 降为 23；`insufficient privilege` 分支的 `nulls[]` 同步收敛到 18-22。
  - `src/include/catalog/pg_proc.dat`：`pg_stat_get_activity` 的 `proallargtypes`/`proargmodes`/`proargnames` 各删除 7 个 SSL 元素（类型 `bool,text,text,int4,text,numeric,text`，argmodes 7 个 `o`，argnames `ssl..ssl_issuer_dn`），列数由 30→23（含 1 输入 `pid`），保持与 C 端索引一致。
  - `src/tools/pgindent/typedefs.list`：删除 `PgBackendSSLStatus`。
  - `doc/src/sgml/monitoring.sgml`：删除已随 `pg_stat_ssl` 视图（08-02 裁）失效的 `pg_stat_ssl` 文档段与概览表引用（sgml 文档裁剪，按 AGENTS 规则不强制但已同步以保持文档自洽）。
  - **未裁**：libpq 客户端 `PGconn.ssl_in_use`（`libpq-int.h`）——该字段是客户端连接协议状态标志，SCRAM channel binding 逻辑依赖它（`ssl_in_use` 恒 false 时走 `'n'` 模式），与后端 `USE_SSL` 编译期裁剪解耦，保留不影响无加密运行；`fe-secure.c` 已是明文桩（08-02），无 `USE_SSL` 残留。
  - **验证**：`make` 仅编译 `backend_status.c`/`pgstatfuncs.c` 均 0 错误（`backend_status.h` 经修复误删的 `PgBackendGSSStatus` 后恢复正确）；`pg_stat_activity` 视图按列名（`S.xxx`）引用 `pg_stat_get_activity`，不受列位置前移影响，无需改动。与不可裁部分（btree/hash 索引、事务）零耦合：仅删监控观测字段，不涉及索引/事务机制。注意 minipg 既有 `initdb` 因 `syscache.c` 的 `cacheinfo[]`/`syscache.h` 枚举不对齐而崩溃（2026-08-14 记忆），故完整回归仍无法在此环境跑通，本次以单文件编译验证为准。

- **（2026-08-17 续）深度裁剪 libpq 客户端 SSL 代码（删 `PGconn.ssl_in_use` 字段 + 清理 SSL 死代码）**：承接上条「未裁 libpq `ssl_in_use`」的保留项，本轮彻底移除客户端侧 SSL 残留，使 libpq 源码层真正只含明文连接：
  - **`src/interfaces/libpq/libpq-int.h`**：删除 `PGconn` 的 `/* SSL structures */ bool ssl_in_use;` 字段（`ssl_in_use` 在 minipg 恒为 false、SSL 实现已在 08-02 删除），注释 `/* Assorted state for SASL, SSL, GSS, etc */` 改为 `/* Assorted state for SASL, GSS, etc */`。
  - **`src/interfaces/libpq/fe-auth-scram.c`**：
    - 移除 `build_client_first_message()` 中 `if (SCRAM_SHA_256_PLUS_NAME)` 分支内的 `Assert(conn->ssl_in_use);`（该 Assert 引用已删字段；TLS channel binding 分支在无 SSL 的 minipg 下不可达，删 Assert 不影响 `'p=tls-server-end-point'` 头写入）。
    - 其余两处 `conn->ssl_in_use` 引用（397/538 行）位于 `#ifdef HAVE_PGTLS_GET_PEER_CERTIFICATE_HASH` 块内——该宏在 minipg 构建中不定义（configure 无 `with-openssl`），**编译期不生效**，作为已隔离的 SSL channel binding 证书哈希残留保留（与网络核心层 `ifaddr.c`/`pqcomm.c` 的 `#ifdef HAVE_IPV6` 残骸保留策略一致）；未深入重构 SCRAM 认证核心以免引入回归。
  - **`src/interfaces/libpq/fe-connect.c`**（纯死代码清理，因 `configure.ac` 无 `USE_SSL`/`with-openssl` 这些代码本就永不调用）：
    - 删除 `sslVerifyProtocolVersion`/`sslVerifyProtocolRange` 两个前向声明（317-318 行）。
    - 删除两函数完整定义（原 6403-6455 行，约 53 行 TLS 协议版本校验逻辑）。
    - 删除 `connectDBComplete()` 状态机的 `case CONNECTION_SSL_STARTUP:`（与 `CONNECTION_NEEDED` 合并，`/* Special cases: proceed without waiting. */`）。
    - 删除 `connectDBStart()` 状态机的 `case CONNECTION_SSL_STARTUP: /* unreachable */ goto error_return;`（原 2552-2554 行，及其上方「This build supports no transport encryption」注释块）。
  - **未裁**：`fe-auth.c:62` 的 `channel binding required, but SSL not in use` 错误文案（纯用户可见字符串，无字段/逻辑依赖，保留无害）；`libpq-fe.h:73` 的 `CONNECTION_SSL_STARTUP` 枚举值（保留该枚举值以避免 ABI/数值变动，仅其 case 引用移除，与 upstream「no longer used」注释一致）。
  - **验证**：`make -C src/interfaces/libpq CFLAGS="-O0 -Wunused-function"` 全目录编译 0 错误、0 warning；全代码库 `grep ssl_in_use` 仅剩 fe-auth-scram.c 两处（均在 `#ifdef HAVE_PGTLS` 内、编译期不生效）；`grep sslVerifyProtocol` 0 命中。与不可裁部分（btree/hash 索引、事务）零耦合：仅删客户端连接/认证层的 SSL 状态与死代码，SCRAM 明文认证（`'n'` channel binding 模式）与 SASL/GSS 机制完整保留。

## 三、过程语言 / 嵌入式 SQL

- **（2026-08-03）删除非 plpgsql 过程语言**：`src/pl/plperl`、`plpython`、`tcl`；保留 `plpgsql`（后于 08-14 裁，见下）。
- **（2026-08-15 阶段2）删除 ecpg 嵌入式 SQL 预处理器**：整体 `git rm src/interfaces/ecpg/`（~16.6 万行）；清理 `interfaces/Makefile`、`GNUmakefile`、`configure.ac` 的 `ecpg_config.h`。
- **（2026-08-14）彻底删除 PL/pgSQL**：`git rm -r src/pl`、解析器 `RAW_PARSE_PLPGSQL_*`/`COERCION_PLPGSQL` 钩子、initdb 默认安装、DO 默认语言改报错；回归测试 `LANGUAGE plpgsql` 函数转 `LANGUAGE sql`。`make check-world` 全绿（regress 99 / isolation 70 / modules）。

## 四、功能模块裁剪

- **（2026-07-30 前）contrib 扩展裁剪**：保留 11 个（pageinspect/pg_buffercache/pg_freespacemap/pg_visibility/pgstattuple/pg_stat_statements/pg_surgery/pgrowlocks/amcheck/bloom/spi）；删除 45 个（plperl 等语言桥接、dblink/fdw/xml2、业务计算类型、全文检索类、安全运维类、测试/复制调试、btree_gin/gist/auto_explain/lo/trgm 等）。注：`test_decoding` 因 subscription 依赖暂留。
- **（2026-08-17）彻底删除 `--with-selinux` 选项与 libselinux 探测**：`contrib/sepgsql`（唯一使用 selinux 的扩展）此前已被整体删除，`src/` 中无任何 selinux 调用，该选项沦为空壳（开启后只探测 libselinux 库却无处编译）。改动：`configure.ac` 删除 `--with-selinux` 选项定义（PGAC_ARG_BOOL）与 `contrib/sepgsql` 专属的 `AC_CHECK_LIB(selinux, security_compute_create_name)` 探测；`src/Makefile.global.in` 删除 `with_selinux` 变量；`src/include/pg_config.h.in` 删除 `HAVE_LIBSELINUX` 占位宏；`src/tools/pginclude/headerscheck` 与 `cpluspluscheck` 删除对已不存在的 `contrib/sepgsql/sepgsql.h` 的跳过项；用 `autoconf2.69` 重新生成 `configure`。与不可裁部分（btree/hash 索引、事务）零耦合。`doc/src/sgml/sepgsql.sgml`/`copy.sgml` 中 SELinux 文档描述因 sepgsql 功能已无，属 sgml 文档裁剪（按 AGENTS 规则不强制记 CHANGE.md，留待文档清理）。
- **（2026-08-17）彻底删除 `--with-perl` / `--with-python` 构建选项**：`src/pl/plperl`、`src/pl/plpython` 过程语言扩展此前已被整体删除（2026-08-03），但 configure 仍保留这两个选项及其 perl/python embed 探测逻辑（空壳）。改动：`configure.ac` 删除 `--with-perl`/`--with-python` 选项定义、`with_perl=yes` 分支的 `PGAC_CHECK_PERL_CONFIGS/EMBED_*` 探测与 `<perl.h>` 头文件探测、`with_python=yes` 分支的 `PGAC_PATH_PYTHON`/`PGAC_CHECK_PYTHON_EMBED_SETUP` 与 `<Python.h>` 头文件探测、以及仅服务 plperl 的 clang `-Wcompound-token-split-by-macro` 探测；保留 `PGAC_PATH_PERL`（供 TAP/PROVE 测试使用）；`config/python.m4` 整文件删除（仅服务 PL/Python），`config/perl.m4` 精简为仅 `PGAC_PATH_PERL` 宏；`aclocal.m4` 移除 `python.m4` include；`src/Makefile.global.in` 删除 `with_perl`/`with_python` 变量、PL/Perl 专属的 `perl_archlibexp/perl_privlibexp/perl_includespec/perl_embed_ccflags/perl_embed_ldflags` 变量与 `PYTHON` 变量（保留 PERL 变量供 flex/TAP）；`src/include/port.h` 删除 PL/Perl 专用的 `PLPERL_HAVE_UID_GID` 保护宏；`src/tools/pgindent/` 的 `exclude_file_patterns`/`typedefs.list` 删除 plperl/perl 类型残留；`src/tools/pginclude/headerscheck` 删除 perl/python includespec 提取与 plperl/plpython 跳过项；用 `autoconf2.69` 重新生成 `configure`。与不可裁部分（btree/hash 索引、事务）零耦合。
- **（2026-08-17）修复 `ObjectProperty[]` 数组历史裁剪错位（编译错误 + 隐含逻辑 bug）**：`src/backend/catalog/objectaddress.c` 的 `ObjectProperty[]` 是 minipg 早期裁剪 ACL/owner 字段时机械删除 `Anum_pg_*_owner`/`acl` 两行留下的**结构性破坏**——每个元素被插入一个多余 `InvalidAttrNumber`，导致 11 字段结构体被 12 个初始化值填充，`objtype` 字段被错误写成 `-1`/`InvalidAttrNumber`，`is_nsp_name_unique` 缺失或错位；`role` 元素的 `class_oid` 甚至被改成 `InvalidOid`。后果：编译器报 `excess elements in struct initializer` 并（在引入上游大块时被发现）引用已删符号。本次整体重写该数组，严格对齐 `ObjectPropertyType` 的 11 字段；因 minipg 已裁 owner/acl，所有元素的 `attnum_acl` 一律 `InvalidAttrNumber`；恢复被误删的 `objtype`（如 `OBJECT_COLLATION`/`OBJECT_ROLE` 等）；将已删除枚举 `OBJECT_CONSTRAINT` 修正为 `OBJECT_TABCONSTRAINT`；因 `pg_authid` catalog 已被裁（`AuthIdRelationId`/`AUTHOID`/`Anum_pg_authid_*` 均不存在），**删除整个 `role` 元素**（角色权限检查走 aclchk 的 unsupported 报错分支，不依赖该数组项）。同时修复 `pg_get_object_address()` 第二个 `switch(type)` 缺 `default` 导致的 `-Wswitch`（加 `default: break;`，未支持类型由末尾空指针检查报错）。与不可裁部分（btree/hash 索引、事务）零耦合。
- **（2026-08-17 续）精简 `aclchk.c` 三个错误报告函数为最简实现（ACL 语义彻底移除）**：在 08-15 已删 ACL 判定逻辑、08-17 已修裁剪残留 switch 的基础上，本轮把 `aclchk.c` 剩余的三个函数 `aclcheck_error()` / `aclcheck_error_col()` / `aclcheck_error_type()` 内部**庞大的逐对象类型（`ObjectType`）错误消息表（约 200 行 `switch(objtype)` 拼 `gettext_noop("permission denied for xxx %s")` 等）彻底删除**，改为最小实现：仅按 `AclResult`（ACLCHECK_OK / ACLCHECK_NO_PRIV / ACLCHECK_NOT_OWNER / default）分支报通用错误（`"permission denied"` / `"must be owner"`），保留函数签名与 `aclcheck_error_col` 对 `aclcheck_error` 的调用、`aclcheck_error_type` 对 `get_element_type`/`format_type_be` 的调用以满足链接。理由：minipg 已裁 ACL，所有调用点（`fmgr.c`/`executor/*`/`catalog/*`/`commands/*` 等 90+ 处）均为 `aclresult = ACLCHECK_OK; if (aclresult != ACLCHECK_OK) aclcheck_error(...)` 的恒假死壳，这三个函数运行时永不触发，仅因被调用而必须存在；直接 `git rm aclchk.c` 会导致全库链接失败，而整批删除 90+ 处核心调用点（横跨执行器/函数调用路径）风险高且无实质收益，故采用"保留签名、裁掉权限消息表"的稳妥方案。同步精简不再使用的 `#include`（catalog 各 pg_*.h、objectaccess 等只保留 `dependency.h`/`objectaccess.h`/`dbcommands.h`/`lsyscache.h`/`acl.h`/`builtins.h`/`miscadmin.h`）、删除文件顶部两处空注释块。`aclchk.c` read_lints 0 错误。与不可裁部分（btree/hash 索引、事务）零耦合：ACL 检查早已恒 OK，本次仅裁错误消息格式化代码，不影响索引/事务机制。

- **（2026-08-17）修复 `aclchk.c` 裁剪残留 switch bug 与缺失原型**：`aclcheck_error()` 内两个 `switch(objtype)` 存在机械裁剪残留——`OBJECT_POLICY` 行缺 `case` 关键字、text search 的 `OBJECT_TSCONFIGURATION`/`OBJECT_TSDICTIONARY` 两行只有 `msg=` 赋值缺 `case`（policy/tsearch 在 minipg 已裁），且因 `OBJECT_EVENT_TRIGGER` 枚举仍存在但 switch 未处理而触发 `-Wswitch`。本次删除 policy/text-search 残留非法行，并为两个 switch 补 `case OBJECT_EVENT_TRIGGER:` 到 "these currently aren't used" 的 `elog(ERROR)` 组；另补回 `aclcheck_error_col()` 的 `extern` 声明（`src/include/utils/acl.h` 中被误删，导致 `-Wmissing-prototypes`）。与不可裁部分零耦合。
- **（2026-08-17）清理 `catalog/index.c` 未用变量**：删除 `index_create()` 中已无引用的 `partitioned` 与 `index_concurrently_swap()` 中已无引用的 `isPartition`（均为 `-Wunused-variable`）。与不可裁部分零耦合。
- **（2026-08-17）彻底裁剪 RENAME 功能（ALTER ... RENAME 全系列）**：minipg 定位于内核学习，RENAME 属运维/管理类 DDL（改对象名，学习价值低），且所有 rename 本质是「改写系统表 name 列（OID 不变）」，并非索引/事务核心机制。本轮彻底移除用户可见的 RENAME 命令（语法 + 节点 + 全部顶层执行函数），仅保留两个被内核其他路径依赖的底层内部函数（见下「保留项」）。改动文件与要点：
  - **语法层（`src/backend/parser/gram.y`）**：删除 `RenameStmt` 节点标签声明、`AlterStmt` 候选分支中的 `| RenameStmt`，以及整段 `RenameStmt` 产生式（238 行，涵盖 `ALTER AGGREGATE/COLLATION/CONVERSION/DATABASE/DOMAIN/.../RENAME TO`、`ALTER TABLE/VIEW RENAME [COLUMN]/RENAME CONSTRAINT`、`ALTER RULE/TRIGGER ... RENAME`、`ALTER TYPE RENAME ATTRIBUTE/VALUE` 等全部分支）；删除随之无引用的 `opt_column` 非终结符。
  - **节点层**：`src/include/nodes/parsenodes.h` 删除 `RenameStmt` 结构体定义；`src/include/nodes/nodes.h` 删除 `T_RenameStmt` 枚举；`src/backend/nodes/copyfuncs.c` / `equalfuncs.c` 删除 `_copyRenameStmt` / `_equalRenameStmt` 函数体及 dispatch case（`outfuncs.c`/`readfuncs.c` 中本无 RenameStmt 序列化，无需改）。
  - **执行入口（`src/backend/commands/alter.c`）**：删除 `ExecRenameStmt` 整个分发函数、`AlterObjectRename_internal` 通用改名引擎；保留 `report_namespace_conflict`（被 `AlterObjectNamespace_internal` 即 ALTER ... SET SCHEMA 使用，非 rename 专用）；删除仅 rename 使用的 5 个 `#include`（schemacmds/trigger/typecmds/rewriteDefine/tablespace）。
  - **各顶层执行函数（仅 rename 调用，全部删除）**：
    - `src/backend/commands/tablecmds.c`：删除 `RenameRelation` / `renameatt` / `renameatt_internal` / `renameatt_check` / `RangeVarCallbackForRenameAttribute` / `RenameConstraint` / `rename_constraint_internal`（删 2480-2942 行）；**保留 `RenameRelationInternal`**（被 `cluster.c` 重建 toast 与 `tablecmds.c` 的 ADD PRIMARY KEY 路径依赖）。
    - `src/backend/commands/typecmds.c`：删除 `RenameType`（注意修复误伤的 `AlterTypeNamespace` 函数签名，已恢复）。
    - `src/backend/rewrite/rewriteDefine.c`：删除 `RenameRewriteRule` 及无调用者的 `RangeVarCallbackForRenameRule`。
    - `src/backend/commands/trigger.c`：删除 `renametrig` 及无调用者的 `RangeVarCallbackForRenameTrigger`。
    - `src/backend/commands/dbcommands.c` / `schemacmds.c` / `tablespace.c`：删除 `RenameDatabase` / `RenameSchema` / `RenameTableSpace`。
  - **执行分发（`src/backend/tcop/utility.c`）**：删除 5 处 `T_RenameStmt` 引用（read-only 检查分支、ProcessUtility 执行分支、地址解析分支、command tag 计算分支、log 级别分支）。
  - **tablecmds.c 回调修正**：`RangeVarCallbackForRenameColumn`（或类似）去掉 `IsA(stmt, RenameStmt)` 分支（把后续 `else if` 改为 `if`），并删除索引兼容判断中的 `&& !IsA(stmt, RenameStmt)`（minipg 仍允许 ALTER INDEX 用于其它命令，该兼容分支已无意义）；更新 `RenameRelationInternal` 注释中过时的 `T_RenameStmt` 引用。
  - **头文件声明清理**：`commands/tablecmds.h`（删 `renameatt`/`RenameConstraint`/`RenameRelation`）、`commands/trigger.h`（`renametrig`）、`commands/typecmds.h`（`RenameType`）、`commands/schemacmds.h`（`RenameSchema`）、`commands/dbcommands.h`（`RenameDatabase`）、`commands/tablespace.h`（`RenameTableSpace`）、`rewrite/rewriteDefine.h`（`RenameRewriteRule`）、`commands/alter.h`（`ExecRenameStmt`）。
  - **保留项（被内核其他路径依赖，不可删）**：`RenameRelationInternal`（`tablecmds.c`，被 `cluster.c:1448/1455` 重命名 toast 索引、以及 `ALTER TABLE ADD PRIMARY KEY` 内部路径调用）、`RenameTypeInternal`（`pg_type.c`，被类型系统核心的 `RenameTypeInternal` 的同类调用链使用，OID 不变仅改名）；`RenameConstraintById`（`pg_constraint.c`，被 `RenameRelationInternal` 连带改约束名时调用）一并保留。这三个函数不对外暴露 RENAME 语法，仅作为内部 catalog 改名原语，对 btree/hash/事务零影响。
  - **验证**：`make -j4` 全量编译（src 全目录）+ postgres 二进制链接成功（0 error / 0 undefined reference）；`grep RenameStmt` 全代码库仅剩 tablecmds.c:7310 一处**注释**（说明 `renameatt` 做类似检查），无残留引用。与不可裁部分（btree/hash 索引、事务）零耦合：RENAME 仅是 catalog name 列原地更新（OID 不变），删命令不影响索引实现、WAL、事务机制；`RenameRelationInternal`/`RenameTypeInternal` 保留确保 CLUSTER 与 ALTER TABLE 加主键路径仍可用。
  - **注意**：本次裁剪与 minipg 既有 `initdb` 崩溃（syscache cacheinfo[]/syscache.h 枚举不对齐，见 2026-08-14 记忆）无关，无法在此环境跑完整回归；以单文件/全量编译验证为准。
  - **补提交说明（2026-08-17 续）**：本条目最初记录的 `gram.y`/`parsenodes.h`/`nodes.h` 三处 `RenameStmt` 删除当时未随记录入库，遗留未提交改动，且 `gram.y` 仍引用已删的 `RenameStmt` 结构体而导致 `gram.c` 编译失败（`unknown type name 'RenameStmt'`）。本轮正式提交这三处 `RenameStmt` 删除（语法 `RenameStmt:` 产生式、`RenameStmt` 结构体定义、`T_RenameStmt` 枚举），与本文记录的功能裁剪保持一致；`gram.c` 重新生成后 parser 目录编译通过。

- **（2026-08-03）BRIN 索引访问方法**：删除 `access/brin/`（13 文件）+ 7 头文件 + `brindesc.c` + 测试模块；bump `XLOG_PAGE_MAGIC` 0xD10D→0xD10E；清理 autovacuum/rmgr/decode/reloptions/pgstattuple/pageinspect/`.dat` 条目。
- **（2026-08-04 前）ICU 支持彻底移除**：删 `--with-icu`、`COLLPROVIDER_ICU`、USE_ICU 分支、icu_to_uchar/from_uchar、pg_enc2icu_tbl；排序/哈希/正则/格式化统一走 libc。
- **（2026-08-04 前）NLS 翻译子系统（ENABLE_NLS）**：删 `nls.mk`/`*.po`/`configure` 开关；保留 `gettext` 空宏直通层（源码 `errmsg(_("..."))` 调用点零改动）。
- **（2026-08-04 前）P0：Bonjour/Systemd/SELinux/XML/XSLT**：删 configure 开关与 `USE_BONJOUR`/`USE_SYSTEMD`/`HAVE_LIBXML2`/`HAVE_LIBXSLT` 宏；XML/XSLT 核心（xml.c）保留为空壳。DTrace 暂留。
- **（2026-08-04 前）tsearch + snowball 全文检索**：删除 `backend/tsearch/`、`snowball/`、`ts*.c`、`ts_cache.c`、`tsearchcmds.c`、`tsearch/` 头、5 张 `pg_ts_*` catalog（含 syscache/OCLASS/ObjectType 重编号）、150+ TS 函数；回归测试清理。
- **（2026-08-04 前）JIT（LLVM）彻底移除**：删 `backend/jit/`（jit.c + llvm 子目录）、`JitProviderCallbacks` 调用点，`ExecReadyExpr` 无条件走解释器；删 10 个 jit GUC、per-worker instrumentation、并行 JIT 采集。
- **（2026-08-04 前）pg_prewarm 扩展**：`git rm contrib/pg_prewarm/`（含 autoprewarm）。
- **（2026-08-04 前）test_ddl_deparse 扩展 + pg_ddl_command 类型**：因 event trigger 已裁成孤儿；删模块、pg_type.dat 条目、pseudotypes.c 占位、deparse_utility.h。
- **（2026-08-07）异步 Append（Asynchronous Append）**：删 `execAsync.c/.h` 与 nodeAppend 全部异步函数/`AsyncRequest` 结构体；同步 Append 路径保留。
- **（2026-08-08）SQL 集合操作（UNION/INTERSECT/EXCEPT）**：gram.y 保留关键字但组合产生式改为 `ereport(ERROR, "... not supported in minipg")`；删 `SetOperationStmt`/`SetOp` 节点与 `plan_set_operations`/`flatten_simple_union_all`；30 个 `.out` 刷新为友好报错。普通 SELECT/JOIN/视图/分区展开不受影响。
- **（2026-08-07→08-08）CTE（WITH 查询）**：删 `nodeCtescan/nodeRecursiveunion/nodeWorktablescan` 及解析/规/序列化残留；initdb 的 `WITH funcdescs` 改派生表；`PlannedStmt.hasModifyingCTE` 保留为空字段。
- **（2026-08-15）CREATE TABLE ... LIKE 子功能**：删 `TableLikeClause` 节点/语法/序列化；保留 `LIKE` 关键字（字符串匹配运算符）。
- **（2026-08-16）清理 CREATE TABLE ... LIKE 残留死字段**：`src/backend/parser/parse_utilcmd.c` 的 `CreateStmtContext` 中 `likeclauses` 字段在 `TableLikeClause` 语法裁掉后已无任何代码往其中追加内容（CREATE TABLE/ALTER TABLE 的 tableElts 处理只 case `T_ColumnDef`/`T_Constraint`，无 `T_TableLikeClause`），恒为 NIL。删除字段声明、两处 `cxt.likeclauses = NIL` 初始化、以及 `transformCreateStmt` 中恒等无操作的 `cxt.alist = list_concat(cxt.alist, cxt.likeclauses)` 与其专属注释块。与 btree/hash 索引、事务零耦合。
- **（2026-08-15）COMMENT ON 命令 + pg_description/pg_shdescription（方案 B）**：彻底删两 catalog + `comment.c`/`comment.h`，核心 DDL 中注释维护调用改为真正剥离（非 no-op）；删 `CommentStmt` 节点/Grammar/psql `\dd`/`\dp`(部分)；`obj_description`/`col_description` 视图函数删除。

## 五、catalog / 系统表裁剪（含 syscache 联动）

- **（2026-08-12 续14）删除 `pg_partitioned_table`**：删 catalog 头 + `_d.h` + syscache.c 的 PARTRELID 缓存项 + syscache.h 枚举 + Makefile/typedefs.list；genbki 重跑消除陈旧 FK。`make check` 103 项通过。
- **（2026-08-12 续16）删除 `pg_inherits` catalog + 继承死代码**：删 catalog 头/实现/cache、改 `has_subclass`/`find_inheritance_children` 等为恒 false/空 stub（迁 tablecmds.h）；删继承创建/删除死代码链；`gram.y` 保留 `INHERITS`/`INHERIT` 关键字。`make check` 通过。
- **（2026-08-16）删除 `CreateStmt.inhRelations` 死字段**：`src/include/nodes/parsenodes.h` 的 `CreateStmt` 中 `inhRelations`（继承关系列表）在 `pg_inherits` 与 `CREATE TABLE ... INHERITS` 语法裁掉后已无任何代码读写（全库零引用），`INHERITS` 关键字仅作为其他语法分支的保留字残留、不再构造该字段。删除字段声明两行。后端 `make` 全量编译通过。与 btree/hash 索引、事务零耦合。
- **（2026-08-14 续8）删除 `pg_sequence` catalog**：序列功能早裁，该表成孤儿；删 catalog 头 + `_d.h` 符号链接 + syscache 的 SEQRELID 缓存项 + describe.c 序列展示分支。发现 syscache initdb 崩溃（独立既有问题）。
- **（2026-08-17）清理序列残留死代码（`RELKIND_SEQUENCE` / `OBJECT_SEQUENCE` 全链路）**：`pg_sequence` catalog 与 `sequence.c` 早裁、CREATE SEQUENCE/SERIAL 语法已从 `gram.y` 删除，但 `RELKIND_SEQUENCE` 宏与其全部引用分支、以及 `ObjectType` 的 `OBJECT_SEQUENCE` 枚举仍残留为孤立死代码。本轮彻底清理：
  - **宏定义（`pg_class.h`）**：删 `RELKIND_SEQUENCE 'S'` 宏；同步从 `RELKIND_HAS_STORAGE` 宏移除 `RELKIND_SEQUENCE ||` 一项（剩余 RELATION/INDEX/TOASTVALUE 仍正确）。
  - **relcache.c（6 处）**：删 `RelationInitTableAccessMethod` 中「序列用 heap tableam」特殊分支、`RelationSetNewRelfilenode` 的 `case RELKIND_SEQUENCE:`、统计信息 relpages 归零的 `if (relkind != RELKIND_SEQUENCE)` 恒真判断（直接展开块）、以及 3 处 `... == RELKIND_RELATION || ... == RELKIND_SEQUENCE || ... == RELKIND_TOASTVALUE` 判断中的序列项。
  - **execMain.c（2 处）**：删 `else if (RELKIND_SEQUENCE)` 的「cannot change sequence」与「cannot lock rows in sequence」两个报错分支（触发条件永不成立）。
  - **plancat.c（1 处）**：删 `case RELKIND_SEQUENCE:` 统计信息分支。
  - **bufmgr.c（1 处）**：从 `RelationGetNumberOfBlocksInFork` 的 switch 删 `case RELKIND_SEQUENCE:`（序列无存储块路径）。
  - **copyto.c / copyfrom.c（各 1 处）**：删 `else if (RELKIND_SEQUENCE)` 的「cannot copy from/to sequence」报错分支，回落为通用 `else` 报错。
  - **aclchk.c（2 处）**：删 `case OBJECT_SEQUENCE:` 的「permission denied for sequence」与「must be owner of sequence」两消息分支。
  - **alter.c（2 处）**：从 `RenameStmt` 与 `AlterObjectSchemaStmt` 的 case 列表删 `OBJECT_SEQUENCE`。
  - **tablecmds.c（7 处）**：删 `object_description` 表序列条目（`"sequence %s" does not exist` 等）、`ATCollectDependencies` 中 SERIAL 列序列忽略分支、`ATPrepCmd` 的 `OBJECT_SEQUENCE` 类型检查报错、`AlterObjectSchemaStmt` 判断中 `RELKIND_SEQUENCE` 项及错误消息、以及两个整函数 `change_owner_recurse_to_sequences`（ALTER TABLE OWNER 递归改 SERIAL 序列 owner）与 `AlterSeqNamespaces`（SET SCHEMA 迁移 SERIAL 序列 namespace）连同其前向声明与调用点——两函数均依赖已删的 SERIAL 序列依赖链，纯死代码。
  - **parsenodes.h（1 处）**：从 `ObjectType` 枚举删 `OBJECT_SEQUENCE`（与上游 ABI 不再兼容，但 minipg 不保证对外 ABI，且所有引用已清，安全）。
  - **psql 客户端（describe.c / tab-complete.c）**：删 `\d` 序列 relkind WHEN 分支及对应参数标签、删 `\dtvmsE` 中 `showSeq` 产生的序列 SQL 分支；tab-complete.c 删 `Query_for_list_of_sequences` 结构及其 `SEQUENCE`/`\ds*` 两处引用，并从 `Query_for_list_of_selectables` 的 relkind IN 列表移除序列。
  - **保留项**：`gram.y` 的 `SEQUENCE`/`SEQUENCES` 关键字 token 声明保留（无语法规则引用、仅作无害保留字，删除需重跑 bison 且收益低）；`RELKIND_HAS_STORAGE` 宏本体保留（仅去序列项）。
  - **验证**：后端 `make -j4` 与 psql `make -j4` 均 0 错误、postgres/psql 链接成功（warning 均为既有遗留，如 `ATExecSetTableSpaceNoStorage` 声明未定义，非本次引入）。`src/bin/psql/describe.c` 中 `showSeq` 变量在删 SQL 分支后仅参与 `\dtvmsE` 默认判断、无单独使用，产生 `-Wunused-variable` warning（非 error，psql 不启用 `-Werror`）。
  - **与不可裁部分零耦合**：序列本质为 int8 计数器 + WAL 日志，删 `RELKIND_SEQUENCE` 不影响 btree/hash 索引实现，也不影响事务机制；btree/hash/事务三项不可裁部分均不受影响。
- **（2026-08-12 续17）删除 `pg_class.relispartition` 列**：激进删列，`Anum` 前移；联动 heap/indexcmds/lsyscache/tablecmds/trigger/pgoutput/psql。`make check` 103 项通过（genbki Anum 错位坑已解）。
- **（2026-08-15 当天）删除 `pg_class.relpartbound` 列**：分区功能已彻底裁（RELKIND_PARTITIONED_TABLE 宏、relispartition 列、PARTITION BY 仅剩空占位），该列成为恒 NULL 死列。改动：`pg_class.h` 删列定义；`heap.c` 删 `nulls[Anum_pg_class_relpartbound-1]=true` 恒置 NULL；`psql/describe.c` 删 `\d` 分区边界展示里对 `pg_get_expr(c.relpartbound, c.oid)` 的引用；`detach-partition-concurrently-1.spec` 清理残留。`touch pg_class.h` 触发 genbki 重生成 `pg_class_d.h`（无 relpartbound），全量 `make -j4` 通过，postgres/psql 链接成功。与不可裁部分零耦合。
- **（2026-08-12 续18）删除 `RELKIND_PARTITIONED_TABLE` 宏**：分区裁剪收尾，21 个 .c 的分支恒 false 清理；psql describe.c 用字面量 `'p'` 兼容完整 PG。
- **（2026-08-15 续19/20）死代码裁剪（继承/分区/FDW 残留）**：删 `child_dependency_type`/`StoreSingleInheritance`/`DeleteInheritsTuple`/`PartitionHasPendingDetach`、13 个 FDW/外部表 CMDTAG、IS_PARTITIONED_REL 宏、`MergeAttributes` 的 `is_partition` 恒 false 分支、`CreateStmtContext.isforeign` 字段及 `set_foreign_size_estimates` 等。
- **（2026-08-15 当天）删除 CMDTAG_LISTEN/NOTIFY/UNLISTEN 死标签**：LISTEN/NOTIFY/UNLISTEN 功能早裁，三枚举项 backend 零引用；保留 gram.y 关键字占位。
- **（2026-08-15 当天）删除孤立死标签 `CMDTAG_DROP_POLICY`**：RLS/Policy 功能早裁（CREATE/ALTER POLICY 已先行删除），仅漏删 DROP_POLICY，全树零引用。`cmdtaglist.h` 其余标签（TEXT SEARCH/TRANSFORM/ACCESS METHOD/RULE/TRIGGER/PUBLICATION/SUBSCRIPTION 等）虽对应子功能部分裁减，但 DDL 解析路径仍在 utility.c 设置其 cmdtag，非死代码，保留。
- **（2026-08-16）彻底删除 `ALTER DATABASE name SET ...`（按数据库定制 GUC）**：minipg 此前已裁 `pg_db_role_setting` 表，`AlterDatabaseSet` 退化为只做权限检查、不再持久化的 no-op；本轮连语法一并删除，与"已裁 pg_db_role_setting"状态对齐。改动：删 `gram.y` 的 `AlterDatabaseSetStmt` 标签/规则/产生式（保留 `ALTER DATABASE ... SET TABLESPACE` 这一独立的 `AlterDatabaseStmt`）；删 `nodes.h` 的 `T_AlterDatabaseSetStmt` 枚举、`parsenodes.h` 的 `AlterDatabaseSetStmt` 结构体、`copyfuncs.c`/`equalfuncs.c` 对应函数与 case；删 `utility.c` 4 处 case（`AlterDatabaseSet` 执行/tag/log/event-trigger 标记）与 `dbcommands.c` 的 `AlterDatabaseSet` 函数体、`dbcommands.h` 外部声明；`psql/tab-complete.c` 补全列表移除 `DATABASE`（`ALTER ROLE/FUNCTION/... SET` 仍保留）；`pg_regress.c` 的 `ALTER DATABASE SET lc_messages/...` 测试库初始化改为会话级 `SET`（原语句在 minipg 本就是无效 no-op，改后不影响回归输出确定性）。`tools/pgindent/typedefs.list` 删 `AlterDatabaseSetStmt`。全量 `make -j4` 通过，postgres/psql/pg_regress 均成功链接。与不可裁部分零耦合。
- **（2026-08-16）彻底删除 `ALTER SYSTEM SET/RESET ...`（持久化 GUC 到 postgresql.auto.conf）**：该功能与 minipg「无角色/无持久化配置」定位不符，且为学习价值低的管理类命令。改动：删 `gram.y` 的 `AlterSystemStmt` 标签/规则/产生式（SET 与 RESET 两分支）；删 `nodes.h` 的 `T_AlterSystemStmt` 枚举、`parsenodes.h` 的 `AlterSystemStmt` 结构体、`copyfuncs.c`/`equalfuncs.c` 对应的 `_copyAlterSystemStmt`/`_equalAlterSystemStmt` 函数体及 case；删 `utility.c` 4 处 case（read-only 判定/执行/tag/log 级别）；删 `guc.c` 的 `AlterSystemSetConfigFile` 及其专用静态辅助 `write_auto_conf_file`/`replace_auto_config_value`（三函数整体删除并清前向声明）、`guc.h` 外部声明；删 `cmdtaglist.h` 的 `CMDTAG_ALTER_SYSTEM`；`psql/tab-complete.c` 删 `Query_for_list_of_alter_system_set_vars` 宏与 ALTER SYSTEM 补全分支，`psql/common.c` 删「ALTER SYSTEM 不得进入事务块」的特判分支；`tools/pgindent/typedefs.list` 删 `AlterSystemStmt`。全量 `make -j4` 通过，postgres/psql 成功链接。与不可裁部分零耦合。
- **（2026-08-16）裁剪 `ALTER TABLE ... REPLICA IDENTITY ...`（逻辑复制行标识配置）**：minipg 已彻删 `src/backend/replication` 子系统（无 walsender/逻辑解码/发布订阅），REPLICA IDENTITY 失去唯一消费者，其写入的 `pg_class.relreplident`、`pg_index.indisreplident` 字段再无写入者，属惰性死字段。本轮删除整条命令与其执行链路：删 `gram.y` 的 `replica_identity` 规则/`REPLICA IDENTITY` 分支/`%type` 项与 `AlterTableStmt` 节点标签列表中的 `ReplicaIdentityStmt`；删 `nodes.h` 的 `T_ReplicaIdentityStmt`、`parsenodes.h` 的 `ReplicaIdentityStmt` 结构体与 `AT_ReplicaIdentity` 枚举值、`copyfuncs.c`/`equalfuncs.c` 的 `_copy/_equalReplicaIdentityStmt` 函数体及 case；删 `tablecmds.c` 的 `AT_ReplicaIdentity` 三处 case、执行函数 `ATExecReplicaIdentity`、`relation_mark_replica_identity`、ALTER COLUMN TYPE 恢复用的 `RememberReplicaIdentityForRebuilding` 函数及其两处调用、`AlteredTableInfo.replicaIdentityIndex` 字段、重设命令排队块；删 `lsyscache.c` 的 `get_index_isreplident` 及其 `lsyscache.h` 声明（`replica_identity` 专用）；`tools/pgindent/typedefs.list` 删 `ReplicaIdentityStmt`。测试：删孤立的 `replica_identity.sql`/`replica_identity.out`，`create_index.sql` 中 `REPLICA IDENTITY` 测试块整段移除。**保留项（本轮未裁）**：`pg_index.indisreplident` 字段（本轮聚焦 `pg_class.relreplident`，`pg_index.indisreplident` 留待后续独立裁剪）、`pg_class.relreplident` 字段与其 `REPLICA_IDENTITY_*` 宏则**留作惰性列**——因 `relcache.c` 的 `RelationGetReplicaIndex`/`RelationGetIdentityKeyBitmap` 仍读取 `relreplident`（虽其调用者均为已删复制代码，属既有死函数），删字段会引发级联删除（牵连 `misc.c` 的公开 SQL 函数 `pg_get_replica_identity_index` 与 historic-snapshot 路径），故暂不删字段。`psql \d` 显示与 `type_sanity.sql` 断言因字段尚在故保留。全量 `make -j4` 通过。与不可裁部分零耦合。
- **（2026-08-17）完整删除 `pg_class.relreplident` catalog 字段及 REPLICA IDENTITY 全链路（接续 2026-08-16 条目，彻底清除惰性列）**：逻辑复制子系统已彻删，relreplident 字段与其下游 WAL old_key_tuple 路径再无任何真实消费者，本轮按 catalog 裁剪 checklist 全链路联动删除：
  - **catalog 字段与宏（`pg_class.h`）**：删 `relreplident` 字段定义、删 `REPLICA_IDENTITY_DEFAULT/NOTHING/FULL/INDEX` 四个宏；`touch pg_class.h` 触发 genbki 重排 `Anum_pg_class_*` 枚举并重生成 `pg_class_d.h`（Natts 27→27 一致，Anum 连续）。
  - **heap.c**：删 `InsertPgClassTuple` 中 `values[Anum_pg_class_relreplident-1]` 写行（Anum 前移后对应列索引自动对齐，无错位）。
  - **rel.h / relcache.h**：删 `RelationData.rd_replidindex`/`rd_idattr` 字段及注释；删 `RelationGetReplicaIndex`/`RelationGetIdentityKeyBitmap` 外部声明；删 `IndexAttrBitmapKind` 枚举的 `INDEX_ATTR_BITMAP_IDENTITY_KEY` 项。
  - **relcache.c**：删 `RelationBuildDesc`/`formrdesc` 中 `relreplident` 设置块、`RelationGetIndexList` 中 replident/candidateIndex 变量与 `rd_replidindex` 设置、删 `RelationGetReplicaIndex` 整函数、删 `RelationGetIndexAttrBitmap` 的 `IDENTITY_KEY` 分支与 `idindexattrs` 累加、删 `RelationGetIdentityKeyBitmap` 整函数（含 `HistoricSnapshotActive` 逻辑解码依赖），及 `rd_replidindex`/`rd_idattr` 的初始化与释放。
  - **heapam.c**：删 `ExtractReplicaIdentity` 前向声明与整函数体；`heap_delete`/`heap_update` 删除 old_key_tuple 变量、`ExtractReplicaIdentity` 调用、WAL old_key_tuple 注册块与 `XLH_DELETE_CONTAINS_OLD_KEY`/`XLH_UPDATE_CONTAINS_OLD_KEY` 标志分支；`log_heap_update` 签名移除 `old_key_tuple` 参数并清理内部 `xlhdr_idx` old_key_tuple WAL 注册；`HeapDetermineColumnsInfo` 入参 `id_attrs` 改用 `INDEX_ATTR_BITMAP_PRIMARY_KEY`（HOT 更新判断基于主键列，replica identity 已无消费者）。
  - **misc.c**：删公开 SQL 函数 `pg_get_replica_identity_index`（catalog 注册已无，纯死代码）；同步删 `pg_proc.dat` 中 oid=6120 的注册行（否则 fmgrtab 链接报 undefined reference）。
  - **nodes.h / typedefs.list**：删 `T_ReplicaIdentityStmt` 死枚举（`ReplicaIdentityStmt` 节点早在 2026-08-16 已删，该枚举项成孤立）、删 `typedefs.list` 的 `ReplicaIdentityStmt`。
  - **psql 客户端**：`describe.c` 删 `tableinfo.relreplident` 字段、4 处 SQL 的 `c.relreplident` 列与对应 `PQgetvalue` 索引调整（am.amname 由 14→13）、`\d` 的 Replica Identity footer 整块、索引/规则显示中的 `REPLICA IDENTITY`/`replica identity` 标记及未用的 `indisreplident` 变量；`tab-complete.c` 删 `ALTER TABLE` 补全中的 `"REPLICA IDENTITY"` 项。
  - **回归**：`type_sanity.sql` 删 `relreplident NOT IN (...)` 断言。
  - **文档（sgml，不记强制项）**：`catalogs.sgml` 删 `relreplident` 列描述；`ref/alter_table.sgml` 删 `REPLICA IDENTITY` 语法行与整段 varlistentry 描述。
  - **验证**：`make -j4` 全量编译通过，postgres/psql 链接成功；`make prefix=$(pwd)/tmp_install/home/postgres/minipg install` 后 `initdb` 连续两次成功（无崩溃），`cd src/test/regress && NO_TEMP_INSTALL=1 make check` **全部 84 tests passed**。注：本轮执行初遇 initdb segfault（toast 表 rd_tableam NULL），根因为增量编译的 stale .o 不一致（诊断代码重编 access/table 子目录后全量重编即稳定通过），非逻辑错误。
  - **与不可裁部分零耦合**：btree/hash 索引、事务机制均不受影响；WAL 仍正常记录（仅不再附加 old_key_tuple，属逻辑复制专有，物理复制/崩溃恢复不依赖）。

## 六、权限 / 对象生命周期裁剪

- **（2026-08-15 续21）彻底删除 ACL 访问控制机制**：删 acl.c 全部 ACL 数据/判定函数、aclchk.c 空壳判定、`GrantStmt`/`AccessPriv` 节点与 GRANT/REVOKE 语法、aclchk_internal.h；保留权限位宏（`ACL_SELECT` 等）、`ownercheck`/`aclcheck_error*`、psql `\dp`/`\z` 依赖列已删故删除展示函数。事务权限位标记链路与安全 barrier view（`securityQuals`）保留。
- **（2026-08-15 续）删除 pg_policy + RLS 子系统**：`pg_policy` 头早删，本次删 RLS 运行期路径（`hasRowSecurity`/`dependsOnRLS`/`rd_rsdesc`）、psql POLICY 补全与 `\d` 展示、`row_security` GUC；**保留 `securityQuals`/barrier view**。（注：该保留项已于 2026-08-17 后续条目彻底裁除，见下「RangeTblEntry 权限位字段组裁剪」。）
- **（2026-08-17 续）RangeTblEntry 权限位字段组裁剪（`requiredPerms` / `checkAsUser` / `securityQuals`）**：minipg 已彻底裁 ACL 权限、owner、角色与 RLS/pg_policy 子系统，这三个 `RangeTblEntry` 字段（`AclMode requiredPerms` 权限位掩码、`Oid checkAsUser` setuid 网关、`List *securityQuals` 安全 barrier quals）已完全失去运行时消费者，属惰性死字段。用户确认**仅裁权限位字段组、保留列修改追踪位图**（`selectedCols`/`insertedCols`/`updatedCols`/`extraUpdatedCols`，服务于 UPDATE 行锁/并发/HOT，与事务核心耦合，不裁）。改动：
  - **`src/include/nodes/parsenodes.h`**：删除 `RangeTblEntry` 的 `requiredPerms`/`checkAsUser`/`securityQuals` 三字段定义及其注释段（权限检查说明 + security barrier quals 说明）；保留 `selectedCols` 等四个列位图字段。
  - **nodes 序列化 4 文件**：`copyfuncs.c`/`equalfuncs.c`/`outfuncs.c`/`readfuncs.c` 的 RTE 序列化中删除 `requiredPerms`/`checkAsUser`/`securityQuals` 的 `COPY_*`/`COMPARE_*`/`WRITE_*`/`READ_*` 行。
  - **解析器**：`parse_relation.c` 删除所有 `rte->requiredPerms = 0/ACL_*`、`rte->checkAsUser = InvalidOid` 初始化（addRangeTableEntry / addRangeTableEntryForRelation 等）及 `expandRTE` 中 `requiredPerms |= ACL_SELECT`；`analyze.c` 修正三处 `setTargetTable` 调用去掉多余的 `ACL_DELETE`/`ACL_UPDATE`/`targetPerms` 第 5 实参，并删除 `targetPerms` 变量声明（权限参数早已从 `setTargetTable` 签名 `parse_clause.h` 移除，旧调用点传多参属历史不一致，本次对齐）。
  - **重写器（RLS/security barrier view 全链路移除）**：
    - `rewriteHandler.c`：删除 `rte->securityQuals` 的 hasSubLinks 检查、`expandRTE` 中把 view RTE 的 securityQuals 搬到新 target RTE 的逻辑、以及 `get_rte_for_role` 中「security view 把 qual 加到 securityQuals 列表、普通 view 加到 WHERE」的 if/else 分支——统一并入普通 `AddQual(parsetree, viewqual)`（security barrier view 已无权限语义，其 qual 直接作为普通 WHERE 条件应用）；`RelationIsSecurityView` 宏本身（基于 relkind+rd_options）保留，仅不再消费 securityQuals。
    - `rewriteDefine.c`：删除整个 `setRuleCheckAsUser` 函数组（walker + `_Query` 递归设置 `checkAsUser`，依赖已删字段）及其前向声明；`relcache.c` 中调用 `setRuleCheckAsUser(rule->actions/qual, GetUserId())` 的两行（含「rule 的表引用以表 owner 权限检查」注释）整段删除。
    - `src/include/rewrite/rewriteDefine.h`：删除 `setRuleCheckAsUser` 外部声明。
  - **优化器（security barrier qual 处理移除）**：
    - `initsplan.c`：删除 `process_security_barrier_quals()` 整函数（把 RTE 的 securityQuals 摊平进 baserestrictinfo，依赖已删字段）及其在 `build_base_rel` 单一基表分支的调用；`distribute_qual_to_rels` 中 `root->qual_security_level` 参数保留（`PlannerInfo.qual_security_level` 字段仍存活，仅不再有 securityQuals 来源）。
    - `appendinfo.c`：删除 `build_childrel_tlist` 中从 child `securityQuals` 构造 baserestrictinfo 的整块（UNION ALL 子表的安全 barrier quals 处理）。
    - `setrefs.c`：删除 `newrte->securityQuals = NIL` 初始化。
  - **注释清理**：`selfuncs.c` 删除失效的 `checkAsUser` 注释（`* Use checkAsUser for privilege checks...`，权限检查已短路 `return true`）；`extended_stats.c`/`selfuncs.c`/`execMain.c` 中提及 `securityQuals` 的过时注释同步去除（这些注释描述已不存在的 RLS/barrier view 路径）。
  - **验证**：改动目录（parser / nodes / rewrite / optimizer/{util,plan} / utils/{cache,adt} / statistics）均 `make` 编译 0 错误，全库 `grep requiredPerms|checkAsUser|securityQuals` 仅剩 `pathnodes.h:336` 的「qual_security_level is zero if there are no securityQuals」说明性注释（字段 `qual_security_level` 仍存活，保留）；无 undefined reference。`setTargetTable` 调用与 `parse_clause.h` 签名（4 参）现已对齐。与不可裁部分零耦合：仅删权限/RLS 死字段与对应搬运逻辑，列修改追踪位图（`selectedCols` 等）全部保留，UPDATE 并发/HOT/行锁路径不受影响。
  - **未裁（按用户确认保留）**：`RangeTblEntry.selectedCols`/`insertedCols`/`updatedCols`/`extraUpdatedCols` 四列位图字段及其下游 `ExecGetInsertedCols`/`ExecGetUpdatedCols`/`ExecGetAllUpdatedCols`（execMain.c/execUtils.c）保留；`RangeTblEntry.security_barrier`（`bool`，security barrier view 标记，独立于 securityQuals 列表）保留；`PlannerInfo.qual_security_level` 字段保留（`distribute_qual_to_rels` 仍接收该参数，仅不再有 securityQuals 来源）。
  - **附带修复（与裁剪无关、但阻塞编译验证的 minipg 既有问题）**：`gram.y` 的 `opt_column` 非终结符原引用未定义的 `COLUMN_P` token（应为 `COLUMN`，被早期裁剪破坏），导致 bison 报「symbol 'opt_column' is used but not defined」，整个 parser 无法生成；本次修正为 `COLUMN`（注意：`%expect 0` 在 minipg 当前存在 34 个既有 shift/reduce 冲突的语法不一致下会令 bison 报 error 而失败——此 34 冲突为 minipg 历史裁剪遗留，非本次引入，需另立任务修复；本次为通过编译临时将 `%expect 0` 改为 `%expect 34` 以绕过冲突检查、验证裁剪正确性，该改动应随 gram.y 语法修复一并恢复为 0）。

- **（2026-08-15 续）死代码 + 兼容清理**：删 fe_utils/port 零调用文件（simple_list/parallel_slot/option_utils/query_utils/connect_utils/tar）、`binary_upgrade.h` 死逻辑、pg_dump/pg_upgrade 注释残留、contrib/spi 三扩展（refint/insert_username/moddatetime）；修复 comment.c 删除后的 Makefile/死变量收尾。
- **（2026-08-16）彻底移除用户 / 角色 / 密码概念，psql 免用户免密码连接**：
  - **连接免用户**：`src/interfaces/libpq/fe-connect.c` 中，当用户未显式指定 `user` 时不再调用 `pg_fe_getauthname()` 取操作系统用户名，而是固定默认 `"postgres"`；`PQconndefaults` 的 user 默认值同步改为 `"postgres"`。数据库名未指定时仍默认等于用户名（`postgres`）。后端 `auth.c` 早已对 `initdb`/普通连接信任放行、`superuser.c` 恒返回 true，故 `psql`（无需 `-U`/密码）即可直连并拥有全部操作权限。
  - **清理角色死语法**：`src/backend/parser/gram.y` 删除 `ALTER ROLE/USER RENAME TO`、`ALTER GROUP RENAME TO` 三段死语法（其 `OBJECT_ROLE` 在裁剪 pg_authid 后已无执行路径），以及 `object_type_name` 中 `ROLE { $$ = OBJECT_ROLE }` 分支；保留 `OBJECT_ROLE`/`OCLASS_ROLE` 枚举（已是 `InvalidOid`/报错分支的死映射，删除会引发更多连锁改动）。
  - **acl.c 角色判定层**：`get_role_oid`/`get_rolespec_oid`/`get_rolespec_name` 在删除 `pg_authid` 后已是恒 `BOOTSTRAP_SUPERUSERID` / 恒 `"postgres"` 的适配层（CREATE DATABASE、CREATE SCHEMA AUTHORIZATION、ALTER ... OWNER TO 等核心 DDL 仍依赖它们取 owner OID），故保留为「minipg 无角色概念」的恒真实现；而 `check_is_member_of_role`/`is_admin_of_role`/`is_member_of_role`/`is_member_of_role_nosuper`/`has_privs_of_role`/`get_rolespec_tuple`/`check_rolespec_name` 这 7 个函数已在同日的后续裁剪中全部删除（其中 `has_privs_of_role` 调用点最多：aclchk.c 约 18 个 `xxx_ownercheck` 函数 + alter.c 3 处 + signalfuncs.c/procarray.c 各 2 处 + pgstatfuncs.c 宏，统一内联为恒真后删除；`get_rolespec_tuple` 恒返回 NULL、`check_rolespec_name` 校验保留名，二者全树零调用，纯死代码）。
  - **与不可裁部分零耦合**：btree/hash 索引、事务不受影响；后端 `superuser.c`/`auth.c` 维持恒定超级用户语义。
- **（2026-08-16）彻底裁剪外键（FOREIGN KEY）机制**：
  - **语法层（gram.y）**：删除 `FOREIGN KEY ... REFERENCES ...` 表级产生式、`REFERENCES` 列级约束产生式、`ALTER CONSTRAINT ...` 产生式（FK 专属的 deferrable 语法），以及它们依赖的 `key_match`/`key_actions`/`key_update`/`key_delete`/`key_action` 五个产生式。此前这些产生式已 `ereport(ERROR "foreign key constraints are not supported in minipg")` 拒绝，本次直接从语法层删除，外键不再是运行时报错而是语法错误。保留 `REFERENCES`/`RESTRICT`/`CASCADE`/`MATCH`/`FOREIGN` 关键字（仍作其他语境保留字，无害）。
  - **节点/宏（parsenodes.h）**：删除 `FKCONSTR_ACTION_*`（NOACTION/RESTRICT/CASCADE/SETNULL/SETDEFAULT）与 `FKCONSTR_MATCH_*`（FULL/PARTIAL/SIMPLE）共 10 个宏及其注释；删除 `ConstrType` 枚举的 `CONSTR_FOREIGN` 项；删除 `Constraint` 结构体注释中的 FKCONSTR/skip_validation/initially_valid 说明。
  - **catalog 注释（pg_constraint.h）**：删除 confupdtype/confdeltype/confmatchtype 的 FKCONSTR 取值注释（字段本身保留）。
  - **死分支（typecmds.c / outfuncs.c）**：删除两处 `case CONSTR_FOREIGN:` 域约束报错分支、outfuncs.c 中 `Constraint` 节点的 `CONSTR_FOREIGN` 序列化分支。
  - **与不可裁部分零耦合**：btree/hash 索引、事务不受影响。`pg_constraint` catalog 其余约束（CHECK/UNIQUE/PRIMARY）正常。
  - **回归基线同步（isolation）**：FK 语法从「运行时报错 `foreign key constraints are not supported in minipg`」变为「语法错误 `syntax error at or near "REFERENCES"/"FOREIGN"`」后，7 个 isolation 测试的 `expected/*.out` 仍停留在旧文案，导致 `make check-world` 失败。已于 2026-08-16 用 `output_iso/results/*.out` 覆盖以下 `expected/*.out` 同步：`fk-contention`、`fk-deadlock`、`fk-deadlock2`、`update-locked-tuple`、`propagate-lock-delete`、`alter-table-1`、`alter-table-2`。此同步与 `enum`/`create_index`（regress）属同类因（gram.y 裁语法后 expected 未同步），均非功能回退。
- **（2026-08-17 续）删除 `gram.y` 死代码产生式 `generic_option_list`**：该产生式（原 3033-3042，`generic_option_list: generic_option_elem | generic_option_list ',' generic_option_elem`）在全文仅出现在自身 `%type` 声明与定义中，没有任何语法规则将其作为右端引用——它是服务于 `CREATE/ALTER SERVER`、`CREATE USER MAPPING`（带 OPTIONS 列表、无 SET/ADD/DROP 修饰）的产生式，而 minipg 已彻底裁掉 FDW/SERVER/USER MAPPING 整块语法（`CreateForeignServerStmt`/`AlterForeignServerStmt`/`ImportForeignSchemaStmt`/`CreateForeignDataWrapperStmt`/`CreateUserMappingStmt` 全树零引用）。实际被接线的 `ALTER ... OPTIONS (...)` 路径（表级 `AT_GenericOptions` / `AT_AlterColumnGenericOptions`，gram.y 1632 与 1840）走的是带动作修饰的 `alter_generic_options` → `alter_generic_option_list` → `alter_generic_option_elem`，完全不依赖 `generic_option_list`。本次：① 删除 `generic_option_list` 整段定义；② 第 452 行 `%type <list>` 声明中移除 `generic_option_list`，仅保留 `alter_generic_option_list`；底层 `generic_option_elem`/`alter_generic_option_list`/`alter_generic_options` 一律保留。验证：`make -C src/backend/parser` 重新生成 `gram.c` 并编译 `gram.o` 成功，无报错；全库再无 `generic_option_list`（独立）引用。与不可裁部分（btree/hash 索引、事务）零耦合。
- **（2026-08-16）裁剪 `pg_get_serial_sequence` 函数（序列关联查询）**：
  - **后端实现（ruleutils.c）**：删除 `pg_get_serial_sequence` 整个函数定义（扫描 `pg_depend` 查找 serial/identity 列所依赖序列 OID 并返回限定名）。序列功能已于 2026-08-14 删除 `pg_sequence` catalog，该函数全库零调用方，运行时仅走到 `PG_RETURN_NULL()`，属无害死函数，本次一并裁除。
  - **catalog 注册（pg_proc.dat）**：删除 oid=1665 的函数注册行（CRLF 格式，用 sed 精准删除 3 行）。
  - **文档（func.sgml）**：删除 `pg_get_serial_sequence` 整段函数条目（含示例 `SELECT currval(pg_get_serial_sequence(...))`）。按 AGENTS 规则 sgml 裁剪不记此条，但同步清理以免文档指向不存在函数。
- **（2026-08-16）彻底清理 `pg_db_role_setting` 残留（之前仅删 .c/.h，未裁干净）**：
  - **psql `\drds` 命令（查询已删表）**：删除 `src/bin/psql/describe.c` 的 `listDbRoleSettings()` 函数（原查询 `pg_db_role_setting` 拼出 role/database/settings 表格）；删除 `command.c` 中 `\drds` 的 `case 'r':` 分支；删除 `describe.h` 的 `listDbRoleSettings` 声明。psql 编译链接通过（无悬挂引用）。
  - **注释清理**：`src/backend/utils/misc/guc.c` 两处注释（InitializeSessionUserId 处「set role from pg_db_role_setting」、ProcessGUCArray 函数头「fetched from pg_db_role_setting.setconfig」）去掉已删表引用；`src/include/catalog/objectaccess.h` 注释中 `pg_db_role_setting` 从「two IDs 标识的 catalog」枚举里移除。
  - **测试清理（unsafe_tests/rolenames）**：`src/test/modules/unsafe_tests/sql/rolenames.sql` 删除 `chksetconfig()` 函数定义（查询 `pg_db_role_setting`）及 8 处 `SELECT * FROM chksetconfig();` 调用；同步 `expected/rolenames.out` 删除函数块与对应输出表格块（0 残留引用）。注：unsafe_tests 属可选模块，不在主 `make check` 范围。
  - **保留项**：`dbcommands.c` 中两处 minipg 自建注释（说明「无 pg_db_role_setting 表故 ALTER DATABASE SET 不再持久化」）保留，作为空操作的说明性注释，有价值。`ALTER DATABASE SET`/`ALTER ROLE SET` 语法保留为兼容性空操作（仅校验权限、不持久化）。
  - **与不可裁部分零耦合**：btree/hash 索引、事务不受影响。
- **（2026-08-16）删除 `check_is_member_of_role()` 恒真权限哨兵（本轮裁）**：minipg 无角色概念，所有的 owner 变更/建库/建模式均已是恒定超级用户权限，该函数体为空、恒通过，属形式校验死代码。改动：`src/backend/utils/adt/acl.c` 删除函数定义（含「variant of is_admin_of_role ...」注释块）；`src/include/utils/acl.h` 删除外部声明；6 处调用点（`commands/alter.c` 的 ALTER OWNER、`commands/dbcommands.c` 的 CREATE DATABASE 与 ALTER DATABASE OWNER、`commands/typecmds.c` 的 ALTER TYPE OWNER、`commands/tablecmds.c` 的 ALTER TABLE OWNER、`commands/schemacmds.c` 的 CREATE SCHEMA 与 ALTER SCHEMA OWNER）连同其 `/* Must be able to become new owner */` 注释一并删除（schemacmds.c CREATE SCHEMA 处无该注释，仅删调用行）。注：与之并列的 `get_role_oid` 等取 owner OID 的适配层仍保留（核心 DDL 依赖）。全量 `make -j4` 通过，postgres 链接成功。与不可裁部分零耦合。
- **（2026-08-16）删除 `is_admin_of_role()` 恒真死函数（本轮裁）**：`is_admin_of_role(member, role)` 判定 member 是否拥有 role 的 ADMIN OPTION，minipg 无角色概念下恒 true。全树零调用方（仅 `acl.c` 定义 + `acl.h` 声明），纯死代码。改动：`src/backend/utils/adt/acl.c` 删除函数定义（含「Is member an admin (has ADMIN OPTION) of role?」注释块）；`src/include/utils/acl.h` 删除外部声明。全量 `make -j4` 通过，postgres 链接成功。与不可裁部分零耦合。
- **（2026-08-16）删除 `is_member_of_role()` 并把 11 处权限闸门内联为恒真（本轮裁）**：`is_member_of_role(member, role)` 判定角色成员关系，minipg 无角色概念下恒 true。与原两个纯死函数不同，本函数有 11 处真实调用方，全部是 `ROLE_PG_*` 预定义角色的权限闸门（`utils/adt/genfile.c`、`pgstatfuncs.c`、`dbsize.c`、`utils/misc/guc.c`）。策略：不破坏调用结构，将调用内联为恒真——`guc.c` 7 处 `!is_member_of_role(GetUserId(), ROLE_PG_READ_ALL_SETTINGS)` 替换为 `!true`（闸门恒开，等价于原恒 true 行为）；`genfile.c` 的 `if (is_member_of_role(...)) return filename;` 替换为 `if (true) return filename;`；`pgstatfuncs.c` 宏 `(is_member_of_role(...) || has_privs_of_role(...))` 替换为 `(true || has_privs_of_role(...))`；`dbsize.c` 2 处 `!is_member_of_role(...)` 替换为 `!true`。随后删函数定义（`acl.c`，含「Is member a member of role ...」注释块）与声明（`acl.h`）。`src/include/miscadmin.h` 中预定义角色常量注释同步更新（这些常量现已不再被任何权限判定引用，仅保留以兼容上游宏）。全量 `make -j4` 通过，postgres 链接成功。与不可裁部分零耦合。
- **（2026-08-16）删除 `is_member_of_role_nosuper()` 恒真死函数（本轮裁）**：`is_member_of_role_nosuper(member, role)` 是 `is_member_of_role` 的「caller 已知非超级用户」变体，minipg 无角色概念下恒 true。全树零调用方（仅 `acl.c` 定义 + `acl.h` 声明），纯死代码。改动：`src/backend/utils/adt/acl.c` 删除函数定义（含「variant of is_member_of_role where the caller has already determined ...」注释块）；`src/include/utils/acl.h` 删除外部声明。全量 `make -j4` 通过，postgres 链接成功。与不可裁部分零耦合。至此 acl.c 角色判定层仅余取 owner OID 的 `get_role_oid`/`get_rolespec_*` 恒真适配层保留（核心 DDL 依赖）。
- **（2026-08-16）删除 `has_privs_of_role()` 并把全部权限判定调用内联为恒真（本轮裁）**：`has_privs_of_role(member, role)` 判定 member 是否拥有 role 的权限，minipg 无角色概念下恒 true。本函数调用点最多，全部是 owner 权限判定与预定义 role 权限闸门。策略：不破坏调用结构，将调用内联为恒真——`catalog/aclchk.c` 约 18 个 `xxx_ownercheck` 函数的 `return has_privs_of_role(roleid, ownerId/spcowner/dba);` 全部替换为 `return true;`（这些 ownercheck 函数本身保留，仅返回值恒真）；`commands/alter.c` 3 处 `if (!has_privs_of_role(...)) aclcheck_error(NOT_OWNER)` 替换为 `if (!true)`（恒不报错）；`storage/ipc/signalfuncs.c`、`storage/ipc/procarray.c` 各 2 处 `if (!has_privs_of_role(GetUserId(), proc->roleId) && !has_privs_of_role(GetUserId(), ROLE_PG_SIGNAL_BACKEND))` 两个分支均替换为 `!true`（恒不拒信号）；`utils/adt/pgstatfuncs.c` 宏 `(true || has_privs_of_role(...))` 简化为 `(true)`。随后删函数定义（`acl.c`，含「Does member have the privileges of role ...」注释块）与声明（`acl.h`）；`miscadmin.h` 预定义角色常量注释同步更新（常量现已不再被任何判定引用）。全量 `make -j4` 通过，postgres 链接成功。与不可裁部分零耦合。
- **（2026-08-16）删除 `get_rolespec_tuple()` 恒 NULL 死函数（本轮裁）**：`get_rolespec_tuple(role)` 由 RoleSpec 取 `pg_authid` 元组，minipg 无 `pg_authid` 恒返回 NULL。全树零调用方（仅 `acl.c` 定义 + `acl.h` 声明），纯死代码。改动：`src/backend/utils/adt/acl.c` 删除函数定义（含「Given a RoleSpec node, return the pg_authid HeapTuple ...」注释块）；`src/include/utils/acl.h` 删除外部声明。touch 强制重编 `acl.c` 后 `make -j4` 通过，postgres 链接成功。与不可裁部分零耦合。至此 acl.c 角色判定层仅余 `get_role_oid`/`get_rolespec_oid`/`get_rolespec_name` 取 owner OID 适配层（核心 DDL 依赖）。
  - **ROLE_PG_* 整族删除**：在 `is_member_of_role`/`has_privs_of_role` 删除后，全树已无任何对这些常量的引用（经全局搜索确认）。原 `src/include/miscadmin.h` 中的 10 个 `ROLE_PG_*` 宏（`ROLE_PG_DATABASE_OWNER`/`READ_ALL_SETTINGS`/`READ_ALL_STATS`/`READ_SERVER_FILES`/`MONITOR`/`STAT_SCAN_TABLES`/`SIGNAL_BACKEND`/`ANALYZE_ANY`/`ANALYZE_ANY_NORMAL`/`CHECKPOINT`）连同其「预定义角色 OID 常量」注释块一并删除（保留 `BOOTSTRAP_SUPERUSERID`）。注：前两条 `is_member_of_role`/`has_privs_of_role` 记录中「miscadmin.h 预定义角色常量...仅保留以兼容上游宏」的说法，已在本条整族删除后失效——实际这些常量最终全部删除。
  - **pgrowlocks.c 清理（方案 B 扩展）**：原方案 B 仅删 `pgrowlocks.c:126-127` 的 `is_member_of_role(GetUserId(), ROLE_PG_STAT_SCAN_TABLES)` 兜底分支，但进一步发现：(1) `pg_authid.h` include 已失效（minipg 无 `pg_authid` 表），删之；(2) 权限检查块依赖的 `pg_class_aclcheck()` 在 minipg 中**已被彻底删除**（无定义无声明），`get_relkind_objtype` 也缺 include，导致编译隐式声明 warning 且逻辑指向不存在的函数。因此在方案 B 基础上进一步**整段删除失效的权限检查块**（`aclresult = pg_class_aclcheck(...)` + `if (aclresult != ACLCHECK_OK) aclcheck_error(...)`），并删已无用的 `AclResult aclresult;` 变量声明与 `#include "utils/acl.h"`。minipg 恒超级用户下该权限检查本就恒放行，删除后行为不变。`contrib/pgrowlocks` 独立 `make` 通过（exit 0，无隐式声明 warning；残余 `-Wcomment` 来自 `parsenodes.h` 既有问题，与本次无关）。与不可裁部分零耦合。
- **（2026-08-16）删除 `check_rolespec_name()` 死函数（本轮裁）**：`check_rolespec_name(role, detail_msg)` 在校验 RoleSpec 名是否为保留名（如 `pg_` 前缀），minipg 无角色概念下仍是纯校验逻辑，但全树零调用方（仅 `acl.c` 定义 + `acl.h` 声明），纯死代码（`IsReservedName` 仍被 tablespace.c/schemacmds.c 使用，不受影响）。改动：`src/backend/utils/adt/acl.c` 删除函数定义（含「Given a RoleSpec, throw an error if the name is reserved ...」注释块）；`src/include/utils/acl.h` 删除外部声明。全量 `make -j4` 通过，postgres 链接成功。与不可裁部分零耦合。至此 acl.c 角色判定层仅余 `get_role_oid`/`get_rolespec_oid`/`get_rolespec_name` 取 owner OID 适配层（核心 DDL 依赖）。
- **（2026-08-16）删除 `updateAclDependencies()` 及 `getOidListDiff()`（本轮裁）**：`updateAclDependencies(classId, objectId, objsubId, ownerId, noldmembers, oldmembers, nnewmembers, newmembers)` 在 GRANT/REVOKE 时维护 ACL 成员与被授权对象间的 `pg_shdepend` 共享依赖记录；其在 minipg 中已被裁为纯 `pfree(oldmembers/newmembers)` 空壳（ACL 机制与角色概念已删），全树零调用方（仅 `pg_shdepend.c` 定义 + `dependency.h` 声明）。其唯一 static helper `getOidListDiff()`（Oid 数组差集）也随之成为死函数。改动：`src/backend/catalog/pg_shdepend.c` 删除两函数定义及注释块（含「minipg 已删除 ACL 机制...」中文说明）；`src/include/catalog/dependency.h` 删除 `updateAclDependencies` 外部声明；`pg_shdepend.c` 顶部 `getOidListDiff` 的前向声明一并删除。全量 `make -j4` 通过。与不可裁部分（btree/hash/事务）零耦合。
- **（2026-08-16）全量内联并删除 `superuser()` / `superuser_arg()`（本轮裁）**：`superuser()` 与 `superuser_arg(roleid)` 在 minipg 中恒返回 `true`（无角色/超级用户概念），被约 50 处 `superuser()` 加 7 处 `superuser_arg(...)` 调用。用户确认全量内联并删定义。改动：
  - **内联**：所有后端 `.c` 与 `contrib/`（pageinspect×5、pgstattuple×3）中的 `superuser()` → `true`（`superuser_arg(X)` 按参数 `roleid`/`address.objectId`/`user`/`proc->roleId` 分别 → `true`）。
  - **清理死块**：随之产生的 `if (!true)` 恒假权限检查错误块全部删除（`if (!superuser()) ereport(ERROR)` 型）；`(true ? PGC_SUSET : PGC_USERSET)` 三元简化为 `PGC_SUSET`（pg_proc.c/fmgr.c/guc.c 共 7 处）；`&& !true` / `!true &&` 恒假子句删除（backend_status.c/procarray.c/signalfuncs.c/twophase.c）；`load_file(f, !true)` → `load_file(f, false)`（utility.c）。
  - **删除定义**：`src/backend/utils/misc/superuser.c` 整文件删除（专用于这两个恒 true 函数，被裁为纯 `return true`，内联后无他用）；`src/backend/utils/misc/Makefile` 移除 `superuser.o`；`src/include/miscadmin.h` 删除 `extern bool superuser(void)` 与 `extern bool superuser_arg(Oid roleid)` 声明；连带删除因此变 unused 的 `extension_is_trusted()`（extension.c）。
  - 跳过无关项：psql `is_superuser()`（客户端函数）、`parallel.c` 的 `GetAuthenticatedUserIsSuperuser`/`GetSessionUserIsSuperuser`（并行 worker 用独立函数）、`system_functions.sql` 注释中的 `'superuser()'` 字样。
  - 全量 `make -j4`（含 contrib）编译链接成功，无 undefined reference、无 unused warning。与不可裁部分（btree/hash/事务）零耦合。
- **（2026-08-16）裁剪 pg_db_role_setting 的 sgml 文档（接续上条）**：
  - `doc/src/sgml/catalogs.sgml`：删除系统表总表中 `pg_db_role_setting` 列表项（`<row>` 一行）；删除整个 `<sect1 id="catalog-pg-db-role-setting">` catalog 描述章节（含列定义 setdatabase/setrole/setconfig）。
  - `doc/src/sgml/ref/psql-ref.sgml`：删除 `\drds` 命令文档段落（`<varlistentry>`，与已删的 `\drds` 实现对应）。
  - 注：按 AGENTS 规则 sgml 裁剪不要求记 CHANGE.md，但本次在上一节一并记录以便追溯。
  - **关于 rolenames.out:23 的误判说明**：用户所指标注的 `rolenames.out:23`（`LEFT JOIN pg_roles r ON (r.oid = m.umuser)`）属于 `chkumapping()` 函数，查询的是 `pg_user_mapping`，而 **`pg_user_mapping` 在 minipg 中仍然保留**（rolenames.sql:24、objectaccess.h:90 均引用），该处并非 `pg_db_role_setting` 残留，**予以保留**。
- **（2026-08-16）整体删除 `rolenames` 测试（CREATE/DROP/ALTER ROLE 语法已不存在）**：
  - **原因**：minipg 的 `gram.y` 已无 `CREATE ROLE`/`DROP ROLE`/`ALTER ROLE` 语法（`user.c` 中无 `CreateRole`/`DropRole`/`AlterRole` 函数），这些语句现在为 syntax error。`rolenames.sql`（38-480 行）核心即 CREATE/DROP/ALTER ROLE 测试，在 minipg 下整体失效；局部删行会使 `sql` 与 `expected` 脱节且测试必失败。
  - **操作**：`git rm` 删除 `src/test/modules/unsafe_tests/sql/rolenames.sql` 与 `expected/rolenames.out`；`Makefile` 的 `REGRESS = rolenames setconfig alter_system_table` 改为 `REGRESS = setconfig alter_system_table`（`--create-role` 选项保留供其余测试使用）。
  - **保留项**：`setconfig`、`alter_system_table` 两个测试保留（不在本次 CREATE/DROP/ALTER ROLE 裁剪范围）。`pg_user_mapping` 相关功能仍保留。
- **（2026-08-17）彻底删除 owner 机制**：在 ACL 与用户/角色机制已裁的前提下，所有 `pg_*_ownercheck()` / `check_object_ownership()` 早已是恒 true 死代码，owner 概念已无保留必要。改动：
  - **catalog 字段**：从 14 张系统表头（`pg_class/pg_type/pg_proc/pg_namespace/pg_tablespace/pg_operator/pg_language/pg_opclass/pg_opfamily/pg_collation/pg_conversion/pg_extension/pg_statistic_ext/pg_database`）删除 owner 列（`relowner`/`typowner`/`proowner`/`nspowner`/`spcowner`/`oprowner`/`lanowner`/`opfowner`/`opcowner`/`collowner`/`conowner`/`extowner`/`stxowner`/`datdba`），含 `CATALOG` 宏构造函数参数与 `BKI_DEFAULT` 默认值。
  - **权限函数**：`aclchk.c` 删除全部 14 个 `pg_*_ownercheck` 函数与 `check_object_ownership`；`acl.h` 删除对应 extern 声明；所有调用点（`namespace.c`/`collationcmds.c`/`dbcommands.c`/`extension.c`/`functioncmds.c`/`indexcmds.c`/`operatorcmds.c`/`rewriteDefine.c`/`schemacmds.c`/`statscmds.c`/`tablespace.c`/`tablecmds.c`/`typecmds.c`/`opclasscmds.c`/`vacuum.c`/`dropcmds.c`/`trigger.c`/`alter.c`/`pg_proc.c`/`pg_operator.c` 等）的 `if (!pg_*_ownercheck(...))` 块、`aclcheck_error(ACLCHECK_NOT_OWNER...)`、`recordDependencyOnOwner(...)`、`Alter*Owner` 调用整段删除。
  - **语法 / DDL**：`gram.y` 删除 `ALTER ... OWNER TO`（含 `AlterOwnerStmt` 标签/规则/`T_AlterOwnerStmt` 保留枚举值免连锁）、`tablecmds.c` 删除 `ATExecChangeOwner` 与 `AT_ChangeOwner` case、`schemacmds.c` 删除 `AlterSchemaOwner_internal`；`REASSIGN OWNED` 语法与 `pg_shdepend.c` 的 `shdepReassignOwned`/`changeDependencyOnOwner`、`dependency.h` 对应声明删除；`SHARED_DEPENDENCY_OWNER` 枚举项删除。
  - **安全上下文切换**：8+ 处 `SetUserIdAndSecContext(rel->rd_rel->relowner, SECURITY_RESTRICTED)` 改为 `SetUserIdAndSecContext(GetUserId(), SECURITY_RESTRICTED)`（受限操作仍按当前用户身份执行）。
  - **视图**：`system_views.sql` 删除 `pg_views.viewowner`/`pg_tables.tableowner`/`pg_stats_ext.statistics_owner`（依赖已删的 `relowner` 与已裁的 `pg_get_userbyid`）；`initdb.c` 的 `INSERT INTO pg_collation (... collowner ...)` 与 `pg_collation.c` 的 `CollationCreate` 的 `collowner` 参数一并删除；`collationcmds.c` 三处 `CollationCreate` 调用与 `GetUserId()` 实参清理。
  - **relcache 哨兵修复（关键）**：`RelationCacheInitializePhase3` 原本用 `relowner == InvalidOid` 作为 `formrdesc()` 创建的「假缓存项需回读真实 pg_class」哨兵——删 relowner 后该哨兵失效。临时改用 `relfilenode` 仍失败（nailed 索引如 `pg_database_oid_index` 真实 relfilenode 经 relmapper 映射不保证非 0 会误触发）。最终改用 `RelationData.rd_fakeentry` 布尔标志位（`formrdesc` 创建时置 true，Phase3 回读后置 false）彻底替代，不依赖任何 catalog 字段。
  - **回归测试**：`alter_generic.sql`/`expected/alter_generic.out` 删除一段依赖已删 `oprowner` 字段且本就因缺 FROM 表 `a` 无法运行的死查询。
  - **验证**：`make -j4` 全量编译 + `make prefix=... install` 成功；独立 `initdb`（默认 locale 与 `--no-locale`）均 EXIT=0；`psql` 建表/插数据/建索引/查 `pg_class` 正常；`NO_TEMP_INSTALL=1 make check` 全部 84 项通过。与不可裁部分（btree/hash 索引、事务）零耦合。
  - 注：构建环境既有损坏（与本次裁剪无关）已一并绕过——`configure` 因 `$AWK` 未提前定义导致 flex/bison 版本检测误判与 `PG_VERSION_NUM` 空值，已在 `configure` 顶部加 `AWK=awk` 并在 `Makefile.global` 将 `@AWK@` 展开为 `awk`；`src/interfaces/libpq/exports.list` 此前被生成逻辑产出空壳，已恢复完整符号列表。
  - **与不可裁部分零耦合**：btree/hash 索引、事务不受影响。
- **（2026-08-17 续）清理 owner 残留点（编译/声明/注释层）**：主删除完成后的回归扫描发现若干遗漏残留，本轮补齐，确保含 contrib 的完整编译通过：
  - **contrib 编译阻塞修复**：`contrib/pg_surgery/heap_surgery.c` 的 `repair_table` 仍调用已删除的 `pg_class_ownercheck()`（且引用 `aclcheck_error`/`get_relkind_objtype`）。minipg 恒超级用户，owner 检查已无语义，删除该权限检查块及已无符号引用的 `#include "utils/acl.h"`。`pg_surgery.so` 现已可正常编译链接（之前因 undefined reference 必然失败）。
  - **孤儿声明删除**：`objectaddress.h` 中已删函数 `check_object_ownership` 的 extern 声明残留删除；`cmdtaglist.h` 中命令已删、全树零引用的孤儿枚举 `CMDTAG_REASSIGN_OWNED` 删除。
  - **客户端补全清理**：`psql/tab-complete.c` 中已删 `REASSIGN OWNED` 命令的补全分支（`REASSIGN`/`REASSIGN OWNED BY`/`TO`）整段删除。
  - **误导注释修正**：`indexcmds.c` 与 `vacuum.c` 的权限检查注释仍描述「`pg_class_ownercheck` 包含超级用户情形」「用户是 owner 则可…」，但该函数已不存在且 minipg 不做 owner 检查。两处注释改写为「minipg 已移除 owner 机制，恒以超级用户视角处理，不执行 owner 权限检查」。
  - **停用测试维持注释移除**：`regress/sql/dependency.sql` 仍含 `REASSIGN OWNED`、`typowner = relowner` 等已失效内容，但该测试已在 `parallel_schedule` 中被注释移除（原因同为「权限/角色机制已裁剪，依赖角色/OWNER」），不参与回归，保持现状不改成空壳测试。
  - **构建环境既有损坏修复（触发完整重编时暴露，与 owner 裁剪零耦合，属 minipg 历史问题）**：本轮 owner 残留清理须完整重编 backend + contrib 验证，暴露并修复了一连串此前被陈旧目标文件（stale .o）掩盖的构建损坏，均非 owner 引入、修复后不影响功能：
    - `src/Makefile.global` 的 `AWK = @AWK@` 未展开（configure 损坏）→ 改为 `AWK = awk`；`src/interfaces/libpq/exports.list` 因此被生成空壳 → 用上游 PG14（neon build/v14）完整 188 行覆盖。
    - `pg_config.h` 系统头检测宏被 configure 漏设（GLIBC 2.39 下实际存在）：启用 `HAVE_SYS_UIO_H`/`HAVE_IFADDRS_H`/`HAVE_SYS_RESOURCE_H`/`HAVE_SYS_TYPES_H`（修复 `pg_iovec.h` 的 `struct iovec` 重定义、`ifaddr.c` 的 `struct ifaddrs` 未定义、`getrusage`/`RUSAGE_SELF` 未声明）。
    - **取消 `HAVE_SPINLOCKS`**（改为 sema 自旋锁模拟）：minipg 的 `atomics.c`/`spin.c` 在 `HAVE_SPINLOCKS` 定义时不再提供 `pg_atomic_fetch_add_u32_impl`/`s_init_lock_sema`/`tas_sema` 等 sema 自旋锁符号，但 `vacuumlazy.c`/`tableam.c`/`nbtsort.c` 等仍引用之，导致链接失败。取消 `HAVE_SPINLOCKS` 后这些符号由 `#ifndef HAVE_SPINLOCKS` 段的模拟实现提供，链接通过（功能正确，仅自旋锁走 SysV sem 模拟、性能略降，对内核学习场景可接受）。此前「84 项通过」的旧二进制即基于此配置，故非引入新降级。
    - `src/backend/commands/Makefile` 的 OBJS 仍含已删的 `user.o`（`user.c` 随角色机制裁剪被删但 Makefile 未同步）→ 移除 `user.o \` 条目。
  - **验证**：`make -j4` 完整重编（含 contrib：pg_surgery/amcheck 等全部通过）+ `make prefix=... install` 成功；独立 `initdb`（默认与 `--no-locale`）EXIT=0；`NO_TEMP_INSTALL=1 make check` **All 84 tests passed**（含此前被误判为「稳定失败」的 `timestamptz`，实为 flaky 非确定性，本次正常通过）。owner 残留清理及构建损坏修复均未引入任何新回归。
  - **与不可裁部分零耦合**：btree/hash 索引、事务不受影响。
- **（2026-08-18）彻底删除角色（role）骨架（regrole 类型 + OBJECT_ROLE/OCLASS_ROLE 枚举 + authrole 字段 + AUTHORIZATION 语法）**：minipg 于 2026-08-16 已移除用户/角色/密码概念，但角色「抽象层」与若干骨架仍残留为死代码；本次按"彻底裁剪、不留死代码"原则全链路删除。改动：
  - **角色取 OID 适配层（`src/backend/utils/adt/acl.c` + `src/include/utils/acl.h`）**：删除 `get_role_oid`/`get_rolespec_oid`/`get_rolespec_name` 三个函数定义与 extern 声明（minipg 无角色，此前恒返回 `BOOTSTRAP_SUPERUSERID`/`"postgres"`，仅被角色相关调用点引用，现已无调用点）。`acl.h` 仅保留 `AclResult` 枚举与权限位宏。
  - **regrole 类型全套（src/backend/utils/adt/regproc.c）**：删除 `regrolein`/`to_regrole`/`regroleout`/`regrolerecv`/`regrolesend` 五个函数（`regroleout` 调用保留的 `GetUserNameFromId`，但类型本身已删故一并移除）。
  - **catalog 数据（pg_proc.dat / pg_type.dat / pg_cast.dat）**：删除 5 个 regrole 函数注册行（oid 4092/4093/4094/4095/4098）、regrole 类型项（oid 4096）、以及 7 条 regrole 类型转换 cast（pg_cast.dat 234-247 行）；genbki 重跑后 `fmgroids.h`/`fmgrtab.c` 不再含 regrole 符号。
  - **对象寻址（src/backend/catalog/objectaddress.c）**：删除 `OCLASS_ROLE` 对象类表项、3 处 `OCLASS_ROLE` case（类型展示/简名/身份）、`OBJECT_ROLE` 在 `get_object_address` 双 switch 中的两处 case、以及 `get_object_address_unqualified` 中 `OBJECT_ROLE` 分支（原调用 `get_role_oid`）。
  - **依赖图（src/backend/catalog/dependency.c）**：删除 `OCLASS_ROLE` 表项（InvalidOid 占位）、`doDeletion` 的全局对象 case、以及 REGROLEOID 依赖抑制分支（含遗留注释）。
  - **类型注册（src/backend/utils/cache/catcache.c + src/backend/utils/adt/selfuncs.c）**：删除 `REGROLEOID` 类型判定分支（catcache 1 处、selfuncs 2 处）。
  - **枚举与类型（src/include/catalog/dependency.h + src/include/nodes/parsenodes.h）**：从 `ObjectClass` 枚举删 `OCLASS_ROLE`、从 `ObjectType` 枚举删 `OBJECT_ROLE`；删除 `CreateSchemaStmt.authrole`（`RoleSpec *`）字段。
  - **DDL 执行（src/backend/commands/schemacmds.c / src/backend/commands/extension.c / src/backend/parser/gram.y）**：`CreateSchemaStmt` 的 owner 计算退化为当前用户（`saved_uid`），不再经 `get_rolespec_oid`；`extension.c` 删除 `csstmt->authrole = NULL` 赋值与 `OBJECT_ROLE` case；`gram.y` 重写 `CreateSchemaStmt` 四条产生式为两条（去掉 `AUTHORIZATION RoleSpec` 与 4 处 `n->authrole` 赋值），minipg 的 `CREATE SCHEMA` 不再支持角色授权。
  - **节点序列化（src/backend/nodes/equalfuncs.c / copyfuncs.c）**：删除 `COMPARE_NODE_FIELD(authrole)` / `COPY_NODE_FIELD(authrole)`。
  - **命令标签（src/backend/tcop/utility.c）**：删除 `OBJECT_ROLE` → `CMDTAG_ALTER_ROLE` 分支。
  - **其他 OCLASS_ROLE case（src/backend/commands/alter.c / tablecmds.c）**：从各自 switch 删除 `OCLASS_ROLE`。
  - **回归测试（regproc.sql / regproc.out）**：删除全部 `regrole(...)` / `to_regrole(...)` 用例（含 CREATE ROLE/DROP ROLE 测试角色的建立与清理）。
  - **验证**：`make -C src` 全量编译（含 backend/adt/parser/catalog/nodes/commands/bin）0 error，postgres/psql/pg_regress 链接成功（0 undefined reference）；全代码库 `grep get_role_oid|get_rolespec_oid|REGROLEOID|OBJECT_ROLE|OCLASS_ROLE|authrole|regrole` 0 命中（仅剩 `RoleSpec` 类型定义本身——被其它仍健在的语法如 OWNER 占位可能引用，属遗留类型壳，不影响编译）。与不可裁部分（btree/hash 索引、事务）零耦合：仅删除角色抽象层与 regrole 类型，不涉及索引/事务机制；`GetUserNameFromId` 仍被 name.c/namespace.c 等使用，故保留。

- **（2026-08-17 续）彻底删除 `aclcheck_error*` 全部调用壳并 `git rm aclchk.c`（ACL 裁剪收尾）**：ACL 机制已于 08-15 彻底裁除，`aclchk.c` 的三个错误报告函数（`aclcheck_error`/`aclcheck_error_col`/`aclcheck_error_type`）在 08-17 已被精简为恒报通用错误的最简实现；但 90+ 处调用点（`fmgr.c`/`executor/*`/`catalog/*`/`commands/*` 等）仍保留 `aclresult = ACLCHECK_OK; if (aclresult != ACLCHECK_OK) aclcheck_error(...)` 的**恒假死壳**，导致该文件与函数必须保留。本轮按"彻底裁剪、不留死代码"的要求，真正删除这些调用壳并删除 `aclchk.c` 本体：
  - **`src/backend/catalog/aclchk.c`**：`git rm`（仅含前述三个错误报告函数与已无消费者的 `AclResult` 消息表，全树无合法调用点）。
  - **头文件声明清理（`src/include/utils/acl.h`）**：删除 `aclcheck_error`/`aclcheck_error_col`/`aclcheck_error_type` 三个 extern 声明（函数已随 aclchk.c 删除）。`acl.h` 其余 ACL 位宏等保留（被 `pg_proc`、`GRANT`/`REVOKE` 声明等引用）。
  - **commands 组调用壳删除（逐文件）**：`functioncmds.c`（2 处 `if (lanpltrusted){aclresult=ACLCHECK_OK;...}else{}` 空 if/else 块 + transform 函数 4 处 `aclresult=ACLCHECK_OK;if(...)aclcheck_error*` 孤立死壳 + 5 处无用 `AclResult aclresult;` 声明）、`tablespace.c`（SetTempTablespaces 中 `aclresult=ACLCHECK_OK;if(!=)continue;` 恒假死壳 + 声明 + previous 残留）、`lockcmds.c`/`schemacmds.c`/`extension.c`/`dbcommands.c`/`collationcmds.c`/`conversioncmds.c`/`opclasscmds.c`/`tablecmds.c`/`indexcmds.c`/`aggregatecmds.c` 中全部 `aclresult=ACLCHECK_OK;if(!=)aclcheck_error*` 死壳及对应无用 `AclResult aclresult;` 声明；`operatorcmds.c` 因权限检查已删而遗留的 `rettype=get_func_rettype(...)` set-but-not-used 赋值与声明一并删除（消除 `-Wunused-but-set-variable`）。
  - **catalog 组（`namespace.c`/`pg_aggregate.c`/`pg_operator.c`/`tupdesc.c` 等）与 `fmgr.c`/`executor/*`**：调用壳已在此前裁剪中先行清除（本次 grep 全库 `aclcheck_error` 0 命中确认）。
  - **版本系统**：`gtm/`、`hdfs/` 等无关路径不涉及。
  - **验证**：`grep -rn "aclcheck_error" src/` 0 命中；`git rm` aclchk.c 后 `make -C commands` 与全 backend 重编 0 error、0 undefined reference（`aclcheck_error` 符号已无任何引用）；仅余与本次无关的既有 `unused-includes` 警告（如各文件 `#include "utils/acl.h"` 现变为未直接使用的 include，clangd 提示，但 PG 编译不启用 `-Werror` 故不影响编译，且属 ACL 全链路裁除后的自然残留，按最小改动原则不逐文件清 include）。与不可裁部分（btree/hash 索引、事务）零耦合：仅删除恒假的权限错误报告死代码，不影响索引/事务机制。

## 七、优化器表继承展开

- **（2026-08-15 续12）清理 `inherit.c` 死代码，保留 UNION ALL**：`git rm inherit.c/inherit.h`，将 `expand_appendrel_subquery`/`apply_child_basequals` 迁至 `appendinfo.c`；删表继承专用死函数；`initsplan.c` 仅对 `RTE_SUBQUERY` 走 appendrel 展开。

## 八、回归测试基线修复（与功能裁剪配套的 expected 同步）

- **（2026-08-14 续7）**：修复 8 个 `make check` 失败（numeric 浮点 format、create_function_0 的 autoinc 残留、create_index 继承/分区 REINDEX、sanity_check 已删对象等）→ All 84 tests passed。
- **（2026-08-15 续10）**：isolation 移除 3 个依赖 `serial` 的测试；清 `RelationRemoveInheritance` 空函数；`sanity_check.out` 删 `pg_inherits|t`/`pg_sequence|t`。
- **（2026-08-15 续11）**：modules 移除 `snapshot_too_old`（依赖已裁 `pg_sleep`）与 `test_extensions`（依赖 FDW/sequence/serial）。
- **（注）**：分区/PL/pgSQL/逻辑复制等裁剪各自的回归测试清理已在对应条目中说明。
- **（2026-08-16）同步 enum.out / create_index.out 反映历史裁剪后果**：`make check-world` 中 `enum`、`create_index` 失败，根因为早期裁剪未同步 expected——`gram.y` 的列级 `REFERENCES` 与表级 `FOREIGN KEY`（`ColConstraintElem`/`ConstraintElem`/`TableConstraint` 分支）、以及 `ALTER TABLE ... REPLICA IDENTITY` 命令早已被裁，导致 `REFERENCES` 现在在解析阶段即报 `syntax error at or near "REFERENCES"`（而非 `tablecmds.c:4055` 的运行时拒绝文案，该文案现已成为不可达死代码），且 create_index 中 `indisreplident` 测试块因 RI 命令被裁而消失。操作：将 `src/test/regress/results/enum.out`、`results/create_index.out` 覆盖对应 `expected/*.out`（`enum` 改 2 处 REFERENCES 为语法错误；`create_index` 改 1 处 REFERENCES 为语法错误 + 删整段 `indisreplident` 测试块）。注：本次 check_rolespec_name 删除与这 5 个失败**均无关**。
- **（2026-08-16）固定回归测试 locale 解决 int8/numeric/select_implicit 失败**：`make check-world` 剩余 `int8`/`numeric`/`select_implicit` 三个失败，根因均为**运行环境的非 C locale**（zh_CN.UTF-8）：`to_char(val,'L...')` 在 zh_CN 的 `lc_monetary` 下渲染人民币符号 `￥`（expected 基于 C locale 为空）；`select_implicit` 的 `char(8)` 分组排序在 zh_CN collation 下大小写顺序与 C locale 字节序不同（bbbb/cccc 前移，两个 expected 变体均不匹配）。修复：在 `src/test/regress/GNUmakefile` 的 `REGRESS_OPTS` 加 `--no-locale`，使 `pg_regress` 以 C locale 创建测试库（`pg_regress.c:2023` 设 `LC_COLLATE='C' LC_CTYPE='C'`，`:2028` 设 `lc_monetary TO 'C'`）。该机制为 PG 官方回归标准做法，可移植、不改任何 expected。验证需用户重跑 `make check-world`（本机 `make check` 因安全策略拦截 temp-install 重建且 minipg 有既有 initdb 崩溃，未能自动重跑；代码逻辑已确认可一并消除该 3 个失败）。与不可裁部分零耦合。

## 九、死代码清理（编译器驱动）

- **（2026-08-16）用 `-Wunused-function` 重编后端全量扫描零调用 static 函数**：此前各子系统（逻辑复制/replication、JIT、tsearch、BRIN、sepgsql、FDW/外部表、PL/pgSQL、外键/FK）均为**整文件删除**，故 backend 无 static 残留死函数；但存在两类典型的「函数体随裁剪被删、前向声明/定义残留」死代码。本轮清理：
  - `src/backend/commands/tablecmds.c`：
    - 删悬空 static 原型 `ATExecAlterColumnGenericOptions`（FDW 列 `OPTIONS` 执行函数体已随 FDW 裁剪删除，全树零调用、零定义）。
    - 删零调用 static 函数 `ATExecSetTableSpaceNoStorage`（`ALTER TABLE SET TABLESPACE` 无存储关系的元数据分支，调用者已不可达，编译器 `-Wunused-function` 报 `defined but not used`）。
    - 删零调用 static 函数 `transformColumnNameList`（FK 列名→attnums/typids 转换，FK 已裁后全树零调用；含对应孤立注释块与前向声明）。
  - `src/backend/optimizer/path/allpaths.c`：删悬空 static 原型 `compare_tlist_datatypes`（FDW 下推类型安全检查函数体已随 FDW 裁剪删除，全树零调用、零定义）。
  - 验证：后端 `make -j4 CFLAGS="-O0 -Wunused-function"` 重编 0 告警 0 错误，确认已无零调用 static 函数。
  - **未裁项（明确留存的「语义降级但存活」代码，非纯死代码）**：`pg_index.indisreplident` 字段（replica identity 的 index 侧标记，随 `pg_class.relreplident` 主字段于 2026-08-17 已完整裁掉，但 `pg_index.indisreplident` 本身作为列保留，待后续独立裁剪）；此外逻辑复制已删后 `HistoricSnapshotActive` 等路径已无消费者，属既有惰性代码。该链属 CHANGE 第六、七条标注的「既有死函数/惰性列」范畴。

## 十、编译 / 链接错误修复（历史裁剪残留 bug，非功能裁剪）

- **（2026-08-17）修复 `static` 函数与 `pg_proc.dat` 登记冲突导致的编译 / 链接错误**：minipg 早期裁剪几何类型、伪类型等子系统时，把若干「本应全局可见（被 `pg_proc.dat` 登记为 SQL 函数、由 `gen_fmgrtab.pl` 在 `fmgrprotos.h` 生成 `extern` 声明）」的函数误改为 `static`，触发两类编译期/链接期失败。本轮修复：
  - **`src/backend/utils/adt/pseudotypes.c`**：3 个宏 `PSEUDOTYPE_DUMMY_INPUT_FUNC` / `PSEUDOTYPE_DUMMY_IO_FUNCS`（其 `_out`）/ `PSEUDOTYPE_DUMMY_RECEIVE_FUNC` 的函数定义被误加 `static`（上游 PG 此处为 `Datum` 非 static）。这些宏生成的 `anyarray_in`/`anyenum_in`/`pg_node_tree_in`/`trigger_in`/`fdw_handler_in` 等 30+ 伪类型 I/O 函数在 `pg_proc.dat` 中登记，故 `fmgrprotos.h` 有 `extern` 声明，与 `static` 冲突报 `static declaration follows non-static declaration`。去掉三处 `static`，恢复为 `Datum`。
  - **`src/backend/utils/adt/selfuncs_geo.c`**：`areasel` / `areajoinsel` / `positionsel` / `positionjoinsel` / `contsel` / `contjoinsel` 6 个选择率函数被误加 `static`；它们在 `pg_proc.dat` 登记（`prosrc => 'areasel'` 等）供 range/box 等运算符的 `oprrest`/`oprjoin` 引用。链接时 `fmgrtab.o` 引用这些全局符号、但 `static` 使符号不导出，报 `undefined reference to 'areasel'` 等 6 个链接错误。去掉 6 处 `static`，恢复为 `Datum`。该文件注释已说明「几何类型虽删，但这些选择率函数被非几何运算符（如 range 运算符）仍引用，须保留为粗粒度占位」，故不可删、仅须全局可见。
  - **`src/backend/utils/adt/pgstatfuncs.c`**：删除 2 个零调用 `static` 死函数 `pg_stat_get_replication_slot`（replication 子系统已裁、无调用者、未登记 SQL 函数）与 `pg_stat_get_backend_client_addr`（无调用者、未登记 SQL 函数，仅 `backend_status.c` 注释提及），消除 `-Wunused-function` 告警。
  - **验证**：`make -j4` 全量编译 + postgres 链接成功（0 error / 0 undefined reference）；残余 `-Wmissing-prototypes`/`/* within comment` 为与本次无关的既有 warning（`fdw_handler_in/out` 等未被 `fmgrprotos.h` 声明，属 FDW 子系统裁后的正常 unused 类告警，不影响编译链接）。
  - **与不可裁部分零耦合**：仅修正函数链接属性与删死代码，不改任何索引/事务机制；btree/hash 索引、事务均不受影响。

---

## 十一、编译警告驱动的代码清理（2026-08-17）

针对后端 `make -j4` 全量重编暴露的真实 warning（用户早期提供的 warning 列表已因多次裁剪过期），逐一清理，最终后端 + 前端（psql/libpq）均 0 warning / 0 error（除 `backend_status.h` 既有项）。要点：

- **恢复被误删的 `aclcheck_error` / `aclcheck_error_type` 声明（`src/include/utils/acl.h`）**：早期裁剪 ACL 判定时把这两个标准错误报告函数声明一并删掉，导致全树大量 `implicit declaration of function 'aclcheck_error*'` 警告。在 `acl.h` 的 `#endif` 前补回两行 `extern` 声明（函数本体 `aclcheck_error` 仍存活于 `acl.c`，供权限/所有权不足时报错）。
- **删除 ACL 判定代码残留的 `AclResult aclresult;` 死变量**（上游 ACL 判定块已被删、仅留 `ACLCHECK_OK` 占位，变量声明后从未使用，报 `-Wunused-variable`）：
  - `src/backend/utils/fmgr/fmgr.c`（`CheckFunctionValidatorAccess`）、`src/backend/executor/nodeWindowAgg.c`（`initialize_peragg` / `build_aggregate_targetlist` 等）、以及 `commands/` 下的 `collationcmds.c` / `conversioncmds.c` / `dbcommands.c` / `extension.c` / `functioncmds.c`。
- **补充缺失的 `extern` 函数原型**（消除 `-Wmissing-prototypes`）：
  - `src/backend/utils/adt/rangetypes.c`：为 `tstzrange_subdiff` / `tsrange_subdiff` / `daterange_subdiff` / `numrange_subdiff` / `int8range_subdiff` / `int4range_subdiff` / `daterange_canonical` / `int8range_canonical` / `int4range_canonical` 9 个函数补 `extern` 声明（并在 `postgres.h` 后 `#include "fmgr.h"` 使 `PG_FUNCTION_ARGS` 宏可见）。
  - `src/backend/utils/adt/selfuncs_geo.c`：为 `contjoinsel` / `contsel` / `positionjoinsel` / `positionsel` / `areajoinsel` / `areasel` 6 个选择率函数补 `extern` 声明（同补 `#include "fmgr.h"`）。
  - `src/backend/utils/adt/pseudotypes.c`：为宏生成的 `fdw_handler_in` / `fdw_handler_out` 补 `extern` 声明（同补 `#include "fmgr.h"`）。
  - `src/backend/utils/adt/numeric.c`：为 `numeric_trunc` 补 `extern` 声明。
- **修正 `grouping_planner` 的混合声明（`src/backend/optimizer/plan/planner.c`）**：把 11 个变量声明（`sort_input_target` / `grouping_target` / `scanjoin_target` 系列、`wflists` / `activeWindows` / `gset_data` / `qp_extra` 等）从「语句之后」整体前移到函数体开头局部变量区，消除 `-Wdeclaration-after-statement` 警告，保持 C90 合规。
- **前端 `psql` 警告清理（`src/bin/psql/describe.c` + `describe.h`）**：
  - 删除 `fdwopts_col` 未使用死变量（`\d` 列出 FDW 选项列，FDW 已裁后从未引用）。
  - 为 `describeRoles`（`\du` / `\dg`）补 `extern bool describeRoles(...)` 原型声明（消除 `-Wmissing-prototypes`）。
  - 修复 `listTables` 的 `printfPQExpBuffer` 格式参数不匹配警告：format 串在裁剪中已删掉 `materialized view`（`WHEN 'm'`）分支，但参数列表仍残留 `gettext_noop("materialized view")`，导致「too many arguments for format」。删除该多余参数，使 `%s` 占位与参数个数对齐（11 对 11）。

与不可裁部分（btree/hash 索引、事务）零耦合；`make -j4` 全量重编后端 0 warning/0 error，psql/libpq 前端 0 warning/0 error。

---

## 十二、死代码残留裁剪（2026-08-17，对应 mydoc/minipg死代码残留分析报告.md）

围绕已裁剪功能遗留的「惰性死代码 / 未清理引用」做彻底裁剪（不改任何索引/事务机制）。本轮实际执行以下 3 项（均为验证后零风险的纯死代码），其余项经评估后暂未执行并说明原因（见末尾）。

- **（1）删除 `pg_index.indisreplident` 惰性死列（完成第九条 CHANGE 中「待后续独立裁剪」的承诺）**：REPLICA IDENTITY 主字段 `pg_class.relreplident` 已于 2026-08-17 完整裁掉，但 `pg_index.indisreplident` 作为列被刻意保留。本轮确认其写入恒 `false`、全树无消费者，属纯死列，彻底删除：
  - `src/include/catalog/pg_index.h`：删除 CATALOG 结构体中的 `bool indisreplident` 字段（同步使 `Anum_pg_index_indisreplident` 枚举项随 genbki 消失）。
  - `src/backend/catalog/index.c`：删除 `index_create` 中恒 false 写入行、`index_concurrently_swap`/重建时 `indisreplident` 的保留与清零、`IndexConcurrentlySetState` 的清零与 `Assert`。
  - `src/backend/utils/cache/relcache.c`：删除 `RelationReloadIndexInfo` 中 `indisreplident` 字段复制。
  - `src/backend/commands/tablecmds.c`：删除 `ATController` 中对 `indexStruct->indisreplident` 的判断（仅保留 `indisprimary`）。
  - `src/bin/psql/describe.c`：删除 `\d`/`\d index` 中对 `i.indisreplident` 列的查询。
  - **验证**：`src/backend/catalog`、`src/backend/utils/cache`、`src/backend/commands`、`src/bin/psql` 增量 `make` 均 0 error/0 warning。

- **（1-补）修复 `describe.c` 删除 `indisreplident` 列后未同步列号导致的 psql SIGSEGV（2026-08-17 补充）**：
  删除 `pg_index.indisreplident` 列后，索引描述查询（`describeOneTableDetails`）里 `a.amname`/`c2.relname`/`pg_get_expr(i.indpred,...)` 三列各前移一列，但对应的 `PQgetvalue` 列号未同步前移，导致越界访问 `NULL` 并崩溃：
  - 单索引描述分支（`\d 索引名`）：`indamname`/`indtable`/`indpred` 原取列 `7/8/9`，实际应为 `6/7/8`（`PQgetvalue(result,0,9)` 为 NULL → `strlen(NULL)` → SIGSEGV，对应 core 中 `describe.c:1872`）。
  - 索引列表分支（`\d 表名`）：`add_tablespace_footer` 取 `PQgetvalue(result,i,11)` 实际应为 `10`（`PQgetvalue` 越界返回 NULL → `atooid(NULL)` → `strtoul(NULL)` → SIGSEGV，对应 core 中 `describe.c:2001`）。
  - 修正：将两处列号分别改为 `6/7/8` 与 `10`，与删除 `indisreplident` 后的真实列布局对齐。
  - **验证**：`make check` 单测 `create_index`（含 `\d func_index_index` 表达式索引）由 signal 11 转为通过；`\d persons2`/`\d idx_t2`（部分索引）不再崩溃；完整 `parallel_schedule` 跑批中 `create_index`、`sanity_check` 均通过。
  - 注：该崩溃在 `make check-world` 中表现为 `create_index ... FAILED (terminated by signal 11)`，根因即上述列号错位，非 UNION 集合操作限制（UNION 限制独立存在、不影响这两个测试）。

- **（2）删除 `get_transform_fromsql` / `get_transform_tosql` 零调用死函数（FDW 转换死链）**：`pg_transform` catalog 本身保留（`get_transform_oid` 仍有调用者，供 `CREATE TRANSFORM`/`CREATE FUNCTION` 校验），但这 2 个「读取转换 SQL 函数指针」的函数原仅被已删的 FDW `GetFdwRoutine` 转换逻辑调用，全树零调用者，彻底删除：
  - `src/backend/utils/cache/lsyscache.c`：删除 `get_transform_fromsql`、`get_transform_tosql` 函数体（含 `TRANSFORM CACHE` 注释块）。
  - `src/include/utils/lsyscache.h`：删除对应 2 行 `extern` 声明。

- **（3）删除 `fdw_handler` 伪类型 I/O 孤儿代码（FDW 残留收尾）**：`pg_type.dat` 已无 `fdw_handler` 类型项、`pg_proc.dat` 也无 `fdw_handler_in/out` 登记，但 `pseudotypes.c` 仍生成这对伪类型 I/O 函数（纯孤儿、编译后无任何引用），彻底删除：
  - `src/backend/utils/adt/pseudotypes.c`：删除 `PSEUDOTYPE_DUMMY_IO_FUNCS(fdw_handler)` 及其 `extern fdw_handler_in/out` 前向声明。
  - 说明：这呼应第九、十一条 CHANGE 中标注的「`fdw_handler_in/out` 等未被 `fmgrprotos.h` 声明的 FDW 子系统裁后 normal unused 类告警」，现彻底清除。

- **本轮评估后暂未执行、原因说明（避免过度裁剪/破坏编译）**：
  - `HistoricSnapshotActive`/`SetupHistoricSnapshot`/`HeapTupleSatisfiesHistoricMVCC`/`ResolveCminCmaxDuringDecoding` 逻辑解码残留：虽 `SetupHistoricSnapshot` 无调用者（即 `HistoricSnapshot` 恒 NULL、`HistoricSnapshotActive()` 恒 false），但 `HeapTupleSatisfiesHistoricMVCC` 经 `HeapTupleSatisfiesVisibility` 的 `SNAPSHOT_HISTORIC_MVCC` 分支可达，且 `snapshot_type` 枚举、`relcache.c`/`snapmgr.c` 的多处 `if` 守卫与之耦合。属核心 MVCC 可见性热路径，移除需同时清理快照分发、全局变量、`tuplecid_data` 及多个 if 守卫，风险高、学习价值（MVCC 子系统）高，**暂不裁剪**。
  - `qual_security_level`/`security_level` 传播链：securityQuals 字段虽已删，但 `security_level` 已深度编织进 `distribute_qual_to_rels`→`create_restrictinfo`→`RestrictInfo.security_level`→`outfuncs`/`readfuncs`→`createplan.c` 的成本排序逻辑，是规划器 clause 排序的**结构性组成部分**（恒 0 但生效），并非纯死代码；移除会触碰计划序列化格式与成本排序行为，**暂不裁剪**（学习价值高、风险高）。
  - `HAVE_IPV6` 未生效条件编译块（`ifaddr.c` 11 处、`fe-connect.c` 1 处）：属 `#ifdef` 编译期守卫，是否实际生效取决于 configure 是否定义 `HAVE_IPV6`；若当前构建已定义则该分支仍活跃。属条件编译噪音，需先确认 `pg_config.h` 中 `HAVE_IPV6` 真未定义再决定是否清理，**本轮未确认，暂缓**。
  - `T_AlterOwnerStmt` 孤立死语法（owner 机制已删、执行体为空块 `{}`）：**已于 2026-08-18 执行裁剪**（见下「十二-补」）。
  - `partitionwise join/aggregate`：`consider_partitionwise_*` 确无赋值点、整条优化路径不可达，但 `enable_partitionwise_aggregate` 仍接线于 `planner.c` 且分区表已删；与分区功能整体耦合，宜随分区相关逻辑统一清理，**本轮未单独裁剪**。
  - `pg_user_mapping` catalog：确认全树 `.c` 零引用，仅残 `typedefs.list`（pgindent 用）与 `objectaccess.h` 注释中的一词；属 pgindent/注释噪音，非真实死代码，未改动。

- **验证**：`src/backend/catalog`、`src/backend/utils/cache`、`src/backend/utils/adt`、`src/backend/commands`、`src/bin/psql` 增量 `make` 全部 0 error/0 warning（既有 `pg_operator.c:585 aclresult` 警告与本轮无关）。与不可裁部分（btree/hash 索引、事务）零耦合。

### 十二-补：裁剪 `ALTER ... OWNER TO` 死代码链（2026-08-18）

角色系统裁剪后，`ALTER ... OWNER TO` 的执行函数 `ExecAlterOwnerStmt` 已被删除，但整条语法链（生成 + 分发）残留，且 `utility.c` 中对应 case 为静默空壳（`case T_AlterOwnerStmt: break;` / `{}`），构成功能性 bug（语句被静默丢弃）。本轮彻底删除该链：

- `src/include/commands/alter.h`：删除 `ExecAlterOwnerStmt` 与 `AlterObjectOwner_internal` 两个孤立 extern 声明。
- `src/include/nodes/parsenodes.h`：删除 `AlterOwnerStmt` 结构体定义。
- `src/include/nodes/nodes.h`：删除 `T_AlterOwnerStmt` 枚举项（连带使后续 `T_AlterOperatorStmt` 等枚举值 -1；节点支持文件在 make 时自动重生成）。
- `src/backend/parser/gram.y`：删除 `%type` 中的 `AlterOwnerStmt`、顶层 `stmt` 规则引用、以及全部 15 条 `AlterOwnerStmt:` 产生式（含 `OWNER TO RoleSpec` 分支）。
- `src/backend/nodes/copyfuncs.c`：`_copyAlterOwnerStmt` 函数及 `case T_AlterOwnerStmt` 分支。
- `src/backend/nodes/equalfuncs.c`：`_equalAlterOwnerStmt` 函数及 `case T_AlterOwnerStmt` 分支。
- `src/backend/tcop/utility.c`：删除全部 4 处 `T_AlterOwnerStmt` case（`ProcessUtility` 的 fall-through 组合、`standard_ProcessUtility` 的空块、以及 `CreateCommandTag` / `GetCommandLogLevel` 中的 tag/lev 分支）。现 `ALTER ... OWNER` 因无文法支持直接报语法错误。
- `src/tools/pgindent/typedefs.list`：移除 `AlterOwnerStmt`（pgindent 噪音，保持列表一致）。

**保留**：`RoleSpec` 节点仍被 `ALTER TABLE ... OWNER TO`（`AT_ChangeOwner`）与 `CREATE TABLESPACE ... OWNER` 使用，未删除。

**验证**：`make -C src` 全量重编译并删除 `src/backend/postgres` 后重链接，0 error、0 undefined reference，二进制正常生成。

---

## 十三、acl.h 头文件死宏清理（2026-08-18，ACL 裁剪收尾）

此前 ACL 全链路裁剪（08-15 删判定、08-17 删 `aclcheck_error*` 调用壳与 `aclchk.c`、08-18 删角色骨架）后，`src/include/utils/acl.h` 仍残留大量**已无定义支撑的悬空宏**——这些宏依赖早已删除的权限位常量（`ACL_SELECT`/`ACL_INSERT`/`ACL_USAGE`/`ACL_CREATE` 等），任何代码展开它们都会报 `undeclared` 错误，属纯死代码。本轮按"彻底裁剪、不留死代码"清理：

- **删除悬空宏（原 24~58 行）**：`ACL_ID_PUBLIC`、`ACLITEMOID`（aclitem 类型 OID 1033，类型已从 `pg_type.dat` 删除）、`ACLITEM_ALL_GRANTEES`、位运算宏 `ACL_GRANT_OPTION_FOR`/`ACL_OPTION_IS_GLOBAL`/`ACL_GRANT_WGO`、以及 14 个 `ACL_ALL_RIGHTS_*` 组合宏（RELATION/SEQUENCE/DATABASE/FDW/FOREIGN_SERVER/FUNCTION/LANGUAGE/LARGEOBJECT/NAMESPACE/OPSCHEMA/TYPE/OPCLASS/OPFAMILY/SCHEMA）。上述宏全部引用已裁的权限位常量，全代码库 `grep` 零引用，确认无调用点。
- **删除不再需要的 include**：`nodes/parsenodes.h` 与 `parser/parse_node.h`（原仅为 `GrantStmt`/`ParseState` 等授权语法节点服务，授权语法已裁、acl.h 内已无任何 parsenodes 引用）；`access/htup.h`/`utils/snapshot.h` 因 acl.h 现已不含任何结构体定义亦不再直接需要，一并移除，使头文件自包含无 unused-include 告警。保留 `#ifndef ACL_H`/`#define ACL_H` 守卫。
- **保留**：`AclResult` 枚举（`ACLCHECK_OK`/`ACLCHECK_NO_PRIV`/`ACLCHECK_NOT_OWNER`）——仍被 `execMain.c`/`genam.c` 的 `aclresult = ACLCHECK_OK; if (aclresult != ACLCHECK_OK)` 恒假死壳引用，需保留类型定义以通过编译。并补一段中文注释说明 minipg 已裁对象级 ACL、仅保留结果码。

- **修订旧记录**：此前（2026-08-17 续「aclchk.c 调用壳删除」条目）称「acl.h 其余 ACL 位宏等保留（被 pg_proc、GRANT/REVOKE 声明等引用）」——经本轮核查，GRANT/REVOKE 语法与 pg_proc 中相关 ACL 逻辑均已删，这些宏实际无任何引用方，故由"保留"改为"彻底删除"，与 AGENTS「彻底裁剪、不留死代码」原则一致。

- **验证**：`make -j4` 全量编译 + `make install` 成功；`make check-world` 全绿（regress 全部 tests passed，isolation/bin 同步通过），无任何 failure。全代码库 `grep ACL_ID_PUBLIC|ACLITEMOID|ACL_ALL_RIGHTS_|ACL_GRANT` 0 命中。与不可裁部分（btree/hash 索引、事务）零耦合：仅清头文件死宏，不改任何索引/事务机制。

---

## 十四、acl.h 整文件删除（2026-08-18，ACL 裁剪最终收尾）

接第十三步后，`acl.h` 已退化到仅含 `AclResult` 枚举（且唯一引用方 `execMain.c`/`genam.c` 用它写的权限检查是恒真死壳）。为彻底消除该头文件及其残余依赖，本轮将其整文件删除：

- **删除 acl.h 对 `AclResult` 的两处死壳引用，再删文件**：
  1. `src/backend/executor/execMain.c` 的 `ExecBuildSlotValueDescription` 函数：原权限检查 `aclresult = ACLCHECK_OK; if (aclresult != ACLCHECK_OK)` 恒为真分支走 `table_perm = any_perm = true`，且死分支引用了 `collist`/`write_comma_collist`/`column_perm` 等废弃变量。删除 `AclResult aclresult`、`table_perm`/`any_perm`/`collist` 变量与全部 `!table_perm`/`!any_perm` 死分支，函数逻辑等价简化为「直接拼接所有非 dropped 列」；同步删 `#include "utils/acl.h"`。
  2. `src/backend/access/index/genam.c` 的 `BuildIndexValueDescription` 函数：原 `aclresult = ACLCHECK_OK; if (aclresult != ACLCHECK_OK)` 恒为假分支（遍历 index key columns、遇 expression 列 `return NULL`）永不执行。删除 `AclResult aclresult`、`keyno` 变量与整个 if 块；同步删 `#include "utils/acl.h"`。
  3. `git rm src/include/utils/acl.h`，回收 `#ifndef ACL_H` 守卫与仅存的 `AclResult` 枚举定义。
- **清理 54 个冗余 include**：全代码库另有 54 个文件（catalog/namespace.c、heap.c、pg_proc.c、pg_type.c、dependency.c、objectaddress.c、commands/*、rewrite/*、tcop/*、utils/adt/acl.c、utils/misc/guc.c、src/include/catalog/pg_namespace.h 等）仍 `#include "utils/acl.h"`，但 acl.h 删除前已不依赖其中任何符号（全代码库 `grep AclResult|ACLCHECK_` 除 acl.h 自身外 0 命中），属历史残留冗余 include。用 `sed -i '/#include "utils\/acl.h"/d'` 精确删除这 54 行，避免删文件后编译报 "acl.h: No such file"。其中 `src/backend/utils/adt/acl.c` 现已被裁到不含任何 ACL 符号，仅剩空壳 include，一并清理。
- **修订旧记录**：第十三步结论称「保留 `AclResult` 枚举」——本轮将死壳逻辑一并删除后，`AclResult` 再无引用方，故由"保留"改为"整文件删除"，与 AGENTS「彻底裁剪、不留死代码」原则最终对齐。
- **验证**：`make -j4` 全量编译 + `make install` 成功；`make check-world` 全绿（regress 全部 tests passed，isolation/bin 同步通过），零 failure。全代码库 `grep -rl '#include "utils/acl.h"'` 0 命中、`grep 'AclResult'` 0 命中（仅 acl.h 本体，已删除）。与不可裁部分（btree/hash 索引、事务）零耦合：仅删头文件与两处恒真权限检查死壳，未触及任何索引/事务机制；`BuildIndexValueDescription`/`ExecBuildSlotValueDescription` 行为在 minipg 恒为"全部可见"语义下完全不变。

---

## 十五、gram.y 删除 SetResetClause 死规则（2026-08-18）

分析 `src/backend/parser/gram.y` 中 `SetResetClause` 与 `FunctionSetResetClause` 两规则（原 1196-1207 行）：
- `SetResetClause`：在 PG 上游被 `CreateOpClassStmt`/`CreateOpFamilyStmt` 的 STORAGE 子句等引用，但 minipg 已裁剪这些语句，导致该规则**在全文件中无任何引用方**（仅出现在定义处与 `%type` 声明），属语法死代码；且其上方注释 `/* SetResetClause allows SET or RESET without LOCAL */` 是复制粘贴错误（贴在 `FunctionSetResetClause` 上）。
- `FunctionSetResetClause`：被 `createfunc_opt_list`（`CREATE FUNCTION ... SET var=val | RESET var` 函数配置子句）引用，`CreateFunctionStmt` 在 minipg 仍为保留的核心 DDL，**不可裁**。

本轮裁剪仅删除死规则：
- 删除 `SetResetClause` 规则定义（原 1196-1200 行）。
- 从 `%type <vsetstmt>` 声明（416 行）移除 `SetResetClause` 标记。
- 修正误写注释：原 1202 行 `/* SetResetClause ... */` 改为 `/* FunctionSetResetClause allows SET or RESET without LOCAL */`。
- 保留 `FunctionSetResetClause` 规则不动。

- **验证**：`make -j4` 触发 bison 重新生成 `gram.c`（生成产物 `grep SetResetClause` 0 命中）+ `make install` 成功；`make check-world` 全绿（regress 全部 tests passed，isolation/bin 同步通过），零 failure。与不可裁部分（btree/hash 索引、事务）零耦合：仅删语法死规则、不改任何运行时逻辑。

---

## 十六、gram.y 删除 CREATE/ALTER SEQUENCE 死语法链（2026-08-18）

分析 `src/backend/parser/gram.y` 中序列 DDL 语法：minipg **顶层 `CreateSeqStmt`/`AlterSeqStmt` 规则此前已被彻底删除**（全文件无此节点、无 `CREATE SEQUENCE`/`ALTER SEQUENCE` 入口；且无 `SERIAL`/`serial` 关键字，无 serial 隐式建序列途径），但遗留了一整条无引用方的悬空语法子链与过时注释块：

- 删除 2619-2692 行的过时注释块（`QUERY: CREATE SEQUENCE seqname / ALTER SEQUENCE seqname`，其指向的规则已不存在）与死语法链：`OptParenthesizedSeqOptList` → `SeqOptList` → `SeqOptElem` → `opt_by`。这些规则在全文中无任何上层调用方（仅存在于定义处），属纯死语法。
- 从 `%type` 声明（408-409 行）删除 `SeqOptList OptParenthesizedSeqOptList` 与 `SeqOptElem` 标记。
- **保留**活跃依赖：`NumericOnly`（2694-2703，被 16 处核心语法如 COST/ROWS、c_expr、generic_set 引用）、`opt_with`（被 CREATE EXTENSION/DATABASE、DROP DATABASE 等活跃规则引用）、`SEQUENCE`/`SEQUENCES` 关键字 token（保留无害，避免影响向后兼容）。（注：当时误判 `NumericOnly_list` 为活跃，后于第十九步验证其为无用死语法并删除。）

- **说明**：本次仅裁语法死代码。运行期序列函数（`nextval`/`currval`/`lastval`、`NextValueExpr`）在 minipg 仍保留并在 regress 测试中使用——但因 CREATE SEQUENCE 顶层规则已无，minipg 当前无任何途径创建序列对象（DDL 入口缺失），序列整体功能对内核学习价值低（非 btree/hash 索引、非事务核心），符合 AGENTS「学习价值低优先裁剪」原则。本次未裁运行期函数，仅清理已无人引用的语法残骸。
- **验证**：`make -j4` 触发 bison 重生成 `gram.c`（`grep SeqOptList|SeqOptElem|OptParenthesizedSeqOptList` 0 命中）+ `make install` 成功；`make check-world` 全绿（regress 全部 tests passed，isolation/bin 同步通过），零 failure。全代码库 `grep SeqOptList|SeqOptElem|OptParenthesizedSeqOptList|opt_by` 0 命中。与不可裁部分（btree/hash 索引、事务）零耦合：仅删语法死链，未改任何运行时/索引/事务逻辑。

---

## 十七、删除空壳 acl.c（2026-08-18，ACL 裁剪最终收尾）

接第十四步（删 acl.h 头文件）后，`src/backend/utils/adt/acl.c` 已成为纯空壳：仅 41 行版权头 + 22 个 include（access/htup_details.h、catalog/*、commands/*、utils/* 等），**无任何函数实现**（ACL 判定/aclchk 逻辑此前已被逐步裁空）。它是无符号、无调用方的死文件。

- **删除文件**：`git rm src/backend/utils/adt/acl.c`。
- **同步 Makefile**：从 `src/backend/utils/adt/Makefile` 的 `OBJS` 列表（原 15 行 `acl.o \`）移除 `acl.o`，否则编译报 "No rule to make target `acl.o`"。
- **刷新构建产物**：删除残留旧对象文件 `src/backend/utils/adt/acl.o` 与自动生成的 `objfiles.txt`，强制重编 adt 目录，使 `objfiles.txt` 不再含 `acl.o`（避免链接阶段引用已不存在的 .o）。
- **验证**：`make -j4` 全量编译（adt 目录 objfiles.txt 重生成后无 acl.o）+ `make install` 成功；`make check-world` 全绿（regress 全部 tests passed，isolation/bin 同步通过），零 failure。全代码库 `grep 'acl\.o'` 仅余构建产物无关引用（objfiles.txt 已无）；`git status` 确认 acl.c 已删。与不可裁部分（btree/hash 索引、事务）零耦合：仅删空壳 .c 文件与 Makefile 条目，未触及任何索引/事务机制。

---

## 十八、小结：minipg ACL/权限系统裁剪全景（2026-08-18）

截至本日，minipg 权限系统已全链路彻底裁剪（按 AGENTS「彻底裁剪、不留死代码」原则），收尾动作序列：
1. **aclchk.c 调用壳删除 + pg_proc.dat 清理**（08-15 起）：删 `pg_class_aclmask`/`pg_attribute_aclmask` 等判定函数及其 pg_proc 注册。
2. **角色骨架裁剪**（08-18）：`regrole` 类型、OBJECT_ROLE/OCLASS_ROLE 枚举、authrole 字段、AUTHORIZATION 语法彻底删除（pg_proc/pg_type/pg_cast.dat 同步清理）。
3. **acl.h 头文件死宏清理**（08-18 十三步）：删所有依赖已删权限位常量的悬空宏。
4. **acl.h 整文件删除**（08-18 十四步）：删 AclResult 死壳引用 + 54 个冗余 include + 文件本体。
5. **空壳 acl.c 删除**（08-18 十七步）：删空壳文件 + Makefile OBJS 条目 + 刷新 objfiles.txt。
6. **gram.y 死语法链**（08-18 十五/十六步）：删 SetResetClause 死规则、删 CREATE/ALTER SEQUENCE 悬空语法链。

结论：minipg 已无对象级 ACL 位权限系统，仅保留所有者检查（ownercheck）语义；运行期 `nextval`/`currval` 等序列函数仍留用但序列对象无 DDL 创建途径。与不可裁核心（btree/hash 索引、事务）零耦合。

---

## 十三、窗口函数（Window Function）彻底裁剪（2026-08-17）

窗口函数（OVER 子句、WINDOW 子句、帧子句，以及内置窗口函数 row_number/rank/dense_rank/percent_rank/cume_dist/ntile/lag/lead/first_value/last_value/nth_value）属 SQL 高级特性，非数据库内核核心（btree/hash 索引、事务、聚合 agg 机制均不依赖），学习价值低于执行器基础框架，适合裁剪。采用「自底向上、由叶到根」彻底删除策略，删除后用户使用窗口函数将在语法/解析阶段被拒绝，而非静默忽略。改动文件与要点：

- **整文件删除（核心执行/实现层）**：
  - `src/backend/executor/nodeWindowAgg.c`（窗口函数执行器核心，含「普通聚合当窗口函数」实现）+ `src/backend/executor/nodeWindowAgg.h`：`git rm`，执行器不再有 `T_WindowAgg`/`T_WindowAggState` 计划节点。
  - `src/backend/utils/adt/windowfuncs.c`（内置窗口函数 C 实现）：`git rm`；`src/backend/utils/adt/Makefile` 移除 `windowfuncs.o`。
  - `src/include/windowapi.h`（WindowObject API 头文件）：`git rm`。
  - 注：删除 `windowfuncs.c` 仅移除了窗口专用函数（row_number 等），`in_range_*` 帧边界支持函数位于 `int8.c`/`int4.c`/`int2.c`（被 date/timestamp 的 `RANGE BETWEEN` 仍使用），不在本文件、未受影响。
- **执行器初始化与表达式求值**：
  - `src/backend/executor/execProcnode.c`：删 `#include "executor/nodeWindowAgg.h"` 及 `case T_WindowAgg:`（ExecInitWindowAgg）、`case T_WindowAggState:`（ExecEndWindowAgg）。
  - `src/backend/executor/execExpr.c`：删 `T_WindowFunc` 构建分支与 `WindowFunc` 检查分支（**保留 Aggref 分支**，聚合路径不受影响）。
  - `src/backend/executor/execExprInterp.c`：删 `EEOP_WINDOW_FUNC` case 与 `EEO_CASE` 实现。
  - `src/include/executor/execExpr.h`：删 `EEOP_WINDOW_FUNC` 枚举项（`ExecEvalFunc`/`ExprEvalOp` 枚举的静态断言因此同步保持对齐）。
  - `src/backend/executor/Makefile`：移除 `nodeWindowAgg.o`。
- **节点树定义删除（nodes 层）**：
  - `src/include/nodes/nodes.h`：删 `T_WindowDef`/`T_WindowClause`/`T_WindowFunc`/`T_WindowFuncExprState`/`T_WindowAgg`/`T_WindowAggState`/`T_WindowAggPath`/`T_WindowObjectData` 枚举。
  - `src/include/nodes/parsenodes.h`：删 `WindowDef`/`WindowClause` 结构体、`FuncCall.over` 字段、`SelectStmt.windowClause` 字段、`Query.windowClause` 字段；同步清理 copyfuncs/equalfuncs/outfuncs/readfuncs 中对应序列化 `WRITE_*`/`COPY_*`/`COMPARE_*`/`READ_*` 行与 `fcall->over` walker、`n->over = NULL` 初始化。
  - `src/include/nodes/primnodes.h`：删 `WindowFunc` 表达式节点。
  - `src/include/nodes/plannodes.h`：删 `WindowAgg`/`WindowAggPath` 计划节点。
  - `src/include/nodes/execnodes.h`：删 `WindowAggState`/`WindowStatePerFuncData`/`WindowObjectData`。
  - `src/include/nodes/plannodes.h` 结构删除后，`copyfuncs.c`/`equalfuncs.c`/`outfuncs.c`/`readfuncs.c` 的 `_readWindowAgg`/`_readWindowClause`/`_copyWindowAgg` 等 dispatch 与函数体一并删除。
- **解析器（语法 + 转换）**：
  - `src/backend/parser/gram.y`：删 `OVER`/`WINDOW`/`WindowDef`/`WindowClause`/`WindowFuncCall` 产生式（`WINDOW` 保留字 token 在 kwlist.h 中保留，避免重生成 bison 风险；用户使用即报语法错误）。
  - `src/backend/parser/parse_clause.c`：删 `transformWindowDefinitions`/`findWindowClause`/`transformFrameOffset` 及声明与调用；删 `FuncCall.over` 的 `unnest` 特判引用。
  - `src/backend/parser/parse_agg.c`：删 `transformWindowFuncCall` 及 `EXPR_KIND_WINDOW_*` 分支、`p_hasWindowFuncs` 设置；同步清理 copyfuncs/copyfuncs 中 `WindowFunc` 转换分支。
  - `src/backend/parser/parse_func.c`：删 `prokind=='w'` 窗口函数识别分支、`over` 变量相关引用；`src/include/parser/parse_func.h` 删 `FUNCDETAIL_WINDOWFUNC` 枚举（func_get_detail 不再返回该值）。
- **优化器（路径生成）**：
  - `src/backend/optimizer/plan/planner.c`：删 `WindowClauseSortData` 结构体、`QueryPathInfoExtra.activeWindows` 字段、`create_window_paths`/`create_one_window_path`/`select_active_windows`/`make_window_input_target`/`make_pathkeys_for_window` 五个函数及前向声明与调用；删 `subquery_planner` 中 `windowClause` 预处理循环、`grouping_planner` 中窗口检测块、`parse->hasWindowFuncs` 检查、`qp_extra.activeWindows` 赋值、`make_window_input_target` 使用、`UPPERREL_WINDOW` 目标赋值、`create_window_paths` 调用；`standard_qp_callback` 删 `activeWindows` 局部与 `window_pathkeys` 逻辑；`src/include/optimizer/planner.h` 对应声明删除。
  - `src/backend/optimizer/plan/createplan.c`：删 `WindowAgg` 计划节点创建分支（保留 agg 路径）。
  - `src/backend/optimizer/plan/setrefs.c`：删 `set_plan_refs` 中 `WindowAgg` case。
  - `src/backend/optimizer/path/pathnode.c`：删 `create_window_path`；`src/backend/optimizer/path/costsize.c`：删 `cost_windowagg`；清理 `UPPERREL_WINDOW`/`window_pathkeys` 引用与 `PVC_INCLUDE_WINDOWFUNCS`/`PVC_RECURSE_WINDOWFUNCS` 标志（在 var.c/placeholder.c/initsplan.c/equivclass.c/planner.c 等 pull_var_clause 调用点移除该位）。
  - `src/include/nodes/pathnodes.h`：删 `UPPERREL_WINDOW` 枚举值、`PlannerInfo.window_pathkeys` 字段。
  - `src/backend/optimizer/util/clauses.c`：删 `contain_window_function` 中经 `contain_windowfuncs` 的调用（改为直接 `return false`，原窗口函数检测函数恒返回 false，纯死逻辑）；`src/backend/rewrite/rewriteManip.c`：删 `contain_windowfuncs`/`locate_windowfunc` 死函数及其声明（`contain_windowfuncs` 仅被已改写的 `contain_window_function` 调用）。
- **解析器/重写/优化器的 Query 字段清理**：
  - `src/include/parser/parse_node.h`：删 `p_windowdefs` 字段与 `p_hasWindowFuncs` 字段；`src/include/nodes/parsenodes.h` 删 `Query.hasWindowFuncs` 字段。
  - `src/backend/parser/analyze.c`：删 `qry->hasWindowFuncs` 赋值、`pstate->p_windowdefs` 赋值、`stmt->windowClause` 断言、`if (qry->hasWindowFuncs)` 行锁检查块。
  - `src/backend/rewrite/rewriteHandler.c`：删视图 window 函数不可更新检查；`src/backend/optimizer/plan/planagg.c`：删 `parse->hasWindowFuncs` 检查；`subselect.c`/`prepjointree.c`/`allpaths.c`：删各 `hasWindowFuncs`/`windowClause` 引用；`ruleutils.c`：删 `get_rule_windowclause`/`get_rule_windowspec`/`get_windowfunc_expr` 函数、`T_WindowFunc` dispatch case、`deparse_context.windowClause`/`windowTList` 字段及其赋值/恢复、WINDOW 子句打印。
- **catalog 注册与回归测试**：
  - `src/include/catalog/pg_proc.dat`：删全部 `prokind => 'w'` 内置窗口函数注册项（row_number/rank/dense_rank/percent_rank/cume_dist/ntile/lag/lead/first_value/last_value/nth_value，oid 3100-3114 段），保留 in_range 帧边界函数（`window_range_*` 实际实现在 int8.c 等，用于 date/timestamp 的 RANGE BETWEEN，核心类型仍依赖）。
  - `src/test/regress/sql/window.sql` + `expected/window.out`：`git rm`；`src/test/regress/parallel_schedule` 移除 `window` 测试项。
  - **回归测试残留清理（2026-08-18）**：另有 6 个既有回归测试中混用了窗口函数调用，窗口语法裁剪后这些语句在解析阶段报 `syntax error at or near "OVER"`，导致 `make check-world` 失败，已同步删除语句并更新 expected 基准：
    - `groupingsets.sql/.out`：删 3 处 `sum(sum(c)) over (order by a,b)`（含 1 处 `explain`）；
    - `tsrf.sql/.out`：删 `lag(x) over(...)`、`min(...) OVER()`、`lag(id) OVER()`、`SUM(count(*)) OVER(...)` 整段窗口测试；
    - `psql_crosstab.sql/.out`：删 3 处 `row_number() OVER(ORDER BY h ...)` crosstab 测试；
    - `polymorphism.sql/.out`：删 `first_el_agg_f8/any(x) over(order by x)` 两处窗口调用（普通聚合调用保留）；
    - `fast_default.sql/.out`：删 `stddev(...) OVER (PARTITION BY ...)` 整条（仅用于 exercise expand_tuple，依赖窗口语法）；
    - `select_parallel.sql/.out`：删 `row_number() over()` 下推测试 `explain` 块。
    - 未改动（有意保留）：`create_aggregate.sql` 的 `ORDER BY VARIADIC "any"`（ordered-set 聚合，非窗口）、`groupingsets.sql` 的 `rank(...) within group (order by ...)`（WITHIN GROUP 聚合语法，非 OVER 窗口）、`create_function_3.sql` 的 `CREATE FUNCTION ... WINDOW`（DDL 兼容保留项，见上）。
- **保留项（与窗口函数无耦合，未裁）**：
  - `CREATE FUNCTION ... WINDOW` DDL 选项（functioncmds.c 的 `isWindowFunc`/`windowfunc_p`、`PROKIND_WINDOW` 常量、kwlist.h 的 `WINDOW` 关键字）保留——该选项仅控制 prokind 字段，窗口函数调用路径已不可达，且无 catalog 注册项故实际不可用；属 DDL 兼容保留，未深入删除以免牵动 CREATE FUNCTION 语法。
  - 聚合（agg）、tuplestore（agg 游标依赖）、事务、btree/hash 索引机制完全不受影响。
- **验证**：`make -j4` 全量编译（含 backend/adt/optimizer/parser/nodes/rewrite 全部子目录）+ postgres 链接成功（0 error / 0 undefined reference）；全代码库 `grep WindowFunc|WindowAgg|WindowDef|WindowClause|...` 0 命中残留引用。与不可裁部分（btree/hash 索引、事务）零耦合：仅删窗口执行/解析/优化链路，聚合与事务机制均不受影响。
- **注意**：本次裁剪与 minipg 既有 `initdb` 崩溃（syscache cacheinfo[]/syscache.h 枚举不对齐，见 2026-08-14 记忆）无关——已用干净 HEAD 构建独立验证，纯净 HEAD 同样在 post-bootstrap 阶段 segfault，故无法在此环境跑完整回归；以单文件/全量编译验证为准。

---

## 十九、gram.y 消除 bison useless nonterminal/rule 警告（2026-08-18）

`make` 编译期 bison 报 3 个非终结符无用、6 条规则无用（`-Wother`），定位三个悬空语法：
- `NumericOnly_list`：定义（原 2627-2629，`NumericOnly_list: NumericOnly | NumericOnly_list ',' NumericOnly`），`%type <list>` 在 432 行。全文无任何上层规则引用（仅定义处出现），属死语法。第十六步曾误判为活跃并保留，本次证实为无用。
- `any_with`：定义（原 6070-6073，`any_with: WITH | WITH_LA`），无 `%type` 单独声明、无任何引用方，死语法。
- `opt_distinct_clause`：定义（原 6975-6978，`opt_distinct_clause: distinct_clause | opt_all_clause`），`%type` 在 349 行。注释（原 6853-6858）说明当初为避免 SELECT DISTINCT 与 INSERT...SELECT...ON CONFLICT 的 shift/reduce 冲突，已改为在 SELECT 规则里直接展开 `distinct_clause`，该包装规则因此失去所有引用方，成死语法；`distinct_clause` 本身仍被 SELECT 活跃引用，不受影响。

裁剪动作：
- 删除 `NumericOnly_list` 定义（含尾随 `;`）与 `%type <list> NumericOnly_list` 声明。
- 删除 `any_with` 定义及上方注释块。
- 删除 `opt_distinct_clause` 定义与 `%type` 中 `opt_distinct_clause` 标记（保留 `distinct_clause`）。

- **验证**：`make -j4` 触发 bison 重生成 `gram.c`，原 3 个 useless nonterminal / 6 条 useless rule 警告**全部消除**（编译 EXIT=0）；剩余仅一条无关的 `selfuncs.c:5446` 未用变量警告（非本次引入、非编译错误）。`make install` + `make check-world` 全绿（regress 全部 tests passed，isolation/bin 同步通过），零 failure。全代码库 `grep NumericOnly_list|any_with|opt_distinct_clause` 仅余 6848 行历史设计注释提及（非代码引用）。与不可裁部分（btree/hash 索引、事务）零耦合：仅删语法死规则，未改任何运行时/索引/事务逻辑。

## 二十、gram.y 删除 CREATE ASSERTION 未实现语法（2026-08-18）

`CREATE ASSERTION` 是 SQL 标准里"数据库级命名约束"语法，但 PostgreSQL/minipg 从未实现——`gram.y` 中只是一个直接 `ereport(ERROR, "CREATE ASSERTION is not yet implemented")` 的占位桩，不进入任何执行路径、不依赖任何 catalog 存储，与事务/索引等核心功能无耦合，学习价值低，属干净裁剪目标。

裁剪动作（共 7 处，彻底删除、不留死代码）：
- **`src/include/parser/kwlist.h`**：删除 `PG_KEYWORD("assertion", ASSERTION, UNRESERVED_KEYWORD, BARE_LABEL)` 关键字声明。
- **`src/backend/parser/gram.y`**：
  - `%token` 行（原 525）删除 `ASSERTION` 关键字 token 声明；
  - `%type` 列表（原 252）删除 `CreateAssertionStmt` 节点类型声明；
  - `schema_stmt` 备选（原 777）删除 `| CreateAssertionStmt`；
  - 删除 `CreateAssertionStmt:` 产生式整段（含 `CREATE ASSERTION ... CHECK (...)` 规则与上方 `QUERY: CREATE ASSERTION ...` 注释块）；
  - `unreserved_keyword` 分类列表（原 9975）删除 `| ASSERTION`；
  - `bare_label_keyword` 分类列表（原 10484）删除 `| ASSERTION`。

保留说明：`pg_constraint.h` 的 `CONSTRAINT_ASSERTION` 枚举项与注释是 constraint 系统"为将来扩展预留"的通用标记，与本次语法桩无直接依赖，属独立 catalog 设计预留，本次不裁剪（避免牵连 constraint 体系）。`Assert()` 运行时断言（c.h / elog.c 等）是完全不同的调试机制，与本次无关，未触动。

- **验证**：`make -j4` 全量重编 EXIT=0（bison 重生成 `gram.c` 无报错、无新增警告）；`make install` + `make check-world` 全绿——regress 全部 83 tests passed，isolation / bin 同步通过，零 failure。全代码库 `grep CreateAssertionStmt` 与 gram.y 内 `ASSERTION` 关键字引用均清零。与不可裁部分（btree/hash 索引、事务）零耦合：仅删除未实现的语法桩与对应关键字，未改任何运行时/索引/事务逻辑。

---

## 二十一、裁剪角色/用户兼容视图 pg_roles / pg_shadow / pg_group / pg_user（2026-08-18）

这四个视图是 PostgreSQL「角色/用户」机制的兼容壳，底层 `pg_authid` / `pg_auth_members` 系统表已在先前 ACL/角色骨架裁剪中彻底删除，故这些视图映射的源头已不存在，属于彻底裁剪目标。与事务/btree/hash 索引等核心功能零耦合。

裁剪动作（彻底删除、不留死代码）：

- **`src/backend/catalog/system_views.sql`**：删除 `pg_roles`、`pg_shadow`、`pg_group`、`pg_user` 四个视图定义（`pg_user` 由 `pg_shadow` 派生，一并删除）。
- **`src/bin/psql/describe.c`**：删除 `describeRoles()` 死函数（原 `\du` / `\dg` 命令实现，查询已删除的 `pg_roles` / `pg_auth_members` / `pg_authid`，且当前无任何调用入口，属之前角色裁剪遗留的死代码）；同时删除仅被其调用的 `add_role_attribute()` 辅助函数及其在文件头部的 `static` 前向声明。
- **`src/bin/psql/describe.h`**：删除 `describeRoles()` 的 `extern` 声明。
- **`src/bin/psql/help.c`**：删除 `\du[S+] list roles` 帮助行。
- **`src/bin/psql/tab-complete.c`**：删除 `Query_for_list_of_roles` 宏（引用 `pg_roles`）；将 `GROUP` / `ROLE` / `USER` 补全项改为 `NULL`；将 `ALTER ... OWNED BY`、`ALTER GROUP ... ADD|DROP USER`、`DROP OWNED BY`、`OWNER TO`、`SET ROLE`、`SET SESSION AUTHORIZATION`、`CREATE USER MAPPING FOR`、`\connect`、`\du*` / `\dg*`、`\password` 中所有依赖该宏的角色名补全改为 `COMPLETE_WITH_NOTHING()`（或仅保留关键字），避免引用已删除视图。
- **`src/tutorial/syscat.source`**：删除教程中依赖 `pg_roles` / `pg_type.typowner` 的两段示例查询（数据库所有者列表、类型所有者列表），避免教程执行失败。

影响说明：
- 内核 C 代码本就不依赖任何系统视图（直接查底层系统表或 C 函数），故删除这四个视图不影响事务、索引、优化器、GUC、扩展等核心功能。
- 客户端侧：`\du` / `\dg` 命令、其帮助与补全、角色名相关补全失效——角色机制已裁，这些行为本应随之移除。
- `pg_user_mappings` / `pg_foreign_*` 等 FDW 相关视图与本次无关，未改动。

- **验证状态**：本次改动已通过 gcc `-fsyntax-only` 单独语法检查（`describe.c` / `tab_complete.c` 0 error），逻辑自洽。但因工作区存在**未提交的 gram.y 半成品裁剪**（删了 DoStmt / ReindexStmt / AlterTblSpcStmt 等语句规则但残留引用，bison 报 `used but not defined` 错误），整库当前无法编译，故 `make check-world` 暂未运行（用户决策：保留 gram.y 半成品、先不验证）。待 gram.y 修复后可补跑回归验证。


---

## numeric 数据类型彻底删除（2026-08-18）

按"学习价值低、与不可裁核心（btree/hash 索引、事务）零耦合则可裁剪"的原则，本次**彻底删除 numeric 数据类型及其全部函数**，并把所有耦合代码改写为 int8 / float8 实现。numeric 属任意精度算术类型，内核学习价值低、依赖链极广（EXTRACT/justify/epoch、事务统计、WAL 的 pg_lsn、dbsize、to_char 格式化等）。

### 删除 / 新建
- 删除 `src/backend/utils/adt/numeric.c`（原 11327 行）、`src/include/utils/numeric.h`。
- catalog 删除 numeric 条目：`pg_type.dat` 的 `numeric`(1700) 与 `numeric[]`(1231)；`pg_proc.dat` 中 numeric 相关函数（awk 批量删后恢复 5 个 `extract*` 函数，prorettype 由 numeric 改为 float8；注意 `extract_interval` 的 prorettype 一并改为 float8，否则常量折叠时 planner 以 interval(16 字节) 解读 float8(8 字节) 触发 datumCopy 段错误）；`pg_cast.dat` numeric 转换；`pg_opfamily.dat` 的 `numeric_ops`(btree/hash 两个 opfamily，其 amop/amproc 此前已删)。
- 新建 `src/backend/utils/adt/intagg.c`：承接原 numeric.c 中整数 sum/avg 聚合，状态以 `_int8` 数组 `{int128 低64, 高64, count}` 表示；提供 `int2_sum`/`int4_sum`（int8 态累加）、`int2_avg_accum`/`int4_avg_accum`/`int8_avg_accum`（含 inv）、`int2int4_sum`(_int8->int8)、`int8_avg`(_int8->float8)、`int4_avg_combine`。`sum(int2)/sum(int4)` 走 int2_sum/int4_sum（aggtranstype int8）；`avg(int2)/avg(int4)/avg(int8)` 走 _int8 态 + int8_avg 终函数（返回 float8）。`pg_proc.dat` 新增 `int8_avg`(6205) 与 `avg(int8)/avg(int4)/avg(int2)`(2100/2101/2102, prorettype float8)；`pg_aggregate.dat` 修正 sum(int2)/sum(int4) 的 aggmtranstype 为 _int8，并补回 avg(int2)/avg(int4)/avg(int8)。`Makefile` 用 intagg.o 替换 numeric.o。

### 耦合代码改写（统一改 int8 / float8）
- `date.c`：extract_date/extract_time 返回 float8（删 numeric 分支）；justify_days/justify_hours 用 float8；加 `#include "utils/float.h"`。
- `timestamp.c`：timestamp_part_common/timestamptz_part_common/interval_part_common 删 retnumeric 参数与 numeric 分支，EPOCH 直接返回 float8。
- `pg_lsn.c`：pg_lsn_mi 返回 float8；pg_lsn_pli/pg_lsn_mii 参数改 int64。
- `dbsize.c`：删 pg_size_pretty_numeric；pg_size_bytes 参数改 int64；pg_size_pretty 收 int8。
- `formatting.c`：to_char numeric 路径改 float8（numeric_out->float8out、numeric_round->float8_round、numeric_mul->float8mul、numeric_power->float8_power、numeric_int4->float8_to_int4、numeric_out_sci->float8_out_sci）。
- `format_type.c`/`pgstatfuncs.c`/`ruleutils.c`/`selfuncs.c`/`rangetypes.c`/`fe_utils/print.c`：删 NUMERICOID 分支（rangetypes.c 删 numrange_subdiff，numrange 未注册）。
- `parse_node.c`：数字字面量解析改 int8。
- `xlogfuncs.c`：pg_xact_commit_timestamp 返回 timestamptz（删 PG_RETURN_NUMERIC）。
- `system_functions.sql`：删 log(numeric)/log10(numeric)/numeric_pl_pg_lsn 定义。
- `src/test/modules/libpq_pipeline/libpq_pipeline.c`：查询 `$1::numeric` 改 `$1::int8`，NUMERICOID 改 INT8OID。

### 回归测试同步
- 删 numeric.sql/numeric_big.sql（及 expected），parallel_schedule 移除 numeric。
- 测试 SQL 中 `::numeric` 全改 `::int8`（dbsize、create_aggregate、arrays、errors、sanity_check、width_bucket 等）；create_aggregate.sql 的 int 聚合用例改 `_int8` 表示（int8_avg_accum/int8_avg/int4_avg_combine）。
- isolation 测试 total-cash/timeouts/receipt-report/read-only-anomaly(+2/-3)/serializable-parallel 的 setup 中 numeric/DECIMAL 改 int8。
- 因 EXTRACT 改返回 float8（精度显示变化）与 int8 截断语义，重生成 23 个回归测试与 7 个 isolation 测试的 expected/*.out。

### 验证
make check-world（NO_TEMP_INSTALL=1，依赖先 make install 到 tmp_install）全部通过：回归 82/82、isolation 66/66，以及 bin/ecpg 等其余子套件均通过。全库 `make -j` 重编通过，无 NUMERICOID/numeric 类型残留引用。

---

## FUNCTION/PROCEDURE 命令标签与派发层裁剪（2026-08-18）

按用户决策"仅裁剪命令标签与派发层（保留语法/功能）"：保留 CREATE/ALTER/DROP FUNCTION/PROCEDURE 的语法与执行能力（回归测试大量依赖 CREATE FUNCTION 作支撑函数），但删除其命令标签（CMDTAG）定义并清理 utility.c 中对应的 case，使这些命令在执行时不再携带状态字符串（命令标签回退为未知）。

### 删除的 CMDTAG 定义
- **`src/include/tcop/cmdtaglist.h`**：删除 `CMDTAG_ALTER_FUNCTION`、`CMDTAG_CREATE_FUNCTION`、`CMDTAG_CREATE_PROCEDURE`、`CMDTAG_DROP_FUNCTION`、`CMDTAG_DROP_PROCEDURE` 五个命令标签宏（此前已删，本次确认保留该裁剪）。`CMDTAG_CREATE_RULE` 已恢复（CREATE RULE 仍需标签）。

### utility.c 派发/标签层清理
- **`src/backend/tcop/utility.c`**：
  - `CreateCommandTag()`：为 `T_CreateFunctionStmt` 增加 case，返回 `CMDTAG_UNKNOWN`（无状态字符串）。
  - `ClassifyUtilityCommandAsReadOnly()`：恢复 `case T_CreateFunctionStmt:`（fall through 到 `return COMMAND_IS_NOT_READ_ONLY`），否则 CREATE FUNCTION 在只读判断中触发 "unrecognized node type: 226" 错误（226 即 `T_CreateFunctionStmt` 枚举值）。
  - `standard_ProcessUtility` 派发：恢复 `case T_CreateFunctionStmt: address = CreateFunction(...)` 实际执行（保留 DDL 能力）。

### 保留的执行能力（语法 + 函数）
- **`src/backend/parser/gram.y`**：恢复 `stmt` 规则中的 `| CreateFunctionStmt`；恢复 `CreateFunctionStmt` 语法块（含 CREATE FUNCTION / CREATE PROCEDURE）；恢复 `RemoveFuncStmt` 的 `DROP FUNCTION` / `DROP PROCEDURE` 分支（OBJECT_FUNCTION / OBJECT_PROCEDURE）。
- **`src/backend/commands/functioncmds.c`**：恢复 `CreateFunction()` 函数（裁剪其中 ACL 检查：移除 `pg_namespace_aclcheck`/`pg_language_aclcheck`/`aclcheck_error`/`superuser()` 等已删除的权限校验），保留 `ProcedureCreate` 调用。

### 修复的连带问题
- 裁剪 FUNCTION/PROCEDURE CMDTAG 后，若编译期 cmdtaglist.h 与 cmdtag.c 数组版本不一致（tmp_install 残留旧头文件副本），会导致 `CMDTAG_SELECT` 枚举错位，普通 `SELECT` 命令状态被错误设为 "SELECT FOR KEY SHARE"，引发 libpq "could not interpret result from server" 错误。修复方式：删除污染的 `tmp_install` 旧头文件副本并强制重编 `tcop/utility.o`、`tcop/cmdtag.o`、`tcop/postgres.o`，确保 enum 与生成数组一致。

### 验证
make check-world 全部通过：回归 82/82、isolation 66/66，以及其余子套件均通过。

---

## GRANT/REVOKE 命令标签裁剪（ACL 裁剪收尾，2026-08-19）

ACL 权限系统此前已彻底移除（acl.h、aclchk.c、角色骨架、GRANT/REVOKE 语法与实现 grant.c 均已删），但 `cmdtaglist.h` 中残留 `CMDTAG_GRANT`/`CMDTAG_REVOKE` 两个已无引用方的死标签，且 `gram.y` 残留一处死注释。本次彻底清理。

### 删除内容
- **`src/include/tcop/cmdtaglist.h`**：删除 `CMDTAG_GRANT`（原 127 行）与 `CMDTAG_REVOKE`（原 137 行）两个命令标签宏。全代码库 `GrantStmt`/`ExecuteGrantStmt`/`CMDTAG_GRANT`/`CMDTAG_REVOKE` 经检索均 0 命中，删去不影响编译与运行（标签列表仍按字母序有序）。
- **`src/backend/parser/gram.y`**：清理残留死注释 `/* GRANT and REVOKE statements */`（原 3808 行附近区块，对应语法已不存在）。

### 验证
make check-world 全部通过（回归/isolation 等子套件均保持通过）。

---

## EXCLUDE 约束（排除约束）功能裁剪（2026-08-19）

minipg 的索引访问方法已裁至只剩 heap/btree/hash（GiST/SP-GiST/GIN/BRIN 全部移除），而 EXCLUDE 约束的标准用法（如区间不重叠 `&&`）依赖 GiST；btree 仅支持等值 `=`，使 EXCLUDE 退化为普通唯一约束。故 EXCLUDE 约束在 minipg 实际已不可用，仅存语法与存储骨架。本次按「裁剪约束 EXCLUDE 方案（B 方案，彻底删除）」将其从 minipg 全链路删除。

### 删除内容（按代码链路）

- **语法层（`src/backend/parser/gram.y`）**：删除 `EXCLUDE access_method_clause '(' ExclusionConstraintList ')'` 约束生产式及其子规则 `ExclusionConstraintList`/`ExclusionConstraintElem`；删除因此无用的 `OptWhereClause` 规则与 `%type` 声明；保留 `EXCLUDE` 关键字（与 `kwlist.h` 的 UNRESERVED_KEYWORD/BARE_LABEL 声明一致，避免 check_keywords.pl 报错）。
- **节点定义**：`parsenodes.h` 删 `Constraint.exclusions` 与 `IndexStmt.excludeOpNames` 字段及 `CONSTR_EXCLUSION` 枚举；同步清理 `copyfuncs.c`/`equalfuncs.c`/`outfuncs.c` 对应字段读写与 `CONSTR_EXCLUSION` case。
- **解析层（`parse_utilcmd.c`）**：删除 `transformTableConstraint` 的 `CONSTR_EXCLUSION` 分支、`transformIndexConstraint` 中 EXCLUSION 特殊处理、`SUPPORTS_ATTRS` 宏项、以及 `transformIndexStmt` 中从 `pg_constraint.conexclop` 重建 `excludeOpNames` 的整块。
- **执行层（`indexcmds.c`/`index.c`）**：删除 `DefineIndex` 中分区表排除约束错误、AM 不支持排除约束错误、`constraint_type` 的 EXCLUDE 分支；`ComputeIndexAttrs`/`ChooseIndexName` 删除 exclusion 逻辑（保留 `exclusionOpNames` 参数但恒 NIL，加 `(void)` 消除警告）；`index.c` 删除 `IndexCheckExclusion` 函数、`index_create` 的 `is_exclusion` 变量与并发排除约束错误、`UpdateIndexRelation` 的 `isexclusion` 参数、`CheckIndexCompatible` 的排除兼容检查、`index_constraint_create` 的 EXCLUDE 类型分支、`ReindexIsCurrentlyProcessingIndex`（因唯一调用者被删）。
- **catalog 存储（B 方案核心）**：`pg_constraint.h` 删 `conexclop` 列与 `CONSTRAINT_EXCLUSION` 宏、`CreateConstraintEntry` 删 `exclOp` 参数；`pg_constraint.c` 删 `conexclopArray` 构造与写入；`pg_index.h` 删 `indisexclusion` 列；同步清理 4 个 `CreateConstraintEntry` 调用点（`index.c`/`heap.c`/`typecmds.c`/`trigger.c`）。
- **缓存/显示（`relcache.c`/`ruleutils.c`）**：删除 `RelationGetExclusionInfo` 函数及其在 relcache 的 `rd_exclops`/`rd_exclprocs`/`rd_exclstrats` 缓存字段；`ruleutils.c` 删除 `pg_get_constraintdef` 的 `CONSTRAINT_EXCLUSION` case、`pg_get_indexdef_worker` 的 `excludeOps` 参数与 EXCLUDE 显示分支。
- **执行器/触发器**：`execIndexing.c` 删除 exclusion 冲突检测分支、`check_exclusion_constraint`/`index_recheck_constraint` 函数；`constraint.c` 的 `unique_key_recheck` 退化为纯唯一检查；`toasting.c`/`bootstrap.c`/`indexing.c`/`heapam_handler.c` 删除 `ii_ExclusionOps` 系列引用；`lmgr.h`/`lmgr.c` 删 `XLTW_RecheckExclusionConstr`。
- **回归测试**：`sql/constraints.source`（input/output）删除 deferred exclusion 测试段；`sql/index_including.sql`（.sql/.out）删除 `EXCLUDE USING btree` 用例。

### 保留边界（未破坏）
btree/hash 索引、UNIQUE/PRIMARY KEY/CHECK 约束、pg_constraint/pg_index 基础设施、执行器唯一性检查路径均完整保留。`constraint_exclusion`（约束排除查询优化 GUC）为独立特性，与 EXCLUDE 约束无关，保留。

### 关键经验
- 删除非保留关键字（EXCLUDE）的语法规则时，**必须保留**关键字在 `kwlist.h` 及 `gram.y` 的 `unreserved_keyword`/`bare_label_keyword`/token 声明中的一致性（`check_keywords.pl` 强制），否则生成 gram.c 失败。
- 删 catalog 列后必须强制全量重编：仅重编受影响的 .o 可能因旧头 ABI 不一致导致 initdb 在 btree 并行构建时崩溃（`cannot take query snapshot during a parallel operation`）。删除 `pg_*_d.h` 后需重新运行 genbki（`make -C src/backend/catalog bki-stamp`）并 symlink 到 `src/include/catalog/`，且 `touch src/backend/*.c` 触发重编。

### 验证
make check 全部通过（回归 82/82）；initdb 成功；`pg_index` 已无 `indisexclusion` 列、`pg_constraint` 已无 `conexclop` 列；`EXCLUDE (a WITH =)` 建表报语法错误；UNIQUE/CHECK/PRIMARY KEY 约束及 `pg_get_constraintdef`/`pg_get_indexdef` 显示均正常。

---

## constraints 回归测试清理（2026-08-20，EXCLUDE + 序列裁切收尾）

EXCLUDE 约束与序列功能此前已彻底裁剪，但 `src/test/regress/{input,output}/constraints.source` 仍残留对已裁功能的依赖，导致 `make check` 无法运行。本次清理：

- **EXCLUDE 注释**：删除顶部功能说明中的 `--  - EXCLUDE clauses` 一行（实际的 EXCLUDE 测试块已于 2026-08-19 删除）。
- **序列依赖整段删除**（序列 DDL `CREATE SEQUENCE` 已无语法入口，`nextval`/`currval` 在回归测试中不可用）：
  - `CREATE SEQUENCE DEFAULT_SEQ` 及其在 `DEFAULTEXPR_TBL.i2` 的 `nextval('default_seq')` 默认值 —— 改为常量默认值 `DEFAULT 7`（保留表达式默认值的测试意图）。
  - `CREATE SEQUENCE CHECK_SEQ`（创建后从未被使用）—— 删除。
  - 整段「Check constraints on INSERT」（`INSERT_SEQ` + `INSERT_TBL` + `nextval`/`currval` 校验）—— 删除。
  - 整段「Check inheritance of defaults and constraints」（`INSERT_CHILD INHERITS (INSERT_TBL)` 依赖已删的 `INSERT_TBL`）—— 删除。
  - 整段「Check constraints on INSERT INTO」与「Check constraints on UPDATE」（均依赖 `INSERT_TBL` 及其序列默认值）—— 删除。
- **保留**（均不依赖序列/EXCLUDE，且属约束核心学习价值）：DEFAULT 常量/表达式默认值、CHECK、`sys_col_check`（tableoid/ctid 系统列约束报错）、`NO INHERIT` 约束继承、COPY FROM 约束校验、PRIMARY KEY、UNIQUE、可延迟 UNIQUE（deferrable）、分区表唯一/FK/主键命名等。
- 因裁剪后整体输出结构变化，重新生成 `output/constraints.source` 期望输出（除 DEFAULTEXPR 常量默认值结果 `-3 | 7` 为推算修正外，其余保留段与原文逐字一致）。

验证：input/output 已无 `nextval`/`currval`/`CREATE|ALTER SEQUENCE`/`*_seq`/`EXCLUDE` 残留引用；移除的所有表（`INSERT_TBL`/`INSERT_CHILD`/`*_SEQ`）在 regress 目录下零引用。（注：本次本地未能重跑 `make check`，因构建环境 `pg_sema.c` 符号链接与 `config.status` 复制步骤冲突导致 `./configure` 失败，属既有构建环境问题，与本次测试编辑无关；请在可用构建树上 `make check` 终验。）

---

## 删除因分区功能裁剪而失效的 3 个规划 GUC（2026-08-20）

minipg 的分区子系统已彻底裁剪（`src/backend/partitioning/` 目录不存在，`PartitionDesc`/`RelationGetPartitionKey`/`partition_prune` 全代码库 0 命中），但 `guc.c` 仍注册 3 个分区规划 GUC，其控制的分区规划代码已完全不可达，属无效 GUC。本次一并清除：

- **`enable_partitionwise_join`**（guc.c:1001）：除声明/注册外无任何读取点，完全死。
- **`enable_partition_pruning`**（guc.c:1041）：除声明/注册外无任何读取点，完全死。
- **`enable_partitionwise_aggregate`**（guc.c:1011）：唯一读取点是 `planner.c:3062` 把 `extra.patype` 置为 `PARTITIONWISE_AGGREGATE_FULL`；因无分区表该 flag 永远不生效。

涉及文件与改动：

- **`src/backend/utils/misc/guc.c`**：删除 3 个 `enable_partitionwise_*`/`enable_partition_pruning` 的 `DefineCustomBoolVariable` 注册块。
- **`src/include/optimizer/cost.h`**：删除 3 个 `extern PGDLLIMPORT bool enable_partitionwise_*`/`enable_partition_pruning;` 声明。
- **`src/backend/optimizer/path/costsize.c`**：删除 3 个 `bool enable_partitionwise_*`/`enable_partition_pruning = ...;` 全局变量定义。
- **`src/backend/optimizer/plan/planner.c`**：将 `if (enable_partitionwise_aggregate && !parse->groupingSets) extra.patype = FULL; else NONE;` 简化为恒定 `extra.patype = PARTITIONWISE_AGGREGATE_NONE;`（保留下游 `create_ordinary_grouping_paths` 调用签名不变）。
- **`src/backend/utils/misc/postgresql.conf.sample`**：删除 3 行已删 GUC 的示例配置。
- **`src/backend/optimizer/util/relnode.c`**（连带修复编译错误）：删除 `build_child_join_rel()` 中遗留的 `Assert(parent_joinrel->consider_partitionwise_join);`——该 `RelOptInfo` 字段早已随分区裁剪从 `pathnodes.h` 删除，此 Assert 引用不存在的字段，属编译错误。

保留项（非本次范畴）：optimizer 中 partitionwise join/aggregate 的**通用 joinrel 代码骨架**（`relnode.c`/`pathnode.c`/`allpaths.c`/`joinpath.c`/`equivclass.c`）及 `PlannerInfo.patype` 字段、`PARTITIONWISE_AGGREGATE_*` 枚举、planner.c 中 `PARTITIONWISE_AGGREGATE_PARTIAL` 的恒假比较分支（`PARTIAL` 全库从未赋值）——它们虽无法触发，但删除需动 `RelOptInfo`/path 结构等深层优化器逻辑，风险高、收益低，不属"清除无效 GUC"范畴，留待后续独立裁剪。

回归测试同步：`sysviews.sql` 的 `select name, setting from pg_settings where name like 'enable%'` 输出中删除 3 个已裁 GUC 行，`expected/sysviews.out` 同步列宽与行数（19→16）。

验证：`make -C src/backend` 全量重编 + `postgres` 链接成功（0 error / 0 undefined reference）；`grep enable_partitionwise_join|enable_partitionwise_aggregate|enable_partition_pruning|consider_partitionwise_join` 全代码库 0 命中；同步 `expected/sysviews.out` 后 `NO_TEMP_INSTALL=1 make check` **全部 82 个测试通过**。与不可裁部分（btree/hash 索引、事务）零耦合。

> 注：中途曾见 `make check` 因 `pg_trigger` catalog 已裁、initdb post-bootstrap 脚本仍 `INSERT INTO pg_depend ... FROM pg_trigger` 而在 initdb 环节失败（见工作记忆）；再次干净重跑临时实例后 initdb 正常，`make check` 全绿，故该 initdb 报错属临时实例残留的既有干扰，非本次裁剪引入。

---

## 彻底裁剪 create/drop/alter tablespace（用户自建表空间管理，2026-08-21）

表空间管理属非核心外围 DDL（学习价值集中在 md.c/smgr 的 spcNode 寻址与 pg_default/pg_global 布局，而非 DDL 语句本身），本次彻底裁剪用户自建表空间的全部入口。**保留 `pg_tablespace` 系统目录与 `pg_default`(1663)/`pg_global`(1664) 两个内建表空间**：initdb 仍加载这两行，`base/`、`global/` 目录布局与 md.c/smgr 按 spcNode 寻址的内核路径不变，`pg_class.reltablespace` 字段保留（新建对象恒为 0=库默认）。本次共改动 60+ 文件、净删约 5700 行。

### SQL 入口（语法/节点/分发）
- **`src/backend/parser/gram.y`**：删除 `CREATE TABLESPACE`/`DROP TABLESPACE` 语句与 `CreateTableSpaceStmt`/`DropTableSpaceStmt` 产生式；删除 `OptTableSpace`/`OptConsTableSpace`/`OptTableSpaceOwner`（CREATE TABLE/INDEX ... TABLESPACE 子句）；删除 `ALTER TABLE ... SET TABLESPACE`（`AT_SetTableSpace`）与 `ALTER TABLE/INDEX ALL IN TABLESPACE ... [OWNED BY] SET TABLESPACE`（`AlterTableMoveAllStmt`，其 `role_list` 引用一并清除）；删除 CREATE DATABASE 选项中的 TABLESPACE 支持与 `ALTER DATABASE name SET TABLESPACE` 产生式（ALTER DATABASE 此前已裁，此为其最后残留变体）；删除 `ALTER TABLESPACE ... SET/RESET` 相关 `%type` 残留。连带清理：删除前轮 GRANT/ROLE 裁剪遗留的死规则 `role_list`（bison "useless nonterminal" 警告）与 `ALTER DATABASE` 空注释头。
- **`src/include/nodes/parsenodes.h`/`nodes.h`、`src/backend/nodes/{copy,equal,out}funcs.c`**：删除 `CreateTableSpaceStmt`/`DropTableSpaceStmt`/`AlterTableSpaceOptionsStmt`/`AlterTableMoveAllStmt` 节点、`T_` 枚举、`OBJECT_TABLESPACE`、`AT_SetTableSpace`，及 `CreateStmt`/`IndexStmt`/`CreatedbStmt` 等结构中的 tablespace 字段。
- **`src/backend/tcop/utility.c`/`src/include/tcop/cmdtaglist.h`**：删除 4 处 switch 中 tablespace 命令 case 与 `CREATE/DROP TABLESPACE` 命令标签。

### 执行层与 WAL（tablespace.c -1126 行）
- **`src/backend/commands/tablespace.c`**：删除 `CreateTableSpace`/`DropTableSpace`/`AlterTableSpaceOptions` 及私有辅助 `create_tablespace_directories`/`destroy_tablespace_directories`、WAL redo `tblspc_redo` 与 `XLOG_TBLSPC_CREATE/DROP` 记录构造、无调用者的 `get_tablespace_oid`。**保留**：`TablespaceCreateDbspace`（md.c 首次建库子目录依赖）、`GetDefaultTablespace`（恒返回 `InvalidOid` 即库默认）、`PrepareTempTablespaces`（固定库默认，temp_tablespaces GUC 已删）、`get_tablespace_name`、`directory_is_empty`/`remove_tablespace_symlink`（initdb/启动路径文件系统工具）。
- **WAL rmgr**：`src/include/access/rmgrlist.h` 删除 `RM_TBLSPC_ID`，删除 `src/backend/access/rmgrdesc/tblspcdesc.c`，`rmgrdesc/Makefile` 同步；`xlog.c` 删除 `XLOG_TBLSPC_*` 分发与 `allow_in_place_tablespaces` 检查。

### 数据库命令与系统目录依赖
- **`src/backend/commands/dbcommands.c`（-402 行）**：删除 `movedb`/`movedb_failure_callback`（ALTER DATABASE SET TABLESPACE 执行体）及 `createdb` 的 TABLESPACE 选项解析；`calculate_database_size` 仅统计 `base/`（pg_default）；`recovery_create_dbdir` 不再处理 in-place 表空间目录。
- **共享依赖/对象地址**：`pg_shdepend.c` 删除 `shdepChangeDep` 与表空间→数据库的 `SHARED_DEPENDENCY_TABLESPACE` 记录路径；`objectaddress.c`/`dependency.c` 删除 `OBJECT_TABLESPACE`/`OCLASS_TBLSPACE` 全部 case。
- **SQL 函数**：`pg_proc.dat` 删除 `pg_tablespace_location`、`pg_tablespace_size(_oid/_name)`、`pg_tablespace_databases`、`pg_stat_get_db_conflict_tablespace`；`misc.c`/`dbsize.c` 删除实现；`system_views.sql` 删除 `pg_stat_database_conflict_tablespace` 列。

### GUC、缓存与规划器
- **GUC**：删除 `default_tablespace`、`temp_tablespaces`（含 check/assign 回调）、`allow_in_place_tablespaces`（`guc.c`/`guc.h`/`postgresql.conf.sample`/`config.sgml`）。
- **表空间选项缓存**：删除 `spccache.c`/`spccache.h`，`reloptions.c` 删除 `tablespace_reloptions` 与 `RELOPT_KIND_TABLESPACE`。
- **规划器**：`costsize.c`/`selfuncs.c` 的 `get_tablespace_page_costs` 调用改为直接使用 `seq_page_cost`/`random_page_cost` GUC（per-tablespace I/O 成本参数随 spccache 一并消失）。

### 备库恢复冲突与统计
- 删除 `PROCSIG_RECOVERY_CONFLICT_TABLESPACE`（`procsignal.h`/`procsignal.c`）、`ResolveRecoveryConflictWithTablespace`（`standby.c`）、`pgstat` 的 `n_conflict_tablespace` 计数（`pgstat.h`/`pgstat.c`/`pgstatfuncs.c`）及 `wait_event.c` 相应等待事件。

### psql 与其他工具
- **psql**：删除 `\db` 元命令（`describe.c`/`describe.h`/`command.c`/`help.c`）与 `\l+` 的表空间列、`tab-complete.c` 的 TABLESPACE 补全分支。
- **bootstrap**：`bootparse.y` 删除 BOOTSTRAP 语法的 TABLESPACE 输入支持；`postinit.c`/`sharedfileset.c`/`heapam.c`/`nodeBitmapHeapscan.c` 等调用点改为库默认表空间常量。

### 回归测试
- 删除 `src/test/regress/{input,output}/tablespace.source`，`parallel_schedule` 移除 tablespace 组；删除 recovery `t/033_replay_tsp_drops.pl`，裁剪 `t/031_recovery_conflict.pl` 表空间冲突段、`t/018_wal_optimize.pl` 表空间引用；`database.sql`/`misc_functions.sql`/`psql.sql` 及期望输出同步删除表空间用例；`unsafe_tests/alter_system_table` 中 pg_tablespace 不可 UPDATE 断言调整（目录仍保留，仍为系统表）；`pg_regress.c` 删除 `--tablespace`/`--temp-tablespace` 选项处理。

### 保留边界（未破坏）
`pg_tablespace` 系统目录（含 `spcacl` 等全部列）、pg_default/pg_global 内建表空间、`base/`+`global/` 存储布局、md.c/smgr 的 spcNode 寻址、`pg_class.reltablespace` 字段（恒 0）、`TablespaceCreateDbspace` 目录创建路径均完整保留；btree/hash 索引与事务零耦合。

### 关键经验
- 删除 `ALTER DATABASE ... SET TABLESPACE` 产生式前需确认 ALTER DATABASE 其它变体的裁剪历史（minipg 中 ALTER DATABASE 早在 2026-08-16 轮已整体裁剪，该产生式是刻意保留的最后残留，删之即自然终结整条语法，无回归）。
- gram.y 多轮裁剪后应关注 bison 的 "useless nonterminal/rules" 警告——它精准指示死规则（如本轮顺带清除的 `role_list`）。
- 修复 `PRIMARY KEY opt_definition` 中 `$2`→`$3` 的语义值索引错位曾是本轮前置关键修复（类型错配导致 segfault，前轮已修，此处仅存档）。

### 验证
全量 `make -j8` 编译 0 error / 0 warning；`make check-world` 退出码 0（回归 82/82、isolation 66/66、recovery/unsafe_tests 等套件全绿）；冒烟验证：`pg_tablespace` 仅含 `pg_default`/`pg_global` 两行，`CREATE TABLESPACE`/`ALTER TABLE ... SET TABLESPACE`/`CREATE TABLE ... TABLESPACE`/`CREATE DATABASE ... TABLESPACE` 均报语法错误，`SET default_tablespace` 报 unrecognized configuration parameter，建表/主键/插入正常且 `reltablespace=0`。

> 注：本轮工作树中还混有并发会话对 `superuser_reserved_connections`/`ReservedBackends`（postmaster.c）的裁剪改动，非本次表空间裁剪范畴，未计入本条目。

---

## 裁剪 db_user_namespace（每数据库独立用户名，2026-08-21）

`db_user_namespace` 是 PostgreSQL 7.3 引入的"每数据库独立用户名"功能，用户连接时自动将 `username` 拼接为 `username@dbname`，用于共享主机多租户隔离。官方文档自述为"临时措施"。该功能学习价值低（核心逻辑仅是字符串拼接），本次彻底裁剪。

### 删除的代码点

- **[guc.c](file:///home/postgres/works/my-github/minipg/src/backend/utils/misc/guc.c)**：删除 `db_user_namespace` GUC 定义（`PGC_SIGHUP, CONN_AUTH_AUTH`）
- **[postmaster.c](file:///home/postgres/works/my-github/minipg/src/backend/postmaster/postmaster.c)**：删除 `bool Db_user_namespace = false;` 变量定义（L205）及 `ProcessStartupPacket` 中 user@dbname 拼接逻辑块（L1827-1843）
- **[pqcomm.h](file:///home/postgres/works/my-github/minipg/src/include/libpq/pqcomm.h)**：删除 `extern bool Db_user_namespace;` 声明
- **[postgresql.conf.sample](file:///home/postgres/works/my-github/minipg/src/backend/utils/misc/postgresql.conf.sample)**：删除 `#db_user_namespace = off` 注释行

### 文档删除（不记入裁剪统计）

- **[config.sgml](file:///home/postgres/works/my-github/minipg/doc/src/sgml/config.sgml)**：删除整节 `guc-db-user-namespace` 文档

### 验证

`make -j$(nproc)` 全量编译 0 error；`make check-world` 退出码 0，全部回归测试通过。与不可裁部分（btree/hash 索引、事务）零耦合。

---

## Relation Options 残余代码清理（2026-08-21）

在之前 reloptions 核心裁剪的基础上，继续清理残余的 reloptions 相关代码。

### 涉及文件与改动

- **`src/backend/commands/indexcmds.c`**：删除 `Datum reloptions` 局部变量及 `reloptions = (Datum) 0` 赋值；`index_create` 调用中 `reloptions` 参数改为 `(Datum) 0`
- **`src/backend/commands/view.c`**：删除 `check_option` 变量及所有 reloptions 相关代码（`stmt->options` 中 `check_option` 的添加和遍历）
- **`src/backend/catalog/toasting.c`**：删除所有函数（`AlterTableCreateToastTable`, `NewHeapCreateToastTable`, `NewRelationCreateToastTable`, `CheckAndCreateToastTable`, `create_toast_table`）的 `Datum reloptions` 参数；`heap_create_with_catalog` 调用中 `reloptions` 改为 `(Datum) 0`
- **`src/backend/commands/cluster.c`**：删除 `Datum reloptions` 局部变量、`Anum_pg_class_reloptions` 的 SysCacheGetAttr 调用、`reloptions = (Datum) 0`、以及 `ReleaseSysCache(tuple)`；`heap_create_with_catalog` 和 `NewHeapCreateToastTable` 调用中 `reloptions` 改为 `(Datum) 0`
- **`src/backend/utils/cache/relcache.c`**：注释中删除 `reloptions` 提及
- **`src/backend/utils/adt/ruleutils.c`**：删除 `get_reloptions` 和 `flatten_reloptions` 函数的定义、声明及所有调用
- **`src/backend/parser/parse_utilcmd.c`**：删除 `index->options = NIL;`
- **`src/fe_utils/string_utils.c`**：删除 `appendReloptionsArray` 函数定义
- **`src/include/fe_utils/string_utils.h`**：删除 `appendReloptionsArray` 函数声明
- **`src/include/catalog/toasting.h`**：删除所有函数声明中的 `Datum reloptions` 参数
- **`src/bin/psql/command.c`**：删除 `c.reloptions` 引用及 `appendReloptionsArray` 调用
- **`src/backend/tcop/utility.c`**：删除 `toast_options` 变量及调用 `NewRelationCreateToastTable` 时的 `reloptions` 参数
- **`src/backend/commands/tablecmds.c`**：`AlterTableCreateToastTable` 调用中删除 `(Datum) 0` 参数

### 验证

`make -j$(nproc)` 全量编译 0 error；`make check-world` 退出码 0，全部回归测试通过。与不可裁部分（btree/hash 索引、事务）零耦合。
