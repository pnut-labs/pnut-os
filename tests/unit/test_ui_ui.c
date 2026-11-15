/****************************************************************************
 * pnut-os/tests/unit/test_ui_ui.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The ui module (ui.c): ready as it starts, whether Board answers or not
 * (a program that is never ready keeps a firmware update on trial from
 * being confirmed, RFC 0013), and the screen shown once there is one.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "input.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIDTH   240
#define HEIGHT  320
#define STRIDE  (WIDTH / 8)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint8_t g_frame[STRIDE * HEIGHT];
static struct ui_s g_ui;
static struct input_s g_input;
static int g_updates;
static bool g_ready;

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

static void check(FAR struct pnut_loop_s *loop,
                  FAR struct pnut_timer_s *timer, FAR void *arg)
{
  g_ready = pnut_loop_ready(loop);
  pnut_loop_stop(loop, 0);
}

static void run(FAR const char *board,
                FAR const struct display_config_s *screen)
{
  struct pnut_loop_config_s lconfig;
  struct input_config_s iconfig;
  struct ui_config_s uconfig;
  FAR struct pnut_loop_s *loop;
  char dir[] = "/tmp/pnut-ui-XXXXXX";

  assert_non_null(mkdtemp(dir));
  pnut_loop_defaults(&lconfig);
  lconfig.rundir = dir;
  assert_int_equal(pnut_loop_create(&lconfig, &loop), 0);

  memset(&iconfig, 0, sizeof(iconfig));
  iconfig.display = &g_ui.display;
  input_init(&g_input, &iconfig);

  memset(&uconfig, 0, sizeof(uconfig));
  uconfig.board  = board;
  uconfig.screen = screen;
  uconfig.input  = &g_input;
  ui_init(&g_ui, &uconfig);

  assert_int_equal(pnut_module_add(loop, &g_ui.module), 0);
  assert_int_equal(pnut_module_add(loop, &g_input.module), 0);

  g_updates = 0;
  g_ready   = false;
  assert_int_equal(pnut_timer_start(loop, 300, 0, check, NULL, NULL), 0);
  assert_int_equal(pnut_loop_run(loop), 0);
  pnut_loop_destroy(loop);
  rmdir(dir);
}

static void test_ui_screen(void **state)
{
  struct display_config_s screen;

  memset(g_frame, 0xff, sizeof(g_frame));
  memset(&screen, 0, sizeof(screen));
  screen.frame  = g_frame;
  screen.stride = STRIDE;
  screen.width  = WIDTH;
  screen.height = HEIGHT;
  screen.ops    = &g_ops;

  run(NULL, &screen);
  assert_true(g_ready);
  assert_int_equal(g_updates, 1);
}

static void test_ui_no_board(void **state)
{
  /* No Board's service: ready all the same, no screen */

  run("nosuchboard", NULL);
  assert_true(g_ready);
  assert_false(g_ui.shown);
  assert_int_equal(g_updates, 0);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_ui_screen),
    cmocka_unit_test(test_ui_no_board),
  };

  return cmocka_run_group_tests_name("ui_ui", tests, NULL, NULL);
}
