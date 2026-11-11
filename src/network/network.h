/****************************************************************************
 * pnut-os/src/network/network.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_NETWORK_NETWORK_H
#define __PNUT_OS_NETWORK_NETWORK_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/module.h>
#include <pnut/topic.h>

#include "pnut/network.topics.h"
#include "pnut/telephony.topics.h"
#include "pnut/wifi.topics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NETWORK_LINKS_MAX   4     /* Local links */
#define NETWORK_PORTS_MAX   8     /* Ports served on them only */
#define NETWORK_IFNAME      16    /* An interface's name, its NUL included */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A link's state and address as last heard, to see a change */

struct network_news_s
{
  uint8_t state;                  /* PNUT_WIFI_INFO_STATE_*, or
                                   * PNUT_TELEPHONY_INFO_DATA_* */
  uint32_t address;
  uint32_t router;
};

/* What Network does to NuttX's network: quick calls, made on the loop.
 * The program gives NuttX's (network/netstack.c); the tests a fake.
 */

struct network_ops_s
{
  /* The default route, the only one: via router, IPv4, network order,
   * which a network of an interface holds (a point-to-point link's own
   * address, under its host-only netmask); or none (0)
   */

  CODE int (*route)(FAR void *arg, uint32_t router);

  /* The name server every name lookup asks: one, IPv4, network order, or
   * none (0).  The answers kept from another go.
   */

  CODE int (*resolver)(FAR void *arg, uint32_t server);

  /* Connections to these TCP ports reach the device on the local links
   * only, and on none of them that does not exist yet: those from any
   * other link (mobile data) are dropped.  It replaces what an earlier
   * call set, and returns the number of local links that exist, or a
   * negated errno value.
   */

  CODE int (*guard)(FAR void *arg, FAR const char (*links)[NETWORK_IFNAME],
                    int nlinks, FAR const uint16_t *ports, int nports);
};

struct network_config_s
{
  FAR const struct network_ops_s *ops;
  FAR void *arg;

  /* The local links (RFC 0012): the interfaces of the networks the device
   * is in, as Wi-Fi's or Ethernet's, never mobile data's; and the TCP
   * ports served there only, as adb's.  Separated by spaces; NULL or
   * empty for none.
   */

  FAR const char *local_links;
  FAR const char *local_ports;
};

struct network_s
{
  struct pnut_module_s module;    /* First */
  struct network_config_s config;

  /* The local links and ports, as configured; and how many of the links
   * existed when the ports were last kept to them
   */

  char links[NETWORK_LINKS_MAX][NETWORK_IFNAME];
  uint16_t ports[NETWORK_PORTS_MAX];
  uint8_t nlinks;
  uint8_t nports;
  uint8_t found;
  bool guarded;
  bool guard_failed;              /* Said once */

  /* The links, as their topics say */

  struct pnut_wifi_info_reader_s wifi;
  bool wifi_open;
  struct network_news_s wifi_news;
  bool wifi_up;                   /* Joined, with an address and a router */
  uint32_t wifi_address;
  uint32_t wifi_router;
  uint32_t wifi_dns;              /* 0: none */

  struct pnut_telephony_info_reader_s telephony;
  bool telephony_open;
  struct network_news_s mobile_news;
  bool mobile_up;                 /* Mobile data on, with an address */
  uint32_t mobile_address;
  uint32_t mobile_dns;

  /* The link in use, and what was done for it */

  FAR struct pnut_publisher_s *topic;
  pnut_network_info_t state;
  pnut_network_info_t published;
  bool announced;
  uint32_t router;                /* The link in use's; 0: none */
  bool routed;                    /* The route set for router */
  uint32_t routed_via;
  bool resolved;                  /* The resolver set for state's link */
  uint8_t resolved_link;
  uint32_t resolved_dns;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: network_init
 *
 * Description:
 *   Set the module up; pnut_module_add() then starts it.
 *
 ****************************************************************************/

void network_init(FAR struct network_s *net,
                  FAR const struct network_config_s *config);

#endif /* __PNUT_OS_NETWORK_NETWORK_H */
