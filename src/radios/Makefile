############################################################################
# pnut-os/src/radios/Makefile
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = pnut_radios
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = $(CONFIG_PNUT_RADIOS_STACKSIZE)
MODULE    = $(CONFIG_PNUT_RADIOS)

MAINSRC   = main.c
CSRCS     = device.c packetradio.c rules.c

GENDIR    = $(realpath $(APPDIR)/external)/../build/gen
CFLAGS   += ${INCDIR_PREFIX}$(GENDIR)
CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/netutils/nanopb/nanopb

include $(APPDIR)/Application.mk
