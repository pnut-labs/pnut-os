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

/* How an app's change nobody's key caused is drawn (RFC 0037, E-paper) */

enum redraw_app_e
{
  REDRAW_APP_GATHER = 0,          /* Gathered, the app's screen spaced */
  REDRAW_APP_NOW,                 /* At once */
  REDRAW_APP_QUIET                /* With the next redraw anything causes */
};

/* When the screen is drawn.  The first key opens a window of
 * REDRAW_GATHER_MS; keys and changes within it are drawn once at its end.
 * A change no key caused (a topic, the clock) is drawn at once; an app's
 * is gathered, spaced, drawn at once or quietly, as it asks.  A full
 * refresh is due after REDRAW_FULL_AFTER partial ones, and on switching
 * apps and going Home; it is taken at the next change of screen (the
 * app's screen, Home).  The switch's is taken even when a text field has
 * the keys on that screen, once; the one after partial ones never while
 * typing.  Times are in ms.
 */

struct redraw_s
{
  uint64_t due;                   /* When to draw, if window or pending */
  bool window;                    /* Keys being gathered */
  bool pending;                   /* A change to draw at once */
  bool screen;                    /* The next draw changes the screen */
  bool typing;                    /* A text field has the keys */
  bool app;                       /* An app's change being gathered */
  bool quiet;                     /* An app's change waiting quietly */
  bool switched;                  /* Apps switched, or Home: a full one
                                   * at the screen's change */
  bool app_drawn;                 /* app_last holds a draw */
  uint64_t app_last;              /* When the app's gathered change was */
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

/* An app's change at now, nobody's key's: in a key's window, or with a
 * change due at once, it joins that redraw; otherwise gathered for gather
 * ms and no sooner than spacing ms after the last gathered one, at once
 * (now), or with the next redraw (quiet)
 */

void redraw_app(FAR struct redraw_s *r, uint64_t now, int how,
                uint32_t gather, uint32_t spacing);

/* Apps switched, or Home: the redraw that changes the screen is a full
 * one; taken back when what was switched to goes before it is drawn
 */

void redraw_switch(FAR struct redraw_s *r);
void redraw_unswitch(FAR struct redraw_s *r);

/* When to draw next: UINT64_MAX for nothing to draw */

uint64_t redraw_next(FAR const struct redraw_s *r);

/* Draw now, if due: REDRAW_PARTIAL or REDRAW_FULL, and the window and the
 * pending change are taken; REDRAW_NONE if not due
 */

int redraw_take(FAR struct redraw_s *r, uint64_t now);

/* An update went out to the screen, full or not */

void redraw_done(FAR struct redraw_s *r, bool full);

#endif /* __PNUT_OS_UI_REDRAW_H */
