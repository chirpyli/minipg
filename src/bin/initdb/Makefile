#-------------------------------------------------------------------------
#
# Makefile for src/bin/initdb
#
# Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/bin/initdb/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "initdb - initialize a new database cluster"
PGAPPICON=win32

subdir = src/bin/initdb
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -DFRONTEND -I$(libpq_srcdir) $(CPPFLAGS)

# Note: it's important that we link to encnames.o from libpgcommon, not
# from libpq, else we have risks of version skew if we run with a libpq
# shared library from a different PG version.  The libpq_pgport macro
# should ensure that that happens.
#
# We need libpq only because fe_utils does.
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

OBJS = \
	$(WIN32RES) \
	initdb.o

all: initdb

initdb: $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) initdb$(X) '$(DESTDIR)$(bindir)/initdb$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/initdb$(X)'

clean distclean maintainer-clean:
	rm -f initdb$(X) $(OBJS)
	rm -rf tmp_check

# ensure that changes in datadir propagate into object file
initdb.o: initdb.c $(top_builddir)/src/Makefile.global

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
