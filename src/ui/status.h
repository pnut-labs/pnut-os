/****************************************************************************
 * pnut-os/src/ui/status.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_STATUS_H
#define __PNUT_OS_UI_STATUS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <pnut/compiler.h>
#include <pnut/loop.h>
#include <pnut/timer.h>
#include <pnut/tz.h>

#include "pnut/power.topics.h"
#include "pnut/telephony.topics.h"
#include "pnut/time.topics.h"
#include "pnut/wifi.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* What changed, told to the screens */

#define STATUS_TEL      0x01      /* Telephony's topic */
#define STATUS_WIFI     0x02      /* Wi-Fi's */
#define STATUS_TIME     0x04      /* The zone, or a minute began */
#define STATUS_BATTERY  0x08      /* The battery's */

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef CODE void (*status_changed_t)(FAR void *arg, uint8_t what);

/* The device's state the header shows, from the topics: each part unknown
 * (has_ false) until its topic's first message
 */

struct ui_status_s
{
  bool has_tel;
  bool has_wifi;
  bool has_time;                  /* The zone is known */
  bool has_battery;
  pnut_telephony_info_t tel;
  pnut_wifi_info_t wifi;
  struct pnut_tz_s tz;
  uint8_t battery;                /* Percent */
  bool charging;

  /* status.c's */

  FAR struct pnut_loop_s *loop;
  CODE time_t (*now)(void);
  status_changed_t changed;
  FAR void *arg;
  struct pnut_telephony_info_reader_s tel_reader;
  struct pnut_wifi_info_reader_s wifi_reader;
  struct pnut_time_change_reader_s time_reader;
  struct pnut_battery_info_reader_s battery_reader;
  FAR struct pnut_timer_s *minute;
  bool tel_open;
  bool wifi_open;
  bool time_open;
  bool battery_open;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Read the topics on the loop; changed is told what changed, and when a
 * minute begins.  now: the wall clock, NULL for time().
 */

int status_start(FAR struct ui_status_s *st, FAR struct pnut_loop_s *loop,
                 CODE time_t (*now)(void), status_changed_t changed,
                 FAR void *arg);
void status_stop(FAR struct ui_status_s *st);

/* The wall clock, and a time as local time in the zone (UTC until the
 * zone is known)
 */

time_t status_now(FAR const struct ui_status_s *st);
void status_local(FAR const struct ui_status_s *st, time_t t,
                  FAR struct tm *tm);

#endif /* __PNUT_OS_UI_STATUS_H */
