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
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* An app's options, at most (ui.proto); an app's text as drawn, every
 * byte perhaps \xNN (describe_name())
 */

#define APP_OPTIONS   8
#define APP_TEXT_MAX  (64 * 4 + 1)

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The List template's state */

struct tpl_list_s
{
  struct applist_s list;
  FAR lv_obj_t *rows[APPLIST_PER_PAGE];   /* The page's, reused */
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

  union
    {
      struct tpl_list_s list;
    } t;
};

/* A template: whether a Show of it holds together, nothing drawn; its
 * content drawn between the header and the softkeys; its keys and taps
 * (true: taken); its share of an Update (a row; 0, or -EINVAL) and of
 * Rows; what it lets go of
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
  CODE void (*rows)(FAR struct screen_s *s,
                    FAR const pnut_ui_rows_request_t *in);
  CODE void (*leave)(FAR struct screen_s *s);
  FAR const char *middle;         /* The primary action's own label */
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern const struct app_tpl_s g_tpl_list;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* For the templates: an event about the screen to the app; whether the
 * primary action is on; an app's text as drawn (APP_TEXT_MAX); an icon of
 * the set by its name, or NULL
 */

void app_input(FAR struct screen_s *s, int action, uint32_t row,
               uint32_t option);
void app_page_needed(FAR struct screen_s *s, uint32_t first);
bool app_primary_on(FAR struct screen_s *s);
FAR const char *app_text(FAR const char *in, FAR char *out);
FAR const char *app_icon(FAR const char *name);

#endif /* __PNUT_OS_UI_APPTPL_H */
