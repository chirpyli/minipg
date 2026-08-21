# Relation Options 完整裁剪实施方案

## 目标
彻底裁剪 minipg 中整个 reloptions 系统，包括所有选项（fillfactor、autovacuum、view 选项等）。
删除 `reloptions.c` 源文件和 `reloptions.h` 头文件，以及所有消费者代码。

## 详细影响分析

### 需删除/修改的结构体和宏：

1. **Relation 结构体**：删除 `rd_options` 字段
2. **StdRdOptions**：完全删除（rel.h）
3. **AutoVacOpts**：完全删除（rel.h）
4. **ViewOptions**：完全删除（rel.h）
5. **AttributeOpts**：完全删除（attoptcache.h）
6. **BTOptions**：完全删除（nbtree.h）
7. **HashOptions**：完全删除（hash.h）
8. **所有 reloptions 宏**：RelationGetFillFactor、RelationGetTargetPageFreeSpace、
   RelationGetTargetPageUsage、RelationGetToastTupleTarget、
   RelationIsUsedAsCatalogTable、RelationGetParallelWorkers、
   RelationIsSecurityView、RelationHasCheckOption、
   BTGetFillFactor、BTGetTargetPageFreeSpace、BTGetDeduplicateItems、
   HashGetFillFactor、HashGetTargetPageUsage

### 需删除的文件：
- `src/backend/access/common/reloptions.c`
- `src/backend/utils/cache/attoptcache.c`
- `src/include/access/reloptions.h`
- `src/include/utils/attoptcache.h`

### 需修改的消费者文件：

#### 核心代码：
| 文件 | 修改 |
|------|------|
| `src/include/utils/rel.h` | 删除 rd_options 字段、AutoVacOpts、StdRdOptions、ViewOptions、所有 rd_options 相关宏 |
| `src/include/access/nbtree.h` | 删除 BTOptions、BTGetFillFactor、BTGetTargetPageFreeSpace、BTGetDeduplicateItems |
| `src/include/access/hash.h` | 删除 HashOptions、HashGetFillFactor、HashGetTargetPageUsage |
| `src/backend/access/heap/hio.c` | 硬编码 saveFreeSpace = 0（默认 fillfactor=100 无预留空间） |
| `src/backend/access/heap/heapam.c` (L2351) | 硬编码 saveFreeSpace = 0 |
| `src/backend/access/heap/rewriteheap.c` (L662) | 硬编码 saveFreeSpace = 0 |
| `src/backend/access/heap/pruneheap.c` (L187) | 硬编码 minfree = 0 |
| `src/backend/access/heap/heaptoast.c` (L174) | 硬编码 TOAST_TUPLE_TARGET |
| `src/backend/access/nbtree/nbtinsert.c` (L2768) | 删除 BTGetDeduplicateItems 条件 |
| `src/backend/access/nbtree/nbtsort.c` (L712,L1193) | 删除 BTGetTargetPageFreeSpace 和 BTGetDeduplicateItems |
| `src/backend/access/nbtree/nbtsplitloc.c` (L173) | 硬编码 BTREE_DEFAULT_FILLFACTOR |
| `src/backend/access/nbtree/nbtutils.c` | 删除 btoptions() 函数 |
| `src/backend/access/hash/hashpage.c` (L362) | 硬编码默认 fillfactor 计算 |
| `src/backend/access/hash/hashutil.c` | 删除 hashoptions() 函数 |
| `src/backend/access/index/indexam.c` | 删除 amoptions 相关代码 |
| `src/backend/postmaster/autovacuum.c` | 删除 AutoVacOpts 相关代码，全部使用 GUC 默认值 |
| `src/backend/optimizer/util/plancat.c` (L154) | 硬编码 rel_parallel_workers = -1 |
| `src/backend/commands/analyze.c` | 删除 get_attribute_options / n_distinct 覆盖 |
| `src/backend/commands/vacuum.c` (L1877,L1901) | 删除 rd_options 检查，使用默认值 |
| `src/backend/commands/tablecmds.c` | 删除 reloptions 解析/校验/存储 |
| `src/backend/commands/indexcmds.c` | 删除 reloptions 校验 |
| `src/backend/tcop/utility.c` | 删除 reloptions 处理 |
| `src/backend/rewrite/rewriteHandler.c` | 删除 security_barrier / check_option |
| `src/backend/parser/parse_clause.c` (L2720) | 删除 RelationIsUsedAsCatalogTable |
| `src/backend/parser/parse_utilcmd.c` | 删除 untransformRelOptions 调用 |
| `src/backend/statistics/extended_stats.c` | 删除 n_distinct 覆盖 |
| `src/backend/utils/cache/relcache.c` | 删除 RelationParseRelOptions 相关代码 |
| `src/backend/utils/adt/ruleutils.c` | 删除 untransformRelOptions 调用 |
| `src/backend/catalog/index.c` | 删除 reloptions 相关代码 |
| `src/include/utils/attoptcache.h` | 删除 AttributeOpts 和 get_attribute_options 声明 |
| `src/include/access/reloptions.h` | 整个文件删除 |
| `src/backend/utils/cache/Makefile` | 删除 attoptcache.o |

#### 测试代码：
| 文件 | 修改 |
|------|------|
| `src/test/modules/dummy_index_am/dummy_index_am.c` | 删除 build_reloptions 相关代码 |

### pg_class.reloptions 列
- 裁剪 pg_class 表的 reloptions 列

## 实施步骤

### 第1步：删除 reloptions 核心文件
- 删除 reloptions.h、reloptions.c、attoptcache.h、attoptcache.c

### 第2步：修改 rel.h
- 删除 rd_options 字段、AutoVacOpts、StdRdOptions、ViewOptions、所有相关宏

### 第3步：修改 nbtree.h / nbtutils.c
- 删除 BTOptions 结构和所有 BT* 宏
- 删除 btoptions() 函数

### 第4步：修改 hash.h / hashutil.c
- 删除 HashOptions 结构和所有 Hash* 宏
- 删除 hashoptions() 函数

### 第5步：修改 heap 相关消费者
- hio.c, heapam.c, rewriteheap.c, pruneheap.c, heaptoast.c 硬编码默认值

### 第6步：修改 btree/hash 消费者
- btree: nbtinsert.c, nbtsort.c, nbtsplitloc.c
- hash: hashpage.c

### 第7步：修改 autovacuum.c
- 删除 AutoVacOpts 相关代码

### 第8步：修改其他消费者
- plancat.c, analyze.c, vacuum.c, tablecmds.c, indexcmds.c,
- utility.c, rewriteHandler.c, parse_clause.c, parse_utilcmd.c,
- extended_stats.c, relcache.c, ruleutils.c, index.c, indexam.c

### 第9步：修改 Makefile 和测试代码
- 删除 Makefile 中 attoptcache.o
- 修改 dummy_index_am.c

### 第10步：编译 + 回归测试

## 验证
- make check-world

## 风险
- 风险：改动面大，遗漏引用导致编译错误
- 处理：通过编译错误逐一修复，确保每个引用都被正确替换
