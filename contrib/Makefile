# contrib/Makefile

subdir = contrib
top_builddir = ..
include $(top_builddir)/src/Makefile.global

# 所有 contrib 扩展（pageinspect/pg_buffercache/pg_freespacemap/pgrowlocks/
# pg_visibility 等）的安装脚本与回归测试均依赖
# CREATE FUNCTION / CREATE AGGREGATE（扩展本质即定义 SQL 函数），而
# CREATE FUNCTION DDL 已在 FUNCTION/PROCEDURE 裁剪中移除，这些扩展已无法安装，
# 故不再构建/测试 contrib，从回归套件移除。
# 扩展源码目录暂予保留（不纳入构建），待后续决定是否彻底裁剪；
# contrib-global.mk 是构建基础设施，被 src/test/modules/*/Makefile 引用，必须保留。
SUBDIRS =

$(recurse)
$(recurse_always)
