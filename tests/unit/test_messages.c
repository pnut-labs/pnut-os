/****************************************************************************
 * pnut-os/tests/unit/test_messages.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Messages (RFC 0029) through its interface: messages sent, listed, read,
 * deleted, a conversation removed, the topic told, all of it the same
 * after a restart; a store with no room refusing more.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <dirent.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>
#include <pnut/topic.h>

#include "messages.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct msgt_s;
typedef void (*msgt_step_t)(FAR struct msgt_s *t);

struct msgt_s
{
  FAR struct pnut_loop_s *loop;
  struct messages_s messages;
  struct pnut_module_s module;
  struct pnut_messages_client_s client;
  struct pnut_messages_client_s other;   /* A second connection */
  struct pnut_messages_change_reader_s changes;
  char dir[64];
  char store[80];
  uint64_t limit;
  uint32_t segmax;

  /* The script: each step asks, and its answer leads to the next */

  FAR const msgt_step_t *steps;
  int step;
  bool asked;

  /* What came back */

  int status;
  uint32_t code;
  pnut_messages_conversations_t convs;
  pnut_messages_page_t page;
  pnut_messages_sent_t sent;
  pnut_messages_delivered_t delivered;
  uint64_t a;                     /* The first conversation */
  uint64_t b;                     /* The second */
  uint64_t second;                /* A message of a */
  int changes_seen;
  pnut_messages_change_t change;
  int sends;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void msgt_clean(FAR const char *dir)
{
  char path[64 + 80 + 256 + 4];
  struct dirent *de;
  DIR *d;

  snprintf(path, sizeof(path), "%s/store", dir);
  d = opendir(path);
  if (d != NULL)
    {
      while ((de = readdir(d)) != NULL)
        {
          if (de->d_name[0] != '.' &&
              snprintf(path, sizeof(path), "%s/store/%s", dir, de->d_name) <
              (int)sizeof(path))
            {
              unlink(path);
            }
        }

      closedir(d);
      snprintf(path, sizeof(path), "%s/store", dir);
      rmdir(path);
    }

  snprintf(path, sizeof(path), "%s/topic.messages/last", dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/topic.messages", dir);
  rmdir(path);
  rmdir(dir);
}

static void msgt_next(FAR struct msgt_s *t)
{
  t->step++;
  if (t->steps[t->step] == NULL)
    {
      pnut_loop_stop(t->loop, 0);
      return;
    }

  t->steps[t->step](t);
}

/* Answers: kept, and the script goes on */

static void msgt_got(FAR struct msgt_s *t, int status,
                     FAR const pnut_error_t *err)
{
  t->status = status;
  t->code   = err != NULL ? err->code : 0;
  msgt_next(t);
}

static void msgt_convs_done(FAR struct pnut_messages_client_s *client,
                            int status,
                            FAR const pnut_messages_conversations_t *out,
                            FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct msgt_s *t = arg;

  memset(&t->convs, 0, sizeof(t->convs));
  if (out != NULL)
    {
      t->convs = *out;
    }

  msgt_got(t, status, err);
}

static void msgt_read_done(FAR struct pnut_messages_client_s *client,
                           int status, FAR const pnut_messages_page_t *out,
                           FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct msgt_s *t = arg;

  memset(&t->page, 0, sizeof(t->page));
  if (out != NULL)
    {
      t->page = *out;
    }

  msgt_got(t, status, err);
}

static void msgt_sent_done(FAR struct pnut_messages_client_s *client,
                           int status, FAR const pnut_messages_sent_t *out,
                           FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct msgt_s *t = arg;

  memset(&t->sent, 0, sizeof(t->sent));
  if (out != NULL)
    {
      t->sent = *out;
    }

  msgt_got(t, status, err);
}

static void msgt_delivered_done(FAR struct pnut_messages_client_s *client,
                                int status,
                                FAR const pnut_messages_delivered_t *out,
                                FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct msgt_s *t = arg;

  memset(&t->delivered, 0, sizeof(t->delivered));
  if (out != NULL)
    {
      t->delivered = *out;
    }

  msgt_got(t, status, err);
}

static void msgt_done(FAR struct pnut_messages_client_s *client,
                      int status, FAR const pnut_messages_reply_t *out,
                      FAR const pnut_error_t *err, FAR void *arg)
{
  msgt_got(arg, status, err);
}

/* Questions */

static void msgt_send(FAR struct msgt_s *t, FAR const char *peer,
                      FAR const char *text)
{
  pnut_messages_send_request_t in;

  memset(&in, 0, sizeof(in));
  strlcpy(in.transport, "sms", sizeof(in.transport));
  strlcpy(in.peer, peer, sizeof(in.peer));
  strlcpy(in.text, text, sizeof(in.text));
  assert_int_equal(pnut_messages_send(&t->client, &in, 0, msgt_sent_done,
                                      t), 0);
}

static void msgt_register(FAR struct msgt_s *t,
                          FAR struct pnut_messages_client_s *client,
                          FAR const char *name)
{
  pnut_messages_register_request_t in;

  memset(&in, 0, sizeof(in));
  strlcpy(in.transport, name, sizeof(in.transport));
  assert_int_equal(pnut_messages_register_transport(client, &in, 0,
                                                    msgt_done, t), 0);
}

static void msgt_deliver(FAR struct msgt_s *t,
                         FAR struct pnut_messages_client_s *client,
                         FAR const char *peer, FAR const char *text,
                         FAR const char *key)
{
  pnut_messages_deliver_request_t in;

  memset(&in, 0, sizeof(in));
  strlcpy(in.peer, peer, sizeof(in.peer));
  strlcpy(in.text, text, sizeof(in.text));
  strlcpy(in.key, key, sizeof(in.key));
  in.sent = 1000;
  assert_int_equal(pnut_messages_deliver(client, &in, 0,
                                         msgt_delivered_done, t), 0);
}

static void msgt_convs(FAR struct msgt_s *t)
{
  pnut_messages_page_request_t in;

  memset(&in, 0, sizeof(in));
  assert_int_equal(pnut_messages_conversations(&t->client, &in, 0,
                                               msgt_convs_done, t), 0);
}

static void msgt_read(FAR struct msgt_s *t, uint64_t conversation)
{
  pnut_messages_read_request_t in;

  memset(&in, 0, sizeof(in));
  in.conversation = conversation;
  assert_int_equal(pnut_messages_read(&t->client, &in, 0, msgt_read_done,
                                      t), 0);
}

static void msgt_delete(FAR struct msgt_s *t, uint64_t conversation,
                        uint64_t message)
{
  pnut_messages_delete_request_t in;

  memset(&in, 0, sizeof(in));
  in.conversation = conversation;
  in.message      = message;
  assert_int_equal(pnut_messages_delete(&t->client, &in, 0, msgt_done, t),
                   0);
}

static void msgt_later(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  msgt_next(arg);
}

/* A moment for Messages' own jobs after an answer: the newest text read
 * again
 */

static void msgt_wait(FAR struct msgt_s *t)
{
  assert_int_equal(pnut_timer_start(t->loop, 200, 0, msgt_later, t, NULL),
                   0);
}

/* The life: the steps of test_messages_life's first run */

static void life_send1(FAR struct msgt_s *t)
{
  msgt_send(t, "+1", "hello");
}

static void life_send2(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  t->a = t->sent.conversation;
  msgt_send(t, "+2", "other");
}

static void life_send3(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  t->b = t->sent.conversation;
  assert_true(t->a != t->b);
  msgt_send(t, "+1", "second");
}

static void life_list1(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  assert_int_equal(t->sent.conversation, t->a);
  t->second = t->sent.message;
  msgt_convs(t);
}

static void life_read1(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  assert_int_equal(t->convs.conversations_count, 2);
  assert_int_equal(t->convs.conversations[0].id, t->a);
  assert_int_equal(t->convs.conversations[0].count, 2);
  assert_string_equal(t->convs.conversations[0].latest, "second");
  assert_string_equal(t->convs.conversations[1].peer, "+2");
  msgt_read(t, t->a);
}

static void life_delete1(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  assert_int_equal(t->page.messages_count, 2);
  assert_false(t->page.more);
  assert_string_equal(t->page.messages[0].text, "second");
  assert_true(t->page.messages[0].out);
  assert_int_equal(t->page.messages[0].status,
                   PNUT_MESSAGES_MESSAGE_STATUS_QUEUED);
  assert_string_equal(t->page.messages[1].text, "hello");
  msgt_delete(t, t->a, t->second);
}

static void life_wait1(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  msgt_wait(t);
}

static void life_list2(FAR struct msgt_s *t)
{
  msgt_convs(t);
}

static void life_remove(FAR struct msgt_s *t)
{
  /* The newest message now +2's; +1's newest text read again */

  assert_int_equal(t->convs.conversations_count, 2);
  assert_int_equal(t->convs.conversations[0].id, t->b);
  assert_int_equal(t->convs.conversations[1].count, 1);
  assert_string_equal(t->convs.conversations[1].latest, "hello");
  msgt_delete(t, t->b, 0);
}

static void life_list3(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  msgt_convs(t);
}

static void life_bad_read(FAR struct msgt_s *t)
{
  assert_int_equal(t->convs.conversations_count, 1);
  assert_int_equal(t->convs.conversations[0].id, t->a);
  msgt_read(t, 999);
}

static void life_bad_send(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_NOTFOUND);
  assert_int_equal(t->code, PNUT_MESSAGES_ERROR_CODE_NOT_FOUND);
  msgt_send(t, "+3", "");
}

static void life_end(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_INVALID);
  msgt_next(t);
}

static const msgt_step_t g_life[] =
{
  life_send1, life_send2, life_send3, life_list1, life_read1,
  life_delete1, life_wait1, life_list2, life_remove, life_list3,
  life_bad_read, life_bad_send, life_end, NULL
};

/* After a restart: the same */

static void again_list(FAR struct msgt_s *t)
{
  msgt_convs(t);
}

static void again_end(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  assert_int_equal(t->convs.conversations_count, 1);
  assert_int_equal(t->convs.conversations[0].id, t->a);
  assert_int_equal(t->convs.conversations[0].count, 1);
  assert_string_equal(t->convs.conversations[0].latest, "hello");
  msgt_next(t);
}

static const msgt_step_t g_again[] =
{
  again_list, again_end, NULL
};

/* A transport: delivers once registered, on its connection alone, and a
 * key once
 */

static void tr_deliver0(FAR struct msgt_s *t)
{
  msgt_deliver(t, &t->client, "+5", "hi", "k1");
}

static void tr_register(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_DENIED);
  assert_int_equal(t->code, PNUT_MESSAGES_ERROR_CODE_UNREGISTERED);
  msgt_register(t, &t->client, "test");
}

static void tr_deliver1(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  msgt_deliver(t, &t->client, "+5", "hi", "k1");
}

static void tr_deliver2(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  assert_false(t->delivered.again);
  t->a      = t->delivered.conversation;
  t->second = t->delivered.message;
  msgt_deliver(t, &t->client, "+5", "hi", "k1");
}

static void tr_other(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  assert_true(t->delivered.again);
  assert_int_equal(t->delivered.conversation, t->a);
  assert_int_equal(t->delivered.message, t->second);
  msgt_deliver(t, &t->other, "+5", "not mine to give", "k2");
}

static void tr_nopeer(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_DENIED);
  assert_int_equal(t->code, PNUT_MESSAGES_ERROR_CODE_UNREGISTERED);
  msgt_deliver(t, &t->client, "", "no one's", "k3");
}

static void tr_nokey(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_INVALID);
  msgt_deliver(t, &t->client, "+5", "keyless", "");
}

static void tr_list(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_INVALID);
  msgt_convs(t);
}

static void tr_read(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  assert_int_equal(t->convs.conversations_count, 1);
  assert_string_equal(t->convs.conversations[0].transport, "test");
  assert_string_equal(t->convs.conversations[0].peer, "+5");
  assert_int_equal(t->convs.conversations[0].unread, 1);
  msgt_read(t, t->a);
}

static void tr_later(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  assert_int_equal(t->page.messages_count, 1);
  assert_string_equal(t->page.messages[0].text, "hi");
  assert_false(t->page.messages[0].out);
  assert_int_equal(t->page.messages[0].sent, 1000);
  assert_int_equal(t->page.messages[0].status,
                   PNUT_MESSAGES_MESSAGE_STATUS_UNREAD);
  assert_int_equal(t->change.unread, 1);
  msgt_deliver(t, &t->client, "+5", "later", "k2");
}

/* Read up to the first, shown: the one come since stays unread */

static void tr_delivered(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  msgt_wait(t);                   /* The topic told */
}

static void tr_mark(FAR struct msgt_s *t)
{
  pnut_messages_mark_read_request_t in;

  assert_int_equal(t->change.unread, 2);
  memset(&in, 0, sizeof(in));
  in.conversation = t->a;
  in.upto         = t->second;
  assert_int_equal(pnut_messages_mark_read(&t->client, &in, 0, msgt_done,
                                           t), 0);
}

static void tr_marked(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  msgt_wait(t);
}

static void tr_unread(FAR struct msgt_s *t)
{
  assert_int_equal(t->change.unread, 1);
  msgt_delete(t, t->a, t->second);
}

/* Deleted, then delivered again, as after a reset before the transport
 * let go of it: not back
 */

static void tr_redeliver(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  msgt_deliver(t, &t->client, "+5", "hi", "k1");
}

static void tr_move(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  assert_true(t->delivered.again);
  assert_int_equal(t->delivered.message, t->second);
  msgt_register(t, &t->other, "test");
}

/* Registered again on another connection, as by its program started
 * again: the new one delivers, the old one no longer
 */

static void tr_moved(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  msgt_deliver(t, &t->other, "+5", "from the new one", "k5");
}

static void tr_old(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  assert_false(t->delivered.again);
  msgt_deliver(t, &t->client, "+5", "from the old one", "k6");
}

static void tr_end(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_DENIED);
  assert_int_equal(t->code, PNUT_MESSAGES_ERROR_CODE_UNREGISTERED);
  msgt_next(t);
}

static const msgt_step_t g_transport[] =
{
  tr_deliver0, tr_register, tr_deliver1, tr_deliver2, tr_other, tr_nopeer,
  tr_nokey, tr_list, tr_read, tr_later, tr_delivered, tr_mark, tr_marked,
  tr_unread, tr_redeliver, tr_move, tr_moved, tr_old, tr_end, NULL
};

/* After a restart: registered anew, the same key still known */

static void tr_again_register(FAR struct msgt_s *t)
{
  msgt_register(t, &t->client, "test");
}

static void tr_again_deliver(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  msgt_deliver(t, &t->client, "+5", "hi", "k1");
}

static void tr_again_end(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  assert_true(t->delivered.again);
  assert_int_equal(t->delivered.conversation, t->a);
  assert_int_equal(t->delivered.message, t->second);
  msgt_next(t);
}

static const msgt_step_t g_transport_again[] =
{
  tr_again_register, tr_again_deliver, tr_again_end, NULL
};

/* A store too small for unread messages, which never go to make room:
 * a delivery refused once it is full, and a key stored already answered
 * all the same
 */

static void dfull_register(FAR struct msgt_s *t)
{
  msgt_register(t, &t->client, "test");
}

static void dfull_deliver(FAR struct msgt_s *t)
{
  char key[16];

  if (t->sends > 0 && t->status != PNUT_STATUS_OK)
    {
      msgt_next(t);
      return;
    }

  if (t->sends == 1)
    {
      t->a      = t->delivered.conversation;
      t->second = t->delivered.message;
    }

  snprintf(key, sizeof(key), "k%d", t->sends);
  t->sends++;
  assert_true(t->sends < 100);
  t->step--;                      /* This step again */
  msgt_deliver(t, &t->client, "+1", "a message unread, which may never go",
               key);
}

static void dfull_again(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_UNAVAILABLE);
  assert_int_equal(t->code, PNUT_MESSAGES_ERROR_CODE_FULL);
  msgt_deliver(t, &t->client, "+1", "a message unread, which may never go",
               "k0");
}

static void dfull_end(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  assert_true(t->delivered.again);
  assert_int_equal(t->delivered.conversation, t->a);
  assert_int_equal(t->delivered.message, t->second);
  msgt_next(t);
}

static const msgt_step_t g_dfull[] =
{
  dfull_register, dfull_deliver, dfull_again, dfull_end, NULL
};

/* A store too small: queued messages never go to make room */

static void full_send(FAR struct msgt_s *t)
{
  if (t->sends > 0 && t->status != PNUT_STATUS_OK)
    {
      msgt_next(t);
      return;
    }

  t->sends++;
  assert_true(t->sends < 100);
  t->step--;                      /* This step again */
  msgt_send(t, "+1", "a message queued, which may never go");
}

static void full_end(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_UNAVAILABLE);
  assert_int_equal(t->code, PNUT_MESSAGES_ERROR_CODE_FULL);
  msgt_wait(t);
}

static void full_told(FAR struct msgt_s *t)
{
  assert_true(t->change.full);
  msgt_next(t);
}

static const msgt_step_t g_full[] =
{
  full_send, full_end, full_told, NULL
};

/* Full, then messages deleted: room made, the store no longer full */

static void unfull_delete(FAR struct msgt_s *t)
{
  /* The oldest messages, ids 1, 2, ... in the one conversation; full
   * until the first is deleted
   */

  if (t->second == 0)
    {
      assert_true(t->change.full);
    }

  t->second++;
  msgt_delete(t, 1, t->second);
}

static void unfull_more(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  if (t->second < 6)
    {
      t->step -= 2;               /* unfull_delete again */
    }

  msgt_wait(t);
}

static void unfull_send(FAR struct msgt_s *t)
{
  assert_false(t->change.full);
  msgt_send(t, "+1", "room again");
}

static void unfull_end(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  msgt_next(t);
}

/* A segment that cannot be read: requests fail, until it can and the
 * store is read again
 */

static void retry_send(FAR struct msgt_s *t)
{
  msgt_send(t, "+1", "while broken");
}

static void retry_fix(FAR struct msgt_s *t)
{
  char path[160];

  assert_int_equal(t->status, PNUT_STATUS_INTERNAL);
  assert_int_equal(t->code, PNUT_MESSAGES_ERROR_CODE_STORE);
  snprintf(path, sizeof(path), "%s/%016x-%08x.log", t->store, 1, 1);
  assert_int_equal(rmdir(path), 0);
  assert_int_equal(pnut_timer_start(t->loop, 400, 0, msgt_later, t, NULL),
                   0);
}

static void retry_again(FAR struct msgt_s *t)
{
  msgt_send(t, "+1", "read again");
}

static void retry_end(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  msgt_next(t);
}

static const msgt_step_t g_retry[] =
{
  retry_send, retry_fix, retry_again, retry_end, NULL
};

/* A conversation of deleted messages only, as found at start: gone */

static void empty_wait(FAR struct msgt_s *t)
{
  msgt_wait(t);
}

static void empty_list(FAR struct msgt_s *t)
{
  msgt_convs(t);
}

static void empty_end(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  assert_int_equal(t->convs.conversations_count, 0);
  msgt_next(t);
}

static const msgt_step_t g_empty[] =
{
  empty_wait, empty_list, empty_end, NULL
};

/* A job that fails the same way at every try: answers go on, and the
 * store is not read again and again
 */

static void stuck_wait(FAR struct msgt_s *t)
{
  assert_int_equal(pnut_timer_start(t->loop, 800, 0, msgt_later, t, NULL),
                   0);
}

static void stuck_list(FAR struct msgt_s *t)
{
  t->changes_seen = 0;
  msgt_convs(t);
}

static void stuck_count(FAR struct msgt_s *t)
{
  assert_int_equal(t->status, PNUT_STATUS_OK);
  t->changes_seen = 0;
  assert_int_equal(pnut_timer_start(t->loop, 800, 0, msgt_later, t, NULL),
                   0);
}

static void stuck_end(FAR struct msgt_s *t)
{
  /* Reading again every 100 ms would announce 8 times */

  assert_true(t->changes_seen <= 1);
  msgt_next(t);
}

static const msgt_step_t g_stuck[] =
{
  stuck_wait, stuck_list, stuck_count, stuck_end, NULL
};

/* A message written with the store itself, before Messages starts */

static void msgt_put(FAR struct store_s *store, FAR struct store_write_s *w,
                     int status, int len, FAR uint64_t *id,
                     FAR uint64_t *conversation)
{
  pnut_messages_message_record_t m;

  memset(&m, 0, sizeof(m));
  m.out    = true;
  m.status = status;
  memset(m.text, 'x', len);
  m.text[len] = '\0';
  assert_int_equal(store_plan_message(store, "sms", "+1", &m, w), 0);
  store_write_run(w);
  assert_int_equal(w->ret, 0);
  assert_int_equal(store_commit(store, w), 0);
  *id           = w->message;
  *conversation = w->conversation;
}

static const msgt_step_t g_unfull[] =
{
  full_send, full_end, full_told, unfull_delete, unfull_more, unfull_send,
  unfull_end, NULL
};

/* The harness */

static void msgt_change(FAR struct pnut_messages_change_reader_s *reader,
                        FAR const pnut_messages_change_t *msg,
                        FAR void *arg)
{
  FAR struct msgt_s *t = arg;

  t->change = *msg;
  t->changes_seen++;
}

static void msgt_other_state(FAR struct pnut_messages_client_s *client,
                             bool connected, FAR void *arg)
{
}

static void msgt_state(FAR struct pnut_messages_client_s *client,
                       bool connected, FAR void *arg)
{
  FAR struct msgt_s *t = arg;

  if (connected && !t->asked)
    {
      t->asked = true;
      t->step  = -1;
      msgt_next(t);
    }
}

static int msgt_start(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct msgt_s *t = (FAR struct msgt_s *)
    ((FAR uint8_t *)module - offsetof(struct msgt_s, module));

  assert_int_equal(pnut_messages_change_subscribe(loop, msgt_change, t,
                                                  &t->changes), 0);
  assert_int_equal(pnut_messages_connect(loop, MESSAGES_SERVICE,
                                         msgt_other_state, t, &t->other), 0);
  assert_int_equal(pnut_messages_connect(loop, MESSAGES_SERVICE, msgt_state,
                                         t, &t->client), 0);
  pnut_module_ready(module);
  return 0;
}

static void msgt_stop(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct msgt_s *t = (FAR struct msgt_s *)
    ((FAR uint8_t *)module - offsetof(struct msgt_s, module));

  pnut_messages_disconnect(&t->client);
  pnut_messages_disconnect(&t->other);
  pnut_messages_change_unsubscribe(&t->changes);
}

static void msgt_timeout(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, -ETIMEDOUT);
}

/* One run of the program: Messages and the script's module */

static int msgt_run(FAR struct msgt_s *t, FAR const msgt_step_t *steps)
{
  struct pnut_loop_config_s config;
  struct messages_config_s mconfig;
  int ret;

  pnut_loop_defaults(&config);
  config.rundir = t->dir;
  assert_int_equal(pnut_loop_create(&config, &t->loop), 0);

  memset(&mconfig, 0, sizeof(mconfig));
  mconfig.dir    = t->store;
  mconfig.limit  = t->limit;
  mconfig.segmax = t->segmax;
  mconfig.retry  = 100;
  messages_init(&t->messages, &mconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->messages.module), 0);

  memset(&t->module, 0, sizeof(t->module));
  t->module.name  = "test";
  t->module.start = msgt_start;
  t->module.stop  = msgt_stop;
  assert_int_equal(pnut_module_add(t->loop, &t->module), 0);

  t->steps = steps;
  t->asked = false;
  pnut_timer_start(t->loop, 10000, 0, msgt_timeout, NULL, NULL);
  ret = pnut_loop_run(t->loop);
  pnut_loop_destroy(t->loop);
  messages_deinit(&t->messages);
  return ret;
}

static FAR struct msgt_s *msgt_new(uint64_t limit, uint32_t segmax)
{
  static int n;
  FAR struct msgt_s *t = calloc(1, sizeof(*t));

  assert_non_null(t);
  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-msg-%d-%d", (int)getpid(),
           n++);
  msgt_clean(t->dir);
  assert_int_equal(mkdir(t->dir, 0700), 0);
  snprintf(t->store, sizeof(t->store), "%s/store", t->dir);
  t->limit  = limit;
  t->segmax = segmax;
  return t;
}

static void msgt_free(FAR struct msgt_s *t)
{
  msgt_clean(t->dir);
  free(t);
}

/****************************************************************************
 * Name: test_messages_life
 *
 * Description:
 *   Messages sent into two conversations, listed and read; the newest
 *   deleted, and the newest text read again; a conversation removed; a
 *   wrong request refused; the topic told each time; all of it the same
 *   after a restart.
 *
 ****************************************************************************/

static void test_messages_life(void **state)
{
  FAR struct msgt_s *t = msgt_new(64 * 1024, STORE_SEGMENT_MAX);

  assert_int_equal(msgt_run(t, g_life), 0);
  assert_true(t->changes_seen >= 5);
  assert_int_equal(t->change.unread, 0);
  assert_false(t->change.full);

  assert_int_equal(msgt_run(t, g_again), 0);

  msgt_free(t);
}

/****************************************************************************
 * Name: test_messages_transport
 *
 * Description:
 *   A transport delivers once registered, on the connection it registered
 *   on alone, the newest one; a key is stored once, also deleted since and
 *   after a restart; a delivery without a key or a peer refused; an unread
 *   message counts on the topic, and one come since a read stays unread;
 *   a store full of unread messages refuses a delivery, and answers one
 *   whose key it holds.
 *
 ****************************************************************************/

static void test_messages_transport(void **state)
{
  FAR struct msgt_s *t = msgt_new(64 * 1024, STORE_SEGMENT_MAX);

  assert_int_equal(msgt_run(t, g_transport), 0);
  assert_int_equal(msgt_run(t, g_transport_again), 0);
  msgt_free(t);

  /* Full: refused, and a key stored already answered "again" */

  t = msgt_new(600, 300);
  assert_int_equal(msgt_run(t, g_dfull), 0);
  assert_true(t->sends > 1);
  assert_true(t->change.full);
  msgt_free(t);
}

/****************************************************************************
 * Name: test_messages_full
 *
 * Description:
 *   A store whose messages are all queued, which never go to make room:
 *   once over its limit, it refuses more, and the topic says it is full.
 *
 ****************************************************************************/

static void test_messages_full(void **state)
{
  FAR struct msgt_s *t = msgt_new(600, 300);

  assert_int_equal(msgt_run(t, g_full), 0);
  assert_true(t->sends > 1);

  msgt_free(t);
}

/****************************************************************************
 * Name: test_messages_empty
 *
 * Description:
 *   A conversation whose every message was deleted, its removal not done
 *   before a restart: removed at start, its file with it.
 *
 ****************************************************************************/

static void test_messages_empty(void **state)
{
  FAR struct msgt_s *t = msgt_new(64 * 1024, STORE_SEGMENT_MAX);
  FAR struct store_write_s *w = calloc(1, sizeof(*w));
  pnut_messages_message_record_t m;
  struct store_s store;
  uint64_t ids[3];
  uint64_t c = 0;
  struct stat st;
  char path[160];
  int i;

  assert_non_null(w);
  store_init(&store, t->store, t->limit, t->segmax);
  assert_int_equal(store_load(&store), 0);
  for (i = 0; i < 3; i++)
    {
      memset(&m, 0, sizeof(m));
      m.out    = true;
      m.status = PNUT_MESSAGES_MESSAGE_STATUS_QUEUED;
      strlcpy(m.text, "to be deleted", sizeof(m.text));
      assert_int_equal(store_plan_message(&store, "sms", "+1", &m, w), 0);
      store_write_run(w);
      assert_int_equal(store_commit(&store, w), 0);
      ids[i] = w->message;
      c = w->conversation;
    }

  for (i = 0; i < 3; i++)
    {
      assert_int_equal(store_plan_delete(&store, c, ids[i], w), 0);
      store_write_run(w);
      assert_int_equal(store_commit(&store, w), 0);
    }

  store_free(&store);
  free(w);

  snprintf(path, sizeof(path), "%s/%016x-%08x.log", t->store, 1, 1);
  assert_int_equal(stat(path, &st), 0);
  assert_int_equal(msgt_run(t, g_empty), 0);
  assert_int_equal(stat(path, &st), -1);

  msgt_free(t);
}

/****************************************************************************
 * Name: test_messages_stuck
 *
 * Description:
 *   A conversation of deleted messages whose file cannot be removed (its
 *   directory read-only), and a store over its limit whose rewrite cannot
 *   be written (files limited to 512 bytes): Messages goes on answering,
 *   without reading the store again and again.  As root the directory
 *   does not stop the removal, and the first case passes all the same.
 *
 ****************************************************************************/

static void test_messages_stuck(void **state)
{
  FAR struct msgt_s *t = msgt_new(64 * 1024, STORE_SEGMENT_MAX);
  FAR struct store_write_s *w = calloc(1, sizeof(*w));
  struct store_s store;
  struct rlimit rl;
  rlim_t was;
  uint64_t id;
  uint64_t c;

  assert_non_null(w);
  store_init(&store, t->store, t->limit, t->segmax);
  assert_int_equal(store_load(&store), 0);
  msgt_put(&store, w, PNUT_MESSAGES_MESSAGE_STATUS_QUEUED, 10, &id, &c);
  assert_int_equal(store_plan_delete(&store, c, id, w), 0);
  store_write_run(w);
  assert_int_equal(store_commit(&store, w), 0);
  store_free(&store);

  chmod(t->store, 0500);
  assert_int_equal(msgt_run(t, g_stuck), 0);
  chmod(t->store, 0700);
  msgt_free(t);

  t = msgt_new(200, STORE_SEGMENT_MAX);
  store_init(&store, t->store, 64 * 1024, t->segmax);
  assert_int_equal(store_load(&store), 0);
  msgt_put(&store, w, PNUT_MESSAGES_MESSAGE_STATUS_SENT, 10, &id, &c);
  msgt_put(&store, w, PNUT_MESSAGES_MESSAGE_STATUS_QUEUED, 1200, &id, &c);
  store_free(&store);
  free(w);

  signal(SIGXFSZ, SIG_IGN);
  assert_int_equal(getrlimit(RLIMIT_FSIZE, &rl), 0);
  was = rl.rlim_cur;
  rl.rlim_cur = 512;
  assert_int_equal(setrlimit(RLIMIT_FSIZE, &rl), 0);
  assert_int_equal(msgt_run(t, g_stuck), 0);
  rl.rlim_cur = was;
  assert_int_equal(setrlimit(RLIMIT_FSIZE, &rl), 0);
  signal(SIGXFSZ, SIG_DFL);
  assert_true(t->change.full);

  msgt_free(t);
}

/****************************************************************************
 * Name: test_messages_retry
 *
 * Description:
 *   A store with a segment that cannot be read (here a directory in its
 *   place) fails requests, and is read again by itself once it can be.
 *
 ****************************************************************************/

static void test_messages_retry(void **state)
{
  FAR struct msgt_s *t = msgt_new(64 * 1024, STORE_SEGMENT_MAX);
  char path[160];

  assert_int_equal(mkdir(t->store, 0700), 0);
  snprintf(path, sizeof(path), "%s/%016x-%08x.log", t->store, 1, 1);
  assert_int_equal(mkdir(path, 0700), 0);
  assert_int_equal(msgt_run(t, g_retry), 0);

  msgt_free(t);
}

/****************************************************************************
 * Name: test_messages_unfull
 *
 * Description:
 *   A full store whose messages are deleted: room is made, the topic says
 *   it is no longer full, and a message goes in again.
 *
 ****************************************************************************/

static void test_messages_unfull(void **state)
{
  FAR struct msgt_s *t = msgt_new(600, 300);

  assert_int_equal(msgt_run(t, g_unfull), 0);
  assert_false(t->change.full);

  msgt_free(t);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_messages_life),
    cmocka_unit_test(test_messages_transport),
    cmocka_unit_test(test_messages_full),
    cmocka_unit_test(test_messages_unfull),
    cmocka_unit_test(test_messages_retry),
    cmocka_unit_test(test_messages_empty),
    cmocka_unit_test(test_messages_stuck),
  };

  return cmocka_run_group_tests_name("messages", tests, NULL, NULL);
}
