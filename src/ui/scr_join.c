/****************************************************************************
 * pnut-os/src/ui/scr_join.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Joining a new secured network (Set-WiFi-Join): its security, the
 * password (8 to 63 characters, shown as dots unless Show password is on),
 * Join on the middle softkey or Enter in the field.  A refusal is said
 * here; once joined the screen goes.
 *
 * The password is only ever in the field's buffer and the request, both
 * wiped: the request after the call, the field as the screen goes.  An
 * answer that comes after the screen went changes nothing.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <string.h>

#include <pnut/msg.h>

#include "blocks.h"
#include "describe.h"
#include "icons.h"
#include "keymap.h"
#include "list.h"
#include "text.h"
#include "theme.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define JOIN_ID_FIELD  1
#define JOIN_ID_SHOW   2

#define JOIN_MIN       8          /* A WPA passphrase's length */
#define JOIN_MAX       63
#define JOIN_MS        60000      /* Join takes up to about 35 s */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct join_scr_s
{
  struct block_field_s field;     /* First: wiped as the screen goes */
  struct list_s list;
  char name[sizeof(((pnut_wifi_network_t *)0)->name)];
  FAR lv_obj_t *field_row;
  FAR lv_obj_t *show;
  FAR lv_obj_t *message;
  bool joining;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void join_message(FAR struct join_scr_s *st, FAR const char *text)
{
  block_set_text(st->message, text != NULL ? text : "");
}

/* The focus on a row: the field takes the keys while it has it */

static void join_focus(FAR struct screen_s *s, FAR lv_obj_t *row)
{
  FAR struct join_scr_s *st = s->state;

  list_focus(&st->list, row);
  st->field.focused = row == st->field_row;
  block_field_show(&st->field);
  ui_typing(s, st->field.focused);
  s->focus = block_id(row);
}

static void join_show_password(FAR struct join_scr_s *st)
{
  st->field.masked = !st->field.masked;
  block_switch_set(st->show, !st->field.masked);
  block_field_show(&st->field);
}

static void join_done(FAR struct pnut_wifi_client_s *client, int status,
                      FAR const pnut_wifi_reply_t *out,
                      FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct ui_s *ui = (FAR struct ui_s *)((FAR char *)client -
                                            offsetof(struct ui_s, wifi));
  FAR struct screen_s *s = ui_find(ui, (uint32_t)(uintptr_t)arg);
  FAR struct join_scr_s *st;

  if (s == NULL)
    {
      return;                     /* Gone: nothing to change */
    }

  st = s->state;
  st->joining = false;
  if (status == PNUT_STATUS_OK && s == ui_top(ui))
    {
      ui_pop(ui);
      return;
    }

  join_message(st, status == PNUT_STATUS_OK ? "" :
               describe_wifi_failure(status, err != NULL ? err->code : 0));
  ui_place(ui);
}

static void join_go(FAR struct screen_s *s)
{
  FAR struct join_scr_s *st = s->state;
  FAR struct pnut_wifi_client_s *client;
  pnut_wifi_join_request_t req;
  int ret;

  if (st->joining)
    {
      return;
    }

  if (st->field.len < JOIN_MIN || st->field.len > JOIN_MAX)
    {
      join_message(st, UI_T_PASSWORD_LENGTH);
      return;
    }

  client = ui_wifi(s->ui);
  if (client == NULL)
    {
      join_message(st, UI_T_WIFI_NO_ANSWER);
      return;
    }

  memset(&req, 0, sizeof(req));
  strlcpy(req.name, st->name, sizeof(req.name));
  strlcpy(req.passphrase, st->field.text, sizeof(req.passphrase));
  ret = pnut_wifi_join(client, &req, JOIN_MS, join_done,
                       (FAR void *)(uintptr_t)s->serial);
  explicit_bzero(&req, sizeof(req));
  if (ret < 0)
    {
      join_message(st, UI_T_WIFI_NO_ANSWER);
      return;
    }

  st->joining = true;
  join_message(st, UI_T_JOINING_DOTS);
}

static void join_build(FAR struct screen_s *s)
{
  FAR struct join_scr_s *st = s->state;
  FAR const pnut_wifi_network_t *n = s->arg;
  char name[DESCRIBE_NAME_MAX];

  strlcpy(st->name, n->name, sizeof(st->name));
  describe_name(st->name, name, sizeof(name));
  ui_header(s, UI_ICON_WIFI, name);

  list_init(&st->list, s->root);
  block_info(st->list.box, UI_T_SECURITY, UI_T_WPA);
  st->field.max    = JOIN_MAX;
  st->field.masked = true;
  st->field_row = block_field(st->list.box, JOIN_ID_FIELD, UI_T_PASSWORD,
                              &st->field);
  st->show = block_switch_row(st->list.box, JOIN_ID_SHOW,
                              UI_T_SHOW_PASSWORD, false);

  st->message = lv_label_create(st->list.box);
  theme_label(st->message, &g_ui_sans_12);
  lv_label_set_long_mode(st->message, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(st->message, lv_pct(100));
  lv_obj_set_style_pad_hor(st->message, 8, 0);
  lv_obj_set_style_pad_ver(st->message, 6, 0);
  lv_label_set_text(st->message, "");

  block_softkeys(s->root, "", UI_T_JOIN, UI_T_BACK);
  join_focus(s, st->field_row);
}

static bool join_key(FAR struct screen_s *s, uint8_t action, uint32_t ch)
{
  FAR struct join_scr_s *st = s->state;

  if (st->field.focused && block_field_key(&st->field, action, ch))
    {
      return true;
    }

  switch (action)
    {
      case UI_ACT_UP:
      case UI_ACT_DOWN:
        list_move(&st->list, action == UI_ACT_DOWN ? 1 : -1);
        join_focus(s, st->list.focus);
        return true;

      case UI_ACT_LEAVE_FIELD:
        list_move(&st->list, 1);
        join_focus(s, st->list.focus);
        return true;

      case UI_ACT_OPEN:
        if (st->list.focus == st->show)
          {
            join_show_password(st);
            return true;
          }

        join_go(s);
        return true;

      case UI_ACT_CONFIRM:
      case UI_ACT_SOFT_MIDDLE:
        join_go(s);
        return true;

      default:
        return false;
    }
}

static void join_event(FAR struct screen_s *s, uint16_t id)
{
  FAR struct join_scr_s *st = s->state;

  if (id == JOIN_ID_FIELD)
    {
      join_focus(s, st->field_row);
    }
  else if (id == JOIN_ID_SHOW)
    {
      join_focus(s, st->show);
      join_show_password(st);
    }
}

static void join_leave(FAR struct screen_s *s)
{
  FAR struct join_scr_s *st = s->state;

  block_field_wipe(&st->field);
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct screen_class_s g_scr_join =
{
  "join",
  join_build,
  join_key,
  join_event,
  NULL,
  join_leave,
  sizeof(struct join_scr_s)
};
