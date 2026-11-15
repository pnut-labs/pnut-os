/****************************************************************************
 * pnut-os/src/ui/theme.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The theme built in: the owner's canvas ("E-ink Softkey Phone") at 1 bit.
 * Its fonts and icons are made at build time; here are the rules every
 * block follows (theme.h).
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "theme.h"

/****************************************************************************
 * Public Data
 ****************************************************************************/

uint32_t g_ui_event_block;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void theme_init(void)
{
  g_ui_event_block = lv_event_register_id();
}

void theme_screen(FAR lv_obj_t *scr)
{
  lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(scr, lv_color_black(), 0);
  lv_obj_set_style_pad_all(scr, 0, 0);
  lv_obj_set_style_border_width(scr, 0, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

void theme_box(FAR lv_obj_t *obj)
{
  lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_radius(obj, 0, 0);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

void theme_label(FAR lv_obj_t *label, FAR const lv_font_t *font)
{
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
}

void theme_inverted(FAR lv_obj_t *obj, bool inverted)
{
  lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(obj, inverted ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  lv_obj_set_style_text_color(obj, inverted ? lv_color_white() :
                                            lv_color_black(), 0);
}
