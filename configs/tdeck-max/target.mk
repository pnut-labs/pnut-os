############################################################################
# pnut-os/configs/tdeck-max/target.mk
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

# The LilyGo T-Deck Max, flashed over its USB Serial/JTAG port.

BOARD_CONFIG  = lilygo-tdeck-max:nsh
ESPTOOL_PORT ?= /dev/ttyACM0
FLASH         = $(MAKE) -C $(NUTTX) flash ESPTOOL_PORT=$(ESPTOOL_PORT)
