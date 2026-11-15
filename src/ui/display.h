/****************************************************************************
 * pnut-os/src/ui/display.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_DISPLAY_H
#define __PNUT_OS_UI_DISPLAY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <lvgl/lvgl.h>

#include <pnut/compiler.h>
#include <pnut/loop.h>
#include <pnut/timer.h>

#include "redraw.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The screen: send the frame's area to the glass, a full refresh or a
 * partial one.  A negated errno value on failure.
 */

struct display_ops_s
{
  CODE int (*update)(FAR void *arg, int16_t x, int16_t y, int16_t w,
                     int16_t h, bool full);
};

/* A 1-bit frame, a set bit white, the leftmost pixel the byte's top bit:
 * the e-paper's (RFC 0024: Board's screen, depth 1)
 */

struct display_config_s
{
  FAR uint8_t *frame;
  uint32_t stride;                /* Bytes from a row to the next */
  uint16_t width;                 /* A multiple of 8 */
  uint16_t height;
  FAR const struct display_ops_s *ops;
  FAR void *arg;
};

/* LVGL's display over the frame.  LVGL draws what changed into the frame
 * when the redraw rule says (redraw.h); the screen is updated with the
 * area whose pixels differ, and not at all when none do.
 */

struct display_s
{
  FAR struct pnut_loop_s *loop;
  struct display_config_s config;
  FAR lv_display_t *disp;
  FAR uint8_t *buf;               /* LVGL's: a palette, then the pixels */
  struct redraw_s redraw;
  FAR struct pnut_timer_s *timer; /* The next draw, once */
  int16_t x1;                     /* What differs in this draw, if dirty */
  int16_t y1;
  int16_t x2;
  int16_t y2;
  bool dirty;
  bool stale;                     /* The screen refused an update: the
                                   * next one is the whole frame */
  bool full;                      /* This draw's kind */
  bool sent;                      /* An update went out in this draw */
  bool tried;                     /* One was tried in this draw */
  bool drawing;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Make LVGL's display over the frame, after glue_start() */

int display_open(FAR struct display_s *d, FAR struct pnut_loop_s *loop,
                 FAR const struct display_config_s *config);
void display_close(FAR struct display_s *d);

/* A key arrived: changes are gathered for REDRAW_GATHER_MS */

void display_input(FAR struct display_s *d);

/* The next draw shows another screen */

void display_screen(FAR struct display_s *d);

/* A text field has the keys, or no longer */

void display_typing(FAR struct display_s *d, bool typing);

#endif /* __PNUT_OS_UI_DISPLAY_H */
