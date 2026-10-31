/****************************************************************************
 * pnut-os/tests/unit/test_sms.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Telephony's SMS codec (RFC 0030): SMS-DELIVER decoded in GSM 7-bit,
 * UCS-2, from a name, and as a long message's part; SMS-SUBMIT encoded,
 * in parts when long, never splitting a character.
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
#include <string.h>

#include <cmocka.h>

#include "parse.h"
#include "sms.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The time stamp of the vectors built here: 2026-10-30 12:03:00, +01:00 */

#define S_SCTS    "62010321300040"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* A SUBMIT PDU's user data decoded, as the receiving phone would */

static void sms_test_back(FAR const struct sms_part_s *part,
                          FAR struct sms_deliver_s *out)
{
  uint8_t pdu[1 + SMS_TPDU_MAX];
  size_t n = strlen(part->hex) / 2;
  size_t pos;
  size_t i;
  uint8_t first;
  unsigned int v;

  for (i = 0; i < n; i++)
    {
      sscanf(part->hex + 2 * i, "%2x", &v);
      pdu[i] = (uint8_t)v;
    }

  assert_int_equal(pdu[0], 0);                    /* The SIM's SMSC */
  assert_int_equal(part->tpdu, n - 1);
  first = pdu[1];
  assert_int_equal(first & 0x03, 0x01);           /* SMS-SUBMIT */
  pos = 3;                                        /* After TP-MR */
  pos += 2 + (pdu[pos] + 1) / 2;                  /* TP-DA */
  pos += 2;                                       /* TP-PID, TP-DCS */
  assert_int_equal(sms_user_data(pdu + pos + 1, n - pos - 1, pdu[pos],
                                 pdu[pos - 1], (first & 0x40) != 0, out),
                   0);
}

/* A whole text back from its parts, each decoded as the receiving phone
 * would; the parts in all
 */

static int sms_test_all(FAR const char *text, FAR char *back, size_t size,
                        FAR bool *ucs2)
{
  struct sms_part_s part;
  struct sms_deliver_s d;
  int n;
  int k;

  n = sms_parts(text, ucs2);
  back[0] = '\0';
  for (k = 1; k <= n; k++)
    {
      assert_int_equal(sms_encode("+48123456789", text, 5, false,
                                  (uint8_t)k, &part), n);
      sms_test_back(&part, &d);
      if (n > 1)
        {
          assert_int_equal(d.total, n);
          assert_int_equal(d.part, k);
          assert_int_equal(d.ref, 5);
        }

      strlcat(back, d.text, size);
    }

  return n;
}

/****************************************************************************
 * Name: test_sms_deliver
 *
 * Description:
 *   SMS-DELIVER decoded: the classic GSM 7-bit example, UCS-2 with Polish
 *   letters, a name for its sender, and a long message's part.
 *
 ****************************************************************************/

static void test_sms_deliver(void **state)
{
  struct sms_deliver_s d;
  uint8_t mti;

  /* "hellohello", from 27838890001, 1999-03-29 15:16:59 +02:00; a year
   * under 100 is taken as this century's
   */

  assert_int_equal(sms_decode("07917283010010F5040BC87238880900F1000099"
                              "3092516195800AE8329BFD4697D9EC37", &d, &mti),
                   0);
  assert_int_equal(mti, 0);
  assert_string_equal(d.sender, "27838890001");
  assert_string_equal(d.text, "hellohello");
  assert_int_equal(d.offset, 7200);
  assert_int_equal(d.sent, tel_utc(99, 3, 29, 15, 16, 59) - 7200);
  assert_int_equal(d.total, 0);
  assert_false(d.binary);

  /* UCS-2: "ąę", from +48123456789 */

  assert_int_equal(sms_decode("00040B918421436587F90008" S_SCTS
                              "0401050119", &d, &mti), 0);
  assert_string_equal(d.sender, "+48123456789");
  assert_string_equal(d.text, "ąę");
  assert_int_equal(d.offset, 3600);
  assert_int_equal(d.sent, tel_utc(26, 10, 30, 12, 3, 0) - 3600);

  /* A name: "Orange" says "hi"; "Telekom", its length counted in whole
   * octets, has no '@' after it
   */

  assert_int_equal(sms_decode("00040CD04F79D87D2E030000" S_SCTS
                              "02E834", &d, &mti), 0);
  assert_string_equal(d.sender, "Orange");
  assert_string_equal(d.text, "hi");
  assert_int_equal(sms_decode("00040ED0D432BBBC7EB7010000" S_SCTS
                              "02E834", &d, &mti), 0);
  assert_string_equal(d.sender, "Telekom");
  assert_int_equal(sms_decode("00040DD0D432BBBC7EB7010000" S_SCTS
                              "02E834", &d, &mti), 0);
  assert_string_equal(d.sender, "Telekom");

  /* Part 1 of 2 of message 0x7b: the header, a fill bit, then "hi" */

  assert_int_equal(sms_decode("00440B918421436587F90000" S_SCTS
                              "090500037B0201D069", &d, &mti), 0);
  assert_string_equal(d.text, "hi");
  assert_int_equal(d.ref, 0x7b);
  assert_false(d.ref16);
  assert_int_equal(d.total, 2);
  assert_int_equal(d.part, 1);

  /* A 16-bit reference: part 2 of 3 of message 0x1234, UCS-2 "A" */

  assert_int_equal(sms_decode("00440B918421436587F90008" S_SCTS
                              "09060804123403020041", &d, &mti), 0);
  assert_true(d.ref16);
  assert_int_equal(d.ref, 0x1234);
  assert_int_equal(d.total, 3);
  assert_int_equal(d.part, 2);
  assert_string_equal(d.text, "A");

  /* A west zone: -05:00 is 20 quarters, the sign in the first
   * semi-octet; one that is not BCD is no time stamp
   */

  assert_int_equal(sms_decode("00040B918421436587F90000620103213000"
                              "0A02E834", &d, &mti), 0);
  assert_int_equal(d.offset, -18000);
  assert_int_equal(d.sent, tel_utc(26, 10, 30, 12, 3, 0) + 18000);
  assert_int_equal(sms_decode("00040B918421436587F90000620103213000"
                              "FF02E834", &d, &mti), 0);
  assert_int_equal(d.offset, 0);
  assert_int_equal(d.sent, 0);

  /* 8-bit data: no text; compressed text: not taken */

  assert_int_equal(sms_decode("00040B918421436587F90004" S_SCTS
                              "02CAFE", &d, &mti), 0);
  assert_true(d.binary);
  assert_string_equal(d.text, "");
  assert_int_equal(sms_decode("00040B918421436587F90020" S_SCTS
                              "02E834", &d, &mti), -ENOTSUP);
}

/****************************************************************************
 * Name: test_sms_hostile
 *
 * Description:
 *   PDUs no network should send, refused or kept within bounds: an SMS
 *   sent or a status report told apart from a broken one; lengths beyond
 *   the data; a NUL in UCS-2; escapes unknown or at the end.
 *
 ****************************************************************************/

static void test_sms_hostile(void **state)
{
  static char hex[2 * 200];
  struct sms_deliver_s d;
  uint8_t mti;
  int i;

  /* Another kind: an SMS-SUBMIT, a status report */

  assert_int_equal(sms_decode("0001000B916407281553F800000AE8329BFD4697"
                              "D9EC37", &d, &mti), -ENOMSG);
  assert_int_equal(mti, 1);
  assert_int_equal(sms_decode("0002", &d, &mti), -ENOMSG);
  assert_int_equal(mti, 2);

  /* Not hex, odd, empty, cut short, an SMSC's address past the end */

  assert_int_equal(sms_decode("00ZZ", &d, &mti), -EINVAL);
  assert_int_equal(sms_decode("000", &d, &mti), -EINVAL);
  assert_int_equal(sms_decode("", &d, &mti), -EINVAL);
  assert_int_equal(sms_decode("00", &d, &mti), -EINVAL);
  assert_int_equal(sms_decode("00040B91842143", &d, &mti), -EINVAL);
  assert_int_equal(sms_decode("FF04", &d, &mti), -EINVAL);

  /* UDL beyond the data; UDHL beyond it; an IE longer than the header */

  assert_int_equal(sms_decode("00040B918421436587F90000" S_SCTS
                              "A0E834", &d, &mti), -EINVAL);
  assert_int_equal(sms_decode("00440B918421436587F90008" S_SCTS
                              "0420000102", &d, &mti), -EINVAL);
  assert_int_equal(sms_decode("00440B918421436587F90008" S_SCTS
                              "06050009030201", &d, &mti), -EINVAL);

  /* An address of 255 digits: cut to its room, never past it */

  strcpy(hex, "0004FF91");
  for (i = 0; i < 128; i++)
    {
      strcat(hex, "11");
    }

  strcat(hex, "0000" S_SCTS "02E834");
  assert_int_equal(sms_decode(hex, &d, &mti), 0);
  assert_int_equal(strlen(d.sender), SMS_NUMBER_MAX);
  assert_string_equal(d.text, "hi");

  /* UCS-2 "A", NUL, "B": the NUL is no end */

  assert_int_equal(sms_decode("00040B918421436587F90008" S_SCTS
                              "06004100000042", &d, &mti), 0);
  assert_string_equal(d.text, "A\xef\xbf\xbd" "B");

  /* "a", an escape before a code the extension table lacks, then "b":
   * the default table's "A"; and an escape at the end, nothing
   */

  assert_int_equal(sms_decode("00040B918421436587F90000" S_SCTS
                              "04E14D500C", &d, &mti), 0);
  assert_string_equal(d.text, "aAb");
  assert_int_equal(sms_decode("00040B918421436587F90000" S_SCTS
                              "02E10D", &d, &mti), 0);
  assert_string_equal(d.text, "a");
}

/****************************************************************************
 * Name: test_sms_submit
 *
 * Description:
 *   SMS-SUBMIT encoded: the classic example exactly, without a validity
 *   period; the extension table and UCS-2 back as they went.
 *
 ****************************************************************************/

static void test_sms_submit(void **state)
{
  static char back[2048];
  struct sms_part_s part;
  bool ucs2;

  assert_int_equal(sms_encode("+46708251358", "hellohello", 0, false, 1,
                              &part), 1);
  assert_false(part.ucs2);
  assert_string_equal(part.hex,
                      "0001000B916407281553F800000AE8329BFD4697D9EC37");
  assert_int_equal(part.tpdu, 22);

  /* A delivery report asked for */

  assert_int_equal(sms_encode("123", "x", 0, true, 1, &part), 1);
  assert_int_equal(strncmp(part.hex, "0021000381", 10), 0);

  /* The extension table; UCS-2, with a character outside the BMP */

  assert_int_equal(sms_test_all("Cost: 5€ [ok] {x} ~^|\\", back,
                                sizeof(back), &ucs2), 1);
  assert_false(ucs2);
  assert_string_equal(back, "Cost: 5€ [ok] {x} ~^|\\");
  assert_int_equal(sms_test_all("Zażółć gęślą jaźń 😀", back,
                                sizeof(back), &ucs2), 1);
  assert_true(ucs2);
  assert_string_equal(back, "Zażółć gęślą jaźń 😀");

  /* No number, no text, text that is not UTF-8, no such part */

  assert_int_equal(sms_encode("", "x", 0, false, 1, &part), -EINVAL);
  assert_int_equal(sms_encode("+48 123", "x", 0, false, 1, &part),
                   -EINVAL);
  assert_int_equal(sms_encode("123", "", 0, false, 1, &part), -EINVAL);
  assert_int_equal(sms_encode("123", "\xc3\x28", 0, false, 1, &part),
                   -EINVAL);
  assert_int_equal(sms_encode("123", "x", 0, false, 2, &part), -EINVAL);
  assert_int_equal(sms_encode("123", "x", 0, false, 0, &part), -EINVAL);
}

/****************************************************************************
 * Name: test_sms_long
 *
 * Description:
 *   Long messages in parts of 153 septets or 67 UTF-16 units, each with
 *   its header; an escape and a surrogate pair kept whole at a part's
 *   edge; every 1530-byte text RFC 0029 takes goes, up to 23 parts.
 *
 ****************************************************************************/

static void test_sms_long(void **state)
{
  static char text[4096];
  static char back[4096];
  struct sms_part_s part;
  struct sms_deliver_s d;
  bool ucs2;
  int i;

  /* 160 septets fit one part; 161 take two */

  memset(text, 'a', 160);
  text[160] = '\0';
  assert_int_equal(sms_parts(text, &ucs2), 1);
  text[160] = 'b';
  text[161] = '\0';
  assert_int_equal(sms_test_all(text, back, sizeof(back), &ucs2), 2);
  assert_string_equal(back, text);
  assert_int_equal(sms_encode("123", text, 9, false, 2, &part), 2);
  sms_test_back(&part, &d);
  assert_string_equal(d.text, "aaaaaaab");

  /* An escape on the edge goes to the next part whole */

  memset(text, 'a', 152);
  strcpy(text + 152, "€ and more text to pass one part");
  assert_int_equal(sms_encode("123", text, 1, false, 1, &part), 2);
  sms_test_back(&part, &d);
  assert_int_equal(strlen(d.text), 152);
  assert_int_equal(sms_test_all(text, back, sizeof(back), &ucs2), 2);
  assert_string_equal(back, text);

  /* UCS-2: 70 units fit one part; a surrogate pair on the edge stays
   * whole
   */

  text[0] = '\0';
  for (i = 0; i < 70; i++)
    {
      strcat(text, "ą");
    }

  assert_int_equal(sms_parts(text, &ucs2), 1);
  text[0] = '\0';
  for (i = 0; i < 66; i++)
    {
      strcat(text, "ą");
    }

  strcat(text, "😀 more");
  assert_int_equal(sms_encode("123", text, 2, false, 1, &part), 2);
  sms_test_back(&part, &d);
  assert_int_equal(strlen(d.text), 66 * 2);
  assert_int_equal(sms_test_all(text, back, sizeof(back), &ucs2), 2);
  assert_string_equal(back, text);

  /* 1530 bytes, the most RFC 0029 takes: plain text in 10 parts; 765
   * Polish letters in 12; 1529 bytes of one-byte text and one letter
   * outside GSM 7-bit, as UCS-2, in 23
   */

  memset(text, 'x', 1530);
  text[1530] = '\0';
  assert_int_equal(sms_test_all(text, back, sizeof(back), &ucs2), 10);
  assert_string_equal(back, text);

  text[0] = '\0';
  for (i = 0; i < 765; i++)
    {
      strcat(text, "ą");
    }

  assert_int_equal(sms_test_all(text, back, sizeof(back), &ucs2), 12);
  assert_string_equal(back, text);

  memset(text, 'y', 1528);
  strcpy(text + 1528, "ą");
  assert_int_equal(sms_test_all(text, back, sizeof(back), &ucs2), 23);
  assert_true(ucs2);
  assert_string_equal(back, text);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_sms_deliver),
    cmocka_unit_test(test_sms_hostile),
    cmocka_unit_test(test_sms_submit),
    cmocka_unit_test(test_sms_long),
  };

  return cmocka_run_group_tests_name("sms", tests, NULL, NULL);
}
