#-------------------------------------------------------------------------
#
# Makefile for src/bin/psql
#
# Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/bin/psql/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "psql - the PostgreSQL interactive terminal"
PGAPPICON=win32

subdir = src/bin/psql
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# make this available to TAP test scripts
export with_readline


override CPPFLAGS := -I. -I$(srcdir) -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

OBJS = \
	$(WIN32RES) \
	command.o \
	common.o \
	crosstabview.o \
	describe.o \
	help.o \
	input.o \
	mainloop.o \
	prompt.o \
	psqlscanslash.o \
	startup.o \
	stringutils.o \
	variables.o


all: psql

psql: $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)



psqlscanslash.c: FLEXFLAGS = -Cfe -p -p
psqlscanslash.c: FLEX_NO_BACKUP=yes
psqlscanslash.c: FLEX_FIX_WARNING=yes

distprep: psqlscanslash.c

install: all installdirs
	$(INSTALL_PROGRAM) psql$(X) '$(DESTDIR)$(bindir)/psql$(X)'
	$(INSTALL_DATA) $(srcdir)/psqlrc.sample '$(DESTDIR)$(datadir)/psqlrc.sample'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)' '$(DESTDIR)$(datadir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/psql$(X)' '$(DESTDIR)$(datadir)/psqlrc.sample'

clean distclean:
	rm -f psql$(X) $(OBJS) lex.backup
	rm -rf tmp_check

# files removed here are supposed to be in the distribution tarball,
# so do not clean them in the clean/distclean rules
maintainer-clean: distclean
	rm -f psqlscanslash.c

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
