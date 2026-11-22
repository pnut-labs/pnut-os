/****************************************************************************
 * pnut-os/src/ui/keymap.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_KEYMAP_H
#define __PNUT_OS_UI_KEYMAP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/compiler.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* An action as LVGL's key code: above Unicode, so never a character */

#define UI_KEY(a)          (0x110000u + (a))
#define UI_KEY_ACTION(k)   ((k) - 0x110000u)
#define UI_KEY_IS_ACTION(k) ((k) >= 0x110000u)

/* Held keys repeat after this long, then this often (RFC 0027) */

#define KEYMAP_DELAY_MS    500
#define KEYMAP_PERIOD_MS   50

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The keys that are not characters, as Input gives them (input_dev.c
 * turns NuttX's into these)
 */

enum keymap_special_e
{
  KEYMAP_ENTER = 1,
  KEYMAP_BACKSPACE,
  KEYMAP_FIND,                    /* Find's role: Sym on a Q20 keyboard */
  KEYMAP_SHIFT,                   /* In effect, held, tapped or locked */
  KEYMAP_ALT,
  KEYMAP_F1,                      /* The function keys, left to right */
  KEYMAP_F2,
  KEYMAP_F3
};

/* What a key means on screen (RFC 0019) */

enum ui_action_e
{
  UI_ACT_NONE = 0,
  UI_ACT_UP,
  UI_ACT_DOWN,
  UI_ACT_LEFT,
  UI_ACT_RIGHT,
  UI_ACT_BACK,
  UI_ACT_OPEN,                    /* The primary action */
  UI_ACT_OPTIONS,
  UI_ACT_PAGE_NEXT,
  UI_ACT_PAGE_PREV,
  UI_ACT_SOFT_LEFT,               /* The function keys */
  UI_ACT_SOFT_MIDDLE,
  UI_ACT_SOFT_RIGHT,
  UI_ACT_CHAR,                    /* A character typed */
  UI_ACT_DELETE,
  UI_ACT_CONFIRM,                 /* Enter in a one-line field */
  UI_ACT_LEAVE_FIELD,             /* Find and Q: its text kept */
  UI_ACT_HOME                     /* Alt and Backspace held outside a
                                   * text field (RFC 0037) */
};

/* The modifiers in effect, Find held, and whether a text field has the
 * keys
 */

struct keymap_state_s
{
  bool shift;
  bool alt;
  bool find;
  bool typing;
};

struct keymap_result_s
{
  uint8_t action;
  uint32_t ch;                    /* UI_ACT_CHAR's */
  bool repeats;                   /* Repeats while held */
  uint8_t hold;                   /* Sent instead, once, when held: action
                                   * is then sent when let go sooner */
};

/* A key held, repeating */

struct keyrepeat_s
{
  uint64_t due;
  uint8_t action;
  uint32_t ch;
  bool held;
  uint8_t hold;                   /* A key's hold action, still owed */
  uint8_t owed;                   /* ... and its short press's */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* A key pressed or released: a character, or with special set, a
 * keymap_special_e.  A release, and a modifier, mean nothing on screen
 * (UI_ACT_NONE); they change the state.
 */

void keymap_key(FAR struct keymap_state_s *s, uint32_t code, bool special,
                bool press, FAR struct keymap_result_s *out);

/* The digit a letter key carries on a keyboard layout (Board's), for a
 * field of digits; a digit stays itself; 0 for none
 */

uint32_t keymap_digit(FAR const char *layout, uint32_t ch);

/* A key's repeat: pressed at now, released; whether it repeats at now.
 * A key with a hold action fires once, held KEYMAP_DELAY_MS, with action
 * set to it; let go sooner, release gives its short press's action, owed
 * (UI_ACT_NONE otherwise).
 */

void keyrepeat_press(FAR struct keyrepeat_s *k, uint64_t now,
                     FAR const struct keymap_result_s *r);
uint8_t keyrepeat_release(FAR struct keyrepeat_s *k);
bool keyrepeat_fire(FAR struct keyrepeat_s *k, uint64_t now);

#endif /* __PNUT_OS_UI_KEYMAP_H */
