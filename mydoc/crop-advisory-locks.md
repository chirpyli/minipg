# 裁剪「咨询锁（Advisory Locks）」功能

## 一、背景与目标

minipg 以 PostgreSQL 14.23 为基线，目标是裁剪非核心功能、保留核心内核机制，供数据库内核学习者学习使用。

咨询锁（advisory locks）是构建在通用重量级锁管理器之上的一层**应用级协作锁**：它复用 `LOCKTAG`/`LockAcquire` 机制，但使用独立的锁方法 `USER_LOCKMETHOD`，通过 `pg_advisory_lock()` 等一组 SQL 函数由应用自行加锁解锁，与数据库自身的对象锁完全正交。

本次目标：彻底移除咨询锁功能，包括 SQL 函数、专用锁方法与相关释放钩子，并同步清理目录元数据、GUC、注释与回归测试。

判定依据：咨询锁属于**可选的应用层功能**，不参与任何数据库核心机制（不做并发控制、不保护系统数据结构），学习价值低于通用锁管理器、死锁检测、SSI 与事务本身；其实现代码量小、接入点集中，裁剪后不影响任何核心路径。

## 二、可裁剪性判定

| 判断维度 | 结论 |
| --- | --- |
| 是否核心机制 | 否。仅是一层供应用使用的协作锁，与并发控制核心逻辑无关 |
| 是否被核心路径调用 | 否。仅由 `pg_advisory_*` SQL 函数与两个会话/事务退出钩子调用 |
| 与不可裁剪项的关系 | 不影响 btree/hash 索引，不影响事务机制；复用但可脱离通用锁管理器 |
| 依赖关系 | 删 `USER_LOCKMETHOD` 后，`LockMethods[]` 只剩默认锁方法；`LockReleaseSession` 失去唯一调用方而成为死代码 |
| 学习价值 | 低。其机制是通用锁管理器的一个特例应用，通用锁管理器本身已保留 |

**裁剪后保留**：
- 通用锁管理器（`DEFAULT_LOCKMETHOD`）、死锁检测、锁等待与唤醒；
- SSI（`predicate.c`）与 SIReadLock 机制；
- `pg_lock_status`（`pg_locks` 语义），被 `sysviews.sql` 与 `tidscan.sql`（SIReadLock 断言）依赖；
- `LOCKTAG_USERLOCK`（注释标明 "reserved for old contrib/userlock code"，与咨询锁无关联）；
- `LockReleaseAll`（事务提交/中止仍以 `DEFAULT_LOCKMETHOD` 调用）；
- `LockMethods[0] = NULL` 的下标占位设计，`DEFAULT_LOCKMETHOD` 编号保持 1 不变。

## 三、涉及文件清单

| 文件 | 操作 | 说明 |
| --- | --- | --- |
| `src/backend/utils/adt/lockfuncs.c` | MODIFY | 删除全部 `pg_advisory_*` 函数、`SET_LOCKTAG_INT64/INT32` 宏、`LockTagTypeNames[]` 的 `"advisory"` 项及其静态断言、`pg_lock_status` 中 `case LOCKTAG_ADVISORY`；保留 `pg_lock_status` |
| `src/include/storage/lock.h` | MODIFY | 删除 `USER_LOCKMETHOD`、`Trace_userlocks` 声明、`LockTagType` 中 `LOCKTAG_ADVISORY`（并更新 `LOCKTAG_LAST_TYPE`）、`SET_LOCKTAG_ADVISORY` 宏、`LockReleaseSession` 声明 |
| `src/backend/storage/lmgr/lock.c` | MODIFY | 删除 `user_lockmethod` 定义、`LockMethods[]` 中 `&user_lockmethod` 表项、`Trace_userlocks` 变量、`LockReleaseSession()` 实现、`TRACE_USERLOCKS` 注释 |
| `src/backend/storage/lmgr/lmgr.c` | MODIFY | 删除 `DescribeLockTag()` 中 `case LOCKTAG_ADVISORY` 分支 |
| `src/backend/storage/lmgr/proc.c` | MODIFY | 删除 `ProcReleaseLocks()` 中 `LockReleaseAll(USER_LOCKMETHOD, false)` 及注释 |
| `src/backend/utils/init/postinit.c` | MODIFY | 删除 `ShutdownPostgres()` 中 `LockReleaseAll(USER_LOCKMETHOD, true)` 及注释 |
| `src/backend/utils/misc/guc.c` | MODIFY | 删除 `trace_userlocks` GUC 定义 |
| `src/backend/utils/misc/check_guc` | MODIFY | 从 `INTENTIONALLY_NOT_INCLUDED` 列表删除 `trace_userlocks` |
| `src/backend/storage/lmgr/README` | MODIFY | 删除 "User Locks (Advisory Locks)" 章节 |
| `src/include/catalog/pg_proc.dat` | MODIFY | 删除 21 条 advisory 函数目录条目（oid 2880~2892、3089~3096） |
| `src/include/catalog/catversion.h` | MODIFY | 提升 `CATALOG_VERSION_NO`（202609285 → 202609291），强制 initdb |
| `src/test/regress/sql/advisory_lock.sql` | DELETE | 咨询锁专用测试 |
| `src/test/regress/expected/advisory_lock.out` | DELETE | 咨询锁期望输出 |
| `src/test/regress/parallel_schedule` | MODIFY | 第 131 行移除 `advisory_lock` 调度条目 |

## 四、具体步骤

1. **SQL 函数层**：在 `lockfuncs.c` 中删除从 `Functions for manipulating advisory locks` 注释到文件末尾的全部内容（含 18 个 `pg_advisory_*` 函数实现）；删除 `LockTagTypeNames[]` 的 `"advisory"` 项，静态断言改用 `LOCKTAG_USERLOCK + 1`；删除 `pg_lock_status` 的 `case LOCKTAG_ADVISORY`。
2. **锁方法定义**：在 `lock.h` 删除 `USER_LOCKMETHOD`、`SET_LOCKTAG_ADVISORY`、`LOCKTAG_ADVISORY` 枚举项与 `Trace_userlocks`、`LockReleaseSession` 声明；在 `lock.c` 删除 `user_lockmethod`、`LockMethods[]` 表项、`Trace_userlocks` 变量与 `LockReleaseSession()` 实现。
3. **调用钩子**：删除 `proc.c`（`ProcReleaseLocks`）与 `postinit.c`（`ShutdownPostgres`）中的 `USER_LOCKMETHOD` 释放调用。
4. **锁标签描述**：删除 `lmgr.c` 的 `DescribeLockTag()` 中 advisory 分支。
5. **GUC**：删除 `guc.c` 的 `trace_userlocks` 定义与 `check_guc` 列表项。
6. **目录元数据**：删除 `pg_proc.dat` 的 21 条 advisory 条目，提升 `catversion.h`。
7. **测试**：删除 `advisory_lock.sql` / `advisory_lock.out`，更新 `parallel_schedule`。
8. **注释文档**：清理 `lock.c`、`proc.c` 注释与 `lmgr/README` 中的 advisory 章节。
9. **全量重编译与验证**（见下节）。

## 五、验证方式

1. **全量重编译**：因改动了 `lock.h` 且项目未启用头文件依赖跟踪，必须 `make clean && make` 全量重建，确认零编译警告。
2. **目录一致性**：确认 `pg_proc.dat` 变化经由 genbki.pl 反映到生成的 `postgres.bki` / `*_d.h`。
3. **回归测试**：执行 `make check-world`，要求全部通过；重点确认 `sysviews`（`pg_lock_status`）、`tidscan`（SIReadLock 断言）不受影响。
4. **残留扫描**：全局搜索 `USER_LOCKMETHOD`、`pg_advisory`、`advisory`、`Trace_userlocks`，确认除有意保留项外无残留。

## 六、风险与注意事项

- `LockTagType` 枚举删除末项后，须同步核对 `LOCKTAG_LAST_TYPE`、`LockTagTypeNames` 静态断言与 `GetLockNameFromTagType()` 的边界判断，避免数组越界或断言失败。
- `LockReleaseSession` 在删除 `pg_advisory_unlock_all` 后成为死代码，必须一并删除实现与声明，否则留下无效代码。
- `proc.c` / `postinit.c` 的钩子删除后，`LockReleaseAll` 仍需保留（xact.c 仍以 `DEFAULT_LOCKMETHOD` 调用）。
- 删除测试条目后需确认同一调度行中 `bitmapops`、`combocid` 正常执行。
- `isolation_schedule` 中的 `deadlock-parallel` 已在此前裁剪移除，isolation 测试无 advisory 残留，无需改动。
