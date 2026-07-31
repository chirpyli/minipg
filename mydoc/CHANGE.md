# 变更日志

- 2026-07-13: 提交postgres 14.23版本
- 2026-07-31: 裁剪跨平台兼容性，仅保留Linux。删除所有 Windows / MinGW / MSVC / Cygwin / MSYS 专属代码与构建脚本，回归测试 `make check-world` 全部通过。详见下文。

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

