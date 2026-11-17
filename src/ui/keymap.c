/****************************************************************************
 * pnut-os/src/ui/keymap.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* What keys mean in the system UI: RFC 0019's suggested map.
 *
 *   W S A D      up, down, left, right; repeat when held
 *   Q, E         back, the primary action
 *   Backspace    back; while typing, deletes (never back)
 *   Enter        the primary action; while typing, confirms the field
 *   Alt Enter    the options, while typing too
 *   Space        the next page; with Shift, the previous one
 *   Find held    W S A D steer, Q back, E the primary action, while typing
 *                too (Q leaves the field, its text kept); no cursor yet
 *   F1 F2 F3     the function keys: options, primary action, back
 *   A letter     another, a command letter, lowercase (RFC 0037): the
 *                options open choose by it
 *
 * The driver gives characters as printed, its layers already applied, and
 * reports Shift and Alt while they are in effect (RFC 0024); the letters
 * that steer are never given to commands.  Repeat (RFC 0027): the steering
 * keys, Space and Backspace, after half a second, then 20 times a second;
 * a letter being typed does not repeat.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <string.h>

#include "keymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The keyboard whose letter keys carry digits (Board's layout name) */

#define KEYMAP_Q20  "blackberry-q20"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void keymap_set(FAR struct keymap_result_s *out, uint8_t action,
                       bool repeats)
{
  out->action  = action;
  out->repeats = repeats;
}

static void keymap_special(FAR struct keymap_state_s *s, uint32_t code,
                           bool press, FAR struct keymap_result_s *out)
{
  switch (code)
    {
      case KEYMAP_SHIFT:
        s->shift = press;
        return;

      case KEYMAP_ALT:
        s->alt = press;
        return;

      case KEYMAP_FIND:
        s->find = press;
        return;
    }

  if (!press)
    {
      return;
    }

  switch (code)
    {
      case KEYMAP_F1:
        keymap_set(out, UI_ACT_SOFT_LEFT, false);
        break;

      case KEYMAP_F2:
        keymap_set(out, UI_ACT_SOFT_MIDDLE, false);
        break;

      case KEYMAP_F3:
        keymap_set(out, UI_ACT_SOFT_RIGHT, false);
        break;

      case KEYMAP_ENTER:
        keymap_set(out, s->alt ? UI_ACT_OPTIONS :
                        s->typing ? UI_ACT_CONFIRM : UI_ACT_OPEN, false);
        break;

      case KEYMAP_BACKSPACE:
        keymap_set(out, s->typing ? UI_ACT_DELETE : UI_ACT_BACK, true);
        break;
    }
}

/* A steering letter, as a lowercase one, outside a text field or with Find
 * held: its action
 */

static void keymap_steer(FAR struct keymap_state_s *s, uint32_t lower,
                         FAR struct keymap_result_s *out)
{
  switch (lower)
    {
      case 'w':
        keymap_set(out, UI_ACT_UP, true);
        break;

      case 's':
        keymap_set(out, UI_ACT_DOWN, true);
        break;

      case 'a':
        keymap_set(out, UI_ACT_LEFT, true);
        break;

      case 'd':
        keymap_set(out, UI_ACT_RIGHT, true);
        break;

      case 'q':
        keymap_set(out, UI_ACT_BACK, false);
        break;

      case 'e':
        keymap_set(out, UI_ACT_OPEN, false);
        break;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void keymap_key(FAR struct keymap_state_s *s, uint32_t code, bool special,
                bool press, FAR struct keymap_result_s *out)
{
  uint32_t lower;

  memset(out, 0, sizeof(*out));
  if (special)
    {
      keymap_special(s, code, press, out);
      return;
    }

  if (!press)
    {
      return;
    }

  lower = code >= 'A' && code <= 'Z' ? code - 'A' + 'a' : code;
  if (s->typing && s->find)
    {
      /* No cursor yet: left and right mean nothing */

      if (lower != 'a' && lower != 'd')
        {
          keymap_steer(s, lower, out);
          if (out->action == UI_ACT_BACK)
            {
              out->action = UI_ACT_LEAVE_FIELD;
            }
        }
    }
  else if (s->typing)
    {
      out->action  = UI_ACT_CHAR;
      out->ch      = code;
      out->repeats = code == ' ';
    }
  else if (code == ' ')
    {
      keymap_set(out, s->shift ? UI_ACT_PAGE_PREV : UI_ACT_PAGE_NEXT, true);
    }
  else
    {
      keymap_steer(s, lower, out);

      /* Another letter: a command letter of the options open (RFC 0037) */

      if (out->action == UI_ACT_NONE && lower >= 'a' && lower <= 'z')
        {
          out->action = UI_ACT_CHAR;
          out->ch     = lower;
        }
    }
}

uint32_t keymap_digit(FAR const char *layout, uint32_t ch)
{
  static const char letters[] = "wersdfzxc";
  FAR const char *at;

  if (ch >= '0' && ch <= '9')
    {
      return ch;
    }

  if (layout == NULL || strcmp(layout, KEYMAP_Q20) != 0)
    {
      return 0;
    }

  if (ch >= 'A' && ch <= 'Z')
    {
      ch = ch - 'A' + 'a';
    }

  at = ch != 0 && ch < 0x80 ? strchr(letters, (int)ch) : NULL;
  return at != NULL ? '1' + (uint32_t)(at - letters) : 0;
}

void keyrepeat_press(FAR struct keyrepeat_s *k, uint64_t now,
                     FAR const struct keymap_result_s *r)
{
  k->held   = r->repeats;
  k->action = r->action;
  k->ch     = r->ch;
  k->due    = now + KEYMAP_DELAY_MS;
}

void keyrepeat_release(FAR struct keyrepeat_s *k)
{
  k->held = false;
}

bool keyrepeat_fire(FAR struct keyrepeat_s *k, uint64_t now)
{
  if (!k->held || now < k->due)
    {
      return false;
    }

  /* Late (the loop was busy): no burst to catch up */

  k->due += KEYMAP_PERIOD_MS;
  if (k->due <= now)
    {
      k->due = now + KEYMAP_PERIOD_MS;
    }

  return true;
}
