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
