#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for the timezone library

# IDENTIFICATION
#    src/timezone/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "timezone - timezone library"

subdir = src/timezone
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

# files to build into backend
OBJS = \
	localtime.o \
	pgtz.o \
	strftime.o

include $(top_srcdir)/src/backend/common.mk

install: all
	$(MAKE) -C tznames $@

uninstall:
	$(MAKE) -C tznames $@
