/****************************************************************************
 * pnut-os/src/telephony/smsjoin.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_TELEPHONY_SMSJOIN_H
#define __PNUT_OS_TELEPHONY_SMSJOIN_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/compiler.h>

#include "sms.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* A long SMS's parts, joined as they come (RFC 0030): each part keeps its
 * slot in the modem's memory until the whole message is delivered.  A
 * message whose parts are still missing after a day is delivered as it
 * is, with a mark where each gap is.
 */

#define SMSJOIN_MESSAGES  6         /* Long messages joined at once */
#define SMSJOIN_PARTS     64        /* Their parts at most; a message of
                                     * more is delivered a part at a time */
#define SMSJOIN_WAIT      86400     /* Seconds a part waits for the rest */
#define SMSJOIN_KEY       32        /* The key's characters (RFC 0029) */

/* smsjoin_add(): a copy of a part already in */

#define SMSJOIN_COPY      2

/* Where a part is missing */

#define SMSJOIN_GAP       "[…]"

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A message whole, or given up waiting for: what Telephony delivers */

struct smsjoin_msg_s
{
  char sender[SMS_NUMBER_MAX + 1];
  int64_t sent;                     /* The first part present's time stamp,
                                     * UTC */
  char key[SMSJOIN_KEY + 1];        /* Names it on the network */
  bool gaps;                        /* Parts were missing */
  bool binary;                      /* 8-bit data: no text */
  uint8_t nslots;
  int16_t slots[SMSJOIN_PARTS];     /* Its parts' slots in the modem */
  FAR char *text;                   /* UTF-8, its parts joined; given back
                                     * with smsjoin_free() */
};

/* A long message's parts so far */

struct smsjoin_part_s
{
  bool in;
  int16_t slot;
  int64_t sent;
  FAR char *text;                   /* From the heap */
};

struct smsjoin_long_s
{
  bool used;
  char sender[SMS_NUMBER_MAX + 1];
  bool ref16;
  uint16_t ref;
  uint8_t total;
  uint8_t count;                    /* Parts in */
  bool binary;
  uint64_t first;                   /* When its first part came, in s */
  struct smsjoin_part_s parts[SMSJOIN_PARTS];
};

struct smsjoin_s
{
  struct smsjoin_long_s longs[SMSJOIN_MESSAGES];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: smsjoin_add
 *
 * Description:
 *   A part read from the modem's slot.  A whole message, or the last part
 *   of a long one, makes out, which holds anything only then.  A part
 *   already in from the same slot, as when the modem's memory is read
 *   again, changes nothing; the same part again from another slot, as
 *   when the network sent it twice, is a copy whose slot is to be
 *   deleted; a part of the same number with another text, a reference
 *   two messages share, goes as a message of its own.  now is the
 *   monotonic clock's seconds, which never go back.
 *
 *   A reference two messages share can still join a part of one with the
 *   other's when their texts do not tell them apart: nothing in the parts
 *   does.
 *
 * Returned Value:
 *   1 when out holds a message to deliver; 0 when the part waits for the
 *   rest; SMSJOIN_COPY when it is a copy, its slot to delete; -ENOSPC
 *   when SMSJOIN_MESSAGES long messages are being joined already, and the
 *   part stays in the modem; -ENOMEM: a whole message or a lone part to
 *   read again later, and a long one complete but not joined, which
 *   smsjoin_expired() gives next; -EINVAL for a part numbered 0, or beyond
 *   its message's parts.
 *
 ****************************************************************************/

int smsjoin_add(FAR struct smsjoin_s *join,
                FAR const struct sms_deliver_s *part, int16_t slot,
                uint64_t now, FAR struct smsjoin_msg_s *out);

/****************************************************************************
 * Name: smsjoin_expired
 *
 * Description:
 *   A long message whose first part came SMSJOIN_WAIT seconds ago or more,
 *   with a mark for each missing part, or one complete whose joining ran
 *   out of memory, taken out of the join.  Called now and then, and after
 *   smsjoin_add() returns -ENOMEM.
 *
 * Returned Value:
 *   1 when out holds one; 0 when none is due, or the memory still runs
 *   out.
 *
 ****************************************************************************/

int smsjoin_expired(FAR struct smsjoin_s *join, uint64_t now,
                    FAR struct smsjoin_msg_s *out);

/****************************************************************************
 * Name: smsjoin_key
 *
 * Description:
 *   The key RFC 0029 has an SMS stored once for: its sender, its first
 *   part's time stamp, a long one's reference, and its text, hashed into
 *   SMSJOIN_KEY hex characters; and part, for a part that goes on its own
 *   (0 for a message whole or joined).
 *
 ****************************************************************************/

void smsjoin_key(FAR const char *sender, int64_t sent, uint16_t ref,
                 uint8_t part, FAR const char *text, FAR char *key);

/****************************************************************************
 * Name: smsjoin_free, smsjoin_clear
 *
 * Description:
 *   A message's text given back; every long message being joined dropped,
 *   as when the modem restarts (its parts are read again).
 *
 ****************************************************************************/

void smsjoin_free(FAR struct smsjoin_msg_s *msg);
void smsjoin_clear(FAR struct smsjoin_s *join);

#endif /* __PNUT_OS_TELEPHONY_SMSJOIN_H */
