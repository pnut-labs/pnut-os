############################################################################
# pnut-os/src/sensors/Makefile
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = pnut_sensors
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = $(CONFIG_PNUT_SENSORS_STACKSIZE)
MODULE    = $(CONFIG_PNUT_SENSORS)

MAINSRC   = main.c
CSRCS     = location.c receiver.c ubx.c

GENDIR    = $(realpath $(APPDIR)/external)/../build/gen
CFLAGS   += ${INCDIR_PREFIX}$(GENDIR)
CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/netutils/nanopb/nanopb

include $(APPDIR)/Application.mk
