/****************************************************************************
 * pnut-os/src/ui/apps.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The system's native apps, as Menu and Home show them, in the owner's
 * canvas's order.  The names are marked for translation (text.h).
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>

#include "apps.h"
#include "icons.h"
#include "text.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct ui_app_s g_tools[] =
{
  { N_("Clock"),      UI_ICON_CLOCK,      NULL, false },
  { N_("Files"),      UI_ICON_FILES,      NULL, false },
  { N_("Calculator"), UI_ICON_CALCULATOR, NULL, false },
  { N_("Recorder"),   UI_ICON_RECORDER,   NULL, false },
};

static const struct ui_app_s g_games[] =
{
  { N_("Chess"),      UI_ICON_CHESS,      NULL, false },
  { N_("2048"),       UI_ICON_GAME_2048,  NULL, false },
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct ui_folder_s g_ui_tools =
{
  N_("Tools"), UI_ICON_FOLDER, g_tools, sizeof(g_tools) / sizeof(g_tools[0])
};

const struct ui_folder_s g_ui_games =
{
  N_("Games"), UI_ICON_FOLDER, g_games, sizeof(g_games) / sizeof(g_games[0])
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct ui_app_s g_menu[] =
{
  { N_("Messages"),   UI_ICON_MESSAGES,   NULL,        false },
  { N_("Phone"),      UI_ICON_PHONE,      NULL,        false },
  { N_("Contacts"),   UI_ICON_CONTACTS,   NULL,        false },
  { N_("Email"),      UI_ICON_EMAIL,      NULL,        false },
  { N_("Calendar"),   UI_ICON_CALENDAR,   NULL,        false },
  { N_("Reminders"),  UI_ICON_REMINDERS,  NULL,        false },
  { N_("Notes"),      UI_ICON_NOTES,      NULL,        false },
  { N_("Maps"),       UI_ICON_MAPS,       NULL,        false },
  { N_("Weather"),    UI_ICON_WEATHER,    NULL,        false },
  { N_("Reader"),     UI_ICON_READER,     NULL,        false },
  { N_("Read later"), UI_ICON_READ_LATER, NULL,        false },
  { N_("Settings"),   UI_ICON_SETTINGS,   NULL,        true },
  { N_("Podcasts"),   UI_ICON_PODCASTS,   NULL,        false },
  { N_("Music"),      UI_ICON_MUSIC,      NULL,        false },
  { N_("AI chat"),    UI_ICON_AI_CHAT,    NULL,        false },
  { N_("Passwords"),  UI_ICON_PASSWORDS,  NULL,        false },
  { N_("Wallet"),     UI_ICON_WALLET,     NULL,        false },
  { N_("Tools"),      UI_ICON_FOLDER,     &g_ui_tools, false },
  { N_("Games"),      UI_ICON_FOLDER,     &g_ui_games, false },
};

static const struct ui_app_s g_shortcuts[] =
{
  { N_("Messages"),   UI_ICON_MESSAGES,   NULL,        false },
  { N_("Phone"),      UI_ICON_PHONE,      NULL,        false },
  { N_("Calendar"),   UI_ICON_CALENDAR,   NULL,        false },
  { N_("Reminders"),  UI_ICON_REMINDERS,  NULL,        false },
  { N_("Reader"),     UI_ICON_READER,     NULL,        false },
  { N_("Maps"),       UI_ICON_MAPS,       NULL,        false },
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct ui_folder_s g_ui_menu =
{
  N_("Menu"), UI_ICON_MENU, g_menu, sizeof(g_menu) / sizeof(g_menu[0])
};

const struct ui_folder_s g_ui_shortcuts =
{
  N_("Home"), NULL, g_shortcuts, sizeof(g_shortcuts) / sizeof(g_shortcuts[0])
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/
