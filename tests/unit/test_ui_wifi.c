/****************************************************************************
 * pnut-os/tests/unit/test_ui_wifi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Settings, Wi-Fi and the join screen (scr_settings.c, scr_wifi.c,
 * scr_join.c), driven by keys and taps: Settings runs in the test's loop,
 * Wi-Fi's service is a fake the test answers for, and the test publishes
 * the topics.  Their pictures go to SCREENS_DIR, as test_ui_screens'.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/service.h>
#include <pnut/timer.h>
#include <pnut/topic.h>

#include "pnut/telephony.topics.h"
#include "pnut/time.topics.h"
#include "pnut/wifi.pnut.h"
#include "pnut/wifi.settings.h"
#include "pnut/wifi.topics.h"

#include "blocks.h"
#include "input.h"
#include "keymap.h"
#include "settings.h"
#include "text.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIDTH    240
#define HEIGHT   320
#define STRIDE   (WIDTH / 8)

/* 2026-09-22 12:07:00 UTC: 14:07 in Warsaw */

#define WIFI_NOW   1790078820
#define WIFI_RULE  "CET-1CEST,M3.5.0,M10.5.0/3"

#ifndef SCREENS_DIR
#  define SCREENS_DIR  "screens"
#endif

/* How the fake answers a join */

#define JOIN_OK       0
#define JOIN_REFUSED  1
#define JOIN_LATER    2

/* The switch row's centre: the first row under the header */

#define SWITCH_X  120
#define SWITCH_Y  (BLOCK_HEADER_H + BLOCK_ROW_H / 2)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct step_s
{
  uint32_t at;
  CODE void (*run)(void);
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct settings_limits_s g_limits =
{
  .owners   = 4,
  .settings = 128,
  .texts    = 96,
  .choices  = 64,
};

static uint8_t g_frame[STRIDE * HEIGHT];
static struct ui_s g_ui;
static struct input_s g_input;
static struct settings_s g_settings;
static struct pnut_module_s g_owners;
static struct pnut_wifi_settings_s g_cache;
static FAR struct pnut_publisher_s *g_tel;
static FAR struct pnut_publisher_s *g_wifi;
static FAR struct pnut_publisher_s *g_time;
static FAR struct pnut_loop_s *g_loop;
static time_t g_now = WIFI_NOW;

/* The fake Wi-Fi service */

static struct pnut_wifi_server_s g_server;
static bool g_serving;
static struct pnut_stream_s g_stream;
static bool g_stream_open;
static pnut_wifi_network_t g_nets[3];
static int g_nnets;
static int g_pushed;
static bool g_end;                /* End the scan once all are pushed */
static int g_scans;
static int g_joins;
static int g_knowns;
static int g_leaves;
static char g_join_name[33];
static char g_join_pass[65];
static int g_join_answer;
static struct pnut_request_s g_join_req;
static bool g_join_waiting;
static pnut_wifi_known_networks_t g_known;
static bool g_start_on;           /* wifi.enabled on before the first step */

/* The join screen's field, as the screen went */

static FAR struct block_field_s *g_field;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int fake_update(FAR void *arg, int16_t x, int16_t y, int16_t w,
                       int16_t h, bool full)
{
  return 0;
}

static const struct display_ops_s g_ops =
{
  fake_update
};

static time_t fixed_now(void)
{
  return g_now;
}

static void net(int i, FAR const char *name, int32_t dbm, bool secured)
{
  memset(&g_nets[i], 0, sizeof(g_nets[i]));
  strcpy(g_nets[i].name, name);
  g_nets[i].signal   = dbm;
  g_nets[i].security = secured ? PNUT_WIFI_SECURITY_WIFI_SECURITY_SECURED :
                                 PNUT_WIFI_SECURITY_WIFI_SECURITY_OPEN;
}

/* The fake's methods */

static void fake_push(void)
{
  while (g_stream_open && g_pushed < g_nnets)
    {
      if (pnut_wifi_scan_push(&g_server, &g_stream, &g_nets[g_pushed]) < 0)
        {
          return;
        }

      g_pushed++;
    }

  if (g_stream_open && g_end && g_pushed == g_nnets)
    {
      pnut_stream_end(&g_stream, PNUT_STATUS_ENDED);
      g_stream_open = false;
    }
}

static void fake_ready(FAR struct pnut_stream_s *stream, FAR void *arg)
{
  fake_push();
}

static void fake_closed(FAR struct pnut_stream_s *stream, int status,
                        FAR void *arg)
{
  g_stream_open = false;
}

static void fake_scan(FAR struct pnut_wifi_server_s *server,
                      FAR const struct pnut_request_s *req,
                      FAR const pnut_wifi_scan_request_t *in, FAR void *arg)
{
  g_scans++;
  g_pushed = 0;
  if (pnut_wifi_scan_accept(server, req, &g_stream, fake_ready, fake_closed,
                            NULL) == 0)
    {
      g_stream_open = true;
      fake_push();
    }
}

static void fake_join(FAR struct pnut_wifi_server_s *server,
                      FAR const struct pnut_request_s *req,
                      FAR const pnut_wifi_join_request_t *in, FAR void *arg)
{
  pnut_wifi_reply_t out;

  memset(&out, 0, sizeof(out));
  g_joins++;
  strcpy(g_join_name, in->name);
  strcpy(g_join_pass, in->passphrase);
  switch (g_join_answer)
    {
      case JOIN_OK:
        pnut_wifi_join_reply(server, req, PNUT_STATUS_OK, &out);
        break;

      case JOIN_REFUSED:
        pnut_wifi_fail(server, req, PNUT_STATUS_UNAVAILABLE,
                       PNUT_WIFI_ERROR_CODE_REFUSED, "refused");
        break;

      default:
        g_join_req     = *req;
        g_join_waiting = true;
        break;
    }
}

static void fake_leave(FAR struct pnut_wifi_server_s *server,
                       FAR const struct pnut_request_s *req,
                       FAR const pnut_wifi_leave_request_t *in,
                       FAR void *arg)
{
  pnut_wifi_reply_t out;

  memset(&out, 0, sizeof(out));
  g_leaves++;
  pnut_wifi_leave_reply(server, req, PNUT_STATUS_OK, &out);
}

static void fake_known(FAR struct pnut_wifi_server_s *server,
                       FAR const struct pnut_request_s *req,
                       FAR const pnut_wifi_known_request_t *in,
                       FAR void *arg)
{
  g_knowns++;
  pnut_wifi_known_reply(server, req, PNUT_STATUS_OK, &g_known);
}

static void fake_forget(FAR struct pnut_wifi_server_s *server,
                        FAR const struct pnut_request_s *req,
                        FAR const pnut_wifi_forget_request_t *in,
                        FAR void *arg)
{
  pnut_wifi_reply_t out;

  memset(&out, 0, sizeof(out));
  pnut_wifi_forget_reply(server, req, PNUT_STATUS_OK, &out);
}

static const struct pnut_wifi_handlers_s g_handlers =
{
  fake_scan,
  fake_join,
  fake_leave,
  fake_known,
  fake_forget
};

static void fake_serve(void)
{
  assert_int_equal(pnut_wifi_serve(g_loop, "wifi", 4, &g_handlers, NULL,
                                   &g_server), 0);
  g_serving = true;
}

static void fake_stop(void)
{
  pnut_wifi_close(&g_server);
  g_serving     = false;
  g_stream_open = false;
}

/* Wi-Fi's settings, registered as Wi-Fi does; the fake service */

static void owner_changed(FAR struct pnut_settings_cache_s *cache,
                          int index, FAR void *arg)
{
}

static int owners_start(FAR struct pnut_module_s *module,
                        FAR struct pnut_loop_s *loop)
{
  pnut_module_ready(module);
  assert_int_equal(pnut_wifi_settings_open(module, owner_changed, NULL,
                                           &g_cache), 0);
  fake_serve();
  return 0;
}

static void owners_stop(FAR struct pnut_module_s *module,
                        FAR struct pnut_loop_s *loop)
{
  if (g_serving)
    {
      fake_stop();
    }

  pnut_wifi_settings_close(&g_cache);
}

/* The topics: Telephony's, Wi-Fi's, the zone */

static void publish_wifi(int state, FAR const char *name, int32_t dbm)
{
  pnut_wifi_info_t wifi;

  memset(&wifi, 0, sizeof(wifi));
  wifi.state  = state;
  wifi.signal = dbm;
  strcpy(wifi.name, name);
  assert_int_equal(pnut_wifi_info_publish(g_wifi, &wifi), 0);
}

static void publish(void)
{
  pnut_telephony_info_t tel;
  pnut_time_change_t time;

  memset(&tel, 0, sizeof(tel));
  tel.modem        = PNUT_TELEPHONY_INFO_MODEM_ON;
  tel.sim          = PNUT_TELEPHONY_INFO_SIM_READY;
  tel.registration = PNUT_TELEPHONY_INFO_REGISTRATION_HOME;
  tel.tech         = PNUT_TELEPHONY_INFO_TECH_LTE;
  tel.bars         = 3;
  assert_int_equal(pnut_telephony_info_publish(g_tel, &tel), 0);

  publish_wifi(g_start_on ? PNUT_WIFI_INFO_STATE_ON :
                            PNUT_WIFI_INFO_STATE_OFF, "", 0);

  memset(&time, 0, sizeof(time));
  strcpy(time.zone, "Europe/Warsaw");
  strcpy(time.rule, WIFI_RULE);
  assert_int_equal(pnut_time_change_publish(g_time, &time), 0);

  if (g_start_on)
    {
      assert_int_equal(pnut_wifi_settings_set_enabled(&g_cache, true, NULL,
                                                      NULL), 0);
    }
}

static void step_timer(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR const struct step_s *step = arg;

  if (step->run != NULL)
    {
      step->run();
    }
  else
    {
      pnut_loop_stop(loop, 0);
    }
}

static void run(FAR const struct step_s *steps, int nsteps)
{
  struct pnut_loop_config_s lconfig;
  struct settings_config_s sconfig;
  struct display_config_s screen;
  struct input_config_s iconfig;
  struct ui_config_s uconfig;
  char dir[] = "/tmp/pnut-uiwifi-XXXXXX";
  char cmd[64];
  int i;

  assert_non_null(mkdtemp(dir));
  pnut_loop_defaults(&lconfig);
  lconfig.rundir = dir;
  lconfig.timers = 48;            /* The steps', Settings', the UI's */
  assert_int_equal(pnut_loop_create(&lconfig, &g_loop), 0);
  assert_int_equal(pnut_telephony_info_advertise(g_loop, &g_tel), 0);
  assert_int_equal(pnut_wifi_info_advertise(g_loop, &g_wifi), 0);
  assert_int_equal(pnut_time_change_advertise(g_loop, &g_time), 0);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir    = dir;
  sconfig.limits = g_limits;
  sconfig.delay  = 20;
  settings_init(&g_settings, &sconfig);
  assert_int_equal(pnut_module_add(g_loop, &g_settings.module), 0);

  memset(&g_owners, 0, sizeof(g_owners));
  g_owners.name  = "owners";
  g_owners.start = owners_start;
  g_owners.stop  = owners_stop;
  assert_int_equal(pnut_module_add(g_loop, &g_owners), 0);

  memset(g_frame, 0xff, sizeof(g_frame));
  memset(&screen, 0, sizeof(screen));
  screen.frame  = g_frame;
  screen.stride = STRIDE;
  screen.width  = WIDTH;
  screen.height = HEIGHT;
  screen.ops    = &g_ops;

  memset(&iconfig, 0, sizeof(iconfig));
  iconfig.display = &g_ui.display;
  input_init(&g_input, &iconfig);

  memset(&uconfig, 0, sizeof(uconfig));
  uconfig.screen = &screen;
  uconfig.input  = &g_input;
  uconfig.now    = fixed_now;
  ui_init(&g_ui, &uconfig);
  assert_int_equal(pnut_module_add(g_loop, &g_ui.module), 0);
  assert_int_equal(pnut_module_add(g_loop, &g_input.module), 0);

  g_scans = g_joins = g_knowns = g_leaves = 0;
  g_join_waiting = false;
  g_join_answer  = JOIN_OK;
  g_end          = true;
  g_field        = NULL;

  /* Three in reach: Home (known), Office, Cafe (open) */

  net(0, "Home", -50, true);
  net(1, "Office", -70, true);
  net(2, "Cafe", -80, false);
  g_nnets = 3;
  memset(&g_known, 0, sizeof(g_known));
  g_known.networks_count = 1;
  strcpy(g_known.networks[0].name, "Home");
  g_known.networks[0].security = PNUT_WIFI_SECURITY_WIFI_SECURITY_SECURED;

  for (i = 0; i < nsteps; i++)
    {
      assert_int_equal(pnut_timer_start(g_loop, steps[i].at, 0, step_timer,
                                        (FAR void *)&steps[i], NULL), 0);
    }

  assert_int_equal(pnut_loop_run(g_loop), 0);
  pnut_topic_unadvertise(g_tel);
  pnut_topic_unadvertise(g_wifi);
  pnut_topic_unadvertise(g_time);
  pnut_loop_destroy(g_loop);
  snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
  assert_int_equal(system(cmd), 0);
}

static void key(uint32_t code, bool special)
{
  input_key(&g_input, code, special, true);
  input_key(&g_input, code, special, false);
}

static void type(FAR const char *text)
{
  while (*text != '\0')
    {
      key((uint8_t)*text++, false);
    }
}

static void tap(int16_t x, int16_t y)
{
  input_touch(&g_input, x, y, true);
  input_touch(&g_input, x, y, false);
}

static FAR const char *top(void)
{
  return ui_top(&g_ui)->cls->name;
}

/* Whether a label shown on the top screen has this text */

static bool shown_in(FAR lv_obj_t *obj, FAR const char *text)
{
  uint32_t i;

  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN))
    {
      return false;
    }

  if (lv_obj_check_type(obj, &lv_label_class) &&
      strcmp(lv_label_get_text(obj), text) == 0)
    {
      return true;
    }

  for (i = 0; i < lv_obj_get_child_count(obj); i++)
    {
      if (shown_in(lv_obj_get_child(obj, i), text))
        {
          return true;
        }
    }

  return false;
}

static bool shows(FAR const char *text)
{
  return shown_in(ui_top(&g_ui)->root, text);
}

/* A heading's: in capitals */

static bool shows_heading(FAR const char *text)
{
  char caps[64];
  size_t i;

  for (i = 0; text[i] != '\0' && i < sizeof(caps) - 1; i++)
    {
      caps[i] = text[i] >= 'a' && text[i] <= 'z' ? text[i] - 'a' + 'A' :
                text[i];
    }

  caps[i] = '\0';
  return shows(caps);
}

/* The frame as a PBM picture (P4: a set bit black, the panel's white) */

static void save(FAR const char *name)
{
  char path[128];
  FILE *f;
  size_t i;

  display_draw_now(&g_ui.display);
  mkdir(SCREENS_DIR, 0755);
  snprintf(path, sizeof(path), "%s/%s.pbm", SCREENS_DIR, name);
  f = fopen(path, "wb");
  assert_non_null(f);
  fprintf(f, "P4\n%d %d\n", WIDTH, HEIGHT);
  for (i = 0; i < sizeof(g_frame); i++)
    {
      fputc((uint8_t)~g_frame[i], f);
    }

  fclose(f);
}

/* test_wifi_switch: Settings' lines; the switch by a key and by a tap */

static void switch_settings(void)
{
  ui_push(&g_ui, &g_scr_settings, NULL);
}

static void switch_open(void)
{
  assert_string_equal(top(), "settings");
  assert_true(shows("Off"));
  assert_true(shows("LTE \xc2\xb7 data off"));
  save("settings");
  key('e', false);
}

static void switch_off(void)
{
  assert_string_equal(top(), "wifi");
  assert_true(shows("Off"));
  assert_false(shows_heading(UI_T_NEARBY));
  assert_int_equal(g_scans, 0);
  save("wifi-off");
  key('e', false);
}

static void switch_on(void)
{
  assert_true(shows("On"));
  assert_int_equal(g_scans, 1);
  tap(SWITCH_X, SWITCH_Y);
}

static void switch_off_again(void)
{
  assert_true(shows("Off"));
  assert_false(shows("Office"));
  assert_false(g_cache.values.enabled);
}

static void test_wifi_switch(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, publish },
    { 400, switch_settings },
    { 600, switch_open },
    { 1200, switch_off },
    { 1800, switch_on },
    { 2400, switch_off_again },
    { 2500, NULL },
  };

  g_start_on = false;
  run(steps, 7);
}

/* test_wifi_scan_join: the scan's results; a known network joined at once,
 * a new one through the join screen
 */

static void scan_open(void)
{
  ui_push(&g_ui, &g_scr_wifi, NULL);
}

static void scan_shown(void)
{
  assert_int_equal(g_scans, 1);
  assert_true(shows("On"));
  assert_true(shows("Secured \xc2\xb7 strong \xc2\xb7 saved"));
  assert_true(shows("Secured \xc2\xb7 fair"));
  assert_true(shows("Open \xc2\xb7 weak"));
  assert_true(shows_heading("Nearby \xc2\xb7 scanned 14:07"));

  /* Home, known: joined at once, with no passphrase */

  key('s', false);
  key('e', false);
}

static void scan_joined(void)
{
  assert_int_equal(g_joins, 1);
  assert_string_equal(g_join_name, "Home");
  assert_string_equal(g_join_pass, "");
  assert_string_equal(top(), "wifi");
  publish_wifi(PNUT_WIFI_INFO_STATE_JOINED, "Home", -50);
}

static void scan_connected(void)
{
  assert_true(shows_heading(UI_T_CONNECTED));
  save("wifi-scanned");

  /* Office, new: the join screen */

  key('s', false);
  key('e', false);
}

static void scan_join_screen(void)
{
  assert_string_equal(top(), "join");
  assert_true(shows("Office"));
  assert_true(shows(UI_T_SHOW_PASSWORD));
  type("secret7");
  key(KEYMAP_ENTER, true);
  assert_true(shows(UI_T_PASSWORD_LENGTH));
  assert_int_equal(g_joins, 1);
  save("join");
  g_join_answer = JOIN_REFUSED;
  type("x");
  key(KEYMAP_ENTER, true);
}

static void scan_refused(void)
{
  assert_int_equal(g_joins, 2);
  assert_string_equal(g_join_name, "Office");
  assert_string_equal(g_join_pass, "secret7x");
  assert_string_equal(top(), "join");
  assert_true(shows(UI_T_WIFI_REFUSED));
  save("join-refused");

  /* Accepted: back to Wi-Fi */

  g_join_answer = JOIN_OK;
  key(KEYMAP_ENTER, true);
}

static void scan_back(void)
{
  assert_int_equal(g_joins, 3);
  assert_string_equal(top(), "wifi");

  /* Options on the connected one: Scan again, Leave, Forget */

  key('w', false);
  key(KEYMAP_F1, true);
  assert_true(ui_options_open(&g_ui));
}

static void scan_options(void)
{
  save("wifi-options");
  key('s', false);
  key('e', false);
}

static void scan_left(void)
{
  assert_int_equal(g_leaves, 1);
}

static void test_wifi_scan_join(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, publish },
    { 400, scan_open },
    { 1000, scan_shown },
    { 1400, scan_joined },
    { 1800, scan_connected },
    { 2200, scan_join_screen },
    { 2600, scan_refused },
    { 3000, scan_back },
    { 3400, scan_options },
    { 3800, scan_left },
    { 3900, NULL },
  };

  g_start_on = true;
  run(steps, 11);
}

/* test_wifi_join_gone: the join screen left before the answer: the field
 * wiped, and the answer changes nothing
 */

static void gone_join(void)
{
  ui_push(&g_ui, &g_scr_wifi, NULL);
}

static void gone_office(void)
{
  key('s', false);
  key('s', false);
  key('e', false);
  assert_string_equal(top(), "join");
  g_join_answer = JOIN_LATER;
  type("password");
  key(KEYMAP_ENTER, true);
}

static void gone_back(void)
{
  int i;

  assert_true(g_join_waiting);
  g_field = ui_top(&g_ui)->state;
  key(KEYMAP_F3, true);
  assert_string_equal(top(), "wifi");
  assert_int_equal(g_field->len, 0);
  for (i = 0; i < (int)sizeof(g_field->text); i++)
    {
      assert_int_equal(g_field->text[i], 0);
    }

  pnut_wifi_fail(&g_server, &g_join_req, PNUT_STATUS_UNAVAILABLE,
                 PNUT_WIFI_ERROR_CODE_REFUSED, "refused");
}

static void gone_nothing(void)
{
  assert_string_equal(top(), "wifi");
  assert_false(shows(UI_T_WIFI_REFUSED));
}

static void test_wifi_join_gone(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, publish },
    { 400, gone_join },
    { 1000, gone_office },
    { 1400, gone_back },
    { 1800, gone_nothing },
    { 1900, NULL },
  };

  g_start_on = true;
  run(steps, 6);
}

/* test_wifi_off_scanning: Wi-Fi turned off while the scan goes on: the
 * list empties, and no error
 */

static void off_open(void)
{
  g_end = false;
  g_nnets = 2;
  ui_push(&g_ui, &g_scr_wifi, NULL);
}

static void off_partial(void)
{
  /* The first at once; Office waits for the next redraw, 5 s on */

  assert_true(shows("Home"));
  assert_false(shows("Office"));
  assert_true(shows_heading(UI_T_SCANNING));
  assert_int_equal(pnut_wifi_settings_set_enabled(&g_cache, false, NULL,
                                                  NULL), 0);
}

static void off_empty(void)
{
  assert_true(shows("Off"));
  assert_false(shows("Home"));
  assert_false(shows_heading(UI_T_SCAN_FAILED));
  assert_false(shows(UI_T_WIFI_FAILED));
}

static void test_wifi_off_scanning(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, publish },
    { 400, off_open },
    { 1000, off_partial },
    { 1600, off_empty },
    { 1700, NULL },
  };

  g_start_on = true;
  run(steps, 5);
}

/* test_wifi_known_again: the known networks read again when Wi-Fi's state
 * changes and when its service comes back
 */

static void again_open(void)
{
  ui_push(&g_ui, &g_scr_wifi, NULL);
}

static void again_state(void)
{
  assert_int_equal(g_knowns, 1);
  publish_wifi(PNUT_WIFI_INFO_STATE_JOINED, "Home", -50);
}

static void again_restart(void)
{
  assert_int_equal(g_knowns, 2);
  fake_stop();
  fake_serve();
}

static void again_back(void)
{
  assert_int_equal(g_knowns, 3);
}

static void test_wifi_known_again(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, publish },
    { 400, again_open },
    { 1000, again_state },
    { 1400, again_restart },
    { 4000, again_back },
    { 4100, NULL },
  };

  g_start_on = true;
  run(steps, 6);
}

/* test_wifi_row_moved: a stronger network comes between redraws: a row
 * still opens the network it shows
 */

static void moved_open(void)
{
  net(0, "Office", -70, true);
  net(1, "Home", -50, true);
  g_nnets = 1;
  g_end   = false;
  ui_push(&g_ui, &g_scr_wifi, NULL);
}

static void moved_stronger(void)
{
  assert_true(shows("Office"));
  g_nnets = 2;
  fake_push();
}

static void moved_press(void)
{
  assert_false(shows("Home"));    /* Not drawn yet: the next redraw's */
  key('s', false);
  key('e', false);
}

static void moved_join(void)
{
  assert_string_equal(top(), "join");
  assert_true(shows("Office"));
  assert_int_equal(g_joins, 0);
}

static void test_wifi_row_moved(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, publish },
    { 400, moved_open },
    { 1000, moved_stronger },
    { 1400, moved_press },
    { 1800, moved_join },
    { 1900, NULL },
  };

  g_start_on = true;
  run(steps, 6);
}

/* test_wifi_service_back: Wi-Fi's service away as the screen opens: the
 * scan is made once the service is back
 */

static void back_away(void)
{
  publish();
  fake_stop();
}

static void back_failed(void)
{
  assert_int_equal(g_scans, 0);
  assert_false(shows_heading(UI_T_SCANNING));
  fake_serve();
}

static void back_scanned(void)
{
  assert_int_equal(g_scans, 1);
  assert_true(shows("Office"));
}

static void test_wifi_service_back(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, back_away },
    { 400, again_open },
    { 1200, back_failed },
    { 4000, back_scanned },
    { 4100, NULL },
  };

  g_start_on = true;
  run(steps, 5);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_wifi_switch),
    cmocka_unit_test(test_wifi_scan_join),
    cmocka_unit_test(test_wifi_join_gone),
    cmocka_unit_test(test_wifi_off_scanning),
    cmocka_unit_test(test_wifi_known_again),
    cmocka_unit_test(test_wifi_row_moved),
    cmocka_unit_test(test_wifi_service_back),
  };

  return cmocka_run_group_tests_name("ui_wifi", tests, NULL, NULL);
}
