/****************************************************************************
 * pnut-os/src/ui/scr_pin.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The SIM PIN (Proto-SIM-PIN): over any screen while Telephony's topic
 * says the SIM needs its PIN or PUK (ui.c opens it).  The code is typed
 * with the digits, or the letter keys that carry them on the keyboard
 * Board names; OK sends it to Telephony.  A PUK is asked first, then a new
 * PIN, and both go together.  A blocked SIM says so, and only Skip is
 * left.  Skip goes on without the SIM until it asks anew.
 *
 * A code is only ever in the screen's buffers and the request: the request
 * wiped after the call, the buffers after it too and as the screen goes;
 * nothing logs it.  An answer after the screen went changes nothing.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <pnut/msg.h>

#include "blocks.h"
#include "icons.h"
#include "keymap.h"
#include "text.h"
#include "theme.h"
#include "ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PIN_MIN       4
#define PIN_MAX       8
#define PUK_LEN       8
#define PIN_CODE      (PIN_MAX + 1)

/* The top block: its small head, the code's dots under it, a 2 px rule */

#define PIN_HEAD_Y    (BLOCK_HEADER_H + 4)
#define PIN_DOTS_Y    (BLOCK_HEADER_H + 18)
#define PIN_RULE_Y    (BLOCK_HEADER_H + 56)
#define PIN_LIST_Y    (PIN_RULE_Y + 2)
#define PIN_PAD       12

/* A dot and a space each, the last without */

#define PIN_DOT       "\xe2\x80\xa2"
#define PIN_SHOWN     (PIN_MAX * 4)

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum pin_stage_e
{
  PIN_STAGE_PIN = 0,
  PIN_STAGE_PUK,
  PIN_STAGE_NEW,                  /* The new PIN, after the PUK */
  PIN_STAGE_BLOCKED
};

struct pin_scr_s
{
  char code[PIN_CODE];            /* First: the code typed (tests read it) */
  char puk[PIN_CODE];             /* The PUK, kept for the new PIN */
  uint8_t len;
  uint8_t stage;
  bool asking;                    /* A code is out */
  bool sent_puk;                  /* What it was */

  FAR lv_obj_t *head;
  FAR lv_obj_t *dots;
  FAR lv_obj_t *tries;
  FAR lv_obj_t *hint;
  FAR lv_obj_t *message;
  FAR lv_obj_t *keys;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void pin_wipe(FAR struct pin_scr_s *st)
{
  explicit_bzero(st->code, sizeof(st->code));
  st->len = 0;
}

static void pin_show_dots(FAR struct pin_scr_s *st)
{
  char shown[PIN_SHOWN + 1];
  size_t o = 0;
  int i;

  for (i = 0; i < st->len; i++)
    {
      if (i > 0)
        {
          shown[o++] = ' ';
        }

      memcpy(shown + o, PIN_DOT, 3);
      o += 3;
    }

  shown[o] = '\0';
  block_set_text(st->dots, shown);
}

static void pin_show_tries(FAR struct screen_s *s)
{
  FAR struct pin_scr_s *st = s->state;
  FAR const struct ui_status_s *dev = &s->ui->status;
  char line[24];

  if (!dev->has_tel || dev->tel.tries == 0)
    {
      block_info_set(st->tries, UI_T_UNKNOWN);
      return;
    }

  snprintf(line, sizeof(line),
           st->stage == PIN_STAGE_PIN ? UI_T_TRIES_PIN : UI_T_TRIES_PUK,
           (unsigned int)dev->tel.tries);
  block_info_set(st->tries, line);
}

static void pin_stage(FAR struct screen_s *s, uint8_t stage)
{
  static FAR const char * const heads[] =
  {
    UI_T_SIM_PIN_HEAD, UI_T_SIM_PUK_HEAD, UI_T_SIM_NEW_PIN_HEAD, ""
  };

  FAR struct pin_scr_s *st = s->state;
  bool blocked = stage == PIN_STAGE_BLOCKED;

  st->stage = stage;
  pin_wipe(st);
  if (stage != PIN_STAGE_NEW)
    {
      explicit_bzero(st->puk, sizeof(st->puk));
    }

  block_set_text(st->head, heads[stage]);
  block_set_text(st->message, "");
  pin_show_dots(st);
  pin_show_tries(s);

  /* Blocked: nothing to type, only Skip */

  if (blocked != lv_obj_has_flag(st->dots, LV_OBJ_FLAG_HIDDEN))
    {
      lv_obj_delete(st->keys);
      st->keys = block_softkeys(s->root, "", blocked ? "" : UI_T_OK,
                                UI_T_SKIP);
      if (blocked)
        {
          lv_obj_add_flag(st->dots, LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(st->tries, LV_OBJ_FLAG_HIDDEN);
          block_set_text(st->message, UI_T_SIM_BLOCKED);
        }
      else
        {
          lv_obj_remove_flag(st->dots, LV_OBJ_FLAG_HIDDEN);
          lv_obj_remove_flag(st->tries, LV_OBJ_FLAG_HIDDEN);
        }
    }

  ui_typing(s, !blocked);
}

static FAR const char *pin_failure(int status, uint32_t code, bool puk)
{
  switch (code)
    {
      case PNUT_TELEPHONY_ERROR_CODE_REFUSED:
        return puk ? UI_T_WRONG_PUK : UI_T_WRONG_PIN;

      case PNUT_TELEPHONY_ERROR_CODE_NO_MODEM:
        return UI_T_TEL_NO_MODEM;

      case PNUT_TELEPHONY_ERROR_CODE_BUSY:
        return UI_T_TEL_BUSY;

      default:
        return status == PNUT_STATUS_TIMEOUT ||
               status == PNUT_STATUS_UNAVAILABLE ? UI_T_TEL_NO_ANSWER :
                                                   UI_T_TEL_FAILED;
    }
}

/* Telephony's answer, to either code; the SIM's state then comes on the
 * topic, which closes the screen once it is ready
 */

static void pin_answer(FAR struct pnut_telephony_client_s *client,
                       int status, FAR const pnut_error_t *err,
                       FAR void *arg)
{
  FAR struct ui_s *ui = (FAR struct ui_s *)((FAR char *)client -
                                            offsetof(struct ui_s, tel));
  FAR struct screen_s *s = ui_find(ui, (uint32_t)(uintptr_t)arg);
  FAR struct pin_scr_s *st;
  uint32_t code = err != NULL ? err->code : 0;

  if (s == NULL)
    {
      return;                     /* Gone: nothing to change */
    }

  st = s->state;
  st->asking = false;
  if (st->stage == PIN_STAGE_BLOCKED)
    {
      return;                     /* "SIM blocked" stays */
    }

  if (status == PNUT_STATUS_OK ||
      code == PNUT_TELEPHONY_ERROR_CODE_NOT_NEEDED)
    {
      block_set_text(st->message, "");
    }
  else
    {
      if (st->sent_puk && st->stage == PIN_STAGE_NEW)
        {
          pin_stage(s, PIN_STAGE_PUK);
        }

      block_set_text(st->message, pin_failure(status, code, st->sent_puk));
    }

  ui_place(ui);
}

static void pin_pin_done(FAR struct pnut_telephony_client_s *client,
                         int status, FAR const pnut_telephony_reply_t *out,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  pin_answer(client, status, err, arg);
}

static void pin_puk_done(FAR struct pnut_telephony_client_s *client,
                         int status, FAR const pnut_telephony_reply_t *out,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  pin_answer(client, status, err, arg);
}

/* Send the PIN, or the PUK with the new PIN; the code wiped either way */

static void pin_send(FAR struct screen_s *s)
{
  FAR struct pin_scr_s *st = s->state;
  FAR struct pnut_telephony_client_s *client = ui_telephony(s->ui);
  FAR void *token = (FAR void *)(uintptr_t)s->serial;
  pnut_telephony_pin_request_t pin;
  pnut_telephony_puk_request_t puk;
  int ret = -ENOTCONN;

  st->sent_puk = st->stage == PIN_STAGE_NEW;
  if (client != NULL && !st->sent_puk)
    {
      memset(&pin, 0, sizeof(pin));
      strlcpy(pin.pin, st->code, sizeof(pin.pin));
      ret = pnut_telephony_enter_pin(client, &pin, 0, pin_pin_done, token);
      explicit_bzero(&pin, sizeof(pin));
    }
  else if (client != NULL)
    {
      memset(&puk, 0, sizeof(puk));
      strlcpy(puk.puk, st->puk, sizeof(puk.puk));
      strlcpy(puk.pin, st->code, sizeof(puk.pin));
      ret = pnut_telephony_enter_puk(client, &puk, 0, pin_puk_done, token);
      explicit_bzero(&puk, sizeof(puk));
      explicit_bzero(st->puk, sizeof(st->puk));
    }

  pin_wipe(st);
  pin_show_dots(st);
  if (ret < 0)
    {
      if (st->sent_puk)
        {
          pin_stage(s, PIN_STAGE_PUK);
        }

      block_set_text(st->message, UI_T_TEL_NO_ANSWER);
      return;
    }

  st->asking = true;
  block_set_text(st->message, "");
}

static void pin_ok(FAR struct screen_s *s)
{
  FAR struct pin_scr_s *st = s->state;

  if (st->asking || st->stage == PIN_STAGE_BLOCKED)
    {
      return;
    }

  if (st->stage == PIN_STAGE_PUK)
    {
      if (st->len != PUK_LEN)
        {
          block_set_text(st->message, UI_T_PUK_LENGTH);
          return;
        }

      /* The PUK kept for the new PIN */

      memcpy(st->puk, st->code, sizeof(st->puk));
      pin_stage(s, PIN_STAGE_NEW);
      block_set_text(st->message, "");
      return;
    }

  if (st->len < PIN_MIN || st->len > PIN_MAX)
    {
      block_set_text(st->message, UI_T_PIN_LENGTH);
      return;
    }

  pin_send(s);
}

static void pin_build(FAR struct screen_s *s)
{
  FAR struct pin_scr_s *st = s->state;
  FAR lv_obj_t *box;
  FAR lv_obj_t *note;
  FAR const char *layout;

  ui_header(s, UI_ICON_CELLULAR, UI_T_SIM_PIN_TITLE);

  st->head = lv_label_create(s->root);
  theme_label(st->head, &g_ui_mono_bold_10);
  lv_label_set_text(st->head, "");
  lv_obj_set_pos(st->head, PIN_PAD, PIN_HEAD_Y);

  st->dots = lv_label_create(s->root);
  theme_label(st->dots, &g_ui_mono_bold_26);
  lv_label_set_text(st->dots, "");
  lv_obj_set_pos(st->dots, PIN_PAD, PIN_DOTS_Y);
  block_rule(s->root, 0, PIN_RULE_Y, 240, false);

  box = lv_obj_create(s->root);
  theme_box(box);
  lv_obj_set_pos(box, 0, PIN_LIST_Y);
  lv_obj_set_size(box, 240, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  st->tries = block_info(box, UI_T_TRIES_LEFT, UI_T_UNKNOWN);
  layout = s->ui->config.input != NULL ?
           input_layout(s->ui->config.input) : NULL;
  if (keymap_digit(layout, 'w') == '1')
    {
      st->hint = block_info(box, UI_T_LETTER_DIGITS, UI_T_LETTER_DIGITS_Q20);
    }

  st->message = lv_label_create(box);
  theme_label(st->message, &g_ui_sans_14);
  lv_label_set_long_mode(st->message, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(st->message, lv_pct(100));
  lv_obj_set_style_pad_hor(st->message, 8, 0);
  lv_obj_set_style_pad_ver(st->message, 6, 0);
  lv_label_set_text(st->message, "");

  note = lv_label_create(s->root);
  theme_label(note, &g_ui_sans_11);
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(note, 240 - 2 * 10);
  lv_label_set_text(note, UI_T_PIN_NOTE);
  lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, 10, -(BLOCK_FOOTER_H + 8));

  st->keys = block_softkeys(s->root, "", UI_T_OK, UI_T_SKIP);

  /* Telephony's client connected now, ready by the time a code is typed */

  ui_telephony(s->ui);
  pin_stage(s, s->ui->status.tel.sim == PNUT_TELEPHONY_INFO_SIM_NEEDS_PUK ?
               PIN_STAGE_PUK : PIN_STAGE_PIN);
}

static bool pin_key(FAR struct screen_s *s, uint8_t action, uint32_t ch)
{
  FAR struct pin_scr_s *st = s->state;
  FAR const char *layout;
  uint32_t digit;

  switch (action)
    {
      case UI_ACT_CHAR:
        layout = s->ui->config.input != NULL ?
                 input_layout(s->ui->config.input) : NULL;
        digit = keymap_digit(layout, ch);
        if (st->stage != PIN_STAGE_BLOCKED && !st->asking &&
            digit >= '0' && digit <= '9' && st->len < PIN_MAX)
          {
            st->code[st->len++] = (char)digit;
            pin_show_dots(st);
            block_set_text(st->message, "");
          }

        return true;

      case UI_ACT_DELETE:
        if (st->len > 0)
          {
            st->code[--st->len] = '\0';
            pin_show_dots(st);
          }

        return true;

      case UI_ACT_OPEN:
      case UI_ACT_CONFIRM:
      case UI_ACT_SOFT_MIDDLE:
        pin_ok(s);
        return true;

      case UI_ACT_BACK:
      case UI_ACT_SOFT_RIGHT:
        s->ui->pin_skipped = true;
        ui_pop(s->ui);
        return true;

      default:
        return true;              /* Nothing else leaves it */
    }
}

static void pin_status(FAR struct screen_s *s, uint8_t what)
{
  FAR struct pin_scr_s *st = s->state;
  FAR const pnut_telephony_info_t *t = &s->ui->status.tel;

  if ((what & STATUS_TEL) == 0)
    {
      return;
    }

  if (t->modem != PNUT_TELEPHONY_INFO_MODEM_ON ||
      (t->sim != PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN &&
       t->sim != PNUT_TELEPHONY_INFO_SIM_NEEDS_PUK &&
       t->sim != PNUT_TELEPHONY_INFO_SIM_BLOCKED))
    {
      /* Ready, or no SIM to ask for: the screen goes */

      if (s == ui_top(s->ui))
        {
          ui_pop(s->ui);
        }

      return;
    }

  if (t->sim == PNUT_TELEPHONY_INFO_SIM_BLOCKED)
    {
      if (st->stage != PIN_STAGE_BLOCKED)
        {
          pin_stage(s, PIN_STAGE_BLOCKED);
        }
    }
  else if (t->sim == PNUT_TELEPHONY_INFO_SIM_NEEDS_PUK)
    {
      if (st->stage == PIN_STAGE_PIN)
        {
          /* The PIN's last try refused: said, as the PUK is asked */

          pin_stage(s, PIN_STAGE_PUK);
          block_set_text(st->message, UI_T_WRONG_PIN);
        }
      else if (st->stage == PIN_STAGE_BLOCKED)
        {
          pin_stage(s, PIN_STAGE_PUK);
        }
    }
  else if (st->stage != PIN_STAGE_PIN)
    {
      pin_stage(s, PIN_STAGE_PIN);
    }

  pin_show_tries(s);
}

static void pin_leave(FAR struct screen_s *s)
{
  FAR struct pin_scr_s *st = s->state;

  pin_wipe(st);
  explicit_bzero(st->puk, sizeof(st->puk));
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct screen_class_s g_scr_pin =
{
  "pin",
  pin_build,
  pin_key,
  NULL,
  pin_status,
  pin_leave,
  sizeof(struct pin_scr_s)
};
