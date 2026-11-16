/****************************************************************************
 * pnut-os/src/ui/ui.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The system UI's module (RFC 0019).  It starts LVGL on the loop, asks
 * Board for the screen and the input devices, opens the screen and hands
 * the input devices to Input (RFC 0027), which alone reads them.  It is
 * ready as it starts: it serves no one, and the screen comes when Board
 * answers (a program never ready would keep a firmware update on trial
 * from being confirmed, RFC 0013).
 *
 * It keeps the stack of screens, Home at its foot: each a screen of
 * LVGL's, built by its class (scr_*.c), the only object with the keys'
 * focus while on top.  Keys reach the top screen as the UI's actions,
 * taps on its blocks as their ids; Back pops it unless it says otherwise.
 * The header of the screen on top follows the device's state (status.c).
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <pnut/log.h>
#include <pnut/timer.h>

#include "glue.h"
#include "keymap.h"
#include "blocks.h"
#include "text.h"
#include "theme.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Asking Board again after a call that could not be made */

#define UI_RETRY_MS  1000
#define UI_WIFI      "wifi"
#define UI_TELEPHONY "telephony"

/* The options' frame, over the screen above its softkeys; their rows' ids,
 * and the id of a tap outside them
 */

#define UI_OPTIONS_X        6
#define UI_OPTIONS_W        (240 - 6 - 10)
#define UI_OPTIONS_HEAD     20
#define UI_OPTIONS_ROW      30
#define UI_OPTIONS_FOOT     40
#define UI_OPTIONS_ID       0xfe00
#define UI_OPTIONS_OUTSIDE  (UI_OPTIONS_ID + UI_OPTIONS)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* LVGL places objects lazily, at the next draw, which may wait for the
 * keys' half second: what a handler built is placed at once, so that a tap
 * meanwhile finds it
 */

void ui_place(FAR struct ui_s *ui)
{
  FAR struct screen_s *s = ui_top(ui);

  if (s != NULL)
    {
      lv_obj_update_layout(s->root);
    }
}

static void ui_header_update(FAR struct ui_s *ui, FAR struct screen_s *s)
{
  struct tm tm;

  if (s->header != NULL)
    {
      ui_local(ui, &tm);
      block_header_status(s->header, &ui->status, &tm);
    }
}

/* The device's state changed: the top screen's header, and the screen */

/* The SIM asks for its PIN or PUK: the SIM PIN screen over any screen,
 * unless skipped since it began to ask
 */

static void ui_pin_check(FAR struct ui_s *ui)
{
  FAR const struct ui_status_s *st = &ui->status;
  int i;

  if (!st->has_tel || st->tel.modem != PNUT_TELEPHONY_INFO_MODEM_ON ||
      (st->tel.sim != PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN &&
       st->tel.sim != PNUT_TELEPHONY_INFO_SIM_NEEDS_PUK))
    {
      ui->pin_skipped = false;
      return;
    }

  if (ui->pin_skipped || ui->depth == 0)
    {
      return;
    }

  for (i = 0; i < ui->depth; i++)
    {
      if (ui->stack[i]->cls == &g_scr_pin)
        {
          return;
        }
    }

  ui_push(ui, &g_scr_pin, NULL);
}

static void ui_status_changed(FAR void *arg, uint8_t what)
{
  FAR struct ui_s *ui = arg;
  FAR struct screen_s *s = ui_top(ui);

  if (s == NULL)
    {
      return;
    }

  ui_header_update(ui, s);
  if (s->cls->status != NULL)
    {
      s->cls->status(s, what);
    }

  if (what & STATUS_TEL)
    {
      ui_pin_check(ui);
    }

  ui_place(ui);
}

static void ui_typing_off(FAR struct ui_s *ui)
{
  if (ui->config.input != NULL)
    {
      input_typing(ui->config.input, false);
    }
}

/* Wi-Fi's service came (back): the top screen reads its state again */

static void ui_wifi_state(FAR struct pnut_wifi_client_s *client,
                          bool connected, FAR void *arg)
{
  FAR struct ui_s *ui = arg;
  FAR struct screen_s *s = ui_top(ui);

  if (connected && s != NULL && s->cls->status != NULL)
    {
      s->cls->status(s, UI_WIFI_SERVICE);
      ui_place(ui);
    }
}

static void ui_options_tapped(FAR lv_event_t *e)
{
  FAR lv_obj_t *obj = lv_event_get_current_target(e);

  lv_obj_send_event(lv_obj_get_screen(obj), g_ui_event_block,
                    lv_event_get_user_data(e));
}

static void ui_options_close(FAR struct ui_s *ui)
{
  FAR struct ui_options_s *o = &ui->options;

  if (o->box != NULL)
    {
      lv_obj_delete_async(o->catcher);
      lv_obj_delete_async(o->shadow);
      lv_obj_delete_async(o->box);
      o->box     = NULL;
      o->shadow  = NULL;
      o->catcher = NULL;
    }
}

static void ui_options_focus(FAR struct ui_options_s *o, uint8_t focus)
{
  lv_obj_t *label;
  int i;

  o->focus = focus;
  for (i = 0; i < o->count; i++)
    {
      theme_inverted(o->rows[i], i == focus);
      label = lv_obj_get_child(o->rows[i], 0);
      lv_obj_set_style_text_font(label, i == focus ? &g_ui_sans_bold_13 :
                                                     &g_ui_sans_13, 0);
    }
}

static void ui_options_choose(FAR struct ui_s *ui, uint8_t index)
{
  FAR struct ui_options_s *o = &ui->options;
  FAR struct screen_s *s = o->screen;

  ui_options_close(ui);
  if (o->chosen != NULL && index < o->count)
    {
      o->chosen(s, index);
    }
}

/* A key while the options are open: theirs */

static void ui_options_key(FAR struct ui_s *ui, uint8_t action,
                           uint32_t ch)
{
  FAR struct ui_options_s *o = &ui->options;
  int i;

  switch (action)
    {
      case UI_ACT_CHAR:
        for (i = 0; i < o->count; i++)
          {
            if (o->letters[i] != '\0' && (uint32_t)o->letters[i] == ch)
              {
                ui_options_choose(ui, i);
                break;
              }
          }

        break;

      case UI_ACT_UP:
        ui_options_focus(o, o->focus > 0 ? o->focus - 1 : 0);
        break;

      case UI_ACT_DOWN:
        ui_options_focus(o, o->focus + 1 < o->count ? o->focus + 1 :
                         o->focus);
        break;

      case UI_ACT_OPEN:
      case UI_ACT_CONFIRM:
      case UI_ACT_SOFT_MIDDLE:
        ui_options_choose(ui, o->focus);
        break;

      case UI_ACT_BACK:
      case UI_ACT_SOFT_RIGHT:
      case UI_ACT_SOFT_LEFT:
      case UI_ACT_OPTIONS:
        ui_options_close(ui);
        break;

      default:
        break;
    }
}

static void ui_key_event(FAR lv_event_t *e)
{
  FAR struct screen_s *s = lv_event_get_user_data(e);
  uint32_t key = lv_event_get_key(e);
  uint8_t action = UI_KEY_IS_ACTION(key) ? UI_KEY_ACTION(key) : UI_ACT_CHAR;

  if (s != ui_top(s->ui))
    {
      return;
    }

  if (ui_options_open(s->ui))
    {
      ui_options_key(s->ui, action, action == UI_ACT_CHAR ? key : 0);
      ui_place(s->ui);
      return;
    }

  if ((s->cls->key == NULL ||
       !s->cls->key(s, action, action == UI_ACT_CHAR ? key : 0)) &&
      (action == UI_ACT_BACK || action == UI_ACT_SOFT_RIGHT))
    {
      ui_pop(s->ui);
    }

  ui_place(s->ui);
}

static void ui_block_event(FAR lv_event_t *e)
{
  FAR struct screen_s *s = lv_event_get_user_data(e);
  uint16_t id = (uint16_t)(uintptr_t)lv_event_get_param(e);

  /* While the options are open a tap is theirs, or closes them */

  if (s == ui_top(s->ui) && ui_options_open(s->ui))
    {
      if (id >= UI_OPTIONS_ID && id < UI_OPTIONS_ID + UI_OPTIONS)
        {
          ui_options_choose(s->ui, id - UI_OPTIONS_ID);
        }
      else
        {
          ui_options_close(s->ui);
        }

      ui_place(s->ui);
      return;
    }

  if (s == ui_top(s->ui) && s->cls->event != NULL)
    {
      s->cls->event(s, (uint16_t)(uintptr_t)lv_event_get_param(e));
      ui_place(s->ui);
    }
}

/* Show a screen of the stack: loaded, with the keys, its header as the
 * device is
 */

static void ui_load(FAR struct ui_s *ui, FAR struct screen_s *s)
{
  ui_header_update(ui, s);
  lv_group_focus_obj(s->root);
  lv_screen_load(s->root);
  lv_obj_update_layout(s->root);
  display_screen(&ui->display);
}

/* Let go of the framebuffer */

static void ui_close_screen(FAR struct ui_s *ui)
{
  if (ui->map != NULL)
    {
      munmap(ui->map, ui->maplen);
      ui->map = NULL;
    }

  if (ui->fd >= 0)
    {
      close(ui->fd);
      ui->fd = -1;
    }
}

/* The screen is open: Input's devices, then the screens */

static int ui_show(FAR struct ui_s *ui,
                   FAR const struct display_config_s *screen)
{
  int ret;

  ret = display_open(&ui->display, ui->module.loop, screen);
  if (ret < 0)
    {
      pnut_log(&ui->module, PNUT_LOG_ERROR, "No display: %d", ret);
      return ret;
    }

  /* The screen shows without input rather than not at all */

  ui->shown = true;
  ret = input_attach(ui->config.input);
  if (ret < 0)
    {
      pnut_log(&ui->module, PNUT_LOG_ERROR, "No input: %d", ret);
    }

  ret = ui_push(ui, &g_scr_home, NULL);
  ui_pin_check(ui);
  return ret;
}

static FAR const pnut_board_part_t *
ui_part(FAR const pnut_board_description_t *description,
        pnut_board_part_kind_t kind)
{
  pb_size_t i;

  for (i = 0; i < description->parts_count; i++)
    {
      if (description->parts[i].kind == kind &&
          description->parts[i].device[0] != '\0')
        {
          return &description->parts[i];
        }
    }

  return NULL;
}

static void ui_open_input(FAR struct ui_s *ui,
                          FAR const pnut_board_description_t *out,
                          pnut_board_part_kind_t kind, bool touch)
{
  FAR const pnut_board_part_t *part = ui_part(out, kind);
  int ret;

  if (part == NULL)
    {
      return;
    }

  ret = ui->config.open_input(ui->config.input, part->device, touch);
  if (ret < 0)
    {
      pnut_log(&ui->module, PNUT_LOG_WARNING, "Cannot open %s: %d",
               part->device, ret);
    }
}

static void ui_board(FAR struct pnut_board_client_s *client,
                     bool connected, FAR void *arg);

static void ui_ask_again(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct ui_s *ui = arg;

  ui->retry = NULL;
  if (ui->board_open)
    {
      ui_board(&ui->board, true, ui);
    }
}

static void ui_described(FAR struct pnut_board_client_s *client,
                         int status,
                         FAR const pnut_board_description_t *out,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct ui_s *ui = arg;
  FAR const pnut_board_part_t *part;
  struct display_config_s screen;
  int ret;

  if (status == PNUT_STATUS_TIMEOUT ||
      (status == PNUT_STATUS_UNAVAILABLE && err == NULL))
    {
      pnut_log(&ui->module, PNUT_LOG_WARNING,
               "No answer from Board: status %d; asking again", status);
      if (status == PNUT_STATUS_TIMEOUT)
        {
          ui_board(client, true, ui);
        }

      return;
    }

  pnut_board_disconnect(&ui->board);
  ui->board_open = false;
  if (status != PNUT_STATUS_OK || out == NULL)
    {
      pnut_log(&ui->module, PNUT_LOG_ERROR,
               "No description from Board: status %d; no screen", status);
      return;
    }

  part = ui_part(out, PNUT_BOARD_PART_KIND_SCREEN);
  if (part == NULL || ui->config.open_screen == NULL)
    {
      pnut_log(&ui->module, PNUT_LOG_ERROR, "The device has no screen");
      return;
    }

  memset(&screen, 0, sizeof(screen));
  ret = ui->config.open_screen(part->device, &screen, &ui->maplen);
  if (ret < 0)
    {
      pnut_log(&ui->module, PNUT_LOG_ERROR, "Cannot open %s: %d",
               part->device, ret);
      return;
    }

  ui->fd  = ret;
  ui->map = screen.frame;
  if (ui_show(ui, &screen) < 0)
    {
      ui_close_screen(ui);
      return;
    }

  if (ui->config.open_input == NULL)
    {
      return;
    }

  /* The touch controller's first open (the function keys' device on the
   * T-Deck Max) resets and wakes it: about 60 ms of the loop, once
   */

  ui_open_input(ui, out, PNUT_BOARD_PART_KIND_KEYBOARD, false);
  ui_open_input(ui, out, PNUT_BOARD_PART_KIND_FUNCTION_KEYS, false);
  ui_open_input(ui, out, PNUT_BOARD_PART_KIND_TOUCH, true);

  part = ui_part(out, PNUT_BOARD_PART_KIND_KEYBOARD);
  if (part != NULL && part->which_detail == PNUT_BOARD_PART_KEYBOARD_TAG)
    {
      input_set_layout(ui->config.input, part->detail.keyboard.layout);
    }
}

static void ui_board(FAR struct pnut_board_client_s *client,
                     bool connected, FAR void *arg)
{
  FAR struct ui_s *ui = arg;
  pnut_board_describe_request_t req;
  int ret;

  if (!connected)
    {
      return;
    }

  memset(&req, 0, sizeof(req));
  ret = pnut_board_describe(client, &req, 0, ui_described, ui);

  /* Not connected: the library connects again, and calls this again */

  if (ret < 0 && ret != -ENOTCONN && ui->retry == NULL)
    {
      pnut_log(&ui->module, PNUT_LOG_WARNING,
               "Cannot ask Board: %d; again in %d ms", ret, UI_RETRY_MS);
      pnut_timer_start(ui->module.loop, UI_RETRY_MS, 0, ui_ask_again, ui,
                       &ui->retry);
    }
}

/* A screen's state lives as long as its LVGL screen */

static void ui_root_deleted(FAR lv_event_t *e)
{
  lv_free(lv_event_get_user_data(e));
}

/* The top screen gone, without loading the one below.  A screen popped by
 * its own key or tap is still in LVGL's hands: it is deleted once LVGL is
 * done with it (async), its handlers left in place (LVGL is still going
 * through them) but no longer the top's.
 */

static void ui_drop_screen(FAR struct ui_s *ui, FAR struct screen_s *s,
                           bool async)
{
  if (s->cls->leave != NULL)
    {
      s->cls->leave(s);
    }

  lv_group_remove_obj(s->root);
  if (async)
    {
      lv_obj_delete_async(s->root);
    }
  else
    {
      lv_obj_delete(s->root);
    }
}

static void ui_drop(FAR struct ui_s *ui, bool async)
{
  ui_drop_screen(ui, ui->stack[--ui->depth], async);
}

/* A new screen of a class, with its LVGL screen and handlers, not built
 * yet; or NULL
 */

static FAR struct screen_s *ui_screen_new(FAR struct ui_s *ui,
                                          FAR const struct screen_class_s
                                          *cls, FAR const void *arg)
{
  FAR struct screen_s *s;

  s = lv_malloc_zeroed(sizeof(*s) + cls->size);
  if (s == NULL)
    {
      return NULL;
    }

  s->cls    = cls;
  s->ui     = ui;
  s->arg    = arg;
  s->state  = s + 1;
  s->serial = ++ui->serials;
  s->root  = lv_obj_create(NULL);
  theme_screen(s->root);
  lv_obj_add_event_cb(s->root, ui_key_event, LV_EVENT_KEY, s);
  lv_obj_add_event_cb(s->root, ui_block_event, g_ui_event_block, s);
  lv_obj_add_event_cb(s->root, ui_root_deleted, LV_EVENT_DELETE, s);
  if (input_group(ui->config.input) != NULL)
    {
      lv_group_add_obj(input_group(ui->config.input), s->root);
    }

  return s;
}

/* Where a screen is on the stack, or -1 */

static int ui_index(FAR struct ui_s *ui, FAR struct screen_s *s)
{
  int i;

  for (i = 0; i < ui->depth; i++)
    {
      if (ui->stack[i] == s)
        {
          return i;
        }
    }

  return -1;
}

static int ui_start(FAR struct pnut_module_s *module,
                    FAR struct pnut_loop_s *loop)
{
  FAR struct ui_s *ui = (FAR struct ui_s *)module;
  int ret;

  ret = glue_start(loop);
  if (ret < 0)
    {
      return ret;
    }

  theme_init();
  ret = status_start(&ui->status, loop, ui->config.now, ui_status_changed,
                     ui);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "No device state: %d", ret);
    }
  else
    {
      ui->status_open = true;
    }

  if (ui->config.services != NULL && ui->config.apps != NULL)
    {
      ret = host_open(&ui->host, ui, loop, ui->config.apps,
                      ui->config.services);
      if (ret < 0)
        {
          pnut_log(module, PNUT_LOG_ERROR, "No apps: %d", ret);
        }
      else
        {
          ui->host_open = true;
        }
    }

  pnut_module_ready(module);
  if (ui->config.board == NULL)
    {
      return ui->config.screen != NULL ? ui_show(ui, ui->config.screen) :
                                         0;
    }

  ret = pnut_board_connect(loop, ui->config.board, ui_board, ui,
                           &ui->board);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "No Board: %d; no screen", ret);
      return 0;
    }

  ui->board_open = true;
  return 0;
}

static void ui_stop(FAR struct pnut_module_s *module,
                    FAR struct pnut_loop_s *loop)
{
  FAR struct ui_s *ui = (FAR struct ui_s *)module;

  if (ui->host_open)
    {
      host_close(&ui->host);
      ui->host_open = false;
    }

  if (ui->retry != NULL)
    {
      pnut_timer_cancel(loop, ui->retry);
      ui->retry = NULL;
    }

  if (ui->board_open)
    {
      pnut_board_disconnect(&ui->board);
      ui->board_open = false;
    }

  while (ui->depth > 0)
    {
      ui_drop(ui, false);
    }

  if (ui->wifi_open)
    {
      pnut_wifi_disconnect(&ui->wifi);
      ui->wifi_open = false;
    }

  if (ui->tel_open)
    {
      pnut_telephony_disconnect(&ui->tel);
      ui->tel_open = false;
    }

  if (ui->status_open)
    {
      status_stop(&ui->status);
      ui->status_open = false;
    }

  if (ui->shown)
    {
      display_close(&ui->display);
      ui->shown = false;
    }

  ui_close_screen(ui);
  glue_stop();
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ui_init(FAR struct ui_s *ui, FAR const struct ui_config_s *config)
{
  memset(ui, 0, sizeof(*ui));
  ui->config       = *config;
  ui->fd           = -1;
  ui->module.name  = "ui";
  ui->module.start = ui_start;
  ui->module.stop  = ui_stop;
}

int ui_push(FAR struct ui_s *ui, FAR const struct screen_class_s *cls,
            FAR const void *arg)
{
  FAR struct screen_s *s;

  ui_options_close(ui);

  if (ui->depth == UI_DEPTH)
    {
      return -ENOSPC;
    }

  ui_typing_off(ui);
  s = ui_screen_new(ui, cls, arg);
  if (s == NULL)
    {
      return -ENOMEM;
    }

  ui->stack[ui->depth++] = s;
  cls->build(s);

  /* Building it may have taken it off again (an app's screen whose app
   * went): then the screen below shows already
   */

  if (ui_index(ui, s) == ui->depth - 1)
    {
      ui_load(ui, s);
    }

  return 0;
}

void ui_pop(FAR struct ui_s *ui)
{
  FAR struct screen_s *below;

  if (ui->depth <= 1)
    {
      return;
    }

  /* The one below on screen first: the top one is deleted after */

  ui_options_close(ui);
  below = ui->stack[ui->depth - 2];
  ui_load(ui, below);
  ui_drop(ui, true);
  if (ui->config.input != NULL)
    {
      input_typing(ui->config.input, below->typing);
    }

  if (below->cls->status != NULL)
    {
      below->cls->status(below, UI_STATUS_ALL);
    }
}

FAR struct screen_s *ui_top(FAR struct ui_s *ui)
{
  return ui->depth > 0 ? ui->stack[ui->depth - 1] : NULL;
}

int ui_replace(FAR struct ui_s *ui, FAR struct screen_s *old,
               FAR const struct screen_class_s *cls, FAR const void *arg)
{
  FAR struct screen_s *s;
  int i = ui_index(ui, old);
  bool top;

  if (i < 0)
    {
      return -ENOENT;
    }

  top = i == ui->depth - 1;

  if (ui->options.box != NULL && ui->options.screen == old)
    {
      ui_options_close(ui);
    }

  if (top)
    {
      ui_typing_off(ui);
    }

  s = ui_screen_new(ui, cls, arg);
  if (s == NULL)
    {
      return -ENOMEM;
    }

  ui->stack[i] = s;
  cls->build(s);

  /* The old one is deleted after: LVGL may be going through its handlers.
   * Building may have taken the new one off again: then the screen below
   * shows already.
   */

  if (top && ui_index(ui, s) == ui->depth - 1)
    {
      ui_load(ui, s);
    }

  ui_drop_screen(ui, old, true);
  return 0;
}

int ui_insert(FAR struct ui_s *ui, int index,
              FAR const struct screen_class_s *cls, FAR const void *arg)
{
  FAR struct screen_s *s;

  if (index <= 0 || index >= ui->depth)
    {
      return ui_push(ui, cls, arg);
    }

  if (ui->depth == UI_DEPTH)
    {
      return -ENOSPC;
    }

  s = ui_screen_new(ui, cls, arg);
  if (s == NULL)
    {
      return -ENOMEM;
    }

  memmove(&ui->stack[index + 1], &ui->stack[index],
          (ui->depth - index) * sizeof(ui->stack[0]));
  ui->stack[index] = s;
  ui->depth++;
  cls->build(s);
  return 0;
}

void ui_remove(FAR struct ui_s *ui, FAR struct screen_s *s)
{
  int i = ui_index(ui, s);

  if (i <= 0)
    {
      return;                     /* Not on the stack, or Home */
    }

  if (i == ui->depth - 1)
    {
      ui_pop(ui);
      return;
    }

  if (ui->options.box != NULL && ui->options.screen == s)
    {
      ui_options_close(ui);
    }

  memmove(&ui->stack[i], &ui->stack[i + 1],
          (ui->depth - i - 1) * sizeof(ui->stack[0]));
  ui->depth--;
  ui_drop_screen(ui, s, true);
}

FAR lv_obj_t *ui_header(FAR struct screen_s *s, FAR const char *icon,
                        FAR const char *title)
{
  s->header = block_header(s->root, icon, title);
  ui_header_update(s->ui, s);
  return s->header;
}

void ui_local(FAR struct ui_s *ui, FAR struct tm *tm)
{
  status_local(&ui->status, status_now(&ui->status), tm);
}

int ui_options(FAR struct screen_s *s, FAR const char * const *items,
               uint8_t count,
               CODE void (*chosen)(FAR struct screen_s *s, int index))
{
  return ui_options_lettered(s, items, NULL, count, chosen);
}

int ui_options_lettered(FAR struct screen_s *s,
                        FAR const char * const *items,
                        FAR const char *letters, uint8_t count,
                        CODE void (*chosen)(FAR struct screen_s *s,
                                            int index))
{
  FAR struct ui_options_s *o = &s->ui->options;
  char caps[2];
  FAR lv_obj_t *head;
  FAR lv_obj_t *label;
  int32_t h = 2 + UI_OPTIONS_HEAD + count * UI_OPTIONS_ROW + 2;
  int32_t y = 320 - UI_OPTIONS_FOOT - h;
  int i;

  if (count == 0 || count > UI_OPTIONS)
    {
      return -EINVAL;
    }

  ui_options_close(s->ui);
  o->screen = s;
  o->chosen = chosen;
  o->count  = count;

  /* A tap outside them, above the softkeys, closes them (the softkeys still
   * take theirs); the canvas's offset shadow, then the frame
   */

  o->catcher = lv_obj_create(s->root);
  theme_box(o->catcher);
  lv_obj_set_pos(o->catcher, 0, 0);
  lv_obj_set_size(o->catcher, 240, 320 - BLOCK_FOOTER_H);
  lv_obj_add_flag(o->catcher, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(o->catcher, ui_options_tapped, LV_EVENT_CLICKED,
                      (FAR void *)(uintptr_t)UI_OPTIONS_OUTSIDE);

  o->shadow = lv_obj_create(s->root);
  theme_box(o->shadow);
  lv_obj_set_pos(o->shadow, UI_OPTIONS_X + 4, y + 4);
  lv_obj_set_size(o->shadow, UI_OPTIONS_W, h);
  lv_obj_set_style_bg_color(o->shadow, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(o->shadow, LV_OPA_COVER, 0);

  o->box = lv_obj_create(s->root);
  theme_box(o->box);
  lv_obj_set_pos(o->box, UI_OPTIONS_X, y);
  lv_obj_set_size(o->box, UI_OPTIONS_W, h);
  lv_obj_set_style_bg_color(o->box, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(o->box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o->box, 2, 0);
  lv_obj_set_style_border_color(o->box, lv_color_black(), 0);
  lv_obj_add_flag(o->box, LV_OBJ_FLAG_CLICKABLE);

  head = lv_label_create(o->box);
  theme_label(head, &g_ui_mono_bold_10);
  lv_label_set_text(head, UI_T_OPTIONS_HEAD);
  lv_obj_set_pos(head, 6, (UI_OPTIONS_HEAD - 14) / 2);

  for (i = 0; i < count; i++)
    {
      o->rows[i] = lv_obj_create(o->box);
      theme_box(o->rows[i]);
      lv_obj_set_pos(o->rows[i], 0, UI_OPTIONS_HEAD + i * UI_OPTIONS_ROW);
      lv_obj_set_size(o->rows[i], UI_OPTIONS_W - 4, UI_OPTIONS_ROW);
      label = lv_label_create(o->rows[i]);
      theme_label(label, &g_ui_sans_13);
      lv_label_set_text(label, items[i]);
      lv_obj_align(label, LV_ALIGN_LEFT_MID, 6, 0);

      o->letters[i] = letters != NULL ? letters[i] : '\0';
      if (o->letters[i] != '\0')
        {
          caps[0] = o->letters[i] >= 'a' && o->letters[i] <= 'z' ?
                    o->letters[i] - 'a' + 'A' : o->letters[i];
          caps[1] = '\0';
          label = lv_label_create(o->rows[i]);
          theme_label(label, &g_ui_mono_bold_12);
          lv_label_set_text(label, caps);
          lv_obj_align(label, LV_ALIGN_RIGHT_MID, -8, 0);
        }

      block_rule(o->rows[i], 0, 0, UI_OPTIONS_W - 4, true);
      lv_obj_add_flag(o->rows[i], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(o->rows[i], ui_options_tapped, LV_EVENT_CLICKED,
                          (FAR void *)(uintptr_t)(UI_OPTIONS_ID + i));
    }

  ui_options_focus(o, 0);
  ui_place(s->ui);
  return 0;
}

bool ui_options_open(FAR struct ui_s *ui)
{
  return ui->options.box != NULL;
}

FAR struct screen_s *ui_find(FAR struct ui_s *ui, uint32_t serial)
{
  int i;

  for (i = 0; i < ui->depth; i++)
    {
      if (ui->stack[i]->serial == serial)
        {
          return ui->stack[i];
        }
    }

  return NULL;
}

FAR struct pnut_wifi_client_s *ui_wifi(FAR struct ui_s *ui)
{
  int ret;

  if (!ui->wifi_open)
    {
      ret = pnut_wifi_connect(ui->module.loop, UI_WIFI, ui_wifi_state, ui,
                              &ui->wifi);
      if (ret < 0)
        {
          pnut_log(&ui->module, PNUT_LOG_ERROR, "No Wi-Fi client: %d",
                   ret);
          return NULL;
        }

      ui->wifi_open = true;
    }

  return &ui->wifi;
}

void ui_typing(FAR struct screen_s *s, bool typing)
{
  s->typing = typing;
  if (s == ui_top(s->ui) && s->ui->config.input != NULL)
    {
      input_typing(s->ui->config.input, typing);
    }
}

FAR struct pnut_telephony_client_s *ui_telephony(FAR struct ui_s *ui)
{
  int ret;

  if (!ui->tel_open)
    {
      ret = pnut_telephony_connect(ui->module.loop, UI_TELEPHONY, NULL, NULL,
                                   &ui->tel);
      if (ret < 0)
        {
          pnut_log(&ui->module, PNUT_LOG_ERROR, "No Telephony client: %d",
                   ret);
          return NULL;
        }

      ui->tel_open = true;
    }

  return &ui->tel;
}
