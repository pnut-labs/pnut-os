/****************************************************************************
 * pnut-os/src/ui/redraw.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* When the system UI draws on an e-paper screen (RFC 0019): every action
 * costs one redraw, so keys are gathered into one, and ghosting is cleared
 * by a full refresh now and then.  The rule alone, without LVGL: display.c
 * asks it.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <string.h>

#include "redraw.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void redraw_init(FAR struct redraw_s *r)
{
  memset(r, 0, sizeof(*r));
}

void redraw_input(FAR struct redraw_s *r, uint64_t now)
{
  if (!r->window)
    {
      r->window = true;
      r->due    = now + REDRAW_GATHER_MS;
    }
}

void redraw_changed(FAR struct redraw_s *r, uint64_t now)
{
  if (!r->window && !r->pending)
    {
      r->pending = true;
      r->due     = now;
    }
}

void redraw_screen(FAR struct redraw_s *r)
{
  r->screen = true;
}

void redraw_typing(FAR struct redraw_s *r, bool typing)
{
  r->typing = typing;
}

void redraw_app(FAR struct redraw_s *r, uint64_t now, int how,
                uint32_t gather, uint32_t spacing)
{
  uint64_t due;

  if (r->window || r->pending)
    {
      return;                     /* Joins the redraw due */
    }

  switch (how)
    {
      case REDRAW_APP_NOW:
        r->pending = true;
        r->due     = now;
        break;

      case REDRAW_APP_QUIET:
        r->quiet = true;
        break;

      default:
        if (!r->app)
          {
            due = now + gather;
            if (r->app_drawn && r->app_last + spacing > due)
              {
                due = r->app_last + spacing;
              }

            r->app = true;
            r->due = due;
          }

        break;
    }
}

void redraw_switch(FAR struct redraw_s *r)
{
  r->switched = true;
}

void redraw_unswitch(FAR struct redraw_s *r)
{
  r->switched = false;
}

uint64_t redraw_next(FAR const struct redraw_s *r)
{
  return r->window || r->pending || r->app ? r->due : UINT64_MAX;
}

int redraw_take(FAR struct redraw_s *r, uint64_t now)
{
  if (!(r->window || r->pending || r->app) || now < r->due)
    {
      return REDRAW_NONE;
    }

  if (r->app)
    {
      r->app_last  = now;
      r->app_drawn = true;
    }

  r->window  = false;
  r->pending = false;
  r->app     = false;
  r->quiet   = false;

  /* The switch's even in a text field, once, before anything is typed
   * (the owner, 2026-11-22); the ghosting's never while typing
   */

  return r->screen &&
         (r->switched ||
          (!r->typing && r->partials >= REDRAW_FULL_AFTER)) ?
         REDRAW_FULL : REDRAW_PARTIAL;
}

void redraw_done(FAR struct redraw_s *r, bool full)
{
  if (full)
    {
      r->partials = 0;
      r->switched = false;
    }
  else if (r->partials < UINT8_MAX)
    {
      r->partials++;
    }

  r->screen = false;
}
