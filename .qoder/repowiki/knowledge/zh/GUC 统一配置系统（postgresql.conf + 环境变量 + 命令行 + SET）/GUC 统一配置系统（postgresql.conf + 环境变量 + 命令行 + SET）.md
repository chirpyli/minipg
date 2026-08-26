---
kind: configuration_system
name: GUC 统一配置系统（postgresql.conf + 环境变量 + 命令行 + SET）
category: configuration_system
scope:
    - '**'
source_files:
    - src/backend/utils/misc/guc.c
    - src/backend/utils/misc/guc-file.c
    - src/include/utils/guc.h
    - src/include/utils/guc_tables.h
    - src/backend/postmaster/postmaster.c
    - src/bin/initdb/initdb.c
    - src/backend/bootstrap/bootstrap.c
    - src/backend/tcop/postgres.c
---

## 1. 系统/框架概述

MiniPostgreSQL 沿用了 PostgreSQL 的 **GUC（Grand Unified Configuration）** 子系统，作为统一的运行时配置体系。它把以下四种来源的配置值按优先级合并为最终生效的值：
- 硬编码默认值（boot_val / dynamic default）
- 环境变量（`PG_*` 系列，如 `PGDATA`、`PGPORT` 等）
- 配置文件 `postgresql.conf`（位于数据目录 `$PGDATA` 下）
- 命令行参数（postmaster 的 `-c name=value`、`-B`、`-h`、`-p` 等）
- 会话级 `SET` / `SET LOCAL`（事务内局部覆盖）
- 数据库/用户/数据库+用户级别的 per-* GUC 设置（通过 `ALTER SYSTEM` / `pg_settings` 写入系统表）

该系统的核心实现集中在 `src/backend/utils/misc/guc.c` 与 `guc-file.c`（由 flex 生成的词法分析器），公共类型定义在 `src/include/utils/guc.h` 和 `src/include/utils/guc_tables.h`。

## 2. 关键文件与包

| 路径 | 作用 |
|---|---|
| `src/backend/utils/misc/guc.c` | GUC 注册、解析、验证、赋值、序列化、SIGHUP 重载、`ProcessConfigFile` 调度 |
| `src/backend/utils/misc/guc-file.c` | 基于 flex 的词法分析器，负责解析 `postgresql.conf` 语法 |
| `src/include/utils/guc.h` | `GucSource`、`GucContext`、`SetConfigOption`、`DefineCustom*Variable` 等 API |
| `src/include/utils/guc_tables.h` | `config_generic`、`config_bool/int/real/string/enum` 结构体及分组枚举 |
| `src/backend/postmaster/postmaster.c` | postmaster 启动时调用 `InitializeGUCOptions()`，并解析命令行参数到 GUC |
| `src/bin/initdb/initdb.c` | 初始化数据目录时从 `postgresql.conf.sample` 生成初始 `postgresql.conf` |
| `src/backend/bootstrap/bootstrap.c`、`src/backend/tcop/postgres.c` | 引导阶段与后端进程入口也调用 `InitializeGUCOptions()` |

## 3. 架构与设计约定

### 3.1 变量注册模型
每个模块通过 `DefineCustomBoolVariable` / `DefineCustomIntVariable` / `DefineCustomRealVariable` / `DefineCustomStringVariable` / `DefineCustomEnumVariable` 将自身配置项注册到全局 GUC 表中。这些函数内部会构造 `config_generic` 及其派生结构，并调用 `add_guc_variable` 插入到集中管理的链表/哈希中。变量的元数据包括：名称、上下文（`PGC_POSTMASTER`、`PGC_USERSET`、`PGC_SIGHUP` 等）、所属分组（`FILE_LOCATIONS`、`WAL_SETTINGS`、`QUERY_TUNING_COST` 等）、短/长描述、check/assign/show hook。

### 3.2 配置来源优先级（`GucSource`）
`src/include/utils/guc.h` 定义了 `PGC_S_DEFAULT` → `PGC_S_DYNAMIC_DEFAULT` → `PGC_S_ENV_VAR` → `PGC_S_FILE` → `PGC_S_ARGV` → `PGC_S_GLOBAL` → `PGC_S_DATABASE` → `PGC_S_USER` → `PGC_S_DATABASE_USER` → `PGC_S_CLIENT` → `PGC_S_SESSION` 的严格优先级链。更高优先级的来源会覆盖低优先级来源；`RESET` 操作恢复的是“低于当前 source 的最高有效 source”对应的值。

### 3.3 生命周期
1. postmaster 启动时调用 `InitializeGUCOptions()`，先注册所有内置 GUC，再调用 `InitializeGUCOptionsFromEnvironment()` 读取环境变量。
2. 随后解析命令行参数，通过 `SetConfigOption(..., PGC_S_ARGV)` 写入。
3. 接着调用 `ProcessConfigFile(PGC_POSTMASTER)` 解析 `$PGDATA/postgresql.conf`（常量 `CONFIG_FILENAME` 定义为 `"postgresql.conf"`，见 `guc.c:102`）。
4. 后续 SIGHUP 信号处理路径（`autovacuum.c`、`checkpointer.c`、`syslogger.c`、`walwriter.c`、`tcop/postgres.c` 等）均通过 `ProcessConfigFile(PGC_SIGHUP)` 动态重载配置。
5. 会话级 `SET` / `SET LOCAL` 通过 `guc.c` 中的 `set_config_option` 入栈 `GucStack` 实现事务级隔离。

### 3.4 配置文件格式
`guc-file.c` 是 flex 生成的词法分析器，专门用于解析 `postgresql.conf` 风格的键值对（支持注释、引号字符串、布尔/整数/实数/枚举等字面量）。`ProcessConfigFileInternal` 负责打开文件、逐行扫描、调用对应类型的 check/assign hook 并记录 `sourcefile`/`sourceline` 以便错误定位。

### 3.5 初始化模板
`initdb` 工具通过 `setup_data_file_paths()` 将 `postgresql.conf.sample` 作为输入模板，复制到 `$PGDATA/postgresql.conf` 供首次运行使用（见 `initdb.c:1795` 及后续拷贝逻辑）。

## 4. 约定与约束

- **新增配置项必须通过 `DefineCustom*Variable` 注册**，并在 `guc_tables.h` 的 `config_group` 中归入合适的分组，否则不会出现在 `SHOW ALL` / `pg_settings` 中。
- **文件名固定**：配置文件名硬编码为 `postgresql.conf`（`guc.c:102`），不可通过编译选项更改。
- **认证配置裁剪**：代码注释多处标注 `minipg: pg_hba.conf / pg_ident.conf 已移除`（如 `auth.c:63`、`postmaster.c:2130`），因此 MiniPostgreSQL 不再加载主机访问控制或身份映射配置文件，所有入站连接被无条件信任放行。
- **SIGHUP 重载**：所有后台进程（autovacuum、checkpointer、syslogger、walwriter、统计收集器等）都通过 `ProcessConfigFile(PGC_SIGHUP)` 响应热重载；若某选项标记为仅重启生效（`PGC_POSTMASTER` 且不允许热更新），则需重启进程。
- **检查钩子（check_hook）**：每个 GUC 可通过 check hook 限制取值范围（例如 `check_maxconnections`、`check_wal_consistency_checking`、`check_temp_buffers` 等），违反约束时报错并拒绝设置。
- **源追踪**：每次设置都会记录 `source`、`scontext`、`sourcefile`、`sourceline`，便于调试配置来源。
- **环境变量前缀**：通过 `InitializeGUCOptionsFromEnvironment()` 识别 `PG_*` 环境变量并映射到同名 GUC，但仅限允许通过环境设置的变量（受 `flags` 位控制）。
- **构建期配置**：除运行时 GUC 外，项目还保留 autoconf/m4 构建期配置（`configure.ac`、`config/` 下的 `.m4` 宏、`Makefile.global.in`、`src/include/pg_config.h.in`），用于检测编译器/平台能力并生成 `pg_config.h`——这是编译期配置，与运行时 GUC 正交。

## 5. 适用性说明

本仓库完整实现了 PostgreSQL 的 GUC 配置系统，涵盖配置注册、多来源合并、配置文件解析、热重载、会话级覆盖等全部核心机制；同时因裁剪移除了 `pg_hba.conf`/`pg_ident.conf` 等认证相关配置。因此该类别完全适用，且证据充分（多个后端模块调用 `ProcessConfigFile`、大量 `DefineCustom*Variable` 注册点、flex 生成的 conf 解析器、initdb 模板生成流程）。