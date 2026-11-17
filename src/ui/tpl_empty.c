/****************************************************************************
 * pnut-os/src/ui/tpl_empty.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* An app's Empty (RFC 0037, the canvas's Cat-Empty): nothing yet, said
 * with an icon of the set, a heading and a line of what to do, at the
 * left, in the middle of the screen; the primary action is the app's
 * (New), sent as the middle action.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "apptpl.h"
#include "blocks.h"
#include "keymap.h"
#include "theme.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Cat-Empty: 16 px at the sides, 8 between the parts */

#define TPL_EMPTY_PAD  16
#define TPL_EMPTY_GAP  8

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int tpl_empty_check(FAR const pnut_ui_show_request_t *in)
{
  return 0;
}

static int tpl_empty_build(FAR struct screen_s *s,
                           FAR const pnut_ui_show_request_t *in)
{
  FAR char *text = s->ui->text_long;
  FAR const pnut_ui_empty_t *e = &in->content.empty;
  FAR const char *icon = app_icon(e->icon);
  FAR lv_obj_t *box;
  FAR lv_obj_t *label;

  box = lv_obj_create(s->root);
  theme_box(box);
  lv_obj_set_pos(box, 0, BLOCK_HEADER_H);
  lv_obj_set_size(box, 240, 320 - BLOCK_HEADER_H - BLOCK_FOOTER_H);
  lv_obj_set_style_pad_hor(box, TPL_EMPTY_PAD, 0);
  lv_obj_set_style_pad_row(box, TPL_EMPTY_GAP, 0);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);

  if (icon != NULL)
    {
      label = lv_label_create(box);
      theme_label(label, &g_ui_icons_20);
      lv_label_set_text(label, icon);
    }

  label = lv_label_create(box);
  theme_label(label, &g_ui_sans_bold_16);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, lv_pct(100));
  lv_label_set_text(label, app_text(e->heading, text, UI_TEXT_LONG));

  label = lv_label_create(box);
  theme_label(label, &g_ui_sans_12);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, lv_pct(100));
  lv_label_set_text(label, app_text(e->text, text, UI_TEXT_LONG));
  return 0;
}

static bool tpl_empty_key(FAR struct screen_s *s, uint8_t action)
{
  switch (action)
    {
      case UI_ACT_OPEN:
      case UI_ACT_SOFT_MIDDLE:
        if (app_primary_on(s))
          {
            app_input(s, PNUT_UI_INPUT_ACTION_MIDDLE, 0, 0);
          }

        return true;

      default:
        return false;
    }
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct app_tpl_s g_tpl_empty =
{
  tpl_empty_check,
  tpl_empty_build,
  tpl_empty_key,
  NULL,
  NULL,
  NULL,
  NULL,
  ""
};
