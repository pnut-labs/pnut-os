############################################################################
# pnut-os/src/ui/Makefile
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = pnut_ui
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = $(CONFIG_PNUT_UI_STACKSIZE)
MODULE    = $(CONFIG_PNUT_UI)

MAINSRC   = main.c
CSRCS     = display.c fb.c glue.c input.c input_dev.c keymap.c redraw.c
CSRCS    += ui.c

# The interfaces' code, and the fonts and icons, which the top-level
# Makefile makes in build/gen and build/ui

BUILDDIR  = $(realpath $(APPDIR)/external)/../build
CSRCS    += $(wildcard $(BUILDDIR)/ui/*.c)

CFLAGS   += ${INCDIR_PREFIX}$(BUILDDIR)/gen
CFLAGS   += ${INCDIR_PREFIX}$(BUILDDIR)/ui
CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/netutils/nanopb/nanopb

include $(APPDIR)/Application.mk
