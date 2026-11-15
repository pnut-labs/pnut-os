/****************************************************************************
 * pnut-os/src/ui/input.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_INPUT_H
#define __PNUT_OS_UI_INPUT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <lvgl/lvgl.h>

#include <pnut/compiler.h>
#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "display.h"
#include "keymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Keys given to LVGL and not read yet: a press and a release each */

#define INPUT_QUEUE  16

/* The input devices Board's description names (RFC 0024), at most */

#define INPUT_DEVICES  4

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct input_config_s
{
  FAR struct display_s *display;  /* Told of every key */
};

struct input_key_s
{
  uint32_t key;                   /* LVGL's: UI_KEY(action), a character */
  bool pressed;
};

/* A device read by input_dev.c: a keyboard (the keys, the function keys)
 * or a touch screen
 */

struct input_dev_s
{
  int fd;
  bool touch;
};

/* Input (RFC 0027): a module of the system UI's program, the only reader
 * of the input devices.  Keys become the UI's actions (keymap.c) and reach
 * the object with LVGL's focus as LV_EVENT_KEY; held keys repeat; touches
 * reach LVGL as a pointer.  Every key opens the redraw's gathering window.
 */

struct input_s
{
  struct pnut_module_s module;    /* First: "input" */
  struct input_config_s config;
  FAR struct pnut_loop_s *loop;
  struct keymap_state_s state;
  struct keyrepeat_s repeat;
  uint32_t held;                  /* The repeating key's code */
  bool held_special;
  FAR struct pnut_timer_s *timer; /* The next repeat */
  struct input_key_s queue[INPUT_QUEUE];
  uint8_t head;
  uint8_t count;
  uint32_t last;                  /* The key LVGL read last */
  FAR lv_group_t *group;
  FAR lv_indev_t *keypad;
  FAR lv_indev_t *pointer;
  int16_t x;                      /* The touch, as last read */
  int16_t y;
  int16_t finger;                 /* The finger followed, or -1 */
  bool down;
  bool sending;                   /* Keys going to LVGL */
  char layout[33];                /* The keyboard's, from Board */
  struct input_dev_s devs[INPUT_DEVICES];
  uint8_t ndevs;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void input_init(FAR struct input_s *in, FAR const struct input_config_s *c);

/* A key pressed or released: a character, or with special set, a
 * keymap_special_e (keymap.h).  From a device, or a test.
 */

void input_key(FAR struct input_s *in, uint32_t code, bool special,
               bool press);

/* A touch at x, y, down or lifted */

void input_touch(FAR struct input_s *in, int16_t x, int16_t y, bool down);

/* A finger's point, of a sample of several: the first finger down is
 * followed until it lifts, the others are not.  A lift with no position
 * (valid false) is where the finger last was.
 */

void input_touch_point(FAR struct input_s *in, uint8_t id, bool down,
                       bool valid, int16_t x, int16_t y);

/* A text field has the keys, or no longer: letters type */

void input_typing(FAR struct input_s *in, bool typing);

/* Make LVGL's keypad and pointer, once the screen is open: Input's start
 * does, if it is by then
 */

int input_attach(FAR struct input_s *in);

/* The group whose focused object gets the keys */

FAR lv_group_t *input_group(FAR struct input_s *in);

/* The keyboard's layout, as Board's description names it ("" for none):
 * which digits its letter keys carry
 */

void input_set_layout(FAR struct input_s *in, FAR const char *layout);
FAR const char *input_layout(FAR struct input_s *in);

/* input_dev.c, the device's alone: open one of the devices Board's
 * description names (the system UI's program asks Board), a keyboard (the
 * keyboard, the function keys) or a touch screen, and read it on the loop
 * until Input stops
 */

int input_dev_open(FAR struct input_s *in, FAR const char *path,
                   bool touch);
void input_dev_close(FAR struct input_s *in);

#endif /* __PNUT_OS_UI_INPUT_H */
