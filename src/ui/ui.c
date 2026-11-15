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
 * So far it shows a check screen for the developer: each key's action and
 * each tap, to try the screen and Input on the device.
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
#include "theme.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Asking Board again after a call that could not be made */

#define UI_RETRY_MS  1000

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The actions' names, for the check screen */

static FAR const char * const g_ui_actions[] =
{
  "NONE", "UP", "DOWN", "LEFT", "RIGHT", "BACK", "OPEN", "OPTIONS",
  "PAGE_NEXT", "PAGE_PREV", "SOFT_LEFT", "SOFT_MIDDLE", "SOFT_RIGHT",
  "CHAR", "DELETE", "CONFIRM", "LEAVE_FIELD"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void ui_check_key(FAR lv_event_t *e)
{
  FAR struct ui_s *ui = lv_event_get_user_data(e);
  uint32_t key = lv_event_get_key(e);
  uint32_t action;

  if (!UI_KEY_IS_ACTION(key))
    {
      lv_label_set_text_fmt(ui->check, "CHAR U+%04lx", (unsigned long)key);
      return;
    }

  action = UI_KEY_ACTION(key);
  lv_label_set_text(ui->check, action < sizeof(g_ui_actions) /
                               sizeof(g_ui_actions[0]) ?
                               g_ui_actions[action] : "?");
}

static void ui_check_tap(FAR lv_event_t *e)
{
  FAR struct ui_s *ui = lv_event_get_user_data(e);
  lv_point_t p;

  lv_indev_get_point(lv_indev_active(), &p);
  lv_label_set_text_fmt(ui->check, "TAP %d %d", (int)p.x, (int)p.y);
}

/* The developer's check screen */

static void ui_check(FAR struct ui_s *ui)
{
  FAR lv_obj_t *scr = lv_screen_active();
  FAR lv_obj_t *title;

  lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  title = lv_label_create(scr);
  lv_obj_set_style_text_font(title, &g_ui_sans_bold_16, 0);
  lv_obj_set_style_text_color(title, lv_color_black(), 0);
  lv_obj_set_pos(title, 8, 8);
  lv_label_set_text(title, "pnut-os: input check");

  ui->check = lv_label_create(scr);
  lv_obj_set_style_text_font(ui->check, &g_ui_sans_14, 0);
  lv_obj_set_style_text_color(ui->check, lv_color_black(), 0);
  lv_obj_set_pos(ui->check, 8, 40);
  lv_label_set_text(ui->check, "Press a key, or tap");

  lv_obj_add_event_cb(scr, ui_check_key, LV_EVENT_KEY, ui);
  lv_obj_add_event_cb(scr, ui_check_tap, LV_EVENT_CLICKED, ui);
  lv_group_add_obj(input_group(ui->config.input), scr);
  lv_group_focus_obj(scr);
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

  ui_check(ui);
  return 0;
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
