/****************************************************************************
 * pnut-os/src/ui/scr_unavailable.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* An app that will ship with the system, not built yet: its name and icon,
 * and plainly "Not available yet", with Back.  Nothing on it looks as if
 * it works.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "apps.h"
#include "blocks.h"
#include "text.h"
#include "theme.h"
#include "ui.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void unavailable_build(FAR struct screen_s *s)
{
  FAR const struct ui_app_s *app = s->arg;
  FAR lv_obj_t *label;

  ui_header(s, app->icon, app->name);
  label = lv_label_create(s->root);
  theme_label(label, &g_ui_sans_14);
  lv_label_set_text(label, UI_T_NOT_AVAILABLE);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0,
               BLOCK_HEADER_H + (320 - BLOCK_HEADER_H - BLOCK_FOOTER_H) / 2
               - 10);
  block_softkeys(s->root, "", "", UI_T_BACK);
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct screen_class_s g_scr_unavailable =
{
  "unavailable",
  unavailable_build,
  NULL,
  NULL,
  NULL,
  NULL,
  0
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/
