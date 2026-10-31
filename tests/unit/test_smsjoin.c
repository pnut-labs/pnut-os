/****************************************************************************
 * pnut-os/tests/unit/test_smsjoin.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* A long SMS's parts joined (RFC 0030): in any order, once each; long
 * messages apart by sender and reference; a day's wait, then delivered
 * with a mark for each gap; the key that keeps a message stored once.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "smsjoin.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sj_part(FAR struct sms_deliver_s *d, FAR const char *sender,
                    uint16_t ref, uint8_t total, uint8_t part,
                    FAR const char *text)
{
  memset(d, 0, sizeof(*d));
  strlcpy(d->sender, sender, sizeof(d->sender));
  strlcpy(d->text, text, sizeof(d->text));
  d->ref   = ref;
  d->total = total;
  d->part  = part;
  d->sent  = 1793363696 + part;
}

/****************************************************************************
 * Name: test_smsjoin_parts
 *
 * Description:
 *   A whole message at once; a long one in any order, a part read twice
 *   counted once, joined in order with its slots and its first part's
 *   time stamp; two long messages kept apart.
 *
 ****************************************************************************/

static void test_smsjoin_parts(void **state)
{
  FAR struct smsjoin_s *j = calloc(1, sizeof(*j));
  FAR struct smsjoin_msg_s *m = calloc(1, sizeof(*m));
  struct sms_deliver_s d;
  char key[SMSJOIN_KEY + 1];
  size_t i;

  assert_non_null(j);
  assert_non_null(m);

  sj_part(&d, "+48123456789", 0, 0, 0, "whole");
  assert_int_equal(smsjoin_add(j, &d, 4, 0, m), 1);
  assert_string_equal(m->text, "whole");
  assert_int_equal(m->nslots, 1);
  assert_int_equal(m->slots[0], 4);
  assert_int_equal(strlen(m->key), SMSJOIN_KEY);
  for (i = 0; i < SMSJOIN_KEY; i++)
    {
      assert_non_null(strchr("0123456789abcdef", m->key[i]));
    }

  smsjoin_free(m);

  /* Parts 2, 1 (twice), 3 of message 7, and another sender's message 7
   * between them
   */

  sj_part(&d, "+48111", 7, 3, 2, "world, ");
  assert_int_equal(smsjoin_add(j, &d, 11, 10, m), 0);
  sj_part(&d, "+48222", 7, 2, 1, "other ");
  assert_int_equal(smsjoin_add(j, &d, 20, 10, m), 0);
  sj_part(&d, "+48111", 7, 3, 1, "Hello ");
  assert_int_equal(smsjoin_add(j, &d, 10, 11, m), 0);
  assert_int_equal(smsjoin_add(j, &d, 10, 12, m), 0);
  sj_part(&d, "+48111", 7, 3, 3, "again");
  assert_int_equal(smsjoin_add(j, &d, 12, 13, m), 1);
  assert_string_equal(m->sender, "+48111");
  assert_string_equal(m->text, "Hello world, again");
  assert_false(m->gaps);
  assert_int_equal(m->nslots, 3);
  assert_int_equal(m->slots[0], 10);
  assert_int_equal(m->slots[1], 11);
  assert_int_equal(m->slots[2], 12);
  assert_int_equal(m->sent, 1793363696 + 1);
  smsjoin_free(m);

  sj_part(&d, "+48222", 7, 2, 2, "message");
  assert_int_equal(smsjoin_add(j, &d, 21, 14, m), 1);
  assert_string_equal(m->text, "other message");
  smsjoin_free(m);

  /* A part beyond its message's parts, or numbered 0 */

  sj_part(&d, "+48111", 8, 3, 4, "x");
  assert_int_equal(smsjoin_add(j, &d, 30, 15, m), -EINVAL);
  sj_part(&d, "+48111", 8, 3, 0, "x");
  assert_int_equal(smsjoin_add(j, &d, 30, 15, m), -EINVAL);

  /* A message of more parts than the join takes: each part on its own,
   * marked as a part, with keys apart
   */

  sj_part(&d, "+48111", 9, SMSJOIN_PARTS + 1, 1, "first");
  assert_int_equal(smsjoin_add(j, &d, 31, 16, m), 1);
  assert_string_equal(m->text, "first");
  assert_true(m->gaps);
  strlcpy(key, m->key, sizeof(key));
  smsjoin_free(m);
  sj_part(&d, "+48111", 9, SMSJOIN_PARTS + 1, 2, "first");
  d.sent = 1793363696 + 1;
  assert_int_equal(smsjoin_add(j, &d, 32, 16, m), 1);
  assert_string_not_equal(m->key, key);
  smsjoin_free(m);

  smsjoin_clear(j);
  free(m);
  free(j);
}

/****************************************************************************
 * Name: test_smsjoin_copies
 *
 * Description:
 *   The same part again from another slot is a copy, its slot to delete;
 *   another text under the same reference goes on its own; an 8-bit and
 *   a 16-bit reference of the same value are two messages.
 *
 ****************************************************************************/

static void test_smsjoin_copies(void **state)
{
  FAR struct smsjoin_s *j = calloc(1, sizeof(*j));
  FAR struct smsjoin_msg_s *m = calloc(1, sizeof(*m));
  struct sms_deliver_s d;

  assert_non_null(j);
  assert_non_null(m);

  sj_part(&d, "+48111", 3, 2, 1, "one ");
  assert_int_equal(smsjoin_add(j, &d, 10, 0, m), 0);
  assert_int_equal(smsjoin_add(j, &d, 10, 0, m), 0);       /* Re-read */
  assert_int_equal(smsjoin_add(j, &d, 11, 0, m), SMSJOIN_COPY);

  /* A gateway's reference used again: another first part, alone */

  sj_part(&d, "+48111", 3, 2, 1, "other ");
  assert_int_equal(smsjoin_add(j, &d, 12, 0, m), 1);
  assert_string_equal(m->text, "other ");
  assert_true(m->gaps);
  assert_int_equal(m->slots[0], 12);
  smsjoin_free(m);

  /* A 16-bit reference of the same value: not this message's */

  sj_part(&d, "+48111", 3, 2, 2, "sixteen");
  d.ref16 = true;
  assert_int_equal(smsjoin_add(j, &d, 13, 0, m), 0);
  sj_part(&d, "+48111", 3, 2, 2, "two");
  assert_int_equal(smsjoin_add(j, &d, 14, 0, m), 1);
  assert_string_equal(m->text, "one two");
  assert_int_equal(m->nslots, 2);
  smsjoin_free(m);

  /* A clock that went back expires nothing */

  assert_int_equal(smsjoin_expired(j, 0, m), 0);
  sj_part(&d, "+48222", 4, 2, 1, "late");
  assert_int_equal(smsjoin_add(j, &d, 15, 1000, m), 0);
  assert_int_equal(smsjoin_expired(j, 999, m), 0);

  smsjoin_clear(j);
  free(m);
  free(j);
}

/****************************************************************************
 * Name: test_smsjoin_wait
 *
 * Description:
 *   After a day, a long message delivered with a mark where its part is
 *   missing; with SMSJOIN_MESSAGES joined, the oldest given up for a new
 *   one.
 *
 ****************************************************************************/

static void test_smsjoin_wait(void **state)
{
  FAR struct smsjoin_s *j = calloc(1, sizeof(*j));
  FAR struct smsjoin_msg_s *m = calloc(1, sizeof(*m));
  struct sms_deliver_s d;
  int i;

  assert_non_null(j);
  assert_non_null(m);

  sj_part(&d, "+48111", 1, 3, 1, "first ");
  assert_int_equal(smsjoin_add(j, &d, 1, 100, m), 0);
  sj_part(&d, "+48111", 1, 3, 3, " last");
  assert_int_equal(smsjoin_add(j, &d, 3, 200, m), 0);

  for (i = 2; i <= SMSJOIN_MESSAGES; i++)
    {
      sj_part(&d, "+48333", (uint16_t)i, 2, 1, "waiting");
      assert_int_equal(smsjoin_add(j, &d, (int16_t)(10 + i), 300, m), 0);
    }

  /* Not yet a day: none */

  assert_int_equal(smsjoin_expired(j, 100 + SMSJOIN_WAIT - 1, m), 0);

  /* No room for another: the one that has waited longest goes, with its
   * gap, and the new part takes its place; parts that never complete
   * cannot keep long messages out
   */

  sj_part(&d, "+48444", 99, 2, 1, "no room ");
  assert_int_equal(smsjoin_add(j, &d, 50, 400, m), SMSJOIN_EVICTED);
  assert_string_equal(m->text, "first " SMSJOIN_GAP " last");
  assert_true(m->gaps);
  assert_int_equal(m->nslots, 2);
  assert_int_equal(m->slots[0], 1);
  assert_int_equal(m->slots[1], 3);
  smsjoin_free(m);

  sj_part(&d, "+48444", 99, 2, 2, "after all");
  assert_int_equal(smsjoin_add(j, &d, 51, 401, m), 1);
  assert_string_equal(m->text, "no room after all");
  smsjoin_free(m);

  /* Full again, every one in the same second: the one that came first
   * goes
   */

  sj_part(&d, "+48555", 1, 2, 1, "newer");
  assert_int_equal(smsjoin_add(j, &d, 60, 300, m), 0);
  sj_part(&d, "+48666", 1, 2, 1, "newest");
  assert_int_equal(smsjoin_add(j, &d, 61, 300, m), SMSJOIN_EVICTED);
  assert_string_equal(m->text, "waiting" SMSJOIN_GAP);
  assert_int_equal(m->slots[0], 12);
  smsjoin_free(m);

  /* The others, a day after they came */

  assert_int_equal(smsjoin_expired(j, 300 + SMSJOIN_WAIT, m), 1);
  smsjoin_free(m);

  smsjoin_clear(j);
  free(m);
  free(j);
}

/****************************************************************************
 * Name: test_smsjoin_key
 *
 * Description:
 *   The same message makes the same key; another sender, time stamp,
 *   reference or text, another.
 *
 ****************************************************************************/

static void test_smsjoin_key(void **state)
{
  char a[SMSJOIN_KEY + 1];
  char b[SMSJOIN_KEY + 1];

  smsjoin_key("+48111", 1000, 7, 0, "text", a);
  smsjoin_key("+48111", 1000, 7, 0, "text", b);
  assert_string_equal(a, b);
  smsjoin_key("+48112", 1000, 7, 0, "text", b);
  assert_string_not_equal(a, b);
  smsjoin_key("+48111", 1001, 7, 0, "text", b);
  assert_string_not_equal(a, b);
  smsjoin_key("+48111", 1000, 8, 0, "text", b);
  assert_string_not_equal(a, b);
  smsjoin_key("+48111", 1000, 7, 0, "texu", b);
  assert_string_not_equal(a, b);

  /* A part on its own: its number apart from its reference */

  smsjoin_key("+48111", 1000, 0x100, 1, "text", a);
  smsjoin_key("+48111", 1000, 0x200, 2, "text", b);
  assert_string_not_equal(a, b);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_smsjoin_parts),
    cmocka_unit_test(test_smsjoin_copies),
    cmocka_unit_test(test_smsjoin_wait),
    cmocka_unit_test(test_smsjoin_key),
  };

  return cmocka_run_group_tests_name("smsjoin", tests, NULL, NULL);
}
