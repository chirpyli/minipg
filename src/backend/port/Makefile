#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the port-specific subsystem of the backend
#
# We have two different modes of operation: 1) put stuff specific to Port X
# in subdirectory X and have that subdirectory's make file make it all, and
# 2) use conditional statements in the present make file to include what's
# necessary for a specific port in our own output.  (1) came first, but (2)
# is superior for many things, like when the same thing needs to be done for
# multiple ports and you don't want to duplicate files in multiple
# subdirectories.  Much of the stuff done via Method 1 today should probably
# be converted to Method 2.
#
# IDENTIFICATION
#    src/backend/port/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/port
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	atomics.o \
	pg_sema.o \
	pg_shmem.o

# 注：minipg 仅支持 Linux。原 $(TAS)（HPPA/Sun Studio 汇编）、tas.o 构建规则、
# SUN_STUDIO_CC 分支、aix/hpux/tas 子目录、win32 子目录均已删除。

include $(top_srcdir)/src/backend/common.mk
