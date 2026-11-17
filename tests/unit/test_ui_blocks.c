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
#include "list.h"
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

static void test_blocks_field(void **state)
{
  struct block_field_s f;
  lv_obj_t *scr = start();
  int i;

  memset(&f, 0, sizeof(f));
  f.max = 63;
  block_field(scr, 3, "Password", &f);

  /* 63 characters, not 64 */

  for (i = 0; i < 64; i++)
    {
      assert_true(block_field_key(&f, UI_ACT_CHAR, 'a' + i % 26));
    }

  assert_int_equal(f.len, 63);
  assert_int_equal(strlen(f.text), 63);

  /* Delete takes the last; in an empty field it does nothing, and is
   * taken (never Back)
   */

  assert_true(block_field_key(&f, UI_ACT_DELETE, 0));
  assert_int_equal(f.len, 62);
  while (f.len > 0)
    {
      block_field_key(&f, UI_ACT_DELETE, 0);
    }

  assert_true(block_field_key(&f, UI_ACT_DELETE, 0));
  assert_int_equal(f.len, 0);

  /* Steering is not the field's */

  assert_false(block_field_key(&f, UI_ACT_UP, 0));

  /* Masked: a dot for each */

  f.masked = true;
  block_field_key(&f, UI_ACT_CHAR, 's');
  block_field_key(&f, UI_ACT_CHAR, 'e');
  block_field_key(&f, UI_ACT_CHAR, 'c');
  assert_string_equal(block_field_shown(&f),
                      "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2");

  /* Wiped: nothing left of it */

  block_field_wipe(&f);
  assert_int_equal(f.len, 0);
  for (i = 0; i < (int)sizeof(f.text); i++)
    {
      assert_int_equal(f.text[i], 0);
    }

  /* Digits only: a letter is refused */

  f.digits = true;
  f.masked = false;
  assert_true(block_field_key(&f, UI_ACT_CHAR, '7'));
  assert_true(block_field_key(&f, UI_ACT_CHAR, 'x'));
  assert_string_equal(f.text, "7");
  lv_deinit();
}

static void test_blocks_rows(void **state)
{
  lv_obj_t *scr = start();
  lv_obj_t *row;
  lv_obj_t *title;

  row = block_row(scr, 1, NULL,
                  "A network name of thirty-two bytes, and more", "Secured");
  lv_obj_set_width(row, 240);
  lv_obj_update_layout(row);
  title = block_row_title(row);
  assert_int_equal(lv_obj_get_height(title),
                   lv_font_get_line_height(&g_ui_sans_14));
  assert_true(lv_obj_get_width(title) < 240);

  row = block_switch_row(scr, 2, "Wi-Fi", true);
  assert_string_equal(block_row_value(row), "On");
  block_switch_set(row, false);
  assert_string_equal(block_row_value(row), "Off");

  /* A tap on a row: its id */

  lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);
  assert_int_equal(g_nids, 1);
  assert_int_equal(g_ids[0], 2);
  lv_deinit();
}

/* An app's row (Cat-Rows): initials or an icon as its lead, the title
 * bold while unread whatever the focus, the value at the right in the
 * meta's small mono, the trailer
 */

static void test_blocks_app_row(void **state)
{
  lv_obj_t *scr = start();
  lv_obj_t *row;
  lv_obj_t *lead;
  lv_obj_t *value;
  lv_obj_t *trailer;

  row = block_app_row(scr, 3);
  block_app_row_set(row, "OL", true, "Ola", "Are we still on for Thursday?",
                    "13:52", true, BLOCK_TRAILER_NONE);
  lead    = lv_obj_get_child(row, 0);
  value   = lv_obj_get_child(row, 2);
  trailer = lv_obj_get_child(row, 3);

  assert_string_equal(lv_label_get_text(lead), "OL");
  assert_ptr_equal(lv_obj_get_style_text_font(lead, 0), &g_ui_mono_bold_11);
  assert_ptr_equal(lv_obj_get_style_text_font(block_row_title(row), 0),
                   &g_ui_sans_bold_14);
  assert_string_equal(lv_label_get_text(value), "13:52");
  assert_ptr_equal(lv_obj_get_style_text_font(value, 0),
                   &g_ui_mono_bold_10);
  assert_true(lv_obj_has_flag(trailer, LV_OBJ_FLAG_HIDDEN));

  /* Unread stays bold as the focus comes and goes; read is bold only
   * while focused
   */

  block_focus(row, true);
  block_focus(row, false);
  assert_ptr_equal(lv_obj_get_style_text_font(block_row_title(row), 0),
                   &g_ui_sans_bold_14);

  block_app_row_set(row, UI_ICON_NOTES, false, "Shopping", NULL, "", false,
                    BLOCK_TRAILER_CHEVRON);
  assert_ptr_equal(lv_obj_get_style_text_font(lead, 0), &g_ui_icons_18);
  assert_ptr_equal(lv_obj_get_style_text_font(block_row_title(row), 0),
                   &g_ui_sans_14);
  block_focus(row, true);
  assert_ptr_equal(lv_obj_get_style_text_font(block_row_title(row), 0),
                   &g_ui_sans_bold_14);
  block_focus(row, false);
  assert_ptr_equal(lv_obj_get_style_text_font(block_row_title(row), 0),
                   &g_ui_sans_14);
  assert_false(lv_obj_has_flag(trailer, LV_OBJ_FLAG_HIDDEN));
  assert_string_equal(lv_label_get_text(trailer), UI_ICON_CHEVRON_RIGHT);
  assert_true(lv_obj_has_flag(lv_obj_get_child(lv_obj_get_child(row, 1), 1),
                              LV_OBJ_FLAG_HIDDEN));

  /* No lead: none shown; a switch's trailer */

  block_app_row_set(row, NULL, false, "Relay", NULL, NULL, false,
                    BLOCK_TRAILER_ON);
  assert_true(lv_obj_has_flag(lead, LV_OBJ_FLAG_HIDDEN));
  assert_string_equal(lv_label_get_text(trailer), "On");
  block_app_row_set(row, NULL, false, "Relay", NULL, NULL, false,
                    BLOCK_TRAILER_OFF);
  assert_string_equal(lv_label_get_text(trailer), "Off");

  /* A tap: its id */

  lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);
  assert_int_equal(g_nids, 1);
  assert_int_equal(g_ids[0], 3);
  lv_deinit();
}

static void test_blocks_list(void **state)
{
  struct list_s l;
  lv_obj_t *scr = start();
  lv_obj_t *rows[4];

  /* Headings and hidden rows are passed over */

  list_init(&l, scr);
  rows[0] = block_switch_row(l.box, 1, "Wi-Fi", true);
  block_heading(l.box, "Nearby");
  rows[1] = block_row(l.box, 2, NULL, "Home", "Secured");
  rows[2] = block_row(l.box, 3, NULL, "Gone", NULL);
  lv_obj_add_flag(rows[2], LV_OBJ_FLAG_HIDDEN);
  rows[3] = block_row(l.box, 4, NULL, "Cafe", "Open");

  assert_true(list_move(&l, 1));
  assert_ptr_equal(l.focus, rows[0]);
  assert_true(list_move(&l, 1));
  assert_ptr_equal(l.focus, rows[1]);
  assert_true(list_move(&l, 1));
  assert_ptr_equal(l.focus, rows[3]);
  assert_false(list_move(&l, 1));
  assert_ptr_equal(l.focus, rows[3]);
  assert_true(list_move(&l, -1));
  assert_ptr_equal(l.focus, rows[1]);

  assert_ptr_equal(list_find(&l, 4), rows[3]);
  assert_null(list_find(&l, 9));

  /* The focused row hidden: the focus goes to the next shown */

  lv_obj_add_flag(rows[1], LV_OBJ_FLAG_HIDDEN);
  list_check(&l);
  assert_ptr_equal(l.focus, rows[3]);
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
    cmocka_unit_test(test_blocks_field),
    cmocka_unit_test(test_blocks_rows),
    cmocka_unit_test(test_blocks_app_row),
    cmocka_unit_test(test_blocks_list),
    cmocka_unit_test(test_blocks_softkeys_tapped),
  };

  return cmocka_run_group_tests_name("ui_blocks", tests, NULL, NULL);
}
