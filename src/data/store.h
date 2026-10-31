/****************************************************************************
 * pnut-os/src/data/store.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_DATA_STORE_H
#define __PNUT_OS_DATA_STORE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pnut/compiler.h>

#include "pnut/messages.pb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Messages' store (RFC 0029): a log for each conversation, appended to, in
 * segments of up to STORE_SEGMENT_MAX bytes, each a file that begins with
 * a head record naming the conversation, so that it stands alone.  Each
 * record is framed: its length (2 bytes, little-endian), the record
 * (pnut.MessagesRecord), and a CRC-32 of both (4 bytes, little-endian).
 *
 * The index is in memory: built from the files at start, by store_load()
 * on a worker, and kept on the loop after that.  A change goes in three
 * steps: planned on the loop, which frames its records and names the file
 * (store_plan_*()); written on a worker, which touches nothing but the
 * file and the plan (store_write_run()); and applied to the index on the
 * loop once written (store_commit()).  One change at a time: the next is
 * planned once the last is committed.
 */

#define STORE_VERSION       1
#define STORE_TEXT_MAX      1530      /* A message's text, without its NUL */
#define STORE_LATEST_MAX    48        /* The start of the newest text */
#define STORE_SEGMENT_MAX   (128 * 1024)
#define STORE_COMPACT_MIN   4096      /* A segment worth compacting */
#define STORE_FRAME_MAX     (2 + PNUT_MESSAGES_RECORD_SIZE + 4)
#define STORE_WRITE_MAX     (2 * STORE_FRAME_MAX)  /* A head and a record */
#define STORE_PATH_MAX      96
#define STORE_NAME_LEN      29        /* A segment's file name */
#define STORE_TEMP          ".new"    /* A rewrite's file, renamed after */
#define STORE_DIR_MAX \
  (STORE_PATH_MAX - 1 - 1 - STORE_NAME_LEN - (int)sizeof(STORE_TEMP))
#define STORE_PAGE          2         /* Messages in a page (messages.proto) */
#define STORE_CONV_PAGE     8         /* Conversations in a page */

/* A message's flags in the index */

#define STORE_OUT           0x01
#define STORE_DELETED       0x02

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A message, as the index keeps it: where its record is, and its state */

struct store_msg_s
{
  uint64_t id;
  uint64_t key;                   /* A delivered one's key, hashed; 0 */
  uint32_t segment;               /* Its segment's number */
  uint32_t offset;                /* Its record's place in the segment */
  uint16_t size;                  /* Its record's, framed */
  uint8_t status;                 /* pnut_messages_message_status_t */
  uint8_t flags;
};

struct store_segment_s
{
  uint32_t number;
  uint32_t size;                  /* Its file's bytes */
  uint32_t dead;                  /* Of them, deleted messages' */
};

struct store_conv_s
{
  uint64_t id;
  uint64_t newest;                /* Its newest message's id */
  uint64_t read;                  /* Read up to this message's id */
  int64_t stored;                 /* The newest's, by the device's clock */
  uint32_t count;                 /* Its messages, deleted ones apart */
  uint32_t unread;
  char transport[65];
  char peer[33];
  char latest[STORE_LATEST_MAX + 1];
  bool stale;                     /* latest is not the newest's */
  FAR struct store_msg_s *msgs;   /* In the order stored */
  uint32_t nmsgs;
  uint32_t msgs_cap;
  FAR struct store_segment_s *segs;
  uint32_t nsegs;
  uint32_t segs_cap;
};

/* A failed message's reason: few fail, and a status record may be in a
 * later segment than its message
 */

struct store_reason_s
{
  uint64_t id;
  char reason[65];
};

struct store_s
{
  char dir[STORE_DIR_MAX + 1];    /* Empty when the one given is too long */
  uint64_t limit;                 /* Bytes, every segment's together */
  uint64_t bytes;
  uint32_t segmax;
  uint64_t next_msg;
  uint64_t next_conv;
  FAR struct store_conv_s *convs; /* The newest message's first */
  uint32_t nconvs;
  uint32_t convs_cap;
  FAR struct store_reason_s *reasons;
  uint32_t nreasons;
  uint32_t reasons_cap;
  uint32_t cut;                   /* Segments cut back as they loaded */
  uint32_t bad;                   /* Files left aside: no head */

  /* Plans' records, on the loop: too large for its stack */

  pnut_messages_record_t rec;
  pnut_messages_record_t head;
};

/* A change, from its plan to its commit */

struct store_write_s
{
  /* Planned on the loop */

  char path[STORE_PATH_MAX];
  uint8_t data[STORE_WRITE_MAX];  /* Framed records */
  uint16_t len;
  bool create;                    /* A new segment: its file is new */
  uint32_t expect;                /* Where the records go: the segment's
                                   * size as the index has it */
  uint64_t conversation;
  uint32_t segment;
  uint64_t message;               /* The message it makes or changes */

  /* Written on a worker */

  uint32_t offset;                /* Where the records went */
  int ret;
};

/* A page of messages, read on a worker: where each record is */

struct store_at_s
{
  char path[STORE_PATH_MAX];
  uint32_t offset;
  uint16_t size;
};

struct store_read_s
{
  uint64_t conversation;
  uint8_t n;
  struct store_at_s at[STORE_PAGE];
  pnut_messages_record_t records[STORE_PAGE];
  uint8_t buf[STORE_FRAME_MAX];   /* The worker's, off its stack */
  int ret;
};

/* A conversation's files, to remove */

struct store_remove_s
{
  uint64_t conversation;
  uint32_t n;
  FAR char (*paths)[STORE_PATH_MAX];
  uint32_t done;                  /* The files removed, first to last */
  int ret;
};

/* A segment rewritten without the messages that may go, to make room:
 * the plan names the messages kept, the worker gives their new places
 */

struct store_kept_s
{
  uint64_t id;
  uint8_t status;
  char reason[65];
  uint32_t offset;                /* In the new file */
  uint16_t size;
};

struct store_rewrite_s
{
  char path[STORE_PATH_MAX];
  char temp[STORE_PATH_MAX];
  uint64_t conversation;
  uint32_t segment;
  uint32_t n;
  FAR struct store_kept_s *kept;  /* Sorted by id */
  uint32_t nlive;
  FAR uint64_t *live;             /* The conversation's messages in its
                                   * other segments, sorted: records about
                                   * them stay */
  uint32_t size;                  /* The new file's; 0 when removed */
  int ret;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: store_init, store_free
 *
 * Description:
 *   An empty index for the store in dir; or give back its memory.
 *
 ****************************************************************************/

void store_init(FAR struct store_s *store, FAR const char *dir,
                uint64_t limit, uint32_t segmax);
void store_free(FAR struct store_s *store);

/****************************************************************************
 * Name: store_load
 *
 * Description:
 *   On a worker, into an index of its own: read every segment in the
 *   directory, made if missing.  A rewrite's temporary file left by a
 *   reset is removed; a segment whose last record is cut short, or does
 *   not check, is cut back to its last whole record; one with no head is
 *   left aside; a message left sending is not known to be sent.
 *
 * Returned Value:
 *   Zero (OK) on success; -ENAMETOOLONG for a directory's name too long
 *   for its files' names; a negated errno when the directory or a
 *   segment cannot be read, or memory runs out: the caller tries again
 *   later.
 *
 ****************************************************************************/

int store_load(FAR struct store_s *store);

/****************************************************************************
 * Name: store_move
 *
 * Description:
 *   Hand a loaded index over: to's memory is given back, and from's taken.
 *
 ****************************************************************************/

void store_move(FAR struct store_s *to, FAR struct store_s *from);

/****************************************************************************
 * Name: store_find, store_find_peer, store_msg
 *
 * Description:
 *   A conversation by its id, or by its transport and peer; a message of
 *   a conversation by its id.  NULL when there is none.
 *
 ****************************************************************************/

FAR struct store_conv_s *store_find(FAR struct store_s *store,
                                    uint64_t conversation);
FAR struct store_conv_s *store_find_peer(FAR struct store_s *store,
                                         FAR const char *transport,
                                         FAR const char *peer);
FAR struct store_msg_s *store_msg(FAR struct store_conv_s *conv,
                                  uint64_t message);

/****************************************************************************
 * Name: store_unread, store_reason
 *
 * Description:
 *   Unread messages in every conversation; a failed message's reason, or
 *   "".
 *
 ****************************************************************************/

uint32_t store_unread(FAR struct store_s *store);
FAR const char *store_reason(FAR struct store_s *store, uint64_t message);

/****************************************************************************
 * Name: store_plan_message
 *
 * Description:
 *   A new message in the conversation with the transport and peer, made if
 *   there is none: msg's id is given, and its key hashed for the index.
 *   A message whose key is stored already in the conversation is not
 *   stored again.
 *
 * Returned Value:
 *   Zero (OK) when planned; -EEXIST when its key is stored already, and
 *   w->message is the message that has it; -ENOSPC while the store is
 *   over its limit: a message may pass it, and room is made after it
 *   (store_plan_room()), but none goes in while none could be made;
 *   -EINVAL for a record that does not encode.
 *
 ****************************************************************************/

int store_plan_message(FAR struct store_s *store, FAR const char *transport,
                       FAR const char *peer,
                       FAR pnut_messages_message_record_t *msg,
                       FAR struct store_write_s *w);

/****************************************************************************
 * Name: store_plan_status, store_plan_read, store_plan_delete
 *
 * Description:
 *   A message's new status; a conversation read up to its newest message;
 *   a message deleted.
 *
 * Returned Value:
 *   Zero (OK) when planned; -ENOENT for no such conversation or message;
 *   -EALREADY when there is nothing to write.
 *
 ****************************************************************************/

int store_plan_status(FAR struct store_s *store, uint64_t message,
                      uint8_t status, FAR const char *reason,
                      FAR struct store_write_s *w);
int store_plan_read(FAR struct store_s *store, uint64_t conversation,
                    FAR struct store_write_s *w);
int store_plan_delete(FAR struct store_s *store, uint64_t conversation,
                      uint64_t message, FAR struct store_write_s *w);

/****************************************************************************
 * Name: store_write_run
 *
 * Description:
 *   On a worker: append the plan's records to its file, made new when the
 *   plan says, and sync it; w->offset is where they went.  A write cut
 *   short is cut back off; and a file longer than the index has it, from
 *   such a write not cut back, is cut back first, or the write fails
 *   (-EIO), so that no record is left after a broken one.
 *
 ****************************************************************************/

void store_write_run(FAR struct store_write_s *w);

/****************************************************************************
 * Name: store_commit
 *
 * Description:
 *   On the loop, once written: the plan's records applied to the index.
 *
 * Returned Value:
 *   Zero (OK); -ENOMEM when the index could not take them: the records
 *   are on flash, and the index is short of them until the next start,
 *   so the caller writes nothing more.
 *
 ****************************************************************************/

int store_commit(FAR struct store_s *store,
                 FAR const struct store_write_s *w);

/****************************************************************************
 * Name: store_conversations
 *
 * Description:
 *   A page of conversations, the newest message's first, after the one
 *   whose id is after (0: from the first).
 *
 * Returned Value:
 *   Zero (OK); -ENOENT when after names no conversation.
 *
 ****************************************************************************/

int store_conversations(FAR struct store_s *store, uint64_t after,
                        FAR pnut_messages_conversations_t *out);

/****************************************************************************
 * Name: store_plan_page, store_read_run, store_page
 *
 * Description:
 *   A page of a conversation's messages, newest first, after the one whose
 *   id is after (0: from the newest): planned on the loop, read on a
 *   worker, and made into the answer on the loop, with each message's
 *   status as the index has it now.
 *
 * Returned Value:
 *   Zero (OK); -ENOENT for no such conversation, or no such message.
 *
 ****************************************************************************/

int store_plan_page(FAR struct store_s *store, uint64_t conversation,
                    uint64_t after, FAR struct store_read_s *r,
                    FAR bool *more);
void store_read_run(FAR struct store_read_s *r);
int store_page(FAR struct store_s *store, FAR const struct store_read_s *r,
               FAR pnut_messages_page_t *out);

/****************************************************************************
 * Name: store_plan_latest, store_latest
 *
 * Description:
 *   A conversation's newest message read again, once the one that was its
 *   newest has gone: planned on the loop, read by store_read_run() on a
 *   worker, and the start of its text kept on the loop.
 *
 * Returned Value:
 *   store_plan_latest(): zero (OK); -EALREADY when the conversation's
 *   newest is known, or it has none; -ENOENT.
 *
 ****************************************************************************/

int store_plan_latest(FAR struct store_s *store, uint64_t conversation,
                      FAR struct store_read_s *r);
void store_latest(FAR struct store_s *store,
                  FAR const struct store_read_s *r);

/****************************************************************************
 * Name: store_plan_remove, store_remove_run, store_removed
 *
 * Description:
 *   A whole conversation deleted: its files named on the loop, removed on
 *   a worker, and the conversation dropped from the index on the loop,
 *   unless a file stayed: then it stays in the index, as it is, to be
 *   deleted again.
 *
 * Returned Value:
 *   store_plan_remove(): zero (OK); -ENOENT; -ENOMEM.
 *
 ****************************************************************************/

int store_plan_remove(FAR struct store_s *store, uint64_t conversation,
                      FAR struct store_remove_s *rm);
void store_remove_run(FAR struct store_remove_s *rm);
void store_removed(FAR struct store_s *store,
                   FAR struct store_remove_s *rm);

/****************************************************************************
 * Name: store_plan_room, store_rewrite_run, store_rewritten
 *
 * Description:
 *   Room made while the store is over its limit: the segment that holds
 *   the oldest message that may go is chosen, and rewritten without every
 *   such message it holds, its statuses folded into its messages.  A
 *   message may go once read, or, going out, once sent, delivered, failed
 *   or not known to be sent; never unread, queued or sending.  Records
 *   about the conversation's messages in other segments stay; records
 *   about messages gone go.
 *
 * Returned Value:
 *   store_plan_room(): zero (OK) when planned; -EALREADY when the store is
 *   within its limit; -ENOSPC when no message may go; -ENOMEM.
 *
 ****************************************************************************/

int store_plan_room(FAR struct store_s *store,
                    FAR struct store_rewrite_s *rw);

/* store_rewrite_run()'s rw->ret: -ESTALE when the file is missing, or
 * lacks a message the index keeps there (they disagree, and the index is
 * to be read again); another negated errno when the flash failed
 */

/****************************************************************************
 * Name: store_plan_compact
 *
 * Description:
 *   A segment whose bytes are mostly deleted messages', rewritten without
 *   them by store_rewrite_run(), and store_rewritten() after.
 *
 * Returned Value:
 *   Zero (OK) when planned; -EALREADY when no segment is so; -ENOMEM.
 *
 ****************************************************************************/

int store_plan_compact(FAR struct store_s *store,
                       FAR struct store_rewrite_s *rw);
void store_rewrite_run(FAR struct store_rewrite_s *rw);
void store_rewritten(FAR struct store_s *store,
                     FAR struct store_rewrite_s *rw);

/****************************************************************************
 * Name: store_frame, store_unframe
 *
 * Description:
 *   A record framed into buf; the record framed at the start of buf.
 *
 * Returned Value:
 *   The frame's size; -E2BIG when it does not fit; -EINVAL for a record
 *   that does not encode, a frame cut short (-EAGAIN), or one that does
 *   not check.
 *
 ****************************************************************************/

int store_frame(FAR const pnut_messages_record_t *rec, FAR uint8_t *buf,
                size_t size);
int store_unframe(FAR const uint8_t *buf, size_t len,
                  FAR pnut_messages_record_t *rec);

#endif /* __PNUT_OS_DATA_STORE_H */
