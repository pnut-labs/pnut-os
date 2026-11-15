/****************************************************************************
 * pnut-os/src/ui/redraw.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_REDRAW_H
#define __PNUT_OS_UI_REDRAW_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/compiler.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Keys pressed within this long of the first are drawn once, at the final
 * state (RFC 0019: one redraw per decision on e-paper)
 */

#define REDRAW_GATHER_MS   500

/* Partial refreshes before a full one clears their ghosting */

#define REDRAW_FULL_AFTER  5

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum redraw_kind_e
{
  REDRAW_NONE = 0,                /* Nothing to draw yet */
  REDRAW_PARTIAL,                 /* What changed */
  REDRAW_FULL                     /* The whole screen, ghosting cleared */
};

/* When the screen is drawn.  The first key opens a window of
 * REDRAW_GATHER_MS; keys and changes within it are drawn once at its end.
 * A change no key caused (a topic, the clock) is drawn at once.  A full
 * refresh is due after REDRAW_FULL_AFTER partial ones, and taken at the
 * next change of screen, never while typing.  Times are in ms.
 */

struct redraw_s
{
  uint64_t due;                   /* When to draw, if window or pending */
  bool window;                    /* Keys being gathered */
  bool pending;                   /* A change to draw at once */
  bool screen;                    /* The next draw changes the screen */
  bool typing;                    /* A text field has the keys */
  uint8_t partials;               /* Partial refreshes since a full one */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void redraw_init(FAR struct redraw_s *r);

/* A key, at now: opens the window, unless one is open */

void redraw_input(FAR struct redraw_s *r, uint64_t now);

/* Something on screen changed, at now */

void redraw_changed(FAR struct redraw_s *r, uint64_t now);

/* The next draw shows another screen */

void redraw_screen(FAR struct redraw_s *r);

void redraw_typing(FAR struct redraw_s *r, bool typing);

/* When to draw next: UINT64_MAX for nothing to draw */

uint64_t redraw_next(FAR const struct redraw_s *r);

/* Draw now, if due: REDRAW_PARTIAL or REDRAW_FULL, and the window and the
 * pending change are taken; REDRAW_NONE if not due
 */

int redraw_take(FAR struct redraw_s *r, uint64_t now);

/* An update went out to the screen, full or not */

void redraw_done(FAR struct redraw_s *r, bool full);

#endif /* __PNUT_OS_UI_REDRAW_H */
