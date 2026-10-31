/****************************************************************************
 * pnut-os/src/telephony/sms.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_TELEPHONY_SMS_H
#define __PNUT_OS_TELEPHONY_SMS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pnut/compiler.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SMS in PDU mode (3GPP TS 23.040, 23.038), Telephony's own codec (RFC
 * 0030): GSM 7-bit with its extension table, UCS-2 (UTF-16, as phones
 * send it), and the header that joins a long message's parts.
 */

#define SMS_NUMBER_MAX    32        /* A number or a name, without NUL */
#define SMS_PART_TEXT     480       /* A part's text in UTF-8, with room:
                                     * 320 bytes from GSM 7-bit at most */
#define SMS_TPDU_MAX      164       /* A TPDU's octets at most */
#define SMS_PARTS_MAX     255       /* Parts of one message: the header
                                     * numbers them in an octet */
#define SMS_SEPTETS       160       /* GSM 7-bit characters in one part */
#define SMS_SEPTETS_CONCAT 153      /* and in a part of a long one */
#define SMS_UNITS         70        /* UTF-16 units in one part */
#define SMS_UNITS_CONCAT  67        /* and in a part of a long one */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* An SMS received (SMS-DELIVER), one part of it */

struct sms_deliver_s
{
  char sender[SMS_NUMBER_MAX + 1];  /* "+48...", other digits, or a name */
  int64_t sent;                     /* The network's time stamp, UTC */
  int32_t offset;                   /* Its zone, seconds east of UTC */
  uint8_t pid;
  uint8_t dcs;
  bool binary;                      /* 8-bit data, not text: text empty */
  bool ref16;                       /* ref is a 16-bit one */
  uint16_t ref;                     /* A long message's reference */
  uint8_t total;                    /* Its parts; 0 for a whole message */
  uint8_t part;                     /* This part, from 1 */
  char text[SMS_PART_TEXT + 1];     /* UTF-8 */
};

/* A part of an SMS to send (SMS-SUBMIT): its PDU in hex as AT+CMGS takes
 * it, the SMSC's address left to the SIM ("00" first), and its TPDU's
 * length in octets for AT+CMGS=<length>
 */

struct sms_part_s
{
  char hex[2 * (1 + SMS_TPDU_MAX) + 1];
  uint8_t tpdu;
  bool ucs2;                        /* Not every character is GSM 7-bit */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: sms_decode
 *
 * Description:
 *   An SMS-DELIVER PDU in hex, as AT+CMGR gives it in PDU mode, the SMSC's
 *   address first.  A text whose characters do not decode keeps U+FFFD in
 *   their place.
 *
 * Returned Value:
 *   Zero (OK); -ENOMSG for a PDU of another kind, such as a status report
 *   or a message sent, its TP-MTI in *mti; -EINVAL for one that is not
 *   hex or is cut short; -ENOTSUP for compressed text.
 *
 ****************************************************************************/

int sms_decode(FAR const char *hex, FAR struct sms_deliver_s *out,
               FAR uint8_t *mti);

/****************************************************************************
 * Name: sms_user_data
 *
 * Description:
 *   A TP-UD decoded: its header, if udhi, for a long message's part, and
 *   its text by the coding dcs gives.  udl is in septets for GSM 7-bit,
 *   in octets otherwise; len is the octets ud holds.
 *
 * Returned Value:
 *   As sms_decode().
 *
 ****************************************************************************/

int sms_user_data(FAR const uint8_t *ud, size_t len, uint8_t udl,
                  uint8_t dcs, bool udhi, FAR struct sms_deliver_s *out);

/****************************************************************************
 * Name: sms_parts
 *
 * Description:
 *   The parts a text takes: GSM 7-bit when every character has a code
 *   there, UCS-2 otherwise; one part of 160 septets or 70 UTF-16 units,
 *   or parts of 153 or 67, each with the header that joins them.  A part
 *   never splits a character's escape or a surrogate pair.
 *
 * Returned Value:
 *   The parts (1 or more); -EINVAL for a text that is empty or not UTF-8;
 *   -E2BIG for more than SMS_PARTS_MAX parts.
 *
 ****************************************************************************/

int sms_parts(FAR const char *text, FAR bool *ucs2);

/****************************************************************************
 * Name: sms_encode
 *
 * Description:
 *   Part k (from 1) of a text to a number, as an SMS-SUBMIT PDU, the parts
 *   as sms_parts() counts them, joined under ref when there are several.
 *   report asks the network for a delivery report.  One part at a time,
 *   as Telephony sends them.
 *
 * Returned Value:
 *   The parts in all; -EINVAL for a number that is no number, a text that
 *   is empty or not UTF-8, or no part k; -E2BIG as sms_parts().
 *
 ****************************************************************************/

int sms_encode(FAR const char *number, FAR const char *text, uint8_t ref,
               bool report, uint8_t k, FAR struct sms_part_s *part);

#endif /* __PNUT_OS_TELEPHONY_SMS_H */
