# 变更日志

- 2026-07-13: 提交postgres 14.23版本
- 2026-08-02: 裁剪 PO 翻译文件：删除全部非英文/中文的 `.po` 文件（cs/de/el/es/fr/he/it/ja/ko/pl/pt_BR/ru/sv/tr/uk/vi 共 140 个），仅保留各模块的 `zh_CN.po`（12 个）。同步将 12 个 `nls.mk` 的 `AVAIL_LANGUAGES` 收敛为 `zh_CN`。当前构建 `ENABLE_NLS` 默认关闭，翻译不参与 `make check-world` 编译，回归测试不受影响。详见下文。
- 2026-08-02: 彻底移除 Native Language Support（ENABLE_NLS）翻译子系统（保留 gettext 空宏直通层，源码业务调用零改动）。删除 `--enable-nls` 配置项、12 个 `nls.mk`、`src/nls-global.mk`、`src/Makefile` 的 nls-global.mk 安装指令、configure.ac 的 NLS 段与 PGAC_CHECK_GETTEXT 调用、pg_config.h.in 的 `#undef ENABLE_NLS`；拍平 c.h（删 libintl.h 包含、gettext 空宏移出条件保护）、elog.c（err_gettext 直通）、pg_locale.c（SetMessageEncoding→GetDatabaseEncoding）、miscinit.c（pg_bindtextdomain 空函数）、mbutils.c（删 NLS 编码绑定函数）、pg_wchar.h（删 pg_enc2gettext 类型/声明）、encnames.c（删 pg_enc2gettext_tbl）、fe-misc.c（删 libpq 翻译实现）、libpq-int.h（libpq_gettext 空宏化）、print.c（删两处 NLS 翻译块）、exec.c（删 bindtextdomain 块）。autoconf 2.69 重生成 configure/autoheader。make check-world 全量通过。详见下文。
- 2026-08-02: 清空 NLS 翻译数据：删除保留的 12 个 `zh_CN.po`（backend/libpq/plpgsql/pg_dump/pg_waldump/pg_ctl/pg_config/pg_basebackup/pg_controldata/psql/pg_rewind/initdb 各模块 `po/zh_CN.po`），NLS 子系统数据与机制一并归零；空 `po/` 目录由 git 自动忽略。修复重生成 configure 时误删 `with_tcl` 外层 if 闭合 fi 导致的 configure 截断语法错误。make check-world 全量通过。详见下文。
- 2026-07-31: 裁剪 contrib 扩展（方案 A）：删除 44 个与内核学习无关的扩展，仅保留 12 个"内核观察类 + 示例型"扩展，约删减 12.3 万行。保留 test_decoding（逻辑复制插件，随阶段 8 裁 replication 时再删）；subscription 测试改用 jsonb 替代已删的 hstore。详见下文。
- 2026-07-31: 裁剪跨平台兼容性，仅保留Linux。删除所有 Windows / MinGW / MSVC / Cygwin / MSYS 专属代码与构建脚本，回归测试 `make check-world` 全部通过。详见下文。
- 2026-07-31: 修复 Windows 裁剪遗留的 `make clean` 失败：`src/backend/port/Makefile` 残留对 `win32` 子目录的引用（`SUBDIRS += win32` 与 `clean` 规则中的 `$(MAKE) -C win32 clean`），因 win32 目录已删导致 `make clean` 报 "No such file or directory"。已移除该引用，`make clean` / `make check-world` 均通过。
- 2026-07-31: 裁剪非 plpgsql 过程语言（阶段 3）：删除 `src/pl/plperl/`、`src/pl/plpython/`、`src/pl/tcl/`（pltcl）三个外部解释器桥接语言，仅保留 `src/pl/plpgsql/`（PG 原生、最能体现 fmgr call-handler + SPI 扩展机制的教学样例）。修改 `src/pl/Makefile` 移除对应条件子目录。约删 4 万行。`make check-world` 全部通过。详见下文。
- 2026-07-31: 裁剪 ecpg（嵌入式 SQL 预处理器，阶段 2）：整体删除 `src/interfaces/ecpg/`（约 16.6 万行，405 个文件），并清理构建系统引用（interfaces/Makefile、GNUmakefile 的 world 递归、Makefile.global[.in] 的 ecpg_config.h 规则、configure.ac 的 AC_CONFIG_HEADERS）。`make check-world` 全部通过。详见下文。
- 2026-07-31: 收尾清理源码级非 Linux 条件编译：修复上一轮 Windows 裁剪遗留的 `src/fe_utils/cancel.c` 未闭合 `#ifndef WIN32` 编译错误，并手工删除源码（`.c`/`.h`）中残留的 `WIN32`/`__CYGWIN__`/`_MSC_VER` 条件编译分支（含 `fe_utils/{cancel,print,string_utils,parallel_slot}.c`、`common/{exec,d2s}.c`、`backend/libpq/{pqsignal,pqcomm}.c`）。`make` 与 `make check-world` 均通过。注意：初次全仓库扫描存在工具假阴性误判，实际 `src/bin`、`src/test`、`fe_utils/psqlscan.c`、`configure.ac` 等仍含大量平台宏，需后续继续清理。详见下文。
- 2026-07-31: 继续清理 `src/backend/main/main.c` 中遗漏的 7 处平台代码（见上）。
- 2026-07-31: 手工清理 src/bin 下产品工具的非 Linux 平台条件编译（生成代码如 *scan.c/*gram.c 暂不处理）：
  - fe_utils: cancel.c、print.c、string_utils.c、parallel_slot.c、psqlscan.c（__ia64__ 块）
  - common: exec.c、d2s.c
  - backend/libpq: pqsignal.c、pqcomm.c
  - backend/main: main.c
  - psql(8 文件)、pg_dump(7 文件，含并行 fork 实现深度清理)、pgbench、pg_resetwal、initdb(2)、pg_basebackup(3)、pg_receivewal、pg_recvlogical
  - pg_rewind(2 文件)、pg_upgrade(9 文件：pg_upgrade.h/server.c/controldata.c/util.c/file.c/check.c/exec.c/option.c/pg_upgrade.c/parallel.c，删除 Windows 线程实现与 CopyFile/xcopy 分支，统一为 fork/posix 路径)、pg_ctl(单文件 pg_ctl.c，删除整个 Windows 服务管理实现块 ~600 行，含 pgwin32_* 函数、CreateRestrictedProcess、do_register/do_unregister/do_runservice、eventlog、全局 WIN32 变量，并将 -N/-P/-U/-S/-e 选项在 Linux 下改为"not supported on this platform"报错)
  - 全部通过 make 编译验证，make -j16 全量编译通过。
  - initdb/findtimezone.c：删除 `#else /* WIN32 */` 整段 Windows 实现块（约 1000 行，含 win32_tzmap[] 映射表、注册表读取逻辑），并去掉 `#ifndef WIN32` 开头使 Linux 实现无条件。`make -j16` 与 `make check-world` 均通过。详见下文。
  - EXEC_BACKEND 清理（方案 A，仅删孤立低风险分支，不动 postmaster.c/guc.c/sysv_shmem.c 等核心启动架构）：
    - common/exec.c：删除 `pg_disable_aslr()` 整函数及其 EXEC_BACKEND 专属 include 头块（CRLF 文件，用 sed 删 28-34 行）。
    - include/port.h：删除 `pg_disable_aslr()` 声明。
    - bin/pg_ctl/pg_ctl.c、test/regress/pg_regress.c：删除 fork 前的 `pg_disable_aslr()` 调用分支。
    - backend/main/main.c：删除 `main()` 中 `--fork` 分发到 `SubPostmasterMain` 的分支。
    - 说明：保留 `NON_EXEC_STATIC` 宏（其余文件仍在用）；本阶段不动 guc.c/sysv_shmem.c 等。
    - `make -j16` 全量编译 + backend/common 强制重编均通过，无未定义符号。详见下文。
  - EXEC_BACKEND 清理（方案 B，逐文件彻底删除 exec 模型双实现）：
    - backend/postmaster/postmaster.c + include/postmaster/postmaster.h：删除全部 exec 后端机制——`postmaster_forkexec` / `backend_forkexec` / `internal_forkexec` / `SubPostmasterMain`、`save_backend_variables` / `restore_backend_variables` / `read_backend_variables` / `read_inheritable_socket`、`ShmemBackendArray*` 系列（Add/Remove/Alloc）及其声明与全部调用点；`BackendStartup` / `StartChildProcess` / `StartAutovacuumWorker` / `bgworker_forkexec` / `do_start_bgworker` 的 fork/exec 双分支统一为 `fork_process()` 路径；`write_nondefault_variables` / `find_other_exec` 相关的 EXEC 专属初始化块一并移除。
    - backend/postmaster/autovacuum.c + include/postmaster/autovacuum.h：删除 `avlauncher_forkexec()` / `avworker_forkexec()`、`AutovacuumLauncherIAm()` / `AutovacuumWorkerIAm()` 及头文件中的 `#ifdef EXEC_BACKEND` 声明块；`StartAutoVacLauncher` / `StartAutoVacWorker` 只保留 `fork_process()` 分支；`AutoVacLauncherMain` / `AutoVacWorkerMain` 的 `NON_EXEC_STATIC` 展开为 `static`，其中 `InitProcess()` 去掉 `#ifndef EXEC_BACKEND` 包裹。
    - backend/postmaster/syslogger.c + include/postmaster/syslogger.h（同时清理该文件残留的 Windows 死代码，共删约 320 行）：
      - EXEC_BACKEND 部分：删除 `syslogger_forkexec()` / `syslogger_parseArgs()` 整块及头文件中的 `SysLoggerMain` 声明；`SysLogger_Start()` 的 fork/exec 双分支合并为纯 `fork_process()`；`SysLoggerMain` 与 `first_syslogger_file_time` 的 `NON_EXEC_STATIC` 展开为 `static`。
      - WIN32 部分：删除 Windows 数据传输线程实现 `pipeThread()`（约 76 行）及 `threadHandle` / `sysloggerSection` 临界区变量与其 `InitializeCriticalSection` / `Enter` / `LeaveCriticalSection` 调用；`syslogPipe` 去掉 `HANDLE` 分支统一为 `int[2]`；管道创建去掉 `CreatePipe`+`SECURITY_ATTRIBUTES` 分支；stderr 重定向去掉 `_open_osfhandle` / `_setmode(_O_BINARY)` 分支；主循环去掉 Windows 专用的 `LeaveCriticalSection` + 等待路径，统一走 `WaitEventSetWait` + `WL_SOCKET_READABLE`；`logfile_open()` 与 `update_metainfo_datafile()` 中两处 `_setmode(_O_TEXT)`（CRLF 行尾）一并删除。
    - backend/postmaster/bgworker.c + include/postmaster/bgworker_internals.h：删除仅供 exec 后端从共享内存回读自身定义的 `BackgroundWorkerEntry()` 及其声明；`StartBackgroundWorker()` 中 `InitProcess()` 去掉 `#ifndef EXEC_BACKEND` 包裹；修正 `LookupBackgroundWorkerFunction()` 注释中的 EXEC_BACKEND 描述。
    - backend/storage/ipc/ipci.c：删除共享内存尺寸估算中的 `ShmemBackendArraySize()` 累加与 `ShmemBackendArrayAllocation()` 调用块（配套 postmaster.c 已删的 ShmemBackendArray 机制）；`CreateSharedMemoryAndSemaphores()` 中"重新 attach 已存在段"分支简化为无条件 `elog(PANIC)`；重写函数头注释。
    - backend/storage/ipc/dsm.c + include/storage/dsm.h：`dsm_backend_startup()` 删除 exec 后端专用的控制段 attach + sanity 校验逻辑（子进程经 fork 继承映射），简化为只置 `dsm_init_done`；删除 `dsm_set_control_handle()` 及其声明。
    - backend/port/sysv_shmem.c + include/storage/pg_shmem.h：删除 `PGSharedMemoryReAttach()` / `PGSharedMemoryNoReAttach()` 两个函数整块（约 82 行，含 `__CYGWIN__` 分支）及其头文件声明；`InternalIpcMemoryCreate()` 删除 `PG_SHMEM_ADDR` 环境变量指定 attach 地址的 exec 专用变通逻辑（含 macOS ASLR 默认值）；`PGSharedMemoryDetach()` 删除 cygipc 的 `shmdt(NULL)` 变通；`DEFAULT_SHARED_MEMORY_TYPE` 从条件宏固定为 `SHMEM_TYPE_MMAP`；重写文件头注释。
    - `make -j16` 全量编译通过。
    - 续（方案 B 收尾，彻底删除剩余 EXEC_BACKEND 双实现 + NON_EXEC_STATIC 宏）：
      - include/c.h：删除 `NON_EXEC_STATIC` 宏定义块（`#ifdef EXEC_BACKEND ... #else ... static`）；全仓库剩余的 `NON_EXEC_STATIC` 用法（proc.c 的 `ProcStructLock`/`AuxiliaryProcs`、pmsignal.c 的 `PMSignalState`、pgstat.c 的 `pgStatSock`/`PgstatCollectorMain`）直接展开为 `static`。
      - backend/utils/misc/guc.c + include/utils/guc.h：删除 EXEC 专用 `CONFIG_EXEC_PARAMS` 宏、`write_nondefault_variables()` / `read_nondefault_variables()` 整块（含其 `#ifdef EXEC_BACKEND` 包裹，约 220 行）及 guc.h 声明；`shared_memory_options[]` 中 mmap 项去掉 `#ifndef EXEC_BACKEND` 始终可用；简化两处注释中的 EXEC_BACKEND 描述。
      - backend/postmaster/pgstat.c + include/pgstat.h：删除 `pgstat_forkexec()`（含前向声明）、`#ifdef EXEC_BACKEND` 包裹；`SysLogger_Start` 同款 fork 双分支已删（上轮）；`PgstatCollectorMain` 的 `NON_EXEC_STATIC`→`static` 并去掉 pgstat.h 中的 `#ifdef EXEC_BACKEND` 声明块；`PgstatCollectorStart` 的 fork 双分支统一为 `fork_process()`（恢复 `case 0:` 子进程分支）。
      - backend/utils/init/globals.c + include/miscadmin.h：删除 EXEC 专用全局变量 `postgres_exec_path[]`（globals.c 定义 + miscadmin.h `extern` 声明，已无引用）。
      - backend/tcop/postgres.c、backend/bootstrap/bootstrap.c、backend/utils/init/miscinit.c：去掉 `InitProcess()` / `InitAuxiliaryProcess()` 的 `#ifndef EXEC_BACKEND` 包裹（始终调用），miscinit.c 中 `pqinitmask()` 的 `#ifdef EXEC_BACKEND` 包裹去除（始终调用）。
      - backend/utils/init/postinit.c：删除 EXEC 专属 "重新加载 pg_hba.conf/pg_ident.conf" 块（`#ifdef EXEC_BACKEND` 包裹，约 40 行），因 fork 子进程已继承。
      - backend/replication/basebackup.c：删除 `noChecksumFiles[]` 中 EXEC 专用的 `config_exec_params` 排除项。
      - backend/storage/lmgr/{proc,predicate,lock,lwlock}.c：删除 `#ifndef EXEC_BACKEND`/`#ifdef EXEC_BACKEND` 包裹与 `Assert`、简化相关注释（fork 继承语义）。
      - backend/port/posix_sema.c：删除 `USE_NAMED_POSIX_SEMAPHORES && EXEC_BACKEND` 的 `#error` 块，简化文件头注释。
      - include/pg_config_manual.h：删除描述已不存在的 EXEC_BACKEND 宏的说明注释块。
      - 其余纯注释提及（main.c、postmaster.c、guc.c、pgtz.c、mcxt.c、walreceiver.c、walsender.c、parallel.c、be-secure-openssl.c、buf_init.c、shmem.c、latch.c、fd.c、fork_process.c、elog.c）：将注释中的 "EXEC_BACKEND case / SubPostmasterMain" 描述改写为 fork() 继承语义，集中清理以免误导。
      - `make -j16` 全量编译通过（修复 pgstat.c 合并 fork 分支后残留的 `#endif`）。
    - 清理 `src/backend/postmaster/postmaster.c` 中全部 WIN32 死代码（minipg 仅 Unix/Linux）：
      - 删除 `PostmasterHandle`（HANDLE 全局变量）及其 `#else` 分支，仅保留 Unix 的 `postmaster_alive_fds[]`。
      - 删除 `InitPostmasterDeathWatchHandle()` 内 `DuplicateHandle`/`GetLastError` 的 Windows 分支与 `#ifndef WIN32`/`#else`/`#endif` 包裹，仅保留 Unix `pipe()` 实现。
      - 删除 Windows 专用的 `waitpid()` 子集实现（~50 行）及 `pgwin32_deadchild_callback()` 回调函数整块（含 `CreateIoCompletionPort` 初始化）。
      - 删除 I/O completion port 初始化块（`CreateIoCompletionPort`）。
      - 删除 syslogPipe 关闭逻辑中的 `CloseHandle` 分支，仅保留 Unix `close()`。
      - 删除 `LogChildExit()` 中 `exception 0x%X` 的 Windows 异常消息分支，仅保留 Unix `signal N: strsignal` 消息。
      - 删除 `CleanupBackgroundWorker()` / `CleanupBackend()` 中 `ERROR_WAIT_NO_CHILDREN` 的 Windows 专用处理块。
      - 删除全部 Windows 缺少 sigaction 时手动 `PG_SETMASK(&BlockSig)`/`PG_SETMASK(&UnBlockSig)` 的变通块（sigusr1_handler、SIGHUP_handler、pmdie、reaper、process_startup_packet_die 共 8 处），并简化相应注释。
      - 删除 `extern char **environ` 的 Windows 条件声明块（Unix 下 environ 已由 `<unistd.h>` 提供）。
      - `postmaster.c:4441` 用户指定的 `PG_SETMASK(&BlockSig)` Windows 变通块已删除。
      - `make -j16` 编译通过；`make check` 回归测试 216 项全部通过。
    - 继续清理 backend 核心代码中的 WIN32 死代码（minipg 仅 Unix/Linux，共约 130+ 处）：
      - 去 `#ifndef WIN32` / `#ifdef WIN32` 包裹，保留 Unix 侧：
        - `utils/misc/guc.c`：`<sys/mman.h>` 与 `shared_memory_options[]` 中 sysv 项去掉 `WIN32` 包裹，删除 windows 枚举项（SHMEM_TYPE_WINDOWS 变孤儿，保留无害）。
        - `storage/file/fd.c`、`storage/ipc/dsm.c`、`storage/ipc/dsm_impl.c`：`<sys/mman.h>` 去 `WIN32` 包裹。
        - `utils/init/miscinit.c`：`getppid()` 去 `WIN32` 分支（删 Windows `my_p_pid=0` 分支）。
        - `postmaster/fork_process.c`：整个 `fork_process()` 去 `#ifndef WIN32` 包裹。
        - `utils/fmgr/dfmgr.c`：结构体 `inode` 成员、`SAME_INODE` 宏、`inode` 赋值去 `WIN32` 包裹。
        - `commands/copyto.c`：行结束符去 `WIN32` 分支，仅留 Unix `\n`。
        - `utils/misc/ps_status.c`：`extern char **environ` 改为无条件声明。
        - `libpq/ifaddr.c`：删除 Win32 版 `pg_foreach_ifaddr`（Winsock）整段，`#elif HAVE_GETIFADDRS` 改 `#ifdef`。
        - `storage/ipc/pmsignal.c`：`PostmasterDeathTest()` 删除 `WaitForSingleObject(PostmasterHandle)` 的 Windows 分支，仅留 Unix `read()` 逻辑。
      - 删除纯 Windows-only 整块：
        - `utils/init/miscinit.c`：删 `_setmode(stderr, _O_BINARY)` 块。
        - `utils/adt/misc.c`：删 `_setmode(fd, _O_TEXT)` 块。
        - `utils/adt/selfuncs.c`：删 `strxfrm` 返回 `INT_MAX` 的 Windows 特殊处理块。
        - `libpq/hba.c`：LDAP 头文件去 `WIN32` 分支，仅留 `<ldap.h>`。
      - 修复编译：补全 `fork_process.c` 被误删的 `result`/`oomfilename` 声明、`pmsignal.c` 残留的 `#ifndef WIN32` 未闭合。
      - 暂缓（高风险，需单独一轮重构）：`utils/error/elog.c` eventlog 输出路径（含 `pgwin32_message_to_UTF16` 跨文件调用）、`utils/mb/mbutils.c` 的 `pgwin32_message_to_UTF16` 与悬空 `if`、`utils/adt/pg_locale.c`（~42 处 WIN32/_MSC_VER 混合）、`storage/ipc/latch.c`（WAIT_USE_WIN32 宏贯穿 33 处四路选择链）、`utils/adt/varlena.c`、`postmaster/pgstat.c` 的 `pgwin32_noblock`、`storage/file/fd.c` 的 `GetLastError` 重试、`guc.c` 的 `SHMEM_TYPE_WINDOWS` 枚举引用。
      - `make -j16` 编译通过（修复 fork_process.c / pmsignal.c 两处误删）。
- 2026-07-31: 裁剪 bin 运维/性能/升级类工具（与内核学习无关，且非回归测试依赖）：删除 `src/bin/` 下 `pgbench`、`pg_amcheck`、`pg_archivecleanup`、`pg_checksums`、`pg_resetwal`、`pg_test_fsync`、`pg_test_timing`、`pg_upgrade`、`pg_verifybackup` 以及 `scripts/`（clusterdb/createdb/createuser/dropdb/dropuser/reindexdb/vacuumdb/pg_isready）。同步修改 `src/bin/Makefile` 的 `SUBDIRS` 移除对应条目。保留 `initdb`/`pg_ctl`/`psql`/`pg_config`（PostgresNode.pm 测试框架硬依赖）、`pg_dump`（test_pg_dump 依赖 + 逻辑转储教学）、`pg_basebackup`/`pg_rewind`（replication 子系统保留，待阶段 8 再删）、`pg_controldata`/`pg_waldump`（内核观察工具）。`make check-world` 通过。详见下文。
- 2026-08-02: 裁剪 SSL/TLS 与 GSSAPI 传输加密，认证收敛为 trust/reject/password/scram-sha-256 四种（md5 仅删认证协商、保留存储格式兼容）。详见下文。

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

**后续修复（同次裁剪遗留）**：`make clean` 曾报错 `make[4]: *** win32: No such file or directory`。
原因：初次 Windows 裁剪删除了 `src/backend/port/win32/` 目录，但 `src/backend/port/Makefile`
仍残留两处引用——第 30-32 行 `ifeq ($(PORTNAME), win32) SUBDIRS += win32`（条件，仅 win32 平台生效）
与第 48 行 `distclean clean:` 规则中无条件的 `$(MAKE) -C win32 clean`。由于 minipg 已仅支持 Linux，
已移除该 `SUBDIRS` 条件块与 clean 规则中的 win32 递归。修复后 `make clean` 与 `make check-world` 均通过。

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

## 裁剪：非 plpgsql 过程语言（阶段 3，存储过程）

**目的**：存储过程/PL 对内核学习的价值不在于"多"，而在于展示 **fmgr 的 call-handler 扩展点** 与 **SPI（Server Programming Interface）回连执行器** 两大机制。plperl / plpython / pltcl 三种语言均为"外部解释器 + 胶水代码"桥接，原理与 plpgsql 完全一致，仅宿主语言不同；体积大（合计约 4 万行）、依赖重（perl/python/tcl 解释器）、对内核无新启发。故仅保留 **plpgsql**（PG 自研、零外部依赖、最贴近内核，是触发器/函数默认教学载体），删除其余三种。

**删除内容**：
- `src/pl/plperl/`（~24,371 行）
- `src/pl/plpython/`（~10,878 行）
- `src/pl/tcl/`（pltcl，~4,761 行）

**保留**：`src/pl/plpgsql/`（~31,243 行，PG 原生过程语言）

**修改的构建文件**：
- `src/pl/Makefile`：移除 `ifeq ($(with_perl)/with_python/with_tcl)` 条件块与 `ALWAYS_SUBDIRS`，SUBDIRS 仅保留 `plpgsql`。
- `configure.ac` 中 `with_perl/with_python/with_tcl` 选项**保留**（其用于定位 perl/python 可执行文件供核心代码生成使用，与已删 PL 目录解耦，不可删）；`src/Makefile.global` 中 `with_perl/with_python/with_tcl` 变量定义亦保留。

**说明**：本机 autoconf 版本不符，未重跑 configure；`src/pl/Makefile` 不再用这些选项加入子目录，构建即生效。

**验证**：`make -j4` 编译成功；`make check-world` 全部通过（EXIT=0）。plpgsql 自身 13 个回归测试全部 ok。

## 裁剪：WIN32 死代码清理（第二轮，高风险文件）

**目的**：minipg 明确仅支持 Unix/Linux，`#ifdef WIN32` / `_MSC_VER` / `__CYGWIN__` 分支在本平台**永不参与编译**，属纯死代码。第一轮已清理 backend 核心低风险文件，本轮处理此前暂缓的高风险文件（跨文件引用、条件嵌套复杂、`#ifdef` 切割 if/else 结构）。

**删除内容**（按文件）：

*等待事件多路复用*
- `storage/ipc/latch.c`（33 处，最大项）：删除整个 `WAIT_USE_WIN32` 实现路径——实现选择宏分支、`WaitEventSet.handles` 成员、`WaitEventAdjustWin32()`（41 行）、Windows 版 `WaitEventSetWaitBlock()`（225 行）、`Latch.event` 的 `CreateEvent`/`SetEvent`、`FreeWaitEventSet` 的 `WSACloseEvent` 清理等。**保留** epoll/kqueue/poll 三套可移植实现，不破坏抽象层。

*本地化*
- `utils/adt/pg_locale.c`（32 处）：删除 `IsoLocaleName()`/`get_iso_localename()` 整块（257 行）、`strftime_win32()`（59 行）、`PGLC_localeconv()` 与 `cache_locale_time()` 中的 `save_lc_ctype` 保存/恢复逻辑、`_create_locale()` 分支、`wchar2char`/`char2wchar` 的 UTF16 分支、`GetNLSVersionEx` 排序版本分支。
- `utils/mb/mbutils.c`：删除 `pgwin32_message_to_UTF16()`（73 行，其唯一调用方已随 elog.c 删除）、`pg_bind_textdomain_codeset()` 的 Windows 分支。

*日志*
- `utils/error/elog.c`：删除 `write_eventlog()` + `GetACPEncoding()`（110 行）、`write_console()` 的 `WriteConsoleW` 分支、`vwrite_stderr()` 的服务模式分支、两处 eventlog 输出调用点。
- 连带删除已成孤儿的 **Windows 事件日志 GUC**：`event_source` 变量与 GUC 定义（guc.c）、`DEFAULT_EVENT_SOURCE` 宏（pg_config_manual.h）、`log_destination` 的 `eventlog` 关键字、postgresql.conf.sample 中对应配置项。（`LOG_DESTINATION_EVENTLOG` 位值保留，避免 CSVLOG 等位值重新编号。）

*其他*
- `utils/adt/varlena.c`：删除 Windows UTF-8 排序回退限制、UTF-16 比较分支（87 行）
- `storage/file/fd.c`：`FileRead`/`FileWrite` 的 `GetLastError`/`_dosmaperr` 映射（保留 EINTR 重试）、`pg_truncate` 的 Windows 实现、`_commit`、3 处 `!defined(WIN32)` 条件简化
- `access/transam/xlog.c`、`access/transam/xlogarchive.c`：删除 WAL 文件"先改名再删除"的 Windows FILE_SHARE_DELETE 变通逻辑
- `postmaster/pgstat.c`：删除 2 秒轮询变通（改回无限等待）与 `pgwin32_noblock`
- `libpq/auth.c`：删除 `winldap.h`/`ldap_sslinit`/动态加载 WLDAP32.DLL 分支、`auth_peer` 的 Windows 桩
- `libpq/be-secure-common.c`：私钥文件权限检查不再跳过
- `utils/init/miscinit.c`：数据目录属主/权限检查不再跳过（3 处）
- `utils/misc/ps_status.c`：删除 `PS_USE_WIN32` 模式
- `postmaster/postmaster.c`、`postmaster/pgarch.c`、`tcop/postgres.c`、`commands/dbcommands.c`、`commands/collationcmds.c`、`replication/basebackup.c`、`replication/libpqwalreceiver/`、`utils/adt/genfile.c`、`utils/adt/misc.c`、`port/atomics.c`、`utils/misc/guc.c`：各 1-3 处小块

**行为变化说明**：以下检查在 Windows 上原本被跳过，现无条件生效（在 Unix 上本就生效，故无实际行为变化）：数据目录属主/权限检查、SSL 私钥权限检查。`update_process_title` 默认值固定为 `true`（Unix 原值）。

**保留项**：`utils/adt/float.c`、`utils/adt/numutils.c` 中的 `_MSC_VER` 为**编译器特性检测**（HUGE_VALF、`_BitScanReverse`）而非平台代码，予以保留；Bison 生成文件（`gram.c` 等）不手改。

**验证**：`make -j` 全量编译无 error/warning；`make check-world` 全部通过（EXIT=0），主回归 216 项、plpgsql 107 项及各 contrib 套件均 ok。

## 裁剪：SSL/TLS 与 GSSAPI 传输加密，认证收敛为口令类

**目的**：minipg 面向内核学习，SSL/TLS 与 GSSAPI 传输加密是一整套与"连接建立/认证"正交的密码学子系统，且当前构建（`pg_config.h` 中 `USE_OPENSSL`/`ENABLE_GSS` 等均 `#undef`）这些特性本就关闭，相关代码全为编译期死代码。删除后可显著降低连接建立与认证流程的阅读成本。认证方式收敛为仅 `trust`/`reject`/`password`/`scram-sha-256` 四种口令类方法。

**为什么可以删**：
- 当前构建所有 SSL/GSS/LDAP/PAM/BSD/SSPI 宏均未定义，待删代码绝大部分是 `#ifdef` 未命中分支，二进制中根本不存在，删除不改变运行时行为。
- `secure_*` 抽象层是 `pqcomm.c` 与 `fe-secure.c` 的稳定 I/O 契约，保留函数签名、仅拍平 SSL/GSS 分支为裸 socket 直通，避免跨模块级联改动。
- 客户端 libpq 同步裁剪，保持前后端协议一致。

**删除的认证方式**：`md5`（仅删认证协商方式，保留存储格式识别与校验）、`ident`、`peer`、`gss`、`sspi`、`pam`、`bsd`、`ldap`、`cert`、`radius`。
**保留的认证方式**：`trust`、`reject`（含内部 `uaImplicitReject`）、`password`（明文）、`scram-sha-256`。

**删除的文件与目录**：
- 后端：`src/backend/libpq/be-secure-openssl.c`、`be-secure-common.c`、`be-secure-gssapi.c`、`be-gssapi-common.c`、`README.SSL`
- 客户端：`src/interfaces/libpq/fe-secure-openssl.c`、`fe-secure-common.c`、`fe-secure-gssapi.c`、`fe-gssapi-common.c`
- 头文件：`src/include/common/openssl.h`、`src/include/libpq/be-gssapi-common.h`、`src/interfaces/libpq/fe-gssapi-common.h`
- 测试目录：`src/test/ssl/`、`src/test/kerberos/`、`src/test/ldap/`、`src/test/modules/ssl_passphrase_callback/`

**修改的文件（按层）**：
- 头文件：`libpq-be.h`（删 `Port` 的 SSL/GSS/SSPI 字段块）、`hba.h`（`UserAuth` 枚举收敛为 5 项、删 `HbaLine` 中 ldap*/radius*/pamservice/krb_realm/clientcert 字段与 `ClientCertMode` 枚举）、`crypt.h`（删 `md5_crypt_verify` 原型）、`libpq.h`、`libpq-int.h`、`libpq-fe.h`（删 `PQssl*`/`PQgss*` 声明）
- 后端核心：`be-secure.c`（10 处分支拍平为 `secure_raw_read/write` 直通）、`postmaster.c`（保留 `SSLRequest`/`GSSENCRequest` 的 `'N'` 应答与重读启动包逻辑，删握手调用分支）、`pqcomm.c`/`postinit.c`/`main.c`（去宏引用）
- 认证：`auth.c`（删 PAM/BSD/LDAP/SSL/GSS/SSPI 实现与外部认证函数，`ClientAuthentication` 的 switch 收敛为 5 分支，删 `md5_crypt_verify` 调用路径）、`hba.c`（`UserAuthName[]` 收敛为 5 项并与枚举严格对齐，方法关键字解析仅接受 4 种、其余走 `unsupauth` 报错，删 ldap*/radius*/pam*/sspi 专属选项解析）、`crypt.c`（删 `md5_crypt_verify` 实现，保留 `get_password_type`/`encrypt_password`/`plain_crypt_verify` 的 md5 存储兼容）
- SCRAM：`auth-scram.c`/`fe-auth-scram.c` 禁用依赖 TLS 通道绑定的 `SCRAM-SHA-256-PLUS` 变体（通告与协商）；`port/pg_strong_random.c`（删 OpenSSL `RAND_bytes` 分支，保留 `/dev/urandom`）、`port/timingsafe_bcmp.c`（删 `CRYPTO_memcmp` 分支保留自实现）
- GUC/视图/配置：`guc.c`（删全部 `ssl_*` 参数、`check_ssl`、`ssl_renegotiation_limit`、`CONN_AUTH_SSL` 分组、`ssl_protocol_versions_info`）、`postgresql.conf.sample`（删整个 SSL 配置段）、`pg_hba.conf.sample`（方法列表仅留 4 种）、`system_views.sql`（删 `pg_stat_ssl`/`pg_stat_gssapi` 两个视图）、`backend_status.c`（去 SSL/GSS 状态采集，但保持 `pg_stat_get_activity` 元组列数不变，相关列恒 NULL/false）、`catalog/catversion.h`（bump CATALOG_VERSION_NO）
- 客户端：`fe-secure.c`（拍平）、`fe-connect.c`（删 sslmode/gssencmode 等连接参数与 SSL/GSS 状态机）、`fe-auth.c`（删 `AUTH_REQ_MD5`/`GSS`/`SSPI`/`KRB5`/`SCM_CREDS` 分支）、`exports.txt`（删导出符号、不回收序号）
- 构建系统：`configure.ac`（删 `--with-ssl`/`--with-openssl`/`--with-gssapi`/`--with-ldap`/`--with-pam`/`--with-bsd-auth` 及 OpenSSL 随机源选择分支）、重生成 `configure` 与 `pg_config.h.in`（使用 autoconf 2.69 忠实重生成）、`Makefile.global.in`、`pg_config_manual.h`（删 `USE_SSL` 派生）、`config/programs.m4`（删 `PGAC_LDAP_SAFE`）、`utils/misc/Makefile`、`backend/storage/lmgr/Makefile`、libpq 与 libpq 后端 `Makefile`（OBJS 与条件块）
- 测试：`src/test/Makefile`、`src/test/modules/Makefile`、`authentication/t/001_password.pl`（删 md5 认证用例，plan 23→21）、`initdb/initdb.c`（authmethod 合法值收敛为 trust/reject/password/scram-sha-256）、`regress/pg_regress.c` 与 `perl/TestLib.pm`（删 SSL/GSS 相关环境变量）、`psql/command.c`（删 `printSSLInfo`/`printGSSInfo` 及调用）、`regress/expected/rules.out`（删 `pg_stat_ssl`/`pg_stat_gssapi` 的 `pg_rules` 行）

**兼容性保证**：
- 服务端对标准 psql（默认 `sslmode=prefer`）发来的 `SSLRequest`/`GSSENCRequest` 仍正确回复单字节 `'N'` 后继续读真实启动包，外部驱动握手不中断。
- `md5` 存储的口令仍可被 `password`（明文）/scram 认证流程校验（`plain_crypt_verify` 既有能力），避免既有 `pg_authid` 数据失效。
- `password_encryption=md5` 仍可作为存储格式保留。

**验证**：`make`（autoconf 2.69 重生成 configure）全量编译通过；`make check-world` 全部通过（EXIT=0），主回归 216 项全 ok（含 `rules` 测试因删两个系统视图需同步更新预期）；全仓库扫描 `pg_stat_ssl`/`pg_stat_gssapi`/`be_tls_`/`pq_gss`/`AUTH_REQ_MD5`/`USE_OPENSSL`/`PQsslInUse` 等残留符号为 0 处。

**注意事项**：本次 `configure` 使用 autoconf 2.69 忠实重生成（与 PG14 要求一致），无需放宽版本宏。

## 裁剪：彻底移除 Native Language Support（ENABLE_NLS）翻译子系统

**目的**：minipg 面向内核学习，GNU gettext 翻译体系（`ENABLE_NLS` + `.po`/`.mo` + `nls.mk` + `bindtextdomain` 调用链）是一套与内核逻辑完全正交、且当前构建本就关闭（死代码）的消息展示层。拆除后可显著减少 `c.h`/`elog.c`/`pg_locale.c`/`libpq` 等处跨模块的条件编译分支，降低阅读干扰。

**设计原则（半裁剪）**：保留 `gettext` 空宏直通层（`c.h` 中 `#define gettext(x) (x)` 等无条件保留），使源码中所有 `errmsg(_("..."))`/`libpq_gettext("...")` 调用**无需任何改动**即编译为原文直通。这样未来若需重新启用翻译，只需恢复 `#ifdef ENABLE_NLS` 外壳即可，业务源码零回归成本。

**删除的认证/构建开关**：
- `configure.ac`：`PGAC_ARG_OPTARG(enable, nls, ...)` 整段、`if test "$enable_nls" = yes; then PGAC_CHECK_GETTEXT; fi` 块
- `pg_config.h.in`：`#undef ENABLE_NLS`
- `configure`（autoconf 2.69 重生成）

**删除的文件**：
- 构建描述：12 个 `src/**/nls.mk`、顶层 `src/nls-global.mk`
- 翻译数据：`*.po`（前次已裁剪，仅留 `zh_CN.po`）

**修改的文件（按层）**：
- 头文件：`c.h`（删 `#ifdef ENABLE_NLS #include <libintl.h> #endif`，`gettext`/`dgettext`/`ngettext`/`dngettext` 空宏移出条件保护、改为无条件保留并补注释）、`libpq-int.h`（`libpq_gettext`/`libpq_ngettext` 由外部函数声明+条件空宏改为无条件空宏）、`pg_wchar.h`（删 `pg_enc2gettext` 类型、`pg_enc2gettext_tbl` 声明、`pg_bind_textdomain_codeset` 的 `#ifdef ENABLE_NLS` 声明）
- 后端核心：`elog.c`（`err_gettext` 拍平为 `return str;`）、`pg_locale.c`（`SetMessageEncoding` 的 NLS 分支拍平为 `GetDatabaseEncoding()`，保留编码核心逻辑）、`miscinit.c`（`pg_bindtextdomain` 拍平为空函数）、`mbutils.c`（删 `#ifdef ENABLE_NLS` 包住的 `raw_pg_bind_textdomain_codeset`/`pg_bind_textdomain_codeset` 整块）
- 字符集：`encnames.c`（删 `pg_enc2gettext_tbl[]` 定义块，仅 NLS 使用）
- 客户端 libpq：`fe-misc.c`（删 `#ifdef ENABLE_NLS` 包住的 `libpq_binddomain`/`libpq_gettext`/`libpq_ngettext` 实现整块）、`exec.c`（删 `bindtextdomain`/`textdomain`/`setenv PGLOCALEDIR` 块）
- 工具：`print.c`（两处 `printTableAddHeader`/`printTableAddCell` 删 `#ifdef ENABLE_NLS` 翻译块，无条件保留 `(void) translate;` 消未用参数警告）
- 构建：`src/Makefile`（删 `install-local` 安装 `nls-global.mk` 的行）、`src/Makefile.global.in`（删 `enable_nls = @enable_nls@` 变量行与 NLS 递归构建块，改为说明性注释）

**保留项（兼容性保证）**：
- `gettext`/`dgettext`/`ngettext`/`dngettext`/`libpq_gettext`/`libpq_ngettext` 空宏：`errmsg(_("..."))` 等数百处调用点**保持原样不改**，编译后透明直通原文
- `pg_enc`、`pg_wchar`、`GetDatabaseEncoding`、`SetMessageEncoding`、locale/排序规则（ICU 除外）等**字符集核心**：完全保留，与翻译层正交，不受影响
- `PG_TEXTDOMAIN(...)` 宏：仅为字符串常量标识，空宏环境下被忽略，保留无害

**验证**：autoconf 2.69 重生成 `configure`/`pg_config.h.in` 后 `./configure` + `make -j4` 全量编译通过（exit 0，无 error/warning）；`make check-world` 全量通过（exit 0，主回归 216 项、plpgsql 107 项及各 contrib 套件均 ok）。全仓库扫描 `ENABLE_NLS`/`pg_bind_textdomain_codeset`/`bindtextdomain` 等功能符号残留为 0 处（仅剩空宏定义与注释中的说明文字）。

**注意事项**：本次 `configure` 使用 autoconf 2.69 忠实重生成（与 PG14 要求一致）。`src/Makefile` 的 `install-local` 原会安装已删除的 `nls-global.mk`，已一并移除该行，`make install`/`make check-world` 的 temp-install 阶段不再报错。


## 裁剪：彻底移除 ICU（International Components for Unicode）支持

**目的**：minipg 仅支持 Unix/Linux 固定环境，ICU 解决的"跨 OS 版本排序可移植/可复现"问题在此场景下价值很低，而其代价（外部 libicu 依赖、二进制膨胀、构建复杂度、大量条件编译分支）对精简目标是负担。本次按用户确认**彻底删除**（含 `--with-icu` 配置选项、COLLPROVIDER_ICU 枚举、所有 USE_ICU 代码分支、icu_to_uchar/icu_from_uchar 辅助函数、pg_enc2icu_tbl 编码映射表、相关回归测试）。

**删除的配置开关与构建文件**：
- `configure.ac`：删 `PGAC_ARG_OPTARG(with, icu, ...)` 段、`PGAC_CHECK_ICU` 调用、`ICU_CFLAGS`/`ICU_LIBS` 变量与 `with_icu` 结果写入；`src/include/pg_config.h.in` 删 `#undef USE_ICU`
- `configure`（autoconf 2.69 重生成）：删 ICU 检测块、`--with-icu` help 文本、ICU_CFLAGS/ICU_LIBS/with_icu 变量声明
- `src/Makefile.global.in`：删 `with_icu = @with_icu@` 与 ICU 链接变量
- `src/include/pg_config.h`：删 `#undef USE_ICU` 及注释

**删除的数据结构与枚举**：
- `pg_collation.h` / `pg_collation_d.h`：删 `COLLPROVIDER_ICU 'i'` 枚举值
- `pg_wchar.h`：删 `is_encoding_supported_by_icu` / `get_encoding_name_for_icu` 声明
- `encnames.c`：删 `pg_enc2icu_tbl[]` 表、`is_encoding_supported_by_icu` / `get_encoding_name_for_icu` 定义

**修改的 backend 文件（删 USE_ICU / COLLPROVIDER_ICU 分支，保留 libc 路径）**：
- `pg_locale.c`：删 ucnv.h include、icu_set_collation_attributes 前向声明、icu_to_uchar/icu_from_uchar/init_icu_converter/icu_set_collation_attributes 整段实现；`pg_newlocale_from_collation` 删 COLLPROVIDER_ICU 分支（保留 libc/error）；`get_collation_actual_version` 删 ICU 分支
- `varlena.c`：`varstr_cmp`/`varstrfastcmp_locale`/`varstr_abbrev_convert` 删 ICU sort-key 路径（ucol_strcollUTF8/ucol_getSortKey/ucol_nextSortKeyPart/icu_to_uchar），统一走 strcoll_l/strxfrm_l
- `formatting.c`：删 unicode/ustring.h include、ICU 辅助函数块、str_initcap/str_lower/str_upper 内 ICU 分支
- `varchar.c`：`hashbpchar`/`bpchar` 比较删 ICU 分支
- `hashfunc.c`：`hashbpchar`/`hashtextextended` 删 ICU sort-key 分支（非确定性 collation 仅 ICU 支持，删后走报错路径）
- `regc_pg_locale.c`：删 `PG_REGEX_LOCALE_ICU` 枚举值及 13 处 case 分支（u_isalpha/u_isupper/u_islower 等）
- `collationcmds.c`：删 "icu" provider 解析、"icu" collencoding 分支、`get_icu_language_tag`/`get_icu_locale_comment` 整段、pg_import_system_collations 内 ICU 检测块、ucol_countAvailable 导入循环；nondeterministic 检查简化为无条件报错
- `namespace.c`：`FindDefaultCollation` 删 ICU 编码判定分支
- `like.c` / `like_support.c`：删 COLLPROVIDER_ICU 分支（统一多字节/isalpha_l 判断）

**修改的前端/其他文件**：
- `psql/describe.c`：删除 `\dC` 中 `WHEN 'i' THEN 'icu'` 显示分支
- 回归测试：`src/test/regress/sql/collate.icu.utf8.sql`、`expected/collate.icu.utf8.out`、`expected/collate.icu.utf8_1.out`、`results/collate.icu.utf8.out` 四个文件删除；`parallel_schedule` 移除 `collate.icu.utf8` 调度项

**行为变化**：
- 创建 collation 不再支持 `provider='icu'`；指定会报 `unrecognized collation provider: icu`
- 不再支持 ICU collation 的 nondeterministic（大小写/重音不敏感）语义，相关 CREATE 直接报错
- 排序/哈希/正则/格式化全部走 libc locale，语义随 glibc 版本确定（固定环境下稳定）
- 二进制不再链接 libicu

**验证**：`./configure --without-icu` + `make -j` 全量编译通过（exit 0，无 error/warning）；`make check` 215 项、`make check-world` 全量通过（exit 0）。全仓库扫描 `USE_ICU`/`COLLPROVIDER_ICU`/`icu_to_uchar`/`pg_enc2icu_tbl` 等功能符号残留为 0 处（仅注释中的说明文字已同步清理）。

**注意事项**：本次 `configure` 使用 autoconf 2.69 忠实重生成。

## 裁剪 P0：彻底移除 Bonjour / Systemd / SELinux / XML / XSLT 支持

**目的**：这些特性在 Unix/Linux 固定部署场景下无用，且引入外部依赖（libdns_sd、libsystemd、libselinux、libxml2、libxslt）与条件编译分支。本次彻底删除对应配置开关与死代码（DTrace 按用户要求暂不裁剪）。

**删除的配置开关（configure）**：
- 移除 `--with-bonjour`、`--with-selinux`、`--with-systemd` 三个选项：help 文本、变量声明（with_bonjour/with_selinux/with_systemd）、AC_ARG_WITH 段、库/头检测块（selinux 链接检测、systemd sd-daemon.h 头检测、bonjour dns_sd.h 头检测）
- `src/include/pg_config.h`（生成）：删除 `#undef USE_BONJOUR`/`USE_SYSTEMD`/`HAVE_LIBXML2`/`HAVE_LIBXSLT` 占位宏

**删除的 backend 代码**：
- `postmaster.c`：删 USE_BONJOUR（dns_sd.h include、bonjour_sdref 变量、Bonjour 注册块、mDNS 关闭块）和 USE_SYSTEMD（6 处 sd_notify("STOPPING=1"/"READY=1") 块 + sd-daemon.h include）；删 enable_bonjour/bonjour_name 全局变量
- `guc.c`：删 check_bonjour 声明/函数、bonjour 与 bonjour_name 两个 GUC 定义（含 check_bonjour hook）
- `postgresql.conf.sample`：删 bonjour / bonjour_name 注释行

**说明**：LDAP、PAM、SELinux 的 backend 代码此前已被清理（auth.c 等无对应 USE_* 引用），本次仅移除 configure 开关与剩余宏；XML/XSLT 核心实现（xml.c）仍保留文件但内部功能由 USE_LIBXML 控制（当前 undef，编译为空壳），未做整文件删除以避免影响 SQL/JSON 相关路径。

**行为变化**：不再支持 Bonjour 服务发现、systemd 生命周期通知、SELinux 标签；无法创建 XML/XSLT 相关对象（原已因未启用而无法使用）。二进制不再链接 libdns_sd/libsystemd/libselinux/libxml2/libxslt。

**验证**：`./configure` + `make -j` 全量编译通过（exit 0，无 error/warning）；`make check` 215 项全部通过（exit 0）。全仓库扫描 `USE_BONJOUR`/`USE_SYSTEMD` 残留为 0 处（配置开关与代码分支均干净）。

**注意事项**：DTrace 未裁剪（用户要求暂留）。本次 `configure` 使用 autoconf 2.69 忠实重生成。

## 裁剪：tsearch + snowball（全文检索 + 词干提取）

**目的**：minipg 面向数据库内核学习，全文检索（tsearch）和 snowball 词干提取是一套与内核核心机制（存储/事务/优化器/执行器）正交、且代码量可观的功能模块。删除后可显著简化目录结构、消除 5 张系统表（`pg_ts_*`）及其 syscache/OCLASS/ObjectType 枚举、移除 150+ 个内置函数/类型/操作符，降低学习干扰。

**为什么可以删**：
- tsearch 模块（`src/backend/tsearch/`）无条件编译进主程序，但在内核学习中不被需要。
- snowball（`src/backend/snowball/`）是独立 `.so` 扩展，依赖词干提取的外部语言规则，对理解内核无帮助。
- 全文检索的 SQL 语法（CREATE TEXT SEARCH DICTIONARY/CONFIGURATION/PARSER/TEMPLATE）和相关内置函数（`to_tsvector`、`to_tsquery`、`ts_headline`、`ts_rank` 等）全部移除。
- 5 张系统表（`pg_ts_dict`、`pg_ts_config`、`pg_ts_config_map`、`pg_ts_parser`、`pg_ts_template`）及相关索引/依赖关系全部删除。

**删除的目录与文件**：
- `src/backend/tsearch/`（~50 个 C 源文件）
- `src/backend/snowball/`（词干提取库 + libstemmer）
- `src/backend/utils/adt/ts*.c`（12 个文件：tsvector、tsvector_op、tsvector_parser、tsquery、tsquery_cleanup、tsquery_op、tsquery_util、tsquery_rewrite、tsquery_gist、tsgistidx、tsginidx、tsrank）
- `src/backend/utils/cache/ts_cache.c`
- `src/backend/commands/tsearchcmds.c`
- `src/include/tsearch/`（全部头文件）
- 5 个目录数据文件：`src/include/catalog/pg_ts_{config,config_map,dict,parser,template}.h` 及对应的 `.dat`、`.d.h`（共 15 文件）
- 回归测试：`sql/tsearch.sql`、`sql/tsdicts.sql`、`sql/tstypes.sql` 及对应 expected/results 文件、`data/tsearch.data`
- 测试模块：`src/test/modules/test_parser/`、`src/test/modules/test_ddl_deparse/`（依赖 tsearch）
- contrib 全文检索扩展：`dict_int`、`dict_xsyn`、`unaccent`（随 contrib 裁剪批次删除）

**修改的构建系统文件**：
- `src/backend/Makefile`：移除 snowball 和 tsearch 子目录（SUBDIRS）
- `src/backend/catalog/Makefile`：移除 6 个 `pg_ts_*` 安装行与 CATALOG_HEADERS 中的 TS 条目
- `src/backend/commands/Makefile`：移除 tsearchcmds.o
- `src/backend/utils/adt/Makefile`：移除 12 个 ts*.o
- `src/backend/utils/cache/Makefile`：移除 ts_cache.o
- `src/include/Makefile`：移除 tsearch 和 tsearch/dicts 头目录
- `src/Makefile`：移除 snowball 相关条目

**修改的 C/H 源文件（按层）**：
- 语法解析：`gram.y`（删除 T_AlterTSDictionaryStmt/T_AlterTSConfigurationStmt/T_CreateTextSearch*Stmt 语法节点，删除 n->objectType = OBJECT_TSDICTIONARY 等 5 处赋值）
- 节点定义：`parsenodes.h`（删除 OBJECT_TSCONFIGURATION/DICTIONARY/PARSER/TEMPLATE 枚举值，ObjectType 枚举重新编号）、`copyfuncs.c`、`equalfuncs.c`
- 目录管理：`namespace.c`（删除 8 个 get_ts_*_oid 声明与实现，约 500 行）、`namespace.h`、`genbki.pl`（删除 pg_ts_* 的 OID 校验块）、`aclchk.c`、`dependency.c`/`dependency.h`（删除 OCLASS_TSPARSER/DICT/TEMPLATE/CONFIG 枚举值，ObjectClass 枚举重新编号）
- 对象地址：`objectaddress.c`（删除 ObjectProperty 数组中 4 个 TS 条目与 getObjectClass 等 switch 中的 TS case；恢复误删的 check_object_ownership 中 OBJECT_ACCESS_METHOD case）、`objectaddress.h`
- 目录支持：`pg_shdepend.c`、`pg_proc.dat`（删除 150+ 个 TS 函数定义）、`pg_type.dat`（删除 regconfig/regdictionary 类型定义）、`pg_cast.dat`（删除 regconfig/regdictionary 的 cast）
- 类型处理：`regproc.c`（删除 regconfigin/out/regdictionaryin/out 4 函数）、`selfuncs.c`（删除 REGCONFIGOID/REGDICTIONARYOID case 标签）、`catcache.c`（删除 REGCONFIGOID/REGDICTIONARYOID 类型哈希处理）
- 缓存系统：`syscache.c`（删除 5 个 pg_ts_* 的 include 与 5 个 syscache 条目）、`syscache.h`（同步删除对应枚举值）
- 命令层：`alter.c`（删除 OCLASS_TSPARSER/DICT/TEMPLATE/TSCONFIG case）、`dropcmds.c`、`event_trigger.c`、`seclabel.c`、`tablecmds.c`（删除 OCLASS_TSPARSER 等 4 个 case 标签）
- GUC 配置：`guc.c`（删除 `default_text_search_config` GUC 定义与 tsearch/ts_cache.h include）
- initdb：`initdb.c`（删除 `-T --text-search-config` 选项、`default_text_search_config` 变量、`setup_text_search()` 调用、`tsearch_config_languages[]` 数组与 `find_matching_ts_config()` 函数、postgresql.conf 模板中的 text_search 配置块、pg_depend 中对 pg_ts_* 的 INSERT）

**修改的回归测试文件**：
- `parallel_schedule`：移除 `tstypes`/`tsearch`/`tsdicts` 调度项
- `create_table.sql`：移除 `test_tsvector` 表创建
- `create_index.sql`：移除 tsvector opclass 测试段
- `json.sql`/`jsonb.sql`：移除 json→tsvector 转换测试段
- `type_sanity.sql`：移除 regconfig/regdictionary/tsvector/tsquery/gtsvector 类型引用
- `alter_generic.sql`：移除 TS 对象 alter 测试
- `object_address.sql`：移除 TS 对象地址测试
- `guc.sql`：用 `work_mem` 替换已删除的 `default_text_search_config` GUC
- `copy.sql`/`create_type.sql`/`drop_if_exists.sql`/`opr_sanity.sql`/`sanity_check.sql`：移除 TS 类型/函数/操作符相关测试
- `psql.sql`：移除 `\dT+` 等命令中 TS 类型引用
- `amutils.sql`：移除 tsvector 相关索引访问方法测试
- `alter_table.sql`：移除 TS 类型列测试
- `oidjoins.sql`：移除 pg_ts_* 目录外键检查
- 删除 `test_parser`、`test_ddl_deparse` 测试模块
- `system_functions.sql`：删除 `ts_debug` 函数定义（两处重载）
- 14 个 expected 文件同步更新以匹配新的实际输出

**行为变化**：
- 不再支持 `CREATE TEXT SEARCH DICTIONARY/CONFIGURATION/PARSER/TEMPLATE` 语法
- 不再支持 `to_tsvector()`、`to_tsquery()`、`ts_headline()`、`ts_rank()` 等全文检索函数
- 不再支持 `tsvector`、`tsquery`、`regconfig`、`regdictionary`、`gtsvector` 类型
- `default_text_search_config` GUC 已删除
- `initdb` 的 `-T` 选项已删除
- 5 张 `pg_ts_*` 系统表不再存在
- `ObjectClass` 和 `ObjectType` 枚举值重新编号（TS 条目被移除，后续枚举值下移）

**验证**：`make check` 全部 212 项通过。`make check-world` 全部通过（EXIT=0）。全仓库扫描 `pg_ts_`、`tsvector`、`tsquery`、`regconfig`、`regdictionary`、`tsearch` 等功能符号在 C/H 源码中残留为 0 处（仅注释中的说明文字保留）。

**注意事项**：本次裁剪涉及枚举重新编号（`ObjectClass` 和 `ObjectType`），需特别注意以下修复：
- `check_object_ownership` 中 `OBJECT_ACCESS_METHOD` 原与 `OBJECT_TSPARSER`/`OBJECT_TSTEMPLATE` 共享 case 标签，删除 TS 条目后必须为 ACCESS_METHOD 单独添加 case（否则值 0 fall through 到 default 报 unrecognized object type）。
- `syscache.c` 的枚举与 `cacheinfo[]` 数组必须严格对齐（`StaticAssertStmt` 编译期检查）。
- 所有 switch on `ObjectClass`/`ObjectType` 的 case 标签必须与重新编号后的枚举值一致。
- 编译时需确保 `configure` 已运行，`genbki.pl` 已生成正确的 `pg_*_d.h` 头文件。
