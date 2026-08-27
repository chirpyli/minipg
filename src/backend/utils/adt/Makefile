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
	amutils.o \
	array_expanded.o \
	array_selfuncs.o \
	array_typanalyze.o \
	array_userfuncs.o \
	arrayfuncs.o \
	arraysubs.o \
	arrayutils.o \
	ascii.o \
	bool.o \
	char.o \
	date.o \
	datetime.o \
	datum.o \
	dbsize.o \
	domains.o \
	encode.o \
	expandeddatum.o \
	expandedrecord.o \
	float.o \
	format_type.o \
	formatting.o \
	genfile.o \
	int.o \
	int8.o \
	json_escape.o \
	like.o \
	like_support.o \
	lockfuncs.o \
	mcxtfuncs.o \
	misc.o \
	name.o \
	intagg.o \
	numutils.o \
	oid.o \
	oracle_compat.o \
	orderedsetaggs.o \
	multirangetypes.o \
	multirangetypes_selfuncs.o \
	pg_locale.o \
	pg_lsn.o \
	pgstatfuncs.o \
	pseudotypes.o \
	quote.o \
	regproc.o \
	rowtypes.o \
	ruleutils.o \
	rangetypes.o \
	rangetypes_selfuncs.o \
	rangetypes_typanalyze.o \
	selfuncs_geo.o \
	selfuncs.o \
	tid.o \
	timestamp.o \
	varbit.o \
	varchar.o \
	varlena.o \
	version.o \
	xid.o \
	xid8funcs.o

clean distclean maintainer-clean:
	rm -f lex.backup

like.o: like.c like_match.c

varlena.o: varlena.c levenshtein.c

include $(top_srcdir)/src/backend/common.mk
