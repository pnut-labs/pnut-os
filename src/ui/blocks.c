/****************************************************************************
 * pnut-os/src/ui/blocks.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The system UI's building blocks (RFC 0019), placed as the owner's canvas
 * places them on its 240 x 320 pages (Proto-Home, Proto-Menu): a 44 px
 * header, a 32 px row of softkeys, a 30 px row of pages, tiles.  At 1 bit:
 * the light grey's rules dotted, its fills white with a black outline.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include "blocks.h"
#include "describe.h"
#include "icons.h"
#include "keymap.h"
#include "text.h"
#include "theme.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BLOCK_W          240
#define BLOCK_H          320

/* The header: 3 px down, 5 px in, a meter 10 px wide each side, 6 px from
 * the middle; the status row 13 px, the title row 26 px
 */

#define BLOCK_TOP        3
#define BLOCK_SIDE       5
#define BLOCK_METER_W    10
#define BLOCK_GAP        6
#define BLOCK_MID_X      (BLOCK_SIDE + BLOCK_METER_W + BLOCK_GAP)
#define BLOCK_MID_W      (BLOCK_W - 2 * BLOCK_MID_X)
#define BLOCK_ROW1_H     13
#define BLOCK_ROW2_H     26

/* A meter's five steps, top to bottom: 3 px high, 2 px apart */

#define BLOCK_STEPS      5
#define BLOCK_STEP_H     3

/* The softkeys: the middle one 72 px wide, its box 24 px high */

#define BLOCK_KEY_MID_W  72
#define BLOCK_KEY_PAD    9
#define BLOCK_BOX_H      24

/* The pages' row: the arrows 56 px wide */

#define BLOCK_ARROW_W    56
#define BLOCK_DOT        6

/* A row: 8 px in, its parts 8 px apart; a row is told from a tile by its
 * flag, so that focus makes its title bold, not its name
 */

#define BLOCK_PAD        8
#define BLOCK_ROW_FLAG   LV_OBJ_FLAG_USER_1
#define BLOCK_FIELD_FLAG LV_OBJ_FLAG_USER_2
#define BLOCK_BOX_PAD    6

/* The field's dot: U+2022 */

#define BLOCK_DOT_UTF8   "\xe2\x80\xa2"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct block_header_s
{
  FAR lv_obj_t *steps[2][BLOCK_STEPS];  /* The signal's, the battery's */
  uint8_t lit[2];
  FAR lv_obj_t *wifi;
  FAR lv_obj_t *time;
  FAR lv_obj_t *level;
  FAR lv_obj_t *icon;
  FAR lv_obj_t *title;
  FAR lv_obj_t *page;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint8_t g_block_widths[BLOCK_STEPS] =
{
  10, 8, 7, 5, 4
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR lv_obj_t *block_box(FAR lv_obj_t *parent, int32_t x, int32_t y,
                               int32_t w, int32_t h)
{
  FAR lv_obj_t *obj = lv_obj_create(parent);

  theme_box(obj);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, w, h);
  return obj;
}

static FAR lv_obj_t *block_label(FAR lv_obj_t *parent,
                                 FAR const lv_font_t *font,
                                 FAR const char *text)
{
  FAR lv_obj_t *label = lv_label_create(parent);

  theme_label(label, font);
  lv_label_set_text(label, text);
  return label;
}

static FAR struct block_header_s *block_header_of(FAR lv_obj_t *header)
{
  return lv_obj_get_user_data(header);
}

static void block_header_deleted(FAR lv_event_t *e)
{
  lv_free(lv_event_get_user_data(e));
}

/* A meter: five steps top to bottom, the widest on top, against the left
 * edge (the signal's) or the right (the battery's)
 */

static void block_meter(FAR lv_obj_t *header,
                        FAR struct block_header_s *h, int which)
{
  FAR lv_obj_t *step;
  int32_t x;
  int i;

  for (i = 0; i < BLOCK_STEPS; i++)
    {
      x = which == 0 ? BLOCK_SIDE :
          BLOCK_W - BLOCK_SIDE - g_block_widths[i];
      step = block_box(header, x, BLOCK_TOP + 2 + i * (BLOCK_STEP_H + 2),
                       g_block_widths[i], BLOCK_STEP_H);
      lv_obj_set_style_border_color(step, lv_color_black(), 0);
      lv_obj_set_style_bg_color(step, lv_color_black(), 0);
      h->steps[which][i] = step;
    }
}

/* Light a meter's bottom n steps; the others outlined */

static void block_light(FAR struct block_header_s *h, int which, uint8_t n)
{
  FAR lv_obj_t *step;
  bool lit;
  int i;

  if (h->lit[which] == n)
    {
      return;
    }

  h->lit[which] = n;
  for (i = 0; i < BLOCK_STEPS; i++)
    {
      step = h->steps[which][i];
      lit  = i >= BLOCK_STEPS - n;
      lv_obj_set_style_bg_opa(step, lit ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(step, lit ? 0 : 1, 0);
    }
}

/* Place the title row: the icon, then the title */

static void block_header_place(FAR struct block_header_s *h)
{
  int32_t y = BLOCK_TOP + BLOCK_ROW1_H;

  lv_obj_align(h->icon, LV_ALIGN_TOP_LEFT, BLOCK_MID_X,
               y + (BLOCK_ROW2_H - 16) / 2);
  lv_obj_update_layout(h->icon);
  lv_obj_align(h->title, LV_ALIGN_TOP_LEFT,
               BLOCK_MID_X + (lv_label_get_text(h->icon)[0] != '\0' ?
                              16 + BLOCK_GAP : 0),
               y + (BLOCK_ROW2_H - lv_font_get_line_height(
                                      &g_ui_sans_bold_16)) / 2);
}

/* A tap on a numbered block: its id to its screen */

static void block_tapped(FAR lv_event_t *e)
{
  FAR lv_obj_t *obj = lv_event_get_current_target(e);
  uintptr_t id = (uintptr_t)lv_event_get_user_data(e);

  lv_obj_send_event(lv_obj_get_screen(obj), g_ui_event_block,
                    (FAR void *)id);
}

/* A tap on a softkey: its key to its screen */

static void block_key_tapped(FAR lv_event_t *e)
{
  FAR lv_obj_t *obj = lv_event_get_current_target(e);
  uint32_t key = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

  lv_obj_send_event(lv_obj_get_screen(obj), LV_EVENT_KEY, &key);
}

static void block_rule_draw(FAR lv_event_t *e)
{
  FAR lv_obj_t *obj = lv_event_get_current_target(e);
  lv_draw_line_dsc_t dsc;
  lv_area_t area;

  lv_obj_get_coords(obj, &area);
  lv_draw_line_dsc_init(&dsc);
  dsc.color      = lv_color_black();
  dsc.width      = 1;
  dsc.dash_width = 1;
  dsc.dash_gap   = 1;
  dsc.p1.x       = area.x1;
  dsc.p1.y       = area.y1;
  dsc.p2.x       = area.x2;
  dsc.p2.y       = area.y1;
  lv_draw_line(lv_event_get_layer(e), &dsc);
}

/* A row's frame: a flex row 40 px high, 8 px in, a dotted rule at its
 * foot
 */

static FAR lv_obj_t *block_row_frame(FAR lv_obj_t *parent, uint16_t id,
                                     int32_t h)
{
  FAR lv_obj_t *row = lv_obj_create(parent);

  theme_box(row);
  lv_obj_set_size(row, BLOCK_W, h);
  lv_obj_set_user_data(row, (FAR void *)(uintptr_t)id);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_hor(row, BLOCK_PAD, 0);
  lv_obj_set_style_pad_column(row, BLOCK_PAD, 0);
  return row;
}

/* The row's dotted rule, its last child: the others keep their places */

static void block_row_rule(FAR lv_obj_t *row)
{
  FAR lv_obj_t *rule;

  rule = block_rule(row, -BLOCK_PAD, lv_obj_get_style_height(row, 0) - 1,
                    BLOCK_W, true);
  lv_obj_add_flag(rule, LV_OBJ_FLAG_IGNORE_LAYOUT);
}

/* A label cut to one line at its width */

static FAR lv_obj_t *block_line(FAR lv_obj_t *parent,
                                FAR const lv_font_t *font,
                                FAR const char *text)
{
  FAR lv_obj_t *label = block_label(parent, font, text);

  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_set_size(label, lv_pct(100), lv_font_get_line_height(font));
  return label;
}

static void block_row_tapped(FAR lv_obj_t *row, uint16_t id)
{
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(row, block_tapped, LV_EVENT_CLICKED,
                      (FAR void *)(uintptr_t)id);
}

/* Text in capitals, as the canvas's small labels (ASCII; other letters as
 * they are)
 */

static void block_caps(FAR lv_obj_t *label, FAR const char *text)
{
  char caps[64];
  size_t i;

  for (i = 0; text[i] != '\0' && i < sizeof(caps) - 1; i++)
    {
      caps[i] = text[i] >= 'a' && text[i] <= 'z' ? text[i] - 'a' + 'A' :
                text[i];
    }

  caps[i] = '\0';
  block_set_text(label, caps);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR lv_obj_t *block_header(FAR lv_obj_t *scr, FAR const char *icon,
                           FAR const char *title)
{
  FAR struct block_header_s *h;
  FAR lv_obj_t *header;
  FAR lv_obj_t *row;

  h = lv_malloc_zeroed(sizeof(*h));
  LV_ASSERT_MALLOC(h);

  header = block_box(scr, 0, 0, BLOCK_W, BLOCK_HEADER_H);
  lv_obj_set_user_data(header, h);
  lv_obj_add_event_cb(header, block_header_deleted, LV_EVENT_DELETE, h);
  block_rule(header, 0, BLOCK_HEADER_H - 2, BLOCK_W, false);

  block_meter(header, h, 0);
  block_meter(header, h, 1);
  h->lit[0] = UINT8_MAX;
  h->lit[1] = UINT8_MAX;
  block_light(h, 0, 0);
  block_light(h, 1, 0);

  /* The status row: Wi-Fi's icon; the time and the battery's level */

  row = block_box(header, BLOCK_MID_X, BLOCK_TOP, BLOCK_MID_W,
                  BLOCK_ROW1_H);
  h->wifi = block_label(row, &g_ui_icons_11, UI_ICON_WIFI);
  lv_obj_align(h->wifi, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_add_flag(h->wifi, LV_OBJ_FLAG_HIDDEN);
  h->level = block_label(row, &g_ui_mono_bold_10, UI_T_UNKNOWN);
  lv_obj_align(h->level, LV_ALIGN_RIGHT_MID, 0, 0);
  h->time = block_label(row, &g_ui_mono_bold_10, UI_T_UNKNOWN);
  lv_obj_align_to(h->time, h->level, LV_ALIGN_OUT_LEFT_MID, -8, 0);

  /* The title row: an icon and the title; the page at the right */

  h->icon  = block_label(header, &g_ui_icons_16, icon != NULL ? icon : "");
  h->title = block_label(header, &g_ui_sans_bold_16, title);
  h->page  = block_label(header, &g_ui_mono_bold_10, "");
  lv_obj_align(h->page, LV_ALIGN_TOP_RIGHT, -BLOCK_MID_X,
               BLOCK_TOP + BLOCK_ROW1_H + (BLOCK_ROW2_H - 14) / 2);
  block_header_place(h);
  return header;
}

void block_header_status(FAR lv_obj_t *header,
                         FAR const struct ui_status_s *st,
                         FAR const struct tm *local)
{
  FAR struct block_header_s *h = block_header_of(header);
  char text[16];
  bool joined;
  bool moved;

  block_light(h, 0, st->has_tel &&
                    st->tel.modem == PNUT_TELEPHONY_INFO_MODEM_ON ?
                    describe_signal_steps(st->tel.bars) : 0);
  block_light(h, 1, st->has_battery ?
                    describe_battery_steps(st->battery) : 0);

  /* Only what changes is touched: LVGL redraws whatever is */

  joined = st->has_wifi && st->wifi.state == PNUT_WIFI_INFO_STATE_JOINED;
  if (joined == lv_obj_has_flag(h->wifi, LV_OBJ_FLAG_HIDDEN))
    {
      if (joined)
        {
          lv_obj_remove_flag(h->wifi, LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(h->wifi, LV_OBJ_FLAG_HIDDEN);
        }
    }

  if (st->has_battery)
    {
      snprintf(text, sizeof(text), "%u%%%s", st->battery,
               st->charging ? "+" : "");
    }
  else
    {
      strlcpy(text, UI_T_UNKNOWN, sizeof(text));
    }

  moved = block_set_text(h->level, text);

  if (st->has_time)
    {
      describe_clock(local, text, sizeof(text));
    }
  else
    {
      strlcpy(text, UI_T_UNKNOWN, sizeof(text));
    }

  /* The time sits left of the level, which may have grown */

  if (block_set_text(h->time, text) || moved)
    {
      lv_obj_align_to(h->time, h->level, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    }
}

void block_header_title(FAR lv_obj_t *header, FAR const char *title)
{
  block_set_text(block_header_of(header)->title, title);
}

void block_header_page(FAR lv_obj_t *header, uint16_t page,
                       uint16_t npages)
{
  char text[12];

  text[0] = '\0';
  if (npages > 1)
    {
      snprintf(text, sizeof(text), "%u/%u", page + 1, npages);
    }

  block_set_text(block_header_of(header)->page, text);
}

FAR const char *block_header_text(FAR lv_obj_t *header, int part)
{
  FAR struct block_header_s *h = block_header_of(header);

  return lv_label_get_text(part == BLOCK_HEADER_TIME ? h->time :
                           part == BLOCK_HEADER_BATTERY ? h->level :
                           part == BLOCK_HEADER_TITLE ? h->title : h->page);
}

uint8_t block_header_lit(FAR lv_obj_t *header, bool battery)
{
  return block_header_of(header)->lit[battery ? 1 : 0];
}

bool block_header_wifi(FAR lv_obj_t *header)
{
  return !lv_obj_has_flag(block_header_of(header)->wifi,
                          LV_OBJ_FLAG_HIDDEN);
}

FAR lv_obj_t *block_softkeys(FAR lv_obj_t *scr, FAR const char *left,
                             FAR const char *middle,
                             FAR const char *right)
{
  FAR const char *texts[3];
  FAR lv_obj_t *keys;
  FAR lv_obj_t *cell;
  FAR lv_obj_t *box;
  FAR lv_obj_t *label;
  int32_t side = (BLOCK_W - BLOCK_KEY_MID_W) / 2;
  int i;

  texts[0] = left;
  texts[1] = middle;
  texts[2] = right;

  keys = block_box(scr, 0, BLOCK_H - BLOCK_FOOTER_H, BLOCK_W,
                   BLOCK_FOOTER_H);
  block_rule(keys, 0, 0, BLOCK_W, false);
  block_rule(keys, 0, 1, BLOCK_W, false);

  for (i = 0; i < 3; i++)
    {
      cell = block_box(keys, i == 0 ? 0 : i == 1 ? side :
                       side + BLOCK_KEY_MID_W, 2,
                       i == 1 ? BLOCK_KEY_MID_W : side,
                       BLOCK_FOOTER_H - 2);
      if (texts[i] == NULL || texts[i][0] == '\0')
        {
          continue;
        }

      lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(cell, block_key_tapped, LV_EVENT_CLICKED,
                          (FAR void *)(uintptr_t)
                          UI_KEY(UI_ACT_SOFT_LEFT + i));
      if (i != 1)
        {
          label = block_label(cell, &g_ui_sans_bold_13, texts[i]);
          lv_obj_align(label, i == 0 ? LV_ALIGN_LEFT_MID :
                       LV_ALIGN_RIGHT_MID,
                       i == 0 ? BLOCK_KEY_PAD : -BLOCK_KEY_PAD, 0);
          continue;
        }

      /* The primary action in a box: text, or an icon */

      box = lv_obj_create(cell);
      theme_box(box);
      lv_obj_set_style_border_width(box, 2, 0);
      lv_obj_set_style_border_color(box, lv_color_black(), 0);
      lv_obj_set_style_pad_hor(box, 6, 0);
      lv_obj_set_size(box, LV_SIZE_CONTENT, BLOCK_BOX_H);
      lv_obj_set_style_min_width(box, 32, 0);
      lv_obj_center(box);
      label = block_label(box, (uint8_t)texts[i][0] == 0xee ?
                          &g_ui_icons_14 : &g_ui_sans_bold_12, texts[i]);
      lv_obj_center(label);
    }

  return keys;
}

FAR lv_obj_t *block_softkey(FAR lv_obj_t *keys, int i)
{
  /* The two rules first, then the three cells */

  return lv_obj_get_child(keys, 2 + i);
}

FAR lv_obj_t *block_pages(FAR lv_obj_t *scr, uint16_t page,
                          uint16_t npages)
{
  FAR lv_obj_t *row;
  FAR lv_obj_t *arrow;
  FAR lv_obj_t *mid;
  FAR lv_obj_t *dot;
  FAR lv_obj_t *label;
  char text[12];
  uint16_t i;
  int a;

  row = block_box(scr, 0, BLOCK_H - BLOCK_FOOTER_H - BLOCK_PAGES_H,
                  BLOCK_W, BLOCK_PAGES_H);
  block_rule(row, 0, 0, BLOCK_W, true);

  /* The arrows: one with no page that way is left out */

  for (a = 0; a < 2; a++)
    {
      arrow = block_box(row, a == 0 ? 0 : BLOCK_W - BLOCK_ARROW_W, 1,
                        BLOCK_ARROW_W, BLOCK_PAGES_H - 1);
      if (a == 0 ? page == 0 : page + 1 >= npages)
        {
          continue;
        }

      label = block_label(arrow, &g_ui_icons_16, a == 0 ?
                          UI_ICON_CHEVRON_LEFT : UI_ICON_CHEVRON_RIGHT);
      lv_obj_center(label);
      lv_obj_add_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(arrow, block_tapped, LV_EVENT_CLICKED,
                          (FAR void *)(uintptr_t)(a == 0 ?
                          BLOCK_ID_PAGE_PREV : BLOCK_ID_PAGE_NEXT));
    }

  /* A dot for each page, the page's filled, then its number */

  mid = block_box(row, BLOCK_ARROW_W, 1, BLOCK_W - 2 * BLOCK_ARROW_W,
                  BLOCK_PAGES_H - 1);
  lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(mid, BLOCK_GAP, 0);
  for (i = 0; i < npages; i++)
    {
      dot = lv_obj_create(mid);
      theme_box(dot);
      lv_obj_set_size(dot, BLOCK_DOT, BLOCK_DOT);
      lv_obj_set_style_border_color(dot, lv_color_black(), 0);
      lv_obj_set_style_border_width(dot, i == page ? 0 : 1, 0);
      lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
      lv_obj_set_style_bg_opa(dot, i == page ? LV_OPA_COVER :
                                               LV_OPA_TRANSP, 0);
    }

  snprintf(text, sizeof(text), "%u/%u", page + 1, npages);
  label = block_label(mid, &g_ui_mono_bold_10, text);
  lv_obj_set_style_margin_left(label, 4, 0);
  return row;
}

FAR lv_obj_t *block_tile(FAR lv_obj_t *parent, uint16_t id,
                         FAR const char *icon, FAR const char *name)
{
  FAR lv_obj_t *tile = lv_obj_create(parent);

  theme_box(tile);
  lv_obj_set_user_data(tile, (FAR void *)(uintptr_t)id);
  lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(tile, 4, 0);
  block_label(tile, &g_ui_icons_20, icon);
  block_label(tile, &g_ui_sans_12, name);
  lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(tile, block_tapped, LV_EVENT_CLICKED,
                      (FAR void *)(uintptr_t)id);
  return tile;
}

FAR lv_obj_t *block_shortcut(FAR lv_obj_t *parent, uint16_t id,
                             FAR const char *icon)
{
  FAR lv_obj_t *cell = lv_obj_create(parent);

  theme_box(cell);
  lv_obj_set_user_data(cell, (FAR void *)(uintptr_t)id);
  lv_obj_center(block_label(cell, &g_ui_icons_18, icon));
  lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(cell, block_tapped, LV_EVENT_CLICKED,
                      (FAR void *)(uintptr_t)id);
  return cell;
}

FAR lv_obj_t *block_row(FAR lv_obj_t *parent, uint16_t id,
                        FAR const char *icon, FAR const char *title,
                        FAR const char *detail)
{
  FAR lv_obj_t *row = block_row_frame(parent, id, BLOCK_ROW_H);
  FAR lv_obj_t *glyph;
  FAR lv_obj_t *col;
  FAR lv_obj_t *line;

  lv_obj_add_flag(row, BLOCK_ROW_FLAG);

  /* Child 0, the icon; 1, the title over the detail; 2, the value */

  glyph = block_label(row, &g_ui_icons_16, icon != NULL ? icon : "");
  if (icon == NULL)
    {
      lv_obj_add_flag(glyph, LV_OBJ_FLAG_HIDDEN);
    }

  col = block_box(row, 0, 0, 10, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(col, 1);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 2, 0);
  block_line(col, &g_ui_sans_14, title);
  line = block_line(col, &g_ui_sans_12, detail != NULL ? detail : "");
  if (detail == NULL)
    {
      lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
    }

  block_label(row, &g_ui_mono_bold_12, "");
  block_row_rule(row);
  block_row_tapped(row, id);
  return row;
}

void block_row_set(FAR lv_obj_t *row, FAR const char *title,
                   FAR const char *detail)
{
  FAR lv_obj_t *col = lv_obj_get_child(row, 1);
  FAR lv_obj_t *line = lv_obj_get_child(col, 1);

  if (title != NULL)
    {
      block_set_text(lv_obj_get_child(col, 0), title);
    }

  if (detail != NULL)
    {
      block_set_text(line, detail);
      if (lv_obj_has_flag(line, LV_OBJ_FLAG_HIDDEN))
        {
          lv_obj_remove_flag(line, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void block_row_set_value(FAR lv_obj_t *row, FAR const char *value)
{
  block_set_text(lv_obj_get_child(row, 2), value);
}

void block_row_chevron(FAR lv_obj_t *row)
{
  FAR lv_obj_t *value = lv_obj_get_child(row, 2);

  lv_obj_set_style_text_font(value, &g_ui_icons_14, 0);
  block_set_text(value, UI_ICON_CHEVRON_RIGHT);
}

FAR lv_obj_t *block_row_title(FAR lv_obj_t *row)
{
  return lv_obj_get_child(lv_obj_get_child(row, 1), 0);
}

FAR const char *block_row_value(FAR lv_obj_t *row)
{
  return lv_label_get_text(lv_obj_get_child(row, 2));
}

FAR lv_obj_t *block_switch_row(FAR lv_obj_t *parent, uint16_t id,
                               FAR const char *title, bool on)
{
  FAR lv_obj_t *row = block_row(parent, id, NULL, title, NULL);

  block_switch_set(row, on);
  return row;
}

void block_switch_set(FAR lv_obj_t *row, bool on)
{
  block_row_set_value(row, on ? UI_T_ON : UI_T_OFF);
}

FAR lv_obj_t *block_heading(FAR lv_obj_t *parent, FAR const char *text)
{
  FAR lv_obj_t *heading = block_row_frame(parent, 0, BLOCK_HEADING_H);

  block_line(heading, &g_ui_mono_bold_10, "");
  block_heading_set(heading, text);
  block_row_rule(heading);
  return heading;
}

void block_heading_set(FAR lv_obj_t *heading, FAR const char *text)
{
  block_caps(lv_obj_get_child(heading, 0), text);
}

FAR lv_obj_t *block_info(FAR lv_obj_t *parent, FAR const char *label,
                         FAR const char *value)
{
  FAR lv_obj_t *row = block_row_frame(parent, 0, BLOCK_ROW_H);
  FAR lv_obj_t *col;

  col = block_box(row, 0, 0, 10, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(col, 1);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 3, 0);
  block_caps(block_line(col, &g_ui_mono_bold_10, ""), label);
  block_line(col, &g_ui_sans_14, value);
  block_row_rule(row);
  return row;
}

void block_info_set(FAR lv_obj_t *info, FAR const char *value)
{
  block_set_text(lv_obj_get_child(lv_obj_get_child(info, 0), 1), value);
}

FAR lv_obj_t *block_field(FAR lv_obj_t *parent, uint16_t id,
                          FAR const char *title, FAR struct block_field_s *f)
{
  FAR lv_obj_t *row = block_row_frame(parent, id, BLOCK_ROW_H);
  FAR lv_obj_t *box;

  lv_obj_add_flag(row, BLOCK_FIELD_FLAG);
  block_label(row, &g_ui_sans_14, title);
  box = lv_obj_create(row);
  theme_box(box);
  lv_obj_set_flex_grow(box, 1);
  lv_obj_set_height(box, BLOCK_BOX_H);
  lv_obj_set_style_border_width(box, 2, 0);
  lv_obj_set_style_border_color(box, lv_color_black(), 0);
  lv_obj_set_style_pad_hor(box, BLOCK_BOX_PAD, 0);

  f->label = lv_label_create(box);
  theme_label(f->label, &g_ui_sans_12);
  lv_label_set_long_mode(f->label, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(f->label, lv_pct(100));
  lv_obj_align(f->label, LV_ALIGN_LEFT_MID, 0, 0);
  block_field_show(f);
  block_row_rule(row);
  block_row_tapped(row, id);
  return row;
}

bool block_field_key(FAR struct block_field_s *f, uint8_t action,
                     uint32_t ch)
{
  switch (action)
    {
      case UI_ACT_CHAR:

        /* One byte a character so far: what the keyboard types */

        if (ch >= 0x20 && ch < 0x7f && f->len < f->max &&
            f->len < BLOCK_FIELD_MAX && (!f->digits ||
                                         (ch >= '0' && ch <= '9')))
          {
            f->text[f->len++] = (char)ch;
            f->text[f->len]   = '\0';
            block_field_show(f);
          }

        return true;

      case UI_ACT_DELETE:
        if (f->len > 0)
          {
            f->text[--f->len] = '\0';
            block_field_show(f);
          }

        return true;

      default:
        return false;
    }
}

void block_field_show(FAR struct block_field_s *f)
{
  size_t o = 0;
  uint8_t i;

  for (i = 0; i < f->len; i++)
    {
      if (f->masked)
        {
          memcpy(f->shown + o, BLOCK_DOT_UTF8, 3);
          o += 3;
        }
      else
        {
          f->shown[o++] = f->text[i];
        }
    }

  if (f->focused)
    {
      f->shown[o++] = '_';
    }

  f->shown[o] = '\0';
  if (f->label != NULL)
    {
      lv_label_set_text_static(f->label, f->shown);
    }
}

FAR const char *block_field_shown(FAR struct block_field_s *f)
{
  return f->shown;
}

void block_field_wipe(FAR struct block_field_s *f)
{
  explicit_bzero(f->text, sizeof(f->text));
  explicit_bzero(f->shown, sizeof(f->shown));
  f->len = 0;
  block_field_show(f);
}

FAR lv_obj_t *block_rule(FAR lv_obj_t *parent, int32_t x, int32_t y,
                         int32_t w, bool dotted)
{
  FAR lv_obj_t *rule = block_box(parent, x, y, w, dotted ? 1 : 2);

  if (dotted)
    {
      lv_obj_add_event_cb(rule, block_rule_draw, LV_EVENT_DRAW_MAIN, NULL);
    }
  else
    {
      lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
      lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    }

  return rule;
}

uint16_t block_id(FAR lv_obj_t *item)
{
  return (uint16_t)(uintptr_t)lv_obj_get_user_data(item);
}

void block_focus(FAR lv_obj_t *item, bool focused)
{
  FAR lv_obj_t *name;

  /* A field shows its focus by its cursor (the screen's) */

  if (lv_obj_has_flag(item, BLOCK_FIELD_FLAG))
    {
      return;
    }

  theme_inverted(item, focused);
  if (lv_obj_has_flag(item, BLOCK_ROW_FLAG))
    {
      lv_obj_set_style_text_font(block_row_title(item), focused ?
                                 &g_ui_sans_bold_14 : &g_ui_sans_14, 0);
      return;
    }

  name = lv_obj_get_child(item, 1);
  if (name != NULL)
    {
      lv_obj_set_style_text_font(name, focused ? &g_ui_sans_bold_12 :
                                                 &g_ui_sans_12, 0);
    }
}

bool block_set_text(FAR lv_obj_t *label, FAR const char *text)
{
  if (strcmp(lv_label_get_text(label), text) == 0)
    {
      return false;
    }

  lv_label_set_text(label, text);
  return true;
}
