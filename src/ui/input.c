/****************************************************************************
 * pnut-os/src/ui/input.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Input (RFC 0027): the keys and touches of the devices Board's
 * description names, as the system UI's actions.
 *
 * A key goes through the key map (keymap.c) and reaches the object with
 * the keypad group's focus as LV_EVENT_KEY: UI_KEY(action), or the
 * character typed.  LVGL's keypad is fed a press and a release at once, so
 * that LVGL's own long press and repeat never happen: repeat is Input's,
 * from a timer of the loop.  Touches reach LVGL as a pointer, read when
 * they come (event mode: no polling).  Each key opens the redraw's
 * gathering window first, so what it changes is drawn with the keys that
 * follow it within half a second.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "glue.h"
#include "input.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void input_arm(FAR struct input_s *in);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void input_read_keypad(FAR lv_indev_t *indev,
                              FAR lv_indev_data_t *data)
{
  FAR struct input_s *in = lv_indev_get_user_data(indev);
  FAR struct input_key_s *k;

  if (in->count == 0)
    {
      data->key   = in->last;
      data->state = LV_INDEV_STATE_RELEASED;
      return;
    }

  k = &in->queue[in->head];
  in->head = (in->head + 1) % INPUT_QUEUE;
  in->count--;
  in->last    = k->key;
  data->key   = k->key;
  data->state = k->pressed ? LV_INDEV_STATE_PRESSED :
                             LV_INDEV_STATE_RELEASED;
}

static void input_read_pointer(FAR lv_indev_t *indev,
                               FAR lv_indev_data_t *data)
{
  FAR struct input_s *in = lv_indev_get_user_data(indev);

  data->point.x = in->x;
  data->point.y = in->y;
  data->state   = in->down ? LV_INDEV_STATE_PRESSED :
                             LV_INDEV_STATE_RELEASED;
}

/* The keys queued, given to LVGL.  LVGL may not read at all (the keypad
 * disabled, a screen loading with an animation): what it does not take is
 * dropped.
 */

static void input_drain(FAR struct input_s *in)
{
  uint8_t count;

  while (in->count > 0)
    {
      count = in->count;
      lv_indev_read(in->keypad);
      if (in->count == count)
        {
          in->count = 0;
        }
    }
}

/* A key for LVGL, pressed and released, and LVGL told of it.  A key sent
 * while LVGL handles input (from a key's or a tap's handler) is queued,
 * and delivered once that is over: LVGL's input processing is not
 * re-entrant.
 */

static void input_send(FAR struct input_s *in, uint8_t action, uint32_t ch)
{
  uint32_t key = action == UI_ACT_CHAR ? ch : UI_KEY(action);
  int i;

  if (in->keypad == NULL || in->count + 2 > INPUT_QUEUE)
    {
      return;
    }

  for (i = 0; i < 2; i++)
    {
      in->queue[(in->head + in->count) % INPUT_QUEUE].key     = key;
      in->queue[(in->head + in->count) % INPUT_QUEUE].pressed = i == 0;
      in->count++;
    }

  if (!in->sending)
    {
      in->sending = true;
      display_input(in->config.display);
      input_drain(in);
      in->sending = false;
    }
}

static void input_stop_repeat(FAR struct input_s *in)
{
  keyrepeat_release(&in->repeat);
  if (in->timer != NULL)
    {
      pnut_timer_cancel(in->loop, in->timer);
      in->timer = NULL;
    }
}

static void input_repeat(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct input_s *in = arg;

  in->timer = NULL;
  if (keyrepeat_fire(&in->repeat, glue_now()))
    {
      input_send(in, in->repeat.action, in->repeat.ch);
    }

  input_arm(in);
}

/* The held key's next repeat */

static void input_arm(FAR struct input_s *in)
{
  uint64_t now = glue_now();

  if (in->timer != NULL)
    {
      pnut_timer_cancel(in->loop, in->timer);
      in->timer = NULL;
    }

  if (in->repeat.held)
    {
      pnut_timer_start(in->loop, in->repeat.due > now ?
                       (uint32_t)(in->repeat.due - now) : 0, 0,
                       input_repeat, in, &in->timer);
    }
}

/* LVGL's input devices and group gone, those made */

static void input_detach(FAR struct input_s *in)
{
  if (in->pointer != NULL)
    {
      lv_indev_delete(in->pointer);
      in->pointer = NULL;
    }

  if (in->keypad != NULL)
    {
      lv_indev_delete(in->keypad);
      in->keypad = NULL;
    }

  if (in->group != NULL)
    {
      lv_group_delete(in->group);
      in->group = NULL;
    }

  in->count = 0;
}

static int input_start(FAR struct pnut_module_s *module,
                       FAR struct pnut_loop_s *loop)
{
  FAR struct input_s *in = (FAR struct input_s *)module;
  int ret = 0;

  /* LVGL's input devices belong to a display: made now if the screen is
   * open, or by input_attach() once it is
   */

  in->loop = loop;
  if (lv_display_get_default() != NULL)
    {
      ret = input_attach(in);
    }

  pnut_module_ready(module);
  return ret;
}

static void input_stop(FAR struct pnut_module_s *module,
                       FAR struct pnut_loop_s *loop)
{
  FAR struct input_s *in = (FAR struct input_s *)module;

  input_stop_repeat(in);
  input_dev_close(in);
  input_detach(in);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void input_init(FAR struct input_s *in, FAR const struct input_config_s *c)
{
  memset(in, 0, sizeof(*in));
  in->config       = *c;
  in->finger       = -1;
  in->module.name  = "input";
  in->module.start = input_start;
  in->module.stop  = input_stop;
}

void input_key(FAR struct input_s *in, uint32_t code, bool special,
               bool press)
{
  struct keymap_result_s r;

  /* Before LVGL's input devices exist (the screen not open yet) keys mean
   * nothing
   */

  if (in->keypad == NULL)
    {
      return;
    }

  keymap_key(&in->state, code, special, press, &r);
  if (!press)
    {
      if (in->repeat.held && code == in->held &&
          special == in->held_special)
        {
          input_stop_repeat(in);
        }

      return;
    }

  /* A character LVGL takes for one of its keys (Tab, Enter, Esc...) is
   * never typed: the keyboard gives those as special keys
   */

  if (r.action == UI_ACT_NONE ||
      (r.action == UI_ACT_CHAR && (r.ch < 0x20 || r.ch == 0x7f)))
    {
      return;
    }

  /* Another key ends a repeat; so does one a screen sends from a key's or
   * a tap's handler, as a real key would
   */

  keyrepeat_press(&in->repeat, glue_now(), &r);
  in->held         = code;
  in->held_special = special;
  input_arm(in);
  input_send(in, r.action, r.ch);
}

void input_touch(FAR struct input_s *in, int16_t x, int16_t y, bool down)
{
  if (in->pointer == NULL || in->sending)
    {
      return;
    }

  in->sending = true;
  display_input(in->config.display);
  in->x    = x;
  in->y    = y;
  in->down = down;
  lv_indev_read(in->pointer);

  /* Keys a tap's handler sent */

  input_drain(in);
  in->sending = false;
}

void input_touch_point(FAR struct input_s *in, uint8_t id, bool down,
                       bool valid, int16_t x, int16_t y)
{
  if (!valid)
    {
      x = in->x;
      y = in->y;
    }

  if (down)
    {
      if (in->finger < 0)
        {
          in->finger = id;
        }

      if (in->finger == id && valid)
        {
          input_touch(in, x, y, true);
        }
    }
  else if (in->finger == id)
    {
      in->finger = -1;
      input_touch(in, x, y, false);
    }
}

void input_typing(FAR struct input_s *in, bool typing)
{
  if (in->state.typing != typing)
    {
      input_stop_repeat(in);
      in->state.typing = typing;
      if (in->config.display != NULL)
        {
          display_typing(in->config.display, typing);
        }
    }
}

int input_attach(FAR struct input_s *in)
{
  if (in->keypad != NULL)
    {
      return 0;
    }

  in->group   = lv_group_create();
  in->keypad  = lv_indev_create();
  in->pointer = lv_indev_create();
  if (in->group == NULL || in->keypad == NULL || in->pointer == NULL)
    {
      input_detach(in);
      return -ENOMEM;
    }

  lv_indev_set_type(in->keypad, LV_INDEV_TYPE_KEYPAD);
  lv_indev_set_user_data(in->keypad, in);
  lv_indev_set_read_cb(in->keypad, input_read_keypad);
  lv_indev_set_mode(in->keypad, LV_INDEV_MODE_EVENT);
  lv_indev_set_group(in->keypad, in->group);

  lv_indev_set_type(in->pointer, LV_INDEV_TYPE_POINTER);
  lv_indev_set_user_data(in->pointer, in);
  lv_indev_set_read_cb(in->pointer, input_read_pointer);
  lv_indev_set_mode(in->pointer, LV_INDEV_MODE_EVENT);
  return 0;
}

FAR lv_group_t *input_group(FAR struct input_s *in)
{
  return in->group;
}

void input_set_layout(FAR struct input_s *in, FAR const char *layout)
{
  strlcpy(in->layout, layout != NULL ? layout : "", sizeof(in->layout));
}

FAR const char *input_layout(FAR struct input_s *in)
{
  return in->layout;
}

void input_dev_close(FAR struct input_s *in)
{
  while (in->ndevs > 0)
    {
      in->ndevs--;
      pnut_loop_unwatch(in->loop, in->devs[in->ndevs].fd);
      close(in->devs[in->ndevs].fd);
    }
}
