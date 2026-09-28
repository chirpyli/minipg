#
# Makefile for utils/adt
#
# src/backend/utils/adt/Makefile
#

subdir = src/backend/utils/adt
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

# keep this list arranged alphabetically or it gets to be a mess
OBJS = \
	array_expanded.o \
	array_selfuncs.o \
	array_userfuncs.o \
	arrayfuncs.o \
	arraysubs.o \
	arrayutils.o \
	ascii.o \
	bool.o \
	char.o \
	datum.o \
	dbsize.o \
	encode.o \
	expandeddatum.o \
	float.o \
	format_type.o \
	int.o \
	int8.o \
	lockfuncs.o \
	mcxtfuncs.o \
	misc.o \
	name.o \
	numutils.o \
	oid.o \
	oracle_compat.o \
	pg_locale.o \
	pg_lsn.o \
	pgstatfuncs.o \
	prefix_support.o \
	pseudotypes.o \
	quote.o \
	regproc.o \
	rowtypes.o \
	ruleutils.o \
	selfuncs.o \
	tid.o \
	varchar.o \
	varlena.o \
	version.o \
	xid.o \

clean distclean maintainer-clean:
	rm -f lex.backup

varlena.o: varlena.c levenshtein.c

include $(top_srcdir)/src/backend/common.mk
