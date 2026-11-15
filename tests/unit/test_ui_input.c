/****************************************************************************
 * pnut-os/tests/unit/test_ui_input.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Input (RFC 0027, input.c): keys and touches reach LVGL's objects as the
 * system UI's actions, held keys repeat, and keys open the redraw's
 * gathering window.  The devices are left out: the tests give the keys.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "display.h"
#include "glue.h"
#include "input.h"
#include "keymap.h"
#include "theme.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIDTH   240
#define HEIGHT  320
#define STRIDE  (WIDTH / 8)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct step_s
{
  uint32_t at;                    /* ms after the start */
  CODE void (*run)(void);
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint8_t g_frame[STRIDE * HEIGHT];
static struct display_s g_display;
static struct input_s g_input;
static lv_obj_t *g_target;        /* Has the keys */
static lv_obj_t *g_button;        /* Takes taps */
static lv_obj_t *g_label;
static uint32_t g_keys[64];
static int g_nkeys;
static int g_clicks;
static int g_updates;
static int g_at;                  /* g_updates at a step */
static bool g_reenter;            /* on_key sends a key itself, once */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int fake_update(FAR void *arg, int16_t x, int16_t y, int16_t w,
                       int16_t h, bool full)
{
  g_updates++;
  return 0;
}

static const struct display_ops_s g_ops =
{
  fake_update
};

static void on_key(lv_event_t *e)
{
  assert_true(g_nkeys < 64);
  g_keys[g_nkeys++] = lv_event_get_key(e);
  if (g_reenter)
    {
      g_reenter = false;
      input_key(&g_input, 's', false, true);
      input_key(&g_input, 's', false, false);
    }

  /* Each key changes the screen */

  lv_label_set_text_fmt(g_label, "%d", g_nkeys);
}

static void on_click(lv_event_t *e)
{
  g_clicks++;
  if (g_reenter)
    {
      g_reenter = false;
      input_key(&g_input, 'w', false, true);
      input_key(&g_input, 'w', false, false);
    }
}

static int count(uint32_t key)
{
  int n = 0;
  int i;

  for (i = 0; i < g_nkeys; i++)
    {
      n += g_keys[i] == key;
    }

  return n;
}

static void key(uint32_t code, bool special)
{
  input_key(&g_input, code, special, true);
  input_key(&g_input, code, special, false);
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
  struct display_config_s dconfig;
  struct input_config_s iconfig;
  FAR struct pnut_loop_s *loop;
  int i;

  memset(g_frame, 0xff, sizeof(g_frame));
  g_nkeys   = 0;
  g_clicks  = 0;
  g_updates = 0;
  g_reenter = false;

  assert_int_equal(pnut_loop_create(NULL, &loop), 0);
  assert_int_equal(glue_start(loop), 0);

  memset(&dconfig, 0, sizeof(dconfig));
  dconfig.frame  = g_frame;
  dconfig.stride = STRIDE;
  dconfig.width  = WIDTH;
  dconfig.height = HEIGHT;
  dconfig.ops    = &g_ops;
  assert_int_equal(display_open(&g_display, loop, &dconfig), 0);

  memset(&iconfig, 0, sizeof(iconfig));
  iconfig.display = &g_display;
  input_init(&g_input, &iconfig);
  assert_int_equal(pnut_module_add(loop, &g_input.module), 0);

  for (i = 0; i < nsteps; i++)
    {
      assert_int_equal(pnut_timer_start(loop, steps[i].at, 0, step_timer,
                                        (FAR void *)&steps[i], NULL), 0);
    }

  assert_int_equal(pnut_loop_run(loop), 0);
  display_close(&g_display);
  glue_stop();
  pnut_loop_destroy(loop);
}

/* The screen, once Input has started (the first step) */

static void setup(void)
{
  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_white(), 0);
  lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

  g_label = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(g_label, &g_ui_sans_14, 0);
  lv_obj_set_style_text_color(g_label, lv_color_black(), 0);
  lv_label_set_text(g_label, "0");

  g_target = lv_obj_create(lv_screen_active());
  lv_obj_set_size(g_target, 10, 10);
  lv_obj_set_pos(g_target, 200, 200);
  lv_obj_add_event_cb(g_target, on_key, LV_EVENT_KEY, NULL);
  lv_group_add_obj(input_group(&g_input), g_target);
  lv_group_focus_obj(g_target);

  g_button = lv_obj_create(lv_screen_active());
  lv_obj_set_size(g_button, 40, 40);
  lv_obj_set_pos(g_button, 50, 50);
  lv_obj_add_flag(g_button, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_button, on_click, LV_EVENT_CLICKED, NULL);
}

/* test_input_keys */

static void keys_press(void)
{
  key('w', false);
  key('q', false);
  key(KEYMAP_F2, true);
  key('x', false);                /* Means nothing */

  input_key(&g_input, KEYMAP_ALT, true, true);
  key(KEYMAP_ENTER, true);
  input_key(&g_input, KEYMAP_ALT, true, false);
}

static void keys_check(void)
{
  assert_int_equal(g_nkeys, 4);
  assert_int_equal(g_keys[0], UI_KEY(UI_ACT_UP));
  assert_int_equal(g_keys[1], UI_KEY(UI_ACT_BACK));
  assert_int_equal(g_keys[2], UI_KEY(UI_ACT_SOFT_MIDDLE));
  assert_int_equal(g_keys[3], UI_KEY(UI_ACT_OPTIONS));
}

static void test_input_keys(void **state)
{
  static const struct step_s steps[] =
  {
    { 10, setup },
    { 50, keys_press },
    { 100, keys_check },
    { 120, NULL },
  };

  run(steps, 4);
}

/* test_input_typing */

static void typing_press(void)
{
  input_typing(&g_input, true);
  key('w', false);
  key(KEYMAP_BACKSPACE, true);
  key('\t', false);               /* A control character: not typed */
  key(0x0105, false);             /* ą */
}

static void typing_check(void)
{
  assert_int_equal(g_nkeys, 3);
  assert_int_equal(g_keys[0], 'w');
  assert_int_equal(g_keys[1], UI_KEY(UI_ACT_DELETE));
  assert_int_equal(g_keys[2], 0x0105);
}

static void test_input_typing(void **state)
{
  static const struct step_s steps[] =
  {
    { 10, setup },
    { 50, typing_press },
    { 100, typing_check },
    { 120, NULL },
  };

  run(steps, 4);
}

/* test_input_hold: 'w' held 700 ms repeats at 500, 550, ...; 'q' not */

static void hold_w(void)
{
  input_key(&g_input, 'w', false, true);
}

static void hold_w_release(void)
{
  input_key(&g_input, 'w', false, false);
  g_at = count(UI_KEY(UI_ACT_UP));
}

static void hold_q(void)
{
  input_key(&g_input, 'q', false, true);
}

static void hold_q_release(void)
{
  input_key(&g_input, 'q', false, false);
}

static void hold_check(void)
{
  /* Pressed, then from 500 ms every 50 ms: about 5 repeats by 750 ms
   * (a slow loop may lose one or two, never gain)
   */

  assert_true(g_at >= 4 && g_at <= 6);
  assert_int_equal(count(UI_KEY(UI_ACT_UP)), g_at);
  assert_int_equal(count(UI_KEY(UI_ACT_BACK)), 1);
}

static void test_input_hold(void **state)
{
  static const struct step_s steps[] =
  {
    { 10, setup },
    { 50, hold_w },
    { 750, hold_w_release },
    { 800, hold_q },
    { 1600, hold_q_release },
    { 1700, hold_check },
    { 1720, NULL },
  };

  run(steps, 7);
}

/* test_input_touch */

static void touch_tap(void)
{
  input_touch(&g_input, 60, 60, true);
  input_touch(&g_input, 60, 60, false);
  input_touch(&g_input, 150, 150, true);   /* Outside the button */
  input_touch(&g_input, 150, 150, false);
}

static void touch_check(void)
{
  assert_int_equal(g_clicks, 1);
}

static void test_input_touch(void **state)
{
  static const struct step_s steps[] =
  {
    { 10, setup },
    { 50, touch_tap },
    { 100, touch_check },
    { 120, NULL },
  };

  run(steps, 4);
}

/* test_input_gathers: two keys 100 ms apart, drawn once 500 ms after the
 * first
 */

static void gathers_first(void)
{
  g_at = g_updates;
  key('w', false);
}

static void gathers_second(void)
{
  key('s', false);
}

static void gathers_waiting(void)
{
  assert_int_equal(g_nkeys, 2);
  assert_int_equal(g_updates, g_at);
}

static void gathers_drawn(void)
{
  assert_int_equal(g_updates, g_at + 1);
}

static void test_input_gathers(void **state)
{
  static const struct step_s steps[] =
  {
    { 10, setup },
    { 100, gathers_first },
    { 200, gathers_second },
    { 450, gathers_waiting },
    { 800, gathers_drawn },
    { 850, NULL },
  };

  run(steps, 6);
}

/* test_input_no_reader: LVGL not reading the keypad (disabled; a screen
 * loading with an animation) drops the keys, never hangs
 */

static void noread_press(void)
{
  alarm(5);
  lv_indev_enable(g_input.keypad, false);
  key('w', false);
  key('q', false);
  lv_indev_enable(g_input.keypad, true);
  alarm(0);
  key('e', false);
}

static void noread_check(void)
{
  assert_int_equal(g_nkeys, 1);
  assert_int_equal(g_keys[0], UI_KEY(UI_ACT_OPEN));
}

static void test_input_no_reader(void **state)
{
  static const struct step_s steps[] =
  {
    { 10, setup },
    { 50, noread_press },
    { 100, noread_check },
    { 120, NULL },
  };

  run(steps, 4);
}

/* test_input_reentrant: a key handler that sends a key: both arrive, in
 * order
 */

static void reenter_press(void)
{
  g_reenter = true;
  key('w', false);
}

static void reenter_check(void)
{
  assert_int_equal(g_nkeys, 2);
  assert_int_equal(g_keys[0], UI_KEY(UI_ACT_UP));
  assert_int_equal(g_keys[1], UI_KEY(UI_ACT_DOWN));
}

static void reenter_tap(void)
{
  g_reenter = true;
  input_touch(&g_input, 60, 60, true);
  input_touch(&g_input, 60, 60, false);
}

static void reenter_tap_check(void)
{
  assert_int_equal(g_clicks, 1);
  assert_int_equal(g_nkeys, 3);
  assert_int_equal(g_keys[2], UI_KEY(UI_ACT_UP));
}

static void test_input_reentrant(void **state)
{
  static const struct step_s steps[] =
  {
    { 10, setup },
    { 50, reenter_press },
    { 100, reenter_check },
    { 150, reenter_tap },
    { 200, reenter_tap_check },
    { 220, NULL },
  };

  run(steps, 6);
}

/* test_input_other_release: another key's release (Shift's) leaves a held
 * key repeating; the repeat is armed when the program stops
 */

static void other_shift(void)
{
  input_key(&g_input, KEYMAP_SHIFT, true, true);
  input_key(&g_input, KEYMAP_SHIFT, true, false);
}

static void other_check(void)
{
  assert_true(count(UI_KEY(UI_ACT_UP)) >= 3);
}

static void test_input_other_release(void **state)
{
  static const struct step_s steps[] =
  {
    { 10, setup },
    { 50, hold_w },
    { 300, other_shift },
    { 800, other_check },
    { 820, NULL },
  };

  run(steps, 5);
}

/* test_input_unattached: keys before LVGL's input devices exist (the
 * device's start, before Board answers) are dropped, safely
 */

static void test_input_unattached(void **state)
{
  struct input_config_s config;
  struct input_s in;

  memset(&config, 0, sizeof(config));
  input_init(&in, &config);
  input_key(&in, 'w', false, true);
  input_key(&in, 'w', false, false);
  input_touch(&in, 10, 10, true);
  input_typing(&in, true);
  assert_null(input_group(&in));
}

/* test_input_fingers: the first finger down is followed; others are not;
 * a lift with no position is where the finger last was
 */

static void fingers_tap(void)
{
  input_touch_point(&g_input, 1, true, true, 60, 60);
  input_touch_point(&g_input, 2, true, true, 150, 150);
  input_touch_point(&g_input, 2, false, true, 150, 150);
  input_touch_point(&g_input, 1, false, true, 60, 60);

  /* Finger 3 on the button, finger 4 off it; 3 lifts with no position:
   * where it was
   */

  input_touch_point(&g_input, 3, true, true, 62, 62);
  input_touch_point(&g_input, 4, true, true, 150, 150);
  input_touch_point(&g_input, 4, false, true, 150, 150);
  input_touch_point(&g_input, 3, false, false, 0, 0);
  assert_int_equal(g_input.x, 62);
  assert_int_equal(g_input.y, 62);
  assert_false(g_input.down);
}

static void fingers_check(void)
{
  assert_int_equal(g_clicks, 2);
}

static void test_input_fingers(void **state)
{
  static const struct step_s steps[] =
  {
    { 10, setup },
    { 50, fingers_tap },
    { 100, fingers_check },
    { 120, NULL },
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
    cmocka_unit_test(test_input_keys),
    cmocka_unit_test(test_input_typing),
    cmocka_unit_test(test_input_hold),
    cmocka_unit_test(test_input_touch),
    cmocka_unit_test(test_input_gathers),
    cmocka_unit_test(test_input_no_reader),
    cmocka_unit_test(test_input_reentrant),
    cmocka_unit_test(test_input_other_release),
    cmocka_unit_test(test_input_unattached),
    cmocka_unit_test(test_input_fingers),
  };

  return cmocka_run_group_tests_name("ui_input", tests, NULL, NULL);
}
