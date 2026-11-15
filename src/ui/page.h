/****************************************************************************
 * pnut-os/src/ui/page.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_PAGE_H
#define __PNUT_OS_UI_PAGE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/compiler.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* A list's pages, at most */

#define PAGE_MAX_PAGES  16

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A grid of tiles in pages of cols x rows, the focus on one */

struct page_s
{
  uint16_t count;
  uint16_t focus;
  uint16_t page;
  uint16_t per;                   /* Tiles a page */
  uint8_t cols;
  uint8_t rows;
};

/* A list of rows of their own heights in pages, some focusable (not a
 * heading); first[p] is page p's first row, first[npages] the count
 */

struct pagelist_s
{
  uint16_t count;
  uint16_t focus;
  uint16_t page;
  uint16_t npages;
  uint16_t first[PAGE_MAX_PAGES + 1];
  FAR const bool *focusable;      /* NULL: all */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* A grid of count tiles, the first focused */

void page_grid(FAR struct page_s *p, uint16_t count, uint8_t cols,
               uint8_t rows);

/* Its pages, at least one */

uint16_t page_count(FAR const struct page_s *p);

/* Move the focus by a UI action (keymap.h): whether it moved */

bool page_move(FAR struct page_s *p, uint8_t action);

/* Lay rows of these heights out in pages of avail, the first focusable
 * focused.  -E2BIG for more than PAGE_MAX_PAGES pages.
 */

int pagelist_layout(FAR struct pagelist_s *l, FAR const uint16_t *heights,
                    FAR const bool *focusable, uint16_t count,
                    uint16_t avail);

bool pagelist_move(FAR struct pagelist_s *l, uint8_t action);

#endif /* __PNUT_OS_UI_PAGE_H */
