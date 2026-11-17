/****************************************************************************
 * pnut-os/src/ui/apps.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_APPS_H
#define __PNUT_OS_UI_APPS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/compiler.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Menu's apps the softkeys of Home open */

#define UI_APP_CONTACTS  2
#define UI_APP_CALENDAR  4

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct ui_folder_s;

/* An app of the system, as Menu shows it: the native apps that will ship
 * with it.  Settings is the system UI's; a native app has its id (its
 * service's name, RFC 0037); the others are placeholders.
 */

struct ui_app_s
{
  FAR const char *name;
  FAR const char *icon;                 /* icons.h's */
  FAR const struct ui_folder_s *folder; /* A folder: its apps */
  bool settings;                        /* Settings itself */
  FAR const char *id;                   /* A native app's: its id */
};

struct ui_folder_s
{
  FAR const char *name;
  FAR const char *icon;
  FAR const struct ui_app_s *apps;
  uint8_t count;
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* Menu in the owner's canvas's order (Proto-Menu, Proto-Menu-2), its two
 * folders (Proto-Folder-Tools, -Games), Home's six shortcuts
 */

extern const struct ui_folder_s g_ui_menu;
extern const struct ui_folder_s g_ui_tools;
extern const struct ui_folder_s g_ui_games;
extern const struct ui_folder_s g_ui_shortcuts;

#endif /* __PNUT_OS_UI_APPS_H */
