/****************************************************************************
 * pnut-os/src/ui/applist.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_APPLIST_H
#define __PNUT_OS_UI_APPLIST_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/compiler.h>

#include "pnut/ui.pb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Rows a page shows (Cat-List: 40 px each between the header and the
 * softkeys), and rows kept for a screen (RFC 0037)
 */

#define APPLIST_PER_PAGE  6
#define APPLIST_KEPT      200

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* An app's list as the system UI keeps it: count rows in all, the first
 * loaded of them in rows, the focus on one of those; a page of rows asked
 * for once the focus nears the end of the rows loaded
 */

struct applist_s
{
  uint32_t count;                 /* The list's, as the app gave it */
  uint16_t kept;                  /* Rows that may be loaded: at most
                                   * APPLIST_KEPT */
  uint16_t loaded;
  uint16_t focus;                 /* An index; meaningless with none */
  bool asking;                    /* A page asked for, not answered */
  uint16_t asked;                 /* Its first row */
  FAR pnut_ui_row_t *rows;        /* kept of them, allocated */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* A list of count rows, the first n of them given, the focus on the row
 * of id focus (0: the first).  -EINVAL for more rows than the list has,
 * a row id 0, or a focus not among them; -ENOMEM.
 */

int applist_init(FAR struct applist_s *l, uint32_t count,
                 FAR const pnut_ui_row_t *rows, uint16_t n, uint32_t focus);

/* Whether applist_init() would take them: 0 or -EINVAL; nothing kept */

int applist_check(uint32_t count, FAR const pnut_ui_row_t *rows, uint16_t n,
                  uint32_t focus);

/* The rows let go of; the list is empty after */

void applist_free(FAR struct applist_s *l);

/* The row focused, or NULL for an empty list */

FAR const pnut_ui_row_t *applist_focused(FAR const struct applist_s *l);

/* The focus's page, and the pages of the rows that may be loaded (at
 * least one)
 */

uint16_t applist_page(FAR const struct applist_s *l);
uint16_t applist_npages(FAR const struct applist_s *l);

/* Move the focus a row (dir 1 or -1), or to the next or previous page's
 * first row, within the rows loaded: whether it moved
 */

bool applist_move(FAR struct applist_s *l, int dir);
bool applist_page_move(FAR struct applist_s *l, int dir);

/* Whether to ask the app for a page now, and from which row: once the
 * focus is within a page of the end of the rows loaded, more may be
 * loaded, and none is being asked for
 */

bool applist_need(FAR struct applist_s *l, FAR uint32_t *first);

/* The app's answer, Rows from first: appended when they are the ones
 * asked for, cut at the rows that may be loaded; -EINVAL (dropped)
 * otherwise, or for a row id 0
 */

int applist_rows(FAR struct applist_s *l, uint32_t first,
                 FAR const pnut_ui_row_t *rows, uint16_t n);

/* The row of that id replaced: its index, or -ENOENT */

int applist_update(FAR struct applist_s *l, FAR const pnut_ui_row_t *row);

#endif /* __PNUT_OS_UI_APPLIST_H */
