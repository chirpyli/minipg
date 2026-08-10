# src/test/modules/delay_execution/Makefile

PGFILEDESC = "delay_execution - allow delay between parsing and execution"

MODULE_big = delay_execution
OBJS = \
	$(WIN32RES) \
	delay_execution.o

# ISOLATION 用例 partition-addition / partition-removal-1 已随分区功能裁剪而移除
ISOLATION =

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/delay_execution
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
