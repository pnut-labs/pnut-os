/****************************************************************************
 * pnut-os/src/ui/wifilist.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_WIFILIST_H
#define __PNUT_OS_UI_WIFILIST_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/compiler.h>

#include "pnut/wifi.pb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Networks kept, the strongest; a scan's list redrawn at most this often */

#define WIFILIST_MAX       24
#define WIFILIST_EVERY_MS  5000

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The networks in reach, from a scan: one a name and security, the
 * strongest access point's signal, the strongest first; hidden ones left
 * out
 */

struct wifilist_s
{
  pnut_wifi_network_t nets[WIFILIST_MAX];
  bool known[WIFILIST_MAX];
  uint8_t count;
  uint64_t shown;                 /* When last drawn, ms */
  bool drawn;                     /* Results drawn since the scan began */
  bool scanning;
  bool dirty;                     /* Changed since drawn */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void wifilist_start(FAR struct wifilist_s *l, uint64_t now);
void wifilist_add(FAR struct wifilist_s *l,
                  FAR const pnut_wifi_network_t *n);
void wifilist_known(FAR struct wifilist_s *l,
                    FAR const pnut_wifi_known_networks_t *k);
void wifilist_end(FAR struct wifilist_s *l);

/* Whether to draw the list now: its first results at once, then at most
 * every WIFILIST_EVERY_MS while scanning, at once when the scan ends
 */

bool wifilist_due(FAR const struct wifilist_s *l, uint64_t now);
void wifilist_shown(FAR struct wifilist_s *l, uint64_t now);

#endif /* __PNUT_OS_UI_WIFILIST_H */
