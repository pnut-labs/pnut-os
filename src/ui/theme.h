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

#include <stdbool.h>
#include <stdint.h>

#include <lvgl/lvgl.h>

#include <pnut/compiler.h>

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

/* The event a block sends its screen when tapped: its id, as the event's
 * parameter (theme_init() registers it)
 */

extern uint32_t g_ui_event_block;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* The canvas's colours at 1 bit: #EEEDE8 white, #161616 black; text the
 * canvas greys (#4A4A48) black at the next smaller size; the light grey
 * (#B4B3AE) as dotted hairlines and outlined fills; no text on grey.
 * Once, after lv_init().
 */

void theme_init(void);

/* A screen: white, its text black, not scrolled, no padding */

void theme_screen(FAR lv_obj_t *scr);

/* A plain box: transparent, no border, padding, scrolling or taps */

void theme_box(FAR lv_obj_t *obj);

/* A label in a font, its colour its parent's */

void theme_label(FAR lv_obj_t *label, FAR const lv_font_t *font);

/* Inverted, or not: the current item (black, its text white) */

void theme_inverted(FAR lv_obj_t *obj, bool inverted);

#endif /* __PNUT_OS_UI_THEME_H */
