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

uint64_t redraw_next(FAR const struct redraw_s *r)
{
  return r->window || r->pending ? r->due : UINT64_MAX;
}

int redraw_take(FAR struct redraw_s *r, uint64_t now)
{
  if (!(r->window || r->pending) || now < r->due)
    {
      return REDRAW_NONE;
    }

  r->window  = false;
  r->pending = false;
  return r->partials >= REDRAW_FULL_AFTER && r->screen && !r->typing ?
         REDRAW_FULL : REDRAW_PARTIAL;
}

void redraw_done(FAR struct redraw_s *r, bool full)
{
  if (full)
    {
      r->partials = 0;
    }
  else if (r->partials < UINT8_MAX)
    {
      r->partials++;
    }

  r->screen = false;
}
