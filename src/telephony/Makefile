############################################################################
# pnut-os/src/telephony/Makefile
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

include $(APPDIR)/Make.defs

# The telephony program, pnut_telephony (RFC 0007, RFC 0030)

PROGNAME  = pnut_telephony
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = $(CONFIG_PNUT_TELEPHONY_STACKSIZE)
MODULE    = $(CONFIG_PNUT_TELEPHONY)

MAINSRC   = main.c
CSRCS     = at.c mux.c parse.c telephony.c

GENDIR    = $(realpath $(APPDIR)/external)/../build/gen
CFLAGS   += ${INCDIR_PREFIX}$(GENDIR)
CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/netutils/nanopb/nanopb

include $(APPDIR)/Application.mk
