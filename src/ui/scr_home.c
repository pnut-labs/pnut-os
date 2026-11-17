/****************************************************************************
 * pnut-os/src/ui/scr_home.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Home (Proto-Home): the date in the header, the big clock, six shortcuts.
 * The rows the canvas shows under them are apps' widgets, left out until
 * the apps exist.  The softkeys: Calendar, Menu, Contacts; Back does
 * nothing here.  A and D move along the shortcuts; E opens one.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "app.h"
#include "apps.h"
#include "blocks.h"
#include "describe.h"
#include "icons.h"
#include "keymap.h"
#include "page.h"
#include "text.h"
#include "theme.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HOME_CLOCK_H     48
#define HOME_SHORTCUTS   6
#define HOME_CELL_W      40
#define HOME_CELL_H      36

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct home_s
{
  struct page_s page;
  FAR lv_obj_t *clock;
  FAR lv_obj_t *cells[HOME_SHORTCUTS];
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The date and the clock, "—" until the zone is known */

static void home_time(FAR struct screen_s *s)
{
  FAR struct home_s *h = s->state;
  struct tm tm;
  char text[24];

  if (!s->ui->status.has_time)
    {
      block_header_title(s->header, UI_T_UNKNOWN);
      block_set_text(h->clock, UI_T_UNKNOWN);
      return;
    }

  ui_local(s->ui, &tm);
  describe_date(&tm, text, sizeof(text));
  block_header_title(s->header, text);
  describe_clock(&tm, text, sizeof(text));
  block_set_text(h->clock, text);
}

static void home_open(FAR struct screen_s *s, FAR const struct ui_app_s *app)
{
  app_launch(s->ui, app);
}

static void home_build(FAR struct screen_s *s)
{
  FAR struct home_s *h = s->state;
  int32_t y = BLOCK_HEADER_H + HOME_CLOCK_H;
  int i;

  ui_header(s, NULL, UI_T_UNKNOWN);

  h->clock = lv_label_create(s->root);
  theme_label(h->clock, &g_ui_mono_bold_36);
  lv_obj_set_style_text_letter_space(h->clock, -2, 0);
  lv_label_set_text(h->clock, UI_T_UNKNOWN);
  lv_obj_set_pos(h->clock, 10, BLOCK_HEADER_H +
                 (HOME_CLOCK_H - g_ui_mono_bold_36.line_height) / 2);

  block_rule(s->root, 0, y, 240, true);
  for (i = 0; i < HOME_SHORTCUTS; i++)
    {
      h->cells[i] = block_shortcut(s->root, i,
                                   g_ui_shortcuts.apps[i].icon);
      lv_obj_set_pos(h->cells[i], i * HOME_CELL_W, y + 1);
      lv_obj_set_size(h->cells[i], HOME_CELL_W, HOME_CELL_H);
      block_focus(h->cells[i], i == 0);
    }

  block_rule(s->root, 0, y + 1 + HOME_CELL_H, 240, false);
  block_softkeys(s->root, g_ui_menu.apps[UI_APP_CALENDAR].name,
                 UI_ICON_MENU, g_ui_menu.apps[UI_APP_CONTACTS].name);
  page_grid(&h->page, HOME_SHORTCUTS, HOME_SHORTCUTS, 1);
  home_time(s);
}

static bool home_key(FAR struct screen_s *s, uint8_t action, uint32_t ch)
{
  FAR struct home_s *h = s->state;
  uint16_t focus = h->page.focus;

  switch (action)
    {
      case UI_ACT_LEFT:
      case UI_ACT_RIGHT:
        if (page_move(&h->page, action))
          {
            block_focus(h->cells[focus], false);
            block_focus(h->cells[h->page.focus], true);
            s->focus = h->page.focus;
          }

        return true;

      case UI_ACT_OPEN:
        home_open(s, &g_ui_shortcuts.apps[h->page.focus]);
        return true;

      case UI_ACT_SOFT_LEFT:
        home_open(s, &g_ui_menu.apps[UI_APP_CALENDAR]);
        return true;

      case UI_ACT_SOFT_MIDDLE:
        ui_push(s->ui, &g_scr_menu, NULL);
        return true;

      case UI_ACT_SOFT_RIGHT:
        home_open(s, &g_ui_menu.apps[UI_APP_CONTACTS]);
        return true;

      case UI_ACT_CHAR:
        return false;

      default:

        /* Back, and the rest: nothing on Home */

        return true;
    }
}

static void home_event(FAR struct screen_s *s, uint16_t id)
{
  FAR struct home_s *h = s->state;

  if (id < HOME_SHORTCUTS)
    {
      block_focus(h->cells[h->page.focus], false);
      h->page.focus = id;
      s->focus      = id;
      block_focus(h->cells[id], true);
      home_open(s, &g_ui_shortcuts.apps[id]);
    }
}

static void home_status(FAR struct screen_s *s, uint8_t what)
{
  if ((what & STATUS_TIME) != 0)
    {
      home_time(s);
    }
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct screen_class_s g_scr_home =
{
  "home",
  home_build,
  home_key,
  home_event,
  home_status,
  NULL,
  sizeof(struct home_s)
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/
