#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/replication
#
# IDENTIFICATION
#    src/backend/replication/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/replication
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

OBJS =

include $(top_srcdir)/src/backend/common.mk

# repl_gram.c, repl_scanner.c, syncrep_gram.c and syncrep_scanner.c
# are in the distribution tarball, so they are not cleaned here.
# (Our parent Makefile takes care of them during maintainer-clean.)
