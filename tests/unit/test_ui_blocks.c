/****************************************************************************
 * pnut-os/tests/unit/test_ui_blocks.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The system UI's blocks (blocks.c): the header with the device's state,
 * the softkeys, tiles; a tap on one reaches its screen as its key would.
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

#include "blocks.h"
#include "icons.h"
#include "keymap.h"
#include "status.h"
#include "theme.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIDTH    240
#define HEIGHT   320
#define PALETTE  8

/* 2026-09-22 12:06:59 UTC */

#define BLOCKS_NOW   1790078819

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint8_t g_buf[PALETTE + WIDTH / 8 * HEIGHT];
static int g_invalidated;
static uint32_t g_keys[8];
static int g_nkeys;
static uint16_t g_ids[8];
static int g_nids;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
  lv_display_flush_ready(disp);
}

static void invalidated(lv_event_t *e)
{
  g_invalidated++;
}

static void on_key(lv_event_t *e)
{
  g_keys[g_nkeys++] = lv_event_get_key(e);
}

static void on_block(lv_event_t *e)
{
  g_ids[g_nids++] = (uint16_t)(uintptr_t)lv_event_get_param(e);
}

static lv_obj_t *start(void)
{
  lv_display_t *disp;
  lv_obj_t *scr;

  lv_init();
  disp = lv_display_create(WIDTH, HEIGHT);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_I1);
  lv_display_set_buffers(disp, g_buf, NULL, sizeof(g_buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush);
  lv_display_add_event_cb(disp, invalidated, LV_EVENT_INVALIDATE_AREA,
                          NULL);
  theme_init();

  scr = lv_screen_active();
  theme_screen(scr);
  lv_obj_add_event_cb(scr, on_key, LV_EVENT_KEY, NULL);
  lv_obj_add_event_cb(scr, on_block, g_ui_event_block, NULL);
  g_nkeys = 0;
  g_nids  = 0;
  return scr;
}

static void test_blocks_header(void **state)
{
  struct ui_status_s st;
  lv_obj_t *header;
  struct tm tm;

  header = block_header(start(), UI_ICON_MENU, "Menu");

  /* Before any topic: nothing known, nothing lit */

  memset(&st, 0, sizeof(st));
  pnut_tz_parse("CET-1CEST,M3.5.0,M10.5.0/3", &st.tz);
  status_local(&st, BLOCKS_NOW, &tm);
  block_header_status(header, &st, &tm);
  assert_string_equal(block_header_text(header, BLOCK_HEADER_TIME),
                      "\xe2\x80\x94");
  assert_string_equal(block_header_text(header, BLOCK_HEADER_BATTERY),
                      "\xe2\x80\x94");
  assert_int_equal(block_header_lit(header, false), 0);
  assert_int_equal(block_header_lit(header, true), 0);
  assert_false(block_header_wifi(header));

  /* Known: Telephony's 3 bars light 4 steps; 84 % charging */

  st.has_time  = true;
  st.has_tel   = true;
  st.tel.modem = PNUT_TELEPHONY_INFO_MODEM_ON;
  st.tel.bars  = 3;
  st.has_wifi  = true;
  st.wifi.state = PNUT_WIFI_INFO_STATE_JOINED;
  st.has_battery = true;
  st.battery   = 84;
  st.charging  = true;
  block_header_status(header, &st, &tm);
  assert_string_equal(block_header_text(header, BLOCK_HEADER_TIME),
                      "14:06");
  assert_string_equal(block_header_text(header, BLOCK_HEADER_BATTERY),
                      "84%+");
  assert_int_equal(block_header_lit(header, false), 4);
  assert_int_equal(block_header_lit(header, true), 5);
  assert_true(block_header_wifi(header));

  /* The modem off: no signal lit */

  st.tel.modem = PNUT_TELEPHONY_INFO_MODEM_OFF;
  block_header_status(header, &st, &tm);
  assert_int_equal(block_header_lit(header, false), 0);

  /* The page, shown from two pages */

  block_header_page(header, 0, 1);
  assert_string_equal(block_header_text(header, BLOCK_HEADER_PAGE), "");
  block_header_page(header, 1, 4);
  assert_string_equal(block_header_text(header, BLOCK_HEADER_PAGE), "2/4");
  lv_deinit();
}

static void test_blocks_same_text(void **state)
{
  lv_obj_t *label;

  label = lv_label_create(start());
  block_set_text(label, "pnut");
  lv_refr_now(NULL);

  g_invalidated = 0;
  block_set_text(label, "pnut");
  assert_int_equal(g_invalidated, 0);
  block_set_text(label, "nut");
  assert_true(g_invalidated > 0);
  lv_deinit();
}

static void test_blocks_softkeys_tapped(void **state)
{
  lv_obj_t *scr = start();
  lv_obj_t *keys;
  lv_obj_t *tile;

  keys = block_softkeys(scr, "Calendar", UI_ICON_MENU, "Contacts");
  tile = block_tile(scr, 7, UI_ICON_PHONE, "Phone");

  lv_obj_send_event(block_softkey(keys, 0), LV_EVENT_CLICKED, NULL);
  lv_obj_send_event(block_softkey(keys, 1), LV_EVENT_CLICKED, NULL);
  lv_obj_send_event(block_softkey(keys, 2), LV_EVENT_CLICKED, NULL);
  assert_int_equal(g_nkeys, 3);
  assert_int_equal(g_keys[0], UI_KEY(UI_ACT_SOFT_LEFT));
  assert_int_equal(g_keys[1], UI_KEY(UI_ACT_SOFT_MIDDLE));
  assert_int_equal(g_keys[2], UI_KEY(UI_ACT_SOFT_RIGHT));

  /* A tile: its id, as an event of the screen's */

  lv_obj_send_event(tile, LV_EVENT_CLICKED, NULL);
  assert_int_equal(g_nids, 1);
  assert_int_equal(g_ids[0], 7);
  assert_int_equal(block_id(tile), 7);

  /* A blank softkey takes no tap */

  keys = block_softkeys(scr, "", "Open", "Back");
  lv_obj_send_event(block_softkey(keys, 0), LV_EVENT_CLICKED, NULL);
  assert_int_equal(g_nkeys, 3);
  lv_deinit();
}

static void test_blocks_header_same(void **state)
{
  struct ui_status_s st;
  lv_obj_t *header;
  struct tm tm;

  header = block_header(start(), UI_ICON_MENU, "Menu");
  memset(&st, 0, sizeof(st));
  pnut_tz_parse("UTC0", &st.tz);
  status_local(&st, BLOCKS_NOW, &tm);
  st.has_time   = true;
  st.has_tel    = true;
  st.tel.modem  = PNUT_TELEPHONY_INFO_MODEM_ON;
  st.tel.bars   = 2;
  st.has_wifi   = true;
  st.wifi.state = PNUT_WIFI_INFO_STATE_JOINED;
  block_header_status(header, &st, &tm);
  lv_refr_now(NULL);

  /* What the header shows is the same (another dBm, the same bars):
   * nothing to draw
   */

  st.tel.dbm = -71;
  g_invalidated = 0;
  block_header_status(header, &st, &tm);
  assert_int_equal(g_invalidated, 0);

  st.tel.bars = 3;
  block_header_status(header, &st, &tm);
  assert_true(g_invalidated > 0);
  lv_deinit();
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_blocks_header),
    cmocka_unit_test(test_blocks_same_text),
    cmocka_unit_test(test_blocks_header_same),
    cmocka_unit_test(test_blocks_softkeys_tapped),
  };

  return cmocka_run_group_tests_name("ui_blocks", tests, NULL, NULL);
}
