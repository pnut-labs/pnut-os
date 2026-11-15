/****************************************************************************
 * pnut-os/tests/unit/test_ui_lvgl.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* LVGL as the UI's tests build it (lv_conf.h): it draws a 1-bit frame on
 * the computer, with no display.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include <lvgl/lvgl.h>

#include "theme.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIDTH    240
#define HEIGHT   320
#define PALETTE  8                      /* I1 buffers start with it */
#define STRIDE   (WIDTH / 8)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint8_t g_buf[PALETTE + STRIDE * HEIGHT];
static uint8_t g_frame[STRIDE * HEIGHT];
static int g_flushes;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
  int32_t w = lv_area_get_width(area);
  uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_I1);
  int32_t y;

  /* An area's rows, as wide as the area, after the palette */

  for (y = area->y1; y <= area->y2; y++)
    {
      memcpy(g_frame + y * STRIDE + area->x1 / 8,
             px + PALETTE + (y - area->y1) * stride, (w + 7) / 8);
    }

  g_flushes++;
  lv_display_flush_ready(disp);
}

static void test_lvgl_draws_one_bit(void **state)
{
  lv_display_t *disp;
  lv_obj_t *label;
  size_t i;
  bool ink = false;

  lv_init();
  disp = lv_display_create(WIDTH, HEIGHT);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_I1);
  lv_display_set_buffers(disp, g_buf, NULL, sizeof(g_buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush);

  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_white(), 0);
  lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
  label = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_color(label, lv_color_black(), 0);
  lv_obj_set_style_text_font(label, &g_ui_sans_14, 0);
  lv_label_set_text(label, "pnut");

  memset(g_frame, 0xff, sizeof(g_frame));
  lv_refr_now(disp);

  assert_true(g_flushes > 0);
  for (i = 0; i < sizeof(g_frame); i++)
    {
      ink |= g_frame[i] != 0xff;
    }

  assert_true(ink);
  lv_deinit();
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_lvgl_draws_one_bit),
  };

  return cmocka_run_group_tests_name("ui_lvgl", tests, NULL, NULL);
}
