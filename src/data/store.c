/****************************************************************************
 * pnut-os/src/data/store.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "store.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* A segment's file: its conversation's id and its number, in hex */

#define STORE_NAME_FMT      "%016" PRIx64 "-%08" PRIx32 ".log"

/* The index grows by these, so that it is not reallocated at every
 * message
 */

#define STORE_GROW_MSGS     32
#define STORE_GROW_SEGS     4
#define STORE_GROW_CONVS    8
#define STORE_GROW_REASONS  4

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* The segments found, sorted so that each conversation's load in order */

struct store_found_s
{
  uint64_t conversation;
  uint32_t segment;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: store_crc
 *
 * Description:
 *   CRC-32 (IEEE 802.3), bit by bit: the records are small, and a table
 *   would be 1 KB for no need.
 *
 ****************************************************************************/

static uint32_t store_crc(uint32_t crc, FAR const uint8_t *p, size_t n)
{
  int i;

  crc = ~crc;
  while (n-- > 0)
    {
      crc ^= *p++;
      for (i = 0; i < 8; i++)
        {
          crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
        }
    }

  return ~crc;
}

/****************************************************************************
 * Name: store_hash
 *
 * Description:
 *   A key, hashed (FNV-1a, 64 bits); never 0, which is no key.
 *
 ****************************************************************************/

static uint64_t store_hash(FAR const char *key)
{
  uint64_t h = 0xcbf29ce484222325ull;

  if (key[0] == '\0')
    {
      return 0;
    }

  while (*key != '\0')
    {
      h ^= (uint8_t)*key++;
      h *= 0x100000001b3ull;
    }

  return h != 0 ? h : 1;
}

/****************************************************************************
 * Name: store_grow
 *
 * Description:
 *   Room for one more element in an array that grows by step.
 *
 ****************************************************************************/

static int store_grow(FAR void **array, FAR uint32_t *cap, uint32_t n,
                      size_t size, uint32_t step)
{
  FAR void *p;

  if (n < *cap)
    {
      return OK;
    }

  p = realloc(*array, (size_t)(*cap + step) * size);
  if (p == NULL)
    {
      return -ENOMEM;
    }

  *array = p;
  *cap  += step;
  return OK;
}

/****************************************************************************
 * Name: store_path
 ****************************************************************************/

static int store_path(FAR const struct store_s *store,
                      uint64_t conversation, uint32_t segment,
                      FAR char *path)
{
  int n;

  n = snprintf(path, STORE_PATH_MAX, "%s/" STORE_NAME_FMT, store->dir,
               conversation, segment);
  return n > 0 && n < STORE_PATH_MAX ? OK : -ENAMETOOLONG;
}

/****************************************************************************
 * Name: store_latest_set
 *
 * Description:
 *   The start of a text, cut at a whole UTF-8 character.
 *
 ****************************************************************************/

static void store_latest_set(FAR struct store_conv_s *conv,
                             FAR const char *text)
{
  size_t n = strlen(text);

  if (n > STORE_LATEST_MAX)
    {
      n = STORE_LATEST_MAX;
      while (n > 0 && ((uint8_t)text[n] & 0xc0) == 0x80)
        {
          n--;
        }
    }

  memcpy(conv->latest, text, n);
  conv->latest[n] = '\0';
  conv->stale = false;
}

/****************************************************************************
 * Name: store_index_of
 ****************************************************************************/

static int store_index_of(FAR struct store_s *store, uint64_t conversation)
{
  uint32_t i;

  for (i = 0; i < store->nconvs; i++)
    {
      if (store->convs[i].id == conversation)
        {
          return (int)i;
        }
    }

  return -ENOENT;
}

/****************************************************************************
 * Name: store_place
 *
 * Description:
 *   Move a conversation to its place by its newest message, newest first,
 *   after its newest changed.
 *
 ****************************************************************************/

static void store_place(FAR struct store_s *store, uint32_t i)
{
  struct store_conv_s conv = store->convs[i];

  while (i > 0 && store->convs[i - 1].newest < conv.newest)
    {
      store->convs[i] = store->convs[i - 1];
      i--;
    }

  while (i + 1 < store->nconvs && store->convs[i + 1].newest > conv.newest)
    {
      store->convs[i] = store->convs[i + 1];
      i++;
    }

  store->convs[i] = conv;
}

/****************************************************************************
 * Name: store_conv_free
 ****************************************************************************/

static void store_conv_free(FAR struct store_conv_s *conv)
{
  free(conv->msgs);
  free(conv->segs);
  conv->msgs = NULL;
  conv->segs = NULL;
}

/****************************************************************************
 * Name: store_segment
 *
 * Description:
 *   A conversation's segment by its number, added if new.
 *
 ****************************************************************************/

static FAR struct store_segment_s *
store_segment(FAR struct store_conv_s *conv, uint32_t number, bool add)
{
  uint32_t i;

  for (i = 0; i < conv->nsegs; i++)
    {
      if (conv->segs[i].number == number)
        {
          return &conv->segs[i];
        }
    }

  if (!add || store_grow((FAR void **)&conv->segs, &conv->segs_cap,
                         conv->nsegs, sizeof(*conv->segs),
                         STORE_GROW_SEGS) < 0)
    {
      return NULL;
    }

  /* Kept in their order */

  i = conv->nsegs++;
  while (i > 0 && conv->segs[i - 1].number > number)
    {
      conv->segs[i] = conv->segs[i - 1];
      i--;
    }

  conv->segs[i].number = number;
  conv->segs[i].size   = 0;
  conv->segs[i].dead   = 0;
  return &conv->segs[i];
}

/****************************************************************************
 * Name: store_reason_set
 *
 * Description:
 *   A failed message's reason kept, or dropped when it has none.
 *
 ****************************************************************************/

static void store_reason_set(FAR struct store_s *store, uint64_t message,
                             FAR const char *reason)
{
  uint32_t i;

  for (i = 0; i < store->nreasons; i++)
    {
      if (store->reasons[i].id == message)
        {
          break;
        }
    }

  if (reason == NULL || reason[0] == '\0')
    {
      if (i < store->nreasons)
        {
          store->reasons[i] = store->reasons[--store->nreasons];
        }

      return;
    }

  if (i == store->nreasons)
    {
      if (store_grow((FAR void **)&store->reasons, &store->reasons_cap,
                     store->nreasons, sizeof(*store->reasons),
                     STORE_GROW_REASONS) < 0)
        {
          return;                 /* Only the reason is lost */
        }

      store->nreasons++;
    }

  store->reasons[i].id = message;
  strlcpy(store->reasons[i].reason, reason,
          sizeof(store->reasons[i].reason));
}

/****************************************************************************
 * Name: store_is_unread
 ****************************************************************************/

static bool store_is_unread(FAR const struct store_msg_s *msg)
{
  return (msg->flags & (STORE_OUT | STORE_DELETED)) == 0 &&
         msg->status == PNUT_MESSAGES_MESSAGE_STATUS_UNREAD;
}

/****************************************************************************
 * Name: store_may_go
 *
 * Description:
 *   A message that may go to make room (RFC 0029).
 *
 ****************************************************************************/

static bool store_may_go(FAR const struct store_msg_s *msg)
{
  if (msg->flags & STORE_DELETED)
    {
      return true;
    }

  if ((msg->flags & STORE_OUT) == 0)
    {
      return msg->status == PNUT_MESSAGES_MESSAGE_STATUS_READ;
    }

  return msg->status == PNUT_MESSAGES_MESSAGE_STATUS_SENT ||
         msg->status == PNUT_MESSAGES_MESSAGE_STATUS_DELIVERED ||
         msg->status == PNUT_MESSAGES_MESSAGE_STATUS_FAILED ||
         msg->status == PNUT_MESSAGES_MESSAGE_STATUS_UNCONFIRMED;
}

/****************************************************************************
 * Name: store_newest
 *
 * Description:
 *   A conversation's newest message, deleted ones apart, found again; its
 *   text is to be read again unless it is the one already known.
 *
 ****************************************************************************/

static void store_newest(FAR struct store_conv_s *conv)
{
  uint64_t was = conv->newest;
  uint32_t i = conv->nmsgs;

  conv->newest = 0;
  while (i-- > 0)
    {
      if ((conv->msgs[i].flags & STORE_DELETED) == 0)
        {
          conv->newest = conv->msgs[i].id;
          break;
        }
    }

  if (conv->newest != was)
    {
      conv->latest[0] = '\0';
      conv->stale     = conv->newest != 0;
    }
}

/****************************************************************************
 * Name: store_apply
 *
 * Description:
 *   A record applied to the index: the conversation's, read from its
 *   segment at offset, size bytes framed.  A record about a message the
 *   index does not have, gone to make room, changes nothing.
 *
 ****************************************************************************/

static int store_apply(FAR struct store_s *store, uint64_t conversation,
                       uint32_t segment, uint32_t offset, uint16_t size,
                       FAR const pnut_messages_record_t *rec, bool loading)
{
  FAR struct store_conv_s *conv;
  FAR struct store_segment_s *seg;
  FAR struct store_msg_s *msg;
  uint32_t i;
  int idx;

  idx = store_index_of(store, conversation);
  if (rec->which_kind == PNUT_MESSAGES_RECORD_HEAD_TAG)
    {
      FAR const pnut_messages_record_head_t *head = &rec->kind.head;

      if (idx < 0)
        {
          if (store_grow((FAR void **)&store->convs, &store->convs_cap,
                         store->nconvs, sizeof(*store->convs),
                         STORE_GROW_CONVS) < 0)
            {
              return -ENOMEM;
            }

          idx  = (int)store->nconvs++;
          conv = &store->convs[idx];
          memset(conv, 0, sizeof(*conv));
          conv->id = conversation;
          strlcpy(conv->transport, head->transport,
                  sizeof(conv->transport));
          strlcpy(conv->peer, head->peer, sizeof(conv->peer));
          if (conversation >= store->next_conv)
            {
              store->next_conv = conversation + 1;
            }
        }

      conv = &store->convs[idx];
      return store_segment(conv, segment, true) != NULL ? OK : -ENOMEM;
    }

  if (idx < 0)
    {
      return -ENOENT;
    }

  conv = &store->convs[idx];
  switch (rec->which_kind)
    {
      case PNUT_MESSAGES_RECORD_MESSAGE_TAG:
        {
          FAR const pnut_messages_message_record_t *m = &rec->kind.message;

          if (store_grow((FAR void **)&conv->msgs, &conv->msgs_cap,
                         conv->nmsgs, sizeof(*conv->msgs),
                         STORE_GROW_MSGS) < 0)
            {
              return -ENOMEM;
            }

          /* Ids grow: a message is always the conversation's newest */

          msg = &conv->msgs[conv->nmsgs++];
          memset(msg, 0, sizeof(*msg));
          msg->id      = m->id;
          msg->key     = store_hash(m->key);
          msg->segment = segment;
          msg->offset  = offset;
          msg->size    = size;
          msg->status  = (uint8_t)m->status;
          msg->flags   = m->out ? STORE_OUT : 0;

          /* A message its transport had when the device reset is not
           * known to be sent (RFC 0029)
           */

          if (loading && m->out &&
              m->status == PNUT_MESSAGES_MESSAGE_STATUS_SENDING)
            {
              msg->status = PNUT_MESSAGES_MESSAGE_STATUS_UNCONFIRMED;
            }

          if (!m->out && m->id <= conv->read)
            {
              msg->status = PNUT_MESSAGES_MESSAGE_STATUS_READ;
            }

          conv->count++;
          if (store_is_unread(msg))
            {
              conv->unread++;
            }

          conv->newest = m->id;
          conv->stored = m->stored;
          store_latest_set(conv, m->text);
          if (m->status == PNUT_MESSAGES_MESSAGE_STATUS_FAILED)
            {
              store_reason_set(store, m->id, m->reason);
            }

          if (m->id >= store->next_msg)
            {
              store->next_msg = m->id + 1;
            }
        }
        break;

      case PNUT_MESSAGES_RECORD_STATUS_TAG:
        msg = store_msg(conv, rec->kind.status.id);
        if (msg == NULL || (msg->flags & STORE_DELETED))
          {
            break;
          }

        if (store_is_unread(msg))
          {
            conv->unread--;
          }

        msg->status = (uint8_t)rec->kind.status.status;
        if (loading && (msg->flags & STORE_OUT) &&
            msg->status == PNUT_MESSAGES_MESSAGE_STATUS_SENDING)
          {
            msg->status = PNUT_MESSAGES_MESSAGE_STATUS_UNCONFIRMED;
          }

        if (store_is_unread(msg))
          {
            conv->unread++;
          }

        store_reason_set(store, msg->id,
                         msg->status == PNUT_MESSAGES_MESSAGE_STATUS_FAILED ?
                         rec->kind.status.reason : NULL);
        break;

      case PNUT_MESSAGES_RECORD_READ_TAG:
        if (rec->kind.read.id > conv->read)
          {
            conv->read = rec->kind.read.id;
          }

        for (i = 0; i < conv->nmsgs; i++)
          {
            msg = &conv->msgs[i];
            if (msg->id <= conv->read && store_is_unread(msg))
              {
                msg->status = PNUT_MESSAGES_MESSAGE_STATUS_READ;
                conv->unread--;
              }
          }
        break;

      case PNUT_MESSAGES_RECORD_DELETED_TAG:
        msg = store_msg(conv, rec->kind.deleted.id);
        if (msg == NULL || (msg->flags & STORE_DELETED))
          {
            break;
          }

        if (store_is_unread(msg))
          {
            conv->unread--;
          }

        msg->flags |= STORE_DELETED;
        conv->count--;
        store_reason_set(store, msg->id, NULL);
        seg = store_segment(conv, msg->segment, false);
        if (seg != NULL)
          {
            seg->dead += msg->size;
          }

        if (msg->id == conv->newest)
          {
            store_newest(conv);
          }
        break;

      default:
        break;
    }

  return OK;
}

/****************************************************************************
 * Name: store_read_file
 *
 * Description:
 *   A whole file into memory, which the caller frees.
 *
 ****************************************************************************/

static int store_read_file(FAR const char *path, FAR uint8_t **datap,
                           FAR size_t *lenp)
{
  struct stat st;
  FAR uint8_t *data;
  size_t done = 0;
  ssize_t n;
  int fd;

  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      return -errno;
    }

  if (fstat(fd, &st) < 0)
    {
      n = -errno;
      close(fd);
      return (int)n;
    }

  data = malloc(st.st_size > 0 ? (size_t)st.st_size : 1);
  if (data == NULL)
    {
      close(fd);
      return -ENOMEM;
    }

  while (done < (size_t)st.st_size)
    {
      n = read(fd, data + done, (size_t)st.st_size - done);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          break;
        }

      done += (size_t)n;
    }

  close(fd);
  if (done < (size_t)st.st_size)
    {
      free(data);
      return -EIO;                /* Not the whole file: none of it */
    }

  *datap = data;
  *lenp  = done;
  return OK;
}

/****************************************************************************
 * Name: store_cut
 *
 * Description:
 *   A segment cut back to its last whole record.
 *
 ****************************************************************************/

static void store_cut(FAR const char *path, size_t len)
{
  int fd;

  fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd >= 0)
    {
      if (ftruncate(fd, (off_t)len) == 0)
        {
          fsync(fd);
        }

      close(fd);
    }
}

/****************************************************************************
 * Name: store_load_segment
 *
 * Description:
 *   One segment's records applied, in order.  The first must be its head,
 *   naming the conversation its file names.
 *
 ****************************************************************************/

static int store_load_segment(FAR struct store_s *store,
                              FAR const char *path, uint64_t conversation,
                              uint32_t segment)
{
  FAR pnut_messages_record_t *rec;
  FAR struct store_segment_s *seg;
  FAR uint8_t *data;
  size_t len;
  size_t pos = 0;
  int ret;
  int n;
  int idx;

  ret = store_read_file(path, &data, &len);
  if (ret < 0)
    {
      return ret;
    }

  /* Made by a write a reset cut short, before its head was in */

  if (len == 0)
    {
      free(data);
      unlink(path);
      return OK;
    }

  rec = malloc(sizeof(*rec));
  if (rec == NULL)
    {
      free(data);
      return -ENOMEM;
    }

  while (pos < len)
    {
      n = store_unframe(data + pos, len - pos, rec);
      if (n < 0)
        {
          break;
        }

      if (pos == 0 && (rec->which_kind != PNUT_MESSAGES_RECORD_HEAD_TAG ||
                       rec->kind.head.conversation != conversation))
        {
          break;
        }

      ret = store_apply(store, conversation, segment, (uint32_t)pos,
                        (uint16_t)n, rec, true);
      if (ret == -ENOMEM)
        {
          break;
        }

      pos += (size_t)n;
    }

  free(rec);
  free(data);
  if (ret == -ENOMEM)
    {
      return ret;
    }

  if (pos == 0)
    {
      store->bad++;               /* No head: left aside, as it is */
      return OK;
    }

  if (pos < len)
    {
      store_cut(path, pos);
      store->cut++;
    }

  idx = store_index_of(store, conversation);
  seg = idx >= 0 ? store_segment(&store->convs[idx], segment, false) : NULL;
  if (seg != NULL)
    {
      seg->size     = (uint32_t)pos;
      store->bytes += pos;
    }

  return OK;
}

/****************************************************************************
 * Name: store_name
 *
 * Description:
 *   A segment's conversation and number from its file's name.
 *
 ****************************************************************************/

static bool store_name(FAR const char *name, FAR uint64_t *conversation,
                       FAR uint32_t *segment)
{
  char check[STORE_NAME_LEN + 1];

  if (strlen(name) != STORE_NAME_LEN ||
      sscanf(name, "%16" SCNx64 "-%8" SCNx32, conversation, segment) != 2)
    {
      return false;
    }

  snprintf(check, sizeof(check), STORE_NAME_FMT, *conversation, *segment);
  return strcmp(check, name) == 0 && *conversation != 0 && *segment != 0;
}

/****************************************************************************
 * Name: store_found_cmp, store_conv_cmp
 *
 * Description:
 *   The order segments load in, and conversations' order, the newest
 *   message's first.
 *
 ****************************************************************************/

static int store_found_cmp(FAR const void *a, FAR const void *b)
{
  FAR const struct store_found_s *x = a;
  FAR const struct store_found_s *y = b;

  if (x->conversation != y->conversation)
    {
      return x->conversation < y->conversation ? -1 : 1;
    }

  return x->segment < y->segment ? -1 : x->segment > y->segment;
}

static int store_conv_cmp(FAR const void *a, FAR const void *b)
{
  FAR const struct store_conv_s *x = a;
  FAR const struct store_conv_s *y = b;

  return x->newest > y->newest ? -1 : x->newest < y->newest;
}

/****************************************************************************
 * Name: store_frame_into
 *
 * Description:
 *   A record framed at the end of a plan's data.
 *
 ****************************************************************************/

static int store_frame_into(FAR struct store_write_s *w,
                            FAR const pnut_messages_record_t *rec)
{
  int n;

  n = store_frame(rec, w->data + w->len, sizeof(w->data) - w->len);
  if (n < 0)
    {
      return n;
    }

  w->len += (uint16_t)n;
  return OK;
}

/****************************************************************************
 * Name: store_plan_into
 *
 * Description:
 *   A record for a conversation, planned at the end of its last segment,
 *   or of a new one, with its head, when the last would pass its size.
 *
 ****************************************************************************/

static int store_plan_into(FAR struct store_s *store,
                           FAR struct store_conv_s *conv,
                           FAR const pnut_messages_record_t *rec,
                           FAR struct store_write_s *w)
{
  FAR pnut_messages_record_t *head = &store->head;
  FAR struct store_segment_s *last;
  uint16_t len;
  int ret;

  w->len = 0;
  w->conversation = conv->id;
  ret = store_frame_into(w, rec);
  if (ret < 0)
    {
      return ret;
    }

  len  = w->len;
  last = conv->nsegs > 0 ? &conv->segs[conv->nsegs - 1] : NULL;
  if (last != NULL && last->size + len <= store->segmax)
    {
      w->create  = false;
      w->segment = last->number;
      w->expect  = last->size;
      return store_path(store, conv->id, w->segment, w->path);
    }

  /* A new segment: its head first */

  memset(head, 0, sizeof(*head));
  head->which_kind = PNUT_MESSAGES_RECORD_HEAD_TAG;
  head->kind.head.version      = STORE_VERSION;
  head->kind.head.conversation = conv->id;
  strlcpy(head->kind.head.transport, conv->transport,
          sizeof(head->kind.head.transport));
  strlcpy(head->kind.head.peer, conv->peer, sizeof(head->kind.head.peer));

  w->len = 0;
  ret = store_frame_into(w, head);
  if (ret >= 0)
    {
      ret = store_frame_into(w, rec);
    }

  if (ret < 0)
    {
      return ret;
    }

  w->create  = true;
  w->expect  = 0;
  w->segment = last != NULL ? last->number + 1 : 1;
  return store_path(store, conv->id, w->segment, w->path);
}

/****************************************************************************
 * Name: store_owner
 *
 * Description:
 *   The conversation that has a message, or NULL.
 *
 ****************************************************************************/

static FAR struct store_conv_s *store_owner(FAR struct store_s *store,
                                            uint64_t message,
                                            FAR struct store_msg_s **msgp)
{
  FAR struct store_msg_s *msg;
  uint32_t i;

  for (i = 0; i < store->nconvs; i++)
    {
      msg = store_msg(&store->convs[i], message);
      if (msg != NULL)
        {
          *msgp = msg;
          return &store->convs[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: store_find_id
 *
 * Description:
 *   An element of an array sorted by a 64-bit id it begins with, found by
 *   its id, or NULL.
 *
 ****************************************************************************/

static FAR void *store_find_id(FAR const void *array, uint32_t n,
                               size_t size, uint64_t id)
{
  FAR const uint8_t *base = array;
  uint32_t lo = 0;
  uint32_t hi = n;
  uint32_t mid;
  uint64_t v;

  while (lo < hi)
    {
      mid = lo + (hi - lo) / 2;
      memcpy(&v, base + (size_t)mid * size, sizeof(v));
      if (v < id)
        {
          lo = mid + 1;
        }
      else
        {
          hi = mid;
        }
    }

  if (lo < n)
    {
      memcpy(&v, base + (size_t)lo * size, sizeof(v));
      if (v == id)
        {
          return (FAR void *)(base + (size_t)lo * size);
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: store_plan_rewrite
 *
 * Description:
 *   A segment's rewrite planned: the messages it keeps, with their state
 *   now, and the conversation's messages in its other segments, whose
 *   records stay.  To make room, every message that may go goes; to
 *   compact, the deleted ones.
 *
 ****************************************************************************/

static int store_plan_rewrite(FAR struct store_s *store, uint32_t idx,
                              uint32_t segment, bool room,
                              FAR struct store_rewrite_s *rw)
{
  FAR struct store_conv_s *conv = &store->convs[idx];
  FAR struct store_msg_s *msg;
  uint32_t i;
  uint32_t k;
  uint32_t l;
  bool goes;
  int ret;

  memset(rw, 0, sizeof(*rw));
  rw->conversation = conv->id;
  rw->segment      = segment;
  ret = store_path(store, conv->id, segment, rw->path);
  if (ret < 0 ||
      snprintf(rw->temp, sizeof(rw->temp), "%s" STORE_TEMP, rw->path) >=
      (int)sizeof(rw->temp))
    {
      return -ENAMETOOLONG;
    }

  for (i = 0; i < conv->nmsgs; i++)
    {
      msg  = &conv->msgs[i];
      goes = room ? store_may_go(msg) : (msg->flags & STORE_DELETED) != 0;
      if (msg->segment != segment)
        {
          rw->nlive++;
        }
      else if (!goes)
        {
          rw->n++;
        }
    }

  rw->kept = calloc(rw->n > 0 ? rw->n : 1, sizeof(*rw->kept));
  rw->live = calloc(rw->nlive > 0 ? rw->nlive : 1, sizeof(*rw->live));
  if (rw->kept == NULL || rw->live == NULL)
    {
      free(rw->kept);
      free(rw->live);
      rw->kept = NULL;
      rw->live = NULL;
      return -ENOMEM;
    }

  /* In id order, as the index keeps them */

  for (i = 0, k = 0, l = 0; i < conv->nmsgs; i++)
    {
      msg  = &conv->msgs[i];
      goes = room ? store_may_go(msg) : (msg->flags & STORE_DELETED) != 0;
      if (msg->segment != segment)
        {
          rw->live[l++] = msg->id;
        }
      else if (!goes)
        {
          rw->kept[k].id     = msg->id;
          rw->kept[k].status = msg->status;
          strlcpy(rw->kept[k].reason, store_reason(store, msg->id),
                  sizeof(rw->kept[k].reason));
          k++;
        }
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void store_init(FAR struct store_s *store, FAR const char *dir,
                uint64_t limit, uint32_t segmax)
{
  memset(store, 0, sizeof(*store));
  if (strlcpy(store->dir, dir, sizeof(store->dir)) >= sizeof(store->dir))
    {
      store->dir[0] = '\0';        /* store_load() refuses it */
    }

  store->limit     = limit;
  store->segmax    = segmax;
  store->next_msg  = 1;
  store->next_conv = 1;
}

void store_free(FAR struct store_s *store)
{
  uint32_t i;

  for (i = 0; i < store->nconvs; i++)
    {
      store_conv_free(&store->convs[i]);
    }

  free(store->convs);
  free(store->reasons);
  store->convs    = NULL;
  store->reasons  = NULL;
  store->nconvs   = 0;
  store->nreasons = 0;
  store->bytes    = 0;
}

int store_load(FAR struct store_s *store)
{
  FAR struct store_found_s *found = NULL;
  FAR struct dirent *de;
  uint32_t nfound = 0;
  uint32_t cap = 0;
  char path[STORE_PATH_MAX];
  size_t len;
  uint32_t i;
  DIR *dir;
  int ret = OK;

  if (store->dir[0] == '\0')
    {
      return -ENAMETOOLONG;
    }

  if (mkdir(store->dir, 0700) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  dir = opendir(store->dir);
  if (dir == NULL)
    {
      return -errno;
    }

  while ((de = readdir(dir)) != NULL)
    {
      uint64_t conversation;
      uint32_t segment;

      len = strlen(de->d_name);
      if (len > strlen(STORE_TEMP) &&
          strcmp(de->d_name + len - strlen(STORE_TEMP), STORE_TEMP) == 0)
        {
          /* A rewrite a reset cut short: the segment is as it was */

          if (snprintf(path, sizeof(path), "%s/%s", store->dir,
                       de->d_name) < (int)sizeof(path))
            {
              unlink(path);
            }

          continue;
        }

      if (!store_name(de->d_name, &conversation, &segment))
        {
          continue;
        }

      if (store_grow((FAR void **)&found, &cap, nfound, sizeof(*found),
                     64) < 0)
        {
          ret = -ENOMEM;
          break;
        }

      found[nfound].conversation = conversation;
      found[nfound].segment      = segment;
      nfound++;
    }

  closedir(dir);
  if (ret < 0)
    {
      free(found);
      return ret;
    }

  if (nfound > 1)
    {
      qsort(found, nfound, sizeof(*found), store_found_cmp);
    }

  for (i = 0; i < nfound && ret >= 0; i++)
    {
      ret = store_path(store, found[i].conversation, found[i].segment,
                       path);
      if (ret >= 0)
        {
          ret = store_load_segment(store, path, found[i].conversation,
                                   found[i].segment);
        }

      if (ret == -ENOENT)
        {
          ret = OK;               /* Gone meanwhile */
        }

      /* Any other failure fails the load: a file left out of the index
       * could be written over, as a new segment of its number
       */
    }

  free(found);
  if (ret < 0)
    {
      return ret;
    }

  /* A conversation whose newest was deleted: the text of the one before
   * it read now, on the worker, as store_latest() does on the loop
   */

  for (i = 0; i < store->nconvs; i++)
    {
      FAR struct store_read_s *r;

      if (!store->convs[i].stale)
        {
          continue;
        }

      r = malloc(sizeof(*r));
      if (r == NULL)
        {
          return -ENOMEM;
        }

      if (store_plan_latest(store, store->convs[i].id, r) == OK)
        {
          store_read_run(r);
          store_latest(store, r);
        }

      free(r);
    }

  /* A conversation left without a message, all of them gone to make room
   * before a reset, has nothing to show: it is the caller's to remove
   */

  if (store->nconvs > 1)
    {
      qsort(store->convs, store->nconvs, sizeof(*store->convs),
            store_conv_cmp);
    }

  return OK;
}

void store_move(FAR struct store_s *to, FAR struct store_s *from)
{
  store_free(to);
  *to = *from;
  memset(from, 0, sizeof(*from));
}

FAR struct store_conv_s *store_find(FAR struct store_s *store,
                                    uint64_t conversation)
{
  int i = store_index_of(store, conversation);

  return i >= 0 ? &store->convs[i] : NULL;
}

FAR struct store_conv_s *store_find_peer(FAR struct store_s *store,
                                         FAR const char *transport,
                                         FAR const char *peer)
{
  uint32_t i;

  for (i = 0; i < store->nconvs; i++)
    {
      if (strcmp(store->convs[i].transport, transport) == 0 &&
          strcmp(store->convs[i].peer, peer) == 0)
        {
          return &store->convs[i];
        }
    }

  return NULL;
}

FAR struct store_msg_s *store_msg(FAR struct store_conv_s *conv,
                                  uint64_t message)
{
  uint32_t lo = 0;
  uint32_t hi = conv->nmsgs;
  uint32_t mid;

  /* Sorted by id: stored in their order */

  while (lo < hi)
    {
      mid = lo + (hi - lo) / 2;
      if (conv->msgs[mid].id < message)
        {
          lo = mid + 1;
        }
      else
        {
          hi = mid;
        }
    }

  return lo < conv->nmsgs && conv->msgs[lo].id == message ?
         &conv->msgs[lo] : NULL;
}

uint32_t store_unread(FAR struct store_s *store)
{
  uint32_t unread = 0;
  uint32_t i;

  for (i = 0; i < store->nconvs; i++)
    {
      unread += store->convs[i].unread;
    }

  return unread;
}

FAR const char *store_reason(FAR struct store_s *store, uint64_t message)
{
  uint32_t i;

  for (i = 0; i < store->nreasons; i++)
    {
      if (store->reasons[i].id == message)
        {
          return store->reasons[i].reason;
        }
    }

  return "";
}

int store_plan_message(FAR struct store_s *store, FAR const char *transport,
                       FAR const char *peer,
                       FAR pnut_messages_message_record_t *msg,
                       FAR struct store_write_s *w)
{
  FAR struct store_conv_s *conv;
  struct store_conv_s fresh;
  FAR pnut_messages_record_t *rec;
  uint64_t key = store_hash(msg->key);
  uint32_t i;
  int ret;

  /* Over the limit still: no room could be made (RFC 0029) */

  if (store->bytes > store->limit)
    {
      return -ENOSPC;
    }

  conv = store_find_peer(store, transport, peer);
  if (conv != NULL && key != 0)
    {
      for (i = 0; i < conv->nmsgs; i++)
        {
          if (conv->msgs[i].key == key &&
              (conv->msgs[i].flags & STORE_DELETED) == 0)
            {
              w->conversation = conv->id;
              w->message      = conv->msgs[i].id;
              return -EEXIST;
            }
        }
    }

  /* A new conversation is planned under its id to come; it joins the
   * index with its head, once written
   */

  if (conv == NULL)
    {
      memset(&fresh, 0, sizeof(fresh));
      fresh.id = store->next_conv;
      strlcpy(fresh.transport, transport, sizeof(fresh.transport));
      strlcpy(fresh.peer, peer, sizeof(fresh.peer));
      conv = &fresh;
    }

  rec = &store->rec;
  msg->id = store->next_msg;
  memset(rec, 0, sizeof(*rec));
  rec->which_kind   = PNUT_MESSAGES_RECORD_MESSAGE_TAG;
  rec->kind.message = *msg;
  w->message = msg->id;
  ret = store_plan_into(store, conv, rec, w);
  if (ret < 0)
    {
      return ret == -E2BIG ? -EINVAL : ret;
    }

  return OK;
}

int store_plan_status(FAR struct store_s *store, uint64_t message,
                      uint8_t status, FAR const char *reason,
                      FAR struct store_write_s *w)
{
  FAR pnut_messages_record_t *rec = &store->rec;
  FAR struct store_conv_s *conv;
  FAR struct store_msg_s *msg;

  conv = store_owner(store, message, &msg);
  if (conv == NULL || (msg->flags & STORE_DELETED))
    {
      return -ENOENT;
    }

  if (msg->status == status &&
      strcmp(store_reason(store, message), reason) == 0)
    {
      return -EALREADY;
    }

  memset(rec, 0, sizeof(*rec));
  rec->which_kind = PNUT_MESSAGES_RECORD_STATUS_TAG;
  rec->kind.status.id     = message;
  rec->kind.status.status = (pnut_messages_message_status_t)status;
  strlcpy(rec->kind.status.reason, reason, sizeof(rec->kind.status.reason));
  w->message = message;
  return store_plan_into(store, conv, rec, w);
}

int store_plan_read(FAR struct store_s *store, uint64_t conversation,
                    FAR struct store_write_s *w)
{
  FAR pnut_messages_record_t *rec = &store->rec;
  FAR struct store_conv_s *conv;

  conv = store_find(store, conversation);
  if (conv == NULL)
    {
      return -ENOENT;
    }

  if (conv->unread == 0)
    {
      return -EALREADY;
    }

  memset(rec, 0, sizeof(*rec));
  rec->which_kind   = PNUT_MESSAGES_RECORD_READ_TAG;
  rec->kind.read.id = conv->msgs[conv->nmsgs - 1].id;
  w->message = rec->kind.read.id;
  return store_plan_into(store, conv, rec, w);
}

int store_plan_delete(FAR struct store_s *store, uint64_t conversation,
                      uint64_t message, FAR struct store_write_s *w)
{
  FAR pnut_messages_record_t *rec = &store->rec;
  FAR struct store_conv_s *conv;
  FAR struct store_msg_s *msg;

  conv = store_find(store, conversation);
  msg  = conv != NULL ? store_msg(conv, message) : NULL;
  if (msg == NULL || (msg->flags & STORE_DELETED))
    {
      return -ENOENT;
    }

  memset(rec, 0, sizeof(*rec));
  rec->which_kind      = PNUT_MESSAGES_RECORD_DELETED_TAG;
  rec->kind.deleted.id = message;
  w->message = message;
  return store_plan_into(store, conv, rec, w);
}

void store_write_run(FAR struct store_write_s *w)
{
  size_t done = 0;
  ssize_t n;
  off_t end;
  int flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
  int fd;

  /* A new segment's file is new: one left by a write never committed is
   * no part of the store
   */

  if (w->create)
    {
      flags |= O_TRUNC;
    }

  fd = open(w->path, flags, 0600);
  if (fd < 0)
    {
      w->ret = -errno;
      return;
    }

  end = lseek(fd, 0, SEEK_END);
  if (end < 0)
    {
      w->ret = -errno;
      close(fd);
      return;
    }

  /* Longer than the index has it: a write that failed, and was not cut
   * back then.  Cut back now, or a record would follow a broken one, and
   * be lost with it at the next start.
   */

  if (end != (off_t)w->expect)
    {
      if (end < (off_t)w->expect || ftruncate(fd, (off_t)w->expect) < 0)
        {
          w->ret = -EIO;
          close(fd);
          return;
        }

      end = (off_t)w->expect;
    }

  w->offset = (uint32_t)end;
  while (done < w->len)
    {
      n = write(fd, w->data + done, w->len - done);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          w->ret = n < 0 ? -errno : -EIO;
          break;
        }

      done += (size_t)n;
    }

  if (done == w->len)
    {
      w->ret = OK;
    }

  if (w->ret == OK && fsync(fd) < 0)
    {
      w->ret = -errno;
    }

  /* A write cut short leaves nothing behind it */

  if (w->ret < 0 && ftruncate(fd, end) == 0)
    {
      fsync(fd);
    }

  close(fd);
}

int store_commit(FAR struct store_s *store,
                 FAR const struct store_write_s *w)
{
  FAR pnut_messages_record_t *rec = &store->rec;
  FAR struct store_segment_s *seg;
  uint32_t pos = 0;
  int ret = OK;
  int idx;
  int n;

  if (w->ret < 0)
    {
      return OK;
    }

  while (pos < w->len)
    {
      n = store_unframe(w->data + pos, w->len - pos, rec);
      if (n < 0)
        {
          break;
        }

      if (store_apply(store, w->conversation, w->segment, w->offset + pos,
                      (uint16_t)n, rec, false) == -ENOMEM)
        {
          ret = -ENOMEM;
        }

      pos += (uint32_t)n;
    }

  store->bytes += w->len;
  idx = store_index_of(store, w->conversation);
  if (idx < 0)
    {
      return -ENOMEM;             /* Its head did not go in */
    }

  seg = store_segment(&store->convs[idx], w->segment, false);
  if (seg == NULL)
    {
      return -ENOMEM;
    }

  seg->size = w->offset + w->len;
  store_place(store, (uint32_t)idx);
  return ret;
}

int store_conversations(FAR struct store_s *store, uint64_t after,
                        FAR pnut_messages_conversations_t *out)
{
  FAR pnut_messages_conversation_t *c;
  FAR struct store_conv_s *conv;
  uint32_t i = 0;
  int idx;

  memset(out, 0, sizeof(*out));
  if (after != 0)
    {
      idx = store_index_of(store, after);
      if (idx < 0)
        {
          return -ENOENT;
        }

      i = (uint32_t)idx + 1;
    }

  for (; i < store->nconvs; i++)
    {
      conv = &store->convs[i];
      if (conv->count == 0)
        {
          continue;
        }

      if (out->conversations_count == STORE_CONV_PAGE)
        {
          out->more = true;
          break;
        }

      c = &out->conversations[out->conversations_count++];
      c->id     = conv->id;
      c->count  = conv->count;
      c->unread = conv->unread;
      c->stored = conv->stored;
      strlcpy(c->transport, conv->transport, sizeof(c->transport));
      strlcpy(c->peer, conv->peer, sizeof(c->peer));
      strlcpy(c->latest, conv->latest, sizeof(c->latest));
    }

  return OK;
}

int store_plan_page(FAR struct store_s *store, uint64_t conversation,
                    uint64_t after, FAR struct store_read_s *r,
                    FAR bool *more)
{
  FAR struct store_conv_s *conv;
  FAR struct store_msg_s *msg;
  uint32_t i;
  int ret;

  memset(r, 0, offsetof(struct store_read_s, records));
  r->conversation = conversation;
  *more = false;

  conv = store_find(store, conversation);
  if (conv == NULL)
    {
      return -ENOENT;
    }

  i = conv->nmsgs;
  if (after != 0)
    {
      msg = store_msg(conv, after);
      if (msg == NULL)
        {
          return -ENOENT;
        }

      i = (uint32_t)(msg - conv->msgs);
    }

  while (i-- > 0)
    {
      msg = &conv->msgs[i];
      if (msg->flags & STORE_DELETED)
        {
          continue;
        }

      if (r->n == STORE_PAGE)
        {
          *more = true;
          break;
        }

      ret = store_path(store, conv->id, msg->segment, r->at[r->n].path);
      if (ret < 0)
        {
          return ret;
        }

      r->at[r->n].offset = msg->offset;
      r->at[r->n].size   = msg->size;
      r->n++;
    }

  return OK;
}

void store_read_run(FAR struct store_read_s *r)
{
  FAR uint8_t *buf = r->buf;
  ssize_t n;
  uint8_t i;
  int fd;

  r->ret = OK;
  for (i = 0; i < r->n; i++)
    {
      if (r->at[i].size > sizeof(r->buf))
        {
          r->ret = -EINVAL;
          return;
        }

      fd = open(r->at[i].path, O_RDONLY | O_CLOEXEC);
      if (fd < 0)
        {
          r->ret = -errno;
          return;
        }

      n = pread(fd, buf, r->at[i].size, (off_t)r->at[i].offset);
      if (n < 0)
        {
          r->ret = -errno;
        }

      close(fd);
      if (r->ret < 0)
        {
          return;
        }

      if (n != r->at[i].size ||
          store_unframe(buf, (size_t)n, &r->records[i]) != n ||
          r->records[i].which_kind != PNUT_MESSAGES_RECORD_MESSAGE_TAG)
        {
          r->ret = -EIO;
          return;
        }
    }
}

int store_page(FAR struct store_s *store, FAR const struct store_read_s *r,
               FAR pnut_messages_page_t *out)
{
  FAR const pnut_messages_message_record_t *m;
  FAR pnut_messages_message_t *o;
  FAR struct store_conv_s *conv;
  FAR struct store_msg_s *msg;
  uint8_t i;

  memset(out, 0, sizeof(*out));
  conv = store_find(store, r->conversation);
  if (conv == NULL)
    {
      return -ENOENT;
    }

  for (i = 0; i < r->n; i++)
    {
      m   = &r->records[i].kind.message;
      msg = store_msg(conv, m->id);
      if (msg == NULL || (msg->flags & STORE_DELETED))
        {
          continue;               /* Gone while it was read */
        }

      o = &out->messages[out->messages_count++];
      o->id     = m->id;
      o->out    = m->out;
      o->stored = m->stored;
      o->sent   = m->sent;
      o->status = (pnut_messages_message_status_t)msg->status;
      strlcpy(o->text, m->text, sizeof(o->text));
      strlcpy(o->sender, m->sender, sizeof(o->sender));
      strlcpy(o->reason, store_reason(store, m->id), sizeof(o->reason));
    }

  return OK;
}

int store_plan_latest(FAR struct store_s *store, uint64_t conversation,
                      FAR struct store_read_s *r)
{
  FAR struct store_conv_s *conv;
  FAR struct store_msg_s *msg;

  memset(r, 0, offsetof(struct store_read_s, records));
  r->conversation = conversation;
  conv = store_find(store, conversation);
  if (conv == NULL)
    {
      return -ENOENT;
    }

  if (!conv->stale || conv->newest == 0)
    {
      return -EALREADY;
    }

  msg = store_msg(conv, conv->newest);
  if (msg == NULL)
    {
      return -EALREADY;
    }

  r->n = 1;
  r->at[0].offset = msg->offset;
  r->at[0].size   = msg->size;
  return store_path(store, conv->id, msg->segment, r->at[0].path);
}

void store_latest(FAR struct store_s *store,
                  FAR const struct store_read_s *r)
{
  FAR struct store_conv_s *conv;

  conv = store_find(store, r->conversation);
  if (conv == NULL || r->ret < 0 || r->n != 1 ||
      r->records[0].kind.message.id != conv->newest)
    {
      return;
    }

  conv->stored = r->records[0].kind.message.stored;
  store_latest_set(conv, r->records[0].kind.message.text);
}

int store_plan_remove(FAR struct store_s *store, uint64_t conversation,
                      FAR struct store_remove_s *rm)
{
  FAR struct store_conv_s *conv;
  uint32_t i;
  int ret;

  memset(rm, 0, sizeof(*rm));
  rm->conversation = conversation;
  conv = store_find(store, conversation);
  if (conv == NULL)
    {
      return -ENOENT;
    }

  rm->paths = calloc(conv->nsegs > 0 ? conv->nsegs : 1,
                     sizeof(*rm->paths));
  if (rm->paths == NULL)
    {
      return -ENOMEM;
    }

  for (i = 0; i < conv->nsegs; i++)
    {
      ret = store_path(store, conv->id, conv->segs[i].number,
                       rm->paths[i]);
      if (ret < 0)
        {
          free(rm->paths);
          rm->paths = NULL;
          return ret;
        }
    }

  rm->n = conv->nsegs;
  return OK;
}

void store_remove_run(FAR struct store_remove_s *rm)
{
  uint32_t i;

  /* First to last, stopping at a file that stays: those left are always
   * the last ones, which hold every deletion about the messages in them
   */

  rm->ret = OK;
  for (i = 0; i < rm->n; i++)
    {
      if (unlink(rm->paths[i]) < 0 && errno != ENOENT)
        {
          rm->ret = -errno;
          break;
        }
    }

  rm->done = i;
}

void store_removed(FAR struct store_s *store,
                   FAR struct store_remove_s *rm)
{
  FAR struct store_conv_s *conv;
  uint32_t i;
  int idx;

  free(rm->paths);
  rm->paths = NULL;

  /* A file stayed: the conversation too, to be deleted again */

  idx = store_index_of(store, rm->conversation);
  if (idx < 0 || rm->ret < 0)
    {
      return;
    }

  conv = &store->convs[idx];
  for (i = 0; i < conv->nsegs; i++)
    {
      store->bytes -= conv->segs[i].size;
    }

  for (i = 0; i < conv->nmsgs; i++)
    {
      store_reason_set(store, conv->msgs[i].id, NULL);
    }

  store_conv_free(conv);
  memmove(conv, conv + 1,
          (store->nconvs - (uint32_t)idx - 1) * sizeof(*conv));
  store->nconvs--;
}

int store_plan_room(FAR struct store_s *store,
                    FAR struct store_rewrite_s *rw)
{
  FAR struct store_msg_s *msg;
  FAR struct store_msg_s *oldest = NULL;
  uint32_t conv = 0;
  uint32_t i;
  uint32_t j;

  memset(rw, 0, sizeof(*rw));
  if (store->bytes <= store->limit)
    {
      return -EALREADY;
    }

  /* The oldest message that may go, in any conversation */

  for (i = 0; i < store->nconvs; i++)
    {
      for (j = 0; j < store->convs[i].nmsgs; j++)
        {
          msg = &store->convs[i].msgs[j];
          if (store_may_go(msg))
            {
              if (oldest == NULL || msg->id < oldest->id)
                {
                  oldest = msg;
                  conv   = i;
                }

              break;              /* The conversation's oldest that may */
            }
        }
    }

  if (oldest == NULL)
    {
      return -ENOSPC;
    }

  return store_plan_rewrite(store, conv, oldest->segment, true, rw);
}

int store_plan_compact(FAR struct store_s *store,
                       FAR struct store_rewrite_s *rw)
{
  FAR struct store_segment_s *seg;
  uint32_t i;
  uint32_t j;

  memset(rw, 0, sizeof(*rw));
  for (i = 0; i < store->nconvs; i++)
    {
      for (j = 0; j < store->convs[i].nsegs; j++)
        {
          seg = &store->convs[i].segs[j];
          if (seg->size >= STORE_COMPACT_MIN && seg->dead * 2 > seg->size)
            {
              return store_plan_rewrite(store, i, seg->number, false, rw);
            }
        }
    }

  return -EALREADY;
}

void store_rewrite_run(FAR struct store_rewrite_s *rw)
{
  FAR pnut_messages_record_t *rec;
  FAR struct store_kept_s *kept;
  FAR uint8_t *data;
  FAR uint8_t *out = NULL;
  uint32_t found = 0;
  size_t cap;
  size_t len;
  size_t pos;
  size_t olen = 0;
  size_t last_read = SIZE_MAX;
  bool body = false;
  uint64_t id;
  int fd;
  int n;

  rw->ret = store_read_file(rw->path, &data, &len);
  if (rw->ret < 0)
    {
      if (rw->ret == -ENOENT)
        {
          rw->ret = -ESTALE;      /* The index has a file the flash lacks */
        }

      return;
    }

  /* Kept records are written again with their state now, which can make
   * one a little longer (a status, or a reason, folded in): their file's
   * size and a record's more is room enough in practice, and a rewrite
   * that would pass it fails rather than overrun
   */

  cap = len + STORE_FRAME_MAX;
  rec = malloc(sizeof(*rec));
  out = malloc(cap);
  if (rec == NULL || out == NULL)
    {
      rw->ret = -ENOMEM;
      goto done;
    }

  /* The segment's whole records, and its last read */

  for (pos = 0; pos < len; pos += (size_t)n)
    {
      n = store_unframe(data + pos, len - pos, rec);
      if (n < 0)
        {
          break;
        }

      if (rec->which_kind == PNUT_MESSAGES_RECORD_READ_TAG)
        {
          last_read = pos;
        }
    }

  len = pos;                      /* A cut tail is no part of it */
  for (pos = 0; pos < len; pos += (size_t)n)
    {
      bool keep = false;

      n = store_unframe(data + pos, len - pos, rec);
      switch (rec->which_kind)
        {
          case PNUT_MESSAGES_RECORD_HEAD_TAG:
            keep = pos == 0;
            break;

          case PNUT_MESSAGES_RECORD_MESSAGE_TAG:
            kept = store_find_id(rw->kept, rw->n,
                                 sizeof(*rw->kept),
                                 rec->kind.message.id);
            if (kept != NULL)
              {
                rec->kind.message.status =
                  (pnut_messages_message_status_t)kept->status;
                strlcpy(rec->kind.message.reason, kept->reason,
                        sizeof(rec->kind.message.reason));
                kept->offset = (uint32_t)olen;
                n = store_frame(rec, out + olen, cap - olen);
                if (n < 0)
                  {
                    rw->ret = n;
                    goto done;
                  }

                kept->size = (uint16_t)n;
                olen += (size_t)n;
                body  = true;
                found++;
                n = store_unframe(data + pos, len - pos, rec);
              }
            break;

          /* About a message: folded into it here, or gone with it, or of
           * use while it is in another segment
           */

          case PNUT_MESSAGES_RECORD_STATUS_TAG:
          case PNUT_MESSAGES_RECORD_DELETED_TAG:
            id = rec->which_kind == PNUT_MESSAGES_RECORD_STATUS_TAG ?
                 rec->kind.status.id : rec->kind.deleted.id;
            keep = store_find_id(rw->live, rw->nlive, sizeof(*rw->live),
                                 id) != NULL;
            break;

          /* The last read record, while a message it covers is in
           * another segment
           */

          case PNUT_MESSAGES_RECORD_READ_TAG:
            keep = pos == last_read && rw->nlive > 0 &&
                   rw->live[0] <= rec->kind.read.id;
            break;

          default:
            break;
        }

      if (keep)
        {
          if (olen + (size_t)n > cap)
            {
              rw->ret = -E2BIG;
              goto done;
            }

          memcpy(out + olen, data + pos, (size_t)n);
          olen += (size_t)n;
          body  = body || rec->which_kind != PNUT_MESSAGES_RECORD_HEAD_TAG;
        }
    }

  /* A message to keep that the file does not have: the index and the
   * file disagree, and the segment stays as it is
   */

  if (found != rw->n)
    {
      rw->ret = -ESTALE;
      goto done;
    }

  /* Nothing but its head: the segment goes */

  if (!body)
    {
      rw->size = 0;
      if (unlink(rw->path) < 0 && errno != ENOENT)
        {
          rw->ret = -errno;
        }

      goto done;
    }

  fd = open(rw->temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0)
    {
      rw->ret = -errno;
      goto done;
    }

  rw->ret = OK;
  for (pos = 0; pos < olen; pos += (size_t)n)
    {
      n = (int)write(fd, out + pos, olen - pos);
      if (n < 0 && errno == EINTR)
        {
          n = 0;
          continue;
        }

      if (n <= 0)
        {
          rw->ret = n < 0 ? -errno : -EIO;
          break;
        }
    }

  if (rw->ret == OK && fsync(fd) < 0)
    {
      rw->ret = -errno;
    }

  close(fd);
  if (rw->ret == OK && rename(rw->temp, rw->path) < 0)
    {
      rw->ret = -errno;
    }

  if (rw->ret < 0)
    {
      unlink(rw->temp);
    }
  else
    {
      rw->size = (uint32_t)olen;
    }

done:
  free(out);
  free(rec);
  free(data);
}

void store_rewritten(FAR struct store_s *store,
                     FAR struct store_rewrite_s *rw)
{
  FAR struct store_conv_s *conv;
  FAR struct store_segment_s *seg;
  FAR struct store_kept_s *kept;
  FAR struct store_msg_s *msg;
  FAR void *p;
  uint32_t i;
  uint32_t j;
  int idx;

  idx = store_index_of(store, rw->conversation);
  if (rw->ret < 0 || idx < 0)
    {
      goto done;
    }

  conv = &store->convs[idx];
  seg  = store_segment(conv, rw->segment, false);
  if (seg != NULL)
    {
      store->bytes -= seg->size;
      store->bytes += rw->size;
      seg->size     = rw->size;
      seg->dead     = 0;
    }

  /* The segment's messages: the kept at their new places, the others
   * gone
   */

  for (i = 0, j = 0; i < conv->nmsgs; i++)
    {
      msg = &conv->msgs[i];
      if (msg->segment == rw->segment)
        {
          kept = store_find_id(rw->kept, rw->n, sizeof(*rw->kept),
                               msg->id);
          if (kept == NULL)
            {
              if ((msg->flags & STORE_DELETED) == 0)
                {
                  conv->count--;
                  if (store_is_unread(msg))
                    {
                      conv->unread--;
                    }
                }

              store_reason_set(store, msg->id, NULL);
              continue;
            }

          msg->offset = kept->offset;
          msg->size   = kept->size;
        }

      conv->msgs[j++] = *msg;
    }

  conv->nmsgs = j;

  /* The index's memory given back as it shrinks (RFC 0029) */

  if (conv->msgs_cap >= conv->nmsgs + 2 * STORE_GROW_MSGS)
    {
      p = realloc(conv->msgs, (conv->nmsgs + STORE_GROW_MSGS) *
                  sizeof(*conv->msgs));
      if (p != NULL)
        {
          conv->msgs     = p;
          conv->msgs_cap = conv->nmsgs + STORE_GROW_MSGS;
        }
    }

  /* A segment removed.  A conversation left with no message is the
   * caller's to remove (store_plan_remove()): its other segments' records
   * are of no use now.
   */

  if (rw->size == 0 && seg != NULL)
    {
      i = (uint32_t)(seg - conv->segs);
      memmove(seg, seg + 1, (conv->nsegs - i - 1) * sizeof(*seg));
      conv->nsegs--;
    }

  store_newest(conv);
  store_place(store, (uint32_t)idx);

done:
  free(rw->kept);
  free(rw->live);
  rw->kept = NULL;
  rw->live = NULL;
}

int store_frame(FAR const pnut_messages_record_t *rec, FAR uint8_t *buf,
                size_t size)
{
  pb_ostream_t stream;
  uint32_t crc;
  size_t n;

  if (size < 6)
    {
      return -E2BIG;
    }

  stream = pb_ostream_from_buffer(buf + 2, size - 6);
  if (!pb_encode(&stream, PNUT_MESSAGES_RECORD_FIELDS, rec))
    {
      return stream.bytes_written >= size - 6 ? -E2BIG : -EINVAL;
    }

  n = stream.bytes_written;
  buf[0] = (uint8_t)n;
  buf[1] = (uint8_t)(n >> 8);
  crc = store_crc(0, buf, n + 2);
  buf[n + 2] = (uint8_t)crc;
  buf[n + 3] = (uint8_t)(crc >> 8);
  buf[n + 4] = (uint8_t)(crc >> 16);
  buf[n + 5] = (uint8_t)(crc >> 24);
  return (int)(n + 6);
}

int store_unframe(FAR const uint8_t *buf, size_t len,
                  FAR pnut_messages_record_t *rec)
{
  pb_istream_t stream;
  uint32_t crc;
  size_t n;

  if (len < 6)
    {
      return -EAGAIN;
    }

  n = (size_t)buf[0] | ((size_t)buf[1] << 8);
  if (n > PNUT_MESSAGES_RECORD_SIZE)
    {
      return -EINVAL;
    }

  if (n + 6 > len)
    {
      return -EAGAIN;
    }

  crc = (uint32_t)buf[n + 2] | ((uint32_t)buf[n + 3] << 8) |
        ((uint32_t)buf[n + 4] << 16) | ((uint32_t)buf[n + 5] << 24);
  if (crc != store_crc(0, buf, n + 2))
    {
      return -EINVAL;
    }

  memset(rec, 0, sizeof(*rec));
  stream = pb_istream_from_buffer(buf + 2, n);
  if (!pb_decode(&stream, PNUT_MESSAGES_RECORD_FIELDS, rec))
    {
      return -EINVAL;
    }

  return (int)(n + 6);
}
