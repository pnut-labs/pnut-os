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
CSRCS     = board.c firmware.c logs.c power.c programs.c settings.c
CSRCS    += settings_store.c
CSRCS    += states.c
CSRCS    += wallclock.c

ifeq ($(CONFIG_PNUT_TIME_NTP),y)
CSRCS    += ntp.c
endif

# The interfaces' code, which libpnut compiles

GENDIR    = $(realpath $(APPDIR)/external)/../build/gen

CFLAGS   += ${INCDIR_PREFIX}$(GENDIR)
CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/netutils/nanopb/nanopb

# libsodium, for Settings' secrets (RFC 0031): nuttx-apps' package

CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/crypto/libsodium/libsodium/src/libsodium/include

include $(APPDIR)/Application.mk
