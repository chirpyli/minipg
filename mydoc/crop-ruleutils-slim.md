# 裁剪「ruleutils 查询反解析」死代码（瘦身）

## 一、背景与目标

`src/backend/utils/adt/ruleutils.c` 负责把系统中存储的表达式树 / 查询树还原为 SQL 文本。
在 PostgreSQL 中它同时服务两大场景：

1. **表达式/计划反解析**：`pg_get_indexdef`、`pg_get_constraintdef`、`pg_get_expr`，以及 `EXPLAIN` 打印计划中的表达式（`explain.c`）、`genam.c`、`tablecmds.c` 的调用。
2. **完整查询反解析**（`pg_get_viewdef` / `pg_get_ruledef` / `pg_get_triggerdef` / `pg_get_functiondef` 等）：由 `get_query_def()` 驱动的整棵 Query 打印。

minipg 已经删除**视图、规则(rewrite)、触发器、函数**，因此第 2 类的高层 SQL 入口（`pg_get_viewdef` 等）**全部消失**。本文件仍保留 6766 行，其中「查询打印」子系统是否还有存活调用方，需逐一确认。

**本次目标**：确认 `ruleutils.c` 在裁剪视图/规则/触发器/函数之后遗留的死代码边界，彻底删除之，并清理历史残留的不可达片段。

**结论（重要）**：本文件并非整体可删——表达式/计划反解析与 `EXPLAIN` 仍大量依赖它。真正可删的是**由 `get_query_def()` 驱动的查询打印子系统及只能由它到达的辅助函数**，合计约 **1270 行（约占全文 19%）**。其余约 5500 行仍为活代码。

## 二、可裁剪性判定

### 2.1 存活入口（必须保留）

| 入口 | 调用方 | 反解析对象 |
| --- | --- | --- |
| `pg_get_indexdef` | `pg_proc.dat`(1700/2600) → `pg_get_indexdef_worker` | 索引定义/表达式/谓词 |
| `pg_get_constraintdef` | `pg_proc.dat`(1703/2603) | 约束(仅 UNIQUE/PK) |
| `pg_get_expr` | `pg_proc.dat`(1706/2607) | 存储表达式 |
| `pg_get_indexdef_string` / `pg_get_indexdef_columns` | `tablecmds.c`(3856)、`genam.c`(181) | 索引定义/列名 |
| `pg_get_constraintdef_command` | `tablecmds.c`(3814) | 约束定义 |
| `deparse_expression` / `deparse_context_for` | `explain.c`、`tablecmds.c`、内部 | 表达式 |
| `select_rtable_names_for_explain` / `deparse_context_for_plan_tree` / `set_deparse_context_plan` | `explain.c`(475/476/1062/1093/1192) | EXPLAIN 计划反解析 |

### 2.2 判定：查询打印子系统为死代码

`get_query_def()`（查询打印的根）在 `ruleutils.c` 内只有 3 个调用点：

| 调用点 | 所属函数 | 状态 |
| --- | --- | --- |
| 3193 | `get_insert_query_def` | 自身在死区 |
| 5869 | `get_sublink_expr` | 见下 |
| 6009 | `get_from_clause_item`（子查询 RTE） | 只能由 `get_query_def` 体系到达 |

而 `get_sublink_expr()` 的**唯一**调用方是 `get_rule_expr()` 的 `case T_SubLink`（4784-4786）。
`T_SubLink` 只会出现在**被存储的表达式树**中（解析后、规划前）。在 minipg 中：

- **DEFAULT 列约束已删除**：`gram.y` 的 `ColConstraintElem`(1112) 只剩 `UNIQUE` / `PRIMARY`，`ConstraintElem`(1151) 亦然，无 `CONSTR_DEFAULT` / `CONSTR_CHECK` / `CONSTR_FOREIGN`；`parse_node.h` 的 `ParseExprKind` 已无 `EXPR_KIND_COLUMN_DEFAULT`。
- **索引表达式 / 索引谓词禁止子查询**：解析期即报错（`parse_expr.c` 1211/1214，`EXPR_KIND_INDEX_EXPRESSION` / `EXPR_KIND_INDEX_PREDICATE`）。
- **计划树不含 SubLink**：`EXPLAIN` 走 `get_rule_expr` 的 `case T_SubPlan`（4788），只打印 `(SubPlan N)`，不反解析子查询 Query。

因此存活的 deparse 入口（indexdef / constraintdef / expr / EXPLAIN）**不可能遇到 `SubLink`**，`T_SubLink` 分支不可达 ⇒ `get_sublink_expr` 不可达 ⇒ `get_query_def` 及其整个查询打印子系统不可达。

| 判断维度 | 结论 |
| --- | --- |
| 是否核心机制 | 否。仅是「查询文本还原」工具；内核执行/存储不依赖它 |
| 是否被核心路径调用 | 查询打印部分否；表达式/EXPLAIN 部分**是**（保留） |
| 与不可裁剪项关系 | 不影响 btree/hash、事务；`EXPLAIN`（学习价值高）保留 |
| 依赖方向 | 死区只被 `get_rule_expr` 的 `T_SubLink` 与自身内部调用 |
| 学习价值 | 低。属于「把树打回 SQL」的格式化工具，非内核机制 |

**裁剪后保留**（不得误删）：
- `pg_get_indexdef` / `pg_get_constraintdef` / `pg_get_expr` 全链路；
- `EXPLAIN` 计划反解析全链路（`deparse_context_for_plan_tree` / `set_deparse_context_plan` / `select_rtable_names_for_explain` / `set_deparse_plan` / `get_variable` / `get_name_for_var_field` / `find_param_referent` / `get_parameter`）；
- **列别名机制**：`set_deparse_for_query`（仍被 `get_name_for_var_field`(3893) 调用）、`set_rtable_names`（`rtable_names` 仍被 `get_variable`(3422) 使用）、`set_simple_column_names`、`set_using_names`、`has_dangerous_join_using`、`set_relation_column_names`、`set_join_column_names`、`identify_join_columns`、`colname_is_unique`、`make_colname_unique`、`expand_colnames_array_to`；
- `get_rule_expr` 除 `T_SubLink` 外的全部分支、`get_oper_expr`、`get_func_expr`、`get_func_sql_syntax`、`get_coercion_expr`、`get_const_expr`、`get_const_collation`、`processIndirection`、`printSubscripts`、`get_opclass_name`、`quote_identifier` 等。

## 三、涉及文件清单

| 文件 | 操作 | 说明 |
| --- | --- | --- |
| `src/backend/utils/adt/ruleutils.c` | MODIFY | 删除查询打印子系统函数定义与对应前向声明；删除 `get_rule_expr` 的 `T_SubLink` 分支；清理 `get_func_sql_syntax` 末尾不可达残留 |

本任务**不涉及**目录元数据（`pg_proc.dat`）、`catversion.h` 与回归测试期望，因为 SQL 入口 `pg_get_indexdef/constraintdef/expr` 全部保留。

## 四、死区函数清单（含行号，行号为当前文件）

| 行范围 | 函数 | 说明 |
| --- | --- | --- |
| 2419–2437 | `get_rtable_name` | 仅被 6143/6163（死区）调用 |
| 2584–2646 | `get_query_def` | 查询打印根 |
| 2647–2689 | `get_values_def` | 仅查询打印使用 |
| 2690–2715 | `get_select_query_def` | |
| 2716–2783 | `get_simple_values_rte` | |
| 2784–2843 | `get_basic_select_query` | |
| 2844–2985 | `get_target_list` | |
| 2986–3034 | `get_rule_sortgroupclause` | |
| 3035–3091 | `get_rule_orderby` | |
| 3092–3224 | `get_insert_query_def` | |
| 3225–3269 | `get_update_query_def` | |
| 3270–3317 | `get_update_query_targetlist_def` | |
| 3318–3372 | `get_delete_query_def` | |
| 5782–5888 | `get_sublink_expr` | 仅被 `T_SubLink` 调用 |
| 5889–5982 | `get_from_clause` | |
| 5983–6158 | `get_from_clause_item` | |
| 6159–6205 | `get_rte_alias` | 仅被死区调用 |
| 6206–6243 | `get_column_alias_list` | 仅被死区调用 |

合计约 **1270 行**，另加前向声明约 12 行、`T_SubLink` 分支 3 行。

对应前向声明（`ruleutils.c` 340/350/353/354/356/358/360/363/365/367/369/372/406/407/409/411/413）须一并删除。

## 五、具体步骤

1. **切断根入口**：删除 `get_rule_expr()` 中的
   ```c
   case T_SubLink:
       get_sublink_expr((SubLink *) node, context);
       break;
   ```
   及 `get_sublink_expr` 的前向声明。
2. **删除查询打印子系统**：按上表删除 18 个函数的定义与全部前向声明。注意保留 `set_deparse_for_query` / `set_rtable_names` / 列别名机制（它们仍被 `get_name_for_var_field`、`get_variable` 使用）。
3. **清理残留**：`get_func_sql_syntax()` 末尾存在不可达片段（当前 5555–5557，紧随 `F_RTRIM...` 分支的 `return true;` 之后、无 `case` 标签）：
   ```c
   appendStringInfoString(buf, "))");
   return true;
   ```
   属早期裁剪遗留，删除。
4. **消除编译警告**：全量重编译，确认无 unused-function / unused-variable / unreachable 警告；若删函数后出现未使用变量（如某些 `deparse_columns` 局部），一并清理。
5. **残留扫描与回归**（见下节）。

## 六、验证方式

1. **全量重编译**：本项目未启用头文件依赖跟踪，改动后须 `make clean && make` 全量重建，要求零编译警告。
2. **残留扫描**：确认 `get_query_def`、`get_select_query_def`、`get_from_clause`、`get_sublink_expr`、`get_rte_alias`、`get_rtable_name`、`get_column_alias_list` 等符号除有意保留项外无残留；确认 `T_SubLink` 不再被 `get_rule_expr` 处理。
3. **回归测试**：执行 `make check-world`，要求全部通过。重点关注：
   - `create_index`、`index_including`（依赖 `pg_get_indexdef`）；
   - `explain`（依赖 `EXPLAIN` 计划反解析与列别名机制）；
   - `constraints` / `alter_table` 等涉及 `pg_get_constraintdef` 的用例。
4. **功能抽查**（可选）：手工执行 `EXPLAIN (VERBOSE, COSTS OFF) SELECT ... `、`\d` 等价查询、`pg_get_indexdef` / `pg_get_constraintdef`，确认输出与裁剪前一致。

## 七、风险与注意事项

- **核心风险：`T_SubLink` 可达性判断**。若未来重新引入 DEFAULT 表达式、CHECK 约束或允许子查询的存储表达式，则该分支与查询打印子系统将重新变为必需。裁剪前应确认：
  - `gram.y` 的 `ColConstraintElem` / `ConstraintElem` 无 `CONSTR_DEFAULT` / `CONSTR_CHECK`；
  - `parse_expr.c` 对索引表达式/谓词仍禁止子查询；
  - 全库无 `deparse_expression` / `pg_get_expr` 作用于含子查询的表达式树。
- **切勿误删列别名机制**：`set_deparse_for_query` 只是调用方从 `get_query_def` 变为 `get_name_for_var_field`，`rtable_names` 仍被 `get_variable` 使用，删除会导致 `EXPLAIN` 输出回退为 `varN` 且编译失败。
- **`EXPLAIN` 的 `T_SubPlan` 分支必须保留**（`get_rule_expr` 4788），它是计划反解析而非查询反解析。
- **`get_func_sql_syntax` 的 `F_*` 分支**（OVERLAY/POSITION/SUBSTRING/TRIM）仍被表达式反解析使用，不得误删；仅清理末尾不可达片段。
- **收益**：本次为「死代码清理」，`ruleutils.c` 由 6766 行降至 **5436 行**，净减 **1330 行**。若目标是更大的行数削减，建议另行评估数组类型族、位图扫描族等候选（见 `mydoc` 讨论）。

## 八、执行结果（已完成）

- `src/backend/utils/adt/ruleutils.c`：6766 → **5436 行**（净减 1330 行）。删除 18 个查询打印函数 + `get_rule_list_toplevel`（因 `get_values_def` 删除而失去唯一调用方）+ 对应前向声明，切断 `get_rule_expr` 的 `T_SubLink` 分支，清理 `get_func_sql_syntax` 末尾不可达片段与陈旧注释。
- 全量重编译：**零警告**。
- `make check-world`：**通过**（regress 54 项、isolation 20 项全部通过）。
- 残留扫描：已删符号在 `src/backend`、`src/include` 中无残留。

### 附带修复（构建阻断遗留）

验证 `make check-world` 时发现两处**前次裁剪遗留**导致 `temp-install` 失败，与本次 ruleutils 改动无关，一并修复：

1. `src/test/modules/test_misc`：其 `Makefile` 引用已随 contrib 删除的 `contrib/contrib-global.mk`，且唯一 TAP 用例测试已裁剪的 `SET NOT NULL` / `CHECK` 约束。已删除该模块并从 `src/test/modules/Makefile` 的 `SUBDIRS` 移除。
2. `GNUmakefile.in`：`world` / `install-world` / `clean` / `distclean` / `check-world` / `checkprep` / `installcheck-world` 等递归列表仍包含已删除的 `doc` 与 `contrib`。已全部移除对应条目与 `-C contrib` 规则，并重新生成 `GNUmakefile`。

