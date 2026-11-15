/****************************************************************************
 * pnut-os/src/ui/scr_settings.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Settings (Proto-Settings): what works so far, Wi-Fi and Cellular, each
 * with its one-line state from the topics.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "blocks.h"
#include "describe.h"
#include "icons.h"
#include "keymap.h"
#include "list.h"
#include "text.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SETTINGS_WIFI  1
#define SETTINGS_CELL  2

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct settings_scr_s
{
  struct list_s list;
  FAR lv_obj_t *wifi;
  FAR lv_obj_t *cell;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void settings_status(FAR struct screen_s *s, uint8_t what)
{
  FAR struct settings_scr_s *st = s->state;
  FAR const struct ui_status_s *dev = &s->ui->status;
  char line[DESCRIBE_NAME_MAX + 16];

  if (what & STATUS_WIFI)
    {
      if (dev->has_wifi)
        {
          describe_wifi(&dev->wifi, line, sizeof(line));
        }

      block_row_set(st->wifi, NULL, dev->has_wifi ? line : UI_T_UNKNOWN);
    }

  if (what & STATUS_TEL)
    {
      if (dev->has_tel)
        {
          describe_cell(&dev->tel, line, sizeof(line));
        }

      block_row_set(st->cell, NULL, dev->has_tel ? line : UI_T_UNKNOWN);
    }
}

static void settings_open(FAR struct screen_s *s)
{
  FAR struct settings_scr_s *st = s->state;

  if (st->list.focus == st->wifi)
    {
      ui_push(s->ui, &g_scr_wifi, NULL);
    }
  else if (st->list.focus == st->cell)
    {
      ui_push(s->ui, &g_scr_cell, NULL);
    }
}

static void settings_build(FAR struct screen_s *s)
{
  FAR struct settings_scr_s *st = s->state;

  ui_header(s, UI_ICON_SETTINGS, UI_T_SETTINGS);
  list_init(&st->list, s->root);
  st->wifi = block_row(st->list.box, SETTINGS_WIFI, NULL, UI_T_WIFI,
                       UI_T_UNKNOWN);
  st->cell = block_row(st->list.box, SETTINGS_CELL, NULL, UI_T_CELLULAR,
                       UI_T_UNKNOWN);
  block_row_chevron(st->wifi);
  block_row_chevron(st->cell);
  block_softkeys(s->root, "", UI_T_OPEN, UI_T_BACK);
  settings_status(s, STATUS_WIFI | STATUS_TEL);
  list_move(&st->list, 1);
  s->focus = SETTINGS_WIFI;
}

static bool settings_key(FAR struct screen_s *s, uint8_t action,
                         uint32_t ch)
{
  FAR struct settings_scr_s *st = s->state;

  switch (action)
    {
      case UI_ACT_UP:
      case UI_ACT_DOWN:
        list_move(&st->list, action == UI_ACT_DOWN ? 1 : -1);
        s->focus = block_id(st->list.focus);
        return true;

      case UI_ACT_OPEN:
      case UI_ACT_SOFT_MIDDLE:
        settings_open(s);
        return true;

      default:
        return false;
    }
}

static void settings_event(FAR struct screen_s *s, uint16_t id)
{
  FAR struct settings_scr_s *st = s->state;
  FAR lv_obj_t *row = list_find(&st->list, id);

  if (row != NULL)
    {
      list_focus(&st->list, row);
      s->focus = id;
      settings_open(s);
    }
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct screen_class_s g_scr_settings =
{
  "settings",
  settings_build,
  settings_key,
  settings_event,
  settings_status,
  NULL,
  sizeof(struct settings_scr_s)
};
