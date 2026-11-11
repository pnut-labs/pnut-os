/****************************************************************************
 * pnut-os/src/network/radio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The Wi-Fi radio as NuttX's driver works it (wifi.h's operations), each
 * call blocking, on the network program's worker.  The sequence is the
 * one the system before this one found to work on the same hardware
 * (2026-09-26): the board's enable device brings the driver up once,
 * then the interface up and down; a join is an association through the
 * WEXT ioctls, a wait for the access point, then DHCP.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/ether.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <nuttx/ioexpander/gpio.h>
#include <netutils/dhcpc.h>
#include <netutils/netlib.h>
#include <wireless/wapi.h>

#include "radio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RADIO_ASSOC_MS    15000   /* For the access point to take us */
#define RADIO_SCAN_MS     10000   /* For a scan to end */
#define RADIO_STEP_MS     200

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t radio_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/****************************************************************************
 * Name: radio_up, radio_down
 ****************************************************************************/

static int radio_up(FAR void *arg)
{
  FAR struct radio_s *r = arg;
  int ret;
  int fd;

  /* The board's driver, once: writing it again does nothing */

  if (!r->driver && r->enable != NULL)
    {
      fd = open(r->enable, O_RDWR | O_CLOEXEC);
      if (fd < 0)
        {
          return -ENODEV;
        }

      ret = ioctl(fd, GPIOC_WRITE, 1);
      close(fd);
      if (ret < 0)
        {
          return -EIO;
        }

      r->driver = true;
    }

  ret = netlib_ifup(r->ifname);
  return ret < 0 ? -EIO : OK;
}

static void radio_down(FAR void *arg)
{
  FAR struct radio_s *r = arg;
  struct in_addr none;
  int sock;

  /* Nothing to take down while the board's driver was never brought up;
   * without an enable device it is always there
   */

  if (!r->driver && r->enable != NULL)
    {
      return;
    }

  sock = wapi_make_socket();
  if (sock >= 0)
    {
      wpa_driver_wext_disconnect(sock, r->ifname);
      close(sock);
    }

  netlib_ifdown(r->ifname);
  memset(&none, 0, sizeof(none));
  netlib_set_ipv4addr(r->ifname, &none);
}

/****************************************************************************
 * Name: radio_channel
 *
 * Description:
 *   A scan's frequency as a channel: the driver gives a channel number
 *   or a frequency, in Hz or MHz.  0 when it is neither.
 *
 ****************************************************************************/

static uint8_t radio_channel(double freq)
{
  int mhz;

  if (freq >= 1.0 && freq < 200.0)
    {
      return (uint8_t)freq;       /* A channel already */
    }

  mhz = (int)(freq >= 1.0e6 ? freq / 1.0e6 : freq);
  if (mhz == 2484)
    {
      return 14;
    }

  if (mhz >= 2412 && mhz <= 2472)
    {
      return (uint8_t)((mhz - 2407) / 5);
    }

  if (mhz >= 5160 && mhz <= 5885)
    {
      return (uint8_t)((mhz - 5000) / 5);
    }

  return 0;
}

/****************************************************************************
 * Name: radio_scan
 *
 * Description:
 *   The networks in reach, each name once at its strongest, the
 *   strongest max; hidden ones (no name) left out.
 *
 ****************************************************************************/

static int radio_scan(FAR void *arg, FAR struct wifi_found_s *out, int max)
{
  FAR struct radio_s *r = arg;
  FAR struct wapi_scan_info_s *info;
  struct wapi_list_s list;
  uint32_t start;
  int weakest;
  int sock;
  int ret;
  int n = 0;
  int i;

  sock = wapi_make_socket();
  if (sock < 0)
    {
      return -errno;
    }

  ret   = wapi_scan_init(sock, r->ifname, NULL);
  start = radio_ms();
  while (ret >= 0 && (ret = wapi_scan_stat(sock, r->ifname)) > 0 &&
         radio_ms() - start < RADIO_SCAN_MS)
    {
      usleep(RADIO_STEP_MS * 1000);
    }

  memset(&list, 0, sizeof(list));
  if (ret > 0)
    {
      ret = -ETIMEDOUT;           /* Still scanning */
    }

  if (ret >= 0)
    {
      ret = wapi_scan_coll(sock, r->ifname, &list);
    }

  for (info = list.head.scan; ret >= 0 && info != NULL; info = info->next)
    {
      if (!info->has_essid || info->essid[0] == '\0')
        {
          continue;
        }

      for (i = 0; i < n && strcmp(out[i].name, info->essid) != 0; i++)
        {
        }

      if (i < n)
        {
          if (info->rssi > out[i].signal)
            {
              out[i].signal = (int16_t)info->rssi;
            }

          continue;
        }

      /* The strongest max: the weakest goes when full */

      if (n == max)
        {
          weakest = 0;
          for (i = 1; i < n; i++)
            {
              if (out[i].signal < out[weakest].signal)
                {
                  weakest = i;
                }
            }

          if (out[weakest].signal >= info->rssi)
            {
              continue;
            }

          i = weakest;
        }
      else
        {
          i = n++;
        }

      memset(&out[i], 0, sizeof(out[i]));
      strlcpy(out[i].name, info->essid, sizeof(out[i].name));
      out[i].signal  = (int16_t)info->rssi;
      out[i].channel = info->has_freq ? radio_channel(info->freq) : 0;
      out[i].secured = info->has_encode && (info->encode & 0x8000) == 0;
    }

  wapi_scan_coll_free(&list);
  close(sock);
  return ret < 0 ? ret : n;
}

/****************************************************************************
 * Name: radio_linked
 ****************************************************************************/

static bool radio_linked(FAR void *arg, FAR int16_t *signal)
{
  FAR struct radio_s *r = arg;
  static const struct ether_addr none;
  struct ether_addr ap;
  bool linked = false;
  int sense;
  int sock;

  sock = wapi_make_socket();
  if (sock < 0)
    {
      return false;
    }

  if (wapi_get_ap(sock, r->ifname, &ap) >= 0 &&
      memcmp(&ap, &none, sizeof(ap)) != 0)
    {
      linked = true;
      if (wapi_get_sensitivity(sock, r->ifname, &sense) >= 0)
        {
          *signal = (int16_t)sense;
        }
    }

  close(sock);
  return linked;
}

/****************************************************************************
 * Name: radio_join
 ****************************************************************************/

static int radio_join(FAR void *arg, FAR const char *name,
                      FAR const char *passphrase, FAR int16_t *signal)
{
  FAR struct radio_s *r = arg;
  struct wpa_wconfig_s conf;
  uint32_t start;
  int ret;

  memset(&conf, 0, sizeof(conf));
  conf.ifname   = r->ifname;
  conf.sta_mode = WAPI_MODE_MANAGED;
  conf.ssid     = name;
  conf.ssidlen  = strlen(name);
  if (passphrase[0] != '\0')
    {
      conf.auth_wpa    = IW_AUTH_WPA_VERSION_WPA2;
      conf.cipher_mode = IW_AUTH_CIPHER_CCMP;
      conf.alg         = WPA_ALG_CCMP;
      conf.passphrase  = passphrase;
      conf.phraselen   = strlen(passphrase);
    }
  else
    {
      conf.auth_wpa    = IW_AUTH_WPA_VERSION_DISABLED;
      conf.cipher_mode = IW_AUTH_CIPHER_NONE;
      conf.alg         = WPA_ALG_NONE;
    }

  ret = wpa_driver_wext_associate(&conf);
  if (ret < 0)
    {
      return -ECONNREFUSED;
    }

  /* The access point takes a moment */

  start = radio_ms();
  while (!radio_linked(r, signal))
    {
      if (radio_ms() - start > RADIO_ASSOC_MS)
        {
          return -ETIMEDOUT;
        }

      usleep(RADIO_STEP_MS * 1000);
    }

  return OK;
}

/****************************************************************************
 * Name: radio_router
 *
 * Description:
 *   The interface's default router, which ifconfig shows and ARP falls
 *   back to.  Not netlib_set_dripv4addr(): with CONFIG_NET_ROUTE it also
 *   replaces the default route, which Network keeps (RFC 0015).
 *
 ****************************************************************************/

static void radio_router(FAR const char *ifname,
                         FAR const struct in_addr *router)
{
  FAR struct sockaddr_in *in;
  struct ifreq req;
  int sock;

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    {
      return;
    }

  memset(&req, 0, sizeof(req));
  strlcpy(req.ifr_name, ifname, IFNAMSIZ);
  in             = (FAR struct sockaddr_in *)&req.ifr_addr;
  in->sin_family = AF_INET;
  in->sin_addr   = *router;
  ioctl(sock, SIOCSIFDSTADDR, (unsigned long)&req);
  close(sock);
}

/****************************************************************************
 * Name: radio_address
 *
 * Description:
 *   An address by DHCP, set with the router.  The router and name servers
 *   are given back: Network chooses the link in use, and its name server
 *   (RFC 0015).
 *
 ****************************************************************************/

static int radio_address(FAR void *arg, FAR struct wifi_lease_s *lease)
{
  FAR struct radio_s *r = arg;
  struct dhcpc_state ds;
  uint8_t mac[IFHWADDRLEN];
  FAR void *handle;
  int ret;
  int i;

  ret = netlib_getmacaddr(r->ifname, mac);
  if (ret < 0)
    {
      return -EIO;
    }

  handle = dhcpc_open(r->ifname, mac, IFHWADDRLEN);
  if (handle == NULL)
    {
      return -ENOMEM;
    }

  memset(&ds, 0, sizeof(ds));
  ret = dhcpc_request(handle, &ds);
  dhcpc_close(handle);
  if (ret < 0 || ds.ipaddr.s_addr == 0)
    {
      return -EADDRNOTAVAIL;
    }

  netlib_set_ipv4addr(r->ifname, &ds.ipaddr);
  if (ds.netmask.s_addr != 0)
    {
      netlib_set_ipv4netmask(r->ifname, &ds.netmask);
    }

  if (ds.default_router.s_addr != 0)
    {
      radio_router(r->ifname, &ds.default_router);
    }

  for (i = 0; i < ds.num_dnsaddr && i < WIFI_DNS_MAX; i++)
    {
      lease->dns[i] = ds.dnsaddr[i].s_addr;
    }

  lease->addr   = ds.ipaddr.s_addr;
  lease->router = ds.default_router.s_addr;
  return OK;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct wifi_ops_s g_radio_ops =
{
  .up      = radio_up,
  .down    = radio_down,
  .scan    = radio_scan,
  .join    = radio_join,
  .address = radio_address,
  .linked  = radio_linked,
};
