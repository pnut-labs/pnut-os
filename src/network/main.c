/****************************************************************************
 * pnut-os/src/network/main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The network program (RFC 0007, RFC 0031): Wi-Fi now; RFC 0015's Network
 * later.  NxInit starts it once the system program is ready.
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

#include "radio.h"
#include "wifi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Service states', in the system program: who calls */

#define NETWORK_SERVICES  "services"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  struct wifi_config_s wconfig;
  FAR struct pnut_loop_s *loop;
  FAR struct wifi_s *wifi;
  static struct radio_s radio;
  int ret;

  wifi = calloc(1, sizeof(*wifi));
  if (wifi == NULL)
    {
      fprintf(stderr, "pnut_network: no memory\n");
      return EXIT_FAILURE;
    }

  pnut_loop_defaults(&config);
  config.name      = "network";
  config.levels    = true;
  config.stacksize = CONFIG_PNUT_NETWORK_WORKER_STACKSIZE;

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "pnut_network: no loop: %d\n", ret);
      free(wifi);
      return EXIT_FAILURE;
    }

  radio.enable = CONFIG_PNUT_WIFI_ENABLE[0] != '\0' ?
                 CONFIG_PNUT_WIFI_ENABLE : NULL;
  radio.ifname = CONFIG_PNUT_WIFI_IFNAME;

  memset(&wconfig, 0, sizeof(wconfig));
  wconfig.services = NETWORK_SERVICES;
  wconfig.ops      = &g_radio_ops;
  wconfig.arg      = &radio;
  wifi_init(wifi, &wconfig);

  ret = pnut_module_add(loop, &wifi->module);
  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  pnut_loop_destroy(loop);
  wifi_end(wifi);
  free(wifi);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
