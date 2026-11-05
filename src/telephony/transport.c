/****************************************************************************
 * pnut-os/src/telephony/transport.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Telephony's sms transport, its arriving half (RFC 0029, RFC 0030 step
 * 2).  The modem keeps each SMS in its own memory and says where; each is
 * read, a long one's parts joined, and the message delivered to Messages.
 * Only once Messages has it on flash are its slots deleted, so a reset
 * between the two delivers it again, which its key keeps from being
 * stored twice.  The memory is read through at start, whenever Messages
 * registers sms again, and after anything failed.
 *
 * Status reports, and 8-bit data such as the notifications MMS and WAP
 * push send, are no messages to keep: deleted unread.  An SMS that does
 * not decode, or one stored that was sent, stays in the modem.  Texts and
 * numbers are never logged (RFC 0026).
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <pnut/log.h>
#include <pnut/loop.h>

#include "at.h"
#include "parse.h"
#include "telephony.h"
#include "transport.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TR_DELIVER_MS   10000       /* For Messages' answer */
#define TR_REGISTER_MS  5000
#define TR_AGAIN_MS     2000        /* A registration Messages could not
                                     * take yet, asked again */
#define TR_LATER_MS     1000        /* When the modem's queue is full */
#define TR_CHOOSE_MS    5000        /* The memory not chosen, at start */
#define TR_CHOOSE_TRIES 6
#define TR_MTI_STATUS   0x02        /* SMS-STATUS-REPORT */
#define TR_TEXT_MAX     (sizeof(((FAR pnut_messages_deliver_request_t *)0) \
                                ->text) - 1)
#define TR_STORAGE      "AT+CPMS=\"ME\",\"ME\",\"ME\""
#define TR_SEND_MS      60000       /* A part's AT+CMGS, network and all */

/* The SIM's memory read and emptied, what arrives meanwhile still stored in
 * the modem's
 */

#define TR_STORAGE_SIM  "AT+CPMS=\"SM\",\"SM\",\"ME\""

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* What is out */

enum transport_what_e
{
  TR_NONE = 0,
  TR_CHOOSE,                      /* AT+CPMS: the memory, counted */
  TR_READ,                        /* AT+CMGR */
  TR_DROP,                        /* AT+CMGD, of one slot to forget */
  TR_DELIVER,                     /* To Messages */
  TR_DELETE,                      /* AT+CMGD, of a delivered one's slot */
  TR_SEND,                        /* AT+CMGS, a part of out */
  TR_REPORT                       /* out's status, to Messages */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void tr_next(FAR struct transport_s *tr);
static void tr_register(FAR struct transport_s *tr);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tr_now
 *
 * Description:
 *   The monotonic clock's seconds, for the join's day of waiting.
 *
 ****************************************************************************/

static uint64_t tr_now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec;
}

/****************************************************************************
 * Name: tr_seen, tr_see
 *
 * Description:
 *   Whether a slot was read already, and is held or kept; marking it so,
 *   or not.  Slots past TRANSPORT_SLOTS are never marked: they are read
 *   again, which the join takes as nothing new.
 *
 ****************************************************************************/

static bool tr_seen(FAR const struct transport_s *tr, int slot)
{
  return slot >= 0 && slot < TRANSPORT_SLOTS &&
         (tr->seen[slot / 8] & (1 << (slot % 8))) != 0;
}

static void tr_see(FAR struct transport_s *tr, int slot, bool seen)
{
  if (slot < 0 || slot >= TRANSPORT_SLOTS)
    {
      return;
    }

  if (seen)
    {
      tr->seen[slot / 8] |= (uint8_t)(1 << (slot % 8));
    }
  else
    {
      tr->seen[slot / 8] &= (uint8_t)~(1 << (slot % 8));
    }
}

/****************************************************************************
 * Name: tr_later
 *
 * Description:
 *   The modem's queue was full: the work goes on in a moment.
 *
 ****************************************************************************/

static void tr_later_fired(FAR struct pnut_loop_s *loop,
                           FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct transport_s *tr = arg;

  tr->later = NULL;
  if (tr->connected && !tr->registered && !tr->registering)
    {
      tr_register(tr);
    }

  tr_next(tr);
}

static void tr_later(FAR struct transport_s *tr, uint32_t ms)
{
  if (tr->later == NULL &&
      pnut_timer_start(tr->tel->module.loop, ms, 0,
                       tr_later_fired, tr, &tr->later) < 0)
    {
      tr->later  = NULL;
      tr->paused = true;          /* The tick then */
    }
}

/****************************************************************************
 * Name: tr_at
 *
 * Description:
 *   A command to the modem, the one thing out.
 *
 ****************************************************************************/

static void tr_at(FAR struct transport_s *tr, FAR const char *command,
                  FAR const char *prefix, uint8_t what, at_done_t done)
{
  int ret;

  /* Out before it is queued: a command that fails at once is answered
   * from inside at_command()
   */

  tr->busy     = true;
  tr->what     = what;
  tr->sent_gen = tr->gen;
  ret = at_command(&tr->tel->at, command, prefix, 0, 0, done, tr);
  if (ret < 0)
    {
      tr->busy = false;
      tr->what = TR_NONE;
      tr_later(tr, TR_LATER_MS);
    }
}

/****************************************************************************
 * Name: tr_answered
 *
 * Description:
 *   The answer to what is out, unless the modem went since it was sent:
 *   then it is left aside.  A delivery is still waited for when the modem
 *   goes (transport_gone() leaves it busy), so that no newer one is sent
 *   before Messages answers it, and its late answer is never taken for a
 *   newer one's; that answer is then left aside here, and the message,
 *   read again from the modem once it is back, is delivered again
 *   (Messages stores it once, by its key).  A report's answer is taken
 *   whatever the modem did (tr_reported() does not come here).
 *
 ****************************************************************************/

static bool tr_answered(FAR struct transport_s *tr, int result,
                        uint8_t what)
{
  if (!tr->busy || tr->what != what)
    {
      return false;
    }

  tr->busy = false;
  tr->what = TR_NONE;
  if (tr->sent_gen != tr->gen)
    {
      tr_next(tr);
      return false;
    }

  return result != -ECANCELED;
}

/****************************************************************************
 * Name: tr_line
 *
 * Description:
 *   The line of an answer that begins with prefix, and the line after it,
 *   copied: NULL when there is none.
 *
 ****************************************************************************/

static FAR const char *tr_line(FAR const char *reply, FAR const char *prefix,
                               FAR char *line, size_t size,
                               FAR const char **after)
{
  FAR const char *p = reply;
  FAR const char *end;
  size_t n;

  while (p != NULL && *p != '\0')
    {
      end = strchr(p, '\n');
      n   = end != NULL ? (size_t)(end - p) : strlen(p);
      if (strncmp(p, prefix, strlen(prefix)) == 0 && n < size)
        {
          memcpy(line, p, n);
          line[n] = '\0';
          if (after != NULL)
            {
              *after = end != NULL ? end + 1 : p + n;
            }

          return line;
        }

      p = end != NULL ? end + 1 : NULL;
    }

  return NULL;
}

/****************************************************************************
 * Name: tr_release
 *
 * Description:
 *   The message held let go; its slots read again later when it did not
 *   go to Messages.
 *
 ****************************************************************************/

static void tr_release(FAR struct transport_s *tr, bool again)
{
  uint8_t i;

  if (!tr->holding)
    {
      return;
    }

  if (again)
    {
      for (i = 0; i < tr->msg.nslots; i++)
        {
          tr_see(tr, tr->msg.slots[i], false);
        }
    }

  smsjoin_free(&tr->msg);
  tr->holding   = false;
  tr->delivered = false;

  /* A change of memory that waited for it: now */

  if (tr->sim != tr->sim_check)
    {
      tr->storage = false;
    }
}

/****************************************************************************
 * Name: tr_piece
 *
 * Description:
 *   The length of the text's piece from offset: a text longer than a
 *   message holds goes as several, in order (RFC 0029), each cut at a
 *   whole character.
 *
 ****************************************************************************/

static size_t tr_piece(FAR const char *text, size_t offset)
{
  size_t n = strlen(text + offset);
  size_t cut;

  if (n <= TR_TEXT_MAX)
    {
      return n;
    }

  cut = TR_TEXT_MAX;
  while (cut > 0 && ((uint8_t)text[offset + cut] & 0xc0) == 0x80)
    {
      cut--;
    }

  return cut;
}

/****************************************************************************
 * Name: tr_hold
 *
 * Description:
 *   A message made by the join, to deliver.
 *
 ****************************************************************************/

static void tr_hold(FAR struct transport_s *tr)
{
  FAR const char *text = tr->msg.text != NULL ? tr->msg.text : "";
  size_t offset = 0;
  size_t n;

  tr->holding   = true;
  tr->delivered = false;
  tr->chunk     = 0;
  tr->offset    = 0;
  tr->deleting  = 0;
  tr->chunks    = 0;

  do
    {
      n = tr_piece(text, offset);
      offset += n;
      tr->chunks++;
    }
  while (text[offset] != '\0' && n > 0 && tr->chunks < UINT8_MAX);

  pnut_log(&tr->tel->module, PNUT_LOG_INFO,
           "An SMS of %u part%s to deliver%s", tr->msg.nslots,
           tr->msg.nslots == 1 ? "" : "s",
           tr->msg.gaps ? ", parts missing" : "");
}

/****************************************************************************
 * Name: tr_choose_done
 ****************************************************************************/

static void tr_choose_done(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct transport_s *tr = arg;
  char line[96];

  if (!tr_answered(tr, result, TR_CHOOSE))
    {
      return;
    }

  if (tr->sim &&
      (result < 0 ||
       tr_line(reply, "+CPMS:", line, sizeof(line), NULL) == NULL ||
       tel_sms_memory(line, &tr->used, &tr->total) < 0))
    {
      /* No SIM memory to read (or not yet): the modem's, as before */

      pnut_log(&tr->tel->module, PNUT_LOG_INFO,
               "The SIM's SMS memory not read: %d", result);
      tr->sim_check = false;
      tr_next(tr);
      return;
    }

  if (result < 0 ||
      tr_line(reply, "+CPMS:", line, sizeof(line), NULL) == NULL ||
      tel_sms_memory(line, &tr->used, &tr->total) < 0)
    {
      /* Right after start the modem may not take it yet (it says SMS
       * DONE when it does): a few tries soon, then at the tick
       */

      if (tr->tries < UINT8_MAX)
        {
          tr->tries++;
        }

      pnut_log(&tr->tel->module, tr->tries < TR_CHOOSE_TRIES ?
               PNUT_LOG_INFO : tr->tries == TR_CHOOSE_TRIES ?
               PNUT_LOG_WARNING : PNUT_LOG_DEBUG,
               "The modem's SMS memory not chosen: %d", result);
      if (tr->tries < TR_CHOOSE_TRIES)
        {
          tr_later(tr, TR_CHOOSE_MS);
        }
      else
        {
          tr->paused = true;
        }

      return;
    }

  tr->tries = 0;

  pnut_log(&tr->tel->module, PNUT_LOG_INFO, "The %s SMS memory: %u of %u",
           tr->sim ? "SIM's" : "modem's", tr->used, tr->total);
  if (tr->sim && tr->used == 0)
    {
      tr->sim_check = false;      /* Nothing there: the modem's */
      tr_next(tr);
      return;
    }

  tr->storage  = true;
  tr->scanning = tr->used > 0;
  tr->next     = 0;
  tr->found    = 0;
  tr_next(tr);
}

/****************************************************************************
 * Name: tr_drop, tr_drop_done
 *
 * Description:
 *   A slot that holds nothing to keep, deleted unread.
 *
 ****************************************************************************/

static void tr_drop_done(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct transport_s *tr = arg;

  if (!tr_answered(tr, result, TR_DROP))
    {
      return;
    }

  /* Deleted or not, the slot is read again: a failed delete is tried
   * again at the tick
   */

  if (result < 0)
    {
      pnut_log(&tr->tel->module, PNUT_LOG_WARNING,
               "SMS slot %d not deleted: %d", tr->slot, result);
      tr->again = true;
    }

  tr_see(tr, tr->slot, false);
  tr_next(tr);
}

static void tr_drop(FAR struct transport_s *tr, FAR const char *what)
{
  char command[24];

  pnut_log(&tr->tel->module, PNUT_LOG_INFO, "SMS slot %d: %s, deleted",
           tr->slot, what);
  snprintf(command, sizeof(command), "AT+CMGD=%d", tr->slot);
  tr_at(tr, command, NULL, TR_DROP, tr_drop_done);
}

/****************************************************************************
 * Name: tr_read, tr_read_done
 *
 * Description:
 *   An SMS read from its slot, and joined.
 *
 ****************************************************************************/

static void tr_read_done(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct transport_s *tr = arg;
  FAR const char *pdu = NULL;
  FAR const char *end;
  char line[64];
  char hex[2 * (1 + 12 + SMS_TPDU_MAX) + 2];
  uint8_t mti = 0;
  size_t n;
  int ret;

  if (!tr_answered(tr, result, TR_READ))
    {
      return;
    }

  /* No answer, or the command never went: read again at the tick.  An
   * error answer is what an empty slot may give, and goes on.
   */

  if (result < 0 && result != -EIO)
    {
      tr->paused = true;
      return;
    }

  /* An empty slot answers nothing, or an error */

  if (result < 0 ||
      tr_line(reply, "+CMGR:", line, sizeof(line), &pdu) == NULL ||
      tel_sms_length(line) < 0 || pdu == NULL)
    {
      tr_next(tr);
      return;
    }

  end = strchr(pdu, '\n');
  n   = end != NULL ? (size_t)(end - pdu) : strlen(pdu);
  /* Found by the scan, or read from the queue in a slot the scan has
   * passed: it counts towards what was counted, or announced meanwhile
   */

  if (tr->scan_read || (tr->scanning && tr->slot < tr->next))
    {
      tr->found++;
    }

  tr_see(tr, tr->slot, true);
  if (n == 0 || n >= sizeof(hex))
    {
      ret = -EINVAL;
    }
  else
    {
      memcpy(hex, pdu, n);
      hex[n] = '\0';
      ret = sms_decode(hex, &tr->part, &mti);
    }

  if (ret == -ENOMSG && mti == TR_MTI_STATUS)
    {
      tr_drop(tr, "a status report");
      return;
    }

  if (ret == -ENOMSG)
    {
      pnut_log(&tr->tel->module, PNUT_LOG_INFO,
               "SMS slot %d: not one received, kept", tr->slot);
      tr_next(tr);
      return;
    }

  if (ret < 0)
    {
      pnut_log(&tr->tel->module, PNUT_LOG_WARNING,
               "SMS slot %d cannot be read (%d): kept in the modem",
               tr->slot, ret);
      tr_next(tr);
      return;
    }

  if (tr->part.binary)
    {
      tr_drop(tr, "data, not text");
      return;
    }

  ret = smsjoin_add(&tr->join, &tr->part, tr->slot, tr_now(), &tr->msg);
  switch (ret)
    {
      case 0:                     /* Waits for the rest, or read again */
        break;

      case 1:
      case SMSJOIN_EVICTED:
        tr_hold(tr);
        break;

      case SMSJOIN_COPY:
        tr_drop(tr, "a copy");
        return;

      case SMSJOIN_EVICTED_ONLY:

        /* The part did not go in: read again once the message given up
         * is delivered
         */

        tr_hold(tr);
        tr_see(tr, tr->slot, false);
        tr->storage = false;
        break;

      case -ENOMEM:
        tr_see(tr, tr->slot, false);
        tr->paused = true;
        return;

      default:
        pnut_log(&tr->tel->module, PNUT_LOG_WARNING,
                 "SMS slot %d: a part beyond its message, kept", tr->slot);
        break;
    }

  tr_next(tr);
}

static void tr_read(FAR struct transport_s *tr, int slot, bool scan)
{
  char command[24];

  tr->slot      = (int16_t)slot;
  tr->scan_read = scan;
  snprintf(command, sizeof(command), "AT+CMGR=%d", slot);
  tr_at(tr, command, "+CMGR:", TR_READ, tr_read_done);
}

/****************************************************************************
 * Name: tr_deliver, tr_delivered
 *
 * Description:
 *   The held message's next piece to Messages; answered once on flash.
 *
 ****************************************************************************/

static void tr_delivered(FAR struct pnut_messages_client_s *client,
                         int status,
                         FAR const pnut_messages_delivered_t *out,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct transport_s *tr = arg;
  FAR const char *text;

  if (!tr_answered(tr, 0, TR_DELIVER))
    {
      return;
    }

  if (status != PNUT_STATUS_OK)
    {
      /* Kept in the modem: read and delivered again later; told once,
       * not at every try
       */

      pnut_log(&tr->tel->module, tr->refused ? PNUT_LOG_DEBUG :
               PNUT_LOG_WARNING, "Messages did not take an SMS (%d, %"
               PRIu32 "): kept in the modem", status,
               err != NULL ? err->code : 0);
      tr->refused = true;
      tr_release(tr, true);
      tr->paused = true;
      return;
    }

  tr->refused = false;
  text = tr->msg.text != NULL ? tr->msg.text : "";
  tr->offset += tr_piece(text, tr->offset);
  if (++tr->chunk >= tr->chunks)
    {
      tr->delivered = true;
      tr->deleting  = 0;
    }

  tr_next(tr);
}

static void tr_deliver(FAR struct transport_s *tr)
{
  FAR pnut_messages_deliver_request_t *in = &tr->deliver;
  FAR const char *text = tr->msg.text != NULL ? tr->msg.text : "";
  size_t n = tr_piece(text, tr->offset);
  int ret;

  memset(in, 0, sizeof(*in));
  strlcpy(in->peer, tr->msg.sender, sizeof(in->peer));
  memcpy(in->text, text + tr->offset, n);
  in->text[n] = '\0';
  in->sent    = tr->msg.sent;
  if (tr->chunks == 1)
    {
      strlcpy(in->key, tr->msg.key, sizeof(in->key));
    }
  else
    {
      snprintf(in->key, sizeof(in->key), "%s.%u", tr->msg.key,
               tr->chunk + 1);
    }

  /* A peer the network left empty still needs one: Messages keys its
   * conversations by it
   */

  if (in->peer[0] == '\0')
    {
      strlcpy(in->peer, "?", sizeof(in->peer));
    }

  ret = pnut_messages_deliver(&tr->messages, in, TR_DELIVER_MS,
                              tr_delivered, tr);
  if (ret < 0)
    {
      tr_later(tr, TR_LATER_MS);
      return;
    }

  tr->busy     = true;
  tr->what     = TR_DELIVER;
  tr->sent_gen = tr->gen;
}

/****************************************************************************
 * Name: tr_delete, tr_deleted
 *
 * Description:
 *   A delivered message's slots, deleted one by one.  One that stays is
 *   read and delivered again at the tick, which Messages answers "again"
 *   to, and deleted then.
 *
 ****************************************************************************/

static void tr_deleted(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct transport_s *tr = arg;

  if (!tr_answered(tr, result, TR_DELETE))
    {
      return;
    }

  /* One that stays is read and delivered again at the tick, which
   * Messages answers "again" to, and deleted then
   */

  if (result < 0)
    {
      pnut_log(&tr->tel->module, PNUT_LOG_WARNING,
               "SMS slot %d not deleted: %d", tr->slot, result);
      tr->again = true;
    }

  tr_see(tr, tr->slot, false);
  if (++tr->deleting >= tr->msg.nslots)
    {
      /* With Messages: its parts, met again, are copies */

      smsjoin_delivered(&tr->join, &tr->msg, tr_now());
      tr_release(tr, false);
    }

  tr_next(tr);
}

static void tr_delete(FAR struct transport_s *tr)
{
  char command[24];

  if (tr->deleting >= tr->msg.nslots)
    {
      tr_release(tr, false);
      tr_next(tr);
      return;
    }

  tr->slot = tr->msg.slots[tr->deleting];
  snprintf(command, sizeof(command), "AT+CMGD=%d", tr->slot);
  tr_at(tr, command, NULL, TR_DELETE, tr_deleted);
}

/****************************************************************************
 * Name: tr_network
 *
 * Description:
 *   Whether the modem has the network: then SMS can go.
 *
 ****************************************************************************/

static bool tr_network(FAR struct transport_s *tr)
{
  uint32_t reg = tr->tel->state.registration;

  return !tr->tel->airplane &&
         (reg == PNUT_TELEPHONY_INFO_REGISTRATION_HOME ||
          reg == PNUT_TELEPHONY_INFO_REGISTRATION_ROAMING);
}

/****************************************************************************
 * Name: tr_report, tr_reported
 *
 * Description:
 *   The message out reported to Messages: sent, or failed with why.  Then
 *   the next may be taken.  Not taken by Messages, it stays sending there,
 *   not known to be sent after its next start.
 *
 ****************************************************************************/

static void tr_reported(FAR struct pnut_messages_client_s *client,
                        int status, FAR const pnut_messages_reply_t *out,
                        FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct transport_s *tr = arg;

  /* Messages' answer, whatever the modem did meanwhile */

  if (!tr->busy || tr->what != TR_REPORT)
    {
      return;
    }

  tr->busy = false;
  tr->what = TR_NONE;

  /* Messages away, slow or busy: reported again, once it is back or in a
   * moment (a status given twice is taken once)
   */

  if (status == PNUT_STATUS_UNAVAILABLE || status == PNUT_STATUS_TIMEOUT ||
      status == PNUT_STATUS_BUSY)
    {
      pnut_log(&tr->tel->module, PNUT_LOG_INFO,
               "An SMS's status not taken yet: %d", status);
      if (status != PNUT_STATUS_UNAVAILABLE)
        {
          tr_later(tr, TR_LATER_MS);
        }

      return;
    }

  if (status != PNUT_STATUS_OK)
    {
      pnut_log(&tr->tel->module, PNUT_LOG_WARNING,
               "Messages did not take an SMS's status: %d", status);
    }

  tr->reporting = false;
  tr->sending   = false;
  tr->lost      = false;
  tr_next(tr);
}

static void tr_report_send(FAR struct transport_s *tr)
{
  int ret;

  tr->busy     = true;
  tr->what     = TR_REPORT;
  tr->sent_gen = tr->gen;
  ret = pnut_messages_report(&tr->messages, &tr->report, TR_DELIVER_MS,
                             tr_reported, tr);
  if (ret < 0)
    {
      tr->busy = false;
      tr->what = TR_NONE;
      tr_later(tr, TR_LATER_MS);
    }
}

static void tr_report(FAR struct transport_s *tr, int status,
                      FAR const char *why)
{
  FAR pnut_messages_report_request_t *in = &tr->report;

  memset(in, 0, sizeof(*in));
  in->conversation = tr->out.conversation;
  in->message      = tr->out.message;
  in->status       = status;
  strlcpy(in->reason, why, sizeof(in->reason));
  tr->reporting = true;
  tr_report_send(tr);
}

/****************************************************************************
 * Name: tr_send, tr_sent
 *
 * Description:
 *   The message out, a part at a time: AT+CMGS with the part's PDU after
 *   the modem's prompt.  A part that fails ends it, failed with the
 *   modem's word; a modem gone with a part out ends it too, rather than
 *   sending parts twice.
 *
 ****************************************************************************/

static void tr_sent(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct transport_s *tr = arg;
  char why[sizeof(tr->report.reason)];

  /* Cancelled, the modem going: it may have gone, or never have left the
   * queue; the engine cannot tell, so it counts as gone
   */

  if (result == -ECANCELED && tr->busy && tr->what == TR_SEND)
    {
      tr->lost = true;
    }

  if (!tr_answered(tr, result, TR_SEND))
    {
      return;
    }

  if (result < 0)
    {
      if (tr_line(reply, "+CMS ERROR", why, sizeof(why), NULL) == NULL &&
          tr_line(reply, "+CME ERROR", why, sizeof(why), NULL) == NULL)
        {
          strlcpy(why, result == -ETIMEDOUT ? "no answer from the modem" :
                  "not sent", sizeof(why));
        }

      pnut_log(&tr->tel->module, PNUT_LOG_WARNING,
               "An SMS not sent, part %u of %u: %d", tr->out_part,
               tr->out_parts,
               result);
      tr_report(tr, PNUT_MESSAGES_MESSAGE_STATUS_FAILED, why);
      return;
    }

  tr->out_part++;
  tr_next(tr);
}

static void tr_send(FAR struct transport_s *tr)
{
  char command[24];
  int ret;

  if (tr->reporting)
    {
      tr_report_send(tr);         /* Again: Messages was away */
      return;
    }

  /* Every part sent: sent, whatever the modem did since */

  if (tr->out_parts > 0 && tr->out_part > tr->out_parts)
    {
      pnut_log(&tr->tel->module, PNUT_LOG_INFO, "An SMS sent, %u part%s",
               tr->out_parts, tr->out_parts == 1 ? "" : "s");
      tr_report(tr, PNUT_MESSAGES_MESSAGE_STATUS_SENT, "");
      return;
    }

  if (tr->lost)
    {
      tr_report(tr, PNUT_MESSAGES_MESSAGE_STATUS_FAILED,
                "the modem restarted while sending");
      return;
    }

  if (tr->out_parts == 0)
    {
      tr_report(tr, PNUT_MESSAGES_MESSAGE_STATUS_FAILED,
                "the text cannot be sent as SMS");
      return;
    }

  /* A part waits for the network (RFC 0029): the registration's change
   * goes on
   */

  if (!tr_network(tr))
    {
      return;
    }

  ret = sms_encode(tr->out.peer, tr->out.text, tr->ref, false, tr->out_part,
                   &tr->pdu);
  if (ret < 0)
    {
      tr_report(tr, PNUT_MESSAGES_MESSAGE_STATUS_FAILED,
                "the number or text cannot be sent as SMS");
      return;
    }

  snprintf(command, sizeof(command), "AT+CMGS=%u", tr->pdu.tpdu);
  tr->busy     = true;
  tr->what     = TR_SEND;
  tr->sent_gen = tr->gen;
  ret = at_command_text(&tr->tel->at, command, tr->pdu.hex, "+CMGS:",
                        TR_SEND_MS, tr_sent, tr);
  if (ret < 0)
    {
      tr->busy = false;
      tr->what = TR_NONE;
      tr_later(tr, TR_LATER_MS);
    }
}

/****************************************************************************
 * Name: tr_take, tr_take_opened, tr_taken
 *
 * Description:
 *   Messages' queue taken, with no credit at first: one is given while
 *   the modem has the network and nothing is out (RFC 0029).
 *
 ****************************************************************************/

static void tr_take_opened(FAR struct pnut_messages_client_s *client,
                           int status, FAR const pnut_error_t *err,
                           FAR void *arg)
{
  FAR struct transport_s *tr = arg;

  tr->take_out = false;
  if (status != PNUT_STATUS_OK)
    {
      pnut_log(&tr->tel->module, PNUT_LOG_WARNING,
               "Messages' queue not taken: %d", status);
      return;
    }

  tr->taking   = true;
  tr->credited = false;
  tr_next(tr);
}

static void tr_taken(FAR struct pnut_messages_client_s *client, int status,
                     FAR const pnut_messages_queued_t *item, FAR void *arg)
{
  FAR struct transport_s *tr = arg;
  bool ucs2;
  int n;

  if (status != PNUT_STATUS_OK)
    {
      pnut_log(&tr->tel->module, PNUT_LOG_INFO,
               "Messages' queue no longer taken: %d", status);
      tr->taking   = false;
      tr->credited = false;
      return;
    }

  tr->credited = false;
  if (tr->sending)
    {
      /* One credit at a time: never; it stays sending in Messages */

      pnut_log(&tr->tel->module, PNUT_LOG_WARNING,
               "A message handed over while another is out");
      return;
    }

  tr->out     = *item;
  tr->sending = true;
  tr->lost    = false;
  tr->out_part = 1;
  tr->ref++;
  n = sms_parts(item->text, &ucs2);
  tr->out_parts = n > 0 ? (uint8_t)n : 0;
  pnut_log(&tr->tel->module, PNUT_LOG_INFO, "An SMS to send, %u part%s",
           tr->out_parts, tr->out_parts == 1 ? "" : "s");
  tr_next(tr);
}

static void tr_take(FAR struct transport_s *tr)
{
  pnut_messages_take_request_t in;

  memset(&in, 0, sizeof(in));
  if (pnut_messages_take(&tr->messages, &in, 0, TR_REGISTER_MS,
                         tr_take_opened, tr_taken, tr, &tr->take) == 0)
    {
      tr->take_out = true;
    }
}

/****************************************************************************
 * Name: tr_next
 *
 * Description:
 *   The next thing to do, if nothing is out: the memory chosen, the
 *   message held delivered and deleted, the slots announced read, the
 *   memory read through, a long message waited for long enough given up.
 *   Until Messages has sms registered, the SMS stay in the modem.
 *
 ****************************************************************************/

static void tr_next(FAR struct transport_s *tr)
{
  bool sim;
  int slot;

  if (tr->busy || !tr->modem || tr->paused || tr->later != NULL)
    {
      return;
    }

  if (!tr->storage)
    {
      /* The SIM's first when it is to be looked at, never while a
       * message is held, whose slots are the memory's chosen: the join's
       * parts and the slots seen are the memory's they came from, so a
       * change of memory forgets them (the modem's are read again after)
       */

      sim = tr->holding ? tr->sim : tr->sim_check;
      if (sim != tr->sim)
        {
          memset(tr->seen, 0, sizeof(tr->seen));
          smsjoin_clear(&tr->join);
          tr->sim = sim;
        }

      tr_at(tr, sim ? TR_STORAGE_SIM : TR_STORAGE, "+CPMS:", TR_CHOOSE,
            tr_choose_done);
      return;
    }

  if (!tr->registered)
    {
      return;
    }

  if (tr->holding)
    {
      if (!tr->delivered)
        {
          tr_deliver(tr);
        }
      else
        {
          tr_delete(tr);
        }

      return;
    }

  /* The modem's announced slots wait while the SIM's memory is read */

  while (tr->count > 0 && !tr->sim)
    {
      slot = tr->queue[tr->head];
      tr->head = (uint8_t)((tr->head + 1) % TRANSPORT_QUEUE);
      tr->count--;
      if (!tr_seen(tr, slot))
        {
          tr_read(tr, slot, false);
          return;
        }
    }

  while (tr->scanning)
    {
      /* To total, inclusive: the modem may number its slots from 1 */

      if (tr->next > tr->total || tr->found >= tr->used)
        {
          /* Fewer than counted: some could not be read now, or one came
           * behind the scan; read through again at the tick, meanwhile
           * going on with what comes
           */

          tr->scanning = false;
          tr->again    = tr->again || tr->found < tr->used;
          if (tr->sim)
            {
              /* The SIM's read through: what it keeps (parts of a message
               * whose rest is not there, SMS not kept by Messages) stays,
               * looked at again at the next start; then the modem's
               */

              pnut_log(&tr->tel->module, PNUT_LOG_INFO,
                       "The SIM's SMS read through");
              tr->sim_check = false;
              tr->storage   = false;
              tr->again     = false;
              tr_next(tr);
              return;
            }

          break;
        }

      slot = tr->next++;
      if (tr_seen(tr, slot))
        {
          tr->found++;
          continue;
        }

      tr_read(tr, slot, true);
      return;
    }

  /* A long message given up on, before what leaves, which may wait for
   * the network a long while
   */

  if (smsjoin_expired(&tr->join, tr_now(), &tr->msg) == 1)
    {
      tr_hold(tr);
      tr_next(tr);
      return;
    }

  /* Then what leaves: the message out, or a credit for the next */

  if (tr->sending)
    {
      tr_send(tr);
      return;
    }

  if (tr->taking && !tr->credited && tr_network(tr) &&
      pnut_messages_take_credit(&tr->messages, tr->take, 1) == 0)
    {
      tr->credited = true;
    }
}

/****************************************************************************
 * Name: tr_register
 *
 * Description:
 *   sms registered on Messages' connection; tried again at the tick if it
 *   cannot be sent.
 *
 ****************************************************************************/

static void tr_registered(FAR struct pnut_messages_client_s *client,
                          int status, FAR const pnut_messages_reply_t *out,
                          FAR const pnut_error_t *err, FAR void *arg);

static void tr_register(FAR struct transport_s *tr)
{
  pnut_messages_register_request_t in;

  memset(&in, 0, sizeof(in));
  strlcpy(in.transport, TRANSPORT_NAME, sizeof(in.transport));
  tr->registering = true;
  if (pnut_messages_register_transport(&tr->messages, &in, TR_REGISTER_MS,
                                       tr_registered, tr) < 0)
    {
      tr->registering = false;
      pnut_log(&tr->tel->module, PNUT_LOG_WARNING,
               "sms not registered with Messages");
    }
}

/****************************************************************************
 * Name: tr_tick
 *
 * Description:
 *   Now and then: after a failure the memory is chosen and read through
 *   again, sms registered again if Messages refused it, and long messages
 *   waited for long enough given up.
 *
 ****************************************************************************/

static void tr_tick(FAR struct pnut_loop_s *loop,
                    FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct transport_s *tr = arg;

  if (tr->connected && !tr->registered && !tr->registering)
    {
      tr_register(tr);
    }

  if (tr->registered && !tr->taking && !tr->take_out)
    {
      tr_take(tr);
    }

  if (tr->paused || tr->again || (tr->sim_check && !tr->sim))
    {
      tr->paused  = false;
      tr->again   = false;
      tr->storage = false;
    }

  tr_next(tr);
}

/****************************************************************************
 * Name: tr_registered, tr_state
 *
 * Description:
 *   Messages' connection, and sms registered on each.
 *
 ****************************************************************************/

static void tr_registered(FAR struct pnut_messages_client_s *client,
                          int status, FAR const pnut_messages_reply_t *out,
                          FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct transport_s *tr = arg;

  tr->registering = false;

  /* Not yet: Messages starting (it could not tell who calls), busy, or
   * its store unread; asked again in a moment, not at the tick
   */

  if (status == PNUT_STATUS_UNAVAILABLE || status == PNUT_STATUS_BUSY ||
      status == PNUT_STATUS_TIMEOUT || status == PNUT_STATUS_INTERNAL)
    {
      pnut_log(&tr->tel->module, PNUT_LOG_INFO,
               "Messages cannot take sms yet: %d", status);
      tr_later(tr, TR_AGAIN_MS);
      return;
    }

  if (status != PNUT_STATUS_OK)
    {
      pnut_log(&tr->tel->module, PNUT_LOG_WARNING,
               "Messages refused sms: %d", status);
      return;
    }

  pnut_log(&tr->tel->module, PNUT_LOG_INFO, "sms registered with Messages");
  tr->registered = true;
  tr->taking     = false;
  tr->credited   = false;
  if (!tr->take_out)
    {
      tr_take(tr);
    }

  tr->storage    = false;         /* What came meanwhile: read through */
  tr->paused     = false;         /* A failure there was Messages' going */
  tr->refused    = false;
  tr_next(tr);
}

static void tr_state(FAR struct pnut_messages_client_s *client,
                     bool connected, FAR void *arg)
{
  FAR struct transport_s *tr = arg;

  tr->connected  = connected;
  tr->registered = false;
  tr->taking     = false;
  tr->take_out   = false;
  tr->credited   = false;
  if (connected)
    {
      tr_register(tr);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void transport_init(FAR struct transport_s *tr, FAR struct telephony_s *tel,
                    FAR const char *service, uint32_t tick)
{
  memset(tr, 0, sizeof(*tr));
  tr->tel       = tel;
  tr->service   = service;
  tr->tick      = tick != 0 ? tick : TRANSPORT_TICK;
  tr->sim_check = true;           /* SMS left on the SIM, moved over */

  /* Long messages' reference, from where the last start would not have
   * left it
   */

  tr->ref       = (uint8_t)(time(NULL) ^ tr_now());
}

int transport_start(FAR struct transport_s *tr,
                    FAR struct pnut_loop_s *loop)
{
  int ret;

  if (tr->service == NULL)
    {
      return OK;
    }

  ret = pnut_messages_connect(loop, tr->service, tr_state, tr,
                              &tr->messages);
  if (ret < 0)
    {
      pnut_log(&tr->tel->module, PNUT_LOG_WARNING,
               "No Messages: %d; SMS stay in the modem", ret);
      return ret;
    }

  tr->open = true;
  ret = pnut_timer_start(loop, tr->tick, tr->tick, tr_tick, tr, &tr->timer);
  if (ret < 0)
    {
      pnut_log(&tr->tel->module, PNUT_LOG_WARNING,
               "No tick: %d; SMS not read again after a failure", ret);
      tr->timer = NULL;
    }

  return OK;
}

void transport_stop(FAR struct transport_s *tr)
{
  FAR struct pnut_loop_s *loop = tr->tel->module.loop;

  if (tr->timer != NULL)
    {
      pnut_timer_cancel(loop, tr->timer);
      tr->timer = NULL;
    }

  if (tr->later != NULL)
    {
      pnut_timer_cancel(loop, tr->later);
      tr->later = NULL;
    }

  if (tr->open)
    {
      pnut_messages_disconnect(&tr->messages);
      tr->open = false;
    }

  tr->modem = false;
  tr->gen++;
  tr->busy  = false;
  tr->what  = TR_NONE;
  tr_release(tr, false);
  smsjoin_clear(&tr->join);
}

void transport_ready(FAR struct transport_s *tr)
{
  if (tr->service == NULL || tr->modem)
    {
      return;
    }

  tr->modem   = true;
  tr->storage = false;
  tr->tries   = 0;
  tr_next(tr);
}

void transport_gone(FAR struct transport_s *tr)
{
  if (!tr->modem)
    {
      return;
    }

  /* What is out is left aside; the memory is read through when it is
   * back, and what was joined, joined again
   */

  tr->modem     = false;
  tr->sim       = false;          /* The modem's choice goes with it */
  tr->sim_check = true;
  if (tr->sending && tr->out_part <= tr->out_parts &&
      (tr->what == TR_SEND || tr->out_part > 1))
    {
      tr->lost = true;            /* A part may have gone, not all */
    }

  tr->gen++;
  if (tr->what != TR_DELIVER && tr->what != TR_REPORT)
    {
      tr->busy = false;
      tr->what = TR_NONE;
    }

  tr->storage  = false;
  tr->scanning = false;
  tr->paused   = false;
  tr->again    = false;
  tr->count    = 0;
  memset(tr->seen, 0, sizeof(tr->seen));
  tr_release(tr, false);
  smsjoin_clear(&tr->join);
}

bool transport_urc(FAR struct transport_s *tr, FAR const char *line)
{
  int slot;

  if (strcmp(line, "SMS DONE") == 0)
    {
      /* The modem's SMS are ready: chosen and read through, now */

      if (tr->service != NULL && tr->modem && !tr->busy)
        {
          tr->storage = false;
          tr->tries   = 0;
          if (tr->later != NULL)
            {
              pnut_timer_cancel(tr->tel->module.loop, tr->later);
              tr->later = NULL;
            }

          /* The timer may have held a registration asked again */

          if (tr->connected && !tr->registered && !tr->registering)
            {
              tr_register(tr);
            }

          tr_next(tr);
        }

      return true;
    }

  if (strncmp(line, "+CMTI:", 6) != 0)
    {
      return false;
    }

  if (tr->service == NULL)
    {
      return true;
    }

  slot = tel_sms_slot(line);
  if (slot == -ENODEV)
    {
      /* Stored on the SIM: the memory's choice went, and is given again;
       * the SIM's SMS stay there
       */

      pnut_log(&tr->tel->module, PNUT_LOG_INFO,
               "An SMS stored on the SIM: read from there");
      tr->sim_check = true;
      tr->storage   = false;
    }
  else if (slot < 0)
    {
      pnut_log(&tr->tel->module, PNUT_LOG_WARNING, "A +CMTI not understood");
    }
  else if (tr->count >= TRANSPORT_QUEUE)
    {
      tr->storage = false;        /* Read through instead */
    }
  else
    {
      tr->queue[(tr->head + tr->count) % TRANSPORT_QUEUE] = (uint16_t)slot;
      tr->count++;
    }

  /* One more the scan may meet, or has read: counted, so that it does not
   * stop short of one counted at its start
   */

  if (slot >= 0 && tr->scanning && !tr->sim)
    {
      tr->used++;
    }

  tr_next(tr);
  return true;
}

void transport_kick(FAR struct transport_s *tr)
{
  if (tr->service != NULL)
    {
      tr_next(tr);
    }
}
