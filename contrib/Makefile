# contrib/Makefile

subdir = contrib
top_builddir = ..
include $(top_builddir)/src/Makefile.global

# 所有 contrib 扩展（amcheck/pageinspect/pg_buffercache/pg_freespacemap/
# pg_surgery/pgrowlocks/pgstattuple/pg_visibility/spi 等）的安装脚本与回归测试
# 均依赖 CREATE FUNCTION / CREATE AGGREGATE（扩展本质即定义 SQL 函数），而
# CREATE FUNCTION DDL 已在 FUNCTION/PROCEDURE 裁剪中移除，这些扩展已无法安装，
# 故不再构建/测试 contrib，从回归套件移除（仅保留源码目录，不影响内核）。
SUBDIRS =

$(recurse)
$(recurse_always)
