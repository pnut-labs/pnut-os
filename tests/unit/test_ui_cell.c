/****************************************************************************
 * pnut-os/tests/unit/test_ui_cell.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Cellular (scr_cell.c), driven by keys: mobile data, the access point,
 * the modem through Settings, which runs in the test's loop with
 * Telephony's schema registered by a cache of its own; the network's
 * state and the battery from topics the test publishes.  The pictures go
 * to SCREENS_DIR, as test_ui_screens'.
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
#include <pnut/timer.h>
#include <pnut/topic.h>

#include "pnut/power.topics.h"
#include "pnut/telephony.settings.h"
#include "pnut/telephony.topics.h"
#include "pnut/time.topics.h"
#include "pnut/wifi.topics.h"

#include "blocks.h"
#include "input.h"
#include "keymap.h"
#include "settings.h"
#include "text.h"
#include "theme.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIDTH    240
#define HEIGHT   320
#define STRIDE   (WIDTH / 8)

/* 2026-09-22 12:07:00 UTC: 14:07 in Warsaw */

#define CELL_NOW   1790078820
#define CELL_RULE  "CET-1CEST,M3.5.0,M10.5.0/3"

#ifndef SCREENS_DIR
#  define SCREENS_DIR  "screens"
#endif

/* 23 characters, the most the topic carries */

#define CELL_LONG_OPERATOR  "Operator With A Long Na"

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
  .settings = 64,
  .texts    = 32,
  .choices  = 32,
};

static uint8_t g_frame[STRIDE * HEIGHT];
static struct ui_s g_ui;
static struct input_s g_input;
static struct settings_s g_settings;
static struct pnut_module_s g_owners;
static struct pnut_telephony_settings_s g_cache;
static FAR struct pnut_publisher_s *g_tel;
static FAR struct pnut_publisher_s *g_wifi;
static FAR struct pnut_publisher_s *g_time;
static FAR struct pnut_publisher_s *g_battery;
static time_t g_now = CELL_NOW;

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

static void owner_changed(FAR struct pnut_settings_cache_s *cache,
                          int index, FAR void *arg)
{
}

static int owners_start(FAR struct pnut_module_s *module,
                        FAR struct pnut_loop_s *loop)
{
  pnut_module_ready(module);
  assert_int_equal(pnut_telephony_settings_open(module, owner_changed, NULL,
                                                &g_cache), 0);
  return 0;
}

static void owners_stop(FAR struct pnut_module_s *module,
                        FAR struct pnut_loop_s *loop)
{
  pnut_telephony_settings_close(&g_cache);
}

/* The topics: registered on LTE with Plus, 3 bars; Wi-Fi off; Warsaw;
 * the battery at 84 %, charging
 */

static void publish_tel(FAR const char *operator)
{
  pnut_telephony_info_t tel;

  memset(&tel, 0, sizeof(tel));
  tel.modem        = PNUT_TELEPHONY_INFO_MODEM_ON;
  tel.sim          = PNUT_TELEPHONY_INFO_SIM_READY;
  tel.registration = PNUT_TELEPHONY_INFO_REGISTRATION_HOME;
  tel.tech         = PNUT_TELEPHONY_INFO_TECH_LTE;
  tel.bars         = 3;
  tel.dbm          = -71;
  strcpy(tel.operator_name, operator);
  assert_int_equal(pnut_telephony_info_publish(g_tel, &tel), 0);
}

static void publish(void)
{
  pnut_battery_info_t battery;
  pnut_time_change_t time;
  pnut_wifi_info_t wifi;

  publish_tel("Plus");

  memset(&wifi, 0, sizeof(wifi));
  assert_int_equal(pnut_wifi_info_publish(g_wifi, &wifi), 0);

  memset(&time, 0, sizeof(time));
  strcpy(time.zone, "Europe/Warsaw");
  strcpy(time.rule, CELL_RULE);
  assert_int_equal(pnut_time_change_publish(g_time, &time), 0);

  memset(&battery, 0, sizeof(battery));
  battery.level    = 84;
  battery.charging = true;
  assert_int_equal(pnut_battery_info_publish(g_battery, &battery), 0);
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
  FAR struct pnut_loop_s *loop;
  char dir[] = "/tmp/pnut-uicell-XXXXXX";
  char cmd[64];
  int i;

  assert_non_null(mkdtemp(dir));
  pnut_loop_defaults(&lconfig);
  lconfig.rundir = dir;
  lconfig.timers = 48;            /* The steps', Settings', the UI's */
  assert_int_equal(pnut_loop_create(&lconfig, &loop), 0);
  assert_int_equal(pnut_telephony_info_advertise(loop, &g_tel), 0);
  assert_int_equal(pnut_wifi_info_advertise(loop, &g_wifi), 0);
  assert_int_equal(pnut_time_change_advertise(loop, &g_time), 0);
  assert_int_equal(pnut_battery_info_advertise(loop, &g_battery), 0);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir    = dir;
  sconfig.limits = g_limits;
  sconfig.delay  = 20;
  settings_init(&g_settings, &sconfig);
  assert_int_equal(pnut_module_add(loop, &g_settings.module), 0);

  memset(&g_owners, 0, sizeof(g_owners));
  g_owners.name  = "owners";
  g_owners.start = owners_start;
  g_owners.stop  = owners_stop;
  assert_int_equal(pnut_module_add(loop, &g_owners), 0);

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
  assert_int_equal(pnut_module_add(loop, &g_ui.module), 0);
  assert_int_equal(pnut_module_add(loop, &g_input.module), 0);

  for (i = 0; i < nsteps; i++)
    {
      assert_int_equal(pnut_timer_start(loop, steps[i].at, 0, step_timer,
                                        (FAR void *)&steps[i], NULL), 0);
    }

  assert_int_equal(pnut_loop_run(loop), 0);
  pnut_topic_unadvertise(g_tel);
  pnut_topic_unadvertise(g_wifi);
  pnut_topic_unadvertise(g_time);
  pnut_topic_unadvertise(g_battery);
  pnut_loop_destroy(loop);
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

static FAR const char *top(void)
{
  return ui_top(&g_ui)->cls->name;
}

/* The label shown on the top screen with this text, or NULL */

static FAR lv_obj_t *label_in(FAR lv_obj_t *obj, FAR const char *text)
{
  FAR lv_obj_t *found;
  uint32_t i;

  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN))
    {
      return NULL;
    }

  if (lv_obj_check_type(obj, &lv_label_class) &&
      strcmp(lv_label_get_text(obj), text) == 0)
    {
      return obj;
    }

  for (i = 0; i < lv_obj_get_child_count(obj); i++)
    {
      found = label_in(lv_obj_get_child(obj, i), text);
      if (found != NULL)
        {
          return found;
        }
    }

  return NULL;
}

static bool shows(FAR const char *text)
{
  return label_in(ui_top(&g_ui)->root, text) != NULL;
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

/* test_cell_switches: from Settings; mobile data and the modem; the
 * network's state; the battery in the header
 */

static void switches_settings(void)
{
  ui_push(&g_ui, &g_scr_settings, NULL);
  key('s', false);
  key('e', false);
}

static void switches_shown(void)
{
  FAR lv_obj_t *header;

  assert_string_equal(top(), "cell");
  assert_true(shows(UI_T_MOBILE_DATA));
  assert_true(shows("Plus \xc2\xb7 home network"));
  assert_true(shows("3 of 4 \xc2\xb7 -71 dBm"));
  assert_true(shows(UI_T_SIM_READY));

  header = ui_top(&g_ui)->header;
  assert_string_equal(block_header_text(header, BLOCK_HEADER_BATTERY),
                      "84%+");
  assert_int_equal(block_header_lit(header, true), 5);
  save("cell");

  /* Mobile data on, by its row */

  key('e', false);
}

static void switches_data(void)
{
  assert_true(g_cache.values.data);

  /* The modem off: two rows down, past the access point's field (which
   * takes the letters: Find and S leave it)
   */

  key('s', false);
  input_key(&g_input, KEYMAP_FIND, true, true);
  key('s', false);
  input_key(&g_input, KEYMAP_FIND, true, false);
  key('e', false);
}

static void switches_modem(void)
{
  assert_false(g_cache.values.enabled);
}

static void test_cell_switches(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, publish },
    { 500, switches_settings },
    { 1100, switches_shown },
    { 1500, switches_data },
    { 1900, switches_modem },
    { 2000, NULL },
  };

  run(steps, 6);
}

/* test_cell_apn: the access point, set; refused when it has a character
 * it may not; emptied: the network's own
 */

static void apn_open(void)
{
  ui_push(&g_ui, &g_scr_cell, NULL);
}

static void apn_type(void)
{
  key('s', false);
  type("internet.example");
  key(KEYMAP_ENTER, true);
}

static void apn_set(void)
{
  int i;

  assert_string_equal(g_cache.values.apn, "internet.example");
  for (i = 0; i < 16; i++)
    {
      key(KEYMAP_BACKSPACE, true);
    }

  type("bad apn");
  key(KEYMAP_ENTER, true);
  assert_true(shows(UI_T_APN_BAD));
}

static void apn_refused(void)
{
  int i;

  assert_string_equal(g_cache.values.apn, "internet.example");
  for (i = 0; i < 7; i++)
    {
      key(KEYMAP_BACKSPACE, true);
    }

  key(KEYMAP_ENTER, true);
}

static void apn_emptied(void)
{
  assert_string_equal(g_cache.values.apn, "");
  assert_false(shows(UI_T_APN_BAD));
}

static void test_cell_apn(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, publish },
    { 500, apn_open },
    { 1100, apn_type },
    { 1500, apn_set },
    { 1900, apn_refused },
    { 2300, apn_emptied },
    { 2400, NULL },
  };

  run(steps, 7);
}

/* test_cell_apn_follows: the field focused, nothing typed, the access
 * point changed elsewhere: the field shows it, and Enter keeps it
 */

static void follows_open(void)
{
  ui_push(&g_ui, &g_scr_cell, NULL);
}

static void follows_focus(void)
{
  key('s', false);
  assert_int_equal(pnut_telephony_settings_set_apn(&g_cache, "elsewhere",
                                                   NULL, NULL), 0);
}

static void follows_enter(void)
{
  key(KEYMAP_ENTER, true);
}

static void follows_kept(void)
{
  assert_string_equal(g_cache.values.apn, "elsewhere");
}

static void test_cell_apn_follows(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, publish },
    { 300, follows_open },
    { 900, follows_focus },
    { 1300, follows_enter },
    { 1700, follows_kept },
    { 1800, NULL },
  };

  run(steps, 6);
}

/* test_cell_operator: the longest operator name, one line */

static void operator_open(void)
{
  publish_tel(CELL_LONG_OPERATOR);
  ui_push(&g_ui, &g_scr_cell, NULL);
}

static void operator_shown(void)
{
  FAR lv_obj_t *label;

  lv_obj_update_layout(ui_top(&g_ui)->root);
  label = label_in(ui_top(&g_ui)->root,
                   CELL_LONG_OPERATOR " \xc2\xb7 home network");
  if (label == NULL)
    {
      /* Cut by LVGL: its text ends in dots */

      label = label_in(ui_top(&g_ui)->root, "NETWORK");
      assert_non_null(label);
      label = lv_obj_get_child(lv_obj_get_parent(label), 1);
    }

  assert_int_equal(lv_obj_get_height(label),
                   lv_font_get_line_height(&g_ui_sans_14));
}

static void test_cell_operator(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, publish },
    { 300, operator_open },
    { 700, operator_shown },
    { 800, NULL },
  };

  run(steps, 4);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_cell_switches),
    cmocka_unit_test(test_cell_apn),
    cmocka_unit_test(test_cell_apn_follows),
    cmocka_unit_test(test_cell_operator),
  };

  return cmocka_run_group_tests_name("ui_cell", tests, NULL, NULL);
}
