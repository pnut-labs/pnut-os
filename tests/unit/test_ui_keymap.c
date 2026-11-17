/****************************************************************************
 * pnut-os/tests/unit/test_ui_keymap.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* What keys mean in the system UI (keymap.c): RFC 0019's suggested map,
 * outside a text field and while typing, chords with Alt, Shift and Find,
 * and which keys repeat when held (RFC 0027).
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "keymap.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* A key's press: its action, and whether it repeats */

static void press(struct keymap_state_s *s, uint32_t code, bool special,
                  uint8_t action, bool repeats)
{
  struct keymap_result_s r;

  keymap_key(s, code, special, true, &r);
  assert_int_equal(r.action, action);
  assert_int_equal(r.repeats, repeats);
  keymap_key(s, code, special, false, &r);
  assert_int_equal(r.action, UI_ACT_NONE);
}

static void hold(struct keymap_state_s *s, uint32_t code, bool on)
{
  struct keymap_result_s r;

  keymap_key(s, code, true, on, &r);
  assert_int_equal(r.action, UI_ACT_NONE);
}

/* A letter that does not steer, outside a text field: a command letter
 * (RFC 0037), lowercase; anything else is nothing
 */

static void test_command_letters(void **state)
{
  struct keymap_state_s s;
  struct keymap_result_s r;

  memset(&s, 0, sizeof(s));
  keymap_key(&s, 'x', false, true, &r);
  assert_int_equal(r.action, UI_ACT_CHAR);
  assert_int_equal(r.ch, 'x');
  assert_false(r.repeats);

  keymap_key(&s, 'C', false, true, &r);
  assert_int_equal(r.action, UI_ACT_CHAR);
  assert_int_equal(r.ch, 'c');

  press(&s, '1', false, UI_ACT_NONE, false);
  press(&s, '.', false, UI_ACT_NONE, false);
}

static void test_steering(void **state)
{
  struct keymap_state_s s;

  memset(&s, 0, sizeof(s));
  press(&s, 'w', false, UI_ACT_UP, true);
  press(&s, 's', false, UI_ACT_DOWN, true);
  press(&s, 'a', false, UI_ACT_LEFT, true);
  press(&s, 'd', false, UI_ACT_RIGHT, true);
  press(&s, 'q', false, UI_ACT_BACK, false);
  press(&s, KEYMAP_BACKSPACE, true, UI_ACT_BACK, true);
  press(&s, 'e', false, UI_ACT_OPEN, false);
  press(&s, KEYMAP_ENTER, true, UI_ACT_OPEN, false);
  press(&s, ' ', false, UI_ACT_PAGE_NEXT, true);
  press(&s, KEYMAP_F1, true, UI_ACT_SOFT_LEFT, false);
  press(&s, KEYMAP_F2, true, UI_ACT_SOFT_MIDDLE, false);
  press(&s, KEYMAP_F3, true, UI_ACT_SOFT_RIGHT, false);

  /* Shifted letters steer as their own */

  press(&s, 'W', false, UI_ACT_UP, true);

  hold(&s, KEYMAP_SHIFT, true);
  press(&s, ' ', false, UI_ACT_PAGE_PREV, true);
  hold(&s, KEYMAP_SHIFT, false);
  press(&s, ' ', false, UI_ACT_PAGE_NEXT, true);
}

static void test_alt_enter(void **state)
{
  struct keymap_state_s s;

  memset(&s, 0, sizeof(s));
  hold(&s, KEYMAP_ALT, true);
  press(&s, KEYMAP_ENTER, true, UI_ACT_OPTIONS, false);
  hold(&s, KEYMAP_ALT, false);
  press(&s, KEYMAP_ENTER, true, UI_ACT_OPEN, false);

  /* In a text field too */

  s.typing = true;
  hold(&s, KEYMAP_ALT, true);
  press(&s, KEYMAP_ENTER, true, UI_ACT_OPTIONS, false);
  hold(&s, KEYMAP_ALT, false);
}

static void test_typing(void **state)
{
  struct keymap_state_s s;
  struct keymap_result_s r;

  memset(&s, 0, sizeof(s));
  s.typing = true;

  keymap_key(&s, 'w', false, true, &r);
  assert_int_equal(r.action, UI_ACT_CHAR);
  assert_int_equal(r.ch, 'w');
  assert_false(r.repeats);
  keymap_key(&s, 0x0105, false, true, &r);      /* ą, from a layout */
  assert_int_equal(r.action, UI_ACT_CHAR);
  assert_int_equal(r.ch, 0x0105);

  keymap_key(&s, ' ', false, true, &r);
  assert_int_equal(r.action, UI_ACT_CHAR);
  assert_int_equal(r.ch, ' ');
  assert_true(r.repeats);

  press(&s, KEYMAP_BACKSPACE, true, UI_ACT_DELETE, true);
  press(&s, KEYMAP_ENTER, true, UI_ACT_CONFIRM, false);
  press(&s, KEYMAP_F3, true, UI_ACT_SOFT_RIGHT, false);

  /* Find held: leave, the primary action, steering; no cursor yet */

  hold(&s, KEYMAP_FIND, true);
  press(&s, 'q', false, UI_ACT_LEAVE_FIELD, false);
  press(&s, 'e', false, UI_ACT_OPEN, false);
  press(&s, 's', false, UI_ACT_DOWN, true);
  press(&s, 'w', false, UI_ACT_UP, true);
  press(&s, 'a', false, UI_ACT_NONE, false);
  press(&s, 'd', false, UI_ACT_NONE, false);
  hold(&s, KEYMAP_FIND, false);

  keymap_key(&s, 'q', false, true, &r);
  assert_int_equal(r.action, UI_ACT_CHAR);
  assert_int_equal(r.ch, 'q');
}

static void test_find_steers(void **state)
{
  struct keymap_state_s s;

  memset(&s, 0, sizeof(s));
  hold(&s, KEYMAP_FIND, true);
  press(&s, 'w', false, UI_ACT_UP, true);
  press(&s, 'q', false, UI_ACT_BACK, false);
  hold(&s, KEYMAP_FIND, false);
}

static void test_digits(void **state)
{
  const char *letters = "wersdfzxc0";
  const char *digits  = "1234567890";
  int i;

  for (i = 0; letters[i] != '\0'; i++)
    {
      assert_int_equal(keymap_digit("blackberry-q20", letters[i]),
                       digits[i]);
      assert_int_equal(keymap_digit("blackberry-q20",
                                    letters[i] - 'a' + 'A'),
                       letters[i] == '0' ? 0 : digits[i]);
    }

  assert_int_equal(keymap_digit("blackberry-q20", '1'), '1');
  assert_int_equal(keymap_digit("blackberry-q20", 'q'), 0);
  assert_int_equal(keymap_digit("other", 'w'), 0);
  assert_int_equal(keymap_digit("other", '7'), '7');
  assert_int_equal(keymap_digit(NULL, 'w'), 0);
}

static void test_repeat(void **state)
{
  struct keymap_state_s s;
  struct keymap_result_s r;
  struct keyrepeat_s k;

  memset(&s, 0, sizeof(s));
  memset(&k, 0, sizeof(k));

  keymap_key(&s, 'w', false, true, &r);
  keyrepeat_press(&k, 0, &r);
  assert_false(keyrepeat_fire(&k, 499));
  assert_true(keyrepeat_fire(&k, 500));
  assert_int_equal(k.action, UI_ACT_UP);
  assert_false(keyrepeat_fire(&k, 549));
  assert_true(keyrepeat_fire(&k, 550));
  assert_false(keyrepeat_fire(&k, 560));
  assert_true(keyrepeat_fire(&k, 600));

  keyrepeat_release(&k);
  assert_false(keyrepeat_fire(&k, 1000));

  /* A key that does not repeat never fires */

  keymap_key(&s, 'q', false, true, &r);
  keyrepeat_press(&k, 2000, &r);
  assert_false(keyrepeat_fire(&k, 3000));
}

/* Home is Alt and Backspace held (RFC 0037): Back, or Delete in a field,
 * on a short press, sent when the key is let go; Home once held 0.5 s,
 * once; Backspace alone is unchanged
 */

static void test_home_chord(void **state)
{
  struct keymap_state_s s;
  struct keymap_result_s r;
  struct keyrepeat_s k;

  memset(&s, 0, sizeof(s));
  memset(&k, 0, sizeof(k));

  keymap_key(&s, KEYMAP_BACKSPACE, true, true, &r);
  assert_int_equal(r.action, UI_ACT_BACK);
  assert_int_equal(r.hold, UI_ACT_NONE);
  assert_true(r.repeats);
  keymap_key(&s, KEYMAP_BACKSPACE, true, false, &r);

  hold(&s, KEYMAP_ALT, true);
  keymap_key(&s, KEYMAP_BACKSPACE, true, true, &r);
  assert_int_equal(r.action, UI_ACT_BACK);
  assert_int_equal(r.hold, UI_ACT_HOME);
  assert_false(r.repeats);

  keyrepeat_press(&k, 0, &r);
  assert_false(keyrepeat_fire(&k, 499));
  assert_true(keyrepeat_fire(&k, 500));
  assert_int_equal(k.action, UI_ACT_HOME);
  assert_false(keyrepeat_fire(&k, 1000));
  assert_int_equal(keyrepeat_release(&k), UI_ACT_NONE);

  /* Let go before: the short press's action, owed */

  keyrepeat_press(&k, 2000, &r);
  assert_false(keyrepeat_fire(&k, 2100));
  assert_int_equal(keyrepeat_release(&k), UI_ACT_BACK);
  assert_false(keyrepeat_fire(&k, 3000));

  /* In a text field */

  s.typing = true;
  keymap_key(&s, KEYMAP_BACKSPACE, true, true, &r);
  assert_int_equal(r.action, UI_ACT_DELETE);
  assert_int_equal(r.hold, UI_ACT_HOME);
  hold(&s, KEYMAP_ALT, false);

  /* A repeating key owes nothing when let go */

  keymap_key(&s, KEYMAP_BACKSPACE, true, true, &r);
  keyrepeat_press(&k, 4000, &r);
  assert_int_equal(keyrepeat_release(&k), UI_ACT_NONE);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_steering),
    cmocka_unit_test(test_command_letters),
    cmocka_unit_test(test_alt_enter),
    cmocka_unit_test(test_typing),
    cmocka_unit_test(test_find_steers),
    cmocka_unit_test(test_digits),
    cmocka_unit_test(test_repeat),
    cmocka_unit_test(test_home_chord),
  };

  return cmocka_run_group_tests_name("ui_keymap", tests, NULL, NULL);
}
