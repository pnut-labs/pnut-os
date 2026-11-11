/****************************************************************************
 * pnut-os/src/network/netstack.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* NuttX's network, as Network (network.c) works it: the default route
 * (CONFIG_NET_ROUTE), the resolver's name server, and the packet filter
 * that keeps the local ports to the local links (CONFIG_NET_IPFILTER,
 * through netlib's iptables calls).
 *
 * The filter is stateless, and names an interface by the device it is
 * when a rule is written: a rule for an interface that does not exist
 * yet would match every one.  So the rules let a port in on each local
 * link that exists, then drop it from everywhere else, mobile data's
 * interface included, which comes and goes.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <netutils/netlib.h>

#ifdef CONFIG_NET_IPTABLES
#  include <nuttx/net/netfilter/ip_tables.h>
#  include <nuttx/net/netfilter/x_tables.h>
#endif

#include "netstack.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: netstack_route
 *
 * Description:
 *   The default route (0.0.0.0/0): every one deleted, since a table may
 *   hold the same route twice and the first wins, then the new one added.
 *
 ****************************************************************************/

static int netstack_route(FAR void *arg, uint32_t router)
{
#ifdef CONFIG_NET_ROUTE
  struct sockaddr_in target;
  struct sockaddr_in netmask;
  struct sockaddr_in via;
  int ret = OK;
  int sd;

  sd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sd < 0)
    {
      return -errno;
    }

  memset(&target, 0, sizeof(target));
  target.sin_family = AF_INET;
  netmask = target;
  via     = target;
  via.sin_addr.s_addr = router;

  while (delroute(sd, &target, &netmask, sizeof(target)) >= 0)
    {
    }

  if (router != 0 &&
      addroute(sd, &target, &netmask, &via, sizeof(target)) < 0)
    {
      ret = -errno;
    }

  close(sd);
  return ret;
#else
  return -ENOSYS;
#endif
}

/****************************************************************************
 * Name: netstack_resolver
 *
 * Description:
 *   NuttX's one name server: the old one deleted, which drops the answers
 *   kept from it, then the new one added.
 *
 ****************************************************************************/

static int netstack_resolver(FAR void *arg, uint32_t server)
{
  struct in_addr addr;
  int ret;

  while (netlib_del_ipv4dnsaddr_by_index(0) >= 0)
    {
    }

  if (server == 0)
    {
      return OK;
    }

  addr.s_addr = server;
  ret = netlib_set_ipv4dnsaddr(&addr);
  return ret < 0 ? ret : OK;
}

/****************************************************************************
 * Name: netstack_rule
 *
 * Description:
 *   A rule of the input chain: TCP to port, from link (NULL: any), let in
 *   or dropped.
 *
 ****************************************************************************/

#if defined(CONFIG_NET_IPTABLES) && defined(CONFIG_NET_IPFILTER)
static int netstack_rule(FAR struct ipt_replace **repl,
                         FAR const char *link, uint16_t port, bool accept)
{
  FAR struct ipt_entry *entry;
  FAR struct xt_tcp *tcp;
  int ret;

  entry = netlib_ipt_filter_entry(XT_STANDARD_TARGET,
                                  accept ? -NF_ACCEPT - 1 : -NF_DROP - 1,
                                  IPPROTO_TCP);
  if (entry == NULL)
    {
      return -ENOMEM;
    }

  tcp          = (FAR struct xt_tcp *)(IPT_MATCH(entry) + 1);
  tcp->spts[0] = 0;
  tcp->spts[1] = 65535;
  tcp->dpts[0] = port;
  tcp->dpts[1] = port;
  entry->ip.proto = IPPROTO_TCP;

  ret = netlib_ipt_fillifname(entry, link, NULL);
  if (ret >= 0)
    {
      ret = netlib_ipt_append(repl, entry, NF_INET_LOCAL_IN);
    }

  free(entry);
  return ret;
}
#endif

/****************************************************************************
 * Name: netstack_guard
 ****************************************************************************/

static int netstack_guard(FAR void *arg,
                          FAR const char (*links)[NETWORK_IFNAME],
                          int nlinks, FAR const uint16_t *ports, int nports)
{
#if defined(CONFIG_NET_IPTABLES) && defined(CONFIG_NET_IPFILTER)
  FAR struct ipt_replace *repl;
  bool found[NETWORK_LINKS_MAX];
  int nfound = 0;
  int ret = OK;
  int i;
  int j;

  for (j = 0; j < nlinks && j < NETWORK_LINKS_MAX; j++)
    {
      found[j] = if_nametoindex(links[j]) != 0;
      nfound  += found[j];
    }

  repl = netlib_ipt_prepare(XT_TABLE_NAME_FILTER);
  if (repl == NULL)
    {
      return -EIO;
    }

  /* An earlier start's rules go: the input chain is Network's */

  while (ret >= 0 && repl->underflow[NF_INET_LOCAL_IN] >
                     repl->hook_entry[NF_INET_LOCAL_IN])
    {
      ret = netlib_ipt_delete(repl, NULL, NF_INET_LOCAL_IN, 1);
    }

  for (i = 0; ret >= 0 && i < nports; i++)
    {
      for (j = 0; ret >= 0 && j < nlinks && j < NETWORK_LINKS_MAX; j++)
        {
          if (found[j])
            {
              ret = netstack_rule(&repl, links[j], ports[i], true);
            }
        }

      if (ret >= 0)
        {
          ret = netstack_rule(&repl, NULL, ports[i], false);
        }
    }

  if (ret >= 0)
    {
      ret = netlib_ipt_commit(repl);
    }

  free(repl);
  return ret < 0 ? ret : nfound;
#else
  return -ENOSYS;
#endif
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct network_ops_s g_netstack_ops =
{
  .route    = netstack_route,
  .resolver = netstack_resolver,
  .guard    = netstack_guard,
};
