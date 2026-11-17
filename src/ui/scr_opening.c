/****************************************************************************
 * pnut-os/src/ui/scr_opening.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The system UI's own Busy while an app opens (RFC 0037, The life cycle):
 * drawn when the app has drawn nothing 0.7 s after it was started, with
 * the app's icon and name from its manifest, and Cancel, which stops it.
 * The app's first Show takes its place (app.c).
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "apptpl.h"
#include "blocks.h"
#include "host.h"
#include "text.h"
#include "theme.h"
#include "ui.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct opening_s
{
  struct tpl_busy_s busy;         /* First: the tests read it */
  FAR struct host_app_s *app;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void opening_build(FAR struct screen_s *s)
{
  FAR struct opening_s *o = s->state;
  FAR struct host_app_s *app = (FAR struct host_app_s *)s->arg;
  FAR char *heading = s->ui->texts[0];
  pnut_ui_busy_t busy;
  FAR lv_obj_t *box;

  o->app = app;
  ui_header(s, app_icon(app->manifest.icon), app->manifest.name);

  memset(&busy, 0, sizeof(busy));
  snprintf(heading, UI_TEXT_MAX, UI_T_OPENING, app->manifest.name);
  strlcpy(busy.heading, heading, sizeof(busy.heading));
  busy.cancel = true;

  box = lv_obj_create(s->root);
  theme_box(box);
  lv_obj_set_pos(box, 0, BLOCK_HEADER_H);
  lv_obj_set_size(box, 240, 320 - BLOCK_HEADER_H - BLOCK_FOOTER_H);
  lv_obj_set_style_pad_hor(box, 12, 0);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  tpl_busy_parts(s, &o->busy, box, &busy);
  block_softkeys(s->root, "", "", UI_T_CANCEL);
}

/* Back or Cancel stops the app; nothing else does anything */

static bool opening_key(FAR struct screen_s *s, uint8_t action, uint32_t ch)
{
  FAR struct opening_s *o = s->state;

  if (action == UI_ACT_BACK || action == UI_ACT_SOFT_RIGHT)
    {
      host_cancel(o->app);        /* Takes this screen off */
    }

  return true;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct screen_class_s g_scr_opening =
{
  "opening",
  opening_build,
  opening_key,
  NULL,
  NULL,
  NULL,
  sizeof(struct opening_s)
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR struct screen_s *app_opening_screen(FAR struct ui_s *ui,
                                        FAR const struct host_app_s *app)
{
  int i;

  for (i = ui->depth - 1; i >= 0; i--)
    {
      if (ui->stack[i]->cls == &g_scr_opening &&
          (app == NULL || ui->stack[i]->arg == app))
        {
          return ui->stack[i];
        }
    }

  return NULL;
}

void app_opening(FAR struct ui_s *ui, FAR struct host_app_s *app)
{
  /* As the app's own first screen: beneath the SIM PIN, which is the
   * system's (RFC 0037, layer 5)
   */

  if (app_opening_screen(ui, app) != NULL)
    {
      return;
    }

  if (ui_top(ui) != NULL && ui_top(ui)->cls == &g_scr_pin)
    {
      ui_insert(ui, ui->depth - 1, &g_scr_opening, app);
    }
  else
    {
      ui_push(ui, &g_scr_opening, app);
    }
}

void app_opening_gone(FAR struct ui_s *ui, FAR const struct host_app_s *app)
{
  FAR struct screen_s *s = app_opening_screen(ui, app);

  if (s != NULL)
    {
      ui_remove(ui, s);
    }
}
