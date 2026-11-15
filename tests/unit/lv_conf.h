/****************************************************************************
 * pnut-os/tests/unit/lv_conf.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* LVGL as the system UI's tests build it on the computer.  The device's
 * LVGL is configured through Kconfig (configs/tdeck-max/fragment.config):
 * what is set here follows what is set there, and the rest is LVGL's
 * default on both.
 */

/* LVGL checks for this guard's name */

#ifndef LV_CONF_H
#define LV_CONF_H

/* 1 bit, as the e-paper screen */

#define LV_COLOR_DEPTH            1

/* The C library's allocator, strings and formatting */

#define LV_USE_STDLIB_MALLOC      LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING      LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF     LV_STDLIB_CLIB

#define LV_USE_LOG                0

/* A letter a font lacks is left out, not boxed (Kconfig's default) */

#define LV_USE_FONT_PLACEHOLDER   0

/* Labels and plain objects only: the UI's blocks are built from them */

#define LV_USE_ANIMIMG            0
#define LV_USE_ARC                0
#define LV_USE_BAR                0
#define LV_USE_BUTTON             0
#define LV_USE_BUTTONMATRIX       0
#define LV_USE_CALENDAR           0
#define LV_USE_CANVAS             0
#define LV_USE_CHART              0
#define LV_USE_CHECKBOX           0
#define LV_USE_DROPDOWN           0
#define LV_USE_IMAGE              0
#define LV_USE_IMAGEBUTTON        0
#define LV_USE_KEYBOARD           0
#define LV_USE_LABEL              1
#define LV_USE_LED                0
#define LV_USE_LINE               0
#define LV_USE_LIST               0
#define LV_USE_MENU               0
#define LV_USE_MSGBOX             0
#define LV_USE_ROLLER             0
#define LV_USE_SCALE              0
#define LV_USE_SLIDER             0
#define LV_USE_SPAN               0
#define LV_USE_SPINBOX            0
#define LV_USE_SPINNER            0
#define LV_USE_SWITCH             0
#define LV_USE_TEXTAREA           0
#define LV_USE_TABLE              0
#define LV_USE_TABVIEW            0
#define LV_USE_TILEVIEW           0
#define LV_USE_WIN                0

/* The UI's own theme, no LVGL theme */

#define LV_USE_THEME_DEFAULT      0
#define LV_USE_THEME_SIMPLE       0
#define LV_USE_THEME_MONO         0

#define LV_USE_OBSERVER           0

#endif /* LV_CONF_H */
