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
#define HOME_ADDR   htonl(0x0a000005)   /* 10.0.0.5 */

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

static int fake_scan(FAR void *arg, FAR struct wifi_found_s *out, int max)
{
  FAR struct wifit_s *t = arg;

  assert_true(t->up);
  assert_true(max >= 2);
  usleep(t->scan_ms * 1000);
  memset(out, 0, 2 * sizeof(*out));
  strlcpy(out[0].name, "home", sizeof(out[0].name));
  out[0].signal  = -50;
  out[0].secured = true;
  strlcpy(out[1].name, "cafe", sizeof(out[1].name));
  out[1].signal  = -70;
  return 2;
}

static int fake_join(FAR void *arg, FAR const char *name,
                     FAR const char *passphrase, FAR int16_t *signal)
{
  FAR struct wifit_s *t = arg;

  t->joins++;
  assert_true(t->up);
  usleep(t->join_ms * 1000);
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

static int fake_address(FAR void *arg, FAR uint32_t *addr)
{
  *addr = HOME_ADDR;
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

static void wifit_run(wifit_step_t steps, int last)
{
  static const struct settings_limits_s limits =
  {
    .owners   = 4,
    .settings = 32,
    .texts    = 8,
    .choices  = 16,
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
  assert_int_equal(system(cmd), 0);
  assert_int_equal(mkdir(t->dir, 0700), 0);
  assert_int_equal(mkdir(t->sdir, 0700), 0);

  pnut_loop_defaults(&config);
  config.rundir = t->dir;
  assert_int_equal(pnut_loop_create(&config, &t->loop), 0);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir    = t->sdir;
  sconfig.limits = limits;
  sconfig.delay  = 20;
  settings_init(&t->settings, &sconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->settings.module), 0);

  memset(&wconfig, 0, sizeof(wconfig));
  wconfig.ops      = &g_fake;
  wconfig.arg      = t;
  wconfig.link_ms  = 50;
  wconfig.again_ms = 50;
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

  assert_int_equal(system(cmd), 0);
  free(t);
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
  };

  return cmocka_run_group_tests_name("wifi", tests, NULL, NULL);
}
