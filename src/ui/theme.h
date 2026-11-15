/****************************************************************************
 * pnut-os/src/ui/theme.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_THEME_H
#define __PNUT_OS_UI_THEME_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <lvgl/lvgl.h>

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* The theme built in: the owner's canvas ("E-ink Softkey Phone") at 1 bit.
 * Its fonts, IBM Plex Sans and Plex Mono in the sizes the canvas uses, and
 * its icons are made at build time (tools/mkfont.py, tools/mkicons.py) in
 * build/ui: the icons' names are in icons.h there.
 */

extern const lv_font_t g_ui_sans_11;
extern const lv_font_t g_ui_sans_12;
extern const lv_font_t g_ui_sans_13;
extern const lv_font_t g_ui_sans_14;
extern const lv_font_t g_ui_sans_bold_12;
extern const lv_font_t g_ui_sans_bold_13;
extern const lv_font_t g_ui_sans_bold_14;
extern const lv_font_t g_ui_sans_bold_16;
extern const lv_font_t g_ui_mono_bold_10;
extern const lv_font_t g_ui_mono_bold_11;
extern const lv_font_t g_ui_mono_bold_12;

/* Digits only: the clock's and the PIN's */

extern const lv_font_t g_ui_mono_bold_26;
extern const lv_font_t g_ui_mono_bold_36;

extern const lv_font_t g_ui_icons_11;
extern const lv_font_t g_ui_icons_12;
extern const lv_font_t g_ui_icons_14;
extern const lv_font_t g_ui_icons_16;
extern const lv_font_t g_ui_icons_18;
extern const lv_font_t g_ui_icons_20;

#endif /* __PNUT_OS_UI_THEME_H */
