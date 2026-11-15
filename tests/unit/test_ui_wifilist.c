/****************************************************************************
 * pnut-os/tests/unit/test_ui_wifilist.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The Wi-Fi screen's list of networks in reach (wifilist.c): one entry a
 * network, the strongest first, the known ones marked, and redrawn at most
 * every 5 s while the scan goes on.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "wifilist.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void add(struct wifilist_s *l, const char *name, int32_t dbm,
                bool secured)
{
  pnut_wifi_network_t n;

  memset(&n, 0, sizeof(n));
  strcpy(n.name, name);
  n.signal   = dbm;
  n.security = secured ? PNUT_WIFI_SECURITY_WIFI_SECURITY_SECURED :
                         PNUT_WIFI_SECURITY_WIFI_SECURITY_OPEN;
  wifilist_add(l, &n);
}

static void test_wifilist_order(void **state)
{
  pnut_wifi_known_networks_t known;
  struct wifilist_s l;

  wifilist_start(&l, 1000);
  add(&l, "Cafe", -80, false);
  add(&l, "Home", -55, true);
  add(&l, "Cafe", -70, false);     /* Another access point: the stronger */
  add(&l, "Cafe", -70, true);      /* Another network of the same name */
  add(&l, "", -40, true);          /* Hidden: left out */

  assert_int_equal(l.count, 3);
  assert_string_equal(l.nets[0].name, "Home");
  assert_string_equal(l.nets[1].name, "Cafe");
  assert_int_equal(l.nets[1].signal, -70);
  assert_string_equal(l.nets[2].name, "Cafe");

  memset(&known, 0, sizeof(known));
  known.networks_count = 1;
  strcpy(known.networks[0].name, "Home");
  known.networks[0].security = PNUT_WIFI_SECURITY_WIFI_SECURITY_SECURED;
  wifilist_known(&l, &known);
  assert_true(l.known[0]);
  assert_false(l.known[1]);
  assert_false(l.known[2]);
}

static void test_wifilist_full(void **state)
{
  struct wifilist_s l;
  char name[8];
  int i;

  /* More than it keeps: the weakest go */

  wifilist_start(&l, 0);
  for (i = 0; i < WIFILIST_MAX + 4; i++)
    {
      snprintf(name, sizeof(name), "n%d", i);
      add(&l, name, -90 + i, true);
    }

  assert_int_equal(l.count, WIFILIST_MAX);
  assert_int_equal(l.nets[0].signal, -90 + WIFILIST_MAX + 3);
  assert_int_equal(l.nets[WIFILIST_MAX - 1].signal, -86);
}

static void test_wifilist_due(void **state)
{
  struct wifilist_s l;

  /* While scanning: at most every 5 s; at once when it ends */

  wifilist_start(&l, 1000);
  assert_false(wifilist_due(&l, 1000));

  /* The empty list drawn as the scan begins: the first results still at
   * once
   */

  wifilist_shown(&l, 1000);
  add(&l, "Home", -55, true);
  assert_true(wifilist_due(&l, 1000));
  wifilist_shown(&l, 1000);
  add(&l, "Cafe", -70, false);
  assert_false(wifilist_due(&l, 5999));
  assert_true(wifilist_due(&l, 6000));
  wifilist_shown(&l, 6000);
  assert_false(wifilist_due(&l, 7000));
  add(&l, "Office", -60, true);
  wifilist_end(&l);
  assert_true(wifilist_due(&l, 7000));
  wifilist_shown(&l, 7000);
  assert_false(wifilist_due(&l, 7001));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_wifilist_order),
    cmocka_unit_test(test_wifilist_full),
    cmocka_unit_test(test_wifilist_due),
  };

  return cmocka_run_group_tests_name("ui_wifilist", tests, NULL, NULL);
}
