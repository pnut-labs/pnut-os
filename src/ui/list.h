/****************************************************************************
 * pnut-os/src/ui/list.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_LIST_H
#define __PNUT_OS_UI_LIST_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <lvgl/lvgl.h>

#include <pnut/compiler.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A screen's list: a column of rows between the header and the softkeys,
 * scrolled to keep the row focused in view.  A row with an id (blocks.h)
 * takes the focus; headings, rows that only tell and hidden rows do not.
 */

struct list_s
{
  FAR lv_obj_t *box;
  FAR lv_obj_t *focus;            /* The row focused, or NULL */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void list_init(FAR struct list_s *l, FAR lv_obj_t *scr);

/* Move the focus to the next row that takes it, down (1) or up (-1); from
 * none, the first.  Whether it moved.
 */

bool list_move(FAR struct list_s *l, int dir);

/* Focus a row (NULL: none); the row of an id, or NULL */

void list_focus(FAR struct list_s *l, FAR lv_obj_t *row);
FAR lv_obj_t *list_find(FAR struct list_s *l, uint16_t id);

/* After rows were hidden or shown: a focus that can no longer be kept
 * moves to the next row that takes it, else the one before
 */

void list_check(FAR struct list_s *l);

#endif /* __PNUT_OS_UI_LIST_H */
