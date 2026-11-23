/****************************************************************************
 * pnut-os/src/ui/display.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The system UI's screen: LVGL's display over a 1-bit frame (RFC 0019).
 *
 * LVGL would draw on its own timer whenever something changes; here its
 * refresh timer is kept paused, and the screen is drawn when the redraw
 * rule says (redraw.h): keys gathered for half a second, other changes at
 * once.  Areas are widened to whole bytes, as the frame packs eight pixels
 * in one.  Only the rows and bytes whose pixels differ from the frame's go
 * to the screen, so that drawing the same thing again flashes nothing.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "display.h"
#include "glue.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* LVGL's 1-bit buffers start with the two colours' palette */

#define DISPLAY_PALETTE  8

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void display_schedule(FAR struct display_s *d);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* An area to draw: whole bytes of the frame */

static void display_invalidate(FAR lv_event_t *e)
{
  FAR struct display_s *d = lv_event_get_user_data(e);
  FAR lv_area_t *area = lv_event_get_param(e);

  area->x1 &= ~7;
  area->x2 |= 7;
  if (area->x2 >= d->config.width)
    {
      area->x2 = d->config.width - 1;
    }
}

/* Something changed: LVGL resumed its refresh timer, which waits for the
 * rule instead.  What changes while drawing is drawn with it.
 */

static void display_request(FAR lv_event_t *e)
{
  FAR struct display_s *d = lv_event_get_user_data(e);

  lv_timer_pause(lv_display_get_refr_timer(d->disp));
  if (!d->drawing)
    {
      if (d->app)
        {
          redraw_app(&d->redraw, glue_now(), d->how, d->gather,
                     d->spacing);
        }
      else
        {
          redraw_changed(&d->redraw, glue_now());
        }

      display_schedule(d);
    }
}

/* Send what differs to the screen.  The frame holds the new pixels
 * already, so an update the screen refused could not be found again: the
 * next draw sends the whole frame instead.
 */

static void display_send(FAR struct display_s *d)
{
  int ret;

  if (d->stale)
    {
      d->x1 = 0;
      d->y1 = 0;
      d->x2 = d->config.width - 1;
      d->y2 = d->config.height - 1;
    }

  ret = d->config.ops->update(d->config.arg, d->x1, d->y1,
                              d->x2 - d->x1 + 1, d->y2 - d->y1 + 1,
                              d->full);
  d->dirty = false;
  d->tried = true;
  d->stale = ret < 0;
  d->sent  = ret >= 0;
}

static void display_flush(FAR lv_display_t *disp, FAR const lv_area_t *area,
                          FAR uint8_t *px)
{
  FAR struct display_s *d = lv_display_get_user_data(disp);
  int32_t w = lv_area_get_width(area);
  uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_I1);
  size_t bytes = (w + 7) / 8;
  FAR uint8_t *row;
  FAR uint8_t *src;
  size_t first;
  size_t last;
  int32_t y;

  px += DISPLAY_PALETTE;
  for (y = area->y1; y <= area->y2; y++)
    {
      row = d->config.frame + y * d->config.stride + area->x1 / 8;
      src = px + (y - area->y1) * stride;
      if (memcmp(row, src, bytes) == 0)
        {
          continue;
        }

      for (first = 0; row[first] == src[first]; first++);
      for (last = bytes - 1; row[last] == src[last]; last--);
      memcpy(row + first, src + first, last - first + 1);

      /* A draw's areas come in no order of place: the update spans them */

      first = area->x1 + first * 8;
      last  = area->x1 + last * 8 + 7;
      if (!d->dirty)
        {
          d->x1    = first;
          d->x2    = last;
          d->y1    = y;
          d->y2    = y;
          d->dirty = true;
        }

      d->x1 = MIN(d->x1, (int16_t)first);
      d->x2 = MAX(d->x2, (int16_t)last);
      d->y1 = MIN(d->y1, (int16_t)y);
      d->y2 = MAX(d->y2, (int16_t)y);
    }

  if (lv_display_flush_is_last(disp) && (d->dirty || d->stale))
    {
      display_send(d);
    }

  lv_display_flush_ready(disp);
}

static void display_due(FAR struct pnut_loop_s *loop,
                        FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct display_s *d = arg;
  bool stale;
  int kind;

  d->timer = NULL;
  kind = redraw_take(&d->redraw, glue_now());
  if (kind != REDRAW_NONE)
    {
      stale      = d->stale;
      d->full    = kind == REDRAW_FULL;
      d->sent    = false;
      d->tried   = false;
      d->dirty   = false;
      d->drawing = true;
      lv_refr_now(d->disp);
      d->drawing = false;

      /* A refused update is tried again, whole, though nothing changed:
       * once a draw
       */

      if (stale && d->stale && !d->tried)
        {
          display_send(d);
        }

      if (d->sent)
        {
          redraw_done(&d->redraw, d->full);
        }

      /* Still refused: again in half a second, gathering what comes */

      if (d->stale)
        {
          redraw_input(&d->redraw, glue_now());
        }
    }

  display_schedule(d);
}

static void display_schedule(FAR struct display_s *d)
{
  uint64_t next = redraw_next(&d->redraw);
  uint64_t now;

  if (d->timer != NULL)
    {
      pnut_timer_cancel(d->loop, d->timer);
      d->timer = NULL;
    }

  if (next != UINT64_MAX)
    {
      now = glue_now();
      pnut_timer_start(d->loop, next > now ? (uint32_t)(next - now) : 0, 0,
                       display_due, d, &d->timer);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int display_open(FAR struct display_s *d, FAR struct pnut_loop_s *loop,
                 FAR const struct display_config_s *config)
{
  uint32_t size;

  if (config->width % 8 != 0 || config->ops == NULL)
    {
      return -EINVAL;
    }

  memset(d, 0, sizeof(*d));
  d->loop   = loop;
  d->config = *config;
  redraw_init(&d->redraw);

  /* The whole screen in one buffer: e-paper wants one refresh a draw */

  size = DISPLAY_PALETTE +
         lv_draw_buf_width_to_stride(config->width, LV_COLOR_FORMAT_I1) *
         config->height;
  d->buf = malloc(size);
  if (d->buf == NULL)
    {
      return -ENOMEM;
    }

  d->disp = lv_display_create(config->width, config->height);
  if (d->disp == NULL)
    {
      free(d->buf);
      return -ENOMEM;
    }

  lv_display_set_user_data(d->disp, d);
  lv_display_set_color_format(d->disp, LV_COLOR_FORMAT_I1);
  lv_display_set_buffers(d->disp, d->buf, NULL, size,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(d->disp, display_flush);
  lv_display_add_event_cb(d->disp, display_invalidate,
                          LV_EVENT_INVALIDATE_AREA, d);
  lv_display_add_event_cb(d->disp, display_request,
                          LV_EVENT_REFR_REQUEST, d);
  lv_timer_pause(lv_display_get_refr_timer(d->disp));
  return 0;
}

void display_close(FAR struct display_s *d)
{
  if (d->timer != NULL)
    {
      pnut_timer_cancel(d->loop, d->timer);
      d->timer = NULL;
    }

  if (d->disp != NULL)
    {
      lv_display_delete(d->disp);
      d->disp = NULL;
    }

  free(d->buf);
  d->buf = NULL;
}

void display_input(FAR struct display_s *d)
{
  redraw_input(&d->redraw, glue_now());
  display_schedule(d);
}

void display_screen(FAR struct display_s *d)
{
  redraw_screen(&d->redraw);
}

void display_typing(FAR struct display_s *d, bool typing)
{
  redraw_typing(&d->redraw, typing);
}

void display_app(FAR struct display_s *d, int how, uint32_t gather,
                 uint32_t spacing)
{
  d->app     = true;
  d->how     = how;
  d->gather  = gather;
  d->spacing = spacing;
}

void display_app_end(FAR struct display_s *d)
{
  if (d->disp != NULL)
    {
      lv_obj_update_layout(lv_display_get_screen_active(d->disp));
    }

  d->app = false;
}

void display_switch(FAR struct display_s *d)
{
  redraw_switch(&d->redraw);
}

void display_unswitch(FAR struct display_s *d)
{
  redraw_unswitch(&d->redraw);
}

void display_draw_now(FAR struct display_s *d)
{
  if (redraw_next(&d->redraw) == UINT64_MAX)
    {
      return;
    }

  if (d->timer != NULL)
    {
      pnut_timer_cancel(d->loop, d->timer);
      d->timer = NULL;
    }

  d->redraw.due = 0;
  display_due(d->loop, NULL, d);
}
