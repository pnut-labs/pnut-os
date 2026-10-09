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
CSRCS += service.c timer.c worker.c

# The messages every interface shares (proto/pnut), once for all of the
# interfaces' generated code, which needs nanopb; the top-level Makefile
# generates them into build/gen

ifneq ($(CONFIG_NETUTILS_NANOPB),)
GENDIR  = $(realpath $(APPDIR)/external)/../build/gen

CSRCS  += $(GENDIR)/pnut/error.pb.c

CFLAGS += ${INCDIR_PREFIX}$(GENDIR)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/nanopb/nanopb
endif

include $(APPDIR)/Application.mk
