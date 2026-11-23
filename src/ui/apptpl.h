/****************************************************************************
 * pnut-os/src/ui/apptpl.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_APPTPL_H
#define __PNUT_OS_UI_APPTPL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <lvgl/lvgl.h>
#include <pnut/compiler.h>

#include "pnut/ui.pb.h"

#include "applist.h"
#include "list.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* An app's options, at most (ui.proto); its texts as drawn go in ui.h's
 * UI_TEXT_MAX and UI_TEXT_LONG
 */

#define APP_OPTIONS    8

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The List template's state */

struct tpl_list_s
{
  struct applist_s list;
  FAR lv_obj_t *rows[APPLIST_PER_PAGE];   /* The page's, reused */
};

/* The Detail template's: its items, in a column that scrolls; an item's
 * block is numbered by its index + 1 when the app numbered the item
 */

#define TPL_DETAIL_ITEMS \
  (sizeof(((FAR pnut_ui_detail_t *)0)->items) / \
   sizeof(((FAR pnut_ui_detail_t *)0)->items[0]))

struct tpl_detail_s
{
  struct list_s list;
  uint32_t ids[TPL_DETAIL_ITEMS];
};

/* A Busy's parts, on its screen or its sheet: the heading, the bar of
 * TPL_BUSY_STEPS steps (hidden without progress), the line
 */

#define TPL_BUSY_STEPS  10

struct tpl_busy_s
{
  FAR lv_obj_t *heading;
  FAR lv_obj_t *bar;
  FAR lv_obj_t *steps[TPL_BUSY_STEPS];
  FAR lv_obj_t *text;
};

/* The one sheet over the app's screen (RFC 0037, layer 3): a Confirm or
 * a Busy, with its own screen id; the screen's softkeys hidden under its
 * own
 */

struct app_sheet_s
{
  uint32_t screen;                /* Its id; 0: none open */
  bool busy;                      /* A Busy, else a Confirm */
  bool cancel;                    /* A Busy the owner may cancel */
  char confirm[sizeof(((FAR pnut_ui_confirm_t *)0)->confirm)];
  FAR lv_obj_t *shadow;
  FAR lv_obj_t *box;
  FAR lv_obj_t *keys;
  struct tpl_busy_s parts;        /* A Busy's */
};

/* The open app's screen (RFC 0037): the screen on top of its own stack, as
 * its last Show described it; the template draws its content
 */

struct app_tpl_s;

struct app_scr_s
{
  uint32_t screen;                /* The app's id of it */
  FAR const struct app_tpl_s *tpl;
  char middle[sizeof(((FAR pnut_ui_show_request_t *)0)->middle)];
  bool middle_off;
  uint8_t noptions;
  uint32_t option_ids[APP_OPTIONS];
  char options[APP_OPTIONS][sizeof(((FAR pnut_ui_option_t *)0)->label)];
  char letters[APP_OPTIONS];      /* '\0': none */
  FAR lv_obj_t *keys;             /* The softkeys */
  bool cancel;                    /* A Busy screen the owner may cancel */
  struct app_sheet_s sheet;

  union
    {
      struct tpl_list_s list;
      struct tpl_detail_s detail;
      struct tpl_busy_s busy;
    } t;
};

/* A template: whether a Show of it holds together, nothing drawn; its
 * content drawn between the header and the softkeys; its keys and taps
 * (true: taken); its share of an Update (a row; 0, or -EINVAL) and of
 * Rows (0 when taken; -EINVAL, dropped, when not the rows asked); what it
 * lets go of
 */

struct app_tpl_s
{
  CODE int (*check)(FAR const pnut_ui_show_request_t *in);
  CODE int (*build)(FAR struct screen_s *s,
                    FAR const pnut_ui_show_request_t *in);
  CODE bool (*key)(FAR struct screen_s *s, uint8_t action);
  CODE void (*event)(FAR struct screen_s *s, uint16_t id);
  CODE int (*update)(FAR struct screen_s *s,
                     FAR const pnut_ui_update_request_t *in);
  CODE int (*rows)(FAR struct screen_s *s,
                   FAR const pnut_ui_rows_request_t *in);
  CODE void (*leave)(FAR struct screen_s *s);
  FAR const char *middle;         /* The primary action's own label */
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern const struct app_tpl_s g_tpl_list;
extern const struct app_tpl_s g_tpl_detail;
extern const struct app_tpl_s g_tpl_empty;
extern const struct app_tpl_s g_tpl_busy;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* For the templates: an event about the screen to the app; whether the
 * primary action is on; an app's text as drawn, into out of size bytes; an
 * icon of the set by its name, or NULL
 */

void app_input(FAR struct screen_s *s, int action, uint32_t row,
               uint32_t option);
void app_page_needed(FAR struct screen_s *s, uint32_t first);
bool app_primary_on(FAR struct screen_s *s);
FAR const char *app_text(FAR const char *in, FAR char *out, size_t size);
FAR const char *app_icon(FAR const char *name);

/* A Busy's parts made in a column (its screen's, or its sheet's), and set
 * from the app's description: the progress, 0 to 100, lights a step for
 * each tenth
 */

void tpl_busy_parts(FAR struct screen_s *s, FAR struct tpl_busy_s *t,
                    FAR lv_obj_t *parent, FAR const pnut_ui_busy_t *busy);
void tpl_busy_set(FAR struct screen_s *s, FAR struct tpl_busy_s *t,
                  FAR const pnut_ui_busy_t *busy);

#endif /* __PNUT_OS_UI_APPTPL_H */
