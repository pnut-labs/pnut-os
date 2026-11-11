/****************************************************************************
 * pnut-os/src/system/firmware.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Firmware updates (RFC 0013): MCUboot starts a new firmware on trial, and
 * goes back to the previous one at the next start unless the running one
 * is confirmed.  The system program confirms it once every program is
 * ready, as Service states sees them; with no system UI yet, that is the
 * whole check.  MCUboot's library (nuttx-apps' boot/mcuboot) writes the
 * mark in the image's trailer, at the end of the first slot.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#ifdef __NuttX__
#  include <nuttx/config.h>
#endif

#include <errno.h>

#include <pnut/log.h>

#ifdef CONFIG_BOOT_MCUBOOT
#  include <bootutil/bootutil_public.h>
#  include <sysflash/sysflash.h>
#endif

#include "firmware.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: firmware_confirm
 ****************************************************************************/

int firmware_confirm(FAR struct pnut_module_s *module)
{
#ifdef CONFIG_BOOT_MCUBOOT
  struct boot_swap_state state;
  int ret;

  /* MCUboot's library returns its own codes, or -1 */

  ret = boot_read_swap_state_by_id(FLASH_AREA_IMAGE_PRIMARY(0), &state);
  if (ret != 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Firmware: cannot read its state: %d",
               ret);
      return -EIO;
    }

  /* A trailer that is not MCUboot's: nothing MCUboot would go back on */

  if (state.magic == BOOT_MAGIC_BAD)
    {
      pnut_log(module, PNUT_LOG_WARNING,
               "Firmware: its trailer is not MCUboot's");
      return 0;
    }

  /* Flashed over USB as confirmed, or confirmed at an earlier start; or
   * a mark MCUboot does not go back on either (not unset)
   */

  if (state.magic != BOOT_MAGIC_GOOD || state.image_ok != BOOT_FLAG_UNSET)
    {
      return 0;
    }

  ret = boot_set_confirmed_multi(0);
  if (ret != 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Firmware: cannot confirm it: %d",
               ret);
      return -EIO;
    }

  pnut_log(module, PNUT_LOG_INFO,
           "Firmware confirmed: every program has stayed ready");
#else
  (void)module;
#endif

  return 0;
}
