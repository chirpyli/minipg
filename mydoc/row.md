# 复合类型与 ROW 裁剪方案

> 状态：**已执行（2026-09-05）**，主回归 51/51、隔离 50/50 全绿，详见 `mydoc/CHANGE.md` 第三十一条。下文为裁剪前的分析结论与执行步骤记录（部分行号为裁剪前状态）。
> 结论先行：**用户级复合类型 DDL（CREATE TYPE AS）已在早期裁剪中彻底移除；残余部分可分三层处理——ROW 行构造器/RowExpr/RowCompareExpr/expandedrecord 等可彻底裁剪；record 比较/哈希函数群及其 catalog 条目可裁但须保留 record_in/out/recv/send 四个 I/O 函数；表行类型（reltype）全链路与 RECORD 伪类型机制不可裁剪。**

---

## 一、现状盘点

上游 `/home/postgres/works/opensource/postgres` 中「复合类型」包含两套语义，本方案必须区分：

| 语义 | 说明 | minipg 现状 |
|---|---|---|
| 用户级复合类型 | `CREATE TYPE ... AS (...)`，relkind='c' 的独立 catalog 对象 | **已彻底移除**（`utility.c` 无 `CreateTypeStmt`，gram.y 无 `CompositeTypeStmt`，全后端无 `DefineCompositeType`） |
| 表隐式行类型 | 每张表在 pg_type 中自动生成一行类型（`pg_class.reltype`），是 whole-row 引用、`SELECT t`、SRF 返回 row 的根基 | **完整存活，属核心机制** |
| RECORD 伪类型 | OID 2249，系统 SRF（pg_stat 系列、pg_locks 等）动态行输出 | **完整存活，属核心机制** |
| ROW 行构造器 | `ROW(a,b)` / `(a,b)` 表达式语法，产生 `RowExpr` 节点 | **存活，裁剪目标** |

### 1.1 语法层（parser）

| 位置 | 内容 |
|---|---|
| `gram.y:350` | `%type <list> row explicit_row implicit_row ...` 声明 |
| `gram.y:5533-5548` | `row: ROW '(' expr_list ')' ...`、`explicit_row`、`implicit_row: '(' expr_list ',' a_expr ')'` 三条产生式 |
| `gram.y:5198-5210` | a_expr 规则中引用 explicit_row / implicit_row，构造 `RowExpr`（5200、5210） |
| `gram.y:6487` | ROW 出现在 TYPE_FUNC_NAME_KEYWORD 关键字表 |
| `gram.y:6867` | ROW 出现在 UNRESERVED_KEYWORD 关键字表（6868 的 ROWS 属于 FETCH/window 语法，**保留**） |
| `gram.y:4841-4853` | `row OVERLAPS row` 规则：OVERLAPS 操作数必须为行构造器，最终折叠为 `overlaps()` 函数调用（date.c:1424 / timestamp.c:1643），**不产生 RowExpr** |
| `parse_expr.c:226` | `transformExpr` 中 `case T_RowExpr` |
| `parse_expr.c:857-955, 1152` | RowExpr 比较/IS NULL 快速路径 |
| `parse_expr.c:1416, 1481, 1806, 2040, 2298` | FieldSelect 源为 RowExpr、多列赋值（multiassign）等处的 RowExpr 处理 |
| `parse_expr.c:2403-2593` | `transformRowCompare`：`(a,b)<(c,d)` 折叠为 **RowCompareExpr**（2593）——RowCompareExpr 唯一产生点 |
| `analyze.c:63, 827, 1053-1087` | `count_rowexpr_columns` 及多列赋值列数匹配 |
| `analyze.c:863`、`parse_target.c:706-787` | 多列赋值 `SET (a,b) = (...)` 构造 FieldStore/RowExpr（**依赖 implicit_row**） |
| `parse_clause.c:1867`、`parse_target.c:1788`、`parse_collate.c:327` | 各处 `case T_RowExpr` |
| `parse_target.c:1418-1586` | `expandRecordVariable`：whole-row Var 展开（**表行类型机制，不可裁**；仅内部 RowExpr case 随 RowExpr 删除） |
| `parse_coerce.c:662, 865, 966`、`parse_coerce.c:47` | RowExpr 强制转换与 `coerce_record_to_complex`（配合 appendrel 行类型转换） |

### 1.2 节点层（nodes）

- `parsenodes.h`：`RowExpr`、`RowCompareExpr` 定义（约 959/991 行附近注释即指 RowExpr）。
- `copyfuncs.c:1668, 3250`、`outfuncs.c:3218`、`readfuncs.c:853`、`equalfuncs.c:1812`：RowExpr 序列化。
- RowCompareExpr：`equalfuncs.c:569, 1815` 及 out/copy/read 对应函数。
- `nodeFuncs.c`：RowExpr 6 处 case（196/864/1059/1316/1942/2617/3101）、RowCompareExpr 多处 case。
- `makefuncs.c`、`rewriteManip.c:1272`（whole-row 调整）、`queryjumble.c:557/560`、`dependency.c:1917/1924`、`ruleutils.c:5630/6222/6335/6385/7074/7136/8192`：反编译与依赖收集。

### 1.3 运行时层（executor / utils）

| 位置 | 内容 | 定性 |
|---|---|---|
| `utils/adt/rowtypes.c` | record_in/out/recv/send（76/304/455/671）+ 比较/哈希群：record_cmp、record_eq..record_ge、btrecordcmp、record_image_*、btrecordimagecmp、hash_record、hash_record_extended（共 17 个 SQL 函数） | I/O **保留**；比较/哈希群**可裁** |
| `utils/adt/expandedrecord.c`（整文件） | expanded record 机制 | **整文件可裁**：13 个导出函数无任何外部 .c 调用者，唯一引用是 `execExprInterp.c:72` include 的内联宏快速路径（2912-2917）；原最大消费方 PL/pgSQL 已裁剪 |
| `utils/cache/typcache.c` | composite 相关 5 组：`load_typcache_tupdesc`(743)、TYPTYPE_COMPOSITE 分支(726-732,897)、`record_fields_have_equality/compare/hashing/extended_hashing` + `cache_record_field_properties`(848-880)、`lookup_rowtype_tupdesc_*` 族 + RECORD typmod 注册表(987-1151)、`SharedRecordTypmodRegistry`(1284-1457)、`TypeCacheRelCallback`(1488-1524) | 行类型 tupdesc 缓存与失效**保留**；record_fields_have_* 4 个标志位随比较函数群**可裁**；SharedRecordTypmodRegistry 与并行查询 session 机制绑定**保留** |
| `execExpr.h` | `EEOP_ROW`(176)、`EEOP_ROWCOMPARE_STEP/FINAL`(182-185)、`EEOP_WHOLEROW`(85)、`EEOP_FIELDSELECT`(191)、`EEOP_FIELDSTORE_DEFORM/FORM`(197-204)、`EEOP_CONVERT_ROWTYPE`(223)、`EEOP_NULLTEST_ROWISNULL`(148) | ROW/ROWCOMPARE **可裁**；WHOLEROW/FIELDSELECT/FIELDSTORE/ROWISNULL **保留**（表 whole-row）；CONVERT_ROWTYPE 见 §2.3 |
| `execExpr.c:1830` / `execExprInterp.c` | `case T_RowExpr → EEOP_ROW` 求值 | 可裁 |
| `arrayfuncs.c:4192-4227` | `hash_array` 对 RECORD 元素硬编码 `F_HASH_RECORD`（4223） | 随 hash_record 一并裁剪 |

### 1.4 catalog 层

| 位置 | 内容 |
|---|---|
| `pg_type.dat:81-98` | pg_type/pg_attribute/pg_proc/pg_class 四个系统表行类型：typcategory='C'、typinput=record_in、typoutput=record_out、typreceive=record_recv、typsend=record_send —— **不可裁** |
| `pg_type.dat:197-200` | `record` 伪类型（2249）与 `_record` 数组（2287）—— record 保留；`_record` 可选裁剪（record[] 场景已无意义） |
| `heap.c:932, 966, 1023-1213` | 建表时 TypeCreate 生成行类型（reltype 赋值 932、typinput 挂 record_* 970-973）—— **不可裁** |
| `pg_opclass.dat:60-65` | `btree/record_ops`、`hash/record_ops`、`btree/record_image_ops` —— **可裁** |
| `pg_amop.dat:562-591, 660-661`、`pg_amproc.dat:186-189, 305-309` | record 运算符与支持函数 —— **可裁** |
| `pg_proc.dat` | record_in(3643)/record_out(3646)/record_recv(3904)/record_send(3907) **保留**；record_eq 等 11 运算符函数、btrecordcmp、hash_record、hash_record_extended、record_image_* 全系（4366-4391 等）**可裁** |
| `pg_operator.dat` | `(record,record)` 的 =/</> 等 11 个运算符 —— **可裁** |

### 1.5 优化器 / 索引层（RowCompareExpr 链）

RowCompareExpr 由 `parse_expr.c:2593` 唯一产生，但下游被 btree 索引路径深度消费（注意：**btree 本体不可裁，裁的是依附其上的 RowCompare 优化分支**）：

- `indxpath.c:2264-3038`（match_special_index_operator、generate_rowcompare_pathkeys、RowCompareExpr 缩短重构 3038）
- `nodeIndexscan.c:1110, 1306`（索引扫描条件匹配）
- `selfuncs.c:2152-2162, 5880, 6177, 6220`（rowcomparesel）
- `costsize.c:4254`、`clausesel.c:877`、`createplan.c:4311`
- `clauses.c:907, 1099, 1149, 2880, 3020, 3066`（argisrow 处理）
- `parse_collate.c:345`、`nodeFuncs.c:199/868/1063/1320/1626`、`queryjumble.c:560`、`dependency.c:1924`、`ruleutils.c:7136/8192`

### 1.6 不可裁部分（核心机制证据）

- **表行类型**：`heap.c:932`（reltype 赋值）、`pg_class.reltype` 列、`typcache.c:751`（`Assert(rel->rd_rel->reltype == typentry->type_id)`）、`TypeCacheRelCallback(1488)` 失效回调、`relcache.c:448/4762`（tdtypeid 回退 RECORDOID）、`lsyscache.c:2046`（get_rel_type_id）。
- **whole-row / 字段访问**：`EEOP_WHOLEROW`（`SELECT t FROM t`、`t.*`）、FieldSelect（`(t).a`）、FieldStore（`UPDATE t SET (t).a = ...`，rewriteHandler.c:879-929 的 multiassign merge 随多列赋值裁剪，单字段 FieldStore 保留）、`expandRecordVariable`（parse_target.c:1476）。
- **系统 SRF 动态行**：`funcapi.c:244/271/392/945`（RECORD 返回类型解析 + BlessTupleDesc + assign_record_type_typmod）、`BlessTupleDesc`（execTuples.c:2065）被 pgstatfuncs/lockfuncs/xlogfuncs/commit_ts/twophase/pg_controldata/objectaddress 等大量系统函数使用、`spi.c:1055`。
- **tupconvert.c**：`convert_tuples_by_name` 唯一调用者是 `execExprInterp.c:3140`（EEOP_CONVERT_ROWTYPE）；`convert_tuples_by_position` 已无调用者（死代码，可顺手清理，但见 §2.3 决策）。

---

## 二、分层定性

### 2.1 可直接裁剪（约 2500+ 行）

| # | 项 | 理由 |
|---|---|---|
| A1 | gram.y：`row/explicit_row/implicit_row` 产生式与 `%type` 声明、ROW 关键字（6487/6867） | 用户无法再写行构造器后，RowExpr 无产生源 |
| A2 | OVERLAPS 规则改写：`row OVERLAPS row` → 专用产生式 `'(' a_expr ',' a_expr ')' OVERLAPS '(' a_expr ',' a_expr ')'` | 保留 date/timestamp 的 OVERLAPS 功能（与复合类型无关，只借用了行语法壳）；若嫌复杂可连 OVERLAPS 一并裁剪（推荐保留，改动仅 3 行） |
| A3 | `RowExpr` 节点全链（§1.2 全部 + parser/optimizer/rewrite 各 case） | 无产生源即死代码 |
| A4 | 多列赋值 `UPDATE ... SET (a,b) = (...)`（analyze.c multiassign、rewriteHandler 879-929 merge 分支、gram.y 相关产生式） | 唯一语法入口是 implicit_row；`(t).a = x` 单字段赋值走 FieldStore **保留** |
| A5 | `RowCompareExpr` 全链（§1.5 + execExpr EEOP_ROWCOMPARE_* + nodes 序列化 + execExpr.h d.rowcompare） | 唯一产生点 transformRowCompare 随 A1 死亡 |
| A6 | `expandedrecord.c` + `expandedrecord.h` + execExprInterp.c:72 include 及 2912-2917 快速路径 | PL/pgSQL 已裁，13 个导出函数零外部调用者，纯死代码；execExprInterp 快速路径改走既有 deformed 慢路径即可（删 if 分支） |
| A7 | rowtypes.c 比较函数群：record_cmp、record_eq..ge、btrecordcmp、record_image_cmp、record_image_eq..ge、btrecordimagecmp、hash_record、hash_record_extended（保留 record_in/out/recv/send） | 仅经 record opclass / 运算符调用，随 A8 死亡 |
| A8 | catalog 条目（**sed 删除**）：pg_opclass.dat record_ops 3 个、pg_amop.dat record 条目、pg_amproc.dat record 条目、pg_operator.dat record 运算符 11 个、pg_proc.dat record 比较函数 13 个、pg_type.dat `_record` | opclass 无用户入口（无复合类型即无 record 类型列索引） |
| A9 | arrayfuncs.c:4192-4227 F_HASH_RECORD hack、typcache.c:848-880 record_fields_have_*/cache_record_field_properties | 唯一服务对象是 record 比较/哈希 |
| A10 | `EEOP_ROW`（execExpr.h 枚举 + d.make_row 联合体字段 + execExpr.c:1830 + execExprInterp ExecEvalRow） | 无产生源 |

### 2.2 不可裁剪（学习价值高、机制核心）

| # | 项 | 理由 |
|---|---|---|
| B1 | 表行类型 reltype 全链（heap.c TypeCreate、pg_class.reltype ↔ pg_type.typrelid、typcache tupdesc 缓存 + TypeCacheRelCallback 失效、relcache tdtypeid） | 这是「类型系统 ↔ 存储元组描述符」双向绑定的核心教材，TupleDesc 生命周期、relcache 失效传播都在这条链上；删除等于重构整个类型/relcache 体系 |
| B2 | `record` 伪类型 + record_in/out/recv/send（rowtypes.c 剩余部分 + pg_type.dat 197-200） | 系统四张 bootstrap catalog 的行类型 typinput/typoutput 即此四函数（pg_type.dat:81-98）；同时是所有系统 SRF 动态行的 I/O 通道 |
| B3 | RECORD SRF 输出机制：funcapi RECORD 分支、BlessTupleDesc、assign_record_type_typmod、lookup_rowtype_tupdesc_*、spi.c、SharedRecordTypmodRegistry | pg_stat_* / pg_locks / pg_control_* 等大量系统函数的输出根基；SharedRecordTypmodRegistry 与并行查询 session/DSM 机制绑定，属通用并行基础设施 |
| B4 | EEOP_WHOLEROW、FieldSelect/FieldStore、EEOP_NULLTEST_ROWISNULL、expandRecordVariable | `SELECT t FROM t` / `t.*` / `(t).a` / `row IS NULL` 是表级语义，与 ROW 构造器语法无关 |

### 2.3 改造后可裁（本次暂保留，单独验证）

| # | 项 | 说明 |
|---|---|---|
| C1 | `EEOP_CONVERT_ROWTYPE` + `ConvertRowtypeExpr` + `tupconvert.c`（convert_tuples_by_name） | 产生点仅剩 appendrel（UNION ALL ALL 展平）whole-row 调整：appendinfo.c:303、prepjointree.c:2010、var.c:742、rewriteManip.c:1272。若验证后确认 `SELECT s.* FROM (SELECT ... UNION ALL ...) s` 场景可裁（连 tupconvert 一并删，顺手清理 convert_tuples_by_position 死函数），否则保留。建议：**先完成 A 层裁剪并回归，再单独立项处理 C1** |
| C2 | `parse_coerce.c` coerce_record_to_complex / RowExpr 强制转换分支 | 随 A3 裁掉 RowExpr 部分后复查残余 |

---

## 三、裁剪步骤（建议执行顺序）

> 约定：catalog `.dat` 一律用 `sed -i` 删行（genbki 自动重生成 fmgroids.h / fmgrtab.c）；每阶段完成后跑回归；删 `nodes.h` 的 `T_*` 枚举必须全量重编。

### 阶段 0：基线回归

```bash
make -j8
make -C src/test/regress check MAKELEVEL=1
make -C src/test/isolation check MAKELEVEL=1
```

记录基线通过数（当前应为主回归 54/54、隔离 50/50 量级）。

### 阶段 1：语法 + 节点层（A1–A5）

1. `gram.y`：删 `row/explicit_row/implicit_row` 三条产生式、`%type` 声明、TYPE_FUNC_NAME_KEYWORD 与 UNRESERVED_KEYWORD 中的 `ROW`；将 `row OVERLAPS row`（4841）改写为专用产生式（不产生 RowExpr，直接 `makeFuncCall(SystemFuncName("overlaps"), ...)`）。
2. `parsenodes.h`：删 `RowExpr`、`RowCompareExpr` 结构体及 `T_RowExpr`、`T_RowCompareExpr` 枚举值。
3. `nodes/`：删 `_copyRowExpr/_outRowExpr/_readRowExpr/_equalRowExpr/_copyRowCompareExpr/...` 及 nodeFuncs.c、makefuncs.c 各 case。
4. parser：删 `count_rowexpr_columns`（analyze.c）、parse_expr.c transformRowCompare 与 T_RowExpr case、多列赋值逻辑（analyze.c 827/863 区段、parse_target.c 706-787 区段）、parse_clause/parse_target/parse_collate 的 T_RowExpr case、parse_coerce RowExpr 分支。
5. optimizer/rewrite：删 clauses.c、prepjointree.c:2010、var.c:742、appendinfo.c:303、rewriteManip.c:1272 中的 RowExpr 分支（**只删 RowExpr 分支，保留 whole-row Var 分支**）；删 indxpath/nodeIndexscan/selfuncs/costsize/clausesel/createplan 的 RowCompareExpr 分支。
6. executor：删 execExpr.c:1830、execExpr.c:1924、execExprInterp.c EEOP_ROW / EEOP_ROWCOMPARE_STEP/FINAL case；execExpr.h 删枚举值与 d.make_row、d.rowcompare 字段；ruleutils.c/queryjumble.c/dependency.c 删对应 case。
7. **全量重编**（删了 T_* 枚举，make 的依赖跟踪不可信）：

```bash
find src -name 'objfiles.txt' -delete && find src -name '*.o' -delete && find src -name '*.a' -delete && make -j8
```

8. 回归验证。注意 `ClassifyUtilityCommandAsReadOnly()` 等按 T_* 枚举 switch 的地方若无遗漏则无运行时影响，但需 grep `T_RowExpr\|T_RowCompareExpr` 确认清零。

### 阶段 2：catalog 层（A8）

用 `sed -i` 删除以下 `.dat` 条目（先 grep 定位行号，逐文件确认后删除；**不要用 read_file/replace_in_file，.dat 视为二进制**）：

```bash
# pg_proc.dat：record_eq/ne/lt/gt/le/ge、record_image_eq..ge、btrecordcmp、btrecordimagecmp、hash_record、hash_record_extended
#   保留：record_in(3643)/record_out(3646)/record_recv(3904)/record_send(3907)
sed -i '/btrecordcmp/d' src/include/catalog/pg_proc.dat   # 逐函数名执行，勿用一条通配正则误删 record_in/out
# pg_opclass.dat：btree/record_ops、hash/record_ops、btree/record_image_ops
# pg_amop.dat：amopfamily 指向 record opclass 的条目（562-591、660-661 附近）
# pg_amproc.dat：btrecordcmp/btrecordimagecmp/hash_record/hash_record_extended 支持函数条目
# pg_operator.dat：oprleft/oprright 为 'record' 的 11 个运算符
# pg_type.dat：_record（2287）条目；可选删除
```

删除后 `make -j8` 让 genbki 重生成 fmgroids.h / fmgrtab.c（**保留 record_in/out/recv/send 条目**，否则系统表 typinput 悬空，bootstrap 崩溃）。

### 阶段 3：运行时层（A6/A7/A9/A10 + expandedrecord）

1. `rowtypes.c`：删 record_cmp、record_eq..ge、btrecordcmp、record_image_cmp、record_image_* 全系、hash_record、hash_record_extended 及其 PG_MODULE_MAGIC 区外的辅助静态函数；保留文件头、record_in/out/recv/send。
2. 删 `src/backend/utils/adt/expandedrecord.c`（rm）与 `src/include/utils/expandedrecord.h`；`execExprInterp.c` 删 72 行 include 与 2912-2917 快速路径分支（保留慢路径）。
3. `typcache.c`：删 record_fields_have_equality/compare/hashing/extended_hashing 四个标志与 cache_record_field_properties、lookup_type_cache 中相应计算分支（848-880、508/544/568/592/629 调用点）；**保留** load_typcache_tupdesc、lookup_rowtype_tupdesc_*、assign_record_type_typmod、SharedRecordTypmodRegistry、TypeCacheRelCallback。
4. `arrayfuncs.c`：删 4192-4227 的 RECORD 元素 hash hack。
5. `lsyscache.c:1436-1487`（若为 record opclass 辅助）与 `equivclass.c`、`nodeMergejoin.c` 中的 TYPTYPE_COMPOSITE 分支复查：仅删除「record 运算符等价类/合并连接」相关分支，保留通用逻辑。
6. 回归验证（含 psql 未改动则无需同步 tmp_install；若改了 psql/backend，参照惯例同步 `cp src/bin/psql/psql tmp_install/.../bin/psql`）。

### 阶段 4：收尾

1. 全库 grep 清零检查：`RowExpr`、`RowCompareExpr`、`EEOP_ROW\b`、`record_eq`、`hash_record`、`expandedrecord`、`RECORDOID`（RECORDOID 应仅剩 SRF/伪类型合法引用）。
2. 全量重编 + 主回归 + 隔离回归全绿。
3. 更新 `mydoc/CHANGE.md`：记录删减内容——ROW 行构造器语法、RowCompareExpr 及其 btree 索引优化分支、record 类型比较/哈希运算符与 opclass、expandedrecord 机制、多列赋值 `SET (a,b)=(...)`；保留说明——表行类型、RECORD 伪类型与 record I/O 函数、whole-row/FieldSelect/FieldStore。

---

## 四、回归验证

```bash
# 主回归（复用 tmp_install 快照，秒级启动）
make -C src/test/regress check MAKELEVEL=1
# 隔离测试
make -C src/test/isolation check MAKELEVEL=1
```

- 已知 `src/test/regress` 无 composite/row 专项用例（此前已裁/不存在），主要风险在既有用例中偶用的行比较、`t.*`、OVERLAPS 语法。
- 若误删 record_in/out 条目导致 bootstrap 崩溃：core 文件在 `/home/postgres/core-%e.%p`，gdb 定位后恢复 pg_type.dat 条目。
- 全库重编必须在删除 `T_*` 枚举后执行（见阶段 1 步骤 7），否则 up-to-date 误判 → bootstrap segfault。

---

## 五、风险清单与学习价值论证

| 风险 | 缓解 |
|---|---|
| 误删 record_in/out/recv/send（系统表行类型依赖） | 阶段 2 明确保留清单 + bootstrap 验证 |
| OVERLAPS 改写引入语法冲突（ROW 关键字删除后 `'(' a_expr ',' a_expr ')'` 与已有规则冲突） | 改写后 bison 编译即验证；若冲突难解，退化为同时裁剪 OVERLAPS（date/timestamp 的 overlaps 函数与 F_OVERLAPS_* ruleutils case 一并处理） |
| RowCompareExpr 分支散布 15+ 文件，漏删导致链接错误 | 按阶段 1 清单逐文件处理，最终 grep 清零 |
| expandRecordVariable / whole-row 误伤 | 只删 RowExpr case，Var/RECORD 分支保留 |
| EEOP 枚举值删除影响 execExprInterp 大 switch | 与 T_* 同批全量重编 |

**学习价值论证**（为何 B 层不裁）：

1. **表行类型是理解 PostgreSQL 类型系统的最佳切片**：`pg_class.reltype ↔ pg_type.typrelid` 双向指针、typcache 的 TupleDesc 惰性加载与 relcache 失效回调（TypeCacheRelCallback）、tdtypeid 在 TupleDesc/HeapTuple 中的传播——这一条链覆盖了「目录缓存、失效协议、类型注册」三大内核主题，学习价值极高。
2. **RECORD/SRF 动态行是系统函数输出的通用机制**：BlessTupleDesc → assign_record_type_typmod → RecordCacheArray 是「运行时类型注册」的范本；SharedRecordTypmodRegistry 是并行查询共享态设计的典型样本。
3. 反观 ROW 语法与 RowCompareExpr：属 SQL 标准边缘特性，仅在 btree 上叠加一个「多列短路比较」优化分支；expandedrecord 更是只为 PL/pgSQL 性能而生（PL/pgSQL 已裁）——三者学习价值低、死代码属性明确，优先裁剪。
