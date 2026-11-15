/****************************************************************************
 * pnut-os/tests/unit/test_ui_theme.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The system UI's fonts and icons, made at build time (tools/mkfont.py,
 * tools/mkicons.py): the letters the UI needs are there.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <lvgl/lvgl.h>
#include "theme.h"
#include "icons.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The fonts of text, which carry Latin, and the clock's and the PIN's,
 * which carry digits
 */

static const lv_font_t * const g_text[] =
{
  &g_ui_sans_11, &g_ui_sans_12, &g_ui_sans_13, &g_ui_sans_14,
  &g_ui_sans_bold_12, &g_ui_sans_bold_13, &g_ui_sans_bold_14,
  &g_ui_sans_bold_16, &g_ui_mono_bold_10, &g_ui_mono_bold_11,
  &g_ui_mono_bold_12,
};

static const lv_font_t * const g_digits[] =
{
  &g_ui_mono_bold_26, &g_ui_mono_bold_36,
};

static const lv_font_t * const g_icons[] =
{
  &g_ui_icons_11, &g_ui_icons_12, &g_ui_icons_14, &g_ui_icons_16,
  &g_ui_icons_18, &g_ui_icons_20,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool has(const lv_font_t *font, uint32_t cp)
{
  lv_font_glyph_dsc_t dsc;

  return lv_font_get_glyph_dsc(font, &dsc, cp, 0) && dsc.adv_w > 0;
}

static void test_theme_text_glyphs(void **state)
{
  size_t i;

  for (i = 0; i < sizeof(g_text) / sizeof(g_text[0]); i++)
    {
      assert_true(has(g_text[i], 'A'));
      assert_true(has(g_text[i], 0x0105));     /* ą */
      assert_true(has(g_text[i], 0x00b7));     /* · */
      assert_true(has(g_text[i], 0x2022));     /* • */
      assert_true(has(g_text[i], 0x2026));     /* … */
      assert_true(has(g_text[i], 0x2192));     /* → */
    }

  assert_true(g_ui_sans_bold_16.line_height >= 16);
  assert_true(g_ui_sans_bold_16.line_height <= 22);
}

static void test_theme_digit_glyphs(void **state)
{
  size_t i;
  uint32_t c;

  for (i = 0; i < sizeof(g_digits) / sizeof(g_digits[0]); i++)
    {
      for (c = '0'; c <= '9'; c++)
        {
          assert_true(has(g_digits[i], c));
        }

      assert_true(has(g_digits[i], ':'));
      assert_true(has(g_digits[i], 0x2022));   /* • */
    }
}

static void test_theme_icons(void **state)
{
  size_t i;

  for (i = 0; i < sizeof(g_icons) / sizeof(g_icons[0]); i++)
    {
      assert_true(has(g_icons[i], 0xe000));
      assert_int_equal(g_icons[i]->line_height,
                       i == 0 ? 11 : i == 1 ? 12 : i == 2 ? 14 :
                       i == 3 ? 16 : i == 4 ? 18 : 20);
    }

  /* The names are UTF-8 strings of the icons' code points, in the order of
   * the SVGs' names: the first and the last (a dropped SVG shows here)
   */

  assert_string_equal(UI_ICON_AI_CHAT, "\xee\x80\x80");
  assert_string_equal(UI_ICON_WIFI, "\xee\x80\x9c");
}

/* Every glyph of a font, decoded by LVGL as it draws it: the tools trim a
 * glyph's box to its ink, so a right decode touches the box's four edges;
 * and the box lies within the font's line, which labels clip to.
 */

static void check_glyphs(const lv_font_t *font)
{
  lv_font_glyph_dsc_t dsc;
  lv_draw_buf_t *buf;
  const uint8_t *px;
  uint32_t stride;
  uint32_t cp;
  int x;
  int y;
  bool top;
  bool bottom;
  bool left;
  bool right;
  int n = 0;

  for (cp = 0x20; cp < 0xe100; cp++)
    {
      if (!lv_font_get_glyph_dsc(font, &dsc, cp, 0) ||
          dsc.box_w == 0 || dsc.box_h == 0)
        {
          continue;
        }

      if (dsc.ofs_y < -font->base_line ||
          dsc.ofs_y + dsc.box_h > font->line_height - font->base_line)
        {
          print_message("U+%04lx: outside its line\n", (unsigned long)cp);
        }

      assert_true(dsc.ofs_y >= -font->base_line);
      assert_true(dsc.ofs_y + dsc.box_h <=
                  font->line_height - font->base_line);

      buf = lv_draw_buf_create(dsc.box_w, dsc.box_h, LV_COLOR_FORMAT_A8,
                               LV_STRIDE_AUTO);
      assert_non_null(buf);
      assert_ptr_equal(lv_font_get_glyph_bitmap(&dsc, buf), buf);
      px = buf->data;
      stride = lv_draw_buf_width_to_stride(dsc.box_w, LV_COLOR_FORMAT_A8);

      top = bottom = left = right = false;
      for (y = 0; y < dsc.box_h; y++)
        {
          for (x = 0; x < dsc.box_w; x++)
            {
              if (px[y * stride + x] != 0)
                {
                  top    |= y == 0;
                  bottom |= y == dsc.box_h - 1;
                  left   |= x == 0;
                  right  |= x == dsc.box_w - 1;
                }
            }
        }

      if (!(top && bottom && left && right))
        {
          print_message("U+%04lx: ink off its box\n", (unsigned long)cp);
        }

      assert_true(top && bottom && left && right);
      lv_draw_buf_destroy(buf);
      n++;
    }

  assert_true(n > 0);
}

static void test_theme_glyphs_decode(void **state)
{
  size_t i;

  lv_init();
  for (i = 0; i < sizeof(g_text) / sizeof(g_text[0]); i++)
    {
      check_glyphs(g_text[i]);
    }

  for (i = 0; i < sizeof(g_digits) / sizeof(g_digits[0]); i++)
    {
      check_glyphs(g_digits[i]);
    }

  for (i = 0; i < sizeof(g_icons) / sizeof(g_icons[0]); i++)
    {
      check_glyphs(g_icons[i]);
    }

  lv_deinit();
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_theme_text_glyphs),
    cmocka_unit_test(test_theme_digit_glyphs),
    cmocka_unit_test(test_theme_icons),
    cmocka_unit_test(test_theme_glyphs_decode),
  };

  return cmocka_run_group_tests_name("ui_theme", tests, NULL, NULL);
}
