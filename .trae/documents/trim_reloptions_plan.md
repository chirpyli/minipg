# Relation Options 裁剪实施方案

## 调研结论

### 当前 Reloptions 体系包含：
- **核心框架**：reloptions.c/h（注册、解析、映射机制）
- **Heap 选项**：StdRdOptions（fillfactor, toast_tuple_target, autovacuum_*, parallel_workers, user_catalog_table, vacuum_truncate, vacuum_index_cleanup）
- **Btree 选项**：BTOptions（fillfactor, vacuum_cleanup_index_scale_factor, deduplicate_items）
- **Hash 选项**：HashOptions（仅 fillfactor）
- **View 选项**：ViewOptions（security_barrier, check_option）
- **Attribute 选项**：AttributeOpts（n_distinct, n_distinct_inherited）
- **AutoVacOpts**：autovacuum 子结构（14 个字段）

### 裁剪策略：
- **保留**：核心 reloptions 框架 + 各 AM 的 fillfactor（heap/btree/hash 不可裁剪）
- **删除**：所有非 fillfactor 的选项及其消费者代码

---

## 裁剪后保留的内容

### 1. 结构体简化

**StdRdOptions**（rel.h:268-278）：
```c
// 裁剪前：fillfactor + toast_tuple_target + AutoVacOpts + user_catalog_table + 
//          parallel_workers + vacuum_index_cleanup + vacuum_truncate
// 裁剪后：仅保留 fillfactor
typedef struct StdRdOptions {
    int32 vl_len_;
    int   fillfactor;
} StdRdOptions;
```

**BTOptions**（nbtree.h:1087-1093）：
```c
// 裁剪前：fillfactor + vacuum_cleanup_index_scale_factor + deduplicate_items
// 裁剪后：仅保留 fillfactor
typedef struct BTOptions {
    int32 varlena_header_;
    int   fillfactor;
} BTOptions;
```

**HashOptions**：不变（已只有 fillfactor）

### 2. reloptions.c 简化
- 保留所有通用基础设施（parseRelOptions, build_reloptions, transformRelOptions 等）
- 仅注册 fillfactor 选项（heap/btree/hash 各一个）
- 删除 autovacuum/toast/vacuum 等选项定义
- default_reloptions() 映射表仅包含 fillfactor
- 删除 view_reloptions()、attribute_reloptions()
- 删除 view/attribute 相关选项定义

### 3. reloptions.h 简化
- 删除 AutoVacOpts、ViewOptions、AttributeOpts 类型引用
- 删除相关宏声明
- 保留核心 relopt_* 类型和 API 声明

---

## 需要修改的文件

### 核心选项定义文件
| 文件 | 修改内容 |
|------|----------|
| `src/include/access/reloptions.h` | 删除 ViewOptions、AttributeOpts 引用；精简 API 声明 |
| `src/include/utils/rel.h` | 简化 StdRdOptions（删除 AutoVacOpts 等）；删除 parallel_workers、toast_tuple_target、user_catalog_table 等宏 |
| `src/include/access/nbtree.h` | 简化 BTOptions；删除 BTGetDeduplicateItems 宏 |
| `src/include/access/hash.h` | 不变 |
| `src/include/utils/attoptcache.h` | 删除 AttributeOpts 结构体；删除 get_attribute_options 声明 |
| `src/backend/access/common/reloptions.c` | 大幅精简：仅保留 fillfactor 选项注册；删除 autovacuum/view/attribute 选项；删除 view_reloptions()、attribute_reloptions()；default_reloptions() 仅映射 fillfactor |
| `src/backend/access/nbtree/nbtutils.c` | btoptions() 仅保留 fillfactor 映射 |
| `src/backend/access/hash/hashutil.c` | 不变 |

### 消费者代码修改
| 文件 | 修改内容 |
|------|----------|
| `src/backend/access/heap/hio.c` | 不变（仅使用 fillfactor） |
| `src/backend/access/heap/heaptoast.c` | 删除 toast_tuple_target 读取，使用硬编码默认值 |
| `src/backend/access/heap/heapam.c` | 不变（仅通过 hio.c 使用 fillfactor） |
| `src/backend/access/heap/rewriteheap.c` | 不变（仅使用 fillfactor） |
| `src/backend/access/nbtree/nbtinsert.c` | 删除 BTGetDeduplicateItems 调用 |
| `src/backend/access/nbtree/nbtsort.c` | 删除 BTGetDeduplicateItems 调用 |
| `src/backend/postmaster/autovacuum.c` | 删除 AutoVacOpts 依赖，全部使用 GUC 默认值 |
| `src/backend/optimizer/util/plancat.c` | 删除 RelationGetParallelWorkers 调用，使用 -1 |
| `src/backend/commands/analyze.c` | 删除 AttributeOpts / n_distinct 覆盖逻辑 |
| `src/backend/commands/tablecmds.c` | 简化 reloptions 校验（删除 view 相关路径） |
| `src/backend/rewrite/rewriteHandler.c` | 删除 security_barrier、check_option 依赖 |
| `src/backend/parser/parse_clause.c` | 删除 RelationIsUsedAsCatalogTable 检查 |
| `src/backend/commands/tablecmds.c` (L3590) | 删除 RelationIsUsedAsCatalogTable 检查 |
| `src/backend/statistics/extended_stats.c` | 删除 n_distinct 覆盖逻辑 |
| `src/backend/utils/cache/relcache.c` | 删除 view_reloptions、attribute_reloptions 相关路径 |
| `src/backend/utils/attoptcache.c` | 删除 get_attribute_options 实现 |

### 可能需要修改的其他文件
| 文件 | 修改内容 |
|------|----------|
| `src/include/nodes/pg_list.h` | 无变化 |
| `src/include/catalog/pg_class_d.h` | 无变化（reloptions 字段保留） |
| 回归测试期望文件 | 更新 select_views.out、create_view.out 等 |

---

## 实施步骤

### 第一阶段：简化核心定义
1. 简化 `rel.h` 中的 StdRdOptions，删除 AutoVacOpts 子结构和非 fillfactor 字段
2. 简化 `nbtree.h` 中的 BTOptions
3. 删除 `attoptcache.h` 中的 AttributeOpts
4. 简化 `reloptions.c`：删除非 fillfactor 选项定义、简化 default_reloptions()、删除 view/attribute 相关函数
5. 简化 `nbtutils.c` 中的 btoptions()
6. 简化 `reloptions.h` API 声明

### 第二阶段：修改消费者
7. autovacuum.c：删除 AutoVacOpts 依赖，改用 GUC 默认值
8. analyze.c：删除 n_distinct 覆盖
9. plancat.c：删除 parallel_workers 读取
10. rewriteHandler.c：删除 security_barrier/check_option 逻辑
11. heaptoast.c：删除 toast_tuple_target 使用
12. nbtinsert.c / nbtsort.c：删除 deduplicate_items 调用
13. parse_clause.c / tablecmds.c：删除 user_catalog_table 检查
14. extended_stats.c：删除 n_distinct 覆盖
15. relcache.c：删除 view/attribute 相关路径
16. attoptcache.c：删除 get_attribute_options 实现

### 第三阶段：更新回归测试
17. 更新受影响的回归测试期望文件
18. 从测试 SQL 中删除已移除的选项测试用例

### 第四阶段：验证
19. make check-world 全量回归测试

---

## 依赖与注意事项

- `pg_class.reloptions` 列保留（仍需存储 fillfactor）
- `Relation.rd_options` 保留（仍需缓存 StdRdOptions/BTOptions/HashOptions）
- `transformRelOptions()`、`extractRelOptions()`、`build_reloptions()` 等核心 API 保留
- HEAP_RELOPT_NAMESPACES 保留（仍需支持 toast namespace）
- btree/hash 的 fillfactor 功能完整保留

## 风险与处理

| 风险 | 处理方式 |
|------|----------|
| autovacuum 功能受影响 | autovacuum 仍可工作，仅失去逐表覆盖能力，使用 GUC 全局默认值 |
| 回归测试失败 | 更新期望文件，必要时删除受影响的测试用例 |
| 某处遗漏引用导致编译错误 | 通过编译错误定位，逐一修复 |
| 视图功能异常 | 删除 security_barrier/check_option 后，视图仍可工作，仅失去行级安全屏障功能 |
