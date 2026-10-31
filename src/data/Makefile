############################################################################
# pnut-os/src/data/Makefile
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

include $(APPDIR)/Make.defs

# The data program, pnut_data (RFC 0007, RFC 0029)

PROGNAME  = pnut_data
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = $(CONFIG_PNUT_DATA_STACKSIZE)
MODULE    = $(CONFIG_PNUT_DATA)

MAINSRC   = main.c
CSRCS     = messages.c store.c

GENDIR    = $(realpath $(APPDIR)/external)/../build/gen
CFLAGS   += ${INCDIR_PREFIX}$(GENDIR)
CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/netutils/nanopb/nanopb

include $(APPDIR)/Application.mk
