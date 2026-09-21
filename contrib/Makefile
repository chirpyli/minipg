# contrib/Makefile

subdir = contrib
top_builddir = ..
include $(top_builddir)/src/Makefile.global

# minipg 已彻底裁剪全部 contrib 扩展（pageinspect/pg_buffercache/pg_freespacemap/
# pg_visibility 等）：扩展的安装脚本与回归测试均依赖 CREATE FUNCTION /
# CREATE AGGREGATE（扩展本质即定义 SQL 函数），而 CREATE FUNCTION DDL 已在
# FUNCTION/PROCEDURE 裁剪中移除，这些扩展已无法安装。
# 仅保留 contrib-global.mk 作为构建基础设施，它被
# src/test/modules/*/Makefile 引用，必须保留。
SUBDIRS =

$(recurse)
$(recurse_always)
