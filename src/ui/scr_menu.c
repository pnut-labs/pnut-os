/****************************************************************************
 * pnut-os/src/ui/scr_menu.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Menu, and a folder of it: the apps as tiles, 3 x 4 a page (Proto-Menu,
 * Proto-Folder-Tools).  W, S, A, D move, flowing across pages; Space and
 * Shift+Space turn them; E, Enter and the middle softkey open the tile.
 * A folder opens its own grid; an app not built yet its placeholder.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdio.h>

#include "apps.h"
#include "blocks.h"
#include "keymap.h"
#include "page.h"
#include "text.h"
#include "theme.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MENU_COLS    3
#define MENU_ROWS    4
#define MENU_PER     (MENU_COLS * MENU_ROWS)

/* The grid between the header and the pages' row, 3 px in, tiles 2 px
 * apart
 */

#define MENU_GRID_H  (320 - BLOCK_HEADER_H - BLOCK_PAGES_H - BLOCK_FOOTER_H)
#define MENU_PAD     3
#define MENU_TILE_W  76
#define MENU_TILE_H  50

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct menu_s
{
  FAR const struct ui_folder_s *folder;
  struct page_s page;
  FAR lv_obj_t *grid;
  FAR lv_obj_t *foot;
  FAR lv_obj_t *tiles[MENU_PER];
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The page's tiles, and the foot: the pages for Menu, the folder's line
 * for a folder
 */

static void menu_page(FAR struct screen_s *s)
{
  FAR struct menu_s *m = s->state;
  FAR const struct ui_app_s *app;
  FAR lv_obj_t *label;
  char text[32];
  uint16_t first = m->page.page * MENU_PER;
  uint16_t i;

  if (m->grid != NULL)
    {
      lv_obj_delete(m->grid);
      lv_obj_delete(m->foot);
    }

  m->grid = lv_obj_create(s->root);
  theme_box(m->grid);
  lv_obj_set_pos(m->grid, 0, BLOCK_HEADER_H);
  lv_obj_set_size(m->grid, 240, MENU_GRID_H);

  for (i = 0; i < MENU_PER; i++)
    {
      m->tiles[i] = NULL;
      if (first + i >= m->folder->count)
        {
          continue;
        }

      app = &m->folder->apps[first + i];
      m->tiles[i] = block_tile(m->grid, first + i, app->icon, app->name);
      lv_obj_set_pos(m->tiles[i],
                     MENU_PAD + (i % MENU_COLS) * (MENU_TILE_W + 2),
                     MENU_PAD + (i / MENU_COLS) * (MENU_TILE_H + 2));
      lv_obj_set_size(m->tiles[i], MENU_TILE_W, MENU_TILE_H);
      block_focus(m->tiles[i], first + i == m->page.focus);
    }

  if (m->folder == &g_ui_menu)
    {
      m->foot = block_pages(s->root, m->page.page, page_count(&m->page));
      return;
    }

  m->foot = lv_obj_create(s->root);
  theme_box(m->foot);
  lv_obj_set_pos(m->foot, 0, BLOCK_HEADER_H + MENU_GRID_H);
  lv_obj_set_size(m->foot, 240, BLOCK_PAGES_H);
  block_rule(m->foot, 0, 0, 240, true);
  snprintf(text, sizeof(text), UI_T_FOLDER_APPS, m->folder->count);
  label = lv_label_create(m->foot);
  theme_label(label, &g_ui_mono_bold_10);
  lv_label_set_text(label, text);
  lv_obj_center(label);
}

static void menu_open(FAR struct screen_s *s)
{
  FAR struct menu_s *m = s->state;
  FAR const struct ui_app_s *app = &m->folder->apps[m->page.focus];

  if (app->folder != NULL)
    {
      ui_push(s->ui, &g_scr_menu, app->folder);
    }
  else
    {
      ui_push(s->ui, &g_scr_unavailable, app);
    }
}

static bool menu_move(FAR struct screen_s *s, uint8_t action)
{
  FAR struct menu_s *m = s->state;
  uint16_t page = m->page.page;
  uint16_t focus = m->page.focus;

  if (!page_move(&m->page, action))
    {
      return true;
    }

  s->focus = m->page.focus;
  if (m->page.page != page)
    {
      menu_page(s);
    }
  else
    {
      block_focus(m->tiles[focus % MENU_PER], false);
      block_focus(m->tiles[m->page.focus % MENU_PER], true);
    }

  return true;
}

static void menu_build(FAR struct screen_s *s)
{
  FAR struct menu_s *m = s->state;

  m->folder = s->arg != NULL ? s->arg : &g_ui_menu;
  ui_header(s, m->folder->icon, m->folder->name);
  block_softkeys(s->root, "", UI_T_OPEN, UI_T_BACK);
  page_grid(&m->page, m->folder->count, MENU_COLS, MENU_ROWS);
  menu_page(s);
}

static bool menu_key(FAR struct screen_s *s, uint8_t action, uint32_t ch)
{
  switch (action)
    {
      case UI_ACT_UP:
      case UI_ACT_DOWN:
      case UI_ACT_LEFT:
      case UI_ACT_RIGHT:
      case UI_ACT_PAGE_NEXT:
      case UI_ACT_PAGE_PREV:
        return menu_move(s, action);

      case UI_ACT_OPEN:
      case UI_ACT_SOFT_MIDDLE:
        menu_open(s);
        return true;

      default:
        return false;
    }
}

static void menu_event(FAR struct screen_s *s, uint16_t id)
{
  FAR struct menu_s *m = s->state;

  if (id == BLOCK_ID_PAGE_PREV || id == BLOCK_ID_PAGE_NEXT)
    {
      menu_move(s, id == BLOCK_ID_PAGE_PREV ? UI_ACT_PAGE_PREV :
                                              UI_ACT_PAGE_NEXT);
      return;
    }

  if (id < m->folder->count && id / MENU_PER == m->page.page)
    {
      block_focus(m->tiles[m->page.focus % MENU_PER], false);
      m->page.focus = id;
      s->focus      = id;
      block_focus(m->tiles[id % MENU_PER], true);
      menu_open(s);
    }
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct screen_class_s g_scr_menu =
{
  "menu",
  menu_build,
  menu_key,
  menu_event,
  NULL,
  NULL,
  sizeof(struct menu_s)
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/
