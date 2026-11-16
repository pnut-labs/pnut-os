/****************************************************************************
 * pnut-os/tests/unit/test_ui_applist.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* An app's list as the system UI keeps it (RFC 0037), without LVGL: rows
 * loaded from the first, pages of APPLIST_PER_PAGE, a page of rows asked
 * for ahead of the focus, at most APPLIST_KEPT kept.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "applist.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static pnut_ui_row_t g_rows[40];

static void make_rows(uint32_t first, int n)
{
  int i;

  memset(g_rows, 0, sizeof(g_rows));
  for (i = 0; i < n; i++)
    {
      g_rows[i].id = 1000 + first + i;
      snprintf(g_rows[i].title, sizeof(g_rows[i].title), "Row %u",
               (unsigned)(first + i));
    }
}

/****************************************************************************
 * Name: test_applist_pages
 *
 * Description:
 *   32 rows, the first 20 given: 6 a page, the header's page out of 6;
 *   the focus moves row by row and page by page within the rows loaded.
 ****************************************************************************/

static void test_applist_pages(void **state)
{
  struct applist_s l;
  int i;

  make_rows(0, 20);
  assert_int_equal(applist_init(&l, 32, g_rows, 20, 0), 0);
  assert_int_equal(l.loaded, 20);
  assert_int_equal(applist_page(&l), 0);
  assert_int_equal(applist_npages(&l), 6);
  assert_int_equal(applist_focused(&l)->id, 1000);

  for (i = 0; i < 6; i++)
    {
      assert_true(applist_move(&l, 1));
    }

  assert_int_equal(l.focus, 6);
  assert_int_equal(applist_page(&l), 1);

  assert_true(applist_page_move(&l, -1));
  assert_int_equal(l.focus, 0);
  assert_false(applist_page_move(&l, -1));
  assert_false(applist_move(&l, -1));

  /* The last page of those loaded: not past the rows loaded */

  assert_true(applist_page_move(&l, 1));
  assert_true(applist_page_move(&l, 1));
  assert_true(applist_page_move(&l, 1));
  assert_int_equal(l.focus, 18);
  assert_false(applist_page_move(&l, 1));
  assert_true(applist_move(&l, 1));
  assert_int_equal(l.focus, 19);
  assert_false(applist_move(&l, 1));
  applist_free(&l);
}

/****************************************************************************
 * Name: test_applist_focus
 *
 * Description:
 *   The focus a Show gives, by row id; one not among the rows loaded is
 *   invalid; an empty list has none.
 ****************************************************************************/

static void test_applist_focus(void **state)
{
  struct applist_s l;

  make_rows(0, 20);
  assert_int_equal(applist_init(&l, 32, g_rows, 20, 1013), 0);
  assert_int_equal(l.focus, 13);
  assert_int_equal(applist_page(&l), 2);
  applist_free(&l);

  assert_int_equal(applist_init(&l, 32, g_rows, 20, 999), -EINVAL);

  /* More rows given than the list has, or a row id 0: invalid */

  assert_int_equal(applist_init(&l, 10, g_rows, 20, 0), -EINVAL);
  g_rows[3].id = 0;
  assert_int_equal(applist_init(&l, 32, g_rows, 20, 0), -EINVAL);

  assert_int_equal(applist_init(&l, 0, g_rows, 0, 0), 0);
  assert_null(applist_focused(&l));
  assert_int_equal(applist_npages(&l), 1);
  assert_false(applist_move(&l, 1));
  applist_free(&l);
}

/****************************************************************************
 * Name: test_applist_need
 *
 * Description:
 *   A page is asked for once the focus is within a page of the rows'
 *   end, from the first row not loaded, and only once; Rows from another
 *   row are dropped; Rows from the one asked append; nothing is asked past
 *   APPLIST_KEPT or the list's count.
 ****************************************************************************/

static void test_applist_need(void **state)
{
  struct applist_s l;
  uint32_t first;
  int i;

  make_rows(0, 20);
  assert_int_equal(applist_init(&l, 32, g_rows, 20, 0), 0);
  assert_false(applist_need(&l, &first));

  for (i = 0; i < 13; i++)
    {
      applist_move(&l, 1);
    }

  assert_false(applist_need(&l, &first));
  applist_move(&l, 1);                    /* 14: within 6 of 20 */
  assert_true(applist_need(&l, &first));
  assert_int_equal(first, 20);
  assert_false(applist_need(&l, &first)); /* Asked once */

  make_rows(40, 12);
  assert_int_equal(applist_rows(&l, 40, g_rows, 12), -EINVAL);
  assert_int_equal(l.loaded, 20);

  make_rows(20, 12);
  assert_int_equal(applist_rows(&l, 20, g_rows, 12), 0);
  assert_int_equal(l.loaded, 32);
  assert_int_equal(l.rows[31].id, 1031);

  /* All loaded: nothing more to ask */

  for (i = 0; i < 20; i++)
    {
      applist_move(&l, 1);
    }

  assert_int_equal(l.focus, 31);
  assert_false(applist_need(&l, &first));

  /* Rows not asked for: dropped */

  assert_int_equal(applist_rows(&l, 32, g_rows, 1), -EINVAL);
  applist_free(&l);

  /* An answer with no rows: the list ends there, and nothing more is
   * asked
   */

  make_rows(0, 20);
  assert_int_equal(applist_init(&l, 50, g_rows, 20, 1019), 0);
  assert_true(applist_need(&l, &first));
  assert_int_equal(applist_rows(&l, 20, g_rows, 0), 0);
  assert_false(applist_need(&l, &first));
  assert_int_equal(applist_npages(&l), 4);
  applist_free(&l);
}

/****************************************************************************
 * Name: test_applist_kept
 *
 * Description:
 *   A list of 500 rows keeps APPLIST_KEPT: nothing is asked past them,
 *   and Rows that would go past are cut there.
 ****************************************************************************/

static void test_applist_kept(void **state)
{
  struct applist_s l;
  uint32_t first;
  int page;

  make_rows(0, 20);
  assert_int_equal(applist_init(&l, 500, g_rows, 20, 0), 0);
  assert_int_equal(applist_npages(&l),
                   (APPLIST_KEPT + APPLIST_PER_PAGE - 1) / APPLIST_PER_PAGE);

  for (page = 0; l.loaded < APPLIST_KEPT; page++)
    {
      while (!applist_need(&l, &first))
        {
          assert_true(applist_move(&l, 1));
        }

      assert_int_equal(first, l.loaded);
      make_rows(first, 20);
      assert_int_equal(applist_rows(&l, first, g_rows, 20), 0);
    }

  assert_int_equal(l.loaded, APPLIST_KEPT);
  while (applist_move(&l, 1))
    {
      assert_false(applist_need(&l, &first));
    }

  assert_int_equal(l.focus, APPLIST_KEPT - 1);
  applist_free(&l);
}

/****************************************************************************
 * Name: test_applist_update
 *
 * Description:
 *   An Update of a row replaces the row of that id, wherever it is
 *   loaded; an id not loaded is no change.
 ****************************************************************************/

static void test_applist_update(void **state)
{
  struct applist_s l;
  pnut_ui_row_t row;

  make_rows(0, 20);
  assert_int_equal(applist_init(&l, 20, g_rows, 20, 0), 0);

  memset(&row, 0, sizeof(row));
  row.id = 1007;
  strcpy(row.title, "Renamed");
  assert_int_equal(applist_update(&l, &row), 7);
  assert_string_equal(l.rows[7].title, "Renamed");

  row.id = 5;
  assert_int_equal(applist_update(&l, &row), -ENOENT);
  applist_free(&l);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_applist_pages),
    cmocka_unit_test(test_applist_focus),
    cmocka_unit_test(test_applist_need),
    cmocka_unit_test(test_applist_kept),
    cmocka_unit_test(test_applist_update),
  };

  return cmocka_run_group_tests_name("ui_applist", tests, NULL, NULL);
}
