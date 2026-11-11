/****************************************************************************
 * pnut-os/src/network/network.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Network (RFC 0015), its first part: connectivity.  It reads the links'
 * state on the wifi and telephony topics and says the link in use on the
 * network topic: Wi-Fi whenever it is joined with an address and a
 * router, since it costs nothing; mobile data otherwise, while it is on.
 *
 * Traffic follows: NuttX's one default route goes through the link in
 * use, via Wi-Fi's router, or via mobile data's own address (a
 * point-to-point link, whose host-only netmask, data_ppp.c, only that
 * address matches).  Without it, NuttX would send it out of the first
 * interface running, and mobile data's may come before Wi-Fi's (the
 * T-Deck Max's wlan0 only comes with the radio).  A local network's own
 * addresses still go to its interface: the longest match wins.
 *
 * Nothing else writes it: netlib_set_dripv4addr() would replace it, and
 * neither Wi-Fi's DHCP (radio.c) nor pppd, told of its link, calls it.
 * Still, the route is set again at every change of a link's state or
 * address, where such a writer would have run.
 *
 * Name lookups ask the link in use's name server, and only it: a server
 * left from another link would be asked over the wrong one and time out
 * first.
 *
 * As it starts, it keeps the local ports (adb's) to the local links
 * (RFC 0012): their connections from mobile data are dropped.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <pnut/log.h>

#include "network.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: network_word
 *
 * Description:
 *   The next word of a list separated by spaces: its start and length; 0
 *   at the end.
 *
 ****************************************************************************/

static size_t network_word(FAR const char **list, FAR const char **word)
{
  FAR const char *p = *list;
  size_t n = 0;

  while (*p == ' ')
    {
      p++;
    }

  *word = p;
  while (p[n] != '\0' && p[n] != ' ')
    {
      n++;
    }

  *list = p + n;
  return n;
}

/****************************************************************************
 * Name: network_parse
 *
 * Description:
 *   The local links and ports, from the configuration's lists.  A word
 *   that is not a name or a port is left out, and said.
 *
 ****************************************************************************/

static void network_parse(FAR struct network_s *net)
{
  FAR const struct network_config_s *c = &net->config;
  FAR const char *list;
  FAR const char *word;
  FAR char *end;
  char text[8];
  size_t n;
  long port;

  list = c->local_ports != NULL ? c->local_ports : "";
  while ((n = network_word(&list, &word)) > 0)
    {
      port = 0;
      if (n < sizeof(text))
        {
          memcpy(text, word, n);
          text[n] = '\0';
          port = strtol(text, &end, 10);
          if (*end != '\0')
            {
              port = 0;
            }
        }

      if (port < 1 || port > 65535 || net->nports == NETWORK_PORTS_MAX)
        {
          pnut_log(&net->module, PNUT_LOG_WARNING,
                   "Local port left out: %.*s", (int)n, word);
          continue;
        }

      net->ports[net->nports++] = (uint16_t)port;
    }

  list = c->local_links != NULL ? c->local_links : "";
  while ((n = network_word(&list, &word)) > 0)
    {
      if (n >= NETWORK_IFNAME || net->nlinks == NETWORK_LINKS_MAX)
        {
          pnut_log(&net->module, PNUT_LOG_WARNING,
                   "Local link left out: %.*s", (int)n, word);
          continue;
        }

      memcpy(net->links[net->nlinks], word, n);
      net->links[net->nlinks][n] = '\0';
      net->nlinks++;
    }
}

/****************************************************************************
 * Name: network_guard
 *
 * Description:
 *   The local ports kept to the local links that exist: again whenever
 *   Wi-Fi's state changes while one does not, since a radio's interface
 *   may only come with the radio (the T-Deck Max's wlan0); and at each
 *   link's news after a failure, since mobile data may come up meanwhile.
 *
 ****************************************************************************/

static void network_guard(FAR struct network_s *net)
{
  FAR const struct network_config_s *c = &net->config;
  int ret;

  /* Nothing to do once every local link has its rule.  A failed call
   * changes neither count, so it never ends here; guard_failed only
   * makes that plain.
   */

  if (net->nports == 0 ||
      (net->guarded && !net->guard_failed && net->found == net->nlinks))
    {
      return;
    }

  ret = c->ops->guard(c->arg, net->links, net->nlinks, net->ports,
                      net->nports);
  if (ret < 0)
    {
      if (!net->guard_failed)
        {
          pnut_log(&net->module, PNUT_LOG_ERROR,
                   "Local ports open on every link: %d", ret);
        }

      net->guard_failed = true;
      return;
    }

  if (!net->guarded || ret != net->found || net->guard_failed)
    {
      pnut_log(&net->module, PNUT_LOG_INFO,
               "Local ports: on %d of %d local links", ret, net->nlinks);
    }

  net->guarded      = true;
  net->guard_failed = false;
  net->found        = ret;
}

/****************************************************************************
 * Name: network_route
 *
 * Description:
 *   The default route through the link in use, when its router changed,
 *   or the last try failed.
 *
 ****************************************************************************/

static void network_route(FAR struct network_s *net)
{
  int ret;

  if (net->routed && net->routed_via == net->router)
    {
      return;
    }

  ret = net->config.ops->route(net->config.arg, net->router);
  if (ret < 0)
    {
      pnut_log(&net->module, PNUT_LOG_WARNING, "Route not set: %d", ret);
      net->routed = false;
      return;
    }

  net->routed     = true;
  net->routed_via = net->router;
}

/****************************************************************************
 * Name: network_resolve
 *
 * Description:
 *   The resolver given the link's name server, when the link or the
 *   server changed, or the last try failed.
 *
 ****************************************************************************/

static void network_resolve(FAR struct network_s *net)
{
  FAR const pnut_network_info_t *s = &net->state;
  int ret;

  if (net->resolved && net->resolved_link == s->link &&
      net->resolved_dns == s->dns)
    {
      return;
    }

  ret = net->config.ops->resolver(net->config.arg, s->dns);
  if (ret < 0)
    {
      pnut_log(&net->module, PNUT_LOG_WARNING,
               "Name server not set: %d", ret);
      net->resolved = false;
      return;
    }

  net->resolved      = true;
  net->resolved_link = s->link;
  net->resolved_dns  = s->dns;
}

/****************************************************************************
 * Name: network_choose
 *
 * Description:
 *   The link in use, from the links' state: the route and the name
 *   server set for it, and announced.
 *
 ****************************************************************************/

static void network_choose(FAR struct network_s *net)
{
  FAR pnut_network_info_t *s = &net->state;
  static FAR const char *const names[] =
  {
    "none", "Wi-Fi", "mobile data"
  };

  uint8_t was = s->link;
  int ret;

  memset(s, 0, sizeof(*s));
  net->router = 0;
  if (net->wifi_up)
    {
      s->link     = PNUT_NETWORK_INFO_LINK_WIFI;
      s->address  = net->wifi_address;
      s->dns      = net->wifi_dns;
      net->router = net->wifi_router;
    }
  else if (net->mobile_up)
    {
      s->link     = PNUT_NETWORK_INFO_LINK_MOBILE;
      s->metered  = true;
      s->address  = net->mobile_address;
      s->dns      = net->mobile_dns;
      net->router = net->mobile_address;
    }

  if (s->link != was)
    {
      pnut_log(&net->module, PNUT_LOG_INFO, "Link in use: %s%s",
               names[s->link], s->link != 0 && s->dns == 0 ?
               ", no name server" : "");
    }

  network_route(net);
  network_resolve(net);

  if (net->topic == NULL ||
      (net->announced && memcmp(s, &net->published, sizeof(*s)) == 0))
    {
      return;
    }

  ret = pnut_network_info_publish(net->topic, s);
  if (ret < 0)
    {
      pnut_log(&net->module, PNUT_LOG_WARNING, "Cannot announce: %d", ret);
      return;
    }

  net->published = *s;
  net->announced = true;
}

/****************************************************************************
 * Name: network_wifi, network_telephony
 *
 * Description:
 *   A link's state.  Only the first name server is kept: NuttX asks one
 *   (CONFIG_NETDB_DNSSERVER_NAMESERVERS).  Wi-Fi without a router does
 *   not reach the internet: not a link to use.
 *
 ****************************************************************************/

static void network_wifi(FAR struct pnut_wifi_info_reader_s *reader,
                         FAR const pnut_wifi_info_t *msg, FAR void *arg)
{
  FAR struct network_s *net = arg;
  bool changed = msg->state != net->wifi_news.state;

  if (changed || msg->address != net->wifi_news.address ||
      msg->router != net->wifi_news.router)
    {
      net->routed = false;        /* Set again at a link's change */
    }

  net->wifi_news.state   = msg->state;
  net->wifi_news.address = msg->address;
  net->wifi_news.router  = msg->router;
  if (changed || net->guard_failed)
    {
      network_guard(net);
    }

  net->wifi_up      = msg->state == PNUT_WIFI_INFO_STATE_JOINED &&
                      msg->address != 0 && msg->router != 0;
  net->wifi_address = net->wifi_up ? msg->address : 0;
  net->wifi_router  = net->wifi_up ? msg->router : 0;
  net->wifi_dns     = net->wifi_up && msg->dns_count > 0 ?
                      msg->dns[0] : 0;
  network_choose(net);
}

static void
network_telephony(FAR struct pnut_telephony_info_reader_s *reader,
                  FAR const pnut_telephony_info_t *msg, FAR void *arg)
{
  FAR struct network_s *net = arg;

  if (net->guard_failed)
    {
      network_guard(net);
    }

  if (msg->data != net->mobile_news.state ||
      msg->data_address != net->mobile_news.address)
    {
      net->routed = false;        /* Set again at a link's change */
    }

  net->mobile_news.state   = msg->data;
  net->mobile_news.address = msg->data_address;
  net->mobile_up      = msg->data == PNUT_TELEPHONY_INFO_DATA_DATA_ON &&
                        msg->data_address != 0;
  net->mobile_address = net->mobile_up ? msg->data_address : 0;
  net->mobile_dns     = net->mobile_up && msg->data_dns_count > 0 ?
                        msg->data_dns[0] : 0;
  network_choose(net);
}

/****************************************************************************
 * Name: network_start, network_stop
 ****************************************************************************/

static int network_start(FAR struct pnut_module_s *module,
                         FAR struct pnut_loop_s *loop)
{
  FAR struct network_s *net = (FAR struct network_s *)module;
  int ret;

  /* The local ports first: mobile data may come up any time */

  network_parse(net);
  network_guard(net);

  ret = pnut_network_info_advertise(loop, &net->topic);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No network topic: %d", ret);
      net->topic = NULL;
    }

  /* No link yet: no name server, not one left from before */

  network_choose(net);

  ret = pnut_wifi_info_subscribe(loop, network_wifi, net, &net->wifi);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No wifi topic: %d", ret);
    }
  else
    {
      net->wifi_open = true;
    }

  ret = pnut_telephony_info_subscribe(loop, network_telephony, net,
                                      &net->telephony);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No telephony topic: %d", ret);
    }
  else
    {
      net->telephony_open = true;
    }

  pnut_module_ready(module);
  return 0;
}

static void network_stop(FAR struct pnut_module_s *module,
                         FAR struct pnut_loop_s *loop)
{
  FAR struct network_s *net = (FAR struct network_s *)module;

  if (net->wifi_open)
    {
      pnut_wifi_info_unsubscribe(&net->wifi);
      net->wifi_open = false;
    }

  if (net->telephony_open)
    {
      pnut_telephony_info_unsubscribe(&net->telephony);
      net->telephony_open = false;
    }

  pnut_topic_unadvertise(net->topic);
  net->topic = NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: network_init
 ****************************************************************************/

void network_init(FAR struct network_s *net,
                  FAR const struct network_config_s *config)
{
  memset(net, 0, sizeof(*net));
  net->config       = *config;
  net->module.name  = "network";
  net->module.start = network_start;
  net->module.stop  = network_stop;
}
