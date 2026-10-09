############################################################################
# pnut-os/src/lib/Makefile
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

include $(APPDIR)/Make.defs

# libpnut, built into nuttx-apps' library with the programs

CSRCS  = client.c clock.c conn.c log.c loop.c module.c msg.c pool.c
CSRCS += service.c timer.c topic.c worker.c

# The interfaces' generated code (proto/pnut), once for every program that
# serves or calls them, which needs nanopb; the top-level Makefile
# generates it into build/gen.  The tests' interfaces, in pnut/test/, are
# the tests' own.

ifneq ($(CONFIG_NETUTILS_NANOPB),)
GENDIR  = $(realpath $(APPDIR)/external)/../build/gen

CSRCS  += $(wildcard $(GENDIR)/pnut/*.c)

CFLAGS += ${INCDIR_PREFIX}$(GENDIR)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/nanopb/nanopb
endif

include $(APPDIR)/Application.mk
