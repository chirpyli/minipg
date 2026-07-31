# contrib 扩展裁剪分析

本文档分析 `contrib/` 目录下的扩展，判断哪些对"数据库内核学习"有价值、应保留，哪些可以删除及其理由。

## 分类原则

针对**内核学习**场景，一个扩展是否值得保留，主要看它能否：

1. **观察内核内部状态**：直接读取 buffer / 页面 / 索引 / 事务等内部结构，帮助学习者理解存储、缓存、MVCC 等机制。
2. **演示内核机制**：本身是某个内核子系统（AM、FDW、SPI、过程语言桥接）的典型示例实现，可作为教学样例。
3. **基础数据类型/算子**，被其他保留功能依赖。

反之，以下类型的扩展应删除：

- **业务/应用层功能**：特定领域计算（GIS 距离、ISBN、立方体、全文检索词典、加密）。
- **外部系统集成**：dblink / postgres_fdw 连接别的数据库、外部文件 FDW。
- **安全/运维/部署脚手架**：sepgsql、passwordcheck、auth_delay、start-scripts、sslinfo。
- **过程语言桥接**：plperl / plpython 相关的 `xxx_pl*` 扩展（与"存储过程"裁剪一致）。
- **测试/调试专用**：test_decoding（逻辑复制测试插件，依赖 replication）。

## 推荐保留的扩展（内核学习价值高）

| 扩展 | 行数 | 保留理由 |
|---|---|---|
| **pageinspect** | 4995 | 直接读取 heap / btree / gin 等页面的原始字节，观察元组、行指针、索引结构，是学习**存储层与索引**最直观的工具 |
| **pg_buffercache** | 292 | 查看共享缓冲区中缓存了哪些 relation / fork / block，理解**缓冲区管理** |
| **pg_freespacemap** | 93 | 查看堆的空闲空间映射（FSM），理解**空间回收**机制 |
| **pg_visibility** | 1105 | 查看可见性映射（VM）与页可见性，配合理解 **MVCC / vacuum** |
| **pgstattuple** | 2185 | 统计死元组、空闲空间、膨胀率，理解 **heap 结构与 vacuum 效果** |
| **pg_stat_statements** | 3658 | 统计 SQL 执行代价与次数，理解**查询执行与代价模型**（核心优化器/执行器教学辅助） |
| **pg_prewarm** | 1194 | 预加载关系到缓冲区，配合 pg_buffercache 观察加载过程 |
| **pg_surgery** | 539 | 直接修改页面/元组修复损坏数据，演示页面内部结构（进阶，可选） |
| **amcheck** | 5546 | 校验 btree / heap 的逻辑一致性，理解**索引结构与损坏检测** |
| **pg_visibility / pgrowlocks（行锁查看）** | 345 | 查看行级锁，理解**锁机制** |

> 上述"观察类"扩展合计约 **22k 行**（含少量 SQL/头文件），是内核学习的核心辅助，建议全部保留。其中 pgrowlocks（345）、pg_surgery（539）可视教学深度取舍，但体积都很小，保留成本极低。

### 作为内核机制"示例实现"保留（可选，教学样例价值）

| 扩展 | 行数 | 保留理由 |
|---|---|---|
| **bloom** | 1863 | 通用索引 AM 的**完整示例实现**，代码量小，是学习"如何写一个自定义访问方法"的最佳样板 |
| **spi** | 935 | 服务器端过程语言接口（SPI）的示例，演示 C 函数如何通过 SPI 调用 SQL，与执行器交互 |
| **btree_gin / btree_gist** | 1982 / 9521 | 演示如何为 GIN/GiST 编写操作符类；但 btree_gist 体积偏大（9.5k）可酌情删 |
| **intarray** | 4454 | GIN 索引算子示例 |
| **seg / cube** | 5523 / 6740 | GiST 索引算子示例 |

> 这类"示例型"扩展价值在于**展示内核扩展机制本身**，而非业务功能。建议至少保留 `bloom`（最小最完整）与 `spi`（与执行器交互最直接）作为样例；其余 GiST/GIN 示例可删以节省约 22k 行。

## 建议删除的扩展及理由

### 1. 过程语言桥接（与"存储过程"裁剪一致）

| 扩展 | 行数 | 理由 |
|---|---|---|
| bool_plperl / hstore_plperl / jsonb_plperl | 221 / 444 / 600 | plperl 桥接，依赖 plperl |
| hstore_plpython / jsonb_plpython / ltree_plpython | 482 / 866 / 154 | plpython 桥接，依赖 plpython |

合计 ~2.8k 行。随 PL 裁剪一并删除。

### 2. 外部系统集成 / FDW

| 扩展 | 行数 | 理由 |
|---|---|---|
| dblink | 4041 | 连接其他 PostgreSQL 实例，应用层工具 |
| postgres_fdw | 17620 | 外部表对接远程 PG，实现复杂，与应用学习无关 |
| file_fdw | 1488 | 读取服务器本地文件为外部表 |
| xml2 | 1364 | XML 处理，依赖 libxml2，应用层 |

合计 ~24.5k 行。

### 3. 业务/领域计算与数据类型

| 扩展 | 行数 | 理由 |
|---|---|---|
| pgcrypto | 20480 | 加解密，业务安全功能 |
| cube / earthdistance / seg | 6740 / 581 / 5523 | 多维立方体、地球距离、线段类型，特定领域 |
| isn | 6472 | ISBN/ISSN 等编号类型 |
| hstore | 5673 | 键值类型（非内核必需，且 pg14 已有 jsonb） |
| ltree | 6028 | 树形标签路径类型 |
| citext | 2060 | 大小写不敏感文本类型 |
| intarray | 4454 | 整数数组算子（归入示例型时可保留，否则删） |
| fuzzystrmatch | 2318 | 模糊字符串匹配 |
| tablefunc | 1933 | 交叉表等报表函数 |
| uuid-ossp | 697 | UUID 生成（pg14 已有内置 gen_random_uuid） |

合计 ~60k 行（intarray 若保留则 ~55k）。

### 4. 全文检索相关（与 snowball/tsearch 裁剪一致）

| 扩展 | 行数 | 理由 |
|---|---|---|
| dict_int / dict_xsyn / unaccent | 216 / 334 / 508 | 全文检索词典/去重音，依赖 tsearch |

合计 ~1k 行。随全文检索裁剪删除。

### 5. 安全 / 运维 / 部署脚手架

| 扩展 | 行数 | 理由 |
|---|---|---|
| sepgsql | 6074 | SELinux 强制访问控制，强依赖安全模块 |
| passwordcheck | 184 | 密码强度检查钩子 |
| auth_delay | 73 | 认证失败延迟 |
| sslinfo | 553 | SSL 连接信息 |
| adminpack | 804 | 服务器文件管理（pg_ls_dir 等），运维工具 |
| old_snapshot | 179 | 旧快照阈值调试，边缘特性 |
| start-scripts | 0 | 系统启动脚本，非代码 |
| oid2name / vacuumlo | 651 / 555 | 运维小工具 |

合计 ~3k 行。

### 6. 测试 / 复制调试

| 扩展 | 行数 | 理由 |
|---|---|---|
| test_decoding | 2949 | 逻辑复制输出插件，**依赖 replication 子系统** |
| tcn | 195 | 表变更通知，依赖监听机制 |

合计 ~3.1k 行。

> **注意（执行修正）**：`test_decoding` 虽归类为"测试/复制调试"，但它被 `src/test/recovery`、`src/test/subscription`、`src/bin/pg_basebackup` 的回归测试通过 `EXTRA_INSTALL` 强依赖。由于 minipg 当前**仍保留 replication 子系统**，删除 test_decoding 会导致 `make check-world` 的 `temp-install` 阶段失败。因此 **test_decoding 推迟到"阶段 8 裁 replication"时再删除**（届时一并移除上述逻辑复制测试模块）。方案 A 执行时实际保留 13 个扩展（12 个 + test_decoding）。

### 7. 采样方法 / 其他

| 扩展 | 行数 | 理由 |
|---|---|---|
| tsm_system_rows / tsm_system_time | 394 / 419 | 表采样方法，边缘特性 |
| lo | 297 | 大对象维护触发器 |
| pg_trgm | 6036 | 三元组相似度（常用于模糊搜索/索引），业务向但体积小可酌情 |

合计 ~7.3k 行。

## 推荐方案（两种粒度，供选择）

### 方案 A：保守（仅保留"观察类"核心 + bloom + spi 样例）

- **保留**：pageinspect, pg_buffercache, pg_freespacemap, pg_visibility, pgstattuple, pg_stat_statements, pg_prewarm, pg_surgery, amcheck, pgrowlocks, bloom, spi, **test_decoding（推迟到阶段 8 删除）**
- **删除**：其余全部
- **实际执行**：已删除 43 个扩展（test_decoding 暂留），contrib 保留约 25.7k 行
- **特点**：最大化保留"观察内核 + 看一个 AM 示例"的能力，删掉全部业务/外部/安全类；test_decoding 因 replication 测试依赖暂留

### 方案 B：激进（只保留纯"观察类"）

- **保留**：pageinspect, pg_buffercache, pg_freespacemap, pg_visibility, pgstattuple, pg_stat_statements, pg_prewarm, pg_surgery, amcheck（含 pgrowlocks 可选）
- **删除**：bloom, spi 及所有其他
- **收益**：contrib 降到约 **26k 行**
- **特点**：完全聚焦"观察内部状态"，不保留示例型 AM；若学习者想看自定义 AM 实现，可单独保留 bloom

## 与整体裁剪方案的关系

- 第 1 阶段（整体裁剪 contrib）可调整为：**先按本表删除业务/外部/安全/PL桥接/全文检索类**，保留内核观察类。
- `pl*` 桥接扩展与"存储过程"裁剪阶段联动删除。
- `dict_*` / `unaccent` 与 snowball + tsearch 全文检索裁剪阶段联动删除。
- `test_decoding` 与 replication 裁剪阶段联动删除。

## 结论

contrib 中**真正对内核学习有价值的是"观察内部结构"的一批小扩展**（pageinspect、pg_buffercache、pg_freespacemap、pg_visibility、pgstattuple、pg_stat_statements、amcheck 等），合计仅约 20k 行；加上一个最小最完整的自定义 AM 示例（bloom）和 SPI 示例（spi）即可覆盖"看内部 + 看扩展机制"两类需求。其余约 80k 行（业务计算、外部集成、安全运维、PL 桥接、全文检索）均可安全删除。
