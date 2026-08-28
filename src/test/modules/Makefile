# src/test/modules/Makefile

subdir = src/test/modules
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# 注：snapshot_too_old 依赖 pg_sleep() 函数制造 old_snapshot 时间窗口，而 minipg 已裁剪 pg_sleep，其 3 个测试均 setup/运行期报 "function pg_sleep(integer) does not exist"，故从回归套件移除。
# 注：test_extensions 的扩展脚本 test_ext_cine--1.0.sql 使用 CREATE FOREIGN DATA WRAPPER / CREATE SERVER / CREATE SEQUENCE，而 minipg 已裁剪 FDW 与 sequence 功能，CREATE EXTENSION 直接报语法错误，整体无法运行，故从回归套件移除（核心是验证 extension 冲突检测，但其前置依赖的 FDW/sequence 在 minipg 不存在）。
# 注：test_custom_types 定义自定义基础类型 int_custom（CREATE TYPE 基础形式 + CREATE OPERATOR 建运算符类），
# minipg 已裁剪 CREATE TYPE 基础形式与 CREATE OPERATOR，CREATE EXTENSION 直接报语法错误，整体无法运行，故移除。
SUBDIRS = \
		  delay_execution \
		  test_bloomfilter \
		  test_integerset \
		  test_misc \
		  test_rbtree \
		  test_shm_mq


$(recurse)
$(recurse_always)
