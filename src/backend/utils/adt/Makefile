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
	acl.o \
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
	cryptohashfuncs.o \
	date.o \
	datetime.o \
	datum.o \
	dbsize.o \
	domains.o \
	encode.o \
	enum.o \
	expandeddatum.o \
	expandedrecord.o \
	float.o \
	format_type.o \
	formatting.o \
	genfile.o \
	int.o \
	ipaddr.o \
	int8.o \
	json_escape.o \
	like.o \
	like_support.o \
	lockfuncs.o \
	mac.o \
	mac8.o \
	mcxtfuncs.o \
	misc.o \
	multirangetypes.o \
	multirangetypes_selfuncs.o \
	name.o \
	numeric.o \
	numutils.o \
	oid.o \
	oracle_compat.o \
	orderedsetaggs.o \
	partitionfuncs.o \
	pg_locale.o \
	pg_lsn.o \
	pg_upgrade_support.o \
	pgstatfuncs.o \
	pseudotypes.o \
	quote.o \
	rangetypes.o \
	rangetypes_selfuncs.o \
	rangetypes_typanalyze.o \
	regexp.o \
	regproc.o \
	ri_triggers.o \
	rowtypes.o \
	ruleutils.o \
	selfuncs_geo.o \
	selfuncs.o \
	tid.o \
	timestamp.o \
	trigfuncs.o \
	uuid.o \
	varbit.o \
	varchar.o \
	varlena.o \
	version.o \
	windowfuncs.o \
	xid.o \
	xid8funcs.o \
	xml.o

clean distclean maintainer-clean:
	rm -f lex.backup

like.o: like.c like_match.c

# Some code in numeric.c benefits from auto-vectorization
numeric.o: CFLAGS += ${CFLAGS_VECTORIZE}

varlena.o: varlena.c levenshtein.c

include $(top_srcdir)/src/backend/common.mk
