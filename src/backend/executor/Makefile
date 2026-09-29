#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for executor
#
# IDENTIFICATION
#    src/backend/executor/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/executor
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	execAmi.o \
	execExpr.o \
	execExprInterp.o \
	execGrouping.o \
	execIndexing.o \
	execJunk.o \
	execMain.o \
	execProcnode.o \
	execSRF.o \
	execScan.o \
	execTuples.o \
	execUtils.o \
	functions.o \
	nodeBitmapAnd.o \
	nodeBitmapHeapscan.o \
	nodeBitmapIndexscan.o \
	nodeBitmapOr.o \
	nodeHash.o \
	nodeHashjoin.o \
	nodeIndexonlyscan.o \
	nodeIndexscan.o \
	nodeMaterial.o \
	nodeMergejoin.o \
	nodeModifyTable.o \
	nodeNestloop.o \
	nodeProjectSet.o \
	nodeResult.o \
	nodeSeqscan.o \
	nodeSort.o \
	nodeSubplan.o \
	nodeSubqueryscan.o \
	nodeTidrangescan.o \
	nodeTidscan.o \
	nodeUnique.o \
	nodeValuesscan.o \
	tstoreReceiver.o

include $(top_srcdir)/src/backend/common.mk
