/****************************************************************************
 * pnut-os/src/ui/scr_opening.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The system UI's own Busy over an app (RFC 0037, The life cycle and
 * Failures), with the app's icon and name from its manifest: "Opening
 * <app>…" when it has drawn nothing 0.7 s after it was started, Cancel
 * stopping it, its first Show taking the Busy's place (app.c); "Waiting for
 * <app>…" when an answer is owed for 3 s, Cancel closing it; "<app> isn't
 * responding" at 10 s, Wait or Close.  And for an app disabled after it
 * crashed three times in a row, "<app> stopped working", Try again or
 * Cancel.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "apps.h"
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

/* The Busy's parts in the content's box */

static void opening_parts(FAR struct screen_s *s, FAR struct tpl_busy_s *t,
                          FAR const pnut_ui_busy_t *busy)
{
  FAR lv_obj_t *box;

  box = lv_obj_create(s->root);
  theme_box(box);
  lv_obj_set_pos(box, 0, BLOCK_HEADER_H);
  lv_obj_set_size(box, 240, 320 - BLOCK_HEADER_H - BLOCK_FOOTER_H);
  lv_obj_set_style_pad_hor(box, 12, 0);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  tpl_busy_parts(s, t, box, busy);
}

static void opening_build(FAR struct screen_s *s)
{
  FAR struct opening_s *o = s->state;
  FAR struct host_app_s *app = (FAR struct host_app_s *)s->arg;
  FAR char *heading = s->ui->texts[0];
  FAR const char *name;
  pnut_ui_busy_t busy;

  /* A developer's program without a manifest has its id for a name */

  name = app->manifest.name[0] != '\0' ? app->manifest.name : app->id;
  o->app = app;
  ui_header(s, app_icon(app->manifest.icon), name);

  memset(&busy, 0, sizeof(busy));
  snprintf(heading, UI_TEXT_MAX,
           app->wait == HOST_WAIT_HUNG ? UI_T_HUNG :
           app->wait == HOST_WAIT_WAITING ? UI_T_WAITING : UI_T_OPENING,
           name);
  strlcpy(busy.heading, heading, sizeof(busy.heading));
  if (app->wait == HOST_WAIT_HUNG)
    {
      strlcpy(busy.text, UI_T_HUNG_TEXT, sizeof(busy.text));
    }

  opening_parts(s, &o->busy, &busy);
  if (app->wait == HOST_WAIT_HUNG)
    {
      block_softkeys(s->root, UI_T_WAIT, "", UI_T_CLOSE);
    }
  else
    {
      block_softkeys(s->root, "", "", UI_T_CANCEL);
    }
}

/* Opening: Back or Cancel stops the app.  Waiting: they close it.  Not
 * responding: Wait (or Enter) waits, Close (or Back) closes.  Nothing
 * else does anything.
 */

static bool opening_key(FAR struct screen_s *s, uint8_t action, uint32_t ch)
{
  FAR struct opening_s *o = s->state;
  bool close = action == UI_ACT_BACK || action == UI_ACT_SOFT_RIGHT;

  if (o->app->wait == HOST_WAIT_HUNG &&
      (action == UI_ACT_SOFT_LEFT || action == UI_ACT_OPEN))
    {
      host_wait(o->app);          /* Builds this screen again */
    }
  else if (close && o->app->wait == HOST_WAIT_OPENING)
    {
      host_cancel(o->app);        /* Takes this screen off */
    }
  else if (close)
    {
      host_close_app(o->app);
    }

  return true;
}

/* An app disabled: "<app> stopped working", with Menu's tile's icon and
 * name
 */

static void stopped_build(FAR struct screen_s *s)
{
  FAR const struct ui_app_s *a = s->arg;
  FAR char *heading = s->ui->texts[0];
  pnut_ui_busy_t busy;

  ui_header(s, a->icon, a->name);
  memset(&busy, 0, sizeof(busy));
  snprintf(heading, UI_TEXT_MAX, UI_T_STOPPED, a->name);
  strlcpy(busy.heading, heading, sizeof(busy.heading));
  strlcpy(busy.text, UI_T_TRY_AGAIN_Q, sizeof(busy.text));
  opening_parts(s, s->state, &busy);
  block_softkeys(s->root, UI_T_TRY_AGAIN, "", UI_T_CANCEL);
}

/* Try again (or Enter): turned on and opened; Cancel or Back: off */

static bool stopped_key(FAR struct screen_s *s, uint8_t action, uint32_t ch)
{
  FAR struct ui_s *ui = s->ui;
  FAR const struct ui_app_s *a = s->arg;

  if (action == UI_ACT_SOFT_LEFT || action == UI_ACT_OPEN)
    {
      host_enable(&ui->host, a->id);
      ui_pop(ui);                 /* This screen is gone */
      app_launch(ui, a);
      return true;
    }

  return action != UI_ACT_BACK && action != UI_ACT_SOFT_RIGHT;
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

const struct screen_class_s g_scr_stopped =
{
  "stopped",
  stopped_build,
  stopped_key,
  NULL,
  NULL,
  NULL,
  sizeof(struct tpl_busy_s)
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

void app_opening_mode(FAR struct ui_s *ui, FAR struct host_app_s *app)
{
  FAR struct screen_s *s = app_opening_screen(ui, app);

  if (s != NULL)
    {
      ui_replace(ui, s, &g_scr_opening, app);
    }
  else
    {
      app_opening(ui, app);
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
