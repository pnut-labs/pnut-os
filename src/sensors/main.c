/****************************************************************************
 * pnut-os/src/sensors/main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The sensors program (RFC 0007): so far Location (RFC 0035).  NxInit
 * starts it once the system program is ready.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pnut/loop.h>
#include <pnut/module.h>

#include "location.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Board's service, in the system program: the receiver */

#define SENSORS_BOARD  "board"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  struct location_config_s lconfig;
  FAR struct pnut_loop_s *loop;
  static struct location_s loc;
  int ret;

  pnut_loop_defaults(&config);
  config.name      = "sensors";
  config.levels    = true;
  config.workers   = 1;
  config.jobs      = 2;
  config.stacksize = CONFIG_PNUT_SENSORS_WORKER_STACKSIZE;

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "pnut_sensors: no loop: %d\n", ret);
      return EXIT_FAILURE;
    }

  memset(&lconfig, 0, sizeof(lconfig));
  lconfig.ops       = &g_location_gnss;
  lconfig.board     = SENSORS_BOARD;
  lconfig.dir       = CONFIG_PNUT_LOCATION_DIR;
  lconfig.linger_ms = CONFIG_PNUT_LOCATION_LINGER;
  location_init(&loc, &lconfig);

  ret = pnut_module_add(loop, &loc.module);
  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  pnut_loop_destroy(loop);
  location_deinit(&loc);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
