---
name: minipg死代码残留分析报告
overview: 分析 minipg（基于 PG14.23 裁剪版）当前代码库中，已裁剪功能遗留的惰性死代码与未清理引用（security_barrier/FDW/HistoricSnapshot/indisreplident/securityQuals 等），产出一份分析报告 + 可继续裁剪清单（不改代码）。
---

最终确认：`get_transform_fromsql`/`get_transform_tosql`（lsyscache.c:2168/2189）定义后**全树零调用者**——它们在原始 PG 中仅被 FDW 的 `GetFdwRoutine` 相关转换逻辑调用，FDW 已删后成为纯死函数。而 `get_transform_oid` 仍有调用者（functioncmds.c 创建 transform 时、pg_proc.c 创建函数时验证 transform 存在），所以 `pg_transform` catalog 本身作为"可被 CREATE TRANSFORM 写入、可被 get_transform_oid 查询"是存活的，但 `get_transform_fromsql/tosql` 读取转换函数指针的路径已死。

至此证据充分，已能构建完整分析报告。我有足够信息生成计划。按用户要求：仅分析报告+清单（写 mydoc/DEADCODE_ANALYSIS.md），不执行裁剪，死代码残留优先。

让我生成计划。由于这是分析任务（产出报告文件），不需要代码实现，但 plan mode 下仍只产出计划，待用户确认后落盘。我将用 create_plan 提交。

但要注意：当前已经是 plan mode，我应该调用 create_plan 工具。让我先梳理要写入报告的内容框架，作为计划产出。

报告核心发现汇总（已证实）：

1. **`pg_index.indisreplident` 惰性死列**：RELREPLICA IDENTITY 主字段已删，此字段写入恒 false、无消费者，纯死列。
2. **`HistoricSnapshotActive`/SetupHistoricSnapshot 等**：逻辑解码残留，仅 heapam_visibility.c 两处读取，但 SetupHistoricSnapshot 无调用者 → 恒 false 死路径。
3. **securityQuals / qual_security_level 残留注释/字段**：securityQuals 字段已删，仅剩注释与 qual_security_level（无来源、恒 0）。
4. **`pg_user_mapping` catalog 死残留**：所有 .c 零引用，catalog 表+typedef 保留但无任何读写者。
5. **`pg_transform` 部分死链**：`get_transform_fromsql/tosql` 零调用者（FDW 死链）；CreateTransform 可写但无实际消费者。
6. **partitionwise join/aggregate 不可达**：`consider_partitionwise_*` 永不被赋值，整条优化路径死代码；`enable_partitionwise_*` GUC + patype 路径为惰性死分支。
7. **`T_AlterOwnerStmt` 孤立死语法**：owner 已删，语法解析成功但执行体为空块 `{}`。
8. **`fdw_handler_in/out` 死类型 I/O**：无 FdwRoutine 消费者，仅登记 SQL 函数。
9. 未生效条件编译块：`ifaddr.c`/`fe-connect.c` 的 `HAVE_IPV6` 块（宏永未定义，编译期跳过，源码噪音）。

我将生成计划。由于任务产出是分析报告（mydoc/DEADCODE_ANALYSIS.md），这属于文档产出，无代码实现。计划的 todolist 应围绕"生成并落盘分析报告"组织，且需补充可能遗漏的扫描维度（用脚本/编译验证辅助）。

让我组织计划，调用 create_plan。

注意：用户要求最终报告写入 mydoc/。计划应聚焦"分析+产出报告"，且包括编译验证（grep 零引用、make -Wunused 验证）以确保结论可靠。

我将生成计划内容。由于是中文环境，全部用简体中文。todolist 用中文。

报告文件建议命名 mydoc/DEADCODE_ANALYSIS.md（与 CHANGE.md 区分）。

我整理最终计划 XML。现在调用 create_plan。

由于计划需包含技术章节（但本任务非代码实现，是分析+文档），我会包含 <tech> 描述分析方法与验证手段，<design> 不需要（非 UI 任务），<extensions> 不需要（无可用扩展），<todolist> 围绕报告生成。

但注意：plan mode 下 create_plan 工具被调用后，计划细节存用户目录，确认后自动带入。我不应在 plan mode 中输出计划内容。但本系统的指令是要我"生成最优 plan"并以 XML 格式 <plan_result> 输出。这与 create_plan 工具的语义略有冲突。

重新看系统提示：本消息是让我"生成 plan，结合 context"。且 response_format 要求"Strictly output in valid XML format"，用 <plan_result> 作为 root tag。这与最初的系统提示（plan mode，调用 create_plan 工具）不一致。

实际上，最近的 system_reminder 说"Focus on understanding user_query... generate the optimal plan"。而 response_format 明确要求输出 <plan_result> XML。最初的 plan_mode_reminder 说"present your plan by calling the create_plan tool"。

但在这次响应中，我收到的指令明确要输出 <plan_result> XML 格式（response_format 强约束）。且没有提供 create_plan 工具函数。我手头的工具只有 read_file/search_file/search_content/list_dir/RAG_search。

因此正确做法：直接以 <plan_result> XML 格式输出计划（分析报告计划），不调用不存在的 create_plan 工具。

我将生成 <plan_result>，包含 <req>（分析需求）、<tech>（分析方法与验证）、<todolist>（报告生成步骤）。不需要 <design>、<extensions>。

让我组织完整的计划 XML。

对于 todolist，任务应为：

1. 深度扫描死代码残留（按已确认维度 + 扩展扫描其他维度如零调用 static、未生效宏、孤立枚举）
2. 用编译验证辅助（grep 零引用、make -Wunused-function）
3. 撰写 mydoc/DEADCODE_ANALYSIS.md 分析报告（分区、优先级、风险等级）
4. （可选）更新 requirement/CHANGE 提示

实际上"分析报告+清单"是一次性产出，但与用户确认范围（死代码残留优先）相符。我会分阶段：扫描→验证→撰写报告。

让我写计划。需具体文件名 mydoc/DEADCODE_ANALYSIS.md。

最终 <plan_result>：

<req> 用户需求：分析 minipg 当前残留死代码与可裁剪项，产出分析报告+清单，不改代码。重点：死代码残留优先（security_barrier/FDW/HistoricSnapshot/indisreplident 等惰性残留）。

<tech> 分析方法：基于已裁剪历史（CHANGE.md），对已知惰性残留逐项 grep 全库引用，判断"真死代码/惰性死列/孤立死语法/编译期不生效宏"；用 make -Wunused-function 与 grep 零引用交叉验证；参考 PG14.23 原始源码判断功能是否为死代码、学习价值。产出报告写入 mydoc/DEADCODE_ANALYSIS.md。

报告结构：按功能域分组（逻辑复制/解码残留、FDW/外部数据残留、分区优化残留、owner/权限残留、已删功能孤立语法/枚举、未生效条件编译块、零调用 static 函数），每项含 文件:行号、残留原因、是否真死代码、裁剪风险等级（高/中/低）、优先级排序。

<todolist>

- item1: 系统扫描死代码残留（已确认维度 + 扩展扫描零调用 static、孤立枚举、未生效宏）
- item2: 用 grep 零引用与 make -Wunused-function 交叉验证每项结论
- item3: 撰写 mydoc/DEADCODE_ANALYSIS.md 分析报告（分组、文件:行号、风险等级、优先级）
- item4: 汇总可裁剪优先级清单与裁剪风险说明

注意：本任务不改代码，仅在 mydoc/ 产出文档。

我直接输出 plan_result XML。不要包含 design/extensions（无 UI、无扩展）。

让我精确写。我会确保 todolist 每项 <=20 字且动词开头。中文。

todolist:

1. id="scan-deadcode" deps="": 扫描死代码残留维度并定位文件行号
2. id="verify-deadcode" deps="scan-deadcode": 用 grep 与 -Wunused-function 验证结论
3. id="write-report" deps="verify-deadcode": 撰写 mydoc/DEADCODE_ANALYSIS.md 报告
4. id="summarize-priority" deps="write-report": 汇总可裁剪优先级清单与风险

这些符合 3-7 项、批处理合理。

现在输出。</think:6124c78e><plan_result>
<req>

## 用户需求

分析 minipg（PostgreSQL 14.23 裁剪版）当前代码中残留的死代码与可继续裁剪的项，产出一份**分析报告 + 清单**，本次任务**不直接修改代码**。

## 分析重点

- 以"死代码残留优先"：聚焦已被裁剪功能遗留的死代码 / 未清理引用（用户明确举例 security_barrier、FDW、HistoricSnapshot、indisreplident 等惰性残留）。
- 顺带发现、明显可整体裁剪的非核心模块作为附加项列出，但不展开价值判断。

## 产出物

- 一份分析报告，写入 `mydoc/DEADCODE_ANALYSIS.md`，至少包含：

1. 惰性死代码 / 未清理引用清单（按功能域分组，每项含：文件:行号、残留原因、是否真死代码、裁剪风险等级）。
2. 可继续裁剪项的优先级排序（高 / 中 / 低）。
3. 附加：顺带发现的可整体裁剪的非核心模块线索。

## 约束

- 不可裁部分（btree、hash 索引、事务）不纳入裁剪建议。
- 报告只描述现状与证据，不执行删改。
</req>

<tech>

## 分析方法

1. **基线依据**：以 `mydoc/CHANGE.md`（354 行裁剪历史）为线索，定位"保留项/未裁项/惰性残留"中已明确标注但暂缓清理的死代码（如 `pg_index.indisreplident`、`HistoricSnapshotActive`、securityQuals/qual_security_level、`pg_user_mapping`、`fdw_handler`、`partitionwise` 路径、`T_AlterOwnerStmt` 空执行块等）。
2. **全库引用扫描**：对每个候选符号用 `search_content` 做全 `src/` 引用扫描，区分：真死代码（零调用者）、惰性死列（写入恒定、无读者）、孤立死语法（语法解析成功但执行体为空）、编译期不生效宏（`#ifdef` 宏永未定义，仅源码噪音）。
3. **交叉验证**：

- 对疑似零调用函数，用 `make -C <dir> CFLAGS="-O0 -Wunused-function"` 重编确认 `-Wunused-function` 告警。
- 对 catalog / 类型残留，用 `grep` 确认 `.c` 中无任何读写者。

4. **参考原始源码**：对照 `/home/postgres/works/opensource/postgres`（PG14.23）判断某符号在上游的真实消费者，确认其在 minipg 中确已失去调用链（如 `get_transform_fromsql/tosql` 仅被已删 FDW 调用、`consider_partitionwise_*` 无赋值点）。

## 已证实的死代码残留（预探结论，待报告展开）

- **`pg_index.indisreplident` 惰性死列**：REPLICA IDENTITY 主字段 `relreplident` 已删，此字段在 `index.c` 写入恒 `false`、`relcache.c` 复制、无明显消费者，纯死列（`pg_index.h:45`、`index.c:617/1547/1559/3392`、`relcache.c:2124`、`tablecmds.c:5466`、`psql/describe.c:1834/1933`）。
- **`HistoricSnapshotActive` / `SetupHistoricSnapshot` / `TeardownHistoricSnapshot` / `HistoricSnapshotGetTupleCids` 逻辑解码残留**：`SetupHistoricSnapshot` 全树零调用者（仅 `snapmgr.c` 定义、`snapmgr.h` 声明），`HistoricSnapshotActive()` 恒返回 `false`，仅 `heapam_visibility.c:1614/1702` 读取、relcache.c/snapmgr.c 若干 `if` 恒假分支。
- **securityQuals / qual_security_level 残留**：`securityQuals` 字段 2026-08-17 已删，仅剩 `selfuncs.c`/`extended_stats.c` 注释与 `pathnodes.h:335-336` 的 `qual_security_level` 字段（无来源、恒 0；`planner.c:582`、`initsplan.c:813/829/1006`、`prepjointree.c:920`、`outfuncs.c:2101` 引用）。
- **`pg_user_mapping` catalog 死残留**：所有 `.c` 中 `UserMapping`/`CreateUserMapping`/`DropUserMapping` 零引用（仅 `objectaccess.h:90` 注释与 `typedefs.list` 保留），catalog 表无任何读写者。
- **`pg_transform` FDW 死链**：`get_transform_fromsql`/`get_transform_tosql`（`lsyscache.c:2168/2189`）零调用者（原仅 FDW 转换调用）；`CreateTransform`/`DropTransform` 可写 catalog 但无实际消费者；`fdw_handler_in/out`（`pseudotypes.c:99-100`）仅登记 SQL 函数、无 `FdwRoutine` 消费者。
- **partitionwise join/aggregate 不可达死路径**：`consider_partitionwise_join`/`consider_partitionwise_aggregate` 全树**无赋值点**（仅 `relnode.c:700` 一个恒假 `Assert` 与 `allpaths.c:2695` 注释）；`enable_partitionwise_join/aggregate` GUC 与 `planner.c` 的 `patype` 分支依赖已删的分区表，整条优化路径不可达。
- **`T_AlterOwnerStmt` 孤立死语法**：owner 机制已删，`gram.y:5686+` 保留语法，但 `utility.c:761/1204/1865/2375` 的执行体为**空块 `{}`**，解析成功但不执行任何操作。
- **未生效条件编译块（源码噪音）**：`ifaddr.c`（11 处 `#ifdef HAVE_IPV6`）、`fe-connect.c:1378`（`#ifdef HAVE_IPV6`）等宏在 minipg 构建中永未定义，编译期跳过，属可清理噪音。

## 报告结构（写入 mydoc/DEADCODE_ANALYSIS.md）

- 按功能域分组：逻辑复制/解码残留、FDW/外部数据残留、分区优化残留、owner/权限残留、已删功能孤立语法/枚举、未生效条件编译块、零调用 static 函数。
- 每项字段：文件:行号、残留原因、是否真死代码（是/惰性/孤立）、裁剪风险等级（高/中/低）、优先级排序。
- 末尾附"可裁剪优先级清单"（高→低）与"附加可整体裁剪模块线索"。

## 执行注意

- 只读分析，不改任何源文件；仅新增 `mydoc/DEADCODE_ANALYSIS.md`。
- 引用证据必须基于实际 grep / 编译结果，不臆测。
- 与 AGENTS.md 约定一致：sgml 文档残留不纳入本报告重点。
</tech>

<todolist>
<item id="scan-deadcode" deps="">扫描死代码残留维度并定位文件行号</item>
<item id="verify-deadcode" deps="scan-deadcode">用 grep 与 -Wunused-function 验证每项结论</item>
<item id="write-report" deps="verify-deadcode">撰写 mydoc/DEADCODE_ANALYSIS.md 分析报告</item>
<item id="summarize-priority" deps="write-report">汇总可裁剪优先级清单与风险说明</item>
</todolist>
</plan_result>