/****************************************************************************
 * pnut-os/src/network/radio.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_NETWORK_RADIO_H
#define __PNUT_OS_NETWORK_RADIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>

#include "wifi.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The radio's place: the board's device that brings the driver up (a GPIO
 * device written 1, once; NULL: always up), and its interface
 */

struct radio_s
{
  FAR const char *enable;
  FAR const char *ifname;
  bool driver;                    /* Brought up */
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* NuttX's driver, as wifi.h's operations; their arg a struct radio_s */

extern const struct wifi_ops_s g_radio_ops;

#endif /* __PNUT_OS_NETWORK_RADIO_H */
