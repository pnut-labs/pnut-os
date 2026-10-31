/****************************************************************************
 * pnut-os/tests/unit/test_store.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Messages' store (RFC 0029): records framed and checked; messages,
 * statuses, marks of read and deletions written and read back after a
 * restart; a cut tail cut back; segments; room made; a conversation
 * removed.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmocka.h>

#include "store.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define S_LIMIT   (64 * 1024)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct st_s
{
  struct store_s store;
  struct store_write_s w;
  struct store_read_s r;
  char dir[64];
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void st_clean(FAR const char *dir)
{
  char path[64 + 256 + 2];
  struct dirent *de;
  DIR *d;

  d = opendir(dir);
  if (d == NULL)
    {
      return;
    }

  while ((de = readdir(d)) != NULL)
    {
      if (de->d_name[0] != '.' &&
          snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) <
          (int)sizeof(path))
        {
          unlink(path);
        }
    }

  closedir(d);
  rmdir(dir);
}

static FAR struct st_s *st_new(uint64_t limit, uint32_t segmax)
{
  FAR struct st_s *s = calloc(1, sizeof(*s));

  assert_non_null(s);
  snprintf(s->dir, sizeof(s->dir), "/tmp/pnut-store-%d", getpid());
  st_clean(s->dir);
  store_init(&s->store, s->dir, limit, segmax);
  assert_int_equal(store_load(&s->store), 0);
  return s;
}

static void st_free(FAR struct st_s *s)
{
  store_free(&s->store);
  st_clean(s->dir);
  free(s);
}

/* The store read again from its files, as after a restart */

static void st_reload(FAR struct st_s *s)
{
  struct store_s fresh;

  store_init(&fresh, s->dir, s->store.limit, s->store.segmax);
  assert_int_equal(store_load(&fresh), 0);
  store_move(&s->store, &fresh);
}

/* A change written and committed, as the module does */

static int st_run(FAR struct st_s *s, int ret)
{
  if (ret < 0)
    {
      return ret;
    }

  store_write_run(&s->w);
  store_commit(&s->store, &s->w);
  return s->w.ret;
}

static int st_message(FAR struct st_s *s, FAR const char *transport,
                      FAR const char *peer, FAR const char *text, bool out,
                      FAR const char *key)
{
  pnut_messages_message_record_t m;

  memset(&m, 0, sizeof(m));
  m.out    = out;
  m.stored = 1000;
  m.status = out ? PNUT_MESSAGES_MESSAGE_STATUS_QUEUED :
                   PNUT_MESSAGES_MESSAGE_STATUS_UNREAD;
  strlcpy(m.text, text, sizeof(m.text));
  strlcpy(m.key, key != NULL ? key : "", sizeof(m.key));
  return st_run(s, store_plan_message(&s->store, transport, peer, &m,
                                      &s->w));
}

/* A conversation's page of messages, read as the module does */

static void st_page(FAR struct st_s *s, uint64_t conversation,
                    uint64_t after, FAR pnut_messages_page_t *page)
{
  bool more;

  assert_int_equal(store_plan_page(&s->store, conversation, after, &s->r,
                                   &more), 0);
  store_read_run(&s->r);
  assert_int_equal(s->r.ret, 0);
  assert_int_equal(store_page(&s->store, &s->r, page), 0);
  page->more = more;
}

static off_t st_size(FAR const char *path)
{
  struct stat st;

  return stat(path, &st) == 0 ? st.st_size : -1;
}

static int st_files(FAR const char *dir)
{
  struct dirent *de;
  int n = 0;
  DIR *d;

  d = opendir(dir);
  assert_non_null(d);
  while ((de = readdir(d)) != NULL)
    {
      n += de->d_name[0] != '.';
    }

  closedir(d);
  return n;
}

/****************************************************************************
 * Name: test_store_frame
 *
 * Description:
 *   A record framed and read back; one cut short, or changed, is not.
 *
 ****************************************************************************/

static void test_store_frame(void **state)
{
  pnut_messages_record_t rec;
  pnut_messages_record_t back;
  uint8_t buf[STORE_FRAME_MAX];
  int n;

  memset(&rec, 0, sizeof(rec));
  rec.which_kind = PNUT_MESSAGES_RECORD_MESSAGE_TAG;
  rec.kind.message.id = 7;
  strlcpy(rec.kind.message.text, "Hello, zażółć",
          sizeof(rec.kind.message.text));

  n = store_frame(&rec, buf, sizeof(buf));
  assert_true(n > 6);
  assert_int_equal(store_unframe(buf, (size_t)n, &back), n);
  assert_int_equal(back.which_kind, PNUT_MESSAGES_RECORD_MESSAGE_TAG);
  assert_int_equal(back.kind.message.id, 7);
  assert_string_equal(back.kind.message.text, "Hello, zażółć");

  assert_int_equal(store_unframe(buf, (size_t)n - 1, &back), -EAGAIN);
  buf[4] ^= 0x40;
  assert_int_equal(store_unframe(buf, (size_t)n, &back), -EINVAL);
  assert_int_equal(store_frame(&rec, buf, 8), -E2BIG);
}

/****************************************************************************
 * Name: test_store_messages
 *
 * Description:
 *   Messages in two conversations, the newest first; one delivered twice
 *   stored once; read in pages, newest first; all of it the same after a
 *   restart.
 *
 ****************************************************************************/

static void test_store_messages(void **state)
{
  FAR struct st_s *s = st_new(S_LIMIT, STORE_SEGMENT_MAX);
  pnut_messages_conversations_t convs;
  pnut_messages_page_t page;
  uint64_t a;
  uint64_t b;
  int pass;

  assert_int_equal(st_message(s, "sms", "+1", "first", false, "k1"), 0);
  a = s->w.conversation;
  assert_int_equal(st_message(s, "sms", "+2", "other", false, "k2"), 0);
  b = s->w.conversation;
  assert_int_equal(st_message(s, "sms", "+1", "second", false, "k3"), 0);
  assert_int_equal(st_message(s, "sms", "+1", "third, going out", true,
                              NULL), 0);
  assert_true(a != b);

  /* The same key in its conversation: stored once */

  assert_int_equal(st_message(s, "sms", "+1", "again", false, "k1"),
                   -EEXIST);

  for (pass = 0; pass < 2; pass++)
    {
      assert_int_equal(store_conversations(&s->store, 0, &convs), 0);
      assert_int_equal(convs.conversations_count, 2);
      assert_int_equal(convs.conversations[0].id, a);
      assert_int_equal(convs.conversations[0].count, 3);
      assert_int_equal(convs.conversations[0].unread, 2);
      assert_string_equal(convs.conversations[0].latest,
                          "third, going out");
      assert_int_equal(convs.conversations[1].id, b);
      assert_int_equal(convs.conversations[1].unread, 1);
      assert_int_equal(store_unread(&s->store), 3);

      st_page(s, a, 0, &page);
      assert_int_equal(page.messages_count, 2);
      assert_true(page.more);
      assert_string_equal(page.messages[0].text, "third, going out");
      assert_true(page.messages[0].out);
      assert_int_equal(page.messages[0].status,
                       PNUT_MESSAGES_MESSAGE_STATUS_QUEUED);
      assert_string_equal(page.messages[1].text, "second");

      st_page(s, a, page.messages[1].id, &page);
      assert_int_equal(page.messages_count, 1);
      assert_false(page.more);
      assert_string_equal(page.messages[0].text, "first");

      st_reload(s);
    }

  /* Ids go on after a restart */

  assert_int_equal(st_message(s, "sms", "+2", "later", false, "k4"), 0);
  assert_int_equal(store_conversations(&s->store, 0, &convs), 0);
  assert_int_equal(convs.conversations[0].id, b);

  st_free(s);
}

/****************************************************************************
 * Name: test_store_statuses
 *
 * Description:
 *   Statuses, a reason, marks of read and deletions survive a restart; a
 *   message left sending is not known to be sent; the newest deleted, the
 *   conversation's newest text is read again.
 *
 ****************************************************************************/

static void test_store_statuses(void **state)
{
  FAR struct st_s *s = st_new(S_LIMIT, STORE_SEGMENT_MAX);
  pnut_messages_conversations_t convs;
  pnut_messages_page_t page;
  FAR struct store_conv_s *conv;
  uint64_t c;
  uint64_t in;
  uint64_t sent;
  uint64_t sending;
  uint64_t failed;

  assert_int_equal(st_message(s, "sms", "+1", "in", false, "k1"), 0);
  c  = s->w.conversation;
  in = s->w.message;
  assert_int_equal(st_message(s, "sms", "+1", "sent", true, NULL), 0);
  sent = s->w.message;
  assert_int_equal(st_message(s, "sms", "+1", "sending", true, NULL), 0);
  sending = s->w.message;
  assert_int_equal(st_message(s, "sms", "+1", "failed", true, NULL), 0);
  failed = s->w.message;

  assert_int_equal(st_run(s, store_plan_status(&s->store, sent,
                   PNUT_MESSAGES_MESSAGE_STATUS_SENT, "", &s->w)), 0);
  assert_int_equal(st_run(s, store_plan_status(&s->store, sending,
                   PNUT_MESSAGES_MESSAGE_STATUS_SENDING, "", &s->w)), 0);
  assert_int_equal(st_run(s, store_plan_status(&s->store, failed,
                   PNUT_MESSAGES_MESSAGE_STATUS_FAILED, "no network",
                   &s->w)), 0);
  assert_int_equal(store_plan_status(&s->store, sent,
                   PNUT_MESSAGES_MESSAGE_STATUS_SENT, "", &s->w),
                   -EALREADY);
  assert_int_equal(store_plan_status(&s->store, 999,
                   PNUT_MESSAGES_MESSAGE_STATUS_SENT, "", &s->w), -ENOENT);

  assert_int_equal(st_run(s, store_plan_read(&s->store, c, 0, &s->w)), 0);
  assert_int_equal(store_plan_read(&s->store, c, 0, &s->w), -EALREADY);

  st_reload(s);
  conv = store_find(&s->store, c);
  assert_non_null(conv);
  assert_int_equal(conv->unread, 0);
  assert_int_equal(store_msg(conv, in)->status,
                   PNUT_MESSAGES_MESSAGE_STATUS_READ);
  assert_int_equal(store_msg(conv, sent)->status,
                   PNUT_MESSAGES_MESSAGE_STATUS_SENT);
  assert_int_equal(store_msg(conv, sending)->status,
                   PNUT_MESSAGES_MESSAGE_STATUS_UNCONFIRMED);
  assert_int_equal(store_msg(conv, failed)->status,
                   PNUT_MESSAGES_MESSAGE_STATUS_FAILED);
  assert_string_equal(store_reason(&s->store, failed), "no network");

  st_page(s, c, 0, &page);
  assert_string_equal(page.messages[0].reason, "no network");

  /* The newest deleted: its text is read again from the one before */

  assert_int_equal(st_run(s, store_plan_delete(&s->store, c, failed,
                                               &s->w)), 0);
  assert_int_equal(store_plan_delete(&s->store, c, failed, &s->w),
                   -ENOENT);
  conv = store_find(&s->store, c);
  assert_true(conv->stale);
  assert_int_equal(store_plan_latest(&s->store, c, &s->r), 0);
  store_read_run(&s->r);
  store_latest(&s->store, &s->r);
  assert_string_equal(store_find(&s->store, c)->latest, "sending");
  assert_string_equal(store_reason(&s->store, failed), "");

  st_reload(s);
  assert_int_equal(store_conversations(&s->store, 0, &convs), 0);
  assert_int_equal(convs.conversations[0].count, 3);
  assert_string_equal(convs.conversations[0].latest, "sending");

  st_free(s);
}

/****************************************************************************
 * Name: test_store_cut
 *
 * Description:
 *   A segment whose last record a reset cut short is cut back to its last
 *   whole record; a rewrite's temporary file and an empty segment are
 *   removed; a file with no head is left aside.
 *
 ****************************************************************************/

static void test_store_cut(void **state)
{
  FAR struct st_s *s = st_new(S_LIMIT, STORE_SEGMENT_MAX);
  char path[STORE_PATH_MAX + 8];
  char other[STORE_PATH_MAX];
  off_t whole;
  int fd;

  assert_int_equal(st_message(s, "sms", "+1", "kept", false, "k1"), 0);
  strlcpy(path, s->w.path, sizeof(path));
  whole = st_size(path);

  fd = open(path, O_WRONLY | O_APPEND);
  assert_true(fd >= 0);
  assert_int_equal(write(fd, "\x40\x00\x12\x34", 4), 4);
  close(fd);

  snprintf(other, sizeof(other), "%s/%016x-%08x.log", s->dir, 0x77, 1);
  fd = open(other, O_WRONLY | O_CREAT, 0600);
  close(fd);
  strlcat(path, ".new", sizeof(path));
  fd = open(path, O_WRONLY | O_CREAT, 0600);
  close(fd);
  snprintf(other, sizeof(other), "%s/%016x-%08x.log", s->dir, 0x78, 1);
  fd = open(other, O_WRONLY | O_CREAT, 0600);
  assert_int_equal(write(fd, "garbage garbage", 15), 15);
  close(fd);

  st_reload(s);
  assert_int_equal(s->store.cut, 1);
  assert_int_equal(s->store.bad, 1);
  path[strlen(path) - 4] = '\0';
  assert_int_equal(st_size(path), whole);
  assert_int_equal(st_files(s->dir), 2);    /* The segment and the bad */
  assert_int_equal(store_unread(&s->store), 1);

  /* A new message goes on after the whole record */

  assert_int_equal(st_message(s, "sms", "+1", "after", false, "k2"), 0);
  st_reload(s);
  assert_int_equal(s->store.cut, 0);
  assert_int_equal(store_unread(&s->store), 2);

  st_free(s);
}

/****************************************************************************
 * Name: test_store_segments
 *
 * Description:
 *   A conversation's log in segments, each with its head; read back
 *   across them; a status written in a later segment than its message.
 *
 ****************************************************************************/

static void test_store_segments(void **state)
{
  FAR struct st_s *s = st_new(S_LIMIT, 300);
  pnut_messages_page_t page;
  FAR struct store_conv_s *conv;
  char text[64];
  uint64_t first;
  uint64_t c;
  int i;

  assert_int_equal(st_message(s, "sms", "+1", "message 0, going out",
                              true, NULL), 0);
  c     = s->w.conversation;
  first = s->w.message;
  for (i = 1; i < 20; i++)
    {
      snprintf(text, sizeof(text), "message %d, a little longer", i);
      assert_int_equal(st_message(s, "sms", "+1", text, false, NULL), 0);
    }

  conv = store_find(&s->store, c);
  assert_true(conv->nsegs >= 3);
  assert_int_equal(st_run(s, store_plan_status(&s->store, first,
                   PNUT_MESSAGES_MESSAGE_STATUS_DELIVERED, "", &s->w)), 0);
  assert_true(s->w.segment > 1);

  st_reload(s);
  conv = store_find(&s->store, c);
  assert_true(conv->nsegs >= 3);
  assert_int_equal(conv->count, 20);
  assert_int_equal(store_msg(conv, first)->status,
                   PNUT_MESSAGES_MESSAGE_STATUS_DELIVERED);
  assert_int_equal(st_files(s->dir), (int)conv->nsegs);
  assert_true(conv->segs[0].size <= 300);

  st_page(s, c, 0, &page);
  assert_string_equal(page.messages[0].text, "message 19, a little longer");

  st_free(s);
}

/****************************************************************************
 * Name: test_store_room
 *
 * Description:
 *   Over its limit, the store refuses a message; room is made a segment
 *   at a time, oldest first, without the messages read or done with;
 *   unread and queued ones stay, with a status written in another
 *   segment; a conversation left empty is the caller's to remove.
 *
 ****************************************************************************/

static void test_store_room(void **state)
{
  FAR struct st_s *s = st_new(1200, 400);
  struct store_rewrite_s rw;
  struct store_remove_s rm;
  FAR struct store_conv_s *conv;
  uint64_t queued;
  uint64_t c;
  uint64_t d;
  int i;
  int ret;

  /* An outgoing message, still queued; read ones after it */

  assert_int_equal(st_message(s, "sms", "+1", "queued, stays", true, NULL),
                   0);
  c      = s->w.conversation;
  queued = s->w.message;
  for (i = 0; i < 6; i++)
    {
      ret = st_message(s, "sms", "+1", "read, may go when room is needed",
                       false, NULL);
      assert_int_equal(ret, 0);
    }

  assert_int_equal(st_run(s, store_plan_status(&s->store, queued,
                   PNUT_MESSAGES_MESSAGE_STATUS_SENDING, "", &s->w)), 0);
  assert_int_equal(st_run(s, store_plan_read(&s->store, c, 0, &s->w)), 0);

  /* Another conversation, all read, then one unread */

  assert_int_equal(st_message(s, "sms", "+2", "read too", false, NULL), 0);
  d = s->w.conversation;
  assert_int_equal(st_run(s, store_plan_read(&s->store, d, 0, &s->w)), 0);

  while ((ret = st_message(s, "sms", "+3", "unread, stays", false, NULL))
         == 0)
    {
    }

  assert_int_equal(ret, -ENOSPC);

  /* Room made while over the limit, as the module does after the
   * delivery that passed it: here with a limit nothing meets, so that
   * every message that may go goes
   */

  s->store.limit = 1;
  for (i = 0; (ret = store_plan_room(&s->store, &rw)) == 0; i++)
    {
      assert_true(i < 50);
      store_rewrite_run(&rw);
      assert_int_equal(rw.ret, 0);
      store_rewritten(&s->store, &rw);
    }

  assert_int_equal(ret, -ENOSPC);

  /* The queued message stays, its status with it */

  conv = store_find(&s->store, c);
  assert_non_null(conv);
  assert_int_equal(conv->count, 1);
  assert_int_equal(store_msg(conv, queued)->status,
                   PNUT_MESSAGES_MESSAGE_STATUS_SENDING);

  /* The conversation with nothing left goes */

  conv = store_find(&s->store, d);
  assert_non_null(conv);
  assert_int_equal(conv->nmsgs, 0);
  assert_int_equal(store_plan_remove(&s->store, d, &rm), 0);
  store_remove_run(&rm);
  assert_int_equal(rm.ret, 0);
  store_removed(&s->store, &rm);
  assert_null(store_find(&s->store, d));

  st_reload(s);
  conv = store_find(&s->store, c);
  assert_non_null(conv);
  assert_int_equal(conv->count, 1);
  assert_int_equal(store_msg(conv, queued)->status,
                   PNUT_MESSAGES_MESSAGE_STATUS_UNCONFIRMED);
  assert_null(store_find(&s->store, d));
  assert_true(store_unread(&s->store) > 0);

  st_free(s);
}

/****************************************************************************
 * Name: test_store_broken
 *
 * Description:
 *   A write that failed and was not cut back leaves bytes after the last
 *   whole record: the next write cuts them off first, so no record
 *   follows a broken one.  A directory's name too long is refused.
 *
 ****************************************************************************/

static void test_store_broken(void **state)
{
  FAR struct st_s *s = st_new(S_LIMIT, STORE_SEGMENT_MAX);
  struct store_s longdir;
  char path[STORE_PATH_MAX];
  char dir[STORE_DIR_MAX + 8];
  int fd;

  assert_int_equal(st_message(s, "sms", "+1", "first", false, "k1"), 0);
  strlcpy(path, s->w.path, sizeof(path));
  fd = open(path, O_WRONLY | O_APPEND);
  assert_true(fd >= 0);
  assert_int_equal(write(fd, "\x30\x00\x12\x34\x56", 5), 5);
  close(fd);

  assert_int_equal(st_message(s, "sms", "+1", "second", false, "k2"), 0);
  assert_int_equal(st_message(s, "sms", "+1", "third", false, "k3"), 0);
  st_reload(s);
  assert_int_equal(s->store.cut, 0);
  assert_int_equal(store_find(&s->store, s->w.conversation)->count, 3);

  memset(dir, 'd', sizeof(dir) - 1);
  dir[0] = '/';
  dir[sizeof(dir) - 1] = '\0';
  store_init(&longdir, dir, S_LIMIT, STORE_SEGMENT_MAX);
  assert_int_equal(store_load(&longdir), -ENAMETOOLONG);
  store_free(&longdir);

  st_free(s);
}

/****************************************************************************
 * Name: test_store_records
 *
 * Description:
 *   Statuses and deletions written in later segments than their messages
 *   go once their messages have gone: making room empties the
 *   conversation, and every segment with it.
 *
 ****************************************************************************/

static void test_store_records(void **state)
{
  FAR struct st_s *s = st_new(S_LIMIT, 400);
  struct store_rewrite_s rw;
  FAR struct store_conv_s *conv;
  uint64_t ids[12];
  uint64_t c = 0;
  int ret;
  int i;

  for (i = 0; i < 12; i++)
    {
      assert_int_equal(st_message(s, "sms", "+1", "going out, then sent",
                                  true, NULL), 0);
      ids[i] = s->w.message;
      c = s->w.conversation;
    }

  for (i = 0; i < 12; i++)
    {
      assert_int_equal(st_run(s, store_plan_status(&s->store, ids[i],
                       PNUT_MESSAGES_MESSAGE_STATUS_SENT, "", &s->w)), 0);
    }

  assert_int_equal(st_run(s, store_plan_delete(&s->store, c, ids[3],
                                               &s->w)), 0);
  assert_int_equal(st_run(s, store_plan_delete(&s->store, c, ids[7],
                                               &s->w)), 0);

  s->store.limit = 1;
  for (i = 0; (ret = store_plan_room(&s->store, &rw)) == 0; i++)
    {
      assert_true(i < 50);
      store_rewrite_run(&rw);
      assert_int_equal(rw.ret, 0);
      store_rewritten(&s->store, &rw);
    }

  /* Nothing left: within any limit */

  assert_int_equal(ret, -EALREADY);
  conv = store_find(&s->store, c);
  assert_non_null(conv);
  assert_int_equal(conv->nmsgs, 0);
  assert_int_equal(conv->nsegs, 0);
  assert_int_equal(s->store.bytes, 0);
  assert_int_equal(st_files(s->dir), 0);

  st_free(s);
}

/****************************************************************************
 * Name: test_store_ids
 *
 * Description:
 *   A conversation read, the messages of its second segment deleted and
 *   compacted away, the read record kept there: after a restart, ids go
 *   on from the highest given, and a new message is not covered by the
 *   read record.
 *
 ****************************************************************************/

static void test_store_ids(void **state)
{
  FAR struct st_s *s = st_new(S_LIMIT, 6000);
  FAR struct store_conv_s *conv;
  struct store_rewrite_s rw;
  uint64_t gone[160];
  uint64_t newest = 0;
  uint64_t c = 0;
  uint32_t last;
  uint32_t i;
  int n = 0;

  for (i = 0; i < 160; i++)
    {
      assert_int_equal(st_message(s, "sms", "+1",
                                  "a message long enough to count, read "
                                  "and deleted", false, NULL), 0);
      newest = s->w.message;
      c = s->w.conversation;
    }

  assert_int_equal(st_run(s, store_plan_read(&s->store, c, 0, &s->w)), 0);
  conv = store_find(&s->store, c);
  assert_true(conv->nsegs >= 2);
  last = conv->segs[conv->nsegs - 1].number;
  for (i = 0; i < conv->nmsgs; i++)
    {
      if (conv->msgs[i].segment == last)
        {
          gone[n++] = conv->msgs[i].id;
        }
    }

  for (i = 0; i < (uint32_t)n; i++)
    {
      assert_int_equal(st_run(s, store_plan_delete(&s->store, c, gone[i],
                                                   &s->w)), 0);
    }

  /* The segment rewritten: its head and the read record, which covers
   * messages in the first
   */

  assert_int_equal(store_plan_compact(&s->store, &rw), 0);
  assert_int_equal(rw.segment, last);
  store_rewrite_run(&rw);
  assert_int_equal(rw.ret, 0);
  assert_true(rw.size > 0);
  store_rewritten(&s->store, &rw);

  st_reload(s);
  conv = store_find(&s->store, c);
  assert_int_equal(conv->read, newest);
  assert_true(s->store.next_msg > newest);
  assert_int_equal(st_message(s, "sms", "+1", "new", false, NULL), 0);
  assert_true(s->w.message > newest);
  assert_int_equal(store_find(&s->store, c)->unread, 1);
  st_reload(s);
  assert_int_equal(store_find(&s->store, c)->unread, 1);

  st_free(s);
}

/****************************************************************************
 * Name: test_store_compact
 *
 * Description:
 *   A segment mostly of deleted messages is rewritten without them, the
 *   others kept as they were, also after a restart.
 *
 ****************************************************************************/

static void test_store_compact(void **state)
{
  FAR struct st_s *s = st_new(S_LIMIT, STORE_SEGMENT_MAX);
  struct store_rewrite_s rw;
  pnut_messages_page_t page;
  uint64_t ids[60];
  uint64_t before;
  uint64_t c = 0;
  int i;

  for (i = 0; i < 60; i++)
    {
      assert_int_equal(st_message(s, "sms", "+1",
                                  "a message long enough to count, kept or "
                                  "deleted", false, NULL), 0);
      ids[i] = s->w.message;
      c = s->w.conversation;
    }

  /* Over STORE_COMPACT_MIN, with nothing deleted: nothing to compact,
   * also as loaded again
   */

  for (i = 0; i < 100; i++)
    {
      assert_int_equal(st_message(s, "sms", "+2",
                                  "another conversation's message, which "
                                  "stays", false, NULL), 0);
    }

  assert_true(store_find_peer(&s->store, "sms", "+2")->segs[0].size >=
              STORE_COMPACT_MIN);
  assert_int_equal(store_plan_compact(&s->store, &rw), -EALREADY);
  st_reload(s);
  assert_int_equal(store_plan_compact(&s->store, &rw), -EALREADY);
  for (i = 0; i < 45; i++)
    {
      assert_int_equal(st_run(s, store_plan_delete(&s->store, c, ids[i],
                                                   &s->w)), 0);
    }

  before = store_find(&s->store, c)->segs[0].size;
  assert_int_equal(store_plan_compact(&s->store, &rw), 0);
  store_rewrite_run(&rw);
  assert_int_equal(rw.ret, 0);
  store_rewritten(&s->store, &rw);
  assert_true(store_find(&s->store, c)->segs[0].size < before / 2);
  assert_int_equal(store_find(&s->store, c)->nmsgs, 15);
  assert_int_equal(store_find(&s->store, c)->count, 15);
  assert_int_equal(store_plan_compact(&s->store, &rw), -EALREADY);

  st_reload(s);
  assert_int_equal(store_find(&s->store, c)->count, 15);
  st_page(s, c, 0, &page);
  assert_int_equal(page.messages[0].id, ids[59]);

  st_free(s);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_store_frame),
    cmocka_unit_test(test_store_messages),
    cmocka_unit_test(test_store_statuses),
    cmocka_unit_test(test_store_cut),
    cmocka_unit_test(test_store_segments),
    cmocka_unit_test(test_store_room),
    cmocka_unit_test(test_store_broken),
    cmocka_unit_test(test_store_records),
    cmocka_unit_test(test_store_compact),
    cmocka_unit_test(test_store_ids),
  };

  return cmocka_run_group_tests_name("store", tests, NULL, NULL);
}
