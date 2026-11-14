/****************************************************************************
 * pnut-os/src/radios/main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The radios program (RFC 0007): so far Radio (RFC 0034).  NxInit starts
 * it once the system program is ready.
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

#include "packetradio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Board's service, in the system program: the radios */

#define RADIOS_BOARD  "board"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  struct radio_config_s rconfig;
  FAR struct pnut_loop_s *loop;
  static struct radio_s radio;
  int ret;

  pnut_loop_defaults(&config);
  config.name      = "radios";
  config.levels    = true;
  config.workers   = 1;
  config.jobs      = 2;
  config.stacksize = CONFIG_PNUT_RADIOS_WORKER_STACKSIZE;

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "pnut_radios: no loop: %d\n", ret);
      return EXIT_FAILURE;
    }

  memset(&rconfig, 0, sizeof(rconfig));
  rconfig.ops   = &g_radio_sx126x;
  rconfig.board = RADIOS_BOARD;
  radio_init(&radio, &rconfig);

  ret = pnut_module_add(loop, &radio.module);
  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  pnut_loop_destroy(loop);
  radio_deinit(&radio);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
