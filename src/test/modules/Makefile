# src/test/modules/Makefile

subdir = src/test/modules
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# 其余模块（commit_ts、delay_execution、libpq_pipeline、snapshot_too_old、
# test_bloomfilter、test_extensions、test_integerset、test_rbtree、test_shm_mq、
# unsafe_tests、worker_spi）的测试用例分别依赖已裁剪的
# 逻辑复制 / 扩展查询协议 / CREATE FUNCTION / 游标 / 角色权限 / serial / 分区
# 等特性，已彻底删除。
SUBDIRS = \
		  test_misc


$(recurse)
$(recurse_always)
