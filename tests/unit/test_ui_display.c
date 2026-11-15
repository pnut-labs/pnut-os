/****************************************************************************
 * pnut-os/tests/unit/test_ui_display.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The system UI's screen (display.c): LVGL draws into a 1-bit frame on the
 * loop, and the screen is updated when the redraw rule says, with the area
 * that changed, and only when something did.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/timer.h>

#include "display.h"
#include "glue.h"
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

struct update_s
{
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  bool full;
};

struct step_s
{
  uint32_t at;                    /* ms after the start */
  CODE void (*run)(void);
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR struct pnut_loop_s *g_loop;
static uint8_t g_frame[STRIDE * HEIGHT];
static struct display_s g_display;
static lv_obj_t *g_label;
static lv_obj_t *g_upper;
static struct update_s g_updates[16];
static int g_nupdates;
static int g_fail;                /* Updates to fail with -EIO */
static int g_fired;               /* An LVGL timer's runs */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int fake_update(FAR void *arg, int16_t x, int16_t y, int16_t w,
                       int16_t h, bool full)
{
  assert_true(g_nupdates < 16);
  g_updates[g_nupdates].x    = x;
  g_updates[g_nupdates].y    = y;
  g_updates[g_nupdates].w    = w;
  g_updates[g_nupdates].h    = h;
  g_updates[g_nupdates].full = full;
  g_nupdates++;
  if (g_fail > 0)
    {
      g_fail--;
      return -EIO;
    }

  return 0;
}

static const struct display_ops_s g_ops =
{
  fake_update
};

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

/* Open the screen with a label, run the steps (the last one, with no run,
 * stops the loop), close it
 */

static void run(FAR const char *text, FAR const struct step_s *steps,
                int nsteps)
{
  struct display_config_s config;
  FAR struct pnut_loop_s *loop;
  int i;

  memset(g_frame, 0xff, sizeof(g_frame));
  memset(g_updates, 0, sizeof(g_updates));
  g_nupdates = 0;
  g_fail     = 0;
  g_fired    = 0;

  assert_int_equal(pnut_loop_create(NULL, &loop), 0);
  assert_int_equal(glue_start(loop), 0);
  g_loop = loop;

  memset(&config, 0, sizeof(config));
  config.frame  = g_frame;
  config.stride = STRIDE;
  config.width  = WIDTH;
  config.height = HEIGHT;
  config.ops    = &g_ops;
  assert_int_equal(display_open(&g_display, loop, &config), 0);

  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_white(), 0);
  lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
  g_label = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(g_label, &g_ui_sans_14, 0);
  lv_obj_set_style_text_color(g_label, lv_color_black(), 0);
  lv_obj_set_pos(g_label, 10, 100);
  lv_label_set_text(g_label, text);

  g_upper = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(g_upper, &g_ui_sans_14, 0);
  lv_obj_set_style_text_color(g_upper, lv_color_black(), 0);
  lv_obj_set_pos(g_upper, 100, 20);
  lv_label_set_text(g_upper, "x");

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

/* test_display_draws_changes */

static void changes_first(void)
{
  int ink = 0;
  size_t i;

  /* The first frame: drawn at once, in byte columns */

  assert_int_equal(g_nupdates, 1);
  assert_false(g_updates[0].full);
  assert_int_equal(g_updates[0].x % 8, 0);
  assert_int_equal(g_updates[0].w % 8, 0);
  for (i = 0; i < sizeof(g_frame); i++)
    {
      ink += g_frame[i] != 0xff;
    }

  assert_true(ink > 0);

  /* The same text again: nothing differs, no update */

  lv_label_set_text(g_label, "pnut");
}

static void changes_same(void)
{
  assert_int_equal(g_nupdates, 1);
  lv_label_set_text(g_label, "pn");
}

static void changes_less(void)
{
  /* Only the label's end changed: the update covers no more than it */

  assert_int_equal(g_nupdates, 2);
  assert_false(g_updates[1].full);
  assert_true(g_updates[1].x >= 16);
  assert_int_equal(g_updates[1].x % 8, 0);
  assert_int_equal(g_updates[1].w % 8, 0);
  assert_true(g_updates[1].x + g_updates[1].w <= 10 + 48);
  assert_true(g_updates[1].y >= 100);
  assert_true(g_updates[1].y + g_updates[1].h <= 100 + 30);
}

static void test_display_draws_changes(void **state)
{
  static const struct step_s steps[] =
  {
    { 200, changes_first },
    { 400, changes_same },
    { 600, changes_less },
    { 800, NULL },
  };

  run("pnut", steps, 4);
}

/* test_display_gathers_input */

static void gathers_key(void)
{
  assert_int_equal(g_nupdates, 1);
  display_input(&g_display);
  lv_label_set_text(g_label, "b");
}

static void gathers_more(void)
{
  display_input(&g_display);
  lv_label_set_text(g_label, "c");
}

static void gathers_waiting(void)
{
  assert_int_equal(g_nupdates, 1);
}

static void gathers_drawn(void)
{
  assert_int_equal(g_nupdates, 2);
}

static void test_display_gathers_input(void **state)
{
  static const struct step_s steps[] =
  {
    { 200, gathers_key },
    { 400, gathers_more },
    { 600, gathers_waiting },
    { 900, gathers_drawn },
    { 1000, NULL },
  };

  run("a", steps, 5);
}

/* test_display_full_after_five */

static void full_1(void)
{
  lv_label_set_text(g_label, "1");
}

static void full_2(void)
{
  lv_label_set_text(g_label, "2");
}

static void full_3(void)
{
  lv_label_set_text(g_label, "3");
}

static void full_4(void)
{
  lv_label_set_text(g_label, "4");
}

static void full_screen(void)
{
  int i;

  assert_int_equal(g_nupdates, 5);
  for (i = 0; i < 5; i++)
    {
      assert_false(g_updates[i].full);
    }

  display_screen(&g_display);
  lv_label_set_text(g_label, "5");
}

static void full_taken(void)
{
  assert_int_equal(g_nupdates, 6);
  assert_true(g_updates[5].full);
  display_screen(&g_display);
  lv_label_set_text(g_label, "6");
}

static void full_again_partial(void)
{
  assert_int_equal(g_nupdates, 7);
  assert_false(g_updates[6].full);
}

static void test_display_full_after_five(void **state)
{
  static const struct step_s steps[] =
  {
    { 200, full_1 },
    { 400, full_2 },
    { 600, full_3 },
    { 800, full_4 },
    { 1000, full_screen },
    { 1200, full_taken },
    { 1400, full_again_partial },
    { 1600, NULL },
  };

  run("0", steps, 8);
}

/* test_display_two_places: two labels apart change in one draw, the lower
 * one first: one update covers both
 */

static void two_change(void)
{
  assert_int_equal(g_nupdates, 1);
  lv_label_set_text(g_label, "lower");
  lv_label_set_text(g_upper, "upper");
}

static void two_check(void)
{
  assert_int_equal(g_nupdates, 2);
  assert_true(g_updates[1].h > 0);
  assert_true(g_updates[1].y <= 20 + 14);
  assert_true(g_updates[1].y + g_updates[1].h > 100);
  assert_true(g_updates[1].x <= 10);
  assert_true(g_updates[1].x + g_updates[1].w > 100);
}

static void test_display_two_places(void **state)
{
  static const struct step_s steps[] =
  {
    { 200, two_change },
    { 400, two_check },
    { 600, NULL },
  };

  run("a", steps, 3);
}

/* test_display_failed_update: an update the screen refused is sent again,
 * whole, with the next draw, half a second later (the frame already has
 * its pixels)
 */

static void failed_change(void)
{
  assert_int_equal(g_nupdates, 1);
  g_fail = 1;
  lv_label_set_text(g_label, "b");
}

static void failed_again(void)
{
  assert_int_equal(g_nupdates, 2);
  lv_label_set_text(g_upper, "y");
}

static void failed_check(void)
{
  assert_int_equal(g_nupdates, 3);
  assert_int_equal(g_updates[2].x, 0);
  assert_int_equal(g_updates[2].y, 0);
  assert_int_equal(g_updates[2].w, WIDTH);
  assert_int_equal(g_updates[2].h, HEIGHT);
}

static void test_display_failed_update(void **state)
{
  static const struct step_s steps[] =
  {
    { 200, failed_change },
    { 400, failed_again },
    { 1000, failed_check },
    { 1200, NULL },
  };

  run("a", steps, 4);
}

/* test_display_lvgl_timer: a timer of LVGL's made while the loop is idle
 * runs (glue.c)
 */

static void timer_fired(lv_timer_t *timer)
{
  g_fired++;
}

static void timer_make(void)
{
  lv_timer_t *timer = lv_timer_create(timer_fired, 30, NULL);

  lv_timer_set_repeat_count(timer, 1);
}

static void timer_check(void)
{
  assert_int_equal(g_fired, 1);
}

static void test_display_lvgl_timer(void **state)
{
  static const struct step_s steps[] =
  {
    { 200, timer_make },
    { 500, timer_check },
    { 600, NULL },
  };

  run("a", steps, 3);
}

/* test_display_failed_retried: with nothing else to draw, a refused update
 * is tried again, whole, half a second later
 */

static void retried_change(void)
{
  assert_int_equal(g_nupdates, 1);
  g_fail = 1;
  lv_label_set_text(g_label, "b");
}

static void retried_check(void)
{
  assert_int_equal(g_nupdates, 3);
  assert_int_equal(g_updates[2].w, WIDTH);
  assert_int_equal(g_updates[2].h, HEIGHT);
}

static void test_display_failed_retried(void **state)
{
  static const struct step_s steps[] =
  {
    { 200, retried_change },
    { 1000, retried_check },
    { 1200, NULL },
  };

  run("a", steps, 3);
}

/* test_display_glue_pool_full: an LVGL timer made while the loop has no
 * timer to spare waits; once one is free, the next made runs, and it too
 */

static FAR struct pnut_timer_s *g_fillers[32];
static int g_nfillers;

static void filler(FAR struct pnut_loop_s *loop,
                   FAR struct pnut_timer_s *timer, FAR void *arg)
{
}

static void pool_fill(void)
{
  g_nfillers = 0;
  while (g_nfillers < 32 &&
         pnut_timer_start(g_loop, 60000, 0, filler, NULL,
                          &g_fillers[g_nfillers]) == 0)
    {
      g_nfillers++;
    }

  assert_true(g_nfillers < 32);
  timer_make();
}

static void pool_free(void)
{
  int i;

  for (i = 0; i < g_nfillers; i++)
    {
      pnut_timer_cancel(g_loop, g_fillers[i]);
    }

  g_fired = 0;
  timer_make();
}

static void pool_check(void)
{
  assert_int_equal(g_fired, 2);
}

static void test_display_glue_pool_full(void **state)
{
  static const struct step_s steps[] =
  {
    { 200, pool_fill },
    { 400, pool_free },
    { 700, pool_check },
    { 800, NULL },
  };

  run("a", steps, 4);
}

/* test_display_one_try_a_draw: a screen that refuses again is tried once a
 * draw, not twice back to back
 */

static void once_change(void)
{
  assert_int_equal(g_nupdates, 1);
  g_fail = 2;
  lv_label_set_text(g_label, "b");
}

static void once_other(void)
{
  lv_label_set_text(g_upper, "y");
}

static void once_waiting(void)
{
  assert_int_equal(g_nupdates, 3);
}

static void once_check(void)
{
  assert_int_equal(g_nupdates, 4);
  assert_int_equal(g_updates[3].w, WIDTH);
  assert_int_equal(g_updates[3].h, HEIGHT);
}

static void test_display_one_try_a_draw(void **state)
{
  static const struct step_s steps[] =
  {
    { 200, once_change },
    { 400, once_other },
    { 950, once_waiting },
    { 1600, once_check },
    { 1700, NULL },
  };

  run("a", steps, 5);
}

/* test_display_draw_now: what is pending is drawn at once when asked
 * (before reading the screen back)
 */

static void now_key(void)
{
  assert_int_equal(g_nupdates, 1);
  display_input(&g_display);
  lv_label_set_text(g_label, "now");
  display_draw_now(&g_display);
  assert_int_equal(g_nupdates, 2);

  /* Nothing pending: nothing drawn */

  display_draw_now(&g_display);
  assert_int_equal(g_nupdates, 2);
}

static void test_display_draw_now(void **state)
{
  static const struct step_s steps[] =
  {
    { 200, now_key },
    { 800, NULL },
  };

  run("a", steps, 2);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_display_draws_changes),
    cmocka_unit_test(test_display_gathers_input),
    cmocka_unit_test(test_display_full_after_five),
    cmocka_unit_test(test_display_two_places),
    cmocka_unit_test(test_display_failed_update),
    cmocka_unit_test(test_display_lvgl_timer),
    cmocka_unit_test(test_display_failed_retried),
    cmocka_unit_test(test_display_glue_pool_full),
    cmocka_unit_test(test_display_one_try_a_draw),
    cmocka_unit_test(test_display_draw_now),
  };

  return cmocka_run_group_tests_name("ui_display", tests, NULL, NULL);
}
