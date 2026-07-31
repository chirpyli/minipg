#-------------------------------------------------------------------------
#
# Makefile for src/interfaces
#
# Copyright (c) 1994, Regents of the University of California
#
# src/interfaces/Makefile
#
#-------------------------------------------------------------------------

subdir = src/interfaces
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

# 注：ecpg（嵌入式 SQL 预处理器）已在 minipg 裁剪中整体删除（阶段 2）。
# 它是客户端工具链，与 server 内核无关，对内核学习无价值。
SUBDIRS = libpq

$(recurse)
