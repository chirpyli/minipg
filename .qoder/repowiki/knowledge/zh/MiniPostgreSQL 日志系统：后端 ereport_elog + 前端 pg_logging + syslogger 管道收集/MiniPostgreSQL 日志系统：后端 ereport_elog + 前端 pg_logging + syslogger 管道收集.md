---
kind: logging_system
name: MiniPostgreSQL 日志系统：后端 ereport/elog + 前端 pg_logging + syslogger 管道收集
category: logging_system
scope:
    - '**'
source_files:
    - src/backend/utils/error/elog.c
    - src/include/utils/elog.h
    - src/backend/utils/errcodes.txt
    - src/common/logging.c
    - src/include/common/logging.h
    - src/backend/postmaster/syslogger.c
    - src/backend/postmaster/postmaster.c
---

## 1. 使用的系统与框架

MiniPostgreSQL（基于 PostgreSQL 14.23 裁剪）采用与上游一致的**分层日志体系**，分为后端服务器日志、前端工具日志和集中式日志收集三部分：

- **后端核心日志 API**：`src/backend/utils/error/elog.c` 提供 `ereport()` / `elog()` 宏及 `errstart()/errfinish()` 流程，支持结构化字段（`errcode()`, `errmsg()`, `errdetail()`, `errhint()`, `errcontext()`, `errposition()` 等），并通过 GUC 参数控制输出策略。
- **前端工具日志 API**：`src/common/logging.c` 提供面向 `initdb`/`psql`/`pg_ctl` 等前端程序的轻量级日志框架，使用 `pg_log_*` 宏族（`pg_log_fatal/error/warning/info/debug`）。
- **集中式日志收集器**：`src/backend/postmaster/syslogger.c` 作为独立子进程通过管道收集 postmaster 与所有后端进程的 stderr 输出，实现文件轮转、CSV 日志输出和可选的 syslog 集成。

## 2. 关键文件与包

| 组件 | 路径 | 职责 |
|---|---|---|
| 后端错误报告核心 | `src/backend/utils/error/elog.c` | `errstart/errfinish`、消息路由、级别过滤、backtrace、CSV 输出 |
| 后端错误报告接口 | `src/include/utils/elog.h` | `ereport/elog` 宏、错误级别常量、SQLSTATE 编码 |
| SQLSTATE 码表 | `src/backend/utils/errcodes.txt` → 生成 `src/backend/utils/errcodes.h` | 自动生成 SQLSTATE 代码 |
| 前端日志框架 | `src/common/logging.c` + `src/include/common/logging.h` | `pg_log_*` 宏、ANSI 颜色、位置回调 |
| 日志收集器 | `src/backend/postmaster/syslogger.c` | 管道读取、文件轮转、CSV 写入 |
| 主进程入口 | `src/backend/postmaster/postmaster.c` | 启动 syslogger、设置默认 `log_statement=all` |
| 配置项定义 | `src/backend/utils/misc/guc.c` | `log_min_messages`、`log_statement`、`log_connections` 等 GUC |

## 3. 架构与设计决策

### 3.1 后端日志流（`ereport` 链路）

```mermaid
graph LR
  A[调用方] --> B[ereport/elog 宏]
  B --> C[errstart(elevel, domain)]
  C --> D{should_output_to_server?}
  C --> E{should_output_to_client?}
  D --> F[send_message_to_server_log]
  E --> G[send_message_to_frontend]
  F --> H[syslogger 管道]
  H --> I[syslogger.c: 文件/CVS/syslog]
  G --> J[libpq 协议 -> 客户端]
```

- **错误级别体系**（`elog.h` 中定义）：`DEBUG5..DEBUG1 < LOG < INFO < NOTICE < WARNING < ERROR < FATAL < PANIC`。其中 `LOG_SERVER_ONLY` 仅进服务端日志，`WARNING_CLIENT_ONLY` 仅发给客户端。
- **级别过滤**：`is_log_level_output()` 同时考虑 `log_min_messages`（服务端）与 `client_min_messages`（客户端），并对 `LOG`/`LOG_SERVER_ONLY` 做特殊排序处理。
- **递归保护**：`errordata` 栈大小为 5，`recursion_depth > 2` 时触发 panic；内存不足时重置 `ErrorContext` 以释放自身。
- **钩子机制**：`emit_log_hook` 允许 preload 库拦截日志消息（在 `log_min_messages` 过滤之后）。
- **结构化字段**：通过 `errcode/errmsg/errdetail/errhint/errcontext/errposition` 等函数链式填充 `ErrorData`，最终由 `send_message_to_server_log` 统一格式化输出。

### 3.2 前端日志流（`pg_logging`）

- 初始化：`pg_logging_init(argv0)` 设置默认级别为 `PG_LOG_INFO`，检测 `PG_COLOR`/`PG_COLORS` 环境变量启用 ANSI 颜色。
- 级别控制：`pg_logging_set_level()`、`pg_logging_increase_verbosity()`（配合 `--verbose` 开关）。
- 可插拔上下文：`pg_logging_set_pre_callback()` 与 `pg_logging_set_locus_callback()` 允许注入文件名/行号信息。
- 输出目标：始终写 `stderr`，并先 `fflush(stdout)` 保证顺序。

### 3.3 集中式日志收集（syslogger）

- Postmaster 启动后 fork 出独立的 syslogger 进程，通过 `syslogPipe` 管道接收所有子进程 stderr。
- 支持按时间（`Log_RotationAge`）和大小（`Log_RotationSize`）轮转，输出到 `Log_directory/Log_filename`。
- 可同时输出 CSV 格式（`LOG_DESTINATION_CSVLOG`）供外部分析。
- 可选通过 `HAVE_SYSLOG` 编译支持 syslog 输出。

## 4. 约定与约束

### 4.1 后端日志约定

- **必须使用 `ereport` 而非裸 `fprintf`**：`elog.h` 注释明确说明新式 API 应使用 `ereport(ERROR, errmsg(...), errcode(...), ...)` 形式，`elog()` 仅为兼容旧代码的宏。
- **错误级别语义固定**：`ERROR` 中止事务返回已知状态；`FATAL` 终止进程；`PANIC` 拉垮其他后端；`WARNING` 表示意外情况，`NOTICE` 表示预期行为。
- **SQLSTATE 必须显式设置**：`errcode()` 对 `ERROR+` 默认 `ERRCODE_INTERNAL_ERROR`，对 `WARNING` 默认 `ERRCODE_WARNING`，对 `NOTICE` 以下默认 `ERRCODE_SUCCESSFUL_COMPLETION`。
- **临界区内错误升级**：`CritSectionCount > 0` 时所有 `ERROR+` 升级为 `PANIC`。
- **异常安全**：`errstart` 中若 `ErrorContext == NULL` 直接 `exit(2)`，避免未初始化状态下崩溃。

### 4.2 前端日志约定

- **`pg_log_*` 宏仅用于 stderr**：`logging.h` 注释强调“程序正常输出应走 stdout，不应使用日志系统”。
- **默认级别为 INFO**：`pg_logging_init` 将 `__pg_log_level` 设为 `PG_LOG_INFO`，低级别调试需显式降低。
- **颜色受控于环境变量**：`PG_COLOR=always/auto` 配合 `PG_COLORS` 自定义 ANSI SGR 序列。

### 4.3 运行时配置（GUC）

- `log_min_messages`：控制服务端日志最低级别。
- `client_min_messages`：控制客户端收到的最低级别。
- `log_statement`：记录语句级别（postmaster 默认设为 `all`）。
- `log_connections` / `log_disconnections`：连接事件日志。
- `log_duration`：记录查询耗时。
- `log_line_prefix`：自定义日志行前缀格式。
- `Logging_collector`：是否启用 syslogger 进程。
- `Log_destination`：位掩码组合 `LOG_DESTINATION_STDERR`/`LOG_DESTINATION_SYSLOG`/`LOG_DESTINATION_CSVLOG`。

### 4.4 性能约束

- `elog.c` 顶部注释明确指出：日志产生频率极高，获取任何可能被记录的额外信息时必须考虑性能开销；且可能在事务中止期间被调用，此时 syscache 查找不安全。
- `message_level_is_interesting()` 提供快速短路判断，避免昂贵的前置计算。
- `errstart_cold()` 利用 `pg_attribute_cold` 将错误路径从热路径中分离。

## 5. 适用性判定

本仓库完整实现了 PostgreSQL 风格的日志系统，涵盖后端结构化错误报告、前端轻量日志、集中式日志收集三大子系统，具备完整的级别体系、GUC 配置、钩子扩展和输出路由能力，因此该类别完全适用。