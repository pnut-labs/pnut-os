/****************************************************************************
 * pnut-os/src/system/firmware.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_SYSTEM_FIRMWARE_H
#define __PNUT_OS_SYSTEM_FIRMWARE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <pnut/module.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: firmware_confirm
 *
 * Description:
 *   Confirm the running firmware to MCUboot (RFC 0013), if it runs on
 *   trial: MCUboot then keeps it, instead of going back to the previous
 *   one at the next start.  Nothing without MCUboot (CONFIG_BOOT_MCUBOOT),
 *   or when it is confirmed already.  Logged as module's.
 *
 * Returned Value:
 *   Zero (OK) when the firmware is confirmed, or has nothing to confirm;
 *   -EIO when MCUboot's library cannot read or write the image's trailer.
 *
 ****************************************************************************/

int firmware_confirm(FAR struct pnut_module_s *module);

#endif /* __PNUT_OS_SYSTEM_FIRMWARE_H */
