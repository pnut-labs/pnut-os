/****************************************************************************
 * pnut-os/src/telephony/smsjoin.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smsjoin.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: smsjoin_fnv
 *
 * Description:
 *   FNV-1a, 64 bits, over bytes, from a running value.
 *
 ****************************************************************************/

static uint64_t smsjoin_fnv(uint64_t h, FAR const void *data, size_t n)
{
  FAR const uint8_t *p = data;

  while (n-- > 0)
    {
      h ^= *p++;
      h *= 0x100000001b3ull;
    }

  return h;
}

/****************************************************************************
 * Name: smsjoin_drop
 *
 * Description:
 *   A long message's parts given back, and its place freed.
 *
 ****************************************************************************/

static void smsjoin_drop(FAR struct smsjoin_long_s *l)
{
  int i;

  for (i = 0; i < SMSJOIN_PARTS; i++)
    {
      free(l->parts[i].text);
    }

  memset(l, 0, sizeof(*l));
}

/****************************************************************************
 * Name: smsjoin_make
 *
 * Description:
 *   A long message's parts joined into out, a mark where one is missing,
 *   and the message taken out of the join.
 *
 * Returned Value:
 *   1; -ENOMEM, and the message stays.
 *
 ****************************************************************************/

static int smsjoin_make(FAR struct smsjoin_long_s *l,
                        FAR struct smsjoin_msg_s *out)
{
  size_t size = 1;
  uint8_t i;
  bool sent = false;

  memset(out, 0, sizeof(*out));
  for (i = 0; i < l->total; i++)
    {
      size += l->parts[i].in ? strlen(l->parts[i].text) :
              strlen(SMSJOIN_GAP);
    }

  out->text = malloc(size);
  if (out->text == NULL)
    {
      return -ENOMEM;
    }

  out->text[0] = '\0';
  strlcpy(out->sender, l->sender, sizeof(out->sender));
  out->binary = l->binary;
  for (i = 0; i < l->total; i++)
    {
      if (!l->parts[i].in)
        {
          out->gaps = true;
          if (!l->binary)
            {
              strlcat(out->text, SMSJOIN_GAP, size);
            }

          continue;
        }

      /* The first part's time stamp, or the first part in's */

      if (!sent)
        {
          out->sent = l->parts[i].sent;
          sent = true;
        }

      out->slots[out->nslots++] = l->parts[i].slot;
      strlcat(out->text, l->parts[i].text, size);
    }

  smsjoin_key(out->sender, out->sent, l->ref, 0, out->text, out->key);
  smsjoin_drop(l);
  return 1;
}

/****************************************************************************
 * Name: smsjoin_alone
 *
 * Description:
 *   One part delivered as a message of its own: a whole message, or a
 *   part of one too long to join.
 *
 ****************************************************************************/

static int smsjoin_alone(FAR const struct sms_deliver_s *part, int16_t slot,
                         FAR struct smsjoin_msg_s *out)
{
  memset(out, 0, sizeof(*out));
  out->text = strdup(part->text);
  if (out->text == NULL)
    {
      return -ENOMEM;
    }

  strlcpy(out->sender, part->sender, sizeof(out->sender));
  out->sent     = part->sent;
  out->binary   = part->binary;
  out->gaps     = part->total != 0;
  out->nslots   = 1;
  out->slots[0] = slot;
  smsjoin_key(out->sender, out->sent, part->ref, part->part, out->text,
              out->key);
  return 1;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void smsjoin_key(FAR const char *sender, int64_t sent, uint16_t ref,
                 uint8_t part, FAR const char *text, FAR char *key)
{
  uint64_t a = 0xcbf29ce484222325ull;
  uint64_t b = 0xcbf29ce484222325ull;
  uint8_t stamp[11];
  int i;

  for (i = 0; i < 8; i++)
    {
      stamp[i] = (uint8_t)((uint64_t)sent >> (8 * i));
    }

  stamp[8]  = (uint8_t)ref;
  stamp[9]  = (uint8_t)(ref >> 8);
  stamp[10] = part;
  a = smsjoin_fnv(a, sender, strlen(sender) + 1);
  a = smsjoin_fnv(a, stamp, sizeof(stamp));
  b = smsjoin_fnv(b, text, strlen(text));
  snprintf(key, SMSJOIN_KEY + 1, "%016" PRIx64 "%016" PRIx64, a, b);
}

int smsjoin_add(FAR struct smsjoin_s *join,
                FAR const struct sms_deliver_s *part, int16_t slot,
                uint64_t now, FAR struct smsjoin_msg_s *out)
{
  FAR struct smsjoin_long_s *l = NULL;
  FAR struct smsjoin_long_s *free_long = NULL;
  FAR struct smsjoin_part_s *p;
  bool evicted = false;
  int i;

  /* A whole message: delivered as it is */

  if (part->total == 0)
    {
      return smsjoin_alone(part, slot, out);
    }

  if (part->part < 1 || part->part > part->total)
    {
      return -EINVAL;
    }

  /* Longer than the join takes: each part on its own, marked as one */

  if (part->total > SMSJOIN_PARTS)
    {
      return smsjoin_alone(part, slot, out);
    }

  for (i = 0; i < SMSJOIN_MESSAGES; i++)
    {
      if (!join->longs[i].used)
        {
          if (free_long == NULL)
            {
              free_long = &join->longs[i];
            }

          continue;
        }

      if (join->longs[i].ref == part->ref &&
          join->longs[i].ref16 == part->ref16 &&
          join->longs[i].total == part->total &&
          strcmp(join->longs[i].sender, part->sender) == 0)
        {
          l = &join->longs[i];
          break;
        }
    }

  /* No room: the long message that has waited longest is given up, as a
   * day's wait would, so that parts never sent the rest of, as a flood
   * may, cannot keep every other long message out
   */

  if (l == NULL && free_long == NULL)
    {
      free_long = &join->longs[0];
      for (i = 1; i < SMSJOIN_MESSAGES; i++)
        {
          /* The oldest by its first part's time, then by when it came
           * (parts read at start come in the same second)
           */

          if (join->longs[i].first < free_long->first ||
              (join->longs[i].first == free_long->first &&
               (int32_t)(join->longs[i].seq - free_long->seq) < 0))
            {
              free_long = &join->longs[i];
            }
        }

      if (smsjoin_make(free_long, out) < 0)
        {
          return -ENOMEM;
        }

      evicted = true;
    }

  if (l == NULL)
    {
      l = free_long;
      memset(l, 0, sizeof(*l));
      l->used  = true;
      l->ref16 = part->ref16;
      l->ref   = part->ref;
      l->total = part->total;
      l->first = now;
      l->seq   = join->seq++;
      strlcpy(l->sender, part->sender, sizeof(l->sender));
    }

  p = &l->parts[part->part - 1];
  if (p->in)
    {
      if (p->slot != slot)
        {
          /* The same part again: a copy whose slot goes; another text:
           * another message under the same reference, on its own
           */

          return strcmp(p->text, part->text) == 0 ? SMSJOIN_COPY :
                 smsjoin_alone(part, slot, out);
        }

      /* Read again.  A message complete already, whose joining ran out
       * of memory, goes from smsjoin_expired(): joined here, the parts
       * read after this one would start it anew
       */

      return 0;
    }

  p->text = strdup(part->text);
  if (p->text == NULL)
    {
      if (l->count == 0)
        {
          smsjoin_drop(l);
        }

      return evicted ? SMSJOIN_EVICTED_ONLY : -ENOMEM;
    }

  p->in   = true;
  p->slot = slot;
  p->sent = part->sent;
  l->binary = l->binary || part->binary;
  l->count++;

  if (evicted)
    {
      return SMSJOIN_EVICTED;     /* A new long one, of 2 parts or more */
    }

  if (l->count < l->total)
    {
      return 0;
    }

  return smsjoin_make(l, out);
}

int smsjoin_expired(FAR struct smsjoin_s *join, uint64_t now,
                    FAR struct smsjoin_msg_s *out)
{
  int i;

  for (i = 0; i < SMSJOIN_MESSAGES; i++)
    {
      if (join->longs[i].used &&
          (join->longs[i].count == join->longs[i].total ||
           (now >= join->longs[i].first &&
            now - join->longs[i].first >= SMSJOIN_WAIT)) &&
          smsjoin_make(&join->longs[i], out) == 1)
        {
          return 1;
        }
    }

  return 0;
}

void smsjoin_free(FAR struct smsjoin_msg_s *msg)
{
  free(msg->text);
  msg->text = NULL;
}

void smsjoin_clear(FAR struct smsjoin_s *join)
{
  int i;

  for (i = 0; i < SMSJOIN_MESSAGES; i++)
    {
      smsjoin_drop(&join->longs[i]);
    }
}
