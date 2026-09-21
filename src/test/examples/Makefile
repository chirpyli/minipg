#
# Makefile for example programs
#

subdir = src/test/examples
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(PORTNAME), win32)
LDFLAGS += -lws2_32
endif

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += $(libpq_pgport)


# minipg: testlibpq3 has been cropped.  Its whole point was to demonstrate
# PQexecParams() (out-of-line parameters + binary result format), and the
# backend only implements the simple query protocol.
PROGS = testlibpq testlibpq4

all: $(PROGS)

clean distclean maintainer-clean:
	rm -f $(PROGS) *.o
