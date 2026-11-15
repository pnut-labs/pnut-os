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
#include "theme.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Asking Board again after a call that could not be made */

#define UI_RETRY_MS  1000

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* LVGL places objects lazily, at the next draw, which may wait for the
 * keys' half second: what a handler built is placed at once, so that a tap
 * meanwhile finds it
 */

static void ui_place(FAR struct ui_s *ui)
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

  ui_place(ui);
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

  return ui_push(ui, &g_scr_home, NULL);
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

static void ui_drop(FAR struct ui_s *ui, bool async)
{
  FAR struct screen_s *s = ui->stack[--ui->depth];

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

  if (ui->depth == UI_DEPTH)
    {
      return -ENOSPC;
    }

  s = lv_malloc_zeroed(sizeof(*s) + cls->size);
  if (s == NULL)
    {
      return -ENOMEM;
    }

  s->cls   = cls;
  s->ui    = ui;
  s->arg   = arg;
  s->state = s + 1;
  s->root  = lv_obj_create(NULL);
  theme_screen(s->root);
  lv_obj_add_event_cb(s->root, ui_key_event, LV_EVENT_KEY, s);
  lv_obj_add_event_cb(s->root, ui_block_event, g_ui_event_block, s);
  lv_obj_add_event_cb(s->root, ui_root_deleted, LV_EVENT_DELETE, s);
  if (input_group(ui->config.input) != NULL)
    {
      lv_group_add_obj(input_group(ui->config.input), s->root);
    }

  ui->stack[ui->depth++] = s;
  cls->build(s);
  ui_load(ui, s);
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

  below = ui->stack[ui->depth - 2];
  ui_load(ui, below);
  ui_drop(ui, true);
  if (below->cls->status != NULL)
    {
      below->cls->status(below, STATUS_TEL | STATUS_WIFI | STATUS_TIME |
                                STATUS_BATTERY);
    }
}

FAR struct screen_s *ui_top(FAR struct ui_s *ui)
{
  return ui->depth > 0 ? ui->stack[ui->depth - 1] : NULL;
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
