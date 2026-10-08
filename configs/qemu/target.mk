############################################################################
# pnut-os/configs/qemu/target.mk
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

# The ESP32-S3 in Espressif's QEMU, which has the esp32s3 machine (the
# QEMU packaged by distributions has not).

BOARD_CONFIG = esp32s3-devkit:qemu_debug
RUN          = qemu-system-xtensa -nographic -machine esp32s3 \
               -drive file=$(NUTTX)/nuttx.merged.bin,if=mtd,format=raw
