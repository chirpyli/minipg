# RETURNING 功能裁剪方案

## 一、功能定位与裁剪理由

### 1.1 功能背景

RETURNING 是 PostgreSQL 自有扩展语法（非 SQL 标准），PG 8.2 引入（提交 `7a3e30e608a`），允许 INSERT/UPDATE/DELETE 在修改数据的同时返回结果集（典型用途：取回序列号/默认值/触发器修改后的值）。实现方式是分析层把 RETURNING 列表当作 SELECT 目标列变换（`transformReturningList`），执行层每处理完一行 DML 后用 `ExecProcessReturning` 对实际堆元组做投影输出。

### 1.2 裁剪判断

- **非内核核心功能**：完全复用 SELECT 的投影机制（`transformTargetList` + `ExecProject`），不涉及存储、事务、索引、锁等核心路径；
- **学习价值**：投影求值机制在 SELECT 路径已完整可学，RETURNING 本身只是"借道"投影，无独立内核知识点；
- **与不可裁部分零耦合**：不触碰 btree/hash 索引与事务机制；
- **语法接受面收窄**：删除后 `INSERT ... RETURNING` 报语法错误，DML 回归"仅返回命令标签"的朴素形态，符合 minipg 教学定位。

### 1.3 保留边界（重要）

| 相邻功能 | 处置 | 理由 |
|---|---|---|
| ON CONFLICT（upsert） | **保留** | 核心是 speculative insertion + 冲突检测，与 RETURNING 正交；isolation 套件 `insert-conflict-do-nothing` 等测试不依赖 RETURNING |
| 自动更新视图（auto-updatable views） | 保留 | 其 rewriter 改写主链路不依赖 RETURNING，仅删除 RETURNING 交互分支 |
| 规则系统 CREATE RULE | 保留 | 仅失去"规则动作带 RETURNING"能力，规则基础设施不动 |
| EvalPlanQual（并发更新重试） | 保留 | 服务于 UPDATE 的并发语义，与 RETURNING 无关 |
| 数据修改型 CTE（`WITH x AS (INSERT ...)`） | 保留 | CTE 中 DML 不带 RETURNING 仍可执行（只是无输出列），`PORTAL_ONE_MOD_WITH` 路径完整保留 |

---

## 二、影响面总览

代码引用全景（已逐点核实）：

| 层 | 文件 | 内容 |
|---|---|---|
| 语法 | gram.y、kwlist.h | 产生式、保留字 |
| 分析 | analyze.c、parse_node.h、parse_expr.c、parse_func.c、parse_agg.c | transformReturningList、EXPR_KIND_RETURNING |
| 节点 | parsenodes.h、plannodes.h、pathnodes.h、execnodes.h、copy/equal/out/readfuncs.c | 5 类字段 |
| 重写 | rewriteHandler.c、rewriteDefine.c | 规则 RETURNING 改写与校验 |
| 计划 | planner.c、createplan.c、setrefs.c、preptlist.c | returningLists 传递、引用修正 |
| 执行 | nodeModifyTable.c、execMain.c、execUtils.c、execParallel.c | ExecProcessReturning、投影初始化 |
| Portal/SPI | portal.h、pquery.c、spi.h、spi.c | PORTAL_ONE_RETURNING、SPI_OK_*_RETURNING |
| 前端 | psql/common.c | 仅注释 |
| 测试 | regress sql/out、isolation expected、worker_spi | 用例剥离 |

---

## 三、详细实施步骤

> 行号为当前基线（PG 14.23 裁剪版），实施时以符号名定位为准。

### 3.1 语法层

1. `src/backend/parser/gram.y`
   - 删 `returning_clause` 产生式（3238-3240）；
   - `InsertStmt` 产生式（3101-3108）：去掉 `opt_on_conflict` 后的 `returning_clause` 与 `$4->returningList = $6`；
   - `DeleteStmt` 产生式（3250-3259）：去掉 `where_or_current_clause` 后的 `returning_clause` 与 `n->returningList = $6`；
   - `UpdateStmt` 产生式（3283-3294）：去掉末尾 `returning_clause` 与 `n->returningList = $7`；
   - 删保留字列表中的 `| RETURNING`（6765，位于 reserved 段）。
2. `src/include/parser/kwlist.h`：删第 299 行 `PG_KEYWORD("returning", RETURNING, RESERVED_KEYWORD, AS_LABEL)`。
   - 注：上游 PG 14 中 RETURNING 为 UNRESERVED，minipg 已改为 RESERVED，此处按现状删除。
3. gram.y 改动后由 bison 重新生成 gram.c（make 自动）。

### 3.2 分析层

1. `src/backend/parser/analyze.c`
   - 删 `transformReturningList()` 整函数（1567-1611）及文件头声明（67）；
   - 删 4 处调用：`transformInsertStmt` 377、`transformUpdateStmt` 771-773、`transformDeleteStmt` 1472，以及 UPDATE 分析中 namespace 重置条件 `if (stmt->onConflictClause || stmt->returningList)`（758）收敛为仅 `onConflictClause`。
2. `src/include/parser/parse_node.h`：删 `EXPR_KIND_RETURNING` 枚举项（58）。
3. 删各处 `case EXPR_KIND_RETURNING:` 错误消息分支：
   - `parse_expr.c` 466、1686、2754；
   - `parse_func.c` 2217；
   - `parse_agg.c` 436。

### 3.3 节点层

删除字段及其序列化/拷贝/比较代码：

| 文件 | 位置 | 内容 |
|---|---|---|
| parsenodes.h | 144 | `Query.returningList` |
| parsenodes.h | 1148 / 1162 / 1176 | `InsertStmt` / `UpdateStmt` / `DeleteStmt` 的 `returningList` |
| plannodes.h | 50 | `PlannedStmt.hasReturning` |
| plannodes.h | 222 | `ModifyTable.returningLists` |
| pathnodes.h | 1686 | `ModifyTablePath.returningLists` |
| execnodes.h | ResultRelInfo | `ri_projectReturning`、`ri_returningList`、`ri_ReturningSlot`（见 3.6 改名保留） |
| copyfuncs.c | 213 | `_copyModifyTable` 的 `COPY_NODE_FIELD(returningLists)` |
| copyfuncs.c | 2557 / 2597 / 2611 / 2625 | `_copyInsertStmt` / `_copyUpdateStmt` / `_copyDeleteStmt` / `_copyQuery` 的 `returningList` |
| equalfuncs.c | 870 / 906 / 918 / 930 | `_equalInsertStmt` / `_equalDeleteStmt` / `_equalUpdateStmt` / 相邻 DML 节点的 `COMPARE_NODE_FIELD(returningList)` |
| outfuncs.c | 411 / 1879 / 2547 | `_outModifyTablePath` / `_outModifyTable` / `_outQuery` 的 `WRITE_NODE_FIELD` |
| readfuncs.c | 267 / 1375 | `_readQuery` / `_readModifyTable` 的 `READ_NODE_FIELD` |

> readfuncs 反序列化 pg_rewrite 中存储的规则树；全量重编 + 重新 initdb 后不存在旧格式数据，无兼容问题。

### 3.4 重写层

1. `src/backend/rewrite/rewriteHandler.c`
   - `rewriteRuleAction()`（72 声明、333-344）：删 `bool *returning_flag` 出参；
   - 删规则动作 RETURNING 改写块（568-600：触发查询无 RETURNING 则丢弃规则动作的 RETURNING、有则 `ReplaceVarsFromTargetList` 改写、多规则 RETURNING 报错逻辑）；
   - 删自动更新视图中 RETURNING 列表 NEW 值改写（1460-1467 的 `copyObject` + `ChangeVarNodes`）；
   - `fireRules()`（1897 起）：删 `returning_flag` 参数及 3186 处传参；
   - `RewriteQuery()`（3046）：删局部 `bool returning`。
2. `src/backend/rewrite/rewriteDefine.c`
   - 删规则定义校验块（433-461：`haveReturning` 重复/条件规则/非 INSTEAD 规则的 RETURNING 检查）；
   - `checkRuleResultList()`（603-649）**函数保留**（SELECT 目标列校验仍用），仅删其中 RETURNING 特有的报错分支与 636 行 `RETURNING list has too many entries` 消息。

### 3.5 计划器

1. `src/backend/optimizer/plan/planner.c`
   - 414：删 `result->hasReturning = ...`；
   - 638-639：删 `preprocess_expression` 对 `returningList` 的预处理；
   - 1397、1454-1465、1487-1488、1500-1501、1525：删 `returningLists` 局部构建（继承/分区多目标表逐 rel 拷贝改写、单表 `list_make1`）及向 `create_modifytable_path` 的传参。
2. `src/backend/optimizer/plan/createplan.c`
   - `create_modifytable_path()`（276）：删 `returningLists` 形参；2415 调用点同步；
   - `create_modifytable_plan()`（5978）：删形参、5988-5989 断言、6041 赋值。
3. `src/backend/optimizer/plan/setrefs.c`
   - 删 `set_returning_clause_references()` 整函数（167 声明、2737-2765 定义）；
   - 删 `set_plan_refs` 中调用块（913-938）。
4. `src/backend/optimizer/prep/preptlist.c`
   - 删 RETURNING resjunk 块（204-239：为引用其它关系的 Var 补 resjunk 列）。

### 3.6 执行器

1. `src/backend/executor/nodeModifyTable.c`（改动最集中）
   - 删 `ExecProcessReturning()` 整函数（153-183）及三处调用：`ExecInsert` 尾部 669、`ExecDelete` 920、`ExecUpdate` 1219；
   - **三个 DML 函数签名简化为 void**（对齐上游 PG 17 方向）：
     - `ExecInsert()`（461）：删 `TupleTableSlot *result` 局部与返回，固定 `return`；
     - `ExecDelete()`（693）：删 `processReturning` 形参与 RETURNING 输出块（含 908 `ExecGetReturningSlot`、`failed to fetch deleted tuple for DELETE RETURNING` 分支、`ExecMaterializeSlot`）；
     - `ExecUpdate()`（963）：删尾部 RETURNING 返回块；
   - `ExecOnConflictUpdate()`（68 声明、552-562 调用点、1225 定义）：删 `TupleTableSlot **returning` 出参，内部 1428 `*returning = ExecUpdate(...)` 改为普通调用；调用点 `return returning;` 改 `return;`（成功冲突更新后该行无输出）；
   - `ExecModifyTable()` 主循环（1636-1715）：`slot = ExecXxx(...)` 改普通调用，删 `if (slot) return slot;`；
   - `ExecInitModifyTable()`：删 RETURNING 投影初始化块（1954-1991），其 else 分支（dummy result tuple type，TTSOpsVirtual）转正为唯一路径；
   - 清理 2135 附近 `es_auxmodifytables` 注释中 "don't throw away RETURNING rows" 的表述。
2. `src/backend/executor/execMain.c`
   - 305-310：`sendTuples = (operation == CMD_SELECT || ...hasReturning)` 收敛为仅 `CMD_SELECT`；
   - 844、847：删 `ri_projectReturning` / `ri_ReturningSlot` 初始化。
3. `src/backend/executor/execUtils.c` + `executor.h` + `execnodes.h`
   - `ExecGetReturningSlot()`（execUtils.c 1124-1143、executor.h 560）**功能保留但改名**：ON CONFLICT DO NOTHING 路径仍以它获取与目标表描述符一致的 scratch slot 做冲突元组可见性复查（nodeModifyTable.c 579 `ExecCheckTIDVisible`）；
   - 改名方案：`ri_ReturningSlot` → `ri_ConflictSlot`，`ExecGetReturningSlot` → `ExecGetConflictSlot`（避免已裁功能名误导学习；涉及 execnodes.h/execMain.c/execUtils.c/executor.h/nodeModifyTable.c 五处）。
4. `src/backend/executor/execParallel.c`：删 175 行 `pstmt->hasReturning = false;`。

### 3.7 Portal 与 SPI

1. `src/include/utils/portal.h`：删 `PORTAL_ONE_RETURNING` 枚举值（66）及注释（45、53、138）。
2. `src/backend/tcop/pquery.c`
   - `ChoosePortalStrategy()`（261-300）：删 `hasReturning` 判定与 `return PORTAL_ONE_RETURNING`，带 canSetTag 的单条 DML 统一走 `PORTAL_ONE_MOD_WITH`；
   - 删各 switch 中的 `case PORTAL_ONE_RETURNING:`（496、706、911）及注释（117、804、829、894）；
   - `FetchPortalTargetList()`（368-372）：删 hasReturning 分支。
3. `src/include/executor/spi.h`：删 `SPI_OK_INSERT_RETURNING` / `SPI_OK_DELETE_RETURNING` / `SPI_OK_UPDATE_RETURNING` 三常量。
4. `src/backend/executor/spi.c`：删 `_SPI_pquery` 中三处分支（1952-1968）及 1987 的 `hasReturning` 条件（保留 `SPI_OK_SELECT` 判定）。
5. `src/bin/psql/common.c`：969 行注释去 RETURNING 表述（无逻辑依赖）。

### 3.8 死代码顺带清理

- `src/backend/tcop/utility.c`：删 `QueryReturnsTuples()`（940-960，`#ifdef NOT_USED` 死代码，引用 returningList）。

### 3.9 测试同步

**调度内（必须同步，影响 check-world）**：

| 文件 | 改动 |
|---|---|
| `regress/sql/insert.sql` | 删 returningwrtest 整块（434-445：分区表 wholerow RETURNING 两个用例 + 建表）；清理 261 行残留注释 `-- check that RETURNING works correctly with tuple-routing`（该段实际 INSERT 语句已无 RETURNING） |
| `regress/expected/insert.out` | 同步删除对应输出 |
| `regress/sql/tsrf.sql` | 删 106-107 `-- SRFs are not allowed in RETURNING` 用例 |
| `regress/expected/tsrf.out` | 同步删除（原期望 ERROR "set-returning functions are not allowed in RETURNING"，裁剪后变为语法错误，无保留价值） |

**调度外孤儿文件（不在 parallel_schedule，不参与运行；按"不留残骸"原则剥离 RETURNING 用法）**：

- `update.sql`（108-152 ON CONFLICT+RETURNING、257 起 wholerow RETURNING）：剥离 `RETURNING ...` 子句，保留 ON CONFLICT 语句本体与 `:show_data` 验证；
- `arrays.sql`（163-170）：去 RETURNING 子句；
- `fast_default.sql`（335-338）：`DELETE ... RETURNING *` 改普通 DELETE；
- `create_function_3.sql`（370-372）：`voidtest4` 为 INSERT...RETURNING f1 的 SQL 函数，整用例删除；
- `rangefuncs.sql`（430 起 insert_tt 系列）：SQL 函数依赖 RETURNING 返回 serial 值，整段删除；
- `indirect_toast.sql`（20-28）：去 RETURNING 子句（该文件本就因 TOAST 间接路径崩溃不在调度内）；
- 上述文件对应的 `expected/*.out` 同步。

**其它**：

- `isolation/expected/eval-plan-qual.out`：spec 已删的孤儿 expected（56 处 RETURNING），删除文件；
- `src/test/modules/worker_spi/worker_spi.c`：当前不在 modules `SUBDIRS`（仅 delay_execution、test_misc 构建），但其 173-291 行使用 `DELETE/UPDATE ... RETURNING` 与 `SPI_OK_DELETE/UPDATE_RETURNING`，同步改为普通 DML + SELECT 并更新 expected，保持树内一致性。

### 3.10 文档

`doc/` 目录已整体裁剪，无 sgml 残留，无需处理（sgml 类改动本也不记 CHANGE.md）。

---

## 四、实施顺序与构建注意

1. **先删头文件字段/枚举**（parsenodes/plannodes/pathnodes/execnodes/parse_node.h/portal.h/spi.h/kwlist.h），再删各 .c 引用，让编译器顺藤摸瓜暴露全部引用点；
2. **必须全量重编**：本次同时删除 `kwlist.h` 关键字（ScanKeyword 编号前移）、`EXPR_KIND_*` 枚举、`PortalStrategy` 枚举、SPI 结果码、多处结构体字段——均属 CHANGE.md 通用注意 #1 的高危枚举前移场景，残留旧 `.o` 会导致运行时错位（cmdtag/syscache 类崩溃先例）；
   ```sh
   make clean && make -j8
   ```
3. 清理 `tmp_install` 旧副本后 `make check-world`（测试框架自动重建 temp-install 并重新 initdb）；
4. 若 temp-install 的 `rm -rf` 触发 IDE 确认，按既有经验临时注释 `src/Makefile.global` 的 temp-install rm 行，跑完还原。

---

## 五、验证清单

1. **编译**：`make clean && make -j8` 零警告零错误；
2. **回归**：`make check-world` 全绿（regress 主套件 + isolation 58-66 用例 + delay_execution/test_misc 模块）；
3. **反向 grep（全库 0 命中）**：
   ```sh
   rg -w "RETURNING" src/ contrib/                      # SQL 关键字（注释除外，逐一人工确认）
   rg "returningList|returningLists|hasReturning" src/
   rg "EXPR_KIND_RETURNING|PORTAL_ONE_RETURNING" src/
   rg "SPI_OK_[A-Z]+_RETURNING" src/
   rg "ExecProcessReturning|transformReturningList|set_returning_clause_references" src/
   rg "ri_projectReturning|ri_returningList|ExecGetReturningSlot|ri_ReturningSlot" src/
   ```
4. **行为抽查**（psql 手工）：
   - `INSERT INTO t VALUES(1) RETURNING *;` → `ERROR: syntax error at or near "RETURNING"`；
   - `INSERT/UPDATE/DELETE` 普通执行 → 命令标签 `INSERT 0 1` 等正常；
   - `INSERT ... ON CONFLICT DO UPDATE/DO NOTHING`（isolation 套件覆盖）正常；
   - `WITH x AS (INSERT ...) SELECT` / 自动更新视图 UPDATE 正常；
5. **收尾**：回归全绿后更新 `mydoc/CHANGE.md`，记录裁剪范围（建议归入"执行器/语法裁剪"章节：RETURNING 语法、transformReturningList、ModifyTable returningLists 链路、PORTAL_ONE_RETURNING、SPI_OK_*_RETURNING、ExecGetReturningSlot 改名、测试同步清单）。

---

## 六、风险评估

| 风险 | 缓解 |
|---|---|
| 枚举/字段前移导致运行时错位 | 强制 `make clean` 全量重编 + 清 tmp_install（既有踩坑经验） |
| ON CONFLICT 路径误伤 | 仅删 RETURNING 输出逻辑；speculative insertion、`ExecCheckTIDVisible`、`ri_ConflictSlot`（原 ExecGetReturningSlot）完整保留，isolation `insert-conflict-do-nothing` 兜底验证 |
| Portal 策略简化引入边界问题 | DML 统一走 PORTAL_ONE_MOD_WITH（该路径本就处理 rewrite 追加的辅助查询），CTE 中 DML 与触发器行为不变 |
| readfuncs 反序列化旧 catalog | 全新 initdb，不存在旧数据 |
| 孤儿测试文件改动扩大化 | 仅剥离 RETURNING 用法，不重构文件；调度内文件改动最小化并同步 expected |
