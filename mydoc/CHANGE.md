# minipg 变更日志（裁剪记录 · 压缩版）

> 约定：每条裁剪保证与「不可裁部分」（btree / hash 索引、事务）零耦合，删除后全量重编 + `make check-world` 全绿。
> 完整历史条目见 `mydoc/CHANGE.full.md`（如需逐文件细节）。

## 〇、通用构建注意（多次裁剪反复踩坑，集中记录）

1. **枚举前移必全量重编**：删除 `cmdtaglist.h` 命令标签、`nodes.h` 的 `T_*`、`ObjectType`、`parsenodes.h` 字段、`kwlist.h` 关键字、或 `pg_*` syscache 缓存项，都会使对应整数枚举/编号整体前移。本工程 Makefile **未启用头文件自动依赖跟踪**，旧 `*.o` 不会自动重编，残留旧二进制会导致运行时错位：
   - `cmdtag` 错位 → `SELECT 1` 被判为后续枚举，客户端报 `could not interpret result from server`；
   - `nodes.h` 错位 → `initdb` 阶段 `unrecognized node type`；
   - `syscache.h` 删缓存 → `SysCacheIdentifier` 前移，`initdb` bootstrap 段错误（cache 越界）。
   - 正确做法：`make clean && make -j8`（或 `make maintainer-clean && ./configure --prefix=/home/postgres/minipg --enable-debug && make -j`）全量干净重编，并清 `tmp_install` 旧副本。
2. **catalog `.dat` 视为二进制**：`pg_proc/pg_type/pg_operator/pg_opclass/pg_amop/pg_amproc/pg_cast/pg_aggregate/pg_conversion` 等用 `sed -i` 删行，genbki 自动重生成 `fmgroids.h`/`fmgrtab.c`。删 C 实现**必须同步删 `.dat` 条目**，否则链接 `undefined reference: fmgrtab`。
3. **删 catalog 列须核对 psql**：`pg_class` 等列布局变更后，`psql/describe.c` 的 `\d` 主查询所有 `PQgetvalue(res,0,N)` 列序号须整体下移，漏移会报 `column number N is out of range`。
4. **验证口径**：各裁剪均 `make check-world` 通过（regress 主套件 68–73 用例、isolation 58–66 用例、contrib 等），零 FAILED；孤立函数删除以 `全库 grep 已删符号 0 命中` 反向佐证。

---

## 一、类型系统裁剪

- **枚举 enum（08-25）**：删 `pg_enum` 目录（`pg_enum.c/h`、`ENUMOID/ENUMTYPOIDNAME` syscache）、`anyenum` 伪类型、`enum.c`（`enum_in/out/recv/send/eq/lt...`）、枚举比较操作符/`enum_ops` opclass/opfamily/`min(anyenum)/max(anyenum)` 聚合/`hashenum`、xact 的 `AtEOXact_Enum` 与 parallel 的 `Estimate/Serialize/RestoreUncommittedEnums`、`parse_coerce`/`funcapi`/`lsyscache`(`type_is_enum`)/`typecmds` 分支、psql 枚举展示与补全。保留 `CREATE TYPE AS ENUM`、`ALTER TABLE ... ALTER COLUMN ... TYPE` 列类型变更、`ENUM_P` 关键字。
- **range / multirange（09-01）**：删 `pg_range.c/h`、`rangetypes*.c`（`rangetypes`/`_selfuncs`/`_typanalyze`）、`multirangetypes*.c`、相应头文件；收敛 `pg_type/pg_proc/pg_operator/pg_opclass/pg_opfamily/pg_amop/pg_amproc/pg_cast/pg_aggregate` 中 range/multirange 条目与 `anyrange/anymultirange/anycompatiblerange/anycompatiblemultirange` 伪类型；`nodes.h` 节点、`gram.y`、`typcache`（`load_range/multirange_type_info`、`TYPECACHE_RANGE/MULTIRANGE_INFO`）、`syscache`（`RANGETYPE/RANGEMULTI`）、`lsyscache`（`get_range_subtype` 等）、`executor/functions.c`/`funcapi.c`/`format_type.c`/`ruleutils.c`/`subselect.c` 中分支。保留 `makeMultirangeTypeName` 等已提前删的死代码清理。附：同轮删 `varbit.c/h`（位串，见 1.4）与 `common/username.c`。
- **domain 域类型（08-25 语法 + 08-31 基础设施）**：语法层先删 `CreateDomainStmt`/`AlterDomainStmt`/`CMDTAG_CREATE/DROP_DOMAIN` 及 `gram.y` 产生式、节点、`utility.c` 派发、测试 `domain.sql`。08-31 彻底删除基础设施：`pg_type.h` 去 `typbasetype/typtypmod/typtndims/typnotnull/typdefaultbin/typdefault` 与 `TYPTYPE_DOMAIN`、`DECLARE_TOAST(pg_type,...)`；`pg_constraint.h` 去 `contypid/CONSTRAINT_DOMAIN/ConstraintCategory`、唯一索引 2666、重命名 2665→`ConstraintRelidNameIndexId`；`TypeCreate/TypeShellMake/GenerateTypeDependencies` 去域参数；`typcache.c` 删 `DomainConstraint*`/`load_domaintype_info`、`getBaseType*` 直通、`get_type_func_class`/`type_is_rowtype` 去域分支；删 `domains.c`（`domain_check/domain_in/out/recv/send`）+ Makefile + pg_proc 条目；execExpr 域约束求值设施；psql `\dD`（`listDomains`）、`\du/\dg`/`describeRoles`/`tab-complete` 角色补全。保留 `OBJECT_DOMCONSTRAINT`（`objectaddress.c` 依赖级联删除仍用）。
- **bit / varbit 位串（09-01）**：删 `varbit.c/h` + Makefile；`pg_type/pg_proc`（~40 条位串函数：I/O、typmod、比较/位运算/位操作/类型转换/聚合 `bit_and/or/xor`、`bit_length(bit)`/`get_bit/set_bit/bit_count(bit)`）、`pg_operator/pg_opclass/pg_opfamily/pg_amop/pg_amproc/pg_cast/pg_aggregate` 中 bit/varbit 条目；`gram.y` 去 `BCONST/XCONST` 与 `makeBitStringConst`、`T_BitString`（NodeTag 前移）；`parse_node/value/read/copy/equal/out/nodeFuncs/format_type/ruleutils` 去 bit 分支。保留 bytea/text 变体的 `bit_length/get_bit/set_bit/bit_count` 与整型位运算聚合。
- **numeric（08-18）**：删 `numeric.c`（11327 行）与全部 numeric 函数，耦合代码改写 int8/float8（date/timestamp/pg_lsn/dbsize 等），新建 `intagg.c` 承接整数 sum/avg（`_int8` 态）；catalog 与 23 regress + 7 isolation expected 同步。
- **CREATE TYPE / ALTER TYPE 语法族（08-24~08-25）**：删 `CreateTypeStmt`(复合/range)/`CreateEnumStmt`/`CreateRangeStmt`/`CompositeTypeStmt`(复合)/`AlterTypeStmt`/`AlterEnumStmt`/`AlterCompositeTypeStmt` 产生式与节点、`DefineCompositeType/DefineRange/makeRangeConstructors/...`/`AlterType/AlterEnum/AlterTypeRecurse/checkEnumOwner`、`CMDTAG_CREATE/DROP_TYPE`、`CMDTAG_ALTER_TYPE/TRANSFORM/VIEW`(标签回退 UNKNOWN)、`TYPE_P` 关键字保留。删 `CreateAggregateStmt/CreateOperatorStmt/CreateTypeStmt`(基础/shell)、`aggregatecmds.c`、`DefineAggregate/DefineOperator/DefineType`、`CMDTAG_CREATE/DROP_AGGREGATE`、`CMDTAG_CREATE_OPERATOR`、`OBJECT_AGGREGATE/OPERATOR`、`remove_aggregate`、`ComputeFunctionHash`、测试 `create_aggregate/create_operator/create_type/drop_operator/alter_operator/equivclass`、`create_function_1`(int44 基础类型)。保留 `ALTER TABLE ... ALTER COLUMN ... TYPE`(`AT_AlterColumnType`)、`AssignTypeArrayOid`、`CREATE VIEW`、`DROP TYPE` 级联删除核心(`RemoveTypeById`/`OBJECT_TYPE`)、`CreateShellType`、`CreateOperatorClass/Family` 仍在用的 OID/名字解析(`IsThereOpClassInNamespace`/`get_opclass_oid`)。
- **to_char / to_number / to_timestamp 格式化（09-01）**：删 `formatting.c`(6418 行)/`formatting.h` + Makefile；其大小写转换函数 `str_*/asc_*` 迁入 `oracle_compat.c`（`asc_*` 降 static，删 `*_z` 包装）。`pg_proc.dat` sed 删 10 条 SQL 入口（`to_char`×7、`to_timestamp(text,text)`、`to_date`；`to_number` 本无 pg_proc 条目随删）。`lc_time` 整链删除：`pg_locale.c` 的 `locale_time`/`localized_abbrev_days/full_days/abbrev_months/full_months` 缓存/`check_locale_time/assign_locale_time`/`cache_locale_time`/`cache_single_string`/`case LC_TIME`/`MAX_L10N_DATA`/`<time.h>`；`pg_locale.h` 声明；`guc.c` `lc_time` GUC；`main.c` `init_locale("LC_TIME")`；`postgresql.conf.sample`；`pg_regress.c`；`initdb.c` 不再写 `lc_time`（其 `--lc-time` 与 `locale_date_order()` 保留驱动 DateStyle）。保留 `to_timestamp(float8)`(Unix epoch)、`pg_encoding_to_char`。`func.sgml` 删 functions-formatting 整节并修 xref；`config.sgml` 删 `guc-lc-time`；`charset/datatype/features.sgml` 修失效 xref。
- **Unicode 规范化 UAX#15（09-01）**：删 `unicode_norm.c` + `src/common/unicode/` 生成器目录、`unicode_norm*.h`；`wchar.c` 精简 `ucs_wcwidth` 去组合字符宽度表(`mbbisearch`/`struct mbinterval`)；`varlena.c` 删 `unicode_normalize_func/is_normalized`；`pg_proc.dat`/`system_functions.sql` 删 `normalize/is_normalized`(sed 双删)；`gram.y` 删 `NORMALIZE(...)`/`IS [NOT] [form] NORMALIZED` 6 条 + `kwlist.h` 六行；测试 `unicode.sql` 等。保留 CJK/全角宽度 2。
- **几何/网络/UUID 类型簇残骸清理（09-01）**：几何(point/line/lseg/box/path/polygon/circle)、网络(inet/cidr/macaddr/macaddr8)、UUID 四类类型此前已在 catalog(`pg_type.dat` 无这些类型)与 .c 实现层(`geo_ops.c`/`network.c`/`uuid.c` 均不存在；bit/varbit 见 1.4)移除，本次仅清收尾死代码残骸：删 `selfuncs_geo.c`(`areasel`/`areajoinsel`/`positionsel`/`positionjoinsel`/`contsel`/`contjoinsel` 六个零引用选择性估计函数，经核 `pg_operator.dat` 仅 `arraycontsel`/`arraycontjoinsel` 活)+ Makefile OBJS 行 + 其 `pg_proc.dat` 6 条注册(sed 删 `oid` 139/140/1300–1303 块)；删死头文件 `inet.h`/`uuid.h`/`geo_decls.h`，并将 `inet_net_ntop.c` 的 `PGSQL_AF_INET/PGSQL_AF_INET6` 宏本地化、`pgstatfuncs.c`/`tutorial/funcs.c` 死 include 清理、`typedefs.list` 收敛；删 `bit.sql` 已删后的孤儿回归数据 `data/load_bit_bit.sql`/`load_bit_varbit.sql`；`datatype.sgml` 删几何/网络/位串/UUID 四个 sect1 类型描述块及摘要表相关行、`acronyms.sgml` 修指向 `datatype-uuid` 的悬空 xref。`format_type.c` 仅注释提及 bit 不动。全量重编 + `make check-world` 全绿(regress 68 用例，测试对删类型已期望报错)。

## 二、DDL 命令标签与语法裁剪（集中于 cmdtaglist.h / gram.y / utility.c）

**命令标签批量清理**：`cmdtaglist.h` 累计删除 23 条孤立标签（08-18，TEXT SEARCH 12 + ROLE 3 + 其它 8）及 `CMDTAG_CREATE/DROP/ALTER_ROUTINE`、`CMDTAG_CREATE_SUBSCRIPTION/PUBLICATION`（逻辑复制已删，纯死标签）、`CMDTAG_CLOSE`（游标已删）、`CMDTAG_DROP_SUBSCRIPTION`（08-26）、`CMDTAG_CREATE_ROUTINE`（08-27，从未实现）、`CMDTAG_ALTER_DEFAULT_PRIVILEGES`（08-18，ACL 裁后无引用）、`CMDTAG_GRANT/REVOKE`（08-19，GrantStmt 已删）、`CMDTAG_ALTER_ACCESS_METHOD`（08-24）、`CMDTAG_ALTER_*`（LANGUAGE/OPERATOR/OPCLASS/OPFAMILY/PROCEDURE/PUBLICATION/ROUTINE/RULE/SCHEMA/STATISTICS/SUBSCRIPTION/TRANSFORM/TYPE/VIEW/DATABASE/EXTENSION/DOMAIN/AGGREGATE）及 `ALTER/ALTER_* CAST`。`utility.c` 对应分支回退 `CMDTAG_UNKNOWN` 或删除死分支。

**DROP 类语法裁剪**：
- `DROP TYPE`（08-26）：删语法+`CMDTAG_DROP_TYPE`，保留级联删除核心 `RemoveTypeById`/`OBJECT_TYPE` 寻址。
- `DROP LANGUAGE`（08-25）：删语法与标签，`LANGUAGE` 关键字、`OBJECT_LANGUAGE`、pg_language 目录保留。
- `DROP OPERATOR CLASS/FAMILY`、`DROP PUBLICATION`、`DROP ROUTINE`、`DROP RULE`（08-25）：删 5 个标签与 `DropOpClass/FamilyStmt`/`RemoveFuncStmt`(ROUTINE)/`object_type_name_on_any_name`(RULE) 产生式、`utility.c`/`dropcmds.c` 分支；保留 `OBJECT_OPCLASS/OPFAMILY/ROUTINE/RULE/PUBLICATION` 寻址（依赖级联仍用）。
- `DROP OPERATOR`（08-25）：删 `RemoveOperStmt`/`operator_with_argtypes*` 与标签；保留 `OBJECT_OPERATOR` 寻址、内建运算符与 btree/hash opclass。
- `DROP DOMAIN`（08-25）：删语法/`DOMAIN_P` 关键字/`OBJECT_DOMAIN/DOMCONSTRAINT`/`CMDTAG_DROP_DOMAIN`/`get_domain_constraint_oid`/`alter.c`/`typecmds.c`/`dropcmds.c` 分支；保留 `DROP TYPE`(OBJECT_TYPE)、`ALTER TYPE SET SCHEMA`。
- `LOAD`、`LOCK TABLE`（08-25）：删 `LoadStmt`/`LockStmt` 节点、`cmdtag`、`lockcmds.c/h`、`closeAllVfds`(fd.c)；保留锁管理器(lmgr)/行锁/`SELECT FOR UPDATE`/`load_file`(shared_preload_libraries)/`LOCKED`(`SKIP LOCKED`)/`LOAD/LOCK_P` 关键字。
- `CREATE OPERATOR CLASS / CREATE OPERATOR FAMILY`（08-25）：删 `CreateOpClass/FamilyStmt` 产生式与节点、`DefineOpClass/DefineOpFamily` 及助手(`opclasscmds.c`)、`cmdtag`；保留内建 btree/hash opclass（来自 pg_opclass.dat）、`get_opclass_oid/get_opfamily_oid/IsThereOpClassInNamespace` 解析（索引/排序核心）。

**CREATE/ALTER 类语法裁剪**：
- `CREATE DOMAIN`（08-25）：删 `CreateDomainStmt` 语法/节点/`DefineDomain`+助手(`get_rels_with_domain/checkDomainOwner/domainAddConstraint/replace_domain_constraint_value`/`RelToCheck`)/`cmdtag`/`tab-complete`；保留域类型运行期(`domain.c`/`typcache`/`ALTER TABLE ... TYPE` 列变更)、`DROP DOMAIN`/`DOMAIN_P`。
- `ALTER TYPE ... RENAME VALUE`（08-25）：删语法/`AlterEnumStmt.oldVal`/`RenameEnumLabel`；保留 `ALTER TYPE ... ADD VALUE`/`SET(...)`/`AlterTableStmt(OBJECT_TYPE)`。
- `ALTER DOMAIN / DATABASE / EXTENSION`（08-24）：删 `AlterDomainStmt`(全套)/`AlterDatabaseStmt`/`AlterExtensionStmt` 语法、节点、`AlterDomainDefault/NotNull/AddConstraint/DropConstraint/ValidateConstraint/validateDomainConstraint`、`AT_ReAddDomainConstraint`、`CMDTAG_*`；保留 `CREATE/DROP DOMAIN`、`OBJECT_DOMCONSTRAINT` 寻址。
- `CREATE/ALTER/DROP CONVERSION` + SEQUENCE 残留（08-21）：删 conversion DDL(`conversioncmds.c`/`ConversionCreate`/`T_CreateConversionStmt`/`OBJECT_CONVERSION`/psql `\dc`)；清 psql `\ds`、`CMDTAG_CREATE/ALTER/DROP_SEQUENCE` 死标签、`DISCARD_SEQUENCES`；保留 `pg_conversion` 目录与内置编码转换、`FindDefaultConversion`、运行期 `nextval/currval`。
- `CREATE/ALTER/DROP CAST`（08-24）：删 `CMDTAG_*_CAST`、gram.y 残留注释、`utility.c`/`dropcmds.c` 的 `OBJECT_CAST` 死分支；保留 `pg_cast` 目录/`CastCreate`/`OBJECT_CAST`/`OCLASS_CAST`（range→multirange 自动转换、级联删除仍用）。
- `ALTER COLLATION`（08-20，仅空壳）/ `CREATE/DROP/ALTER COLLATION`（08-18）：删 `DefineCollation/AlterCollation` 与语法；保留排序规则内核(`pg_collation` 预置表/`pg_locale.c`/`IsThereCollationInNamespace`/`pg_import_system_collations`)。
- `ALTER AGGREGATE`（08-20 整套 + 08-24 确认零残留）：删 `CMDTAG_ALTER_AGGREGATE` 与 RENAME/OWNER/SET SCHEMA 语法；保留 `OBJECT_AGGREGATE`、CREATE/DROP AGGREGATE、内置聚合。
- `ALTER FUNCTION/OPERATOR/OPFAMILY/STATISTICS/EXTENSION/DATABASE`（08-18）：删 7 类外围 ALTER 语法与实现；保留 `ALTER TABLE`(索引核心) 与 `ALTER TYPE/DOMAIN/ENUM`、`SET SCHEMA` 节点。
- `DO` 语句（08-24）：删 `DoStmt`/`InlineCodeBlock`/`ExecuteDoStmt`/`CMDTAG_DO`；保留 `DO` 关键字(ON CONFLICT DO UPDATE / CREATE RULE DO INSTEAD)。
- 游标 CURSOR（08-24）：删 DECLARE/FETCH/MOVE/CLOSE/`CurrentOfExpr`/`WHERE CURRENT OF`/`refcursor`；保留 Portal 机制、扩展查询协议、`DestTuplestore`、SPI。
- `ALTER ... OWNER TO`（08-18）：删整条语法链/`ExecAlterOwnerStmt`/`AlterObjectOwner_internal`/`T_AlterOwnerStmt`；保留 `RoleSpec`。

**gram.y 死规则清理**：`SetResetClause`(08-18)、`CREATE/ALTER SEQUENCE` 死链 `OptParenthesizedSeqOptList/SeqOptList/SeqOptElem/opt_by`(08-18)、`NumericOnly_list`/`any_with`/`opt_distinct_clause`(08-18)、`opclass_drop_list` 等 7 个 useless 非终结符(08-18)、`CREATE ASSERTION` 占位桩(08-18)、`CREATE TYPE` 基础形式残留。`ALTER TABLE LIKE`、`COMMENT ON`、`UNION/INTERSECT/EXCEPT`、`CTE` 等（功能模块裁剪，见十）。

## 三、客户端 / psql 裁剪

- **tab-complete 自动补全（09-01）**：删 `tab-complete.c/h`、Makefile，保留 readline 行编辑/历史。
- **\dAf / \dAo / \dAp（08-31）**：删 operator family 展示函数与 `command.c`/`help.c`/`tab-complete.c` 分支（不碰后端 pg_opfamily/pg_amproc/\dA/\dAc）。
- **\dRp / \dRp+ / \dRs（08-31）**：删逻辑复制发布/订阅展示函数与派发（后端已删）。
- **describe.c（09-01）**：`pg_class.relhastriggers`/`relhassubclass` 列删除后，7 版 `\d` 主查询去列、结果集列序号整体下移（含修正漏移 `relpersistence 12→11`/`relam 13→12`）、删 "Print triggers next" 块；角色视图 `\du/\dg`/`describeRoles`(08-18)；`\dD`(domain, 08-31)。
- **残留清理（08-31）**：`\dAf/\dAo/\dAp/\dRp/\dRs` 在 psql.sql/out 与 psql-ref.sgml 的测试/文档引用一并清除。

## 四、编码 / 加密 / 哈希裁剪

- **字符集精简（09-01）**：仅留 UTF8 / LATIN1(ISO-8859-1) / SQL_ASCII。`pg_enc` 枚举 41→3；删 `conversion_procs/` 除 `utf8_and_iso8859_1` 外的 25 子目录、`Unicode/`(约 20 万行)、孤立生成器 `iso.c/win1251.c/win866.c`、`conv.c`(死代码)；`pg_wchar_table[]`/`pg_enc2name_tbl[]`/`encoding_match_list[]` 锁步收缩；`pg_conversion.dat` 仅 2 行；`pg_proc.dat` 删被删转换函数；`mbutils.c`/`ascii.c` 去分支；恢复被误删的 `MAX_CONVERSION_INPUT_LENGTH`/`MAX_UNICODE_EQUIVALENT_STRING`(被 parser.c/varlena.c/mbutils.c 引用)；测试 conversion 仅测 UTF8↔LATIN1。**保留** `convert('café',UTF8,LATIN1)` 往返、非 LATIN1 字符正确报错。
- **Unicode 规范化**：见 1.8。
- **加密哈希（08-27）**：删 `md5.c/md5_common.c/md5_int.h/md5.h`、`hmac.c/h`、`sha1.c/sha1_int.h/sha1.h`、`cryptohashfuncs.c`(sha224/256/384/512 SQL)、`pg_proc.dat` 的 md5/sha2 注册、`cryptohash.h` 去 `PG_MD5/PG_SHA1` enum、`cryptohash.c` 去对应 case、`resowner.c` HMAC 引用、`typedefs.list`。**保留** `cryptohash.c/h` 框架(sha2 内部)、`sha2.c/sha2_int.h`、recovery 测试 `md5()`→`repeat('x',32)`。

## 五、认证 / 通信层裁剪

- **SASL / SCRAM（08-21）**：删 `scram-common.c`/`saslprep.c`/`fe-auth-scram.c` 与 `AUTH_REQ_SASL*` 宏、`channel_binding` 参数（服务端无条件信任，永不握手）。
- **libpq / 通信（08-21）**：仅 IPv4，删 `HAVE_IPV6` 及 ifaddr/fe-connect IPv6 死代码，保留 getaddrinfo 封装与 inet/cidr 的 IPv6 存储；删 `PQnotifies/PGnotify/PQfreeNotify` 与 psql 打印/补全/testlibpq2（LISTEN/NOTIFY 已删）；删 `AUTH_REQ_PASSWORD`/`pg_password_sendauth`；删 fe-trace.c Copy 追踪。验证 make check-world。
- **GSSAPI（08-17）**：删 `ENABLE_GSS` 死代码(backend_status/postmaster/wait_event 等)，保留 `AUTH_REQ_GSS` 常数稳布局。
- **initdb auth（08-21）**：删 `-A/--auth`/`--auth-host`/`--auth-local`，`PostgresNode.pm` 去 `-A trust`。
- **SSL/TLS（08-02）、Unix 域套接字 + IPv6 监听层（08-17）**：仅 IPv4 TCP，删 `HAVE_UNIX_SOCKETS` 全家、`USE_SSL` 后端状态、libpq 客户端 SSL 死代码。

## 六、索引相关死代码清理（GiST/SP-GiST/event trigger/logical replication 残留，08-31）

- `selfuncs.c` 删 `gistcostestimate/spgcostestimate`(AM 已删)；
- `cmdtaglist.h` 去每行 `event_trigger_ok` 字段、`cmdtag.h`/`cmdtag.c` 删 `command_tag_event_trigger_ok()`；
- `rewriteheap.c` 删 7 个 logical rewrite 死函数 + 2 结构 + 5 字段 + 3 调用点（保留 `HEAP_INSERT_NO_LOGICAL`）；
- `heapam_xlog.h` 删 `XLOG_HEAP2_REWRITE`/`xl_heap_rewrite_mapping`、`heapdesc.c`/`heapam.c` 去对应 case；
- `procarray.c` 删 `ProcArrayGetReplicationSlotXmin`（保留 `replication_slot_xmin/catalog_xmin` 字段）。

## 七、权限 / ACL 裁剪（08-15 ~ 08-18，最终收尾）

1. 删除 ACL 访问控制、`pg_policy`+RLS、RTE 权限位字段组(requiredPerms/checkAsUser/securityQuals，保留列修改位图)；
2. 删用户/角色/密码概念、外键 FK 语法、`superuser()`、`owner` 机制与 `aclchk.c` 调用壳；
3. 删 `acl.h` 死宏(ACL_ID_PUBLIC/ACLITEMOID/ACL_ALL_RIGHTS_* 等)、整文件 `git rm acl.h`(清 54 个冗余 `#include`)、空壳 `acl.c`(+Makefile)；
4. `regrole` 类型/角色骨架/`ALTER OWNER TO` 死链(08-18)；`ALTER DEFAULT PRIVILEGES`(08-18)；`GRANT/REVOKE` 标签(08-19)；角色视图 `pg_roles/pg_shadow/pg_group/pg_user` + `\du/\dg`(08-18)；`cmdtaglist.h` 23 条角色相关孤立标签。
- **保留**：对象变更位图、`BOOTSTRAP_SUPERUSERID`、核心 DDL 依赖的取 owner 适配层、仅存 ownercheck 语义（无对象级 ACL）。

## 八、表 / 存储裁剪

- **reloptions（08-21 核心 + 残余清理）**：删 reloptions 框架(`StdRdOptions`/`BTOptions`/`HashOptions`/`AutoVacOpts`/fillfactor 宏)，消费改硬编码；删 indexcmds/view/toasting/cluster/relcache/ruleutils/parse_utilcmd/fe_utils/psql 的 reloptions 残留。保留 `pg_class.reloptions` 列(视为 NULL)与 `amoptions` 接口。
- **Page Checksum（08-31）**：删 `checksum.c/h/impl.h`、`PageIsVerified*`/`PageSetChecksum*`、`ignore_checksum_failure`、pg_control 的 `data_checksum_version`、xlog.c `DataChecksumsEnabled`(XLogHintBitIsNeeded 退化为仅 wal_log_hints)、bufmgr/storage 读写路径校验分支、guc 与 pgstat 统计、initdb `-k`、pg_controldata/pg_rewind 列、pg_proc 两条统计函数、pageinspect `page_checksum`、PostgresNode `corrupt_page_checksum`、**保留 `pd_checksum` 字段**(不破坏页布局)。
- **tablespace（08-21）**：删用户自建表空间全部 DDL/`tablespace.c` 执行与 WAL rmgr/GUC/psql `\db`/SQL 函数（60+ 文件 ~5700 行）；保留 `pg_tablespace`/`pg_default`/`pg_global`、md.c/smgr spcNode 寻址、`reltablespace` 恒 0。
- **继承 / 触发器死壳（09-01）**：删 `has_subclass()/has_superclass()/typeInheritsFrom()`(tablecmds.c，planner.c `rte->inh=false`、rewriteDefine.c 删恒假 ereport)；删 `pg_class.relhassubclass` 列 + `SetRelationHasSubclass()` + `acquire_inherited_sample_rows()`(~215 行，analyze.c)；删 `pg_class.relhastriggers` 列 + 赋值/快速跳过守卫/触发器展示块（触发器全链路 08-20 已删，`pg_class.relhastriggers` 此前恒 false）；`system_views.sql` 去 `hastriggers` 列。**保留** 触发器 `TRIGGER` 关键字、FK 校验本体逻辑。
- **分区规划 GUC（08-20）**：删 `enable_partitionwise_join/pruning/aggregate` 三个死 GUC(AM 已裁至 heap/btree/hash)；修 relnode.c 引用已删字段 Assert。
- **EXCLUDE 约束（08-19）**：删语法/节点(`Constraint.exclusions`/`IndexStmt.excludeOpNames`)/catalog 列(`conexclop`/`indisexclusion`)/执行器冲突检测/pg_get_constraintdef 显示；保留 UNIQUE/PRIMARY KEY/CHECK 与 `constraint_exclusion` GUC。

## 九、查询协议 / 计划缓存裁剪（08-28）

- 彻底删扩展查询协议（Parse/Bind/Describe/Execute/Sync）：`postgres.c` 去全部 `'P'/'B'/'D'/'E'/'H'/'S'/'C'` case 与 `exec_parse/bind/execute/describe_*_message`/`errdetail_*/bind_param_error_callback`/`ignore_till_sync` 等；仅留简单查询 `'Q'` 与 fastpath `'F'`。
- 删 `plancache.c/h`；`functions.c/spi.c/extension.c/clauses.c` 改即时执行(`pg_parse_query`/`pg_analyze_and_rewrite_params`/`pg_plan_queries`)。
- 删 `prepare.c/h`(SQL PREPARE/EXECUTE/DEALLOCATE 语法层已先删)；`parsenodes.h` 去 PreparedStmt/PrepareStmt 节点；`utility.c/guc/portalmem/postinit/pquery/cmdtaglist/typedefs.list` 清理；删 `test_predtest`、`prepare.sql`/`plancache.sql`。
- `isolationtester.c` 改用 `PQexec` 简单协议（保留 58 用例，nowait-5 因依赖 SQL PREPARE 移出 schedule）。影响：基于扩展协议的客户端(pgbench -f/JDBC)不可用。

## 十、平台 / 构建链 / 过程语言 / 功能模块裁剪

- **平台**：仅 Linux，删 Windows/Cygwin/MSVC/EXEC_BACKEND 双实现(07-30)；删 pgbench/pg_upgrade 等运维 bin，保留 initdb/pg_ctl/psql/pg_dump(07-31)。
- **协议/加密**：SSL/TLS+GSSAPI(08-02)、物理流复制全链路 walsender/walreceiver/slot/syncrep/basebackup(08-13/08-12)、放弃 PG13 前兼容(08-15)。
- **过程语言**：删 plperl/plpython/tcl(08-03)、ecpg 嵌入式 SQL(08-15)、PL/pgSQL(08-14，含解析器钩子与 initdb 默认安装)、DO/回归转 SQL。
- **功能模块**：contrib 保留 11 删 45；删 `--with-selinux/perl/python`、BRIN、ICU、NLS、Bonjour/Systemd/XML、tsearch、JIT、pg_prewarm、异步 Append、UNION/INTERSECT/EXCEPT、CTE、CREATE TABLE LIKE、COMMENT ON 等；另有 ObjectProperty[] 错位与 aclchk 精简修复。
- **catalog/系统表**：删 pg_partitioned_table、pg_inherits+继承死代码、pg_sequence、序列残留全链、pg_class 的 relispartition/relpartbound/relreplident、`ALTER DATABASE SET`/`ALTER SYSTEM`/`REPLICA IDENTITY`、CREATE SEQUENCE 悬空语法等（含 syscache 联动与 genbki 重排）。
- **优化器表继承展开(08-15)**：删 inherit.c/inherit.h，`expand_appendrel_subquery` 迁 appendinfo.c，仅 RTE_SUBQUERY 走 appendrel 展开，保留 UNION ALL。
- **窗口函数(08-17)**：自底向上删 nodeWindowAgg/windowapi.h、解析(OVER/WINDOW)、优化器路径、`prokind='w'` 注册、window.sql；保留 `in_range_*` 帧函数、`CREATE FUNCTION ... WINDOW` 兼容项与聚合机制。
- **IANA 时区库（09-01）**：`src/timezone/data/tzdata.zi`(2026b，4302 行/108KB) 重写为最小集——仅 `UTC` 与 27 个固定偏移 `Etc/GMT±N`(`Etc/GMT-14`..`Etc/GMT+12`)及其别名链接，删除全部 `R` 规则、带夏令时的大洲/城市 `Z` 时区、非固定偏移 `L` 与注释；编译产物 `share/timezone` 由 1.5MB 降至约 120KB(41 文件)。`tznames/` 缩写集(`Default`/`Australia`/`India`)整体保留，但 `Default` 中 50 条引用已删时区的缩写条目(`MSK Europe/Moscow`、`VET America/Caracas`、`SGT Asia/Singapore` 等)被删除，并将测试依赖的 `MSK`/`VET` 转为纯偏移条目(`MSK 10800`/`VET -14400`)，避免 `pg_timezone_abbrevs` 因引用缺失时区而报错。回归测试 `horology`/`time`/`timestamp`/`timestamptz` 中被删 DST 时区(`America/New_York`/`Los_Angeles`/`Europe/Helsinki`/`Moscow`/`Prague`/`Asia/Singapore`/`Australia/Sydney`/`Pacific/Honolulu`)的引用改为 `Etc/GMT±N` 固定偏移或 UTC；`pg_regress.c` 默认 `PGTZ` 由 `America/Los_Angeles` 改为 `UTC`(确定且不依赖已删时区)；重新生成 `date`/`time`/`timestamp`/`timestamptz`/`horology`/`guc` 共 6 个测试的 `.out`。保留 `localtime.c`/`zic.c`/`pgtz.c`/`strftime.c` 解析编译引擎与 `pg_timezone_names()`/`pg_timezone_abbrevs()` 功能。注：`euc_kr`/`select_implicit`/`conversion` 三例失败为更早「字符集编码裁剪」的遗留(已移除 EUC_KR 与多字节宽度/部分非确定性输出)，与本次时区裁剪无关。

## 十一、其它死代码 / 调试桩 / 孤立函数清理

- **插件钩子与调试桩(08-24)**：删 `ProcessUtility_hook` + 4 个 Executor hook(改调 standard_*)、`RAW_EXPRESSION_COVERAGE_TEST`/`COPY_PARSE_PLAN_TREES` 条件编译块。
- **临时关系文件死链(08-21)**：删 `RemovePgTempRelationFiles`/`...InDbspace`/`looks_like_temp_rel_name`(fd.c)；保留 `RemovePgTempFilesInDir`/`OpenTemporaryFile`。
- **GUC**：`authentication_timeout`(08-21，附修复误删的 `StatementTimeoutHandler` 注册)、`db_user_namespace`(08-21)、relreplident 相关。
- **孤立函数/文件**：`jsonapi.c/h`(JSON 类型已删，零引用，08-27)、`domains.c` 的 `domain_in/domain_recv`(domain 已删，oid 2597/2598，08-27)、`alter.c/typecmds.c` 死代码(`AlterTypeNamespace`/`AlterObjectNamespace_oid` 及静态助手，08-27)、`CMDTAG_DROP_SUBSCRIPTION`(08-26)、`pg_index.indisreplident`(08-17)、`get_transform_fromsql/tosql`(08-17)、`fdw_handler` 伪类型(08-17)。
- **未裁声明**：`selfuncs_geo.c` 的 `areasel/positionsel/contsel` 经核查为活代码(`pg_proc.dat`/`pg_operator.dat` 有注册，fmgrtab 引用)，`git checkout` 恢复（08-27，教训：判定 .dat 注册函数是否死代码须 grep `pg_proc.dat prosrc` 与 `pg_operator.dat oprrest/oprjoin/oprcode`，不能仅凭后端 .c 调用方）。
- **编译器驱动清理**：`-Wunused-function` 清零调用 static(08-16)；误加 `static` 致与 pg_proc.dat/fmgrprotos.h 冲突的函数改全局(08-17)；`-Wdeclaration-after-statement` 与 psql 警告清理(08-17，后端+前端 0 warning)；`aclcheck_error*` 声明恢复、`AclResult` 死变量删(08-17)。
- **Historic MVCC 快照 + partitionwise agg 残余(08-28)**：删 `SNAPSHOT_HISTORIC_MVCC`/`IsMVCCSnapshot`、snapmgr 的 `HistoricSnapshot*`、heapam 的 `HeapTupleSatisfiesHistoricMVCC`、relcache `GetPgClassDescriptor`、planner partitionwise aggregation 残余(`patype`/`common_prefix_cmp`)，净删 ~440 行。

## 十二、聚合函数裁剪（09-02）

- **AGGREGATE 关键字死清理**：`kwlist.h` 删 `PG_KEYWORD("aggregate", AGGREGATE, UNRESERVED_KEYWORD, BARE_LABEL)` 一处；`gram.y` 删 `%token AGGREGATE`、`unreserved_keyword`、`bare_label_keyword` 列表三处。`aggregate` 退化为普通标识符（`WITHIN GROUP` 产生式与假设集聚合 `rank() WITHIN GROUP (ORDER BY ...)` 保留，不受影响）。
- **裁剪 31 条低价值内置聚合**（`pg_aggregate.dat` 96→65 条，同步 `sed -i` 删 `pg_proc.dat` 对应条目）：
  - 统计回归族：`regr_count/slope/intercept/r2/avgx/avgy/sxx/syy/sxy`（9）、`covar_pop/covar_samp`（2）、`corr`（1）；
  - 有序集族：`percentile_disc/percentile_cont`（float8/interval 变体 6）、`mode`（1）；
  - 布尔族：`bool_and/bool_or/every`（3）；
  - 整型位运算族：`bit_and/bit_or/bit_xor`（int2/int4/int8 各 3，共 9）。
- **同步删除失效 C 实现**（仅删上述聚合专属代码，净删约 1400 行）：
  - `float.c`：删 `float8_regr_accum/float8_regr_combine/float8_regr_sxx/syy/sxy/avgx/avgy/r2/slope/intercept`、`float8_covar_pop/covar_samp`、`float8_corr`（13 函数）；保留 `float8_accum/float8_avg`（variance/stddev 系列仍依赖）。
  - `orderedsetaggs.c`：删 `ordered_set_transition`（非 multi 变体）及 `percentile_disc_final/percentile_cont_float8_final/percentile_cont_interval_final/percentile_disc_multi_final/percentile_cont_float8_multi_final/percentile_cont_interval_multi_final/mode_final` 共 14 个函数 + 6 个仅被其调用的静态辅助（`float8_lerp/interval_lerp/percentile_cont_final_common/pct_info_cmp/setup_pct_info/percentile_cont_multi_final_common`）；保留 `ordered_set_transition_multi`、`hypothetical_rank/percent_rank/cume_dist/dense_rank_final` 及共享静态 `ordered_set_startup/ordered_set_shutdown/hypothetical_check_argtypes/hypothetical_rank_common`。
  - `bool.c`：删 `booland_statefunc/boolor_statefunc/bool_accum/bool_accum_inv/bool_alltrue/bool_anytrue` + `BoolAggState`/`makeBoolAggState`；`int8.c` 删 `int8inc_float8_float8`。
- **保留边界（不可删，否则破坏核心/假设集链路）**：
  - 整型位运算符 `int2and/int2or/int2xor`、`int4*`、`int8*`（`&`/`|`/`#` 表达式运算符实现，`pg_operator.dat` 指向，仅删 catalog 聚合条目、C 函数保留）；
  - `count/sum/avg/min/max`、`array_agg/string_agg`、`variance/stddev` 系列；
  - 假设集 `rank/dense_rank/percent_rank/cume_dist` + `ordered_set_transition_multi` + `WITHIN GROUP` 语法。
- **回归同步**：`groupingsets.sql` 删 `percentile_disc(0.5) within group (order by v)` 调用、同步 `expected/groupingsets.out`；`opr_sanity.sql` 注释改「max and min」、同步 `expected/opr_sanity.out`（删 bool_and/bool_or/every 行）。`make check` 主回归套件 68 用例全绿。

---

> 各裁剪的「保留项」「构建注意」「验证」细节已并入上述分组与「〇、通用构建注意」；逐文件清单见 `mydoc/CHANGE.full.md`（由完整历史版本留存）。
