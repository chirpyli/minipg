#-------------------------------------------------------------------------
#
# Makefile for src/test/subscription
#
# Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/subscription/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/subscription
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# 注：原 EXTRA_INSTALL = contrib/hstore 已移除。minipg 裁剪了 hstore 扩展，
# 002_types.pl 改用内核内置 jsonb 类型覆盖键值类型复制场景，不再依赖 hstore。

export with_icu

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

clean distclean maintainer-clean:
	rm -rf tmp_check
