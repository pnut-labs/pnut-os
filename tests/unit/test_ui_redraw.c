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
  };

  return cmocka_run_group_tests_name("ui_redraw", tests, NULL, NULL);
}
