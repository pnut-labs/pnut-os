############################################################################
# pnut-os/src/system/Makefile
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

include $(APPDIR)/Make.defs

# The system program (RFC 0007)

PROGNAME  = pnut_system
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = $(CONFIG_PNUT_SYSTEM_STACKSIZE)
MODULE    = $(CONFIG_PNUT_SYSTEM)

MAINSRC   = main.c
CSRCS     = settings.c settings_store.c

# The interfaces' code, which libpnut compiles

GENDIR    = $(realpath $(APPDIR)/external)/../build/gen

CFLAGS   += ${INCDIR_PREFIX}$(GENDIR)
CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/netutils/nanopb/nanopb

include $(APPDIR)/Application.mk
