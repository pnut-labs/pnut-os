/****************************************************************************
 * pnut-os/src/ui/ui.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_UI_H
#define __PNUT_OS_UI_UI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include <lvgl/lvgl.h>

#include <pnut/compiler.h>
#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "pnut/board.pnut.h"

#include "display.h"
#include "input.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct ui_config_s
{
  /* Board's service, which names the screen and the input devices
   * (RFC 0024); or NULL, and screen gives the frame (tests)
   */

  FAR const char *board;
  FAR const struct display_config_s *screen;

  FAR struct input_s *input;      /* Input, its own module */

  /* The device's: open the screen's framebuffer (fb_open), an input
   * device (input_dev_open).  NULL where there are none (tests).
   */

  CODE int (*open_screen)(FAR const char *path,
                          FAR struct display_config_s *config,
                          FAR size_t *len);
  CODE int (*open_input)(FAR struct input_s *in, FAR const char *path,
                         bool touch);

  /* The wall clock; NULL for time() (tests give a fixed one) */

  CODE time_t (*now)(void);
};

/* The system UI (RFC 0019): its screens, on LVGL, on the program's loop.
 * It asks Board for the screen and the input devices, opens the screen
 * and hands the input devices to Input.
 */

struct ui_s
{
  struct pnut_module_s module;    /* First: "ui" */
  struct ui_config_s config;
  struct display_s display;
  struct pnut_board_client_s board;
  FAR struct pnut_timer_s *retry; /* Asking Board again, once */
  bool board_open;
  bool shown;                     /* The screen is open */
  int fd;                         /* The framebuffer's, or -1 */
  FAR void *map;                  /* Its mapping, or NULL */
  size_t maplen;
  FAR lv_obj_t *check;            /* The developer's check screen's line */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void ui_init(FAR struct ui_s *ui, FAR const struct ui_config_s *config);

#endif /* __PNUT_OS_UI_UI_H */
