############################################################################
# pnut-os/configs/sim/target.mk
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

# NuttX's simulator, run as a program on the computer.

BOARD_CONFIG = sim:nsh
RUN          = $(NUTTX)/nuttx

# libpnut's unit tests, built as programs (src/tests), run in the simulator

TEST         = $(PYTHON) $(ROOT)/tools/sim-test.py $(NUTTX)/nuttx \
               pnut_test_conn pnut_test_ipc pnut_test_loop \
               pnut_test_msg pnut_test_pool pnut_test_timer \
               pnut_test_worker
