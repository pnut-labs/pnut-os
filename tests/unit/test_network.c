/****************************************************************************
 * pnut-os/tests/unit/test_network.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Network (RFC 0015) with a network stack of the test's: the links' state
 * published on the wifi and telephony topics as Wi-Fi and Telephony would,
 * and the link in use read back from the network topic, with the route
 * and the name server set for it; the route set again at each change of
 * a link's state or address, not at a repeated one.  Mobile data alone,
 * then Wi-Fi joining and joined without a router (not in use), and with
 * one (in use), mobile data off, Wi-Fi left (no link: no route, no name
 * server, both failing once, then set again), mobile data with no name
 * server, and a new PPP session's address.  And the local ports, kept to
 * the local links: at start, failing, then at the next news; again as
 * Wi-Fi's interface comes; failing at a change of Wi-Fi's state, then at
 * the same state again.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <arpa/inet.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "network.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MOBILE_ADDR   htonl(0x0a400001)   /* 10.64.0.1 */
#define MOBILE_DNS    htonl(0x0a400035)
#define MOBILE_DNS2   htonl(0x0a400036)
#define WIFI_ADDR     htonl(0xc0a80105)   /* 192.168.1.5 */
#define MOBILE_ADDR2  htonl(0x0a400002)
#define WIFI_DNS      htonl(0xc0a80101)
#define WIFI_ROUTER   htonl(0xc0a801fe)
#define LAST          12

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct nett_s
{
  FAR struct pnut_loop_s *loop;
  struct network_s net;
  struct pnut_module_s module;
  FAR struct pnut_publisher_s *wifi;
  FAR struct pnut_publisher_s *telephony;
  struct pnut_network_info_reader_s reader;
  pnut_network_info_t state;
  int states;                     /* Messages read */
  char dir[64];
  int step;
  int ticks;                      /* Since the step began */

  /* The fake stack's */

  int routes;                     /* Calls */
  uint32_t via;                   /* The last one's */
  bool route_fails;               /* The next call fails */
  int resolves;
  uint32_t server;                /* The last one's */
  bool resolve_fails;             /* The next call fails */
  int guards;
  bool guard_fails;               /* The next call fails */
  bool wlan0;                     /* wlan0 exists: not until Wi-Fi is on */
  char links[NETWORK_LINKS_MAX][NETWORK_IFNAME];
  int nlinks;
  uint16_t ports[NETWORK_PORTS_MAX];
  int nports;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The fake stack */

static int fake_route(FAR void *arg, uint32_t router)
{
  FAR struct nett_s *t = arg;

  t->routes++;
  if (t->route_fails)
    {
      t->route_fails = false;
      return -ENOMEM;
    }

  t->via = router;
  return 0;
}

static int fake_resolver(FAR void *arg, uint32_t server)
{
  FAR struct nett_s *t = arg;

  t->resolves++;
  if (t->resolve_fails)
    {
      t->resolve_fails = false;
      return -ENOMEM;
    }

  t->server = server;
  return 0;
}

static int fake_guard(FAR void *arg, FAR const char (*links)[NETWORK_IFNAME],
                      int nlinks, FAR const uint16_t *ports, int nports)
{
  FAR struct nett_s *t = arg;

  t->guards++;
  if (t->guard_fails)
    {
      t->guard_fails = false;
      return -ENOMEM;
    }

  assert_true(nlinks <= NETWORK_LINKS_MAX);
  assert_true(nports <= NETWORK_PORTS_MAX);
  memcpy(t->links, links, nlinks * sizeof(links[0]));
  t->nlinks = nlinks;
  memcpy(t->ports, ports, nports * sizeof(ports[0]));
  t->nports = nports;
  return t->wlan0 ? 1 : 0;        /* eth0 never */
}

static const struct network_ops_s g_fake =
{
  .route    = fake_route,
  .resolver = fake_resolver,
  .guard    = fake_guard,
};

/* The links' state, as Wi-Fi and Telephony publish it */

static void nett_wifi(FAR struct nett_s *t, uint8_t state, uint32_t addr,
                      uint32_t router, uint32_t dns)
{
  pnut_wifi_info_t msg;

  memset(&msg, 0, sizeof(msg));
  msg.state   = state;
  msg.address = addr;
  msg.router  = router;
  if (dns != 0)
    {
      msg.dns[msg.dns_count++] = dns;
    }

  assert_int_equal(pnut_wifi_info_publish(t->wifi, &msg), 0);
}

static void nett_data(FAR struct nett_s *t, uint8_t data, uint32_t addr,
                      uint32_t dns, uint32_t dns2)
{
  pnut_telephony_info_t msg;

  memset(&msg, 0, sizeof(msg));
  msg.modem        = PNUT_TELEPHONY_INFO_MODEM_ON;
  msg.data         = data;
  msg.data_address = addr;
  if (dns != 0)
    {
      msg.data_dns[msg.data_dns_count++] = dns;
    }

  if (dns2 != 0)
    {
      msg.data_dns[msg.data_dns_count++] = dns2;
    }

  assert_int_equal(pnut_telephony_info_publish(t->telephony, &msg), 0);
}

/* The scenario: true for the next step */

static bool nett_steps(FAR struct nett_s *t)
{
  switch (t->step)
    {
      case 0:                     /* Started: keeping the ports failed */
        if (t->states == 0)
          {
            return false;
          }

        assert_int_equal(t->guards, 1);
        assert_int_equal(t->nports, 0);
        assert_int_equal(t->state.link, PNUT_NETWORK_INFO_LINK_NO_LINK);
        assert_int_equal(t->resolves, 1);
        assert_int_equal(t->server, 0);
        assert_int_equal(t->routes, 1);
        assert_int_equal(t->via, 0);

        /* Mobile data connecting: not a link yet, and the ports kept,
         * tried again at the news
         */

        nett_data(t, PNUT_TELEPHONY_INFO_DATA_CONNECTING, 0, 0, 0);
        break;

      case 1:
        if (t->ticks < 3)
          {
            return false;
          }

        assert_int_equal(t->guards, 2);
        assert_int_equal(t->nlinks, 2);
        assert_string_equal(t->links[0], "wlan0");
        assert_string_equal(t->links[1], "eth0");
        assert_int_equal(t->nports, 2);
        assert_int_equal(t->ports[0], 5555);
        assert_int_equal(t->ports[1], 22);

        assert_int_equal(t->state.link, PNUT_NETWORK_INFO_LINK_NO_LINK);
        assert_int_equal(t->resolves, 1);
        assert_int_equal(t->routes, 2);
        nett_data(t, PNUT_TELEPHONY_INFO_DATA_DATA_ON, MOBILE_ADDR,
                  MOBILE_DNS, MOBILE_DNS2);
        break;

      case 2:                     /* Mobile data: through its own address,
                                   * its first name server */
        if (t->state.link != PNUT_NETWORK_INFO_LINK_MOBILE)
          {
            return false;
          }

        assert_true(t->state.metered);
        assert_int_equal(t->state.address, MOBILE_ADDR);
        assert_int_equal(t->state.dns, MOBILE_DNS);
        assert_int_equal(t->resolves, 2);
        assert_int_equal(t->server, MOBILE_DNS);
        assert_int_equal(t->routes, 3);
        assert_int_equal(t->via, MOBILE_ADDR);
        assert_int_equal(t->guards, 2);    /* Not again: kept */

        /* Wi-Fi on: its interface comes with the radio, and the ports are
         * kept to it again
         */

        t->wlan0 = true;
        nett_wifi(t, PNUT_WIFI_INFO_STATE_ON, 0, 0, 0);
        break;

      case 3:
        if (t->guards < 3)
          {
            return false;
          }

        /* The same states again (a signal's change): nothing more */

        nett_wifi(t, PNUT_WIFI_INFO_STATE_ON, 0, 0, 0);
        nett_data(t, PNUT_TELEPHONY_INFO_DATA_DATA_ON, MOBILE_ADDR,
                  MOBILE_DNS, MOBILE_DNS2);
        break;

      case 4:
        if (t->ticks < 3)
          {
            return false;
          }

        assert_int_equal(t->guards, 3);
        assert_int_equal(t->routes, 4);   /* Not at the same state again */

        /* Wi-Fi joining, with no address yet: not in use; the ports kept
         * again, since eth0 is still missing, and failing
         */

        t->guard_fails = true;
        nett_wifi(t, PNUT_WIFI_INFO_STATE_JOINING, 0, 0, 0);
        break;

      case 5:
        if (t->ticks < 3)
          {
            return false;
          }

        assert_int_equal(t->guards, 4);

        /* The same state again: tried again, since the last one failed */

        nett_wifi(t, PNUT_WIFI_INFO_STATE_JOINING, 0, 0, 0);
        break;

      case 6:
        if (t->ticks < 3)
          {
            return false;
          }

        assert_int_equal(t->state.link, PNUT_NETWORK_INFO_LINK_MOBILE);
        assert_int_equal(t->resolves, 2);
        assert_int_equal(t->guards, 5);
        assert_int_equal(t->routes, 5);
        assert_int_equal(t->via, MOBILE_ADDR);

        /* Joined, but DHCP gave no router: no way out, not in use */

        nett_wifi(t, PNUT_WIFI_INFO_STATE_JOINED, WIFI_ADDR, 0, WIFI_DNS);
        break;

      case 7:
        if (t->ticks < 3)
          {
            return false;
          }

        assert_int_equal(t->state.link, PNUT_NETWORK_INFO_LINK_MOBILE);
        assert_int_equal(t->routes, 6);
        assert_int_equal(t->resolves, 2);
        nett_wifi(t, PNUT_WIFI_INFO_STATE_JOINED, WIFI_ADDR, WIFI_ROUTER,
                  WIFI_DNS);
        break;

      case 8:                     /* Wi-Fi joined: in use, free */
        if (t->state.link != PNUT_NETWORK_INFO_LINK_WIFI)
          {
            return false;
          }

        assert_false(t->state.metered);
        assert_int_equal(t->state.address, WIFI_ADDR);
        assert_int_equal(t->state.dns, WIFI_DNS);
        assert_int_equal(t->routes, 7);
        assert_int_equal(t->via, WIFI_ROUTER);
        assert_int_equal(t->resolves, 3);
        assert_int_equal(t->server, WIFI_DNS);

        /* Mobile data off under Wi-Fi: the same link, its route set again
         * (where a writer of the route would have run)
         */

        nett_data(t, PNUT_TELEPHONY_INFO_DATA_DATA_OFF, 0, 0, 0);
        break;

      case 9:
        if (t->ticks < 3)
          {
            return false;
          }

        assert_int_equal(t->state.link, PNUT_NETWORK_INFO_LINK_WIFI);
        assert_int_equal(t->routes, 8);
        assert_int_equal(t->via, WIFI_ROUTER);
        assert_int_equal(t->resolves, 3);

        /* Wi-Fi left: no link, the route and the resolver failing once */

        t->route_fails   = true;
        t->resolve_fails = true;
        nett_wifi(t, PNUT_WIFI_INFO_STATE_ON, 0, 0, 0);
        break;

      case 10:
        if (t->state.link != PNUT_NETWORK_INFO_LINK_NO_LINK)
          {
            return false;
          }

        assert_int_equal(t->state.dns, 0);
        assert_int_equal(t->routes, 9);
        assert_int_equal(t->via, WIFI_ROUTER);    /* The failed call */
        assert_int_equal(t->resolves, 4);
        assert_int_equal(t->server, WIFI_DNS);

        /* Mobile data, no name server given: none, set this time */

        nett_data(t, PNUT_TELEPHONY_INFO_DATA_DATA_ON, MOBILE_ADDR, 0, 0);
        break;

      case 11:
        if (t->state.link != PNUT_NETWORK_INFO_LINK_MOBILE)
          {
            return false;
          }

        assert_int_equal(t->state.dns, 0);
        assert_int_equal(t->routes, 10);
        assert_int_equal(t->via, MOBILE_ADDR);
        assert_int_equal(t->resolves, 5);
        assert_int_equal(t->server, 0);

        /* A new PPP session, a new address: the route follows it */

        nett_data(t, PNUT_TELEPHONY_INFO_DATA_DATA_ON, MOBILE_ADDR2, 0, 0);
        break;

      case 12:
        if (t->state.address != MOBILE_ADDR2)
          {
            return false;
          }

        assert_int_equal(t->routes, 11);
        assert_int_equal(t->via, MOBILE_ADDR2);
        assert_int_equal(t->resolves, 5);
        break;
    }

  return true;
}

static void nett_tick(FAR struct pnut_loop_s *loop,
                      FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct nett_s *t = arg;

  t->ticks++;
  if (t->step > LAST || !nett_steps(t))
    {
      return;
    }

  t->ticks = 0;
  if (t->step++ == LAST)
    {
      pnut_loop_stop(loop, 0);
    }
}

static void nett_state(FAR struct pnut_network_info_reader_s *reader,
                       FAR const pnut_network_info_t *msg, FAR void *arg)
{
  FAR struct nett_s *t = arg;

  t->state = *msg;
  t->states++;
}

static int nett_start(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct nett_s *t = (FAR struct nett_s *)
    ((FAR uint8_t *)module - offsetof(struct nett_s, module));

  assert_int_equal(pnut_wifi_info_advertise(loop, &t->wifi), 0);
  assert_int_equal(pnut_telephony_info_advertise(loop, &t->telephony), 0);
  assert_int_equal(pnut_network_info_subscribe(loop, nett_state, t,
                                                &t->reader), 0);
  assert_int_equal(pnut_timer_start(loop, 20, 20, nett_tick, t, NULL), 0);
  pnut_module_ready(module);
  return 0;
}

static void nett_stop(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct nett_s *t = (FAR struct nett_s *)
    ((FAR uint8_t *)module - offsetof(struct nett_s, module));

  pnut_network_info_unsubscribe(&t->reader);
  pnut_topic_unadvertise(t->wifi);
  pnut_topic_unadvertise(t->telephony);
}

static void nett_timeout(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, -ETIMEDOUT);
}

/****************************************************************************
 * Name: test_network
 ****************************************************************************/

static void test_network(void **state)
{
  FAR struct nett_s *t = calloc(1, sizeof(*t));
  struct pnut_loop_config_s config;
  struct network_config_s nconfig;
  char cmd[96];
  int ret;

  assert_non_null(t);
  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-network-%d", (int)getpid());
  snprintf(cmd, sizeof(cmd), "rm -rf %s", t->dir);
  assert_int_equal(system(cmd), 0);
  assert_int_equal(mkdir(t->dir, 0700), 0);

  pnut_loop_defaults(&config);
  config.rundir = t->dir;
  assert_int_equal(pnut_loop_create(&config, &t->loop), 0);

  /* Words that are not a link or a port are left out */

  memset(&nconfig, 0, sizeof(nconfig));
  nconfig.ops         = &g_fake;
  nconfig.arg         = t;
  nconfig.local_links = " wlan0  a-name-far-too-long eth0 ";
  nconfig.local_ports = "5555 x 0 22 70000 22x";
  network_init(&t->net, &nconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->net.module), 0);
  t->guard_fails = true;          /* At start */

  t->module.name  = "test";
  t->module.start = nett_start;
  t->module.stop  = nett_stop;
  assert_int_equal(pnut_module_add(t->loop, &t->module), 0);

  assert_int_equal(pnut_timer_start(t->loop, 10000, 0, nett_timeout, NULL,
                                    NULL), 0);
  ret = pnut_loop_run(t->loop);
  pnut_loop_destroy(t->loop);
  assert_int_equal(ret, 0);
  assert_int_equal(t->step, LAST + 1);

  assert_int_equal(system(cmd), 0);
  free(t);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_network),
  };

  return cmocka_run_group_tests_name("network", tests, NULL, NULL);
}
