---
kind: dependency_management
name: 基于 Autoconf/Makefile 的 C 语言依赖管理（无包管理器）
category: dependency_management
scope:
    - '**'
source_files:
    - configure.ac
    - aclocal.m4
    - config/c-compiler.m4
    - config/c-library.m4
    - config/pkg.m4
    - config/ax_pthread.m4
    - config/python.m4
    - config/perl.m4
    - config/tcl.m4
    - config/llvm.m4
    - src/Makefile.global
    - src/Makefile.shlib
    - src/makefiles/pgxs.mk
    - contrib/contrib-global.mk
    - src/template/linux
    - src/port/README
---

## 1. 使用的系统/方法

MiniPostgreSQL 是一个纯 C/C++ 项目，**不使用任何现代语言级别的包管理器**（仓库中不存在 `go.mod`、`package.json`、`Cargo.toml`、`requirements.txt`、`Pipfile`、`composer.json`、`pyproject.toml` 等文件）。项目的第三方依赖通过以下机制管理：

- **Autoconf/Automake + M4 宏**：顶层 `configure.ac` 与 `config/` 目录下的 `.m4` 宏（如 `c-compiler.m4`、`c-library.m4`、`pkg.m4`、`ax_pthread.m4`、`llvm.m4`、`python.m4`、`tcl.m4`、`perl.m4`）负责检测系统提供的库（如 pthread、LLVM、Python、TCL、Perl），并生成 `src/include/pg_config.h` 等构建期配置头。
- **GNU Make 分层构建**：顶层 `Makefile` → `src/Makefile` → `src/makefiles/pgxs.mk` → 各子模块 `Makefile`，通过 `PGXS`（PostgreSQL Extension Build System）统一扩展和工具的编译规则。
- **系统级依赖**：所有外部库（C 标准库、平台库、可选的 TCL/Python/Perl 接口、OpenSSL/libpq 等）均以系统安装方式引入，由 `configure` 脚本在构建时探测可用版本与路径。
- **源码内嵌/静态链接**：部分功能（如 IANA 时区数据库 `src/timezone/`、Unicode 规范化表 `src/common/digit_table.h`、`d2s_full_table.h`）以源码形式直接纳入仓库，不依赖外部运行时包。

## 2. 关键文件

| 文件 | 作用 |
|---|---|
| `configure.ac` | Autoconf 主入口，声明需要检测的系统库与工具 |
| `aclocal.m4` | 自动生成的 m4 宏集合 |
| `config/*.m4` | 自定义检测宏（编译器、库、语言绑定） |
| `src/Makefile.global` / `src/Makefile.shlib` | 全局构建变量、共享库规则 |
| `src/makefiles/pgxs.mk` | PGXS 框架，供 contrib 扩展复用构建逻辑 |
| `contrib/contrib-global.mk` | contrib 模块的统一构建配置 |
| `src/template/linux` | Linux 平台特定的依赖/标志片段 |
| `src/port/` | 平台缺失函数的 C 实现（替代系统依赖） |
| `src/timezone/` | 内嵌 IANA tzcode/tzdata，避免外部时区数据依赖 |

## 3. 架构与约定

- **可选项开关**：通过 `--with-tcl`、`--with-perl`、`--with-python`、`--with-llvm`、`--with-libxml`、`--with-openssl` 等 configure 参数启用可选依赖；未启用的模块不会参与构建。
- **向后兼容层**：`src/port/` 提供跨平台函数 shim（如 `getopt_long.c`、`strlcpy.c`、`snprintf.c`、`pthread_barrier_wait.c`），减少对特定系统版本的依赖。
- **扩展隔离**：`contrib/` 下每个扩展独立 `Makefile`，通过 PGXS 链接到已构建的核心库，便于按需编译。
- **无锁文件**：仓库中没有 `vendor/`、`node_modules/`、`go.sum`、`poetry.lock` 等锁定文件；依赖版本由宿主系统的包管理器或源码树自身决定。

## 4. 约定与约束

- **描述性约定**：新增 C 依赖应通过 `configure.ac` 中的 `PKG_CHECK_MODULES` 或自定义 `.m4` 宏检测，并在对应 `Makefile` 中以条件编译（`#ifdef HAVE_XXX`）控制使用。
- **约束来源**：`configure` 脚本会拒绝无法解析的依赖；若某系统库不可用，相关功能将被静默禁用（通过 `pg_config.h` 中的 `HAVE_*` 宏控制），而非编译失败——这是 PostgreSQL 源码树的既定行为。
- **无强制版本锁定**：由于没有 lockfile，依赖版本完全取决于构建环境的系统库版本；升级外部库需手动更新 `configure.ac`/`.m4` 宏的检测逻辑。
- **私有仓库/代理**：未发现 GOPRIVATE、npm registry、PyPI mirror 等配置；本项目不涉及此类机制。

综上，该仓库的“依赖管理”本质上是 **传统 C 项目的 autoconf + make 构建系统**，通过源码检测与条件编译来适配不同宿主环境，而非现代意义上的包管理器。