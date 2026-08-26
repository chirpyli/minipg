---
kind: build_system
name: 基于 Autoconf + GNU Make 的 PostgreSQL 构建系统（minipg 裁剪版）
category: build_system
scope:
    - '**'
source_files:
    - Makefile
    - build.sh
    - configure.ac
    - GNUmakefile.in
    - src/Makefile.global
    - src/Makefile
    - src/makefiles/pgxs.mk
    - contrib/Makefile
    - src/template/linux
---

## 1. 构建系统与工具链

本项目采用标准的 PostgreSQL 构建体系，核心由 **Autoconf/Automake** 与 **GNU Make** 组成：
- 顶层 `configure.ac` 通过 autoconf 生成 `configure` 脚本，检测编译器、平台特性、可选库（zlib、readline 等），并输出 `config.status`、`Makefile.global`、`pg_config.h` 等构建期配置。
- 根目录 `Makefile` 仅作为“安全门”：强制要求先运行 `./configure`，并在 PATH 中查找 `gmake/gnumake/make` 中的 GNU make 版本，否则报错退出。
- `GNUmakefile.in` 经 `config.status` 展开为 `GNUmakefile`，是真正的顶层入口，定义 `all`/`world`/`install`/`check`/`dist`/`distcheck` 等目标，并通过 `$(call recurse,...)` 递归调用 `src/`、`contrib/`、`doc/`、`config/`。
- `build.sh` 是项目自定义的一键脚本：执行 `make maintainer-clean` 后调用 `./configure --prefix=/home/postgres/minipg --enable-debug`，用于本地开发调试。

## 2. 关键文件与层次

| 文件 | 作用 |
|---|---|
| `configure.ac` | Autoconf 主描述；声明版本 `AC_INIT([PostgreSQL], [14.23], ...)`，限定模板为 `linux`（minipg 仅支持 Linux），提供 `--enable-*`/`--with-*` 选项 |
| `GNUmakefile.in` | 顶层 Makefile 模板，定义 `world`/`check-world`/`install-world`/`dist`/`distcheck` 等目标及子模块递归顺序 |
| `src/Makefile.global` | 全局构建变量集中地：版本号、安装路径（默认 `prefix=/home/postgres/minipg`）、编译标志、特性开关（`enable_debug=yes`、`enable_tap_tests=no` 等） |
| `src/Makefile` | 定义 `SUBDIRS = common/port/timezone/backend/include/interfaces/fe_utils/bin/makefiles/test/*`，并显式 `.NOTPARALLEL:` 禁止并行构建 |
| `src/makefiles/pgxs.mk` | PGXS 扩展构建框架，供 `contrib/` 下各扩展复用 |
| `contrib/Makefile` | 列举 `amcheck/bloom/pageinspect/pg_buffercache/pg_freespacemap/pg_surgery/pgrowlocks/pgstattuple/pg_visibility/spi` 十个扩展子目录 |
| `src/template/linux` | 平台模板，被 `configure` 选择（见 `configure.ac` 第 63-75 行注释：“minipg 仅支持 Linux，直接使用 linux 模板”） |

## 3. 架构与约定

### 3.1 递归构建模型
顶层 `GNUmakefile.in` 使用 `$(call recurse,targets,subdirs,sub-targets)` 宏统一调度。典型顺序：`config → src → contrib → doc`，且 `world-contrib-recurse` 依赖 `world-src-recurse`，保证后端先于扩展构建。

### 3.2 模块划分
- `src/common`、`src/port`、`src/timezone`：基础静态库（`libpgcommon.a`、`libpgport.a`），分别产出 `_shlib.o` 与 `_srv.o` 两套对象以支持共享库与服务端链接。
- `src/backend`：PostgreSQL 服务端核心，按 `access/`、`catalog/`、`executor/`、`optimizer/`、`parser/`、`storage/`、`utils/` 等子系统组织。
- `src/bin`：客户端工具集（`initdb`、`psql`、`pg_ctl`、`pg_rewind`、`pg_waldump`、`pg_controldata`、`pg_config`），每个子目录自带独立 `Makefile`。
- `src/interfaces/libpq`：C 客户端库 `libpq`。
- `contrib/<ext>`：每个扩展遵循 PGXS 约定，含 `<name>.control`、SQL 升级脚本、`expected/` 回归期望、`sql/` 测试用例、`t/` TAP 测试。

### 3.3 构建产物与安装
- 安装前缀默认 `prefix=/home/postgres/minipg`（来自 `src/Makefile.global`），导出 `bindir`、`libdir`、`pkglibdir`、`includedir`、`datadir`、`mandir`、`docdir`、`localedir` 等标准目录。
- `world` 目标构建全部组件；`world-bin` 仅构建二进制；`install-world` 同时安装源码树内所有可安装产物。
- `dist`/`distcheck`：`dist` 用 `find` 打包源码树（跳过 `.git`、`CVS`），`distcheck` 在临时目录解压并重跑 `configure && make && make install && make uninstall` 验证可卸载性。

### 3.4 测试集成
- `check`/`check-tests`/`installcheck`/`installcheck-parallel`/`check-world`：顶层目标通过 `CHECKPREP_TOP=src/test/regress` 委派给 `src/test/regress` 的 pg_regress 框架。
- `contrib` 下每个扩展自带 `REGRESS=` 列表，由 PGXS 自动纳入回归套件。
- `test/isolation`、`test/perl`、`test/modules`、`test/recovery` 等子目录提供隔离并发测试、Perl TAP 测试、自定义模块测试与 WAL 恢复测试。

### 3.5 平台与交叉编译约束
- `configure.ac` 第 63-75 行明确限制：仅接受 `linux*|gnu*|k*bsd*-gnu`，其他平台直接 `AC_MSG_ERROR` 报错，因此本仓库**不实现跨平台支持**。
- 未检出任何 Dockerfile、CI 配置文件（`.github/workflows`、`.travis.yml`、`.gitlab-ci.yml` 等均不存在），发布流程依赖手工 `make dist`。

## 4. 约定与约束

- **必须使用 GNU make**：根 `Makefile` 会搜索 `gmake/gnumake/make` 并校验其 GNU 版本，非 GNU make 直接失败。
- **必须先运行 configure**：未生成 `GNUmakefile` 时所有目标均报错提示先执行 `./configure`。
- **禁用并行构建**：`src/Makefile` 显式声明 `.NOTPARALLEL:`，因为子模块间依赖复杂。
- **版本来源单一**：`AC_INIT` 中的 `14.23` 是唯一版本源，`Makefile.global` 的 `VERSION`/`MAJORVERSION`/`VERSION_NUM` 均由 configure 注入。
- **调试构建**：`build.sh` 默认启用 `--enable-debug`，`Makefile.global` 中 `enable_debug=yes`，便于本地调试。
- **扩展规范**：`contrib` 下每个扩展必须包含 `<name>.control` 文件，并由 PGXS 管理安装路径与依赖。
- **清理策略**：`maintainer-clean` 会递归清理 `doc/`、`contrib/`、`config/`、`src/` 并删除 `autom4te.cache`、`config.cache`、`config.log`、`config.status`、`GNUmakefile`。

## 5. 适用性说明

该仓库完整保留了 PostgreSQL 的 Autoconf+Make 构建体系，虽为“裁剪版”，但构建入口、递归规则、PGXS 扩展机制、测试集成、分发打包等核心模式均与原 PostgreSQL 一致，属于高置信度的构建系统知识。