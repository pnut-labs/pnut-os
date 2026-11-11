/****************************************************************************
 * pnut-os/src/network/main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The network program (RFC 0007): Wi-Fi (RFC 0031), and Network
 * (RFC 0015), so far the link in use and its name server.  NxInit starts
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

#include "netstack.h"
#include "network.h"
#include "radio.h"
#include "wifi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Service states', in the system program: who calls */

#define NETWORK_SERVICES  "services"

/* The ports served on the local links only: adb's, and the board's */

#define NETWORK_STR(x)    NETWORK_STR2(x)
#define NETWORK_STR2(x)   #x

#ifdef CONFIG_ADBD_TCP_SERVER
#  define NETWORK_ADB     " " NETWORK_STR(CONFIG_ADBD_TCP_SERVER_PORT)
#else
#  define NETWORK_ADB     ""
#endif

#define NETWORK_PORTS     CONFIG_PNUT_NETWORK_LOCAL_PORTS NETWORK_ADB

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  struct network_config_s nconfig;
  struct wifi_config_s wconfig;
  FAR struct pnut_loop_s *loop;
  FAR struct wifi_s *wifi;
  static struct network_s network;
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

  /* Network first: it keeps the local ports to the local links as it
   * starts
   */

  memset(&nconfig, 0, sizeof(nconfig));
  nconfig.ops         = &g_netstack_ops;
  nconfig.local_links = CONFIG_PNUT_NETWORK_LOCAL_LINKS;
  nconfig.local_ports = NETWORK_PORTS;
  network_init(&network, &nconfig);

  radio.enable = CONFIG_PNUT_WIFI_ENABLE[0] != '\0' ?
                 CONFIG_PNUT_WIFI_ENABLE : NULL;
  radio.ifname = CONFIG_PNUT_WIFI_IFNAME;

  memset(&wconfig, 0, sizeof(wconfig));
  wconfig.services = NETWORK_SERVICES;
  wconfig.ops      = &g_radio_ops;
  wconfig.arg      = &radio;
  wifi_init(wifi, &wconfig);

  ret = pnut_module_add(loop, &network.module);
  if (ret >= 0)
    {
      ret = pnut_module_add(loop, &wifi->module);
    }

  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  pnut_loop_destroy(loop);
  wifi_end(wifi);
  free(wifi);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
