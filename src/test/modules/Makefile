# src/test/modules/Makefile

subdir = src/test/modules
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = \
		  delay_execution \
		  dummy_index_am \
		  libpq_pipeline \
		  snapshot_too_old \
		  test_bloomfilter \
		  test_custom_types \
		  test_extensions \
		  test_integerset \
		  test_misc \
		  test_predtest \
		  test_rbtree \
		  test_shm_mq

# minipg: worker_spi 的回归测试依赖 plpgsql DO 块轮询等待 bgworker 初始化，
# PL/pgSQL 已裁剪，移除该模块的回归测试。

$(recurse)
$(recurse_always)
