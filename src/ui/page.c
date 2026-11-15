/****************************************************************************
 * pnut-os/src/ui/page.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Grids and lists in pages: on e-paper long lists page, they never scroll
 * (RFC 0019).  Moving flows across pages: down from a page's last row is
 * the next page's first, right from its last tile the next page's first.
 * Space and Shift+Space go to the next or the previous page's first item;
 * in a list, which has no row, left and right do too.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <string.h>

#include "keymap.h"
#include "page.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool pagelist_can(FAR const struct pagelist_s *l, uint16_t i)
{
  return l->focusable == NULL || l->focusable[i];
}

static uint16_t pagelist_page(FAR const struct pagelist_s *l, uint16_t i)
{
  uint16_t p;

  for (p = 0; p + 1 < l->npages && l->first[p + 1] <= i; p++);
  return p;
}

/* A page's first focusable row, or its first row */

static uint16_t pagelist_top(FAR const struct pagelist_s *l, uint16_t page)
{
  uint16_t i;

  for (i = l->first[page]; i < l->first[page + 1]; i++)
    {
      if (pagelist_can(l, i))
        {
          return i;
        }
    }

  return l->first[page];
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void page_grid(FAR struct page_s *p, uint16_t count, uint8_t cols,
               uint8_t rows)
{
  memset(p, 0, sizeof(*p));
  p->count = count;
  p->cols  = cols;
  p->rows  = rows;
  p->per   = cols * rows;
}

uint16_t page_count(FAR const struct page_s *p)
{
  return p->count == 0 ? 1 : (p->count + p->per - 1) / p->per;
}

bool page_move(FAR struct page_s *p, uint8_t action)
{
  uint16_t i   = p->focus % p->per;
  uint16_t row = i / p->cols;
  uint16_t col = i % p->cols;
  int32_t to;

  switch (action)
    {
      case UI_ACT_RIGHT:
        to = p->focus + 1;
        break;

      case UI_ACT_LEFT:
        to = (int32_t)p->focus - 1;
        break;

      case UI_ACT_DOWN:
        to = row + 1 < p->rows ? p->focus + p->cols :
             (p->page + 1) * p->per + col;
        break;

      case UI_ACT_UP:
        to = row > 0 ? (int32_t)p->focus - p->cols :
             p->page > 0 ? (p->page - 1) * p->per +
                           (p->rows - 1) * p->cols + col : -1;
        break;

      case UI_ACT_PAGE_NEXT:
        to = (p->page + 1) * p->per;
        break;

      case UI_ACT_PAGE_PREV:
        to = p->page > 0 ? (p->page - 1) * p->per : -1;
        break;

      default:
        return false;
    }

  if (to < 0 || to >= p->count)
    {
      return false;
    }

  p->focus = to;
  p->page  = to / p->per;
  return true;
}

int pagelist_layout(FAR struct pagelist_s *l, FAR const uint16_t *heights,
                    FAR const bool *focusable, uint16_t count,
                    uint16_t avail)
{
  uint32_t used = 0;
  uint16_t i;

  memset(l, 0, sizeof(*l));
  l->count     = count;
  l->focusable = focusable;
  l->npages    = 1;
  for (i = 0; i < count; i++)
    {
      if (used + heights[i] > avail && i > l->first[l->npages - 1])
        {
          if (l->npages == PAGE_MAX_PAGES)
            {
              return -E2BIG;
            }

          l->first[l->npages++] = i;
          used = 0;
        }

      used += heights[i];
    }

  l->first[l->npages] = count;
  l->focus = count > 0 ? pagelist_top(l, 0) : 0;
  return 0;
}

bool pagelist_move(FAR struct pagelist_s *l, uint8_t action)
{
  int32_t i;

  switch (action)
    {
      case UI_ACT_DOWN:
        for (i = l->focus + 1; i < l->count && !pagelist_can(l, i); i++);
        if (i >= l->count)
          {
            return false;
          }
        break;

      case UI_ACT_UP:
        for (i = (int32_t)l->focus - 1; i >= 0 && !pagelist_can(l, i); i--);
        if (i < 0)
          {
            return false;
          }
        break;

      case UI_ACT_RIGHT:
      case UI_ACT_PAGE_NEXT:
        if (l->page + 1 >= l->npages)
          {
            return false;
          }

        i = pagelist_top(l, l->page + 1);
        break;

      case UI_ACT_LEFT:
      case UI_ACT_PAGE_PREV:
        if (l->page == 0)
          {
            return false;
          }

        i = pagelist_top(l, l->page - 1);
        break;

      default:
        return false;
    }

  l->focus = i;
  l->page  = pagelist_page(l, i);
  return true;
}
