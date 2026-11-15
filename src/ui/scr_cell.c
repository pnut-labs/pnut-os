/****************************************************************************
 * pnut-os/src/ui/scr_cell.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Cellular (Set-Cell, the first row of Set-Cell-2): mobile data
 * (telephony.data), the access point (telephony.apn, a text field: empty
 * for the network's own), the modem (telephony.enabled); then, only to
 * read, the network, the signal and the SIM from Telephony's topic.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <string.h>

#include <pnut/msg.h>

#include "blocks.h"
#include "describe.h"
#include "icons.h"
#include "keymap.h"
#include "list.h"
#include "setting.h"
#include "text.h"
#include "theme.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CELL_ID_DATA   1
#define CELL_ID_APN    2
#define CELL_ID_MODEM  3

/* The settings watched, by their place in the list */

#define CELL_DATA      0
#define CELL_ENABLED   1
#define CELL_APN       2
#define CELL_SETTINGS  3

#define CELL_APN_MAX   63         /* The setting's 64 bytes, its end too */

#define CELL_LINE_MAX  (DESCRIBE_NAME_MAX + 32)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct cell_scr_s
{
  struct block_field_s field;     /* The access point */
  struct list_s list;
  struct setting_watch_s watch;
  struct setting_s settings[CELL_SETTINGS];
  bool watch_open;
  bool edited;                    /* The field typed in, not set yet */

  FAR lv_obj_t *data;
  FAR lv_obj_t *apn;
  FAR lv_obj_t *message;          /* Why the field was not set */
  FAR lv_obj_t *modem;
  FAR lv_obj_t *network;
  FAR lv_obj_t *signal;
  FAR lv_obj_t *sim;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void cell_message(FAR struct cell_scr_s *st, FAR const char *text)
{
  block_set_text(st->message, text != NULL ? text : "");
  if (text != NULL && text[0] != '\0')
    {
      lv_obj_remove_flag(st->message, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_add_flag(st->message, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool cell_on(FAR struct cell_scr_s *st, int index)
{
  return st->settings[index].known &&
         st->settings[index].value.value.boolean;
}

/* The field shows the access point set, unless typed in: focused but
 * untouched it follows too, so that Enter never sets what it no longer is
 */

static void cell_show_settings(FAR struct cell_scr_s *st)
{
  FAR struct setting_s *apn = &st->settings[CELL_APN];

  if (st->settings[CELL_DATA].known)
    {
      block_switch_set(st->data, cell_on(st, CELL_DATA));
    }

  if (st->settings[CELL_ENABLED].known)
    {
      block_switch_set(st->modem, cell_on(st, CELL_ENABLED));
    }

  if (apn->known && !st->edited)
    {
      strlcpy(st->field.text, apn->value.value.text,
              sizeof(st->field.text));
      st->field.len = strlen(st->field.text);
      block_field_show(&st->field);
    }
}

static void cell_show_state(FAR struct screen_s *s)
{
  FAR struct cell_scr_s *st = s->state;
  FAR const struct ui_status_s *dev = &s->ui->status;
  char line[CELL_LINE_MAX];

  if (!dev->has_tel)
    {
      return;
    }

  describe_operator(&dev->tel, line, sizeof(line));
  block_info_set(st->network, line);
  describe_signal(&dev->tel, line, sizeof(line));
  block_info_set(st->signal, line);
  block_info_set(st->sim, describe_sim_state(&dev->tel));
}

static void cell_setting(FAR void *arg, int index)
{
  FAR struct screen_s *s = arg;

  cell_show_settings(s->state);
  ui_place(s->ui);
}

static void cell_set_done(FAR void *arg, int status, uint32_t code)
{
  FAR struct screen_s *s = arg;
  FAR struct cell_scr_s *st = s->state;

  if (status != PNUT_STATUS_OK)
    {
      cell_message(st, UI_T_NOT_SAVED);
      ui_place(s->ui);
    }
}

/* The access point's answer: once taken, the field follows the setting
 * again (not before, or another setting's change in between would show
 * the old one)
 */

static void cell_apn_done(FAR void *arg, int status, uint32_t code)
{
  FAR struct screen_s *s = arg;
  FAR struct cell_scr_s *st = s->state;

  if (status == PNUT_STATUS_OK)
    {
      st->edited = false;
    }

  cell_set_done(arg, status, code);
}

/* Letters, digits, dots, hyphens and underscores, as Telephony takes them;
 * empty for the network's own
 */

static bool cell_apn_good(FAR const char *apn)
{
  for (; *apn != '\0'; apn++)
    {
      if (!((*apn >= 'a' && *apn <= 'z') || (*apn >= 'A' && *apn <= 'Z') ||
            (*apn >= '0' && *apn <= '9') || *apn == '.' || *apn == '-' ||
            *apn == '_'))
        {
          return false;
        }
    }

  return true;
}

static void cell_set_apn(FAR struct screen_s *s)
{
  FAR struct cell_scr_s *st = s->state;

  if (!cell_apn_good(st->field.text))
    {
      cell_message(st, UI_T_APN_BAD);
      return;
    }

  cell_message(st, NULL);
  if (setting_set_text(&st->watch, CELL_APN, st->field.text, cell_apn_done,
                       s) < 0)
    {
      cell_message(st, UI_T_NOT_SAVED);
    }
}

static void cell_toggle(FAR struct screen_s *s, int index)
{
  FAR struct cell_scr_s *st = s->state;

  if (st->settings[index].known &&
      setting_set_bool(&st->watch, index, !cell_on(st, index),
                       cell_set_done, s) < 0)
    {
      cell_message(st, UI_T_NOT_SAVED);
    }
}

/* The focus on a row: the field takes the keys while it has it */

static void cell_focus(FAR struct screen_s *s, FAR lv_obj_t *row)
{
  FAR struct cell_scr_s *st = s->state;

  list_focus(&st->list, row);
  st->field.focused = row == st->apn;
  block_field_show(&st->field);
  ui_typing(s, st->field.focused);
  s->focus = row != NULL ? block_id(row) : 0;
}

static void cell_move(FAR struct screen_s *s, int dir)
{
  FAR struct cell_scr_s *st = s->state;

  list_move(&st->list, dir);
  cell_focus(s, st->list.focus);
}

static void cell_open(FAR struct screen_s *s)
{
  FAR struct cell_scr_s *st = s->state;

  if (st->list.focus == st->data)
    {
      cell_toggle(s, CELL_DATA);
    }
  else if (st->list.focus == st->modem)
    {
      cell_toggle(s, CELL_ENABLED);
    }
  else if (st->list.focus == st->apn)
    {
      cell_set_apn(s);
    }
}

static void cell_build(FAR struct screen_s *s)
{
  FAR struct cell_scr_s *st = s->state;

  ui_header(s, UI_ICON_CELLULAR, UI_T_CELLULAR);
  list_init(&st->list, s->root);

  st->data = block_switch_row(st->list.box, CELL_ID_DATA, UI_T_MOBILE_DATA,
                              false);
  block_row_set_value(st->data, UI_T_UNKNOWN);

  st->field.max = CELL_APN_MAX;
  st->apn = block_field(st->list.box, CELL_ID_APN, UI_T_ACCESS_POINT,
                        &st->field);

  st->message = lv_label_create(st->list.box);
  theme_label(st->message, &g_ui_sans_12);
  lv_label_set_long_mode(st->message, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(st->message, lv_pct(100));
  lv_obj_set_style_pad_hor(st->message, 8, 0);
  lv_obj_set_style_pad_ver(st->message, 4, 0);
  lv_obj_add_flag(st->message, LV_OBJ_FLAG_HIDDEN);

  st->modem = block_switch_row(st->list.box, CELL_ID_MODEM, UI_T_MODEM,
                               false);
  block_row_set_value(st->modem, UI_T_UNKNOWN);

  st->network = block_info(st->list.box, UI_T_NETWORK_ROW, UI_T_UNKNOWN);
  st->signal  = block_info(st->list.box, UI_T_SIGNAL, UI_T_UNKNOWN);
  st->sim     = block_info(st->list.box, UI_T_SIM, UI_T_UNKNOWN);
  block_softkeys(s->root, "", UI_T_OPEN, UI_T_BACK);

  st->settings[CELL_DATA].owner    = "telephony";
  st->settings[CELL_DATA].key      = "data";
  st->settings[CELL_ENABLED].owner = "telephony";
  st->settings[CELL_ENABLED].key   = "enabled";
  st->settings[CELL_APN].owner     = "telephony";
  st->settings[CELL_APN].key       = "apn";
  st->watch_open = setting_open(&st->watch, s->ui->module.loop,
                                st->settings, CELL_SETTINGS, cell_setting,
                                s) >= 0;

  cell_show_state(s);
  cell_focus(s, st->data);
}

static bool cell_key(FAR struct screen_s *s, uint8_t action, uint32_t ch)
{
  FAR struct cell_scr_s *st = s->state;
  uint8_t len;

  len = st->field.len;
  if (st->field.focused && block_field_key(&st->field, action, ch))
    {
      st->edited = st->edited || st->field.len != len;
      return true;
    }

  switch (action)
    {
      case UI_ACT_UP:
      case UI_ACT_DOWN:
        cell_move(s, action == UI_ACT_DOWN ? 1 : -1);
        return true;

      case UI_ACT_LEAVE_FIELD:
        cell_move(s, 1);
        return true;

      case UI_ACT_OPEN:
      case UI_ACT_CONFIRM:
      case UI_ACT_SOFT_MIDDLE:
        cell_open(s);
        return true;

      default:
        return false;
    }
}

static void cell_event(FAR struct screen_s *s, uint16_t id)
{
  FAR struct cell_scr_s *st = s->state;
  FAR lv_obj_t *row = list_find(&st->list, id);

  if (row == NULL)
    {
      return;
    }

  cell_focus(s, row);
  if (row != st->apn)
    {
      cell_open(s);
    }
}

static void cell_status(FAR struct screen_s *s, uint8_t what)
{
  if (what & STATUS_TEL)
    {
      cell_show_state(s);
    }
}

static void cell_leave(FAR struct screen_s *s)
{
  FAR struct cell_scr_s *st = s->state;

  if (st->watch_open)
    {
      setting_close(&st->watch);
      st->watch_open = false;
    }
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct screen_class_s g_scr_cell =
{
  "cell",
  cell_build,
  cell_key,
  cell_event,
  cell_status,
  cell_leave,
  sizeof(struct cell_scr_s)
};
