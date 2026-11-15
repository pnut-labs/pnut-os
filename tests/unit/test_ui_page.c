/****************************************************************************
 * pnut-os/tests/unit/test_ui_page.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Grids and lists in pages (page.c): e-paper pages, never scrolls
 * (RFC 0019).  W/S and A/D flow across pages; Space and Shift+Space go to
 * the next page's first item or the previous one's; in a list, which has
 * no row, A/D turn pages.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "keymap.h"
#include "page.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void at(struct page_s *p, uint16_t focus)
{
  p->focus = focus;
  p->page  = focus / p->per;
}

static void test_page_menu(void **state)
{
  struct page_s p;

  /* The Menu: 19 tiles, 3 columns, 4 rows */

  page_grid(&p, 19, 3, 4);
  assert_int_equal(page_count(&p), 2);
  assert_int_equal(p.focus, 0);
  assert_int_equal(p.page, 0);

  at(&p, 11);
  assert_true(page_move(&p, UI_ACT_RIGHT));
  assert_int_equal(p.focus, 12);
  assert_int_equal(p.page, 1);
  assert_true(page_move(&p, UI_ACT_LEFT));
  assert_int_equal(p.focus, 11);
  assert_int_equal(p.page, 0);

  at(&p, 9);
  assert_true(page_move(&p, UI_ACT_DOWN));
  assert_int_equal(p.focus, 12);
  assert_true(page_move(&p, UI_ACT_UP));
  assert_int_equal(p.focus, 9);

  at(&p, 16);
  assert_false(page_move(&p, UI_ACT_DOWN));
  assert_int_equal(p.focus, 16);
  at(&p, 15);
  assert_true(page_move(&p, UI_ACT_DOWN));
  assert_int_equal(p.focus, 18);

  at(&p, 4);
  assert_true(page_move(&p, UI_ACT_PAGE_NEXT));
  assert_int_equal(p.focus, 12);
  assert_int_equal(p.page, 1);
  assert_false(page_move(&p, UI_ACT_PAGE_NEXT));
  assert_true(page_move(&p, UI_ACT_PAGE_PREV));
  assert_int_equal(p.focus, 0);
  assert_false(page_move(&p, UI_ACT_PAGE_PREV));

  /* The edges of all */

  at(&p, 0);
  assert_false(page_move(&p, UI_ACT_LEFT));
  assert_false(page_move(&p, UI_ACT_UP));
  at(&p, 18);
  assert_false(page_move(&p, UI_ACT_RIGHT));
  assert_false(page_move(&p, UI_ACT_OPEN));
}

static void test_page_small(void **state)
{
  struct page_s p;

  /* A folder: 2 tiles, one page */

  page_grid(&p, 2, 3, 4);
  assert_int_equal(page_count(&p), 1);
  assert_true(page_move(&p, UI_ACT_RIGHT));
  assert_int_equal(p.focus, 1);
  assert_false(page_move(&p, UI_ACT_DOWN));
  assert_false(page_move(&p, UI_ACT_PAGE_NEXT));

  /* Nothing at all */

  page_grid(&p, 0, 3, 4);
  assert_int_equal(page_count(&p), 1);
  assert_false(page_move(&p, UI_ACT_RIGHT));
}

static void test_pagelist(void **state)
{
  /* A heading, then 9 rows: 24 + 5 x 40 fit in 244, the sixth row does
   * not
   */

  static const uint16_t heights[10] =
  {
    24, 40, 40, 40, 40, 40, 40, 40, 40, 40
  };

  static const bool focusable[10] =
  {
    false, true, true, true, true, true, true, true, true, true
  };

  struct pagelist_s l;

  assert_int_equal(pagelist_layout(&l, heights, focusable, 10, 244), 0);
  assert_int_equal(l.npages, 2);
  assert_int_equal(l.first[0], 0);
  assert_int_equal(l.first[1], 6);
  assert_int_equal(l.first[2], 10);

  /* The first focusable is focused: the heading is skipped */

  assert_int_equal(l.focus, 1);
  assert_false(pagelist_move(&l, UI_ACT_UP));
  assert_int_equal(l.focus, 1);

  assert_true(pagelist_move(&l, UI_ACT_DOWN));
  assert_int_equal(l.focus, 2);

  l.focus = 5;
  assert_true(pagelist_move(&l, UI_ACT_DOWN));
  assert_int_equal(l.focus, 6);
  assert_int_equal(l.page, 1);
  assert_true(pagelist_move(&l, UI_ACT_UP));
  assert_int_equal(l.focus, 5);
  assert_int_equal(l.page, 0);

  /* No row: A and D turn pages, to the page's first focusable */

  assert_true(pagelist_move(&l, UI_ACT_RIGHT));
  assert_int_equal(l.page, 1);
  assert_int_equal(l.focus, 6);
  assert_false(pagelist_move(&l, UI_ACT_RIGHT));
  assert_true(pagelist_move(&l, UI_ACT_LEFT));
  assert_int_equal(l.page, 0);
  assert_int_equal(l.focus, 1);
  assert_true(pagelist_move(&l, UI_ACT_PAGE_NEXT));
  assert_int_equal(l.focus, 6);
  assert_true(pagelist_move(&l, UI_ACT_PAGE_PREV));
  assert_int_equal(l.focus, 1);

  l.focus = 9;
  l.page  = 1;
  assert_false(pagelist_move(&l, UI_ACT_DOWN));
}

static void test_pagelist_tall(void **state)
{
  /* A row taller than the page still gets a page of its own */

  static const uint16_t heights[3] =
  {
    40, 300, 40
  };

  struct pagelist_s l;

  assert_int_equal(pagelist_layout(&l, heights, NULL, 3, 244), 0);
  assert_int_equal(l.npages, 3);
  assert_int_equal(l.first[1], 1);
  assert_int_equal(l.first[2], 2);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_page_menu),
    cmocka_unit_test(test_page_small),
    cmocka_unit_test(test_pagelist),
    cmocka_unit_test(test_pagelist_tall),
  };

  return cmocka_run_group_tests_name("ui_page", tests, NULL, NULL);
}
