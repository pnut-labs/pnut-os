/****************************************************************************
 * pnut-os/src/system/power.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_SYSTEM_POWER_H
#define __PNUT_OS_SYSTEM_POWER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/compiler.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "pnut/board.pnut.h"
#include "pnut/time.topics.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Power (RFC 0007), so far its guard of the battery and the sleep clock's
 * rate: a module of the system program that reads the battery's voltage
 * every interval and, on battery power, once reads in a row have been at
 * or under the cutoff, stops the program to power the device off.  The
 * settings are written as the program stops; the program's main powers
 * off after.  It hands the kernel the sleep clock's rate that Time
 * announces (RFC 0028).
 */

struct power_config_s
{
  uint32_t interval;              /* Between readings, in milliseconds */
  int cutoff;                     /* In mV; zero never powers off */
  uint8_t reads;                  /* In a row, at or under the cutoff */

  /* Board's service: the cutoff is then the battery's power-off voltage,
   * from the device's description (RFC 0024), and none without one.
   * NULL: the cutoff as given.
   */

  FAR const char *board;

  /* Read the battery's voltage, in mV, and whether the device runs on
   * external power.  A negated errno value; -ENODEV when the device has no
   * battery to read, which ends the readings.
   */

  CODE int (*measure)(FAR void *arg, FAR int *mv, FAR bool *external);
  FAR void *arg;

  /* Hand the kernel the sleep clock's rate, parts per billion of the time
   * slept that it gains: a negated errno value on failure.  NULL where
   * the kernel takes none.
   */

  CODE int (*sleep_rate)(FAR void *arg, int32_t ppb);
};

struct power_s
{
  struct pnut_module_s module;    /* First: the module is the power */
  struct power_config_s config;

  struct pnut_board_client_s board;
  bool board_open;                /* Asking Board for the description */
  FAR struct pnut_timer_s *timer; /* The readings, periodic */
  struct pnut_sleep_rate_reader_s rates;
  bool rates_open;
  uint8_t low;                    /* Readings in a row under the cutoff */
  bool failing;                   /* The last reading failed, logged */
  bool off;                       /* The program stopped to power off */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: power_init
 *
 * Description:
 *   Set up the module, to be added to the program's loop.
 *
 ****************************************************************************/

void power_init(FAR struct power_s *power,
                FAR const struct power_config_s *config);

/****************************************************************************
 * Name: power_wants_off
 *
 * Description:
 *   Whether the module stopped the program to power the device off.
 *
 ****************************************************************************/

bool power_wants_off(FAR const struct power_s *power);

#endif /* __PNUT_OS_SYSTEM_POWER_H */
