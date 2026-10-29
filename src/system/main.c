/****************************************************************************
 * pnut-os/src/system/main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The system program (RFC 0007): the services every other program waits
 * on.  Board comes first, so that the others can ask it for the device's
 * description; then Service states, since the others ask it who calls;
 * then Settings, then Time, which keeps its settings there, then Log,
 * once the clock is near the right time for its marks, then Power, which
 * may stop the program to power the device off: Settings, Time and Log,
 * stopping, write what they have not written yet.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/boardctl.h>
#include <sys/ioctl.h>

#include <nuttx/power/battery_ioctl.h>

#include <pnut/loop.h>
#include <pnut/module.h>

#include "board.h"
#include "logs.h"
#include "power.h"
#include "settings.h"
#include "states.h"
#include "wallclock.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* The battery's devices, for Power: its gauge, and the charger, which
 * tells whether there is external power
 */

struct system_battery_s
{
  int gauge;
  int charger;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Settings asks Service states who calls (RFC 0006) */

static int system_who(FAR void *arg, pid_t pid,
                      FAR const char **programp)
{
  return states_who(arg, pid, programp);
}

static bool system_runs(FAR void *arg, FAR const char *program,
                        FAR const char *service)
{
  return states_runs(arg, program, service);
}

/* Power reads the battery through the gauge's and the charger's devices.
 * Without a charger, a gauge that sees the battery charge, or full, tells
 * of external power.
 */

static int system_measure(FAR void *arg, FAR int *mv, FAR bool *external)
{
  FAR struct system_battery_s *battery = arg;
  int state;

  if (battery->gauge < 0)
    {
      return -ENODEV;
    }

  if (ioctl(battery->gauge, BATIOC_VOLTAGE, (unsigned long)mv) < 0)
    {
      return -errno;
    }

  if (battery->charger >= 0)
    {
      if (ioctl(battery->charger, BATIOC_ONLINE,
                (unsigned long)external) < 0)
        {
          return -errno;
        }
    }
  else
    {
      if (ioctl(battery->gauge, BATIOC_STATE, (unsigned long)&state) < 0)
        {
          return -errno;
        }

      *external = state == BATTERY_CHARGING || state == BATTERY_FULL;
    }

  return OK;
}

/* Called once the program has stopped for Power: the settings are written
 * by now.  Returns only if the device could not be powered off, when the
 * program exits and NxInit starts it again.
 */

#ifdef CONFIG_BOARDCTL_POWEROFF
static void system_power_off(void)
{
  int ret;

  sync();

  ret = boardctl(BOARDIOC_POWEROFF, 0);
  syslog(LOG_ERR, "cannot power off: %d\n",
         ret < 0 ? -errno : ret);
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  struct board_config_s bconfig;
  struct settings_config_s sconfig;
  struct states_config_s tconfig;
  struct power_config_s pconfig;
  struct wallclock_config_s wconfig;
  struct logs_config_s lconfig;
  struct system_battery_s battery;
  FAR struct settings_s *settings;
  FAR struct board_s *board;
  FAR struct states_s *states;
  FAR struct power_s *power;
  FAR struct wallclock_s *wall;
  FAR struct logs_s *logs;
  FAR struct pnut_loop_s *loop;
  bool off;
  int ret;

  settings = calloc(1, sizeof(*settings));
  board    = calloc(1, sizeof(*board));
  states   = calloc(1, sizeof(*states));
  power    = calloc(1, sizeof(*power));
  wall     = calloc(1, sizeof(*wall));
  logs     = calloc(1, sizeof(*logs));
  if (settings == NULL || board == NULL || states == NULL || power == NULL ||
      wall == NULL || logs == NULL)
    {
      fprintf(stderr, "pnut_system: no memory\n");
      free(settings);
      free(board);
      free(states);
      free(power);
      free(wall);
      free(logs);
      return EXIT_FAILURE;
    }

  pnut_loop_defaults(&config);
  config.name   = "system";
  config.levels = true;

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "pnut_system: no loop: %d\n", ret);
      free(settings);
      free(board);
      free(states);
      free(power);
      free(wall);
      free(logs);
      return EXIT_FAILURE;
    }

  bconfig.path = CONFIG_PNUT_BOARD_DESCRIPTION[0] != '\0' ?
                 CONFIG_PNUT_BOARD_DESCRIPTION : NULL;
  board_init(board, &bconfig);

  tconfig.initctl  = config.initctl;
  tconfig.programs = CONFIG_PNUT_PROGRAMS_PATH;
  tconfig.services = CONFIG_PNUT_STATES_SERVICES;
  states_init(states, &tconfig);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir             = CONFIG_PNUT_SETTINGS_DIR;
  sconfig.defaults        = CONFIG_PNUT_SETTINGS_DEFAULTS[0] != '\0' ?
                            CONFIG_PNUT_SETTINGS_DEFAULTS : NULL;
  sconfig.limits.owners   = CONFIG_PNUT_SETTINGS_OWNERS;
  sconfig.limits.settings = CONFIG_PNUT_SETTINGS_MAX;
  sconfig.limits.texts    = CONFIG_PNUT_SETTINGS_TEXTS;
  sconfig.limits.choices  = CONFIG_PNUT_SETTINGS_CHOICES;
  sconfig.delay           = CONFIG_PNUT_SETTINGS_DELAY;
  sconfig.who             = system_who;
  sconfig.runs            = system_runs;
  sconfig.identity        = states;
  settings_init(settings, &sconfig);

  memset(&wconfig, 0, sizeof(wconfig));
  wconfig.state     = CONFIG_PNUT_TIME_STATE[0] != '\0' ?
                      CONFIG_PNUT_TIME_STATE : NULL;
  wconfig.tolerance = CONFIG_PNUT_TIME_TOLERANCE;
  wconfig.who       = system_who;
  wconfig.identity  = states;
  wallclock_init(wall, &wconfig);

  memset(&lconfig, 0, sizeof(lconfig));
  lconfig.source     = CONFIG_PNUT_LOG_SOURCE[0] != '\0' ?
                       CONFIG_PNUT_LOG_SOURCE : NULL;
  lconfig.dir        = CONFIG_PNUT_LOG_DIR;
  lconfig.file_size  = CONFIG_PNUT_LOG_FILE_SIZE;
  lconfig.files      = CONFIG_PNUT_LOG_FILES;
  lconfig.crashes    = CONFIG_PNUT_LOG_CRASHES;
  lconfig.crash_size = CONFIG_PNUT_LOG_CRASH_SIZE;
  lconfig.delay      = CONFIG_PNUT_LOG_DELAY;
  lconfig.batch      = CONFIG_PNUT_LOG_BATCH;
  logs_init(logs, &lconfig);

  battery.gauge   = open(CONFIG_PNUT_POWER_GAUGE, O_RDONLY | O_CLOEXEC);
  battery.charger = open(CONFIG_PNUT_POWER_CHARGER, O_RDONLY | O_CLOEXEC);

  /* Without a way to power off, the guard never acts: it would only stop
   * the program again and again.  The voltage it powers off at is the
   * battery's, from Board's description.
   */

  memset(&pconfig, 0, sizeof(pconfig));
#ifdef CONFIG_BOARDCTL_POWEROFF
  pconfig.interval = CONFIG_PNUT_POWER_INTERVAL;
  pconfig.reads    = CONFIG_PNUT_POWER_READS;
  pconfig.board    = BOARD_SERVICE;
#endif
  pconfig.measure  = system_measure;
  pconfig.arg      = &battery;
  power_init(power, &pconfig);

  /* Board first: the others read the description (RFC 0024) */

  ret = pnut_module_add(loop, &board->module);
  if (ret >= 0)
    {
      ret = pnut_module_add(loop, &states->module);
    }

  if (ret >= 0)
    {
      ret = pnut_module_add(loop, &settings->module);
    }

  if (ret >= 0)
    {
      ret = pnut_module_add(loop, &wall->module);
    }

  if (ret >= 0)
    {
      ret = pnut_module_add(loop, &logs->module);
    }

  if (ret >= 0)
    {
      ret = pnut_module_add(loop, &power->module);
    }

  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  /* A write may run on a worker until the loop is destroyed */

  pnut_loop_destroy(loop);
  off = power_wants_off(power);
  settings_deinit(settings);
  states_deinit(states);
  logs_deinit(logs);
  free(settings);
  free(board);
  free(states);
  free(power);
  free(wall);
  free(logs);

  if (battery.gauge >= 0)
    {
      close(battery.gauge);
    }

  if (battery.charger >= 0)
    {
      close(battery.charger);
    }

#ifdef CONFIG_BOARDCTL_POWEROFF
  if (off)
    {
      system_power_off();
      return EXIT_FAILURE;
    }
#else
  (void)off;
#endif

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
