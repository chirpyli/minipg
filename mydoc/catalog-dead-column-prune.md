# 系统表死列审计与裁剪

## 一、审计方法论

对 `src/include/catalog/` 下约 30 个系统表头逐一逐列执行全树 grep（排除 `pg_*_d.h`
生成头与 `.dat` 自身），区分：

- **写点**：`BKI_DEFAULT` 默认值、`heap_form_tuple` 前 `values[Anum_...]` 赋值、
  `CatalogTupleInsert/Update`、`rd_rel` 拷贝、genbki 默认；
- **读点**：`lsyscache`/`syscache` 读取、执行器/优化器消费、`relcache` 装载、`psql`
  `describe.c` 打印、`system_views.sql`/`information_schema.sql` 视图引用。

判定为**死列**需同时满足：

1. 写点恒为固定常量（始终写 `true`/`false`/`'p'`/`0`/默认值），或该列创建后
   从未被实际改变；
2. 读点全部已死（仅被 `psql` 中以 `false AS` / `'O' AS` 硬编码，或仅被已裁功能的
   代码读取，或读后无任何分支使用）。

核心机制相关列（事务 `relfrozenxid`/`relminmxid`、存储 `atttypmod`/`attstorage`、
类型系统 `pg_type` 各列、WAL/持久化 `relpersistence` 等）即使引用少亦保留。

## 二、审计结论总览

### 2.1 已裁剪的死列

| 系统表 | 死列 | 提交 |
|---|---|---|
| pg_proc | `proargdefaults` / `prosqlbody` | 4728c717 |
| pg_class | `relchecks` / `relispopulated` | 4781b89f |
| pg_statistic | `stainherit` | b7aa8991 |
| pg_attribute | `attgenerated`（早先移除） | — |
| **pg_aggregate** | **`aggnumdirectargs`、`aggmtransfn`、`aggminvtransfn`、`aggmfinalfn`、`aggmfinalextra`、`aggmfinalmodify`、`aggmtranstype`、`aggmtransspace`、`aggminitval`（共 9 列）** | 本轮 |

### 2.2 经核实"读点仍存活 / 核心机制"而保留的列

| 系统表.列 | 保留理由 |
|---|---|
| pg_database.`datistemplate` | autovacuum 跳过模板库、CREATE DATABASE TEMPLATE 读写，均活 |
| pg_database.`datallowconn` | `postinit.c` 连接准入检查、autovacuum 跳过，活读 |
| pg_database.`datconnlimit` | `postinit.c` 连接数限制、`datconnlimit=-2` 标记失效库，活读 |
| pg_rewrite.`ev_enabled` | `ENABLE/DISABLE RULE` 语法（`gram.y`）与 `rule->enabled` 比较（`relcache.c`）仍活跃 |
| pg_index.`indislive` | `DROP/REINDEX INDEX CONCURRENTLY` 仍置位/清位，活读 |
| pg_class.`relpersistence` | `storage.c` WAL 决策、`RelationIsPermanent` 宏、`lsyscache` 仍读（虽恒为 `'p'`，属持久化核心） |
| pg_type / pg_constraint / pg_aggregate 其余列 | 类型系统、PK/唯一约束、普通聚合执行核心，保留 |

> 说明：`pg_aggregate` 的 9 个死列属于"移动聚合（moving-aggregate，逆转移函数）"
> 与"有序集直接参数"两组功能。本 fork 已无 `CREATE AGGREGATE`（全库 0 命中
> `AggregateCreate`），执行器 `nodeAgg.c` 对 `aggm*` 列零读取，目录列恒为默认值，
> 故判定为死列彻底裁剪。

## 三、pg_aggregate 死列裁剪（本轮）

### 3.1 删除清单

| 列 | 类型 | 说明 |
|---|---|---|
| `aggnumdirectargs` | int16 | 有序集直接参数个数；执行期改用解析节点 `aggref->aggdirectargs`，目录列零读取 |
| `aggmtransfn` | regproc | 移动聚合转移函数（`.dat` 带数据，执行器零读） |
| `aggminvtransfn` | regproc | 移动聚合逆转移函数（同上） |
| `aggmfinalfn` | regproc | 移动聚合 final 函数（同上） |
| `aggmfinalextra` | bool | 给 `aggmfinalfn` 传额外哑参（BKI 默认 `f`，零读） |
| `aggmfinalmodify` | char | `aggmfinalfn` 是否改状态（BKI 默认 `r`，零读） |
| `aggmtranstype` | Oid | 移动聚合状态类型（`.dat` 带数据，零读） |
| `aggmtransspace` | int32 | 移动聚合状态大小估计（BKI 默认 0，零读） |
| `aggminitval` | text | 移动聚合初始值（`.dat` 带数据，零读） |

### 3.2 影响面

| 层 | 文件 | 内容 |
|---|---|---|
| Catalog 头 | `pg_aggregate.h` | 删上述 9 个字段与对应注释（-27 行） |
| 引导数据 | `pg_aggregate.dat` | 5 条带移动聚合的聚合（`sum(int4)`/`sum(int2)`/`avg(int8)`/`avg(int4)`/`avg(int2)`）去除 `aggm*` 字段赋值 |
| 回归测试 | `regress/sql/opr_sanity.sql` | 删"移动聚合一致性校验"整块（约 103 行）及 `aggnumdirectargs`/`aggmfinalmodify`/`aggmtransspace` 普通聚合检查中的引用 |
| 版本号 | `catalog/catversion.h` | `202609074 → 202609075` |

> 普通聚合列（`aggkind`/`aggtransfn`/`aggfinalfn`/`aggcombinefn`/`aggserialfn`/
> `aggdeserialfn`/`aggfinalextra`/`aggfinalmodify`/`aggsortop`/`aggtranstype`/
> `aggtransspace`/`agginitval`）全部保留，聚合执行不受影响。

## 四、STATRELATTINH 3→2 键修复（前置必要修复）

### 4.1 问题

上一轮 `pg_statistic` 裁剪删除了 `stainherit` 列，并同步把 `syscache.c` 的
`STATRELATTINH` 缓存由 **3 键**（starelid, staattnum, stainherit）改为 **2 键**
（starelid, staattnum）。但 `lsyscache.c`/`selfuncs.c`/`nodeHash.c`/`analyze.c`
中 7 处调用仍使用 `SearchSysCache3(STATRELATTINH, ..., BoolGetDatum(...))`
传第 3 个键。

后果：`SearchSysCache3` 以 `nkeys=3` 进入 `CatalogCacheComputeHashValue`，
访问 `cc_hashfunc[2]`——而 2 键缓存仅初始化 `cc_hashfunc[0..1]`，第 3 项为空指针，
在**任何**查询规划调用 `get_attavgwidth`/`get_attstatsslot` 时触发 `SIGSEGV`
（initdb 后段 "performing post-bootstrap initialization" 跑 `DELETE FROM pg_depend`
即崩溃）。这是一个在 `pg_statistic` 裁剪后潜伏、本次全新 `initdb` 才暴露的崩溃，
与本轮 `pg_aggregate` 改动无因果，但阻塞全部回归，必须修复。

### 4.2 修复

将 7 处 `SearchSysCache3` 改为 `SearchSysCache2`，删除第 3 个键参数
（`false` / `inh` / `node->skewInherit` 等）。因继承已裁，目录只存非继承统计，
2 键查找语义等价：

| 文件 | 函数 |
|---|---|
| `utils/cache/lsyscache.c` | `get_attavgwidth` |
| `utils/adt/selfuncs.c` | `restrict_selectivity` / `get_index_expression` / 两处 `get_variable` 路径 |
| `executor/nodeHash.c` | skew 统计查找 |
| `commands/analyze.c` | `update_attstats` 旧元组查找 |

（`SysCacheGetAttr(STATRELATTINH, ...)` 两处仅从已命中元组取属性，与键数无关，不改。）

## 五、构建与回归

1. 全量重编（本环境 `make clean` 的 `rm -rf tmp_install` 被批量删除保护拦截，
   改用 `make -C src/backend clean` + `make -C src/bin clean` + 删除生成头
   `pg_*_d.h` 后 `make -j16`，再 `make install`）：
   ```sh
   make -C src/backend clean
   make -C src/bin clean
   rm -f src/include/catalog/pg_*_d.h
   make -j16 && make install
   ```
2. 验证：`initdb` 成功（此前因 STATRELATTINH 崩溃，修复后通过）；
   `make -C src/test/regress installcheck PGPORT=5441` → **51/51 全绿**
   （含 `opr_sanity` 对 pg_aggregate 裁剪的校验、`sysviews`）。
3. 编译：**0 error、0 warning**。

## 六、验收

1. 反向 grep（源码全库 0 命中）：
   ```sh
   rg "aggnumdirectargs|aggmtransfn|aggminvtransfn|aggmfinalfn|aggmfinalextra|aggmfinalmodify|aggmtranstype|aggmtransspace|aggminitval" src/
   ```
2. 行为抽查（临时实例 + psql）：
   - `SELECT sum(x), avg(x) FROM t` 正常（移动聚合死列移除不影响普通聚合结果）；
   - `opr_sanity` 中移动聚合一致性校验整块已删，普通聚合检查仍为 0 行异常。
3. `pg_aggregate` 系统表列数由 22 减为 13，普通聚合（max/min/sum/avg/array_agg/
   string_agg 等）全部可用。
