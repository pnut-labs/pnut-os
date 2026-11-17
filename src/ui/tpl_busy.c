/****************************************************************************
 * pnut-os/src/ui/tpl_busy.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* An app's Busy (RFC 0037, the canvas's Cat-Busy): what it waits for, a
 * bar of ten steps when the app says how far it is, and a line of how it
 * goes; Cancel when the app lets the owner cancel.  As a screen, or as a
 * sheet over the app's screen (app.c draws the frame).
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>

#include "apptpl.h"
#include "blocks.h"
#include "keymap.h"
#include "theme.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Cat-Busy: 12 px at the sides, 10 between the parts; the bar's ten steps,
 * 10 px high, 2 apart, 216 px in all
 */

#define TPL_BUSY_PAD       12
#define TPL_BUSY_GAP       10
#define TPL_BUSY_STEP_H    10
#define TPL_BUSY_STEP_GAP  2

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int tpl_busy_check(FAR const pnut_ui_show_request_t *in)
{
  FAR const pnut_ui_busy_t *b = &in->content.busy;

  return b->has_progress && b->progress > 100 ? -EINVAL : 0;
}

static int tpl_busy_build(FAR struct screen_s *s,
                          FAR const pnut_ui_show_request_t *in)
{
  FAR struct app_scr_s *st = s->state;
  FAR lv_obj_t *box;

  box = lv_obj_create(s->root);
  theme_box(box);
  lv_obj_set_pos(box, 0, BLOCK_HEADER_H);
  lv_obj_set_size(box, 240, 320 - BLOCK_HEADER_H - BLOCK_FOOTER_H);
  lv_obj_set_style_pad_hor(box, TPL_BUSY_PAD, 0);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  tpl_busy_parts(&st->t.busy, box, &in->content.busy);
  st->cancel = in->content.busy.cancel;
  return 0;
}

static int tpl_busy_update(FAR struct screen_s *s,
                           FAR const pnut_ui_update_request_t *in)
{
  FAR struct app_scr_s *st = s->state;

  if (in->which_change != PNUT_UI_UPDATE_REQUEST_BUSY_TAG ||
      (in->change.busy.has_progress && in->change.busy.progress > 100))
    {
      return -EINVAL;
    }

  tpl_busy_set(&st->t.busy, &in->change.busy);
  st->cancel = in->change.busy.cancel;
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void tpl_busy_parts(FAR struct tpl_busy_s *t, FAR lv_obj_t *parent,
                    FAR const pnut_ui_busy_t *busy)
{
  FAR lv_obj_t *step;
  int i;

  lv_obj_set_style_pad_row(parent, TPL_BUSY_GAP, 0);

  t->heading = lv_label_create(parent);
  theme_label(t->heading, &g_ui_sans_bold_16);
  lv_label_set_long_mode(t->heading, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(t->heading, lv_pct(100));

  t->bar = lv_obj_create(parent);
  theme_box(t->bar);
  lv_obj_set_size(t->bar, lv_pct(100), TPL_BUSY_STEP_H);
  lv_obj_set_flex_flow(t->bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(t->bar, TPL_BUSY_STEP_GAP, 0);
  for (i = 0; i < TPL_BUSY_STEPS; i++)
    {
      step = lv_obj_create(t->bar);
      theme_box(step);
      lv_obj_set_height(step, TPL_BUSY_STEP_H);
      lv_obj_set_flex_grow(step, 1);
      lv_obj_set_style_border_width(step, 1, 0);
      lv_obj_set_style_border_color(step, lv_color_black(), 0);
      lv_obj_set_style_bg_color(step, lv_color_black(), 0);
      t->steps[i] = step;
    }

  t->text = lv_label_create(parent);
  theme_label(t->text, &g_ui_sans_12);
  lv_label_set_long_mode(t->text, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(t->text, lv_pct(100));

  tpl_busy_set(t, busy);
}

void tpl_busy_set(FAR struct tpl_busy_s *t, FAR const pnut_ui_busy_t *busy)
{
  static char text[APP_TEXT_LONG];
  int lit = busy->has_progress ?
            (int)(busy->progress * TPL_BUSY_STEPS) / 100 : 0;
  int i;

  block_set_text(t->heading, app_text(busy->heading, text, sizeof(text)));
  block_set_text(t->text, app_text(busy->text, text, sizeof(text)));
  if (busy->has_progress)
    {
      lv_obj_remove_flag(t->bar, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_add_flag(t->bar, LV_OBJ_FLAG_HIDDEN);
    }

  for (i = 0; i < TPL_BUSY_STEPS; i++)
    {
      lv_obj_set_style_bg_opa(t->steps[i], i < lit ? LV_OPA_COVER :
                                                     LV_OPA_TRANSP, 0);
    }
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct app_tpl_s g_tpl_busy =
{
  tpl_busy_check,
  tpl_busy_build,
  NULL,
  NULL,
  tpl_busy_update,
  NULL,
  NULL,
  ""
};
