/****************************************************************************
 * pnut-os/src/ui/applist.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* An app's list as the system UI keeps it (RFC 0037): rows are pulled, a
 * page at a time, ahead of the focus; up to APPLIST_KEPT of them are kept
 * for paging.  The rule alone, without LVGL: the List template asks it.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "applist.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool applist_ids_ok(FAR const pnut_ui_row_t *rows, uint16_t n)
{
  uint16_t i;

  for (i = 0; i < n; i++)
    {
      if (rows[i].id == 0)
        {
          return false;
        }
    }

  return true;
}

/* The index of the row of id focus among the first n, 0 for none; -1 if
 * it is not there
 */

static int applist_find(FAR const pnut_ui_row_t *rows, uint16_t n,
                        uint32_t focus)
{
  uint16_t i;

  if (focus == 0)
    {
      return 0;
    }

  for (i = 0; i < n; i++)
    {
      if (rows[i].id == focus)
        {
          return i;
        }
    }

  return -1;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int applist_check(uint32_t count, FAR const pnut_ui_row_t *rows, uint16_t n,
                  uint32_t focus)
{
  uint16_t kept = count < APPLIST_KEPT ? count : APPLIST_KEPT;

  if (n > count || !applist_ids_ok(rows, n) ||
      applist_find(rows, n < kept ? n : kept, focus) < 0)
    {
      return -EINVAL;
    }

  return 0;
}

int applist_init(FAR struct applist_s *l, uint32_t count,
                 FAR const pnut_ui_row_t *rows, uint16_t n, uint32_t focus)
{
  int ret;

  memset(l, 0, sizeof(*l));
  ret = applist_check(count, rows, n, focus);
  if (ret < 0)
    {
      return ret;
    }

  l->count  = count;
  l->kept   = count < APPLIST_KEPT ? count : APPLIST_KEPT;
  l->loaded = n < l->kept ? n : l->kept;
  l->focus  = applist_find(rows, l->loaded, focus);

  if (l->kept > 0)
    {
      l->rows = calloc(l->kept, sizeof(*l->rows));
      if (l->rows == NULL)
        {
          memset(l, 0, sizeof(*l));
          return -ENOMEM;
        }

      memcpy(l->rows, rows, l->loaded * sizeof(*l->rows));
    }

  return 0;
}

void applist_free(FAR struct applist_s *l)
{
  free(l->rows);
  memset(l, 0, sizeof(*l));
}

FAR const pnut_ui_row_t *applist_focused(FAR const struct applist_s *l)
{
  return l->loaded > 0 ? &l->rows[l->focus] : NULL;
}

uint16_t applist_page(FAR const struct applist_s *l)
{
  return l->focus / APPLIST_PER_PAGE;
}

uint16_t applist_npages(FAR const struct applist_s *l)
{
  uint16_t n = (l->kept + APPLIST_PER_PAGE - 1) / APPLIST_PER_PAGE;

  return n > 0 ? n : 1;
}

bool applist_move(FAR struct applist_s *l, int dir)
{
  if (dir > 0 && l->focus + 1 < l->loaded)
    {
      l->focus++;
      return true;
    }

  if (dir < 0 && l->focus > 0)
    {
      l->focus--;
      return true;
    }

  return false;
}

bool applist_page_move(FAR struct applist_s *l, int dir)
{
  uint16_t page = applist_page(l);
  uint16_t first;

  if (dir > 0)
    {
      first = (page + 1) * APPLIST_PER_PAGE;
      if (first >= l->loaded)
        {
          return false;
        }
    }
  else
    {
      if (page == 0)
        {
          return false;
        }

      first = (page - 1) * APPLIST_PER_PAGE;
    }

  l->focus = first;
  return true;
}

bool applist_need(FAR struct applist_s *l, FAR uint32_t *first)
{
  if (l->asking || l->loaded >= l->kept ||
      l->focus + APPLIST_PER_PAGE < l->loaded)
    {
      return false;
    }

  l->asking = true;
  l->asked  = l->loaded;
  *first    = l->loaded;
  return true;
}

int applist_rows(FAR struct applist_s *l, uint32_t first,
                 FAR const pnut_ui_row_t *rows, uint16_t n)
{
  uint16_t room;

  if (!l->asking || first != l->asked || !applist_ids_ok(rows, n))
    {
      return -EINVAL;
    }

  /* None: the list ends there, whatever its count said */

  if (n == 0)
    {
      l->kept = l->loaded;
    }

  room = l->kept - l->loaded;
  if (n > room)
    {
      n = room;
    }

  memcpy(&l->rows[l->loaded], rows, n * sizeof(*rows));
  l->loaded += n;
  l->asking  = false;
  return 0;
}

int applist_update(FAR struct applist_s *l, FAR const pnut_ui_row_t *row)
{
  uint16_t i;

  for (i = 0; i < l->loaded; i++)
    {
      if (l->rows[i].id == row->id)
        {
          l->rows[i] = *row;
          return i;
        }
    }

  return -ENOENT;
}
