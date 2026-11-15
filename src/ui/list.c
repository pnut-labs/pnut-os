/****************************************************************************
 * pnut-os/src/ui/list.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* A screen's list of rows (Proto-Settings, Set-WiFi): W and S move the
 * focus over the rows that take it, and the list scrolls to keep it in
 * view.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "blocks.h"
#include "list.h"
#include "theme.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool list_takes(FAR lv_obj_t *row)
{
  return block_id(row) != 0 && !lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN);
}

/* The next row from index i that takes the focus, going dir; or NULL */

static FAR lv_obj_t *list_next(FAR struct list_s *l, int32_t i, int dir)
{
  int32_t n = (int32_t)lv_obj_get_child_count(l->box);
  FAR lv_obj_t *row;

  for (; i >= 0 && i < n; i += dir)
    {
      row = lv_obj_get_child(l->box, i);
      if (list_takes(row))
        {
          return row;
        }
    }

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void list_init(FAR struct list_s *l, FAR lv_obj_t *scr)
{
  l->focus = NULL;
  l->box   = lv_obj_create(scr);
  theme_box(l->box);
  lv_obj_set_pos(l->box, 0, BLOCK_HEADER_H);
  lv_obj_set_size(l->box, lv_pct(100),
                  320 - BLOCK_HEADER_H - BLOCK_FOOTER_H);
  lv_obj_set_flex_flow(l->box, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(l->box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(l->box, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(l->box, LV_SCROLLBAR_MODE_OFF);
}

bool list_move(FAR struct list_s *l, int dir)
{
  FAR lv_obj_t *row;

  if (l->focus == NULL)
    {
      row = list_next(l, dir > 0 ? 0 :
                      (int32_t)lv_obj_get_child_count(l->box) - 1, dir);
    }
  else
    {
      row = list_next(l, lv_obj_get_index(l->focus) + dir, dir);
    }

  if (row == NULL)
    {
      return false;
    }

  list_focus(l, row);
  return true;
}

void list_focus(FAR struct list_s *l, FAR lv_obj_t *row)
{
  if (l->focus == row)
    {
      return;
    }

  if (l->focus != NULL)
    {
      block_focus(l->focus, false);
    }

  l->focus = row;
  if (row != NULL)
    {
      block_focus(row, true);
      lv_obj_update_layout(l->box);
      lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    }
}

FAR lv_obj_t *list_find(FAR struct list_s *l, uint16_t id)
{
  uint32_t n = lv_obj_get_child_count(l->box);
  FAR lv_obj_t *row;
  uint32_t i;

  for (i = 0; i < n; i++)
    {
      row = lv_obj_get_child(l->box, i);
      if (block_id(row) == id)
        {
          return row;
        }
    }

  return NULL;
}

void list_check(FAR struct list_s *l)
{
  FAR lv_obj_t *row;
  int32_t i;

  if (l->focus == NULL || list_takes(l->focus))
    {
      return;
    }

  i   = lv_obj_get_index(l->focus);
  row = list_next(l, i + 1, 1);
  if (row == NULL)
    {
      row = list_next(l, i - 1, -1);
    }

  block_focus(l->focus, false);
  l->focus = NULL;
  list_focus(l, row);
}
