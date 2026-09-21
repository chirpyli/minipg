#-------------------------------------------------------------------------
#
# GNUmakefile--
#    Makefile for src/test/regress (the regression tests)
#
# Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/regress/GNUmakefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_regress - test driver"
PGAPPICON = win32

subdir = src/test/regress
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# maximum simultaneous connections for parallel tests
MAXCONNOPT =
ifdef MAX_CONNECTIONS
MAXCONNOPT += --max-connections=$(MAX_CONNECTIONS)
endif

# stuff to pass into build of pg_regress
EXTRADEFS = '-DHOST_TUPLE="$(host_tuple)"' \
	'-DSHELLPROG="$(SHELL)"' \
	'-DDLSUFFIX="$(DLSUFFIX)"'

##
## Prepare for tests
##

# Build regression test driver

all: pg_regress$(X)

pg_regress$(X): pg_regress.o pg_regress_main.o $(WIN32RES) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@

# dependencies ensure that path changes propagate
pg_regress.o: pg_regress.c $(top_builddir)/src/port/pg_config_paths.h
pg_regress.o: override CPPFLAGS += -I$(top_builddir)/src/port $(EXTRADEFS)

# note: because of the submake dependency, this rule's action is really a no-op
$(top_builddir)/src/port/pg_config_paths.h: | submake-libpgport
	$(MAKE) -C $(top_builddir)/src/port pg_config_paths.h

install: all installdirs
	$(INSTALL_PROGRAM) pg_regress$(X) '$(DESTDIR)$(pgxsdir)/$(subdir)/pg_regress$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(pgxsdir)/$(subdir)'

uninstall:
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/pg_regress$(X)'


# Build dynamically-loaded object file for CREATE FUNCTION ... LANGUAGE C.

NAME = regress
OBJS = $(WIN32RES) regress.o

include $(top_srcdir)/src/Makefile.shlib

all: all-lib

# Ensure parallel safety if a build is started in this directory
$(OBJS): | submake-libpgport submake-generated-headers

# Test input and expected files.  These are created by pg_regress itself, so we
# don't have a rule to create them.  We do need rules to clean them however.
input_files = $(patsubst $(srcdir)/input/%.source,sql/%.sql, $(wildcard $(srcdir)/input/*.source))
output_files := $(patsubst $(srcdir)/output/%.source,expected/%.out, $(wildcard $(srcdir)/output/*.source))


# not installed by default

regress_data_files = \
	$(filter-out $(addprefix $(srcdir)/,$(output_files)),$(wildcard $(srcdir)/expected/*.out)) \
	$(wildcard $(srcdir)/input/*.source) \
	$(wildcard $(srcdir)/output/*.source) \
	$(filter-out $(addprefix $(srcdir)/,$(input_files)),$(wildcard $(srcdir)/sql/*.sql)) \
	$(wildcard $(srcdir)/data/*.data) \
	$(srcdir)/parallel_schedule $(srcdir)/resultmap

install-tests: all install install-lib installdirs-tests
	for file in $(subst $(srcdir)/,,$(regress_data_files)); do \
		$(INSTALL_DATA) $(srcdir)/$$file '$(DESTDIR)$(pkglibdir)/regress/'$$file || exit; \
	done

installdirs-tests: installdirs
	$(MKDIR_P)  $(patsubst $(srcdir)/%/,'$(DESTDIR)$(pkglibdir)/regress/%',$(sort $(dir $(regress_data_files))))


# Get some extra C modules from contrib/spi



## Run tests
##

REGRESS_OPTS = --dlpath=. --max-concurrent-tests=20 \
	--no-locale \
	$(EXTRA_REGRESS_OPTS)

check: all
	$(pg_regress_check) $(REGRESS_OPTS) --schedule=$(srcdir)/parallel_schedule $(MAXCONNOPT) $(EXTRA_TESTS)

check-tests: all | temp-install
	$(pg_regress_check) $(REGRESS_OPTS) $(MAXCONNOPT) $(TESTS) $(EXTRA_TESTS)

installcheck: all
	$(pg_regress_installcheck) $(REGRESS_OPTS) --schedule=$(srcdir)/parallel_schedule --max-connections=1 $(EXTRA_TESTS)

installcheck-parallel: all
	$(pg_regress_installcheck) $(REGRESS_OPTS) --schedule=$(srcdir)/parallel_schedule $(MAXCONNOPT) $(EXTRA_TESTS)

installcheck-tests: all
	$(pg_regress_installcheck) $(REGRESS_OPTS) $(TESTS) $(EXTRA_TESTS)

# minipg: standbycheck 目标已随 Hot Standby 测试用例一起移除。
# 原用例（hs_standby_check/allowed/disallowed）依赖 hs_primary_setup.sql 在
# 主库上预置数据、并要求由主库向备库持续传送 WAL 文件，而该 setup 脚本与
# 驱动它的 src/test/recovery 目录均已裁剪；用例本身还使用了已裁剪的
# GRANT/REVOKE。故连同 standby_schedule 一并删除。

# old interfaces follow...

runcheck: check
runtest: installcheck
runtest-parallel: installcheck-parallel

# minipg: bigtest/bigcheck 目标已移除，其唯一的测试用例 numeric_big 随
# numeric 数据类型一起被裁剪（无 sql/numeric_big.sql 与 expected/numeric_big.out）。


##
## Clean up
##

clean distclean maintainer-clean: clean-lib
# things built by `all' target
	rm -f $(OBJS)
	rm -f pg_regress_main.o pg_regress.o pg_regress$(X)
# things created by various check targets
	rm -f $(output_files) $(input_files)
	rm -rf $(pg_regress_clean_files)
