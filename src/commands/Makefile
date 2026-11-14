############################################################################
# pnut-os/src/commands/Makefile
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

include $(APPDIR)/Make.defs

# The shell's commands (RFC 0012)

PROGNAME  = settings telephony messages logs wifi haptics
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = $(CONFIG_PNUT_COMMANDS_STACKSIZE)
MODULE    = $(CONFIG_PNUT_COMMANDS)

MAINSRC   = settings.c telephony.c messages.c logs.c wifi.c haptics.c

# The computers adb lets in, while adbd keeps the keys offered (RFC 0012)

ifneq ($(CONFIG_ADBD_AUTH_PUBKEY),)
PROGNAME += adbkeys
MAINSRC  += adbkeys.c
endif

# Where the device is, with the sensors program (RFC 0035)

ifneq ($(CONFIG_PNUT_SENSORS),)
PROGNAME += location
MAINSRC  += location.c
endif

# The interfaces' code, which libpnut compiles

GENDIR    = $(realpath $(APPDIR)/external)/../build/gen

CFLAGS   += ${INCDIR_PREFIX}$(GENDIR)
CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/netutils/nanopb/nanopb

include $(APPDIR)/Application.mk
