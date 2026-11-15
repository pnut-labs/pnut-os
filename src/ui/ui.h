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
#include "status.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Screens on the stack, at most */

#define UI_DEPTH  8

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

struct screen_s;

struct ui_s
{
  struct pnut_module_s module;    /* First: "ui" */
  struct ui_config_s config;
  struct display_s display;
  struct ui_status_s status;
  bool status_open;
  FAR struct screen_s *stack[UI_DEPTH];
  uint8_t depth;
  struct pnut_board_client_s board;
  FAR struct pnut_timer_s *retry; /* Asking Board again, once */
  bool board_open;
  bool shown;                     /* The screen is open */
  int fd;                         /* The framebuffer's, or -1 */
  FAR void *map;                  /* Its mapping, or NULL */
  size_t maplen;
};

/* A kind of screen: what it builds, how it takes keys and taps on its
 * blocks (true: handled; else Back and the right softkey pop it), what it
 * does when the device's state changes, what it wipes as it goes
 */

struct screen_class_s
{
  FAR const char *name;
  CODE void (*build)(FAR struct screen_s *s);
  CODE bool (*key)(FAR struct screen_s *s, uint8_t action, uint32_t ch);
  CODE void (*event)(FAR struct screen_s *s, uint16_t id);
  CODE void (*status)(FAR struct screen_s *s, uint8_t what);
  CODE void (*leave)(FAR struct screen_s *s);
  size_t size;                    /* Its state, zeroed */
};

/* A screen on the stack */

struct screen_s
{
  FAR const struct screen_class_s *cls;
  FAR struct ui_s *ui;
  FAR lv_obj_t *root;             /* LVGL's screen */
  FAR lv_obj_t *header;           /* Its header, if it has one */
  FAR const void *arg;            /* What it was opened with */
  uint16_t focus;                 /* The item focused (for tests) */
  FAR void *state;                /* The class's, size bytes */
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern const struct screen_class_s g_scr_home;
extern const struct screen_class_s g_scr_menu;
extern const struct screen_class_s g_scr_unavailable;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void ui_init(FAR struct ui_s *ui, FAR const struct ui_config_s *config);

/* Open a screen on top of the stack, with what it shows; take it off (not
 * Home)
 */

int ui_push(FAR struct ui_s *ui, FAR const struct screen_class_s *cls,
            FAR const void *arg);
void ui_pop(FAR struct ui_s *ui);
FAR struct screen_s *ui_top(FAR struct ui_s *ui);

/* A screen's header, kept as the device's state is */

FAR lv_obj_t *ui_header(FAR struct screen_s *s, FAR const char *icon,
                        FAR const char *title);

/* The time now, local */

void ui_local(FAR struct ui_s *ui, FAR struct tm *tm);

#endif /* __PNUT_OS_UI_UI_H */
