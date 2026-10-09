############################################################################
# pnut-os/src/tests/Makefile
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

include $(APPDIR)/Make.defs

# The unit tests of tests/unit, each file a program.  apps/external links
# to src/, so tests/ is beside it.

UNITDIR = $(realpath $(APPDIR)/external)/../tests/unit

PROGNAME  = pnut_test_conn pnut_test_ipc pnut_test_loop pnut_test_msg
PROGNAME += pnut_test_pool pnut_test_timer pnut_test_worker
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 16384
MODULE    = $(CONFIG_PNUT_TESTS)

MAINSRC   = $(UNITDIR)/test_conn.c $(UNITDIR)/test_ipc.c
MAINSRC  += $(UNITDIR)/test_loop.c $(UNITDIR)/test_msg.c
MAINSRC  += $(UNITDIR)/test_pool.c $(UNITDIR)/test_timer.c
MAINSRC  += $(UNITDIR)/test_worker.c

CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/external/lib

include $(APPDIR)/Application.mk
