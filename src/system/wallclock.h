/****************************************************************************
 * pnut-os/src/system/wallclock.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_SYSTEM_WALLCLOCK_H
#define __PNUT_OS_SYSTEM_WALLCLOCK_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include <pnut/module.h>
#include <pnut/topic.h>
#include <pnut/tz.h>

#include "pnut/board.pnut.h"
#include "pnut/time.pnut.h"
#include "pnut/time.settings.h"
#include "pnut/time.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Time's socket, in the run directory */

#define WALLCLOCK_SERVICE  "time"

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Who calls (RFC 0006): a task's program, or -EAGAIN when it cannot be
 * told now; as Settings' (settings.h)
 */

typedef CODE int (*wallclock_who_t)(FAR void *arg, pid_t pid,
                                    FAR const char **programp);

/* Time (RFC 0028), so far the clock and the zone: a module of the system
 * program that takes the time from the readings sources publish
 * (network_time, gnss_time) and from the owner (Set), keeps the zone from
 * its settings and the network's offset, and announces both on the time
 * topic.  It keeps the last good time in its state file, written with
 * each change of the clock or its source, at most hourly otherwise, and
 * as it stops, so that the clock starts near the right time after a
 * power loss.  Once the clock is set from a source, a reading near it
 * gives way to a more preferred source's of the last hour (RFC 0028), and
 * does not move a clock within its own uncertainty.  The sleep clock's
 * rate is the device's description's, announced on the sleep_rate topic
 * for Power; NTP, and learning the unit's own rate from the readings,
 * come later.
 */

struct wallclock_config_s
{
  FAR const char *state;          /* The state file, or NULL */

  /* How far, in seconds, a reading may be from the clock to be taken
   * alone, once the clock has been set from a source; one further is held
   * until a reading from another source agrees with it
   */

  uint32_t tolerance;

  /* Board's service, whose description gives the sleep clock's rate
   * until Time learns the unit's own (RFC 0028); NULL: none
   */

  FAR const char *board;

  /* Who calls; without it, every caller may set the time */

  wallclock_who_t who;
  FAR void *identity;

  /* The clocks, CLOCK_REALTIME and CLOCK_MONOTONIC: the kernel's when
   * NULL (the tests give their own)
   */

  CODE int (*gettime)(FAR void *arg, clockid_t id,
                      FAR struct timespec *ts);
  CODE int (*settime)(FAR void *arg, FAR const struct timespec *ts);
  FAR void *clock;
};

/* A reading held, until another source agrees with it */

struct wallclock_held_s
{
  int64_t diff;                   /* Its time less the clock's, seconds */
  uint8_t source;                 /* pnut_time_info_source_t; 0 none */
};

struct wallclock_s
{
  struct pnut_module_s module;    /* First: the module is the clock */
  struct wallclock_config_s config;

  struct pnut_time_settings_s settings;
  struct pnut_time_server_s server;
  FAR struct pnut_publisher_s *changes;
  struct pnut_network_time_reader_s network;
  struct pnut_gnss_time_reader_s gnss;

  /* The sleep clock's rate, asked of Board once, and announced */

  FAR struct pnut_publisher_s *rates;
  struct pnut_board_client_s board;
  bool board_open;

  /* The zone in use: the named one's rule, or the network's offset */

  struct pnut_tz_s tz;
  char zone[sizeof(((FAR pnut_time_info_t *)0)->zone)];
  char named[sizeof(((FAR pnut_time_info_t *)0)->zone)]; /* The setting's */
  char rule[sizeof(((FAR pnut_time_info_t *)0)->rule)];
  bool from_network;

  /* The network's offset, summer time in, once it has given one */

  bool has_offset;
  bool net_dst;
  int32_t net_offset;

  /* Where the clock's time came from, and when */

  uint8_t source;                 /* pnut_time_info_source_t */
  int64_t since;
  struct wallclock_held_s held;

  uint32_t seq;                   /* The last change's */
  uint64_t saved_at;              /* The state file's last write, ms */
  bool saved;
  bool serving;
  bool settings_open;
  bool network_open;
  bool gnss_open;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: wallclock_init
 *
 * Description:
 *   Set up the module, to be added to the program's loop.
 *
 ****************************************************************************/

void wallclock_init(FAR struct wallclock_s *wall,
                    FAR const struct wallclock_config_s *config);

#endif /* __PNUT_OS_SYSTEM_WALLCLOCK_H */
