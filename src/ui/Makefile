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
CSRCS     = apps.c blocks.c describe.c display.c fb.c glue.c input.c
CSRCS    += input_dev.c keymap.c page.c redraw.c status.c theme.c ui.c
CSRCS    += list.c setting.c wifilist.c
CSRCS    += app.c host.c manifest.c
CSRCS    += scr_cell.c scr_home.c scr_join.c scr_menu.c scr_settings.c
CSRCS    += scr_pin.c scr_unavailable.c
CSRCS    += scr_wifi.c

# The interfaces' code, and the fonts and icons, which the top-level
# Makefile makes in build/gen and build/ui

BUILDDIR  = $(realpath $(APPDIR)/external)/../build
CSRCS    += $(wildcard $(BUILDDIR)/ui/*.c)

CFLAGS   += ${INCDIR_PREFIX}$(BUILDDIR)/gen
CFLAGS   += ${INCDIR_PREFIX}$(BUILDDIR)/ui
CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/netutils/nanopb/nanopb

include $(APPDIR)/Application.mk
