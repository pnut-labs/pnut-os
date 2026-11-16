/****************************************************************************
 * pnut-os/src/ui/blocks.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_BLOCKS_H
#define __PNUT_OS_UI_BLOCKS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <lvgl/lvgl.h>

#include <pnut/compiler.h>

#include "status.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The canvas's geometry, 240 x 320 */

#define BLOCK_HEADER_H   44
#define BLOCK_FOOTER_H   32
#define BLOCK_PAGES_H    30
#define BLOCK_ROW_H      40
#define BLOCK_HEADING_H  20
#define BLOCK_LEAD_W     24      /* A row's lead: an icon, or initials */

/* A text field's text, at most, and as shown (a password's dots) */

#define BLOCK_FIELD_MAX    64
#define BLOCK_FIELD_SHOWN  (BLOCK_FIELD_MAX * 3 + 2)

/* The ids of the blocks a screen does not number itself */

#define BLOCK_ID_PAGE_PREV  0xff01
#define BLOCK_ID_PAGE_NEXT  0xff02

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The header's parts with text, for block_header_text() */

enum block_header_part_e
{
  BLOCK_HEADER_TIME = 0,
  BLOCK_HEADER_BATTERY,
  BLOCK_HEADER_TITLE,
  BLOCK_HEADER_PAGE
};

/* A one-line text field: its text in a fixed buffer, shown as a label of
 * that buffer (LVGL keeps no copy), wiped when done.  max: the most
 * characters; masked: shown as dots; digits: only digits taken.
 */

struct block_field_s
{
  char text[BLOCK_FIELD_MAX + 1];
  char shown[BLOCK_FIELD_SHOWN];
  uint8_t len;
  uint8_t max;
  bool masked;
  bool digits;
  bool focused;                   /* Shows where typing goes */
  FAR lv_obj_t *label;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* The system UI's building blocks (RFC 0019), as the owner's canvas draws
 * them.  A screen is a tree of them; a block a screen numbers carries its
 * id, and a tap on it sends the screen g_ui_event_block with the id.  The
 * softkeys send their keys instead, as the function keys would.
 */

/* The header: the signal and battery meters, Wi-Fi's icon, the time and
 * the battery's level, an icon (or NULL) and a title, the page.  Before
 * the state is known its parts show "—" and nothing lit.
 */

FAR lv_obj_t *block_header(FAR lv_obj_t *scr, FAR const char *icon,
                           FAR const char *title);
void block_header_status(FAR lv_obj_t *header,
                         FAR const struct ui_status_s *st,
                         FAR const struct tm *local);
void block_header_title(FAR lv_obj_t *header, FAR const char *title);
void block_header_page(FAR lv_obj_t *header, uint16_t page,
                       uint16_t npages);    /* Shown from two pages */

/* The header's state, for tests: a part's text, a meter's lit steps
 * (the battery's or the signal's), whether Wi-Fi's icon shows
 */

FAR const char *block_header_text(FAR lv_obj_t *header, int part);
uint8_t block_header_lit(FAR lv_obj_t *header, bool battery);
bool block_header_wifi(FAR lv_obj_t *header);

/* The softkeys, at the screen's foot: the options (or another action),
 * the primary action (text, or an icon from icons.h), back (or another).
 * An empty one is blank and takes no tap.
 */

FAR lv_obj_t *block_softkeys(FAR lv_obj_t *scr, FAR const char *left,
                             FAR const char *middle,
                             FAR const char *right);
FAR lv_obj_t *block_softkey(FAR lv_obj_t *keys, int i);

/* A grid's pages, above the softkeys: the arrows, a dot for each page,
 * the page's number
 */

FAR lv_obj_t *block_pages(FAR lv_obj_t *scr, uint16_t page,
                          uint16_t npages);

/* A tile of a grid: an icon over a name */

FAR lv_obj_t *block_tile(FAR lv_obj_t *parent, uint16_t id,
                         FAR const char *icon, FAR const char *name);

/* A shortcut: an icon alone (Home's) */

FAR lv_obj_t *block_shortcut(FAR lv_obj_t *parent, uint16_t id,
                             FAR const char *icon);

/* A row of a list, 40 px: an icon (or NULL), a title, a detail (or
 * NULL) under it, cut to one line each; a value at the right
 */

FAR lv_obj_t *block_row(FAR lv_obj_t *parent, uint16_t id,
                        FAR const char *icon, FAR const char *title,
                        FAR const char *detail);
void block_row_set(FAR lv_obj_t *row, FAR const char *title,
                   FAR const char *detail);
void block_row_set_value(FAR lv_obj_t *row, FAR const char *value);
void block_row_chevron(FAR lv_obj_t *row);  /* The value: an arrow */
FAR lv_obj_t *block_row_title(FAR lv_obj_t *row);
FAR const char *block_row_value(FAR lv_obj_t *row);

/* A row of an app's list (Cat-Rows), 40 px: its lead, an icon's glyph
 * (18 px) or up to two initials (11 px mono, no frame); the title, bold
 * while unread or focused; the detail under it; the value at the right in
 * the meta's 10 px mono (a time, a count); a trailer.  A part without text
 * takes no room.  Child 0 is the lead, 2 the value, 3 the trailer.
 */

enum block_trailer_e
{
  BLOCK_TRAILER_NONE = 0,
  BLOCK_TRAILER_CHEVRON,
  BLOCK_TRAILER_ON,
  BLOCK_TRAILER_OFF
};

FAR lv_obj_t *block_app_row(FAR lv_obj_t *parent, uint16_t id);
void block_app_row_set(FAR lv_obj_t *row, FAR const char *lead,
                       bool initials, FAR const char *title,
                       FAR const char *detail, FAR const char *value,
                       bool unread, int trailer);

/* A switch: a row whose value says On or Off */

FAR lv_obj_t *block_switch_row(FAR lv_obj_t *parent, uint16_t id,
                               FAR const char *title, bool on);
void block_switch_set(FAR lv_obj_t *row, bool on);

/* A list's heading, 20 px, in capitals; and a row that only tells: a
 * label in capitals over a value
 */

FAR lv_obj_t *block_heading(FAR lv_obj_t *parent, FAR const char *text);
void block_heading_set(FAR lv_obj_t *heading, FAR const char *text);
FAR lv_obj_t *block_info(FAR lv_obj_t *parent, FAR const char *label,
                         FAR const char *value);
void block_info_set(FAR lv_obj_t *info, FAR const char *value);

/* A text field's row: its title, the text in a box */

FAR lv_obj_t *block_field(FAR lv_obj_t *parent, uint16_t id,
                          FAR const char *title,
                          FAR struct block_field_s *f);

/* A key typed into the field: a character, Delete (nothing in an empty
 * field, and still taken: never Back).  Whether it was the field's.
 */

bool block_field_key(FAR struct block_field_s *f, uint8_t action,
                     uint32_t ch);
void block_field_show(FAR struct block_field_s *f);
FAR const char *block_field_shown(FAR struct block_field_s *f);
void block_field_wipe(FAR struct block_field_s *f);

/* A rule: 2 px black, or a dotted hairline (the canvas's light grey) */

FAR lv_obj_t *block_rule(FAR lv_obj_t *parent, int32_t x, int32_t y,
                         int32_t w, bool dotted);

uint16_t block_id(FAR lv_obj_t *item);

/* The current item: inverted, its name bold */

void block_focus(FAR lv_obj_t *item, bool focused);

/* A label's text, changed only if it differs: the same text again draws
 * nothing.  Whether it changed.
 */

bool block_set_text(FAR lv_obj_t *label, FAR const char *text);

#endif /* __PNUT_OS_UI_BLOCKS_H */
