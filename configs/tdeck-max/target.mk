############################################################################
# pnut-os/configs/tdeck-max/target.mk
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

# The LilyGo T-Deck Max, flashed over its USB Serial/JTAG port.

BOARD_CONFIG  = lilygo-tdeck-max:full
ESPTOOL_PORT ?= /dev/ttyACM0

# MCUboot starts the firmware (RFC 0013): NuttX builds it beside the
# firmware, from MCUboot's and Espressif's sources (fetched once), and
# flashes it at the start of the flash, the firmware in the first slot,
# confirmed

AFTER_BUILD   = $(MAKE) -C $(NUTTX) bootloader
FLASH         = $(MAKE) -C $(NUTTX) flash ESPTOOL_PORT=$(ESPTOOL_PORT) \
                ESPTOOL_BINDIR=.
