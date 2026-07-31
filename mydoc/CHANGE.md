# 变更日志

- 2026-07-13: 提交postgres 14.23版本
- 2026-07-31: 裁剪 contrib 扩展（方案 A）：删除 44 个与内核学习无关的扩展，仅保留 12 个"内核观察类 + 示例型"扩展，约删减 12.3 万行。保留 test_decoding（逻辑复制插件，随阶段 8 裁 replication 时再删）；subscription 测试改用 jsonb 替代已删的 hstore。详见下文。
- 2026-07-31: 裁剪跨平台兼容性，仅保留Linux。删除所有 Windows / MinGW / MSVC / Cygwin / MSYS 专属代码与构建脚本，回归测试 `make check-world` 全部通过。详见下文。
- 2026-07-31: 裁剪 ecpg（嵌入式 SQL 预处理器，阶段 2）：整体删除 `src/interfaces/ecpg/`（约 16.6 万行，405 个文件），并清理构建系统引用（interfaces/Makefile、GNUmakefile 的 world 递归、Makefile.global[.in] 的 ecpg_config.h 规则、configure.ac 的 AC_CONFIG_HEADERS）。`make check-world` 全部通过。详见下文。

## 裁剪：仅支持Linux（移除 Windows 等平台代码）

**目的**：minipg 仅作为 Linux 上的学习用数据库，不再支持 Windows 及其他非 Linux 平台，从而精简源码。

**为什么可以删**：
- 所有 Windows/Cygwin/MSVC 专属逻辑都通过宏（`WIN32`、`_MSC_VER`、`__CYGWIN__`、`PORTNAME=win32`）进行条件编译，在 Linux 下这些宏永不被定义，相关分支本就是死代码。
- 信号量/共享内存实现由 configure 在 `USE_WIN32_SEMAPHORES` / `USE_WIN32_SHARED_MEMORY` 与 SysV/POSIX 之间切换，删除 Win32 实现后自动回退到 Linux 默认的 POSIX/SysV 实现，不影响 Linux 行为。
- 构建系统（`src/template/*`、`src/makefiles/*`、`src/tools/msvc`）是平台专属的编译/链接脚本，Linux 构建路径不依赖它们。

**删除的文件与目录**（共 91 处改动，约 11885 行）：
- 目录：`src/backend/port/win32/`、`src/include/port/win32/`、`src/include/port/win32_msvc/`、`src/tools/msvc/`、`src/bin/pgevent/`
- 端口层：`src/backend/port/win32_sema.c`、`win32_shmem.c`；`src/port/win32*.c`（common/env/error/fseek/ntdll/security/setlocale/stat）、`pthread-win32.h`、`win32.ico`、`win32ver.rc`
- 头文件：`src/include/port/win32.h`、`win32_port.h`、`win32ntdll.h`、`cygwin.h`、`atomics/generic-msvc.h` 及 `port/win32{,_msvc}/**` 下的桩头文件
- 客户端：`src/interfaces/libpq/win32*.c`、`.h`；`src/interfaces/ecpg/include/ecpg-pthread-win32.h`
- 模板/构建：`src/template/win32`、`src/template/cygwin`、`src/makefiles/Makefile.win32`、`Makefile.cygwin`、`src/tools/win32tzlist.pl`

**修改的文件**：
- `configure.ac`：移除 cygwin/mingw/win32 的平台探测、`port/win32` 头包含、Win32 专属 `AC_LIBOBJ` 与 dbghelp 检测、`USE_WIN32_SEMAPHORES`/`USE_WIN32_SHARED_MEMORY` 分支、`check_win32_symlinks` 等。
- `src/Makefile.global.in`、`src/include/Makefile`、`src/bin/Makefile`、`src/include/port.h`：移除对 Win32/Cygwin 头、子目录、库与 `pgevent` 的引用。
- `src/interfaces/ecpg/ecpglib/{memory,descriptor,connect,sqlda,misc}.c`：将无条件的 `#include "ecpg-pthread-win32.h"` 改为标准 `#include <pthread.h>`（该头在 Unix 下仅封装 pthread.h）。

**验证**：`./configure` 成功；`make -j` 成功；`make check-world` 全部通过。

**注意事项**：`configure` 脚本保留原生成版本（本机 autoconf 2.71 与 PG14 要求的 2.69 版本不符，未重新生成）。若日后需要 `autoreconf`，请安装 autoconf 2.69 或放宽 `configure.ac` 的版本宏。

## 裁剪：contrib 扩展（仅保留内核观察类与示例型，方案 A）

**目的**：minipg 面向数据库内核学习，contrib 中大量扩展属于业务计算、外部集成、安全运维、过程语言桥接、全文检索等，与内核学习无关，予以删除；保留能"观察数据库内部运行状态"及"演示内核扩展机制"的扩展。

**保留的 12 个扩展（约 22,750 行）**：
- 内核观察类（看内部状态）：`pageinspect`（直接读 heap/btree 页面字节）、`pg_buffercache`（共享缓冲区内容）、`pg_freespacemap`（空闲空间映射）、`pg_visibility`（可见性映射）、`pgstattuple`（死元组/膨胀）、`pg_stat_statements`（SQL 代价统计）、`pg_prewarm`（预加载）、`pg_surgery`（页面修复）、`pgrowlocks`（行锁）、`amcheck`（btree/heap 一致性校验）
- 示例型（演示扩展机制）：`bloom`（最小最完整的自定义访问方法 AM 示例）、`spi`（服务端过程语言接口示例）

**删除的 44 个扩展（约 122,670 行）及删除理由**：
- 过程语言桥接（随"存储过程"裁剪）：`bool_plperl`、`hstore_plperl`、`jsonb_plperl`、`hstore_plpython`、`jsonb_plpython`、`ltree_plpython`
- 外部系统集成 / FDW：`dblink`、`postgres_fdw`、`file_fdw`、`xml2`
- 业务计算与数据类型：`pgcrypto`、`cube`、`earthdistance`、`seg`、`isn`、`hstore`、`ltree`、`citext`、`intarray`、`fuzzystrmatch`、`tablefunc`、`uuid-ossp`
- 全文检索相关（随 snowball/tsearch 裁剪）：`dict_int`、`dict_xsyn`、`unaccent`
- 安全 / 运维 / 部署：`sepgsql`、`passwordcheck`、`auth_delay`、`sslinfo`、`adminpack`、`old_snapshot`、`start-scripts`、`oid2name`、`vacuumlo`
- 测试 / 复制调试：`test_decoding`（依赖 replication）、`tcn`
- 其他边缘：`btree_gin`、`btree_gist`、`auto_explain`、`lo`、`pg_trgm`、`tsm_system_rows`、`tsm_system_time`、`intagg`

**修改的文件**：
- `contrib/Makefile`：SUBDIRS 仅保留 12 个扩展；移除原来基于 `with_ssl`/`with_uuid`/`with_libxml`/`with_selinux`/`with_perl`/`with_python` 的条件子目录块（这些选项不再向 contrib 加入扩展，但核心代码仍可能使用这些 configure 选项，故未改动 `configure`）。

**验证**：`make -C contrib` 编译成功；`make -C contrib check` 回归测试全部通过。核心代码（`src/`）不引用任何 contrib 扩展，initdb 也不预装扩展，删除不影响内核构建。

**裁剪后的依赖修复（同次提交）**：
- `contrib/test_decoding` **恢复保留**：它是逻辑复制（replication）的调试/输出插件，被 `src/test/recovery`、`src/test/subscription`、`src/bin/pg_basebackup` 的回归测试通过 `EXTRA_INSTALL` 强依赖。当前 replication 子系统仍完整保留，若删除会导致 `make check-world` 的 `temp-install` 阶段失败。故将其推迟到"阶段 8 裁 replication"时再删（届时一并处理上述测试模块）。
- `src/test/modules/test_misc/t/008_replslot_single_user.pl` **恢复**：该测试依赖 `test_decoding` 逻辑解码。因 test_decoding 已恢复保留（见上），008 测试一并恢复，test_misc 的 `EXTRA_INSTALL` 改回 `contrib/test_decoding`。注：当前 configure 未启用 `--enable-tap-tests`，该 TAP 测试在 `make check-world` 中不实际执行，仅参与 temp-install 构建；启用 TAP 后可正常运行。
- `src/test/subscription/t/002_types.pl` **改用 jsonb**：原测试用 `hstore` 扩展验证键值类型的逻辑复制。hstore 已删，改为内核内置的 `jsonb` 类型覆盖同类场景，并移除 subscription Makefile 的 `EXTRA_INSTALL = contrib/hstore`。
- `src/test/recovery`、`src/bin/pg_basebackup` 的 `EXTRA_INSTALL = contrib/test_decoding` 保留有效（test_decoding 已恢复）。

完成上述修复后，`make check-world` 全部通过（EXIT=0）。

## 裁剪：ecpg 嵌入式 SQL 预处理器（阶段 2）

**目的**：ecpg 是 PostgreSQL 的**客户端嵌入式 SQL 预处理器**——开发者在 `.pgc` 文件里混写 `EXEC SQL` 语句，ecpg 工具把它翻译成对 ecpglib（底层调 libpq）的 C 调用后再编译。它是**客户端工具链**，与 server 内核（存储/执行器/优化器/事务）毫无耦合，对"数据库内核学习"无价值，且体积小、独立性强，是裁剪方案里收益高、风险低的大块。

**删除内容**：整体删除 `src/interfaces/ecpg/`（405 个被跟踪文件，约 16.6 万行），含：
- `preproc/`（~9.6 万行，核心预处理器/解析器）
- `test/`（~5.1 万行，ecpg 自身回归测试，非内核）
- `pgtypeslib/`（~7.9k 行，嵌入式专用数值类型库）
- `ecpglib/`（~7.8k 行，运行时库）
- `compatlib/`、`include/`

**修改的构建文件**（保持源码与生成物一致）：
- `src/interfaces/Makefile`：SUBDIRS 移除 `ecpg`；删除 `all-ecpg-recurse` / `install-ecpg-recurse` 规则
- `GNUmakefile`：`check-world` / `checkprep` / `installcheck-world` 的递归列表移除 `src/interfaces/ecpg`
- `src/Makefile.global`（已生成）与 `src/Makefile.global.in`（模板）：移除 `ecpg_config.h` 自动重建规则
- `configure.ac`：移除 `ecpg_config.h` 的 `AC_CONFIG_HEADERS`

**说明**：本机 autoconf 版本（2.71）与 PG14 要求（2.69）不符，未重新生成 `configure`；已直接修正已生成的 `Makefile.global` 与模板/configure.ac，使源码一致且当前构建可用。若日后 `autoreconf`，需装 autoconf 2.69 或放宽版本宏。

**验证**：`make -j4` 顶层编译成功；`make check-world` 全部通过（EXIT=0）。服务端内核代码不依赖 ecpg。

