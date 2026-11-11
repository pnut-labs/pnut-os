/****************************************************************************
 * pnut-os/tests/unit/test_wifi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Wi-Fi (RFC 0031) with a radio of the test's: Settings and Wi-Fi in one
 * loop, and a client that turns Wi-Fi on, scans, joins, loses the network
 * and sees it joined again, leaves it, and turns Wi-Fi off; and the ways
 * that can go wrong: a radio that will not come up, a join asked for
 * while another runs or waits, Wi-Fi turned off under a scan or a join.
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

#include "pnut/settings.pnut.h"

#include "settings.h"
#include "wifi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HOME_PASS   "correct-horse"
#define CAFE        "caf\xe9"           /* Latin-1: not UTF-8 */
#define HOME_ADDR   htonl(0x0a000005)   /* 10.0.0.5 */
#define HOME_DNS    htonl(0x0a000001)   /* 10.0.0.1 */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct wifit_s;
typedef CODE bool (*wifit_step_t)(FAR struct wifit_s *t);

struct wifit_s
{
  FAR struct pnut_loop_s *loop;
  wifit_step_t steps;             /* The scenario: true for the next */
  int last;                       /* Its steps */
  struct settings_s settings;
  struct wifi_s wifi;
  struct pnut_module_s module;
  struct pnut_wifi_client_s client;
  struct pnut_settings_client_s other;
  struct pnut_wifi_info_reader_s reader;
  char dir[64];
  char sdir[80];
  int step;
  int ticks;                      /* Since the step began */
  bool waiting;                   /* A call not answered yet */
  bool jwaiting;                  /* A join not answered yet */
  int jstatus;                    /* The last join's answer */
  uint32_t jcode;
  uint16_t sno;                   /* The scan's stream */
  pnut_wifi_info_t state;         /* The topic's last */
  int found;                      /* Networks the scan streamed */
  bool scan_over;
  int status;                     /* The last answer's */
  uint32_t code;
  pnut_wifi_known_networks_t known;   /* Known's last answer */
  int n;                              /* A scenario's own count */
  int marks;                          /* Joins counted at a mark */

  /* The fake radio's: written on the worker, read once it is done */

  volatile int ups;
  volatile int downs;
  volatile int joins;
  volatile bool up;
  volatile bool linked;
  volatile bool up_fails;         /* The radio will not come up */
  volatile int join_ms;           /* A join takes this long */
  volatile int scan_ms;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The fake radio */

static int fake_up(FAR void *arg)
{
  FAR struct wifit_s *t = arg;

  t->ups++;
  if (t->up_fails)
    {
      return -EIO;
    }

  t->up = true;
  return OK;
}

static void fake_down(FAR void *arg)
{
  FAR struct wifit_s *t = arg;

  t->downs++;
  t->up     = false;
  t->linked = false;
}

/* What the fake radio finds and lets in, besides home and cafe: set by a
 * test before its run
 */

static struct wifi_found_s g_nets[WIFI_SCAN_MAX];
static int g_nnets;
static bool g_open_ok;            /* Open networks let the device in */
static bool g_refuse_all;         /* Every join refused */

static int fake_scan(FAR void *arg, FAR struct wifi_found_s *out, int max)
{
  FAR struct wifit_s *t = arg;
  int i;

  assert_true(t->up);
  assert_true(max >= 2 + g_nnets);
  usleep(t->scan_ms * 1000);
  memset(out, 0, 2 * sizeof(*out));
  strlcpy(out[0].name, "home", sizeof(out[0].name));
  out[0].signal  = -50;
  out[0].secured = true;
  strlcpy(out[1].name, "cafe", sizeof(out[1].name));
  out[1].signal  = -70;
  for (i = 0; i < g_nnets; i++)
    {
      out[2 + i] = g_nets[i];
    }

  return 2 + g_nnets;
}

static int fake_join(FAR void *arg, FAR const char *name,
                     FAR const char *passphrase, FAR int16_t *signal)
{
  FAR struct wifit_s *t = arg;

  t->joins++;
  assert_true(t->up);
  usleep(t->join_ms * 1000);
  if (g_refuse_all)
    {
      return -ECONNREFUSED;
    }

  if (g_open_ok && passphrase[0] == '\0' && strcmp(name, "home") != 0)
    {
      *signal   = -60;
      t->linked = true;
      return OK;
    }

  if (strcmp(name, "home") != 0)
    {
      return -ENOENT;
    }

  if (strcmp(passphrase, HOME_PASS) != 0)
    {
      return -ECONNREFUSED;
    }

  *signal   = -52;
  t->linked = true;
  return OK;
}

static int fake_address(FAR void *arg, FAR struct wifi_lease_s *lease)
{
  lease->addr   = HOME_ADDR;
  lease->router = HOME_DNS;
  lease->dns[1] = HOME_DNS;       /* The first none: left out */
  return OK;
}

static bool fake_linked(FAR void *arg, FAR int16_t *signal)
{
  FAR struct wifit_s *t = arg;

  *signal = -55;
  return t->linked;
}

static const struct wifi_ops_s g_fake =
{
  .up      = fake_up,
  .down    = fake_down,
  .scan    = fake_scan,
  .join    = fake_join,
  .address = fake_address,
  .linked  = fake_linked,
};

/* The client's answers */

static void wifit_set_done(FAR struct pnut_settings_client_s *client,
                           int status, FAR const pnut_settings_reply_t *out,
                           FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct wifit_s *t = arg;

  assert_int_equal(status, PNUT_STATUS_OK);
  t->waiting = false;
}

static void wifit_enable(FAR struct wifit_s *t, bool on)
{
  pnut_settings_set_request_t req;

  memset(&req, 0, sizeof(req));
  strlcpy(req.owner, PNUT_WIFI_SETTINGS_OWNER, sizeof(req.owner));
  strlcpy(req.key, "enabled", sizeof(req.key));
  req.has_value               = true;
  req.value.which_value       = PNUT_SETTING_VALUE_BOOLEAN_TAG;
  req.value.value.boolean     = on;
  t->waiting = true;
  assert_int_equal(pnut_settings_set(&t->other, &req, 0, wifit_set_done,
                                     t), 0);
}

static void wifit_opened(FAR struct pnut_wifi_client_s *client, int status,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  assert_int_equal(status, PNUT_STATUS_OK);
}

static void wifit_item(FAR struct pnut_wifi_client_s *client, int status,
                       FAR const pnut_wifi_network_t *item, FAR void *arg)
{
  FAR struct wifit_s *t = arg;

  if (status != PNUT_STATUS_OK)
    {
      assert_int_equal(status, PNUT_STATUS_ENDED);
      t->scan_over = true;
      return;
    }

  if (t->found == 0)
    {
      assert_string_equal(item->name, "home");
      assert_int_equal(item->signal, -50);
      assert_int_equal(item->security,
                       PNUT_WIFI_SECURITY_WIFI_SECURITY_SECURED);
    }

  t->found++;
}

static void wifit_replied(FAR struct pnut_wifi_client_s *client, int status,
                          FAR const pnut_wifi_reply_t *out,
                          FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct wifit_s *t = arg;

  t->status  = status;
  t->code    = err != NULL ? err->code : 0;
  t->waiting = false;
}

static void wifit_join_replied(FAR struct pnut_wifi_client_s *client,
                               int status, FAR const pnut_wifi_reply_t *out,
                               FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct wifit_s *t = arg;

  t->jstatus  = status;
  t->jcode    = err != NULL ? err->code : 0;
  t->jwaiting = false;
}

static void wifit_join(FAR struct wifit_s *t, FAR const char *pass)
{
  pnut_wifi_join_request_t in;

  memset(&in, 0, sizeof(in));
  strlcpy(in.name, "home", sizeof(in.name));
  strlcpy(in.passphrase, pass, sizeof(in.passphrase));
  t->jwaiting = true;
  assert_int_equal(pnut_wifi_join(&t->client, &in, 0, wifit_join_replied,
                                  t), 0);
}

static void wifit_join_to(FAR struct wifit_s *t, FAR const char *name,
                          FAR const char *pass)
{
  pnut_wifi_join_request_t in;

  memset(&in, 0, sizeof(in));
  strlcpy(in.name, name, sizeof(in.name));
  strlcpy(in.passphrase, pass, sizeof(in.passphrase));
  t->jwaiting = true;
  assert_int_equal(pnut_wifi_join(&t->client, &in, 0, wifit_join_replied,
                                  t), 0);
}

static void wifit_known_replied(FAR struct pnut_wifi_client_s *client,
                                int status,
                                FAR const pnut_wifi_known_networks_t *out,
                                FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct wifit_s *t = arg;

  t->status = status;
  memset(&t->known, 0, sizeof(t->known));
  if (out != NULL)
    {
      t->known = *out;
    }

  t->waiting = false;
}

static void wifit_known(FAR struct wifit_s *t)
{
  pnut_wifi_known_request_t in;

  memset(&in, 0, sizeof(in));
  t->waiting = true;
  assert_int_equal(pnut_wifi_known(&t->client, &in, 0, wifit_known_replied,
                                   t), 0);
}

static void wifit_forget(FAR struct wifit_s *t, FAR const char *name,
                         bool secured)
{
  pnut_wifi_forget_request_t in;

  memset(&in, 0, sizeof(in));
  strlcpy(in.name, name, sizeof(in.name));
  in.security = secured ? PNUT_WIFI_SECURITY_WIFI_SECURITY_SECURED :
                PNUT_WIFI_SECURITY_WIFI_SECURITY_OPEN;
  t->waiting = true;
  assert_int_equal(pnut_wifi_forget(&t->client, &in, 0, wifit_replied, t),
                   0);
}

/* A slot as Settings holds it: its name's text, security and passphrase */

static void wifit_slot(FAR struct wifit_s *t, int i, FAR char *net,
                       FAR char *sec, FAR char *psk)
{
  pnut_setting_value_t v;
  uint32_t code;
  char key[16];

  snprintf(key, sizeof(key), "net.%d", i);
  assert_int_equal(settings_store_get(&t->settings.store, "wifi", key, true,
                                      &v, &code), PNUT_STATUS_OK);
  strcpy(net, v.value.text);
  snprintf(key, sizeof(key), "sec.%d", i);
  assert_int_equal(settings_store_get(&t->settings.store, "wifi", key, true,
                                      &v, &code), PNUT_STATUS_OK);
  strcpy(sec, v.value.text);
  snprintf(key, sizeof(key), "psk.%d", i);
  assert_int_equal(settings_store_get_secret(&t->settings.store, "wifi",
                                             key, psk, 65, &code),
                   PNUT_STATUS_OK);
}

/* Joined, and the slots written */

static bool wifit_joined(FAR struct wifit_s *t, FAR const char *name)
{
  return !t->jwaiting && t->state.state == PNUT_WIFI_INFO_STATE_JOINED &&
         strcmp(t->state.name, name) == 0 && !t->wifi.saving;
}

static void wifit_leave(FAR struct wifit_s *t)
{
  pnut_wifi_leave_request_t leave;

  memset(&leave, 0, sizeof(leave));
  t->waiting = true;
  assert_int_equal(pnut_wifi_leave(&t->client, &leave, 0, wifit_replied,
                                   t), 0);
}

static void wifit_scan(FAR struct wifit_s *t, uint16_t credit)
{
  pnut_wifi_scan_request_t scan;

  memset(&scan, 0, sizeof(scan));
  t->found     = 0;
  t->scan_over = false;
  assert_int_equal(pnut_wifi_scan(&t->client, &scan, credit, 0,
                                  wifit_opened, wifit_item, t, &t->sno),
                   0);
}

/* The whole way: on, a scan, a wrong passphrase, the right one, the
 * network lost and joined again, left, off
 */

static bool wifit_flow(FAR struct wifit_s *t)
{
  if (t->waiting || t->jwaiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:                     /* Off at first: turned on */
        assert_int_equal(t->state.state, PNUT_WIFI_INFO_STATE_OFF);
        wifit_enable(t, true);
        break;

      case 1:                     /* On: a scan */
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        assert_true(t->up);
        wifit_scan(t, 4);
        break;

      case 2:                     /* Both networks: a wrong passphrase */
        if (!t->scan_over)
          {
            return false;
          }

        assert_int_equal(t->found, 2);
        wifit_join(t, "not-the-one");
        break;

      case 3:                     /* Refused: the right one */
        assert_int_equal(t->jstatus, PNUT_STATUS_UNAVAILABLE);
        assert_int_equal(t->jcode, PNUT_WIFI_ERROR_CODE_REFUSED);
        assert_int_equal(t->state.failure, PNUT_WIFI_ERROR_CODE_REFUSED);
        wifit_join(t, HOME_PASS);
        break;

      case 4:                     /* Joined, with an address */
        assert_int_equal(t->jstatus, PNUT_STATUS_OK);
        if (t->state.state != PNUT_WIFI_INFO_STATE_JOINED)
          {
            return false;
          }

        assert_string_equal(t->state.name, "home");
        assert_int_equal(t->state.address, HOME_ADDR);
        assert_int_equal(t->state.dns_count, 1);
        assert_int_equal(t->state.dns[0], HOME_DNS);
        assert_int_equal(t->state.router, HOME_DNS);
        assert_int_equal(t->joins, 2);
        t->linked = false;        /* The network lost */
        break;

      case 5:                     /* Lost, and joined again on its own */
        if (t->joins < 3 || t->state.state != PNUT_WIFI_INFO_STATE_JOINED)
          {
            return false;
          }

        wifit_leave(t);
        break;

      case 6:                     /* Left: on, no network */
        assert_int_equal(t->status, PNUT_STATUS_OK);
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        assert_int_equal(t->state.address, 0);
        assert_int_equal(t->state.router, 0);
        assert_int_equal(t->state.dns_count, 0);

        wifit_enable(t, false);
        break;

      case 7:                     /* Off, the radio down */
        if (t->state.state != PNUT_WIFI_INFO_STATE_OFF || t->up)
          {
            return false;
          }

        break;
    }

  return true;
}

/* A radio that will not come up: said, and not tried again and again;
 * off and on, it is
 */

static bool wifit_no_radio(FAR struct wifit_s *t)
{
  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        t->up_fails = true;
        wifit_enable(t, true);
        break;

      case 1:                     /* Off, the radio's failure said */
        if (t->state.failure != PNUT_WIFI_ERROR_CODE_NO_RADIO)
          {
            return false;
          }

        assert_int_equal(t->state.state, PNUT_WIFI_INFO_STATE_OFF);
        break;

      case 2:                     /* A while on: tried once only */
        if (t->ticks < 10)
          {
            return false;
          }

        assert_int_equal(t->ups, 1);
        t->up_fails = false;
        wifit_enable(t, false);
        break;

      case 3:
        wifit_enable(t, true);
        break;

      case 4:                     /* Off and on: up */
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        assert_int_equal(t->state.failure, PNUT_WIFI_ERROR_CODE_NONE);
        assert_int_equal(t->ups, 2);
        break;
    }

  return true;
}

/* One join at a time: one asked for while a lost network is joined
 * again is refused, busy; one waiting behind a scan, then left, is told
 * so, and the next join goes ahead
 */

static bool wifit_busy(FAR struct wifit_s *t)
{
  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        wifit_enable(t, true);
        break;

      case 1:
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        wifit_join(t, HOME_PASS);
        break;

      case 2:                     /* Joined: lost, its join made slow */
        if (t->jwaiting || t->state.state != PNUT_WIFI_INFO_STATE_JOINED)
          {
            return false;
          }

        assert_int_equal(t->jstatus, PNUT_STATUS_OK);
        t->join_ms = 300;
        t->linked  = false;
        break;

      case 3:                     /* Joined again on its own: a join now */
        if (t->joins < 2 || t->state.state != PNUT_WIFI_INFO_STATE_JOINING)
          {
            return false;
          }

        wifit_join(t, HOME_PASS);
        break;

      case 4:                     /* Busy; the other goes on */
        if (t->jwaiting)
          {
            return false;
          }

        assert_int_equal(t->jstatus, PNUT_STATUS_BUSY);
        if (t->state.state != PNUT_WIFI_INFO_STATE_JOINED)
          {
            return false;
          }

        assert_string_equal(t->state.name, "home");
        t->join_ms = 0;
        t->scan_ms = 300;
        wifit_scan(t, 4);
        break;

      case 5:                     /* A join behind the scan, then left */
        if (t->ticks < 2)
          {
            return false;
          }

        wifit_join(t, HOME_PASS);
        wifit_leave(t);
        break;

      case 6:                     /* The join told it was left */
        if (t->jwaiting || !t->scan_over)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_int_equal(t->jstatus, PNUT_STATUS_UNAVAILABLE);
        assert_int_equal(t->jcode, PNUT_WIFI_ERROR_CODE_CANCELLED);
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON || !t->up)
          {
            return false;
          }

        t->scan_ms = 0;
        wifit_join(t, HOME_PASS);
        break;

      case 7:                     /* The next join is not busy */
        if (t->jwaiting)
          {
            return false;
          }

        assert_int_equal(t->jstatus, PNUT_STATUS_OK);
        assert_int_equal(t->state.state, PNUT_WIFI_INFO_STATE_JOINED);
        break;
    }

  return true;
}

/* Wi-Fi turned off under a scan whose caller has not taken all of it:
 * the radio stays up for it, and goes down once it is taken; turned off
 * under a join: the join told so, the radio down
 */

static bool wifit_off(FAR struct wifit_s *t)
{
  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        wifit_enable(t, true);
        break;

      case 1:                     /* On: a scan, credit for one */
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        wifit_scan(t, 1);
        break;

      case 2:                     /* One network: off */
        if (t->found < 1)
          {
            return false;
          }

        wifit_enable(t, false);
        break;

      case 3:                     /* Up while the stream is open, off */
        if (t->ticks < 5)
          {
            return false;
          }

        assert_true(t->up);
        assert_false(t->scan_over);
        assert_int_equal(t->state.state, PNUT_WIFI_INFO_STATE_OFF);
        assert_int_equal(pnut_wifi_scan_credit(&t->client, t->sno, 4), 0);
        break;

      case 4:                     /* Taken: down, off */
        if (!t->scan_over || t->up ||
            t->state.state != PNUT_WIFI_INFO_STATE_OFF)
          {
            return false;
          }

        assert_int_equal(t->found, 2);
        wifit_enable(t, true);
        break;

      case 5:                     /* On: a slow join */
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        t->join_ms = 300;
        wifit_join(t, HOME_PASS);
        break;

      case 6:                     /* Joining: off */
        if (t->state.state != PNUT_WIFI_INFO_STATE_JOINING)
          {
            return false;
          }

        wifit_enable(t, false);
        break;

      case 7:                     /* The join told Wi-Fi went off */
        if (t->jwaiting || t->up ||
            t->state.state != PNUT_WIFI_INFO_STATE_OFF)
          {
            return false;
          }

        assert_int_equal(t->jstatus, PNUT_STATUS_UNAVAILABLE);
        assert_int_equal(t->jcode, PNUT_WIFI_ERROR_CODE_OFF);
        break;
    }

  return true;
}

/* Joined, Wi-Fi off and on again while a scan holds the radio: the
 * network is still left, and the radio taken down and up again
 */

static bool wifit_off_on(FAR struct wifit_s *t)
{
  static int downs;

  if (t->waiting || t->jwaiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:                     /* Off at first: turned on */
        wifit_enable(t, true);
        break;

      case 1:                     /* On: joined */
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        wifit_join(t, HOME_PASS);
        break;

      case 2:                     /* Joined: a slow scan */
        if (t->state.state != PNUT_WIFI_INFO_STATE_JOINED)
          {
            return false;
          }

        t->scan_ms = 300;
        wifit_scan(t, 4);
        break;

      case 3:                     /* Scanning: off */
        if (t->ticks < 5)
          {
            return false;
          }

        assert_false(t->scan_over);
        downs = t->downs;
        wifit_enable(t, false);
        break;

      case 4:                     /* Off at once: on again, mid-scan */
        if (t->state.state != PNUT_WIFI_INFO_STATE_OFF)
          {
            return false;
          }

        assert_false(t->scan_over);
        wifit_enable(t, true);
        break;

      case 5:                     /* Left: down, up, on */
        if (!t->scan_over || t->downs == downs || !t->up ||
            t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        assert_false(t->linked);
        t->scan_ms = 0;
        break;
    }

  return true;
}

/* Each step run once the one before has what it waited for */

/* Known networks (RFC 0031): a network joined is known, its passphrase a
 * secret in Settings, gone from Wi-Fi's memory; after a restart it is
 * joined on its own; forgotten, it is left and its slot freed
 */

static bool wifit_known_first(FAR struct wifit_s *t)
{
  char net[256];
  char sec[16];
  char psk[65];

  if (t->waiting || t->jwaiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        wifit_enable(t, true);
        break;

      case 1:
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        wifit_join(t, HOME_PASS);
        break;

      case 2:
        if (!wifit_joined(t, "home"))
          {
            return false;
          }

        assert_int_equal(t->jstatus, PNUT_STATUS_OK);
        wifit_known(t);
        break;

      case 3:
        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_int_equal(t->known.networks_count, 1);
        assert_string_equal(t->known.networks[0].name, "home");
        assert_int_equal(t->known.networks[0].security,
                         PNUT_WIFI_SECURITY_WIFI_SECURITY_SECURED);
        assert_false(t->known.networks[0].refused);
        wifit_slot(t, 0, net, sec, psk);
        assert_string_equal(net, "home");
        assert_string_equal(sec, "secured");
        assert_string_equal(psk, HOME_PASS);
        wifit_slot(t, 1, net, sec, psk);
        assert_string_equal(net, "");
        assert_string_equal(psk, "");

        /* Nowhere in Wi-Fi's memory */

        assert_string_equal(t->wifi.passphrase, "");
        assert_false(t->wifi.known[0].pending);
        assert_string_equal(t->wifi.known[0].pass, "");
        assert_string_equal(t->wifi.work.passphrase, "");
        break;
    }

  return true;
}

static bool wifit_known_again(FAR struct wifit_s *t)
{
  char net[256];
  char sec[16];
  char psk[65];

  if (t->waiting || t->jwaiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:                     /* On as it was: joined on its own */
        if (!wifit_joined(t, "home"))
          {
            return false;
          }

        assert_int_equal(t->joins, 1);
        assert_string_equal(t->wifi.passphrase, "");
        wifit_forget(t, "home", true);
        break;

      case 1:                     /* Forgotten: left, and the slot free */
        assert_int_equal(t->status, PNUT_STATUS_OK);
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON || t->wifi.saving)
          {
            return false;
          }

        wifit_slot(t, 0, net, sec, psk);
        assert_string_equal(net, "");
        assert_string_equal(psk, "");
        wifit_forget(t, "home", true);
        break;

      case 2:                     /* Not known any more */
        assert_int_equal(t->status, PNUT_STATUS_NOTFOUND);
        assert_int_equal(t->code, PNUT_WIFI_ERROR_CODE_NOT_KNOWN);
        t->marks = t->joins;
        break;

      case 3:                     /* And not joined again */
        if (t->ticks < 10)
          {
            return false;
          }

        assert_int_equal(t->joins, t->marks);
        assert_int_equal(t->state.state, PNUT_WIFI_INFO_STATE_ON);
        break;
    }

  return true;
}

/* Last used first: a network joined again moves to the first slot, the
 * others down, each slot keeping its own network's passphrase
 */

static bool wifit_known_order(FAR struct wifit_s *t)
{
  char net[256];
  char sec[16];
  char psk[65];

  if (t->waiting || t->jwaiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        wifit_enable(t, true);
        break;

      case 1:
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        wifit_join_to(t, "cafe", "");
        break;

      case 2:
        if (!wifit_joined(t, "cafe"))
          {
            return false;
          }

        wifit_join(t, HOME_PASS);
        break;

      case 3:
        if (!wifit_joined(t, "home"))
          {
            return false;
          }

        wifit_slot(t, 0, net, sec, psk);
        assert_string_equal(net, "home");
        assert_string_equal(psk, HOME_PASS);
        wifit_slot(t, 1, net, sec, psk);
        assert_string_equal(net, "cafe");
        assert_string_equal(sec, "open");
        wifit_join_to(t, "cafe", "");
        break;

      case 4:
        if (!wifit_joined(t, "cafe"))
          {
            return false;
          }

        wifit_slot(t, 0, net, sec, psk);
        assert_string_equal(net, "cafe");
        assert_string_equal(sec, "open");
        assert_string_equal(psk, "");
        wifit_slot(t, 1, net, sec, psk);
        assert_string_equal(net, "home");
        assert_string_equal(sec, "secured");
        assert_string_equal(psk, HOME_PASS);
        wifit_known(t);
        break;

      case 5:
        assert_int_equal(t->known.networks_count, 2);
        assert_string_equal(t->known.networks[0].name, "cafe");
        assert_string_equal(t->known.networks[1].name, "home");
        break;
    }

  return true;
}

/* A join the owner asked for that fails: a new network not kept; a known
 * one's passphrase not replaced, the network not joined on its own until
 * the owner joins it again, with its own passphrase then
 */

static bool wifit_known_refused(FAR struct wifit_s *t)
{
  char net[256];
  char sec[16];
  char psk[65];

  if (t->waiting || t->jwaiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        wifit_enable(t, true);
        break;

      case 1:
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        wifit_join(t, "wrong-one-1");
        break;

      case 2:
        assert_int_equal(t->jcode, PNUT_WIFI_ERROR_CODE_REFUSED);
        wifit_known(t);
        break;

      case 3:
        assert_int_equal(t->known.networks_count, 0);
        wifit_join(t, HOME_PASS);
        break;

      case 4:
        if (!wifit_joined(t, "home"))
          {
            return false;
          }

        wifit_join(t, "wrong-one-2");
        break;

      case 5:
        assert_int_equal(t->jcode, PNUT_WIFI_ERROR_CODE_REFUSED);
        if (t->wifi.saving)
          {
            return false;
          }

        wifit_slot(t, 0, net, sec, psk);
        assert_string_equal(psk, HOME_PASS);
        wifit_known(t);
        break;

      case 6:
        assert_int_equal(t->known.networks_count, 1);
        assert_true(t->known.networks[0].refused);
        t->marks = t->joins;
        break;

      case 7:                     /* Not joined on its own */
        if (t->ticks < 10)
          {
            return false;
          }

        assert_int_equal(t->joins, t->marks);
        wifit_join(t, "");
        break;

      case 8:                     /* The owner's join: its own passphrase */
        if (!wifit_joined(t, "home"))
          {
            return false;
          }

        assert_int_equal(t->jstatus, PNUT_STATUS_OK);
        wifit_known(t);
        break;

      case 9:
        assert_false(t->known.networks[0].refused);
        break;
    }

  return true;
}

/* Left by the owner: not joined on its own until off and on */

static bool wifit_known_held(FAR struct wifit_s *t)
{
  if (t->waiting || t->jwaiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        wifit_enable(t, true);
        break;

      case 1:
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        wifit_join(t, HOME_PASS);
        break;

      case 2:
        if (!wifit_joined(t, "home"))
          {
            return false;
          }

        wifit_leave(t);
        break;

      case 3:
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        t->marks = t->joins;
        break;

      case 4:
        if (t->ticks < 10)
          {
            return false;
          }

        assert_int_equal(t->joins, t->marks);
        assert_int_equal(t->state.state, PNUT_WIFI_INFO_STATE_ON);
        wifit_enable(t, false);
        break;

      case 5:
        if (t->state.state != PNUT_WIFI_INFO_STATE_OFF || t->up)
          {
            return false;
          }

        wifit_enable(t, true);
        break;

      case 6:
        if (!wifit_joined(t, "home"))
          {
            return false;
          }

        assert_int_equal(t->joins, t->marks + 1);
        break;
    }

  return true;
}

/* A name that is not UTF-8: kept escaped, and joined by its bytes after a
 * restart
 */

static bool wifit_known_escaped(FAR struct wifit_s *t)
{
  char net[256];
  char sec[16];
  char psk[65];

  if (t->waiting || t->jwaiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        wifit_enable(t, true);
        break;

      case 1:
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        wifit_join_to(t, CAFE, "");
        break;

      case 2:
        if (!wifit_joined(t, CAFE))
          {
            return false;
          }

        wifit_slot(t, 0, net, sec, psk);
        assert_string_equal(net, "caf\\xe9");
        assert_string_equal(sec, "open");
        break;
    }

  return true;
}

static bool wifit_known_escaped_again(FAR struct wifit_s *t)
{
  if (!wifit_joined(t, CAFE))
    {
      return false;
    }

  assert_int_equal(t->joins, 1);
  return true;
}

/* A seventeenth network forgets the one used longest ago */

static bool wifit_known_full(FAR struct wifit_s *t)
{
  char name[16];
  char net[256];
  char sec[16];
  char psk[65];

  if (t->waiting || t->jwaiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        wifit_enable(t, true);
        break;

      case 1:                     /* n0 to n16, one after another */
        if (t->state.state == PNUT_WIFI_INFO_STATE_OFF ||
            (t->n > 0 &&
             (snprintf(name, sizeof(name), "n%d", t->n - 1),
              !wifit_joined(t, name))))
          {
            return false;
          }

        if (t->n == 17)
          {
            wifit_known(t);
            break;
          }

        snprintf(name, sizeof(name), "n%d", t->n++);
        wifit_join_to(t, name, "");
        return false;

      case 2:
        assert_int_equal(t->known.networks_count, 16);
        assert_string_equal(t->known.networks[0].name, "n16");
        assert_string_equal(t->known.networks[15].name, "n1");
        wifit_slot(t, 15, net, sec, psk);
        assert_string_equal(net, "n1");
        break;
    }

  return true;
}

/* A known network whose joins fail on their own: tried less and less
 * often, not over and over
 */

static bool wifit_known_backoff(FAR struct wifit_s *t)
{
  if (t->waiting || t->jwaiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        wifit_enable(t, true);
        break;

      case 1:
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        wifit_join(t, HOME_PASS);
        break;

      case 2:
        if (!wifit_joined(t, "home"))
          {
            return false;
          }

        g_refuse_all = true;
        t->linked    = false;     /* Lost: joined again, and refused */
        t->marks     = t->joins;
        break;

      case 3:                     /* 400 ms: 50, 100, 200 ms apart */
        if (t->ticks < 20)
          {
            return false;
          }

        assert_true(t->joins - t->marks >= 2);
        assert_true(t->joins - t->marks <= 5);
        assert_true(t->wifi.known[0].fails >= 2);
        g_refuse_all = false;
        break;
    }

  return true;
}

/* Forgotten while its join waits (its passphrase read, the worker busy
 * with a scan): the join is dropped, and the network stays forgotten
 */

static bool wifit_known_forget_waiting(FAR struct wifit_s *t)
{
  switch (t->step)
    {
      case 0:
        wifit_enable(t, true);
        break;

      case 1:
        if (t->waiting || t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        wifit_join(t, HOME_PASS);
        break;

      case 2:
        if (!wifit_joined(t, "home"))
          {
            return false;
          }

        wifit_leave(t);
        break;

      case 3:                     /* A slow scan, and a join behind it */
        if (t->waiting || t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        t->scan_ms = 300;
        wifit_scan(t, 4);
        wifit_join(t, "");
        break;

      case 4:
        if (!t->wifi.join_wanted)
          {
            return false;
          }

        assert_true(t->wifi.working);
        t->marks = t->joins;
        wifit_forget(t, "home", true);
        break;

      case 5:                     /* The join told, and never run */
        if (t->waiting || t->jwaiting || !t->scan_over)
          {
            return false;
          }

        assert_int_equal(t->status, PNUT_STATUS_OK);
        assert_int_equal(t->jstatus, PNUT_STATUS_UNAVAILABLE);
        assert_int_equal(t->jcode, PNUT_WIFI_ERROR_CODE_CANCELLED);
        break;

      case 6:
        if (t->ticks < 10)
          {
            return false;
          }

        assert_int_equal(t->joins, t->marks);
        assert_int_not_equal(t->state.state, PNUT_WIFI_INFO_STATE_JOINED);
        wifit_known(t);
        break;

      case 7:
        if (t->waiting)
          {
            return false;
          }

        assert_int_equal(t->known.networks_count, 0);
        break;
    }

  return true;
}

/* Lost while the owner's scan waits for credit: the look for a known
 * network waits for the scan, then joins it again
 */

static bool wifit_known_after_scan(FAR struct wifit_s *t)
{
  if (t->waiting || t->jwaiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        wifit_enable(t, true);
        break;

      case 1:
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON)
          {
            return false;
          }

        wifit_join(t, HOME_PASS);
        break;

      case 2:                     /* A scan stalled after one network */
        if (!wifit_joined(t, "home"))
          {
            return false;
          }

        wifit_scan(t, 1);
        break;

      case 3:
        if (t->found < 1 || t->wifi.working)
          {
            return false;
          }

        t->marks  = t->joins;
        t->linked = false;        /* Lost meanwhile */
        break;

      case 4:                     /* Not joined while the scan waits */
        if (t->state.state != PNUT_WIFI_INFO_STATE_ON || t->ticks < 10)
          {
            return false;
          }

        assert_int_equal(t->joins, t->marks);
        assert_int_equal(pnut_wifi_scan_credit(&t->client, t->sno, 4), 0);
        break;

      case 5:                     /* The scan over: joined again */
        if (!t->scan_over || !wifit_joined(t, "home"))
          {
            return false;
          }

        assert_int_equal(t->joins, t->marks + 1);
        break;
    }

  return true;
}

static void wifit_tick(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct wifit_s *t = arg;

  t->ticks++;
  if (t->step > t->last || !t->steps(t))
    {
      return;
    }

  t->ticks = 0;
  if (t->step++ == t->last)
    {
      pnut_loop_stop(loop, 0);
    }
}

static void wifit_state(FAR struct pnut_wifi_info_reader_s *reader,
                        FAR const pnut_wifi_info_t *msg, FAR void *arg)
{
  FAR struct wifit_s *t = arg;

  t->state = *msg;
}

static int wifit_start(FAR struct pnut_module_s *module,
                       FAR struct pnut_loop_s *loop)
{
  FAR struct wifit_s *t = (FAR struct wifit_s *)
    ((FAR uint8_t *)module - offsetof(struct wifit_s, module));

  assert_int_equal(pnut_wifi_info_subscribe(loop, wifit_state, t,
                                             &t->reader), 0);
  assert_int_equal(pnut_wifi_connect(loop, WIFI_SERVICE, NULL, NULL,
                                     &t->client), 0);
  assert_int_equal(pnut_settings_connect(loop, SETTINGS_SERVICE, NULL, NULL,
                                         &t->other), 0);
  assert_int_equal(pnut_timer_start(loop, 20, 20, wifit_tick, t, NULL), 0);
  pnut_module_ready(module);
  return 0;
}

static void wifit_stop(FAR struct pnut_module_s *module,
                       FAR struct pnut_loop_s *loop)
{
  FAR struct wifit_s *t = (FAR struct wifit_s *)
    ((FAR uint8_t *)module - offsetof(struct wifit_s, module));

  pnut_wifi_info_unsubscribe(&t->reader);
  pnut_wifi_disconnect(&t->client);
  pnut_settings_disconnect(&t->other);
}

static void wifit_timeout(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, -ETIMEDOUT);
}

/****************************************************************************
 * Name: wifit_run
 ****************************************************************************/

/* Who calls Settings: the network program, which runs Wi-Fi, so that
 * Wi-Fi may read its secrets (Settings refuses them when it cannot tell)
 */

static int wifit_who(FAR void *arg, pid_t pid, FAR const char **programp)
{
  *programp = "network";
  return OK;
}

static bool wifit_runs(FAR void *arg, FAR const char *program,
                       FAR const char *service)
{
  return strcmp(program, "network") == 0;
}

/* A scenario's run: from fresh settings, or from the last run's; and
 * its settings removed after, or kept for the next
 */

static void wifit_run_as(wifit_step_t steps, int last, bool fresh,
                         bool keep)
{
  static const struct settings_limits_s limits =
  {
    .owners   = 4,
    .settings = 64,
    .texts    = 40,
    .choices  = 48,
    .secrets  = 16,
  };

  FAR struct wifit_s *t = calloc(1, sizeof(*t));
  struct pnut_loop_config_s config;
  struct settings_config_s sconfig;
  struct wifi_config_s wconfig;
  char cmd[96];
  int ret;

  assert_non_null(t);
  t->steps = steps;
  t->last  = last;
  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-wifi-%d", (int)getpid());
  snprintf(t->sdir, sizeof(t->sdir), "%s/s", t->dir);
  snprintf(cmd, sizeof(cmd), "rm -rf %s", t->dir);
  if (fresh)
    {
      assert_int_equal(system(cmd), 0);
      assert_int_equal(mkdir(t->dir, 0700), 0);
      assert_int_equal(mkdir(t->sdir, 0700), 0);
    }

  pnut_loop_defaults(&config);
  config.rundir = t->dir;
  assert_int_equal(pnut_loop_create(&config, &t->loop), 0);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir    = t->sdir;
  sconfig.limits = limits;
  sconfig.delay  = 20;
  sconfig.who    = wifit_who;
  sconfig.runs   = wifit_runs;
  settings_init(&t->settings, &sconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->settings.module), 0);

  memset(&wconfig, 0, sizeof(wconfig));
  wconfig.ops      = &g_fake;
  wconfig.arg      = t;
  wconfig.link_ms  = 50;
  wconfig.again_ms = 50;
  wconfig.idle_ms  = 50;
  wifi_init(&t->wifi, &wconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->wifi.module), 0);

  t->module.name  = "test";
  t->module.start = wifit_start;
  t->module.stop  = wifit_stop;
  assert_int_equal(pnut_module_add(t->loop, &t->module), 0);

  pnut_timer_start(t->loop, 10000, 0, wifit_timeout, NULL, NULL);
  ret = pnut_loop_run(t->loop);
  pnut_loop_destroy(t->loop);
  settings_deinit(&t->settings);
  assert_int_equal(ret, 0);
  assert_int_equal(t->step, last + 1);

  if (!keep)
    {
      assert_int_equal(system(cmd), 0);
    }

  free(t);

  /* The fake radio as it was */

  g_nnets      = 0;
  g_open_ok    = false;
  g_refuse_all = false;
}

static void wifit_run(wifit_step_t steps, int last)
{
  wifit_run_as(steps, last, true, false);
}

static void test_wifi(void **state)
{
  wifit_run(wifit_flow, 7);
}

static void test_wifi_no_radio(void **state)
{
  wifit_run(wifit_no_radio, 4);
}

static void test_wifi_busy(void **state)
{
  wifit_run(wifit_busy, 7);
}

static void test_wifi_off(void **state)
{
  wifit_run(wifit_off, 7);
}

static void test_wifi_off_on(void **state)
{
  wifit_run(wifit_off_on, 5);
}

static void test_wifi_known(void **state)
{
  wifit_run_as(wifit_known_first, 3, true, true);
  wifit_run_as(wifit_known_again, 3, false, false);
}

static void test_wifi_known_order(void **state)
{
  g_open_ok = true;
  wifit_run(wifit_known_order, 5);
}

static void test_wifi_known_refused(void **state)
{
  wifit_run(wifit_known_refused, 9);
}

static void test_wifi_known_held(void **state)
{
  wifit_run(wifit_known_held, 6);
}

static void test_wifi_known_escaped(void **state)
{
  memset(g_nets, 0, sizeof(g_nets));
  strlcpy(g_nets[0].name, CAFE, sizeof(g_nets[0].name));
  g_nets[0].signal = -65;
  g_nnets   = 1;
  g_open_ok = true;
  wifit_run_as(wifit_known_escaped, 2, true, true);

  strlcpy(g_nets[0].name, CAFE, sizeof(g_nets[0].name));
  g_nets[0].signal = -65;
  g_nnets   = 1;
  g_open_ok = true;
  wifit_run_as(wifit_known_escaped_again, 0, false, false);
}

static void test_wifi_known_full(void **state)
{
  int i;

  memset(g_nets, 0, sizeof(g_nets));
  for (i = 0; i < 17; i++)
    {
      snprintf(g_nets[i].name, sizeof(g_nets[i].name), "n%d", i);
      g_nets[i].signal = -40 - i;
    }

  g_nnets   = 17;
  g_open_ok = true;
  wifit_run(wifit_known_full, 2);
}

static void test_wifi_known_backoff(void **state)
{
  wifit_run(wifit_known_backoff, 3);
}

static void test_wifi_known_forget_waiting(void **state)
{
  wifit_run(wifit_known_forget_waiting, 7);
}

static void test_wifi_known_after_scan(void **state)
{
  wifit_run(wifit_known_after_scan, 5);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_wifi),
    cmocka_unit_test(test_wifi_no_radio),
    cmocka_unit_test(test_wifi_busy),
    cmocka_unit_test(test_wifi_off),
    cmocka_unit_test(test_wifi_off_on),
    cmocka_unit_test(test_wifi_known),
    cmocka_unit_test(test_wifi_known_order),
    cmocka_unit_test(test_wifi_known_refused),
    cmocka_unit_test(test_wifi_known_held),
    cmocka_unit_test(test_wifi_known_escaped),
    cmocka_unit_test(test_wifi_known_full),
    cmocka_unit_test(test_wifi_known_backoff),
    cmocka_unit_test(test_wifi_known_forget_waiting),
    cmocka_unit_test(test_wifi_known_after_scan),
  };

  return cmocka_run_group_tests_name("wifi", tests, NULL, NULL);
}
