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
