/****************************************************************************
 * pnut-os/tests/unit/test_ui_redraw.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* When the system UI draws on its e-paper screen (redraw.c): key presses
 * gathered for half a second into one redraw, other changes at once, and a
 * full refresh after five partial ones, at a change of screen, never while
 * typing.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "redraw.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void test_redraw_gathers_input(void **state)
{
  struct redraw_s r;

  redraw_init(&r);
  assert_true(redraw_next(&r) == UINT64_MAX);

  redraw_input(&r, 1000);
  assert_int_equal(redraw_next(&r), 1500);

  /* Later presses do not push the redraw back */

  redraw_input(&r, 1200);
  assert_int_equal(redraw_next(&r), 1500);

  /* Nor does a change they caused: it joins */

  redraw_changed(&r, 1300);
  assert_int_equal(redraw_next(&r), 1500);

  assert_int_equal(redraw_take(&r, 1499), REDRAW_NONE);
  assert_int_equal(redraw_take(&r, 1500), REDRAW_PARTIAL);
  redraw_done(&r, false);
  assert_true(redraw_next(&r) == UINT64_MAX);
  assert_int_equal(redraw_take(&r, 1600), REDRAW_NONE);
}

static void test_redraw_change_at_once(void **state)
{
  struct redraw_s r;

  redraw_init(&r);

  /* A change no key caused (a topic, the clock) is drawn at once */

  redraw_changed(&r, 2000);
  assert_int_equal(redraw_next(&r), 2000);
  redraw_changed(&r, 2001);
  assert_int_equal(redraw_next(&r), 2000);

  /* A key pressed before it is drawn opens the window: both wait */

  redraw_input(&r, 2002);
  assert_int_equal(redraw_next(&r), 2502);
  assert_int_equal(redraw_take(&r, 2502), REDRAW_PARTIAL);
}

static void test_redraw_full(void **state)
{
  struct redraw_s r;
  int i;

  redraw_init(&r);
  for (i = 0; i < 5; i++)
    {
      redraw_screen(&r);
      redraw_changed(&r, 100 * i);
      assert_int_equal(redraw_take(&r, 100 * i), REDRAW_PARTIAL);
      redraw_done(&r, false);
    }

  /* Due now, but only at a change of screen */

  redraw_changed(&r, 1000);
  assert_int_equal(redraw_take(&r, 1000), REDRAW_PARTIAL);
  redraw_done(&r, false);

  /* Never while typing */

  redraw_screen(&r);
  redraw_typing(&r, true);
  redraw_changed(&r, 1100);
  assert_int_equal(redraw_take(&r, 1100), REDRAW_PARTIAL);
  redraw_done(&r, false);

  redraw_typing(&r, false);
  redraw_screen(&r);
  redraw_changed(&r, 1200);
  assert_int_equal(redraw_take(&r, 1200), REDRAW_FULL);
  redraw_done(&r, true);

  /* The count starts again */

  redraw_screen(&r);
  redraw_changed(&r, 1300);
  assert_int_equal(redraw_take(&r, 1300), REDRAW_PARTIAL);
}

static void test_redraw_nothing_drawn(void **state)
{
  struct redraw_s r;
  int i;

  /* A redraw that drew nothing (no update went out) is not counted */

  redraw_init(&r);
  for (i = 0; i < 10; i++)
    {
      redraw_changed(&r, i);
      assert_int_equal(redraw_take(&r, i), REDRAW_PARTIAL);
    }

  redraw_screen(&r);
  redraw_changed(&r, 20);
  assert_int_equal(redraw_take(&r, 20), REDRAW_PARTIAL);
}

/* An app's change nobody's key caused (RFC 0037, E-paper): gathered for
 * 1 s, the app's screen drawn at most every 2 s
 */

static void test_redraw_app_gathers(void **state)
{
  struct redraw_s r;

  redraw_init(&r);
  redraw_app(&r, 1000, REDRAW_APP_GATHER, 1000, 2000);
  assert_int_equal(redraw_next(&r), 2000);
  redraw_app(&r, 1500, REDRAW_APP_GATHER, 1000, 2000);
  assert_int_equal(redraw_next(&r), 2000);
  assert_int_equal(redraw_take(&r, 1999), REDRAW_NONE);
  assert_int_equal(redraw_take(&r, 2000), REDRAW_PARTIAL);
  redraw_done(&r, false);

  /* The next no sooner than 2 s after that one */

  redraw_app(&r, 2100, REDRAW_APP_GATHER, 1000, 2000);
  assert_int_equal(redraw_next(&r), 4000);
  assert_int_equal(redraw_take(&r, 4000), REDRAW_PARTIAL);
  redraw_done(&r, false);

  /* Long after: 1 s again */

  redraw_app(&r, 9000, REDRAW_APP_GATHER, 1000, 2000);
  assert_int_equal(redraw_next(&r), 10000);
}

/* One that comes in a key's window joins its redraw */

static void test_redraw_app_joins_key(void **state)
{
  struct redraw_s r;

  redraw_init(&r);
  redraw_input(&r, 0);
  redraw_app(&r, 100, REDRAW_APP_GATHER, 1000, 2000);
  assert_int_equal(redraw_next(&r), 500);
  redraw_app(&r, 200, REDRAW_APP_NOW, 1000, 2000);
  assert_int_equal(redraw_next(&r), 500);

  /* A key while one is gathered: both in the key's redraw */

  assert_int_equal(redraw_take(&r, 500), REDRAW_PARTIAL);
  redraw_done(&r, false);
  redraw_app(&r, 3000, REDRAW_APP_GATHER, 1000, 2000);
  redraw_input(&r, 3100);
  assert_int_equal(redraw_next(&r), 3600);
}

/* Now: at once, a gathered one with it; quiet: with the next redraw */

static void test_redraw_app_now_quiet(void **state)
{
  struct redraw_s r;

  redraw_init(&r);
  redraw_app(&r, 1000, REDRAW_APP_GATHER, 1000, 2000);
  redraw_app(&r, 1200, REDRAW_APP_NOW, 1000, 2000);
  assert_int_equal(redraw_next(&r), 1200);
  assert_int_equal(redraw_take(&r, 1200), REDRAW_PARTIAL);
  redraw_done(&r, false);
  assert_true(redraw_next(&r) == UINT64_MAX);

  redraw_app(&r, 5000, REDRAW_APP_QUIET, 1000, 2000);
  assert_true(redraw_next(&r) == UINT64_MAX);
  assert_true(r.quiet);
  redraw_changed(&r, 6000);
  assert_int_equal(redraw_take(&r, 6000), REDRAW_PARTIAL);
  assert_false(r.quiet);
}

/* Switching apps and going Home: a full refresh, never while typing */

static void test_redraw_switch(void **state)
{
  struct redraw_s r;

  redraw_init(&r);
  redraw_switch(&r);
  redraw_changed(&r, 100);
  assert_int_equal(redraw_take(&r, 100), REDRAW_FULL);
  redraw_done(&r, true);
  redraw_changed(&r, 200);
  assert_int_equal(redraw_take(&r, 200), REDRAW_PARTIAL);
  redraw_done(&r, false);

  redraw_typing(&r, true);
  redraw_switch(&r);
  redraw_changed(&r, 300);
  assert_int_equal(redraw_take(&r, 300), REDRAW_PARTIAL);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_redraw_gathers_input),
    cmocka_unit_test(test_redraw_change_at_once),
    cmocka_unit_test(test_redraw_full),
    cmocka_unit_test(test_redraw_nothing_drawn),
    cmocka_unit_test(test_redraw_app_gathers),
    cmocka_unit_test(test_redraw_app_joins_key),
    cmocka_unit_test(test_redraw_app_now_quiet),
    cmocka_unit_test(test_redraw_switch),
  };

  return cmocka_run_group_tests_name("ui_redraw", tests, NULL, NULL);
}
