/****************************************************************************
 * pnut-os/tests/unit/test_ui_screens.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The system UI's screens, driven as the owner would, with keys and taps:
 * Home, Menu and its folders, the placeholders.  Each state is drawn into
 * a memory frame and written to build/unit/screens/<name>.pbm, which
 * tools/screens.py compares with tests/screens/<name>.png.
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

#include "pnut/telephony.topics.h"
#include "pnut/time.topics.h"
#include "pnut/wifi.topics.h"

#include "apps.h"
#include "blocks.h"
#include "input.h"
#include "keymap.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIDTH    240
#define HEIGHT   320
#define STRIDE   (WIDTH / 8)

/* 2026-09-22 12:07:00 UTC: 14:07 in Warsaw */

#define SCREENS_NOW   1790078820
#define SCREENS_RULE  "CET-1CEST,M3.5.0,M10.5.0/3"

/* Where the images go: tests/unit/Makefile names its build directory's
 * screens/
 */

#ifndef SCREENS_DIR
#  define SCREENS_DIR  "screens"
#endif

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

static uint8_t g_frame[STRIDE * HEIGHT];
static struct ui_s g_ui;
static struct input_s g_input;
static FAR struct pnut_publisher_s *g_tel;
static FAR struct pnut_publisher_s *g_wifi;
static FAR struct pnut_publisher_s *g_time;

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

static time_t g_now = SCREENS_NOW;

static time_t fixed_now(void)
{
  return g_now;
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

/* The topics the header shows: 3 bars, Wi-Fi joined "Home", Warsaw */

static void publish(void)
{
  pnut_telephony_info_t tel;
  pnut_wifi_info_t wifi;
  pnut_time_change_t time;

  memset(&tel, 0, sizeof(tel));
  tel.modem        = PNUT_TELEPHONY_INFO_MODEM_ON;
  tel.sim          = PNUT_TELEPHONY_INFO_SIM_READY;
  tel.registration = PNUT_TELEPHONY_INFO_REGISTRATION_HOME;
  tel.tech         = PNUT_TELEPHONY_INFO_TECH_LTE;
  tel.bars         = 3;
  assert_int_equal(pnut_telephony_info_publish(g_tel, &tel), 0);

  memset(&wifi, 0, sizeof(wifi));
  wifi.state = PNUT_WIFI_INFO_STATE_JOINED;
  strcpy(wifi.name, "Home");
  assert_int_equal(pnut_wifi_info_publish(g_wifi, &wifi), 0);

  memset(&time, 0, sizeof(time));
  strcpy(time.zone, "Europe/Warsaw");
  strcpy(time.rule, SCREENS_RULE);
  assert_int_equal(pnut_time_change_publish(g_time, &time), 0);
}

static void run(FAR const struct step_s *steps, int nsteps)
{
  struct pnut_loop_config_s lconfig;
  struct display_config_s screen;
  struct input_config_s iconfig;
  struct ui_config_s uconfig;
  FAR struct pnut_loop_s *loop;
  char dir[] = "/tmp/pnut-screens-XXXXXX";
  int i;

  assert_non_null(mkdtemp(dir));
  pnut_loop_defaults(&lconfig);
  lconfig.rundir = dir;
  assert_int_equal(pnut_loop_create(&lconfig, &loop), 0);
  assert_int_equal(pnut_telephony_info_advertise(loop, &g_tel), 0);
  assert_int_equal(pnut_wifi_info_advertise(loop, &g_wifi), 0);
  assert_int_equal(pnut_time_change_advertise(loop, &g_time), 0);

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
  pnut_loop_destroy(loop);
  rmdir(dir);
}

static void key(uint32_t code, bool special)
{
  input_key(&g_input, code, special, true);
  input_key(&g_input, code, special, false);
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

static FAR const char *title(void)
{
  return block_header_text(ui_top(&g_ui)->header, BLOCK_HEADER_TITLE);
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

/* test_menu_order */

static void test_menu_order(void **state)
{
  static const char * const page1[12] =
  {
    "Messages", "Phone", "Contacts", "Email", "Calendar", "Reminders",
    "Notes", "Maps", "Weather", "Reader", "Read later", "Settings"
  };

  static const char * const page2[7] =
  {
    "Podcasts", "Music", "AI chat", "Passwords", "Wallet", "Tools",
    "Games"
  };

  static const char * const tools[4] =
  {
    "Clock", "Files", "Calculator", "Recorder"
  };

  static const char * const shortcuts[6] =
  {
    "Messages", "Phone", "Calendar", "Reminders", "Reader", "Maps"
  };

  int i;

  assert_int_equal(g_ui_menu.count, 19);
  for (i = 0; i < 12; i++)
    {
      assert_string_equal(g_ui_menu.apps[i].name, page1[i]);
    }

  for (i = 0; i < 7; i++)
    {
      assert_string_equal(g_ui_menu.apps[12 + i].name, page2[i]);
    }

  assert_non_null(g_ui_menu.apps[17].folder);
  assert_int_equal(g_ui_menu.apps[17].folder->count, 4);
  for (i = 0; i < 4; i++)
    {
      assert_string_equal(g_ui_menu.apps[17].folder->apps[i].name,
                          tools[i]);
    }

  assert_string_equal(g_ui_menu.apps[18].folder->apps[0].name, "Chess");
  assert_string_equal(g_ui_menu.apps[18].folder->apps[1].name, "2048");

  assert_int_equal(g_ui_shortcuts.count, 6);
  for (i = 0; i < 6; i++)
    {
      assert_string_equal(g_ui_shortcuts.apps[i].name, shortcuts[i]);
    }
}

/* test_screens_keys: Home, then Menu by the middle softkey, a placeholder
 * and back with the focus kept, the second page, a folder, Settings
 */

static void keys_home(void)
{
  assert_string_equal(top(), "home");
  key('q', false);                          /* Back on Home: nothing */
  assert_string_equal(top(), "home");
  key(KEYMAP_F2, true);
}

static void keys_menu(void)
{
  assert_string_equal(top(), "menu");
  assert_string_equal(title(), "Menu");
  key('d', false);
  key('e', false);
}

static void keys_phone(void)
{
  assert_string_equal(top(), "unavailable");
  assert_string_equal(title(), "Phone");
  key('q', false);
}

static void keys_back(void)
{
  assert_string_equal(top(), "menu");
  assert_int_equal(ui_top(&g_ui)->focus, 1);
  key(' ', false);
}

static void keys_page2(void)
{
  assert_int_equal(ui_top(&g_ui)->focus, 12);
  key('s', false);
  key('d', false);
  key('d', false);
  key('e', false);
}

static void keys_tools(void)
{
  assert_string_equal(top(), "menu");
  assert_string_equal(title(), "Tools");
  key(KEYMAP_BACKSPACE, true);
}

static void keys_settings(void)
{
  assert_string_equal(title(), "Menu");
  input_key(&g_input, KEYMAP_SHIFT, true, true);
  key(' ', false);
  input_key(&g_input, KEYMAP_SHIFT, true, false);
  key('s', false);
  key('s', false);
  key('s', false);
  key('d', false);
  key('d', false);
  key('e', false);
}

static void keys_settings_shown(void)
{
  assert_string_equal(top(), "settings");
  assert_string_equal(title(), "Settings");
  key(KEYMAP_F3, true);
}

static void keys_home_softkeys(void)
{
  assert_string_equal(top(), "menu");
  key(KEYMAP_F3, true);
}

static void keys_calendar(void)
{
  assert_string_equal(top(), "home");
  key(KEYMAP_F1, true);
}

static void keys_calendar_shown(void)
{
  assert_string_equal(title(), "Calendar");
}

static void test_screens_keys(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, publish },
    { 300, keys_home },
    { 400, keys_menu },
    { 500, keys_phone },
    { 600, keys_back },
    { 700, keys_page2 },
    { 800, keys_tools },
    { 900, keys_settings },
    { 1000, keys_settings_shown },
    { 1100, keys_home_softkeys },
    { 1200, keys_calendar },
    { 1300, keys_calendar_shown },
    { 1400, NULL },
  };

  run(steps, 13);
}

/* test_screens_touch: the same by taps */

static void touch_menu(void)
{
  assert_string_equal(top(), "home");
  tap(120, 304);                            /* The middle softkey */
}

static void touch_contacts(void)
{
  assert_string_equal(top(), "menu");
  tap(198, 72);                             /* The third tile */
}

static void touch_back(void)
{
  assert_string_equal(title(), "Contacts");
  tap(210, 304);                            /* Back */
}

static void touch_page(void)
{
  assert_string_equal(top(), "menu");
  tap(212, 273);                            /* The page's arrow */
}

static void touch_page2(void)
{
  assert_int_equal(ui_top(&g_ui)->focus, 12);

  /* Back to page 1 by its arrow, and at once a tile of it: the new page's
   * tiles are placed before it is drawn
   */

  tap(28, 273);
  tap(41, 72);
  assert_string_equal(title(), "Messages");
}

static void test_screens_touch(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, publish },
    { 300, touch_menu },
    { 400, touch_contacts },
    { 500, touch_back },
    { 600, touch_page },
    { 700, touch_page2 },
    { 800, NULL },
  };

  run(steps, 7);
}

/* test_screens_images: each state, drawn (700 ms: the keys' half-second
 * gathering, then the draw)
 */

static void img_home(void)
{
  save("home");
  key(KEYMAP_F2, true);
}

static void img_menu1(void)
{
  save("menu-1");
  key(' ', false);
}

static void img_menu2(void)
{
  save("menu-2");
  key('s', false);
  key('d', false);
  key('d', false);
  key('e', false);
}

static void img_tools(void)
{
  save("folder-tools");
  key('q', false);
  key('d', false);
  key('e', false);
}

static void img_games(void)
{
  save("folder-games");
  key('e', false);
}

static void img_unavailable(void)
{
  assert_string_equal(title(), "Chess");
  save("unavailable");
}

static void test_screens_images(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, publish },
    { 900, img_home },
    { 1700, img_menu1 },
    { 2500, img_menu2 },
    { 3300, img_tools },
    { 4100, img_games },
    { 4900, img_unavailable },
    { 5000, NULL },
  };

  run(steps, 8);
}

/* test_screens_state: Home by its keys; the header as the state changes
 * while another screen is on top; the clock's minute
 */

static void state_phone(void)
{
  assert_string_equal(top(), "home");
  key('d', false);
  key('e', false);
}

static void state_menu(void)
{
  pnut_telephony_info_t tel;

  assert_string_equal(title(), "Phone");
  key('q', false);
  key(KEYMAP_F2, true);

  /* The signal drops while Menu is on top */

  memset(&tel, 0, sizeof(tel));
  tel.modem = PNUT_TELEPHONY_INFO_MODEM_ON;
  tel.bars  = 1;
  assert_int_equal(pnut_telephony_info_publish(g_tel, &tel), 0);
}

static void state_back(void)
{
  assert_string_equal(top(), "menu");
  assert_int_equal(block_header_lit(ui_top(&g_ui)->header, false), 1);
  key('q', false);
  assert_string_equal(top(), "home");
  assert_int_equal(block_header_lit(ui_top(&g_ui)->header, false), 1);
  assert_string_equal(block_header_text(ui_top(&g_ui)->header,
                                        BLOCK_HEADER_TIME), "14:07");

  /* The minute ends: the minute's timer (set for a second from the
   * start) finds 14:08
   */

  g_now = SCREENS_NOW + 60;
}

static void state_minute(void)
{
  assert_string_equal(block_header_text(ui_top(&g_ui)->header,
                                        BLOCK_HEADER_TIME), "14:08");
}

static void test_screens_state(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, publish },
    { 300, state_phone },
    { 400, state_menu },
    { 600, state_back },
    { 2800, state_minute },
    { 2900, NULL },
  };

  /* A second before 14:08 */

  g_now = SCREENS_NOW + 59;
  run(steps, 6);
  g_now = SCREENS_NOW;
}

/* test_screens_options: a screen's options, by keys and by a tap */

static int g_chosen[4];
static int g_nchosen;

static void opt_chosen(FAR struct screen_s *s, int index)
{
  g_chosen[g_nchosen++] = index;
}

static void opt_build(FAR struct screen_s *s)
{
  ui_header(s, NULL, "Options test");
  block_softkeys(s->root, "Options", "Open", "Back");
}

static bool opt_key(FAR struct screen_s *s, uint8_t action, uint32_t ch)
{
  static FAR const char * const items[3] =
  {
    "Scan again", "Leave", "Forget"
  };

  if (action == UI_ACT_SOFT_LEFT || action == UI_ACT_OPTIONS)
    {
      ui_options(s, items, 3, opt_chosen);
      return true;
    }

  return false;
}

static const struct screen_class_s g_opt_class =
{
  "options-test", opt_build, opt_key, NULL, NULL, NULL, 0
};

static void opt_open(void)
{
  g_nchosen = 0;
  ui_push(&g_ui, &g_opt_class, NULL);
  key(KEYMAP_F1, true);
  assert_true(ui_options_open(&g_ui));
  key('s', false);
  key('e', false);
  assert_false(ui_options_open(&g_ui));
  assert_int_equal(g_nchosen, 1);
  assert_int_equal(g_chosen[0], 1);

  /* Back closes them, nothing chosen, the screen still there */

  input_key(&g_input, KEYMAP_ALT, true, true);
  key(KEYMAP_ENTER, true);
  input_key(&g_input, KEYMAP_ALT, true, false);
  assert_true(ui_options_open(&g_ui));
  key('q', false);
  assert_false(ui_options_open(&g_ui));
  assert_int_equal(g_nchosen, 1);
  assert_string_equal(top(), "options-test");

  /* A tap outside closes them, nothing chosen */

  key(KEYMAP_F1, true);
  tap(120, 100);
  assert_false(ui_options_open(&g_ui));
  assert_int_equal(g_nchosen, 1);

  /* A tap on the third */

  key(KEYMAP_F1, true);
}

static void opt_tap(void)
{
  tap(120, 263);
  assert_false(ui_options_open(&g_ui));
  assert_int_equal(g_nchosen, 2);
  assert_int_equal(g_chosen[1], 2);
}

static void opt_shown(void)
{
  key(KEYMAP_F1, true);
  key('s', false);
}

static void opt_save(void)
{
  assert_true(ui_options_open(&g_ui));
  save("options");
}

static void test_screens_options(void **state)
{
  static const struct step_s steps[] =
  {
    { 100, publish },
    { 300, opt_open },
    { 400, opt_tap },
    { 500, opt_shown },
    { 1300, opt_save },
    { 1400, NULL },
  };

  run(steps, 6);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_menu_order),
    cmocka_unit_test(test_screens_keys),
    cmocka_unit_test(test_screens_touch),
    cmocka_unit_test(test_screens_state),
    cmocka_unit_test(test_screens_options),
    cmocka_unit_test(test_screens_images),
  };

  return cmocka_run_group_tests_name("ui_screens", tests, NULL, NULL);
}
