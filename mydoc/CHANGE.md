# minipg 变更日志（裁剪记录）

> 约定：每条裁剪均保证与「不可裁部分」（btree / hash 索引、事务）零耦合，删除后 `make -j` 全量重编通过。
> 验证命令固化：`cd src/test/regress && NO_TEMP_INSTALL=1 make check`（依赖先 `make prefix=$(pwd)/tmp_install install`）。
> 已知既有问题：minipg 既有 HEAD 的 `initdb` 因 `syscache.c` 的 `cacheinfo[]` 与 `syscache.h` 枚举不对齐而崩溃，须先对齐二者方能跑完整回归；裁剪时遇到该问题以单文件/全量编译验证为准。

## CREATE SUBSCRIPTION 命令标签裁剪（逻辑复制残留死标签，2026-08-25）

### 一、背景
`CMDTAG_CREATE_SUBSCRIPTION`（"CREATE SUBSCRIPTION"）是逻辑复制 `CREATE SUBSCRIPTION` 语句的命令标签。minipg 此前的裁剪已整体移除逻辑复制的语法与执行路径（`subscriptioncmds.c` 及 `CreateSubscriptionStmt` 节点均不存在，`gram.y` 无对应产生式），但 `cmdtaglist.h` 中该标签被漏删，属纯遗留死标签。

### 二、删除内容
- 命令标签 `CMDTAG_CREATE_SUBSCRIPTION`（`cmdtaglist.h`），`CommandTag` 枚举整体前移 1 位。
- 数据库内核（src/backend、src/include）中该标签无任何其他引用，无需清理调用方。

### 三、保留（内核核心，不裁）
- `CMDTAG_DROP_SUBSCRIPTION` 标签及 `DROP SUBSCRIPTION` 相关路径（逻辑复制目录 `pg_subscription*` 的通用对象删除仍按对象类型映射）。
- 其余 `CREATE_*` / `DROP_*` 命令标签不受影响。
- psql `tab-complete.c` 中 CREATE/ALTER SUBSCRIPTION 补全条目为客户端残留，与 ALTER SUBSCRIPTION（2026-08-24）、CREATE PUBLICATION 两轮一致，留待后续统一清理。

### 四、构建注意（重要）
`CommandTag` 枚举数值前移，本工程 Makefile 未启用头文件自动依赖跟踪，必须 `make clean && make -j8` 全量干净重编，否则运行时命令标签错位。

### 五、验证
`make clean && make -j8` 全量重编通过；`make check-world` 全绿（regress 72 项 + isolation 65 项 + modules/contrib 各子套件全通过，无任何 FAILED）。

## CREATE PUBLICATION 命令标签裁剪（逻辑复制残留死标签，2026-08-25）

### 一、背景
`CMDTAG_CREATE_PUBLICATION`（"CREATE PUBLICATION"）是逻辑复制 `CREATE PUBLICATION` 语句的命令标签。minipg 此前的裁剪已整体移除逻辑复制的语法与执行路径（`publicationcmds.c` / `subscriptioncmds.c` 及 `CreatePublicationStmt` 节点均不存在，`gram.y` 无对应产生式，`kwlist.h` 无 `PUBLICATION` 关键字），但 `cmdtaglist.h` 中该标签被漏删，属纯遗留死标签。

### 二、删除内容
- 命令标签 `CMDTAG_CREATE_PUBLICATION`（`cmdtaglist.h`），`CommandTag` 枚举整体前移 1 位。
- `src` 下该标签无任何其他引用（无语法、无节点、无执行路径、无 psql 引用），无需清理调用方。

### 三、保留（内核核心，不裁）
- `CMDTAG_DROP_PUBLICATION` 标签及 `DROP PUBLICATION` 相关路径（逻辑复制目录 `pg_publication*` 的通用对象删除仍按对象类型映射）。
- 其余 `CREATE_*` / `DROP_*` 命令标签不受影响。

### 四、构建注意（重要）
`CommandTag` 枚举数值前移，本工程 Makefile 未启用头文件自动依赖跟踪，必须 `make clean && make -j8` 全量干净重编，否则运行时命令标签错位。

### 五、验证
`make clean && make -j8` 全量重编通过；`make check-world` 全绿（regress 72 项 + isolation 65 项 + modules/contrib 各子套件全通过，无任何 FAILED）。

## CREATE OPERATOR CLASS / CREATE OPERATOR FAMILY 语法整体裁剪（2026-08-25）

### 一、背景
`CREATE OPERATOR CLASS` / `CREATE OPERATOR FAMILY` 是向索引访问方法注册自定义操作符类/操作符族的用户态 DDL。经核查，minipg 内建 btree/hash 的操作符类来自 `pg_opclass.dat` / `pg_opfamily.dat` 目录数据（initdb 装入），**不依赖运行时执行该 DDL**；执行体 `DefineOpClass` / `DefineOpFamily` 仅被 `utility.c` 的 `ProcessUtility` 派发，与索引、排序等核心路径零耦合。上一轮（2026-08-24）曾将其作为「黑盒保留项」，本轮按用户要求彻底裁剪。`DROP OPERATOR CLASS/FAMILY`、`ALTER ... RENAME/SET SCHEMA` 的查重函数（`IsThereOpClassInNamespace` / `IsThereOpFamilyInNamespace`）与 OID 解析（`get_opclass_oid` / `get_opfamily_oid`）仍被 objectaddress / typecmds / parse_clause / alter 等核心路径使用，予以保留。

### 二、删除内容
- **语法层（gram.y）**：删 `CreateOpClassStmt`、`CreateOpFamilyStmt` 两个产生式及其专属助手 `opclass_item_list`、`opclass_item`、`opt_default`、`opt_opfamily`、`opclass_purpose`、`opt_recheck`；同步清理 `%type` 声明与 `stmt` 顶层引用。
- **节点定义**：删 `parsenodes.h` 的 `CreateOpClassStmt`、`CreateOpClassItem`（含 `OPCLASS_ITEM_*` 宏）、`CreateOpFamilyStmt`；删 `nodes.h` 枚举 `T_CreateOpClassStmt` / `T_CreateOpClassItem` / `T_CreateOpFamilyStmt`（后续 `T_*` 整体前移，须全量重编）。
- **节点拷贝/比较**：删 `copyfuncs.c` / `equalfuncs.c` 的 `_copy/_equalCreateOpClassStmt`、`_copy/_equalCreateOpClassItem`、`_copy/_equalCreateOpFamilyStmt` 与对应 `T_*` case。
- **执行层（opclasscmds.c）**：删 `DefineOpClass`、`DefineOpFamily` 及其专属助手 `CreateOpFamily`、`processTypesSpec`、`assignOperTypes`、`assignProcTypes`、`addFamilyMember`、`storeOperators`、`storeProcedures`、`typeDepNeeded`；顺带清理早已无调用的死代码 `dropOperators`、`dropProcedures`（上游仅被已裁的 `AlterOpFamily` 引用）。
- **派发层（utility.c）**：删 4 处 `T_CreateOpClassStmt` / `T_CreateOpFamilyStmt` 引用——`ClassifyUtilityCommandAsReadOnly`、`ProcessUtility`（`DefineOpClass` / `DefineOpFamily` 调用）、`GetCommandTag`、`GetCommandLogLevel`。
- **命令标签**：删 `cmdtaglist.h` 的 `CMDTAG_CREATE_OPERATOR_CLASS` / `CMDTAG_CREATE_OPERATOR_FAMILY`，`CommandTag` 枚举整体前移 2 位。
- **头文件/pgindent**：删 `defrem.h` 的 `DefineOpClass` / `DefineOpFamily` 原型；删 `typedefs.list` 的 `CreateOpClassStmt` / `CreateOpClassItem` / `CreateOpFamilyStmt`。

### 三、保留（内核核心，不裁）
- `DROP OPERATOR CLASS/FAMILY`（`DropStmt` → objectaddress 通用删除）及 `OBJECT_OPCLASS` / `OBJECT_OPFAMILY` 依赖映射。
- `opclasscmds.c` 的 OID/名字解析与查重：`get_opclass_oid` / `get_opfamily_oid`（被 objectaddress.c、typecmds.c、parse_clause.c 调用）、`IsThereOpClassInNamespace` / `IsThereOpFamilyInNamespace`（被 alter.c 调用）。
- 内建 btree/hash 操作符类（来自目录数据）与 `pg_amop` / `pg_amproc` 支持过程表——索引/排序核心零影响。
- `CREATE OPERATOR`（`DefineOperator`）等其它操作符管理命令不受影响。

### 四、测试
- `src/test/regress/sql/insert.sql`：删除整段「direct partition inserts should check hash partition bound constraint」哈希分区测试块（含 `part_hashint4_noop` / `part_hashtext_length` 两个手写哈希函数、两个 `CREATE OPERATOR CLASS`、`hash_parted` 及其 hpart0-3 分区）。该块依赖已裁的 CREATE OPERATOR CLASS，且 minipg 本就不支持 `PARTITION BY`（全部语句原本就报语法错误），属死测试段；`expected/insert.out` 同步删除对应输出（原 544-621 行）。
- `select_parallel.sql` 中的 `CREATE OPERATOR CLASS` 用法不受影响（该测试早已从 `parallel_schedule` 移除）。
- `src/tutorial/complex.source` 中的 `CREATE OPERATOR CLASS` 教程示例不参与 check-world，未改动。

### 五、构建注意（重要）
删除 `nodes.h` 枚举与 `cmdtaglist.h` 命令标签都会使编号前移，必须 `make clean && make -j8` 全量干净重编；本工程 Makefile 未启用头文件自动依赖跟踪，仅增量编译会残留旧二进制。

### 六、验证
`make clean && make -j8` 全量重编通过；`make check-world` 全绿（regress 72 项 + isolation 65 项 + modules/contrib 各子套件全通过，无任何 FAILED）。

## CLOSE 命令标签裁剪（游标残留死标签，2026-08-25）

### 一、背景
`CMDTAG_CLOSE`（"CLOSE"）是 SQL 游标关闭语句 `CLOSE name` / `CLOSE ALL` 的命令标签，上游由 `ClosePortalStmt` 节点 → `utility.c` 派发 → `PerformPortalClose()` 执行。minipg 此前的裁剪已整体移除游标功能（DECLARE/OPEN/FETCH/MOVE/CLOSE 语法与节点全部删除，`kwlist.h` 无 `close` 关键字，`portalcmds.c` 仅剩 portal 基础设施 `PortalCleanup`），但 `cmdtaglist.h` 中仅此一个游标族标签被漏删，属纯遗留死标签。

### 二、删除内容
- 命令标签 `CMDTAG_CLOSE`（`cmdtaglist.h`），`CommandTag` 枚举整体前移 1 位。
- `src` 下该标签无任何其他引用（无语法、无节点、无执行路径、无 psql 引用），无需清理调用方。

### 三、保留（内核核心，不裁）
- portal 基础设施：`portalmem.c` 的 portal 存储管理、`portalcmds.c` 的 `PortalCleanup` 清理钩子——FE/BE 协议执行每个查询都经 portal，属内核核心。
- `CMDTAG_CLUSTER` 等其余命令标签不受影响。

### 四、构建注意（重要）
`CommandTag` 枚举数值前移，本工程 Makefile 未启用头文件自动依赖跟踪，必须 `make clean && make -j8` 全量干净重编，否则运行时命令标签错位。

### 五、验证
`make clean && make -j8` 全量重编通过；`make check-world` 全绿（regress 72 项 + isolation 65 项 + modules/contrib 各子套件全通过，无任何 FAILED）。

## CREATE DOMAIN 语法整体裁剪（2026-08-25）

### 一、背景
按用户要求，从命令标签 `cmdtaglist.h` 的 `CMDTAG_CREATE_DOMAIN` 入手，向下钻取删除整个 `CREATE DOMAIN` SQL 语法与执行路径。domain 类型本身（`pg_type` 的 `TYPTYPE_DOMAIN`、`domain.c` 的输入/输出/约束检查、typcache 域处理、依赖管理中的域约束）属类型系统基础设施，系统目录中仍可能存有域对象，运行期支持必须保留；本轮仅删除"创建域"这一条 DDL 入口。domain 学习价值低，可安全裁剪。

### 二、删除内容
- **命令标签**：`cmdtaglist.h` 删除 `CMDTAG_CREATE_DOMAIN`（"CREATE DOMAIN"），`CommandTag` 枚举整体前移。
- **语法（gram.y）**：`stmt` 顶层删除 `CreateDomainStmt` 引用；删除 `CreateDomainStmt` 产生式（`CREATE DOMAIN_P any_name opt_as Typename ColQualList`）及其专属辅助规则 `opt_as`（`AS` / 空）。
- **节点**：`parsenodes.h` 删除 `CreateDomainStmt` 结构体；`nodes.h` 删除 `T_CreateDomainStmt` 枚举值（后续 `T_*` 编号整体前移，须全量重编）；`copyfuncs.c`/`equalfuncs.c` 删除 `_copyCreateDomainStmt`/`_equalCreateDomainStmt` 及其 switch case。
- **执行（typecmds.c）**：删除 `DefineDomain` 整函数及其辅助 `get_rels_with_domain`、`checkDomainOwner`、`domainAddConstraint`、`replace_domain_constraint_value` 与 `RelToCheck` 结构体；`typecmds.h` 同步删除 `DefineDomain`/`checkDomainOwner` 原型。
- **派发（utility.c）**：删除 `T_CreateDomainStmt` 在 `ClassifyUtilityCommandAsReadOnly`、`ProcessUtility`、`GetCommandTag`、`GetCommandLogLevel` 中的 4 处 case（`GetCommandTag` 原映射回退 `CMDTAG_UNKNOWN`）。
- **pgindent**：`typedefs.list` 删除 `CreateDomainStmt` / `RelToCheck`。

### 三、保留（内核核心，不裁）
- `DROP DOMAIN` / `CMDTAG_DROP_DOMAIN` 及 `OBJECT_DOMAIN` / `OBJECT_DOMCONSTRAINT` 对象类型：删除对象/依赖级联仍须按对象类型映射。
- domain 类型运行期基础设施：`domain.c`（`domain_in/domain_recv/domain_check` 等 I/O 与约束检查）、typcache 域处理、`ALTER TABLE ... ALTER COLUMN ... TYPE` 列类型变更中的域处理等——系统目录中既有域对象必须可正常读写。
- `DOMAIN_P` 关键字保留为无保留关键字（`DROP DOMAIN` 等仍需使用）。
- `ALTER DOMAIN` 已于 2026-08-24 单独裁剪（上一轮删除 `AlterDomainStmt` 时保留 CREATE DOMAIN，本轮继续向下裁剪）。

### 四、测试
- 删除 `src/test/regress/sql/domain.sql` 与 `expected/domain.out`（域专属测试，parallel_schedule 中 `domain` 项此前已移除，本次确认无残留引用后彻底删除）。
- `sql/insert.sql`：删除依赖域类型的 "Make the same tests with domains over the array and composite fields" 整段（`create domain insert_pos_ints` ... `drop type insert_test_type cascade`），`expected/insert.out` 同步删除对应预期。
- 其余 `CREATE DOMAIN` 引用均不在回归调度内，无需处理：`collate.sql`（引用 information_schema，已随 information_schema 裁剪移除）、`drop_if_exists.sql`（依赖 role/owner，已移除）、`fast_default.sql`（既有 minipg domain/fast-default SIGSEGV，临时注释移出）、isolation `ddl-dependency-locking.spec`（依赖 DROP ROLE/OWNER 权限锁，已注释移出）、modules `test_extensions`（扩展脚本依赖 FDW/sequence，已从 check-world 移除）。

### 五、构建注意（重要）
删除 `nodes.h` 的 `T_CreateDomainStmt` 与 `cmdtaglist.h` 的 `CMDTAG_CREATE_DOMAIN` 会使节点枚举与 `CommandTag` 枚举数值整体前移，而本工程 Makefile 未启用头文件自动依赖跟踪，旧的 `*.o` 不会自动重编。**务必 `make clean && make -j8` 全量干净重编再跑测试**，否则运行时命令标签/节点类型错位（initdb 阶段 `unrecognized node type` 或完成标签错乱）。

### 六、验证
`make clean && make -j8` 全量重编通过；`make check-world` 全绿（regress 72 项 + isolation 65 项 + modules/contrib 各子套件全通过，无任何 FAILED）。

## 枚举数据类型（enum）整体裁剪（2026-08-25）

### 一、背景
minipg 此前在「裁剪 ALTER TYPE」多轮中保留了 `CREATE TYPE AS ENUM` 与整个 `pg_enum` 枚举系统。本轮按用户要求将整个枚举数据类型彻底裁剪：枚举类型本身、`pg_enum` 演化目录、`anyenum` 多态伪类型、枚举相关操作符/操作符类族/聚合/哈希函数全部删除。枚举是纯用户自建类型，与 btree/hash 索引、事务等内核核心零耦合，学习价值低，可安全裁剪；`CREATE TYPE`（复合 / range）、`ALTER TABLE ... ALTER COLUMN ... TYPE` 列类型变更等保留。

### 二、删除内容
- **语法/节点**：`gram.y` 中 `CREATE TYPE ... AS ENUM` 的 `enum_type` 相关产生式与 `ENUM_P` 关键产生（`ENUM_P` 作为无保留关键字仍保留于关键字集合）；`parsenodes.h` 的 `CreateEnumStmt` 结构体、`nodes.h` 的 `T_CreateEnumStmt` 枚举值随之删除（`T_*` 整体前移）；`copyfuncs.c` / `equalfuncs.c` 对应 `_copy/_equalCreateEnumStmt` 与 case 一并删除；`typedefs.list` 移除 `CreateEnumStmt`。
- **目录**：删除 `pg_enum.h`、`pg_enum.c`；删除 `syscache.c/h` 中 `ENUMOID`、`ENUMTYPOIDNAME` 两个缓存及其 `cacheinfo[]` 条目，`SysCacheSize` 同步；`objectaddress.c` 移除 `pg_enum.h` 引用；`typedefs.list` 移除 `FormData_pg_enum` / `Form_pg_enum`。
- **多态伪类型**：`pg_type.dat` 删除 `anyenum` 条目，`pg_type.h` 删除 `ANYENUMOID`、`TYPTYPE_ENUM`、`TYPCATEGORY_ENUM`；`pseudotypes.c` 删除 `anyenum_in` / `anyenum_out`；`pg_proc.dat` 删除对应伪类型 I/O 函数。
- **函数**：`enum.c` 删除，`utils/adt/Makefile` 移除 `enum.o`；`enum_in/out/recv/send` 等输入输出函数、`enum_eq/ne/lt/...` 比较操作符全部消失。
- **operator/opclass**：`pg_operator.dat` 删除枚举比较操作符；`pg_opfamily.dat` / `pg_opclass.dat` 删除 `enum_ops`；`pg_amop.dat` / `pg_amproc.dat` 删除 btree/hash 中枚举操作符与支持过程。
- **聚合**：`pg_aggregate.dat` 删除 `min(anyenum)` / `max(anyenum)`。
- **哈希**：`hashfunc.c` 删除 `hashenum` / `hashenumextended`。
- **调用方清理**：`parse_coerce.c` 移除 `ANYENUMOID` 分支与 `type_is_enum` 判断；`funcapi.c` / `functions.c` / `typecmds.c` 移除 `TYPTYPE_ENUM` / `ANYENUMOID` case；`lsyscache.[ch]` 删除 `type_is_enum`。
- **事务/并行钩子**：`enum.c` 被删后，`xact.c` 移除 `AtEOXact_Enum()` 调用及 `pg_enum.h` 引用，`parallel.c` 移除 `EstimateUncommittedEnumsSpace` / `SerializeUncommittedEnums` / `RestoreUncommittedEnums` 及 `PARALLEL_KEY_UNCOMMITTEDENUMS`。
- **psql**：`describe.c` 移除枚举元素（Elements）展示查询；`tab-complete.c` 移除 `COMPLETE_WITH_ENUM_VALUE` 宏与枚举值补全查询。
- **测试**：删除 `sql/enum.sql` 与 `expected/enum.out`；`parallel_schedule` 移除 `enum` 测试项；`hash_func.sql/out` 删除枚举哈希测试；`opr_sanity.sql` 移除 `anyenum` 多态检查、`opr_sanity.out` 删除 `enum_in/out/recv` 等行；`rangefuncs.out` 更新不含 `anyenum` 的错误文案；`arrays.sql/out` 将 `create type _comptype as enum('fooey')` 改为复合类型 `as (f1 text)`（仅测试隐式数组类型名冲突）；`case.sql/out` 删除依赖 `enum_range` 的 CASE 段；`sanity_check.out` 删除 `pg_enum|t`。

### 三、保留（内核核心，不裁）
- `CREATE TYPE` 复合 / range 分支；`ALTER TABLE ... ALTER COLUMN ... TYPE` 列类型变更（`AT_AlterColumnType` / `ATExecAlterColumnType`）。
- `enum` 关键字（`ENUM_P`）保留为无保留关键字。

### 四、构建注意（重要）
本工程 Makefile 未启用头文件自动依赖跟踪。`syscache.h` 中删除两个缓存会导致 `SysCacheIdentifier` 与 `TYPEOID` 等缓存编号前移，但旧的 `*.o`（如 `lsyscache.o`）不会自动重编，运行时 `get_typtype` 会用旧编号越界访问 `SysCache`，`initdb` bootstrap 建 `pg_attrdef` 时 `cache=0x0` 段错误。**务必 `make clean && make -j8` 全量干净重编再跑测试**。

### 五、验证
`make clean && make -j8` 通过；`make check-world` 全绿（regress 72 项 + 各子套件通过）；`initdb` bootstrap 不再段错误。

## ALTER RULE / ALTER SCHEMA / ALTER STATISTICS / ALTER SUBSCRIPTION 命令标签裁剪（2026-08-24）

### 一、背景
`ALTER RULE`、`ALTER SCHEMA`、`ALTER STATISTICS`、`ALTER SUBSCRIPTION` 四个命令在 minipg 中均无语法产生式、无节点类型、无执行路径（无 `RenameStmt` / `AlterOwnerStmt` / `AlterStatsStmt` / `AlterSubscriptionStmt`），属纯遗留死标签，本次仅清理其命令标签与 `AlterObjectTypeCommandTag` 中的死分支。

### 二、删除内容
- 命令标签 `CMDTAG_ALTER_RULE` / `CMDTAG_ALTER_SCHEMA` / `CMDTAG_ALTER_STATISTICS` / `CMDTAG_ALTER_SUBSCRIPTION`（`cmdtaglist.h`）。
- `utility.c` 的 `AlterObjectTypeCommandTag` 中对应死分支（`OBJECT_RULE` / `OBJECT_SCHEMA` / `OBJECT_SUBSCRIPTION` / `OBJECT_STATISTIC_EXT`）。
- `parsenodes.h` 中完全无用的 `OBJECT_SUBSCRIPTION` 枚举值（其余三个枚举值仍被 DROP/对象地址解析使用，保留）。
- 同步删除 `alter_rule.sgml` / `alter_schema.sgml` / `alter_statistics.sgml` 三个文档，并移除其在 `allfiles.sgml` / `reference.sgml` 中的实体引用。

### 三、保留
- `OBJECT_RULE` / `OBJECT_SCHEMA` / `OBJECT_STATISTIC_EXT` 枚举值及 `DROP ... RULE/SCHEMA/STATISTICS`、`REINDEX SCHEMA` 等执行路径：删除对象 / 依赖处理仍需按对象类型映射。
- `ALTER TABLE ... ENABLE/DISABLE RULE`（`AT_EnableRule`/`AT_DisableRule`）：该路径经 `AlterTableStmt(OBJECT_TABLE)` 映射为 `CMDTAG_ALTER_TABLE`，与本次裁剪无关。

> 重要经验：本工程 Makefile **未启用头文件自动依赖跟踪**。编辑 `cmdtaglist.h` 后，`CommandTag` 枚举数值前移（本次前移 4 位），但除被 rm 或直接修改的 `*.o` 外其余对象（如 `dest.c` / `postgres.c` / `executor`）不会自动重编，运行时会出现命令标签错位（如 `SELECT 1` 的完成标签被判为 `SELECT 0 1`，回归大量报「could not interpret result from server」）。正确做法：编辑 `cmdtaglist.h` 后执行 `make clean && make -j8` 全量干净重编，再跑 `make check-world`。

验证：全量干净重编 + `make check-world` 全绿（regress 73 项 + 各子套件全通过）。

## ALTER TYPE ... RENAME VALUE 子命令彻底裁剪（2026-08-25）

### 一、背景
在「仅裁命令标签」基础上进一步向下钻取执行层。`ALTER TYPE ... ADD VALUE`（枚举运行时扩展）属内核保留能力；但 `ALTER TYPE ... RENAME VALUE`（重命名枚举标签）是独立的子命令，执行体 `RenameEnumLabel` 仅被该子命令引用，删去不影响 `CREATE TYPE AS ENUM` / `ADD VALUE` / `AlterTable(OBJECT_TYPE)` 列类型变更等核心路径。

### 二、删除内容
- **语法**：`gram.y` 中 `ALTER TYPE_P any_name RENAME VALUE_P Sconst TO Sconst` 生产式（原第 2531 行起）。
- **节点**：`AlterEnumStmt.oldVal` 字段删除（`parsenodes.h`），同步清理 `equalfuncs.c` / `copyfuncs.c` 的 `oldVal` 比较与拷贝；`gram.y` 三个 `ADD VALUE` 生产式中 `n->oldVal = NULL;` 初始化一并移除。
- **执行**：`typecmds.c` 的 `AlterEnum` 函数删 `if (stmt->oldVal)` RENAME 分支，仅保留 `AddEnumLabel`。
- **catalog 函数**：`pg_enum.c` 的 `RenameEnumLabel` 整函数删除，及其声明 `pg_enum.h` 的 `RenameEnumLabel` 原型。

### 三、保留
- `ALTER TYPE ... ADD VALUE`（含 `IF NOT EXISTS` / `BEFORE` / `AFTER`）；`AlterTypeStmt`（`ALTER TYPE ... SET(storage/receive/send/typmod_in/...)` I/O 函数替换）**整体保留**——其中 `SET(receive/send/typmod_in/typmod_out/analyze)` 是自定义类型管理的内核能力，学习价值高，不裁整条 `AlterTypeStmt`。
- `ALTER VIEW` 复用 `AlterTableStmt(OBJECT_VIEW)`，与视图/规则系统强耦合，本次不裁。

### 四、测试
- `src/test/regress/sql/enum.sql`：删除「rename a value」整段（原 256-265）与 transactional 段中的 `RENAME VALUE` 用法（原 299-303）。
- `src/test/regress/expected/enum.out`：同步删除对应预期输出；因 `RENAME VALUE` 生产式移除后 `ALTER TYPE ... RENAME TO`（RenameStmt，本库未实现）的语法错误报告点由 `TO` 前移至 `RENAME`，更新 3 处 caret 位置；另因 bison 重算语法位置，2 处 `REFERENCES` 错误的 caret 偏移同步校正。

验证：全量干净重编 + `make check`（73 项全绿）+ `make check-world`（各子套件全通过）。

## ALTER TYPE 整个 SQL 语法彻底裁剪（2026-08-25）

### 一、背景
在「仅裁命令标签」「裁 RENAME VALUE 子命令」两轮基础上，用户要求进一步删除整个 `ALTER TYPE` SQL 语法（含 `ADD VALUE` 枚举扩展、`SET(...)` I/O 函数替换、复合类型加列、`SET SCHEMA`、`RENAME TO`）。经 code-explorer 子代理与人工核对：`AlterTypeStmt` / `AlterEnumStmt` 是独立节点，其执行体 `AlterType` / `AlterEnum` 仅被 `utility.c` 的 `ProcessUtility` 直接派发，**与 `ALTER TABLE ... ALTER COLUMN ... TYPE` 列类型变更（走 `AT_AlterColumnType` → `ATExecAlterColumnType`）零耦合**——后者不调用 `AlterType` / `AlterEnum` / `AlterTypeRecurse`。故可安全全链路删除而不破坏内核核心的列类型变更能力。

### 二、删除内容
- **语法层（gram.y）**：删 `AlterEnumStmt`（`ALTER TYPE ... ADD VALUE`）、`AlterTypeStmt`（`ALTER TYPE ... SET(...)`）、`AlterCompositeTypeStmt`（`ALTER TYPE ... alter_type_cmds` 复合类型加列）三个生产式；删 `AlterObjectSchemaStmt` 中 `ALTER TYPE ... SET SCHEMA` 分支、`RenameStmt` 的 `OBJECT_TYPE` 分支；同步清理 `%type` 声明与 `stmt` 顶层引用。保留 `AlterObjectSchemaStmt` / `AlterTableStmt` / `AnalyzeStmt` 节点本身（仍被其它对象复用）。
- **节点定义**：删 `parsenodes.h` 的 `AlterTypeStmt` / `AlterEnumStmt` 结构体；删 `nodes.h` 枚举 `T_AlterTypeStmt` / `T_AlterEnumStmt`（后续 `T_*` 编号整体前移，须全量重编）。
- **执行层（typecmds.c）**：删 `AlterType`、`AlterEnum`、`AlterTypeRecurse`（仅被 `AlterType` 内部递归调用，删后为死代码）三个函数；删无调用的 `checkEnumOwner`（minipg 已裁 ACL，仅检查枚举类型不再检查 owner）；删 `typecmds.h` 的 `AlterType` / `AlterEnum` 原型。
- **节点拷贝/比较**：删 `copyfuncs.c` / `equalfuncs.c` 的 `_copyAlterTypeStmt` / `_copyAlterEnumStmt` / `_equalAlterTypeStmt` / `_equalAlterEnumStmt` 函数与对应 `T_*` case。
- **派发层（utility.c）**：删 7 处 `T_AlterTypeStmt` / `T_AlterEnumStmt` 引用——`ClassifyUtilityCommandAsReadOnly`（fallthrough 只读分类）、`ProcessUtility` 两处派发、`CreateCommandTag` 两处（含 `T_AlterEnumStmt` 原映射 `CMDTAG_CREATE_TYPE`，因 `CMDTAG_ALTER_TYPE` 已于上轮删除，此处曾回退 `CMDTAG_UNKNOWN`）、`LogStmt` 两处 level 判定。
- **pgindent**：删 `typedefs.list` 的 `AlterEnumStmt` / `AlterTypeStmt` 条目。

### 三、保留（内核核心，不裁）
- `CREATE TYPE`（复合/枚举/range）、`CREATE TYPE ... AS ENUM` 与 `pg_enum` 枚举系统；列类型变更 `ALTER TABLE ... ALTER COLUMN ... TYPE`（`AT_AlterColumnType` / `ATExecAlterColumnType` 整条路径）；视图（`ALTER VIEW` 复用 `AlterTableStmt(OBJECT_VIEW)`，与规则系统耦合，留待后续单独评估）。
- `ALTER TYPE` 之下的 `RenameStmt(OBJECT_TYPE)` / `AlterObjectSchemaStmt(OBJECT_TYPE)` 通用节点 branch 已不可达（语法已删），但节点本身保留供 `ALTER TABLE`/`VIEW` 等复用。

### 四、测试
- `src/test/regress/sql/enum.sql`：删除「adding new values」「errors for adding labels」「if not exists tests」「Test inserting so many values that we have to renumber」「check transactional behaviour of ALTER TYPE ... ADD VALUE」全部 `ALTER TYPE ... ADD VALUE` / `RENAME VALUE` / `RENAME TO` 段，仅保留 `CREATE TYPE AS ENUM` 与基础查询/索引/域/数组/支持函数/RI。由于语法已移除，这些语句现报语法错误，必须删除。
- `src/test/regress/sql/domain.sql`：删除复合类型 `alter type comptype alter attribute a type text` / `drop attribute b` 两段（走已删的 `AlterCompositeTypeStmt`）。
- `expected/enum.out`、`expected/domain.out`：同步删除对应预期输出；`opr_sanity.out` 因函数 OID 随节点枚举重排，新增 `enum_in`(3506)/`enum_out`(3507) 出现在 cstring I/O 检查列表，已用真实 `results` 同步。
- `expected/enum.out` 中 `ALTER TYPE ... RENAME TO` 报错点（原 `at or near "TO"`）因 `RENAME` 关键字不再出现在该语法位置，前移至 `at or near "RENAME"`，已在上一轮修正。

### 五、构建注意（重要）
删除 `nodes.h` 中枚举值会使所有 `T_*` 编号前移，必须**全量重编**：本环境经 `make maintainer-clean && ./configure --prefix=/home/postgres/minipg --enable-debug && make -j` 重建后验证；仅增量 `make -C src/backend` 可能因头文件依赖未触发全重编而残留旧二进制，导致 `initdb` 阶段报 `unrecognized node type: 271 (T_Null)`。已确认 `make check`（73 项）+ `make check-world`（各子套件）全绿。

## ALTER TRANSFORM / ALTER TYPE / ALTER VIEW 命令标签裁剪（2026-08-25）

### 一、背景
`ALTER TRANSFORM`、`ALTER TYPE`、`ALTER VIEW` 三类命令的语法与执行路径在 minipg 中仍存在（`AlterTypeStmt`/`AlterEnumStmt`/`CompositeTypeStmt`/`CreateEnumStmt`/`CreateRangeStmt`/`ViewStmt`/`AlterObjectSchemaStmt(OBJECT_TYPE/OBJECT_VIEW/OBJECT_ATTRIBUTE)` 等节点均保留），但 `ALTER TRANSFORM` 没有任何节点类型引用其命令标签（纯预留死标签），`ALTER TYPE`/`ALTER VIEW` 的标签亦仅为派发层状态字符串。本次按「仅裁标签与派发层、保留语法/功能」的既定方案，删除这三个命令标签，对应 `utility.c` 分支回退为 `CMDTAG_UNKNOWN`（命令仍照常执行，仅不再返回专属完成标签）。

### 二、删除内容
- 命令标签 `CMDTAG_ALTER_TRANSFORM` / `CMDTAG_ALTER_TYPE` / `CMDTAG_ALTER_VIEW`（`cmdtaglist.h`）。
- `utility.c` 中全部引用这三个标签的分支回退为 `CMDTAG_UNKNOWN`：
  - `AlterObjectTypeCommandTag`：`OBJECT_ATTRIBUTE` → `CMDTAG_UNKNOWN`；`OBJECT_TYPE` → `CMDTAG_UNKNOWN`；`OBJECT_VIEW` → `CMDTAG_UNKNOWN`。
  - `CreateCommandTag`：`T_AlterEnumStmt` → `CMDTAG_UNKNOWN`；`T_AlterTypeStmt` → `CMDTAG_UNKNOWN`。
  - `CMDTAG_ALTER_TRANSFORM` 在 `src` 下无引用，仅删定义。

### 三、保留
- `CREATE TYPE`（复合/枚举/range）、`ALTER TYPE`（含 `ALTER TYPE ... ADD VALUE`/`ALTER ENUM`/`ALTER TYPE ... SET SCHEMA`）、`CREATE/ALTER/DROP VIEW`、`CREATE/ALTER/DROP TRANSFORM` 的语法与执行路径：类型系统、视图、transform 属内核保留项，本次仅移除命令完成标签。
- `DROP TYPE`/`DROP VIEW`/`DROP TRANSFORM` 及其 `CMDTAG_DROP_*` 标签：删除对象仍须按对象类型映射命令标签。

> 注意：删除 `cmdtaglist.h` 中 3 个标签会使 `CommandTag` 枚举数值整体前移，必须重编译 `cmdtag.o`/`utility.o` 及所有依赖 `cmdtag.h` 的源文件（建议 `make clean && make -j` 全量干净重编），否则运行时命令标签错位（如 `SELECT` 被判为后续枚举），客户端报「could not interpret result from server」。

验证：全量干净重编通过；`make check` 73 项全绿。

## ALTER LANGUAGE / OPERATOR / OPERATOR CLASS / OPERATOR FAMILY / PROCEDURE / PUBLICATION / ROUTINE 命令标签裁剪（2026-08-24）

### 一、背景
`ALTER LANGUAGE`、`ALTER OPERATOR`、`ALTER OPERATOR CLASS`、`ALTER OPERATOR FAMILY`、`ALTER PROCEDURE`、`ALTER PUBLICATION`、`ALTER ROUTINE` 七个命令在 minipg 中早已随语言/操作符/发布/存储过程等功能裁剪（无语法产生式、无节点类型、无执行路径），本次仅清理其遗留命令标签与死分支。

### 二、删除内容
- 命令标签 `CMDTAG_ALTER_LANGUAGE` / `CMDTAG_ALTER_OPERATOR` / `CMDTAG_ALTER_OPERATOR_CLASS` / `CMDTAG_ALTER_OPERATOR_FAMILY` / `CMDTAG_ALTER_PROCEDURE` / `CMDTAG_ALTER_PUBLICATION` / `CMDTAG_ALTER_ROUTINE`（`cmdtaglist.h`）。
- `utility.c` 的 `AlterObjectTypeCommandTag` 中对应死分支（`OBJECT_LANGUAGE` / `OBJECT_OPERATOR` / `OBJECT_OPCLASS` / `OBJECT_OPFAMILY` / `OBJECT_PROCEDURE` / `OBJECT_ROUTINE` / `OBJECT_PUBLICATION`）。
- `utility.c` 的 `CreateCommandTag` 中一段无 case 标签的孤儿死代码（残留引用 `CMDTAG_ALTER_OPERATOR_FAMILY` 与 `CMDTAG_ALTER_OPERATOR`，早已不可达）。
- 同步删除 `alter_operator.sgml` / `alter_opclass.sgml` / `alter_opfamily.sgml` 三个文档，并移除其在 `allfiles.sgml` / `reference.sgml` 中的实体引用。

### 三、保留
- `CREATE OPERATOR`（`CMDTAG_CREATE_OPERATOR`）、`CREATE/DROP OPERATOR CLASS/FAMILY`（`CMDTAG_CREATE/DROP_OPERATOR_CLASS/FAMILY`）标签及其执行路径：opclasscmds.c / operatorcmds.c 仍被索引、排序等核心功能使用，属黑盒保留项；`ALTER` 方向因无语法入口而彻底删除。
- `DROP OPERATOR` / `DROP OPERATOR CLASS` / `DROP OPERATOR FAMILY` / `DROP PUBLICATION` / `DROP SUBSCRIPTION` / `DROP ROUTINE` / `DROP PROCEDURE` / `DROP LANGUAGE` 标签：删除对象 / 依赖级联仍需按对象类型映射命令标签。

> 注意：本次变更删除 `cmdtaglist.h` 中的 7 个标签会使 `CommandTag` 枚举数值整体前移，必须对包含 `cmdtag.o`（及其依赖本次标签列表的源文件）做一次干净重编，否则运行时命令标签会发生 7 位错位（如 `DROP TABLE` 被判为 `DROP OPERATOR FAMILY`）。回归报错「could not interpret result from server」或 read-only 事务命令名错位时，应先 `rm src/backend/tcop/cmdtag.o` 全量重编。

验证：全量重编 + `make check-world` 全绿。

## ALTER DOMAIN / ALTER DATABASE / ALTER EXTENSION 命令裁剪（2026-08-24）

### 一、背景
`ALTER DATABASE` 与 `ALTER EXTENSION` 命令在 minipg 中早已随数据库/扩展功能裁剪（无语法产生式、无节点、无执行路径），本次仅清理其遗留死标签与死分支。`ALTER DOMAIN` 命令此前仍保留完整的语法与执行路径，但领域类型（domain）的核心能力（CREATE DOMAIN、类型约束检查、依赖管理）已由既有代码支撑，`ALTER DOMAIN` 作为 DDL 变更入口学习价值低，予以彻底裁剪。

### 二、删除内容
- 命令标签 `CMDTAG_ALTER_DATABASE` / `CMDTAG_ALTER_DOMAIN` / `CMDTAG_ALTER_EXTENSION`（`cmdtaglist.h`）。
- `gram.y` 中 `AlterDomainStmt` 非终结符及其全部产生式（SET/DROP DEFAULT、SET/DROP NOT NULL、ADD CONSTRAINT、DROP CONSTRAINT、VALIDATE CONSTRAINT），以及 `AlterObjectSchemaStmt` 的 `ALTER DOMAIN ... SET SCHEMA` 分支。
- `AlterDomainStmt` 节点：`nodes.h` 标签、`parsenodes.h` 结构体、`copyfuncs.c`/`equalfuncs.c` 复制与比较函数及 switch 分支、`typedefs.list` 条目。
- `typecmds.c` 中 `AlterDomainDefault` / `AlterDomainNotNull` / `AlterDomainAddConstraint` / `AlterDomainDropConstraint` / `AlterDomainValidateConstraint` 五个执行函数及 `validateDomainConstraint` 辅助函数；`typecmds.h` 中对应 extern 声明。
- `tablecmds.c` 中 `AT_ReAddDomainConstraint` 子命令处理（枚举、锁等级分支、执行分支、`AlterDomainStmt` 解析分支）。
- `utility.c` 中 `T_AlterDomainStmt` 的只读分类、`ProcessUtilitySlow` 分发、`CreateCommandTag`、日志分级等分支，以及 `AlterObjectTypeCommandTag` 中 `OBJECT_DATABASE`/`OBJECT_DOMAIN`/`OBJECT_DOMCONSTRAINT`/`OBJECT_EXTENSION` 死分支。

### 三、保留
- `CREATE DOMAIN` / `DROP DOMAIN` 及其 `CMDTAG_CREATE_DOMAIN` / `CMDTAG_DROP_DOMAIN` 标签：domain 是类型系统核心，`DefineDomain`、`domainAddConstraint`、`get_rels_with_domain` 等仍被 CREATE DOMAIN 与约束依赖管理使用，不可裁剪。
- `ALTER TABLE ... SET SCHEMA`、`ALTER TYPE ... SET SCHEMA`、`ALTER VIEW ... SET SCHEMA` 等其余 `AlterObjectSchemaStmt` 分支。
- `OBJECT_DOMCONSTRAINT` 对象类型枚举：`objectaddress.c` 依赖系统仍须按域约束执行依赖/级联删除。

验证：`make -j8` 全量重编通过；`make check-world` 全绿。

## ALTER ACCESS METHOD 命令标签裁剪（2026-08-24）

`CREATE ACCESS METHOD` 命令此前已随访问方法 DDL 裁剪一并移除（gram.y 产生式、`AlterAmStmt`/`DropAmStmt`/`DefineAm` 均已删除），本次清理遗留的命令标签 `CMDTAG_ALTER_ACCESS_METHOD`（`ALTER ACCESS METHOD` 在 PostgreSQL 中本就不存在命令语法，仅删除预留标签）。经全库检索，`src` 下已无任何 `ACCESS METHOD` 命令相关残留。验证：`make -j8` 全量重编通过；`make check-world` 全绿。

## CREATE/ALTER/DROP CAST 功能裁剪（2026-08-24）

### 一、背景
用户自定义 CAST 的 DDL 入口（gram.y 的 `CREATE CAST`/`DROP CAST` 产生式、`castcmds.c` 整文件、`CreateCastStmt` 节点）此前已随 CREATE TYPE 基础形式裁剪一并移除，本次彻底清理 SQL 命令层的残留死代码。

### 二、删除内容
- `gram.y` 中 `CREATE CAST / DROP CAST` 残留注释头（无产生式）。
- 命令标签 `CMDTAG_ALTER_CAST`/`CMDTAG_CREATE_CAST`/`CMDTAG_DROP_CAST`（`ALTER CAST` 在 PostgreSQL 中本就不存在命令语法，仅删除预留标签）。
- `utility.c` 中 `AlterObjectTypeCommandTag`/`DropObjectTypeCommandTag` 的 `OBJECT_CAST` 分支（不可达）。
- `dropcmds.c` 中 `does_not_exist_skipping` 的 `OBJECT_CAST` 分支（不可达）。
- `drop_if_exists` 测试中 DROP CAST 用例（其期望输出为语法错误）。

### 三、保留
`pg_cast` 系统表、`CastCreate`（`pg_cast.c`）及 `OBJECT_CAST`/`OCLASS_CAST` 对象寻址与依赖管理：range 类型创建时系统自动生成 range→multirange 转换记录，`DROP TYPE ... CASCADE` 时依赖系统仍需按 `pg_cast` 元组执行级联删除，属于内核核心，不可裁剪。

### 四、验证
`make clean && make -j8` 全量重编通过；`make check-world` 全绿（regress 73 用例 + isolation/modules/contrib）。

## ALTER AGGREGATE 全形式裁剪确认与 psql 死代码清理（2026-08-24）

`ALTER AGGREGATE` 的三种形式（RENAME TO / OWNER TO / SET SCHEMA）此前（2026-08-18/08-20）已随外围 ALTER 裁剪与 `AlterAggregateStmt` 产生式删除而全部移除，本次在 `CREATE/DROP AGGREGATE` 裁剪（见上一条）基础上确认后端零残留：`gram.y` 无 `ALTER AGGREGATE` 产生式，`OBJECT_AGGREGATE` 枚举、命令标签、`alter.c`/对象寻址均无聚合分支。

同步清理 `src/bin/psql/tab-complete.c` 中面向已裁命令的 AGGREGATE 补全死代码：`CREATE OR REPLACE`、`COMMENT ON`、`DROP`（含参数补全）候选列表移除 `AGGREGATE`；保留 `\dA`（`Query_for_list_of_aggregates`，内置聚合函数 avg/sum/count 等仍存于 `pg_aggregate`）。

验证：`make check-world` 全绿。

## CREATE/DROP AGGREGATE、CREATE OPERATOR、CREATE TYPE（基础/shell）裁剪（2026-08-24）

### 一、删除内容
删除 `DefineStmt` 中 `CreateAggregateStmt`/`CreateOperatorStmt`/`CreateTypeStmt`（含基础类型与 shell 类型）的整套 DDL 链路：gram.y 产生式、`aggregatecmds.c`（整个文件）、`operatorcmds.c`/`typecmds.c` 中对应的 `DefineAggregate`/`DefineOperator`/`DefineType`（仅保留复合/枚举/range 分支）、`T_CreateAggregateStmt`/`T_CreateOperatorStmt`/`T_CreateTypeStmt` 节点（copy/equal）、命令标签（`CMDTAG_CREATE/DROP_AGGREGATE`、`CMDTAG_CREATE_OPERATOR`）、`OBJECT_AGGREGATE`/`OBJECT_OPERATOR` 对象寻址、`DROP AGGREGATE`、`remove_aggregate`、`ComputeFunctionHash` 等。

### 二、保留
`CREATE TYPE AS`（复合/枚举/range）、`ALTER/DROP TYPE`、`ALTER/DROP OPERATOR`、`CREATE/ALTER/DROP OPERATOR CLASS/FAMILY`、`CreateShellType`（内部 shell 机制，供复合类型自引用等使用）。

### 三、同步清理
- 删除测试文件：`create_aggregate`、`create_operator`、`create_type`、`drop_operator`、`alter_operator`、`equivclass`。
- `create_function_1` 测试删除（其 `int44in`/`int44out` C 函数仅服务于 `city_budget` 基础类型，已随 CREATE TYPE 基础形式裁剪一并移除，`regress.c` 同步删函数）。
- `create_table.sql` 的 `city` 表 `budget` 列由 `city_budget` 基础类型改为 `text`。
- `src/test/modules/test_custom_types` 整个模块删除（依赖 CREATE TYPE 基础形式 + CREATE OPERATOR，CREATE EXTENSION 直接报语法错误）。
- `errors`/`tsrf`/`float4`/`expressions`/`drop_if_exists`/`polymorphism`/`case`/`subselect` 等测试脚本裁剪依赖被删语法的用例，`parallel_schedule` 移除对应条目。

### 四、验证
`make -j8` 全量重编通过；`make check-world` 全绿（regress 73 用例 + modules/contrib）。

## DO 语句裁剪（2026-08-24）

DO 语句是 SQL 匿名块入口，minipg 内置语言均无 `laninline`，执行必然报错，属装饰性语法。删除 gram.y 产生式、`DoStmt`/`InlineCodeBlock` 节点、`ExecuteDoStmt`、`CMDTAG_DO`；**保留 `DO` 关键字**（CREATE RULE ... DO INSTEAD、ON CONFLICT ... DO UPDATE 复用）。验证：make check 通过，DO 报语法错误，upsert/rule 正常。

## 游标（CURSOR）功能裁剪（2026-08-24）

删除游标语法（DECLARE/FETCH/MOVE/CLOSE）、`CurrentOfExpr`、`WHERE CURRENT OF`、可持久/可滚动游标物化及 `refcursor` 类型。**保留 Portal 机制、扩展查询协议、`DestTuplestore`、SPI 内存管理**；`cursorOptions` 计划标志保留。验证：回归 159/159 全绿。

## 插件钩子与调试桩裁剪（2026-08-24）

### 一、插件钩子裁剪
删除 `ProcessUtility_hook` 与 4 个 Executor hook（Start/Run/Finish/End），调用点直接改调 `standard_*`，并清理 typedefs.list 登记。

### 二、调试桩裁剪
删除 `RAW_EXPRESSION_COVERAGE_TEST` 条件编译块（`test_raw_expression_coverage`）与 `COPY_PARSE_PLAN_TREES` 解析树调试转储分支。

## initdb 裁剪：移除 -A/--auth、--auth-host、--auth-local 选项（2026-08-21）

移除 initdb 三个认证方式配置选项，清理 `usage()`/`getopt_long` 的 `A:`，并同步 PostgresNode.pm 移除 `-A trust`。

## CREATE/ALTER/DROP CONVERSION 功能裁剪 与 SEQUENCE 残留裁剪（2026-08-21）

### 一、CREATE/ALTER/DROP CONVERSION 功能裁剪
保留 `pg_conversion` 目录与内置编码转换；删除用户自定义 conversion 的整套 DDL 入口（gram.y 产生式、`conversioncmds.c`、`ConversionCreate`、`T_CreateConversionStmt`、`OBJECT_CONVERSION`、命令标签、psql `\dc`）。保留 `FindDefaultConversion`。

### 二、SEQUENCE 残留裁剪
清理 psql `\ds` 序列展示残留、`CMDTAG_CREATE/ALTER/DROP_SEQUENCE` 死标签、`DiscardMode.DISCARD_SEQUENCES`、typedefs.list。验证：make check-world 全绿（regress 80 用例）。

## libpq / 通信层 IPv6 与死代码裁剪（2026-08-21）

### 一、IPv6 通信层裁剪
仅支持 IPv4：删除 `HAVE_IPV6` 宏及 ifaddr.c/fe-connect.c 的 IPv6 死代码；保留 getaddrinfo 封装与 inet/cidr 的 IPv6 存储能力。

### 二、Notify 前端死代码裁剪
后端 LISTEN/NOTIFY 已删：删除 libpq `PQnotifies`/`PGnotify`/`PQfreeNotify`、psql 打印、tab 补全及示例 testlibpq2。

### 三、密码认证残留死代码
删除 `AUTH_REQ_PASSWORD` 分支与无调用者的 `pg_password_sendauth()`。

### 四、COPY 协议 trace 死代码
删除 fe-trace.c 中 Copy 相关追踪函数与消息分支。验证：make check-world 通过。

## SASL/SCRAM 认证裁剪（2026-08-21）

服务端已无条件信任、永不发起 SASL 握手，客户端 SASL/SCRAM 链路为死代码。删除 scram-common.c、saslprep.c、fe-auth-scram.c 与 `AUTH_REQ_SASL*` 宏；`channel_binding` 连接参数不再识别。验证：make 通过，手工 TCP 连接查询成功。

## 临时关系文件清理死链裁剪（2026-08-21）

### 删除内容（src/backend/storage/file/fd.c）
删除 `RemovePgTempRelationFiles`/`RemovePgTempRelationFilesInDbspace`/`looks_like_temp_rel_name`（无临时表后永远匹配不到的死代码）。

### 同步修改
删除 fd.h 中 `looks_like_temp_rel_name` 导出声明。

### 保留说明
保留 `RemovePgTempFilesInDir`（排序/hash join 普通临时文件）与 `OpenTemporaryFile`。验证：make -C src 通过。

## authentication_timeout 参数裁剪（2026-08-21）

删除该 GUC、`AuthenticationTimeout` 变量及 postgresql.conf.sample 项。附修复：恢复误删的 `StatementTimeoutHandler` 注册，避免 statement_timeout/lock_timeout 段错误。验证：isolation 66 测试全过。

## Relation Options（reloptions）功能裁剪（2026-08-21）

删除 reloptions 框架：核心文件、`StdRdOptions`/`BTOptions`/`HashOptions`/`AutoVacOpts` 及 fillfactor 等访问宏，消费改硬编码；contrib（bloom 等）同步。保留 `pg_class.reloptions` 列（视为 NULL）与 `amoptions` 接口。验证：make check-world 全绿。

## 触发器（TRIGGER）功能裁剪（2026-08-20）

删除触发器全链路（语法、`pg_trigger.h`、`TriggerData`、AFTER 队列、`pg_trigger_depth` 等），保留 `pg_class.relhastriggers` 恒 false 列与 TRIGGER 关键字。修复误删 `CMD_SELECT` case（致 combocid 偏移与 deadlock-parallel 失败）。验证：regress 82、isolation 66 全绿。

## ALTER COLLATION 功能裁剪（2026-08-20）

删除空的 `ALTER COLLATION` 语法壳与 alter.c 中 `OBJECT_COLLATION` case 及 sgml 文档；保留 `OBJECT_COLLATION` 枚举（CREATE/DROP COLLATION 与对象寻址仍用）。

## ALTER AGGREGATE 功能裁剪（2026-08-20）

删除 `ALTER AGGREGATE ... SET SCHEMA` 产生式与 `CMDTAG_ALTER_AGGREGATE`；底层 aggregate 对象系统（`OBJECT_AGGREGATE`、CREATE/DROP AGGREGATE）保留。回归 alter_generic/create_aggregate 同步。验证：make check 全绿。

## ALTER FUNCTION / OPERATOR / OPERATOR FAMILY / STATISTICS / EXTENSION / DATABASE 功能裁剪（2026-08-18）

删除 7 类外围 ALTER 语法与实现（AlterFunction/AlterOperator/AlterOpFamily/AlterStatistics/AlterExtension/AlterExtensionContents/AlterDatabase），保留 ALTER TABLE（索引核心）与 ALTER TYPE/DOMAIN/ENUM、SET SCHEMA 节点（ALTER TABLE 复用）。

### 回归测试同步（2026-08-18）
移除 alter_generic/alter_operator/misc_functions/guc 中对已裁语法的依赖并重生成 expected，83 用例通过。

#### alter_generic 二次清理
清理遗留的 CREATE/ALTER FUNCTION、ALTER OPERATOR/OPERATOR FAMILY 调用，仅留 AGGREGATE/CONVERSION 的 RENAME/SET SCHEMA。

### gram.y 死语法清理
删 7 个 useless 非终结符：opclass_drop_list/opclass_drop、object_type_name、add_drop、alter_extension_opt_list/item、alterfunc_opt_list。

## CREATE/DROP/ALTER COLLATION 功能裁剪（2026-08-18）

保留排序规则内核（pg_collation 预置表、pg_locale.c、collation 字段），删除用户侧 CREATE/ALTER COLLATION 语法及 `DefineCollation`/`AlterCollation` 实现；保留 `IsThereCollationInNamespace`、`pg_import_system_collations`。

## ALTER DEFAULT PRIVILEGES 残留清理（2026-08-18）

删除孤立 `CMDTAG_ALTER_DEFAULT_PRIVILEGES` 标签与 `AlterDefaultPrivilegesStmt` 类型名（ACL 裁后无任何引用）。

## cmdtaglist.h 已裁命令标签清理（2026-08-18）

删 23 条孤立命令标签：TEXT SEARCH 12 条、ROLE 3 条及其它 8 条（CREATE TABLE AS/SELECT INTO/REFRESH MATVIEW/ROLE/SHOW GRANT、DROP OWNED、约束类），并清 utility.c 悬挂死语句。保留被对象类型系统仍引用的标签（ACCESS METHOD/CAST/COLLATION/LANGUAGE/PUBLICATION/SUBSCRIPTION/TRANSFORM/ALTER VIEW）。

## GSSAPI 功能彻底裁剪（2026-08-17）

删除 `ENABLE_GSS` 死代码：backend_status 的 `PgBackendGSSStatus`、postmaster `NEGOTIATE_GSS_CODE` 协商、`AUTH_REQ_GSS` 分支、`CONNECTION_GSS_STARTUP`、wait_event，及专测 004_negotiate.pl。保留 `AUTH_REQ_GSS` 协议常数以稳布局。

## 一、平台 / 构建链裁剪

- （07-30 前）仅支持 Linux，删 Windows/Cygwin/MSVC 代码与模板；删 EXEC_BACKEND 双实现。
- （07-31）删 pgbench/pg_upgrade 等运维 bin，保留 initdb/pg_ctl/psql/pg_dump 等。

## 二、客户端工具与协议层

- （08-02）SSL/TLS 与 GSSAPI 加密裁剪；（08-13）物理流复制全链路（walsender/walreceiver/slot/syncrep/basebackup）；（08-12）删 pg_basebackup；（08-15）放弃 PG13 之前兼容。
- （08-17）裁剪 Unix 域套接字与 IPv6 监听层（仅 IPv4 TCP），全库逐文件删 `HAVE_UNIX_SOCKETS` 死代码，强制 `AF_INET`。
- （08-17 续）删 pqcomm/pgstatfuncs/ipattr 的 IPv6 残骸、`USE_SSL` 后端状态字段、libpq 客户端 SSL 死代码（删 `ssl_in_use` 等）。

## 三、过程语言 / 嵌入式 SQL

删 plperl/plpython/tcl（08-03）、ecpg 嵌入式 SQL（08-15）、PL/pgSQL（08-14，含解析器钩子与 initdb 默认安装），DO/回归转 SQL。

## 四、功能模块裁剪

contrib 保留 11 删 45；删 `--with-selinux/perl/python`、BRIN、ICU、NLS、Bonjour/Systemd/XML、tsearch、JIT、pg_prewarm、异步 Append、UNION/INTERSECT/EXCEPT、CTE、CREATE TABLE LIKE、COMMENT ON 等。另有 ObjectProperty[] 错位与 aclchk 精简修复。

## 五、catalog / 系统表裁剪（含 syscache 联动）

删 pg_partitioned_table、pg_inherits+继承死代码、pg_sequence、序列残留全链、pg_class 的 relispartition/relpartbound/relreplident 列、ALTER DATABASE SET / ALTER SYSTEM / REPLICA IDENTITY、CREATE SEQUENCE 悬空语法等，均含 syscache 联动与 genbki 重排。

## 六、权限 / 对象生命周期裁剪

- （08-15）删 ACL 访问控制、pg_policy+RLS、RTE 权限位字段组（requiredPerms/checkAsUser/securityQuals，保留列修改位图）；（08-16）删用户/角色/密码概念、外键 FK 语法、superuser() 内联；（08-17）删 owner 机制与 aclchk.c 调用壳后整删；（08-18）删 regrole 类型/角色骨架/`ALTER OWNER TO` 死链。
- 保留对象变更位图、`BOOTSTRAP_SUPERUSERID` 及核心 DDL 依赖的取 owner 适配层。

## 七、优化器表继承展开

（08-15）删 inherit.c/inherit.h，`expand_appendrel_subquery` 迁至 appendinfo.c，仅 RTE_SUBQUERY 走 appendrel 展开，保留 UNION ALL。

## 八、回归测试基线修复（与功能裁剪配套的 expected 同步）

多轮修复 expected/*.out 与 isolation/sanity/schedule（删继承/分区/序列/窗口等一致性；enum/create_index REFERENCES 语法化；`--no-locale` 固定 C locale 解决 int8/numeric/select_implicit）至 make check 全绿。

## 九、死代码清理（编译器驱动）

（08-16）用 `-Wunused-function` 清理零调用 static 函数：tablecmds.c 的 `ATExecAlterColumnGenericOptions`/`ATExecSetTableSpaceNoStorage`/`transformColumnNameList`、allpaths.c 的 `compare_tlist_datatypes` 悬空原型。

## 十、编译 / 链接错误修复（历史裁剪残留 bug，非功能裁剪）

（08-17）修复误加 `static` 导致与 pg_proc.dat/fmgrprotos.h 冲突的函数（pseudotypes.c 宏生成 I/O、selfuncs_geo.c 选择率函数），修为全局 `Datum`；删 pgstatfuncs.c 零调用死函数。

## 十一、编译警告驱动的代码清理（2026-08-17）

恢复误删的 `aclcheck_error`/`aclcheck_error_type` 声明、删 `AclResult` 死变量、补 rangetypes/selfuncs_geo/pseudotypes 的 extern 原型、修 planner.c declaration-after-statement 与 psql 格式/列参数警告，后端+前端 0 warning。

## 十二、死代码残留裁剪（2026-08-17，对应 mydoc/minipg死代码残留分析报告.md）

执行 3 项：① 删 `pg_index.indisreplident` 惰性死列（含删除后列号错位致的 psql SIGSEGV 修复）；② 删 `get_transform_fromsql/tosql`；③ 删 `fdw_handler` 伪类型孤儿。暂缓 HistoricSnapshot/MVCC、`qual_security_level`、partitionwise 等高风险残留。

### 十二-补：裁剪 ALTER ... OWNER TO 死代码链（2026-08-18）
删整条 OWNER TO 语法链、`ExecAlterOwnerStmt`/`AlterObjectOwner_internal`、`T_AlterOwnerStmt` 及产生式；保留 `RoleSpec`（ALTER TABLE OWNER、CREATE TABLESPACE OWNER 仍用）。

## 十三、acl.h 头文件死宏清理（2026-08-18，ACL 裁剪收尾）

删除依赖已裁权限位常量的悬空宏（ACL_ID_PUBLIC/ACLITEMOID/ACL_ALL_RIGHTS_* 等）、多余 include；保留 `AclResult` 枚举。全库 grep 0 命中。

## 十四、acl.h 整文件删除（2026-08-18，ACL 裁剪最终收尾）

删 execMain.c/genam.c 两处 `AclResult` 恒假死壳引用与 54 个冗余 include，`git rm acl.h`。验证：make check-world 全绿。

## 十五、gram.y 删除 SetResetClause 死规则（2026-08-18）

删无任何引用方的 `SetResetClause` 规则并校正注释；保留 `FunctionSetResetClause`（CREATE FUNCTION 需用）。

## 十六、gram.y 删除 CREATE/ALTER SEQUENCE 死语法链（2026-08-18）

删 `OptParenthesizedSeqOptList`/`SeqOptList`/`SeqOptElem`/`opt_by` 无引用死链；保留 NumericOnly/opt_with 等活跃依赖。运行期 nextval/currval 仍保留但无建序列 DDL 入口。

## 十七、删除空壳 acl.c（2026-08-18，ACL 裁剪最终收尾）

删已无任何函数实现的空壳 `src/backend/utils/adt/acl.c` 及 Makefile `acl.o`，刷新 objfiles.txt。验证：make check-world 全绿。

## 十八、小结：minipg ACL/权限系统裁剪全景（2026-08-18）

汇总权限系统裁剪收尾（aclchk 调用壳→角色骨架→acl.h 死宏→acl.h 整删→空壳 acl.c→gram.y 死链）。结论：无对象级 ACL，仅存 ownercheck 语义，与不可裁核心零耦合。

## 十三、窗口函数（Window Function）彻底裁剪（2026-08-17）

自底向上删除窗口函数：nodeWindowAgg/windowapi.h、窗口节点树、解析器（OVER/WINDOW）、优化器路径、`prokind='w'` 注册、window.sql 及 6 个混用窗口的回归残留。保留 `in_range_*` 帧函数、`CREATE FUNCTION ... WINDOW` DDL 兼容项与聚合机制。

## 十九、gram.y 消除 bison useless nonterminal/rule 警告（2026-08-18）

删 `NumericOnly_list`、`any_with`、`opt_distinct_clause` 三个死规则及 `%type` 声明，消除 3 nonterminal/6 rule 警告。

## 二十、gram.y 删除 CREATE ASSERTION 未实现语法（2026-08-18）

删从未实现的 CREATE ASSERTION 占位桩及 ASSERTION 关键字（含 %token/产生式/两处关键字分类，共 7 处）；保留 `CONSTRAINT_ASSERTION` 枚举预留与 `Assert()` 运行时断言。

## 二十一、裁剪角色/用户兼容视图 pg_roles / pg_shadow / pg_group / pg_user（2026-08-18）

删四个视图定义、psql `\du`/`\dg`（describeRoles）实现、tab-complete 角色名补全、help/syscat 引用。验证受工作区 gram.y 半成品影响暂未跑回归。

## numeric 数据类型彻底删除（2026-08-18）

删除 numeric 类型与全部函数（numeric.c 11327 行），耦合代码改写 int8/float8（date/timestamp/pg_lsn/dbsize/formatting 等），新建 intagg.c 承接整数 sum/avg（`_int8` 态）；catalog 与回归 expected（23 regress + 7 isolation）同步。验证：回归 82、isolation 66。

## FUNCTION/PROCEDURE 命令标签与派发层裁剪（2026-08-18）

保留 CREATE/ALTER/DROP FUNCTION 语法与执行（回归大量依赖），删除其 5 个 CMDTAG，utility.c 派发 case 返回 CMDTAG_UNKNOWN；修复 CMDTAG 枚举错位导致的 SELECT 状态错误。验证：make check-world 全绿。

## GRANT/REVOKE 命令标签裁剪（ACL 裁剪收尾，2026-08-19）

删 `CMDTAG_GRANT`/`CMDTAG_REVOKE` 死标签与 gram.y 死注释（GrantStmt 已删，全库 0 命中）。验证：make check-world 全绿。

## EXCLUDE 约束（排除约束）功能裁剪（2026-08-19）

AM 已裁至 heap/btree/hash，EXCLUDE 依赖 GiST 故不可用，彻底删除：语法、节点（`Constraint.exclusions`/`IndexStmt.excludeOpNames`）、catalog 列（`conexclop`/`indisexclusion`）、执行器冲突检测、pg_get_constraintdef 显示。保留 UNIQUE/PRIMARY KEY/CHECK 与 `constraint_exclusion` GUC。经验：删非保留关键字须保持 kwlist/gram.y 一致，删 catalog 列须全量重编。

## constraints 回归测试清理（2026-08-20，EXCLUDE + 序列裁切收尾）

清理 constraints.source 对 EXCLUDE/序列的残留（DEFAULT_SEQ/INSERT_TBL 段删除），保留 DEFAULT/CHECK/PRIMARY KEY/UNIQUE/可延迟等约束核心测试并重生成期望输出。

## 删除因分区功能裁剪而失效的 3 个规划 GUC（2026-08-20）

删 enable_partitionwise_join/pruning/aggregate 三个死 GUC 及声明/定义/sample，planner 恒置 PARTITIONWISE_AGGREGATE_NONE，并修复 relnode.c 引用已删字段的 Assert 编译错误；sysviews 同步。验证：回归 82 用例通过。

## 彻底裁剪 create/drop/alter tablespace（用户自建表空间管理，2026-08-21）

保留 `pg_tablespace` 目录与 pg_default/pg_global、md.c/smgr spcNode 寻址、reltablespace 恒 0；删除用户自建表空间全部 DDL、tablespace.c 执行/WAL rmgr、相关 GUC/psql `\db`/SQL 函数等，60+ 文件净删约 5700 行。验证：make check-world 全绿。

## 裁剪 db_user_namespace（每数据库独立用户名，2026-08-21）

删除 "每数据库独立用户名" GUC 及 ProcessStartupPacket 中 user@dbname 拼接逻辑、postgresql.conf.sample 项与文档。验证：make check-world 退出码 0。

## Relation Options 残余代码清理（2026-08-21）

清理 reloptions 核心裁剪后残留：indexcmds/view/toasting/cluster/relcache/ruleutils `get_reloptions`/fe_utils `appendReloptionsArray`/parse_utilcmd/psql 的 reloptions 参数与引用。验证：make check-world 退出码 0。