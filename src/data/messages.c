/****************************************************************************
 * pnut-os/src/data/messages.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Messages (RFC 0029): the device's one store of text messages, served to
 * the system UI and the developer's shell, and filled by the transports
 * (sms, Telephony's).  The store's files are read at start on a worker;
 * every change after that goes to the workers one at a time, and is
 * answered once it is on flash.  Message texts and peers are never logged
 * (RFC 0026).
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pnut/log.h>
#include <pnut/msg.h>
#include <pnut/worker.h>

#include "messages.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void msg_next(FAR struct messages_s *m);
static void msg_written(FAR struct pnut_loop_s *loop, FAR void *arg);
static bool msg_submit(FAR struct messages_s *m, uint8_t kind,
                       pnut_job_run_t run, pnut_job_done_t done,
                       FAR void *work);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: msg_from_work
 *
 * Description:
 *   The module whose work a job had: each job is given its part of the
 *   module's work, and touches nothing else.
 *
 ****************************************************************************/

static FAR struct messages_s *msg_from_work(FAR void *arg)
{
  return (FAR struct messages_s *)((FAR char *)arg -
                                   offsetof(struct messages_s, work));
}

/****************************************************************************
 * Name: msg_fail
 ****************************************************************************/

static void msg_fail(FAR struct messages_s *m,
                     FAR const struct pnut_request_s *req, int status,
                     uint32_t code, FAR const char *text)
{
  pnut_messages_fail(&m->server, req, status, code, text);
}

/****************************************************************************
 * Name: msg_publish
 *
 * Description:
 *   That a conversation changed, on the messages topic: its id, the unread
 *   messages in all, whether the store is full; no text, no peer.
 *
 ****************************************************************************/

static void msg_publish(FAR struct messages_s *m, uint64_t conversation)
{
  pnut_messages_change_t msg;
  int ret;

  if (m->changes == NULL)
    {
      return;
    }

  memset(&msg, 0, sizeof(msg));
  msg.conversation = conversation;
  msg.unread       = store_unread(&m->store);
  msg.full         = m->full;
  msg.seq          = ++m->seq;
  ret = pnut_messages_change_publish(m->changes, &msg);
  if (ret < 0)
    {
      pnut_log(&m->module, PNUT_LOG_WARNING,
               "Cannot announce a change: %d", ret);
    }
}

/****************************************************************************
 * Name: msg_slot
 *
 * Description:
 *   A place in the queue for a job, cleared, to be filled there (a job is
 *   too large for the loop's stack): at the front for Messages' own,
 *   which put the store in order before the next caller's.
 *
 * Returned Value:
 *   The place; NULL when the queue is full.
 *
 ****************************************************************************/

static FAR struct messages_job_s *msg_slot(FAR struct messages_s *m,
                                           bool front)
{
  FAR struct messages_job_s *job;
  uint8_t i;

  if (m->count == MESSAGES_QUEUE)
    {
      return NULL;
    }

  if (front)
    {
      m->head = (uint8_t)((m->head + MESSAGES_QUEUE - 1) % MESSAGES_QUEUE);
      i = m->head;
    }
  else
    {
      i = (uint8_t)((m->head + m->count) % MESSAGES_QUEUE);
    }

  m->count++;
  job = &m->queue[i];
  memset(job, 0, sizeof(*job));
  return job;
}

/****************************************************************************
 * Name: msg_own
 *
 * Description:
 *   One of Messages' own jobs about a conversation, unless it waits
 *   already.  A full queue drops it: the newest text is read again at the
 *   next start, and a conversation left empty removed then.
 *
 ****************************************************************************/

static void msg_own(FAR struct messages_s *m, uint8_t kind,
                    uint64_t conversation)
{
  FAR struct messages_job_s *job;
  uint8_t i;

  for (i = 0; i < m->count; i++)
    {
      job = &m->queue[(m->head + i) % MESSAGES_QUEUE];
      if (!job->answer && job->kind == kind &&
          job->in.conversation == conversation)
        {
          return;                 /* Waiting already */
        }
    }

  job = msg_slot(m, true);
  if (job != NULL)
    {
      job->kind = kind;
      job->in.conversation = conversation;
    }
}

/****************************************************************************
 * Name: msg_after
 *
 * Description:
 *   After a change: room made where the store passed its limit, the store
 *   no longer full once it is within it, a segment of deleted messages
 *   rewritten after a deletion (RFC 0029), and a conversation's newest
 *   text read again where its newest went.  Announced.
 *
 ****************************************************************************/

static void msg_after(FAR struct messages_s *m, uint64_t conversation,
                      bool deleted)
{
  FAR struct store_conv_s *conv;

  if (m->full && m->store.bytes <= m->store.limit)
    {
      m->full = false;
    }

  msg_publish(m, conversation);
  conv = store_find(&m->store, conversation);
  if (conv != NULL && conv->nsegs > 0 && conv->count == 0)
    {
      /* Every message deleted: its files go, the texts with them */

      msg_own(m, MESSAGES_EMPTY, conversation);
    }
  else if (conv != NULL && conv->stale)
    {
      msg_own(m, MESSAGES_LATEST, conversation);
    }

  m->compact = m->compact || deleted;
  m->room    = m->store.bytes > m->store.limit;
}

/****************************************************************************
 * Name: msg_work_free
 *
 * Description:
 *   What a plan took from the heap, given back once its job has run, or
 *   could not.
 *
 ****************************************************************************/

static void msg_work_free(FAR struct messages_s *m)
{
  switch (m->work_kind)
    {
      case MESSAGES_WORK_LOAD:
        store_free(&m->work.load.store);
        break;

      case MESSAGES_WORK_REMOVE:
        free(m->work.remove.paths);
        m->work.remove.paths = NULL;
        break;

      case MESSAGES_WORK_REWRITE:
        free(m->work.rewrite.kept);
        free(m->work.rewrite.live);
        m->work.rewrite.kept = NULL;
        m->work.rewrite.live = NULL;
        break;

      default:
        break;
    }

  m->work_kind = MESSAGES_WORK_NONE;
}

/****************************************************************************
 * Name: msg_store_failed
 *
 * Description:
 *   A write, read or removal that failed on the flash: logged, and its
 *   caller told.
 *
 ****************************************************************************/

static void msg_store_failed(FAR struct messages_s *m, int ret)
{
  pnut_log(&m->module, PNUT_LOG_ERROR, "The store failed: %d", ret);
  if (m->current.answer)
    {
      msg_fail(m, &m->current.req, PNUT_STATUS_INTERNAL,
               PNUT_MESSAGES_ERROR_CODE_STORE, "the store failed");
    }
}

/****************************************************************************
 * Name: msg_*_run
 *
 * Description:
 *   The jobs, on a worker: each given its part of the work.
 *
 ****************************************************************************/

static void msg_load_run(FAR void *arg)
{
  FAR struct messages_load_s *load = arg;

  load->ret = store_load(&load->store);
}

static void msg_write_run(FAR void *arg)
{
  store_write_run((FAR struct store_write_s *)arg);
}

static void msg_read_run(FAR void *arg)
{
  store_read_run((FAR struct store_read_s *)arg);
}

static void msg_remove_run(FAR void *arg)
{
  store_remove_run((FAR struct store_remove_s *)arg);
}

static void msg_rewrite_run(FAR void *arg)
{
  store_rewrite_run((FAR struct store_rewrite_s *)arg);
}

/****************************************************************************
 * Name: msg_load, msg_retry, msg_broken
 *
 * Description:
 *   The store's files read on a worker: at start, and again while the
 *   store is broken, MESSAGES_RETRY ms after it broke, so that a file
 *   read wrong for a moment, or an index short of what is on flash, comes
 *   right without a restart.
 *
 ****************************************************************************/

static void msg_loaded(FAR struct pnut_loop_s *loop, FAR void *arg);
static void msg_retry(FAR struct pnut_loop_s *loop,
                      FAR struct pnut_timer_s *timer, FAR void *arg);

static uint32_t msg_retry_ms(FAR struct messages_s *m)
{
  uint32_t ms = m->config.retry != 0 ? m->config.retry : MESSAGES_RETRY;

  /* Longer each time it fails, up to 16 times as long */

  return ms << (m->retries < 4 ? m->retries : 4);
}

static void msg_retry_timer(FAR struct messages_s *m)
{
  if (pnut_timer_start(m->module.loop, msg_retry_ms(m), 0, msg_retry, m,
                       &m->retry) < 0)
    {
      m->retry = NULL;
      pnut_log(&m->module, PNUT_LOG_ERROR,
               "No timer: the store is read again at the next start");
    }
}

static int msg_load(FAR struct messages_s *m)
{
  int ret;

  store_init(&m->work.load.store, m->config.dir, m->config.limit,
             m->config.segmax);
  m->work_kind = MESSAGES_WORK_LOAD;
  ret = pnut_job_submit(m->module.loop, msg_load_run, msg_loaded,
                        &m->work.load);
  if (ret < 0)
    {
      msg_work_free(m);
      return ret;
    }

  m->working = true;
  return OK;
}

static void msg_retry(FAR struct pnut_loop_s *loop,
                      FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct messages_s *m = arg;

  m->retry = NULL;
  if (m->stopped)
    {
      return;
    }

  if (m->working || msg_load(m) < 0)
    {
      msg_retry_timer(m);
    }
}

static void msg_broken(FAR struct messages_s *m)
{
  m->failed  = true;
  m->room    = false;
  m->compact = false;
  if (m->retry == NULL)
    {
      msg_retry_timer(m);
    }
}

/****************************************************************************
 * Name: msg_loaded
 *
 * Description:
 *   The store's files read: the index taken, conversations left with no
 *   message removed, and the module ready.  A store that cannot be read
 *   fails every request that needs it.
 *
 ****************************************************************************/

static void msg_loaded(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct messages_s *m = msg_from_work(arg);
  uint32_t msgs = 0;
  uint32_t i;

  m->working = false;
  if (m->stopped)
    {
      msg_work_free(m);
      return;
    }

  m->loaded = true;
  if (m->work.load.ret < 0)
    {
      pnut_log(&m->module, m->failed ? PNUT_LOG_DEBUG : PNUT_LOG_ERROR,
               "Cannot read the store: %d; again in %" PRIu32 " ms",
               m->work.load.ret, msg_retry_ms(m));
      msg_work_free(m);
      msg_broken(m);
      if (m->retries < UINT8_MAX)
        {
          m->retries++;
        }
    }
  else
    {
      store_move(&m->store, &m->work.load.store);
      m->work_kind = MESSAGES_WORK_NONE;
      m->failed    = false;
      m->fresh     = true;
      m->retries   = 0;
      m->hand      = true;        /* A transport may have been waiting */
      for (i = 0; i < m->store.nconvs; i++)
        {
          msgs += m->store.convs[i].count;
          if (m->store.convs[i].count == 0)
            {
              msg_own(m, MESSAGES_EMPTY, m->store.convs[i].id);
            }
        }

      pnut_log(&m->module, PNUT_LOG_INFO,
               "The store: %" PRIu32 " conversations, %" PRIu32
               " messages, %" PRIu64 " bytes", m->store.nconvs, msgs,
               m->store.bytes);
      if (m->store.cut > 0)
        {
          pnut_log(&m->module, PNUT_LOG_WARNING,
                   "%" PRIu32 " segments cut back to their last whole "
                   "record", m->store.cut);
        }

      if (m->store.bad > 0)
        {
          pnut_log(&m->module, PNUT_LOG_WARNING,
                   "%" PRIu32 " files with no head left aside",
                   m->store.bad);
        }

      m->room = m->store.bytes > m->store.limit || m->full;
    }

  if (!m->module.ready)
    {
      pnut_module_ready(&m->module);
    }

  msg_publish(m, 0);
  msg_next(m);
}

/****************************************************************************
 * Name: msg_taker, msg_requeue, msg_hand_over
 *
 * Description:
 *   The transport of that name whose Take is open, or NULL; a message
 *   that could not be handed over, queued again; a message written as
 *   sending, pushed to its transport (RFC 0029).
 *
 ****************************************************************************/

static FAR struct messages_transport_s *
msg_taker(FAR struct messages_s *m, FAR const char *name)
{
  int i;

  for (i = 0; i < MESSAGES_TRANSPORTS; i++)
    {
      if (m->transports[i].used && m->transports[i].stream.open &&
          strcmp(m->transports[i].name, name) == 0)
        {
          return &m->transports[i];
        }
    }

  return NULL;
}

static void msg_requeue(FAR struct messages_s *m, uint64_t message,
                        uint8_t status)
{
  /* One at a time: the one waits before any other hand-over */

  if (m->requeue || m->requeue_held)
    {
      pnut_log(&m->module, PNUT_LOG_WARNING,
               "Message %" PRIu64 "'s status not put right", message);
      return;
    }

  m->requeue        = true;
  m->requeue_msg    = message;
  m->requeue_status = status;
}

static void msg_hand_over(FAR struct messages_s *m,
                          FAR const char *transport);

/****************************************************************************
 * Name: msg_hand_later
 *
 * Description:
 *   A hand-over that did not go (the message's read or its status's write
 *   not planned, not submitted, or failed on flash): tried again in a
 *   while, so that the queue does not stay put until something else asks
 *   for one, nor spins on a failure that lasts.  A store with no room is
 *   given some first.
 *
 ****************************************************************************/

static void msg_hand_again(FAR struct pnut_loop_s *loop,
                           FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct messages_s *m = arg;

  m->hand_later = NULL;
  m->hand       = true;
  if (m->requeue_held)
    {
      m->requeue_held = false;
      m->requeue      = true;
    }

  msg_next(m);
}

static void msg_hand_later(FAR struct messages_s *m, uint64_t id, int ret)
{
  if (ret == -ENOSPC)
    {
      m->room = true;
    }

  if (m->hand_later == NULL &&
      pnut_timer_start(m->module.loop, MESSAGES_HAND_MS, 0, msg_hand_again,
                       m, &m->hand_later) < 0)
    {
      m->hand_later = NULL;
      pnut_log(&m->module, PNUT_LOG_WARNING,
               "Message %" PRIu64 " not handed over (%d)", id, ret);
      return;
    }

  pnut_log(&m->module, PNUT_LOG_INFO,
           "Message %" PRIu64 " not handed over (%d): again in a while", id,
           ret);
}

/****************************************************************************
 * Name: msg_requeue_later
 *
 * Description:
 *   A status put right (msg_requeue()) whose write did not go: held, and
 *   tried again with the hand-overs' timer, which wait for it; not at
 *   once, so that a failure that lasts does not spin.
 *
 ****************************************************************************/

static void msg_requeue_later(FAR struct messages_s *m,
                              FAR const struct messages_job_s *job, int ret)
{
  if (m->requeue || m->requeue_held)
    {
      pnut_log(&m->module, PNUT_LOG_WARNING,
               "Message %" PRIu64 "'s status not put right (%d)",
               job->in.hand.message, ret);
      return;
    }

  m->requeue_msg    = job->in.hand.message;
  m->requeue_status = job->in.hand.status;
  m->requeue_held   = true;
  msg_hand_later(m, job->in.hand.message, ret);
  if (m->hand_later == NULL)
    {
      m->requeue_held = false;    /* No timer: the hand-overs go on */
      pnut_log(&m->module, PNUT_LOG_WARNING,
               "Message %" PRIu64 "'s status not put right (%d)",
               job->in.hand.message, ret);
    }
}

static void msg_push_again(FAR struct pnut_loop_s *loop,
                           FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct messages_s *m = arg;

  m->push = NULL;
  msg_hand_over(m, m->pushing_to);
  msg_next(m);
}

static void msg_hand_over(FAR struct messages_s *m,
                          FAR const char *transport)
{
  FAR struct messages_transport_s *t = msg_taker(m, transport);
  int ret = -ENOTCONN;

  m->pushing = false;
  if (t != NULL)
    {
      ret = pnut_messages_take_push(&m->server, &t->stream, &m->handing);
    }

  /* No room on the connection now: it stays sending, and goes in a
   * moment; nothing else is handed over meanwhile
   */

  if (ret == -EBUSY &&
      pnut_timer_start(m->module.loop, MESSAGES_PUSH_MS, 0, msg_push_again,
                       m, &m->push) == 0)
    {
      m->pushing = true;
      strlcpy(m->pushing_to, transport, sizeof(m->pushing_to));
      return;
    }

  if (ret < 0)
    {
      pnut_log(&m->module, PNUT_LOG_WARNING,
               "Message %" PRIu64 " not handed to %s: %d; queued again",
               m->handing.message, transport, ret);
      msg_requeue(m, m->handing.message,
                  PNUT_MESSAGES_MESSAGE_STATUS_QUEUED);
    }

  m->hand = true;                 /* The next, if credit is left */
}

/****************************************************************************
 * Name: msg_hand_read
 *
 * Description:
 *   A queued message read: written as sending, then handed over.
 *
 ****************************************************************************/

static void msg_hand_read(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct messages_s *m = msg_from_work(arg);
  FAR struct messages_job_s *job = &m->current;
  FAR struct store_read_s *r = &m->work.read;
  FAR struct store_conv_s *conv;
  int ret;

  m->working   = false;
  m->work_kind = MESSAGES_WORK_NONE;
  if (m->stopped)
    {
      return;
    }

  conv = store_find(&m->store, job->in.hand.conversation);
  if (r->ret < 0 || r->n != 1 || conv == NULL ||
      r->records[0].which_kind != PNUT_MESSAGES_RECORD_MESSAGE_TAG)
    {
      /* Failed, so that the queue behind it moves */

      pnut_log(&m->module, PNUT_LOG_ERROR,
               "Message %" PRIu64 " cannot be read (%d): failed",
               job->in.hand.message, r->ret);
      msg_requeue(m, job->in.hand.message,
                  PNUT_MESSAGES_MESSAGE_STATUS_FAILED);
      m->hand = true;
      msg_next(m);
      return;
    }

  /* Copied before the write's plan takes the work's place */

  memset(&m->handing, 0, sizeof(m->handing));
  m->handing.conversation = job->in.hand.conversation;
  m->handing.message      = job->in.hand.message;
  strlcpy(m->handing.peer, conv->peer, sizeof(m->handing.peer));
  strlcpy(m->handing.text, r->records[0].kind.message.text,
          sizeof(m->handing.text));

  ret = store_plan_status(&m->store, job->in.hand.message,
                          PNUT_MESSAGES_MESSAGE_STATUS_SENDING, "",
                          &m->work.write);
  if (ret < 0)
    {
      msg_hand_later(m, job->in.hand.message, ret);
      msg_next(m);
      return;
    }

  if (!msg_submit(m, MESSAGES_WORK_WRITE, msg_write_run, msg_written,
                  &m->work.write))
    {
      msg_hand_later(m, job->in.hand.message, -EAGAIN);
      msg_next(m);
    }
}

/****************************************************************************
 * Name: msg_written
 *
 * Description:
 *   A change on flash: in the index, answered, and announced.
 *
 ****************************************************************************/

static void msg_written(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct messages_s *m = msg_from_work(arg);
  FAR struct store_write_s *w = &m->work.write;
  FAR struct messages_job_s *job = &m->current;
  pnut_messages_delivered_t delivered;
  pnut_messages_reply_t reply;
  pnut_messages_sent_t sent;

  m->working   = false;
  m->work_kind = MESSAGES_WORK_NONE;
  if (m->stopped)
    {
      return;
    }

  if (w->ret < 0)
    {
      msg_store_failed(m, w->ret);
      if (job->kind == MESSAGES_HAND)
        {
          msg_hand_later(m, job->in.hand.message, w->ret);
        }
      else if (job->kind == MESSAGES_REQUEUE)
        {
          msg_requeue_later(m, job, w->ret);
        }

      msg_next(m);
      return;
    }

  m->fresh = false;
  if (store_commit(&m->store, w) < 0)
    {
      /* On flash, but not in the index: no more writes until the next
       * start reads them
       */

      pnut_log(&m->module, PNUT_LOG_ERROR,
               "No memory for the index: the store is read again");
      msg_broken(m);
    }

  /* A message written as sending goes to its transport now: one queued
   * for long, or a send taken at once
   */

  if (job->kind == MESSAGES_HAND)
    {
      msg_hand_over(m, job->transport);
    }
  else if (job->kind == MESSAGES_SEND && job->at_once)
    {
      memset(&m->handing, 0, sizeof(m->handing));
      m->handing.conversation = w->conversation;
      m->handing.message      = w->message;
      strlcpy(m->handing.peer, job->in.send.peer, sizeof(m->handing.peer));
      strlcpy(m->handing.text, job->in.send.text, sizeof(m->handing.text));
      msg_hand_over(m, job->in.send.transport);
    }
  else if (job->kind == MESSAGES_SEND || job->kind == MESSAGES_REQUEUE)
    {
      m->hand = true;             /* Queued: its transport may take it */
    }

  if (job->answer)
    {
      memset(&reply, 0, sizeof(reply));
      switch (job->kind)
        {
          case MESSAGES_REPORT:
            pnut_messages_report_reply(&m->server, &job->req,
                                       PNUT_STATUS_OK, &reply);
            break;

          case MESSAGES_SEND:
            memset(&sent, 0, sizeof(sent));
            sent.conversation = w->conversation;
            sent.message      = w->message;
            pnut_messages_send_reply(&m->server, &job->req,
                                     PNUT_STATUS_OK, &sent);
            break;

          case MESSAGES_DELIVER:
            memset(&delivered, 0, sizeof(delivered));
            delivered.conversation = w->conversation;
            delivered.message      = w->message;
            pnut_messages_deliver_reply(&m->server, &job->req,
                                        PNUT_STATUS_OK, &delivered);
            break;

          case MESSAGES_MARK_READ:
            pnut_messages_mark_read_reply(&m->server, &job->req,
                                          PNUT_STATUS_OK, &reply);
            break;

          default:
            pnut_messages_delete_reply(&m->server, &job->req,
                                       PNUT_STATUS_OK, &reply);
            break;
        }
    }

  msg_after(m, w->conversation, job->kind == MESSAGES_DELETE);
  msg_next(m);
}

/****************************************************************************
 * Name: msg_page_read
 *
 * Description:
 *   A page of messages read: answered with their statuses now.
 *
 ****************************************************************************/

static void msg_page_read(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct messages_s *m = msg_from_work(arg);
  FAR pnut_messages_page_t *page;

  m->working   = false;
  m->work_kind = MESSAGES_WORK_NONE;
  if (m->stopped)
    {
      return;
    }

  if (m->work.read.ret < 0)
    {
      msg_store_failed(m, m->work.read.ret);
      msg_next(m);
      return;
    }

  page = &m->answer.page;
  if (store_page(&m->store, &m->work.read, page) < 0)
    {
      msg_fail(m, &m->current.req, PNUT_STATUS_NOTFOUND,
               PNUT_MESSAGES_ERROR_CODE_NOT_FOUND, "no such conversation");
    }
  else
    {
      page->more = m->more;
      pnut_messages_read_reply(&m->server, &m->current.req, PNUT_STATUS_OK,
                               page);
    }

  msg_next(m);
}

/****************************************************************************
 * Name: msg_latest_read
 ****************************************************************************/

static void msg_latest_read(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct messages_s *m = msg_from_work(arg);

  m->working   = false;
  m->work_kind = MESSAGES_WORK_NONE;
  if (m->stopped)
    {
      return;
    }

  store_latest(&m->store, &m->work.read);
  msg_publish(m, m->work.read.conversation);
  msg_next(m);
}

/****************************************************************************
 * Name: msg_removed
 *
 * Description:
 *   A conversation's files removed: out of the index, answered, announced.
 *
 ****************************************************************************/

static void msg_removed(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct messages_s *m = msg_from_work(arg);
  FAR struct store_remove_s *rm = &m->work.remove;
  pnut_messages_reply_t reply;
  uint64_t conversation = rm->conversation;

  m->working = false;
  if (m->stopped)
    {
      msg_work_free(m);
      return;
    }

  store_removed(&m->store, rm);
  m->work_kind = MESSAGES_WORK_NONE;
  if (rm->ret < 0)
    {
      /* Some files went: the flash changed under the index, which is
       * read again from those left (the one that stayed first then, so
       * this ends).  None went: the conversation stays as it is, to be
       * deleted again.
       */

      msg_store_failed(m, rm->ret);
      if (rm->done > 0)
        {
          msg_broken(m);
        }

      msg_next(m);
      return;
    }

  m->fresh = false;

  if (m->current.answer)
    {
      memset(&reply, 0, sizeof(reply));
      pnut_messages_delete_reply(&m->server, &m->current.req,
                                 PNUT_STATUS_OK, &reply);
    }

  msg_after(m, conversation, false);
  msg_next(m);
}

/****************************************************************************
 * Name: msg_rewritten
 *
 * Description:
 *   A segment rewritten to make room: the index follows, a conversation
 *   left with no message goes, and room is made again while the store is
 *   over its limit.
 *
 ****************************************************************************/

static void msg_rewritten(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct messages_s *m = msg_from_work(arg);
  FAR struct store_rewrite_s *rw = &m->work.rewrite;
  FAR struct store_conv_s *conv;
  uint64_t conversation = rw->conversation;

  m->working = false;
  if (m->stopped)
    {
      msg_work_free(m);
      return;
    }

  /* The plan's memory given back either way */

  store_rewritten(&m->store, rw);
  m->work_kind = MESSAGES_WORK_NONE;
  if (rw->ret < 0)
    {
      pnut_log(&m->module, PNUT_LOG_ERROR, "Cannot rewrite a segment: %d",
               rw->ret);
      if (rw->ret == -ESTALE && !m->fresh)
        {
          msg_broken(m);            /* The index and the file disagree */
        }
      else if (m->store.bytes > m->store.limit && !m->full)
        {
          m->full = true;           /* Sends refused: the topic says so */
          msg_publish(m, 0);
        }

      msg_next(m);
      return;
    }

  m->fresh = false;
  conv = store_find(&m->store, conversation);
  if (conv != NULL && conv->nmsgs == 0)
    {
      msg_own(m, MESSAGES_EMPTY, conversation);
    }

  msg_after(m, conversation, false);
  msg_next(m);
}

/****************************************************************************
 * Name: msg_submit
 ****************************************************************************/

static bool msg_submit(FAR struct messages_s *m, uint8_t kind,
                       pnut_job_run_t run, pnut_job_done_t done,
                       FAR void *work)
{
  int ret;

  m->work_kind = kind;
  ret = pnut_job_submit(m->module.loop, run, done, work);
  if (ret < 0)
    {
      msg_work_free(m);
      msg_store_failed(m, ret);
      return false;
    }

  m->working = true;
  return true;
}

/****************************************************************************
 * Name: msg_plan_failed
 *
 * Description:
 *   A job its plan refused: its caller told why.
 *
 ****************************************************************************/

static void msg_plan_failed(FAR struct messages_s *m, int ret)
{
  FAR struct messages_job_s *job = &m->current;

  if (!job->answer)
    {
      return;
    }

  switch (ret)
    {
      case -ENOENT:
        msg_fail(m, &job->req, PNUT_STATUS_NOTFOUND,
                 PNUT_MESSAGES_ERROR_CODE_NOT_FOUND,
                 "no such conversation or message");
        break;

      case -ENOSPC:
        msg_fail(m, &job->req, PNUT_STATUS_UNAVAILABLE,
                 PNUT_MESSAGES_ERROR_CODE_FULL, "the store is full");
        break;

      case -EINVAL:
        msg_fail(m, &job->req, PNUT_STATUS_INVALID,
                 PNUT_MESSAGES_ERROR_CODE_INVALID, "does not encode");
        break;

      default:
        msg_fail(m, &job->req, PNUT_STATUS_INTERNAL,
                 PNUT_MESSAGES_ERROR_CODE_STORE, "the store failed");
        break;
    }
}

/****************************************************************************
 * Name: msg_start_job
 *
 * Description:
 *   The current job planned, and handed to a worker; or answered at once
 *   when it needs no worker.
 *
 * Returned Value:
 *   true when a worker has it.
 *
 ****************************************************************************/

static bool msg_start_job(FAR struct messages_s *m)
{
  FAR struct messages_job_s *job = &m->current;
  FAR pnut_messages_message_record_t *rec;
  pnut_messages_reply_t reply;
  int ret;

  memset(&reply, 0, sizeof(reply));
  switch (job->kind)
    {
      case MESSAGES_LIST:
        {
          FAR pnut_messages_conversations_t *out =
            &m->answer.conversations;

          if (store_conversations(&m->store, job->in.list.after, out) < 0)
            {
              ret = -ENOENT;
              break;
            }

          pnut_messages_conversations_reply(&m->server, &job->req,
                                            PNUT_STATUS_OK, out);
          return false;
        }

      case MESSAGES_READ:
        ret = store_plan_page(&m->store, job->in.read.conversation,
                              job->in.read.after, &m->work.read, &m->more);
        if (ret < 0)
          {
            break;
          }

        if (m->work.read.n == 0)
          {
            FAR pnut_messages_page_t *page =
              &m->answer.page;

            memset(page, 0, sizeof(*page));
            pnut_messages_read_reply(&m->server, &job->req,
                                     PNUT_STATUS_OK, page);
            return false;
          }

        return msg_submit(m, MESSAGES_WORK_READ, msg_read_run,
                          msg_page_read, &m->work.read);

      case MESSAGES_MARK_READ:
        ret = store_plan_read(&m->store, job->in.mark_read.conversation,
                              job->in.mark_read.upto, &m->work.write);
        if (ret == -EALREADY)
          {
            pnut_messages_mark_read_reply(&m->server, &job->req,
                                          PNUT_STATUS_OK, &reply);
            return false;
          }

        if (ret < 0)
          {
            break;
          }

        return msg_submit(m, MESSAGES_WORK_WRITE, msg_write_run,
                          msg_written, &m->work.write);

      case MESSAGES_DELETE:
        if (job->in.delete.message == 0)
          {
            ret = store_plan_remove(&m->store, job->in.delete.conversation,
                                    &m->work.remove);
            if (ret < 0)
              {
                break;
              }

            return msg_submit(m, MESSAGES_WORK_REMOVE, msg_remove_run,
                              msg_removed, &m->work.remove);
          }

        ret = store_plan_delete(&m->store, job->in.delete.conversation,
                                job->in.delete.message, &m->work.write);
        if (ret < 0)
          {
            break;
          }

        return msg_submit(m, MESSAGES_WORK_WRITE, msg_write_run,
                          msg_written, &m->work.write);

      case MESSAGES_SEND:

        /* The record is large: on the heap, not the loop's stack */

        rec = calloc(1, sizeof(*rec));
        if (rec == NULL)
          {
            ret = -ENOMEM;
            break;
          }

        rec->out    = true;
        rec->status = PNUT_MESSAGES_MESSAGE_STATUS_QUEUED;
        rec->stored = (int64_t)time(NULL);
        strlcpy(rec->text, job->in.send.text, sizeof(rec->text));

        /* Its transport takes it at once, nothing older waiting: stored
         * as sending, in the send's one write (RFC 0029)
         */

        {
          FAR struct messages_transport_s *t;
          uint64_t c;
          uint64_t id;

          /* Not while another waits for room to go: handing holds it */

          t = msg_taker(m, job->in.send.transport);
          if (t != NULL && t->stream.credit > 0 && !m->pushing &&
              store_oldest_queued(&m->store, t->name, &c, &id) == -ENOENT)
            {
              rec->status  = PNUT_MESSAGES_MESSAGE_STATUS_SENDING;
              job->at_once = true;
            }
        }

        ret = store_plan_message(&m->store, job->in.send.transport,
                                 job->in.send.peer, rec, &m->work.write);
        free(rec);
        if (ret == -ENOSPC)
          {
            m->room = true;         /* Perhaps there is room by now */
          }

        if (ret < 0)
          {
            break;
          }

        return msg_submit(m, MESSAGES_WORK_WRITE, msg_write_run,
                          msg_written, &m->work.write);

      case MESSAGES_DELIVER:
        rec = calloc(1, sizeof(*rec));
        if (rec == NULL)
          {
            ret = -ENOMEM;
            break;
          }

        rec->status = PNUT_MESSAGES_MESSAGE_STATUS_UNREAD;
        rec->stored = (int64_t)time(NULL);
        rec->sent   = job->in.deliver.sent;
        strlcpy(rec->text, job->in.deliver.text, sizeof(rec->text));
        strlcpy(rec->sender, job->in.deliver.sender, sizeof(rec->sender));
        strlcpy(rec->key, job->in.deliver.key, sizeof(rec->key));
        ret = store_plan_message(&m->store, job->transport,
                                 job->in.deliver.peer, rec, &m->work.write);
        free(rec);
        if (ret == -EEXIST)
          {
            /* Stored already, as before a reset: the transport may let
             * go of it
             */

            pnut_messages_delivered_t delivered;

            memset(&delivered, 0, sizeof(delivered));
            delivered.conversation = m->work.write.conversation;
            delivered.message      = m->work.write.message;
            delivered.again        = true;
            pnut_messages_deliver_reply(&m->server, &job->req,
                                        PNUT_STATUS_OK, &delivered);
            return false;
          }

        if (ret == -ENOSPC)
          {
            /* The transport keeps it: the log is all the owner has until
             * Notifications (RFC 0029)
             */

            pnut_log(&m->module, PNUT_LOG_WARNING,
                     "A message from %s refused: the store is full",
                     job->transport);
            m->room = true;
          }

        if (ret < 0)
          {
            break;
          }

        return msg_submit(m, MESSAGES_WORK_WRITE, msg_write_run,
                          msg_written, &m->work.write);

      case MESSAGES_HAND:
        {
          FAR struct messages_transport_s *t = NULL;
          uint64_t c = 0;
          uint64_t id = 0;
          int i;

          /* The first transport that takes, with credit and something
           * queued
           */

          for (i = 0; i < MESSAGES_TRANSPORTS && t == NULL; i++)
            {
              t = &m->transports[i];
              if (!t->used || !t->stream.open || t->stream.credit == 0 ||
                  store_oldest_queued(&m->store, t->name, &c, &id) < 0)
                {
                  t = NULL;
                }
            }

          if (t == NULL)
            {
              return false;
            }

          strlcpy(job->transport, t->name, sizeof(job->transport));
          job->in.hand.conversation = c;
          job->in.hand.message      = id;
          ret = store_plan_one(&m->store, c, id, &m->work.read);
          if (ret < 0)
            {
              msg_hand_later(m, id, ret);
              return false;
            }

          if (!msg_submit(m, MESSAGES_WORK_READ, msg_read_run,
                          msg_hand_read, &m->work.read))
            {
              msg_hand_later(m, id, -EAGAIN);
              return false;
            }

          return true;
        }

      case MESSAGES_REQUEUE:
        ret = store_plan_status(&m->store, job->in.hand.message,
                                job->in.hand.status,
                                job->in.hand.status ==
                                PNUT_MESSAGES_MESSAGE_STATUS_FAILED ?
                                "the message cannot be read" : "",
                                &m->work.write);

        /* Gone meanwhile, or right already: nothing to put right */

        if (ret == -ENOENT || ret == -EALREADY)
          {
            return false;
          }

        if (ret < 0)
          {
            msg_requeue_later(m, job, ret);
            return false;
          }

        if (!msg_submit(m, MESSAGES_WORK_WRITE, msg_write_run, msg_written,
                        &m->work.write))
          {
            msg_requeue_later(m, job, -EAGAIN);
            return false;
          }

        return true;

      case MESSAGES_REPORT:
        {
          FAR struct store_conv_s *conv;
          FAR struct store_msg_s *msg;

          /* Its transport's own message, going out */

          conv = store_find(&m->store, job->in.report.conversation);
          msg  = conv != NULL ? store_msg(conv, job->in.report.message) :
                                NULL;
          if (msg == NULL || (msg->flags & STORE_DELETED) ||
              strcmp(conv->transport, job->transport) != 0 ||
              msg->status < PNUT_MESSAGES_MESSAGE_STATUS_QUEUED)
            {
              ret = -ENOENT;
              break;
            }

          ret = store_plan_status(&m->store, job->in.report.message,
                                  job->in.report.status,
                                  job->in.report.reason, &m->work.write);
          if (ret == -EALREADY)
            {
              pnut_messages_report_reply(&m->server, &job->req,
                                         PNUT_STATUS_OK, &reply);
              return false;
            }

          if (ret < 0)
            {
              break;
            }

          return msg_submit(m, MESSAGES_WORK_WRITE, msg_write_run,
                            msg_written, &m->work.write);
        }

      case MESSAGES_ROOM:
        ret = store_plan_room(&m->store, &m->work.rewrite);
        if (ret == -ENOSPC && !m->full)
          {
            pnut_log(&m->module, PNUT_LOG_WARNING,
                     "The store is full: nothing may go to make room");
            m->full = true;
            msg_publish(m, 0);
          }
        else if (ret == -EALREADY && m->full)
          {
            m->full = false;
            msg_publish(m, 0);
          }

        if (ret < 0)
          {
            return false;
          }

        return msg_submit(m, MESSAGES_WORK_REWRITE, msg_rewrite_run,
                          msg_rewritten, &m->work.rewrite);

      case MESSAGES_COMPACT:
        ret = store_plan_compact(&m->store, &m->work.rewrite);
        if (ret < 0)
          {
            return false;
          }

        return msg_submit(m, MESSAGES_WORK_REWRITE, msg_rewrite_run,
                          msg_rewritten, &m->work.rewrite);

      case MESSAGES_LATEST:
        ret = store_plan_latest(&m->store, job->in.conversation,
                                &m->work.read);
        if (ret < 0)
          {
            return false;
          }

        return msg_submit(m, MESSAGES_WORK_READ, msg_read_run,
                          msg_latest_read, &m->work.read);

      case MESSAGES_EMPTY:
        ret = store_plan_remove(&m->store, job->in.conversation,
                                &m->work.remove);
        if (ret < 0)
          {
            return false;
          }

        return msg_submit(m, MESSAGES_WORK_REMOVE, msg_remove_run,
                          msg_removed, &m->work.remove);

      default:
        return false;
    }

  msg_plan_failed(m, ret);
  return false;
}

/****************************************************************************
 * Name: msg_next
 *
 * Description:
 *   The next job waiting, once the store is read and no worker has one.
 *
 ****************************************************************************/

static void msg_next(FAR struct messages_s *m)
{
  while (!m->working && m->loaded && !m->stopped &&
         (m->room || m->compact || m->requeue ||
          (m->hand && !m->pushing) || m->count > 0))
    {
      /* Room first, then a compaction, then the queue */

      memset(&m->current, 0, sizeof(m->current));
      if (m->count > 0 && !m->queue[m->head].answer)
        {
          /* Messages' own first: removing an emptied conversation may
           * make the room room-making would
           */

          m->current = m->queue[m->head];
          m->head = (uint8_t)((m->head + 1) % MESSAGES_QUEUE);
          m->count--;
        }
      else if (m->room && !m->failed)
        {
          m->room = false;
          m->current.kind = MESSAGES_ROOM;
        }
      else if (m->compact && !m->failed)
        {
          m->compact = false;
          m->current.kind = MESSAGES_COMPACT;
        }
      else if (m->requeue && !m->failed)
        {
          m->requeue = false;
          m->current.kind           = MESSAGES_REQUEUE;
          m->current.in.hand.message = m->requeue_msg;
          m->current.in.hand.status  = m->requeue_status;
        }
      else if (m->hand && !m->failed && !m->pushing && !m->requeue_held)
        {
          m->hand = false;
          m->current.kind = MESSAGES_HAND;
        }
      else if (m->count > 0)
        {
          m->current = m->queue[m->head];
          m->head = (uint8_t)((m->head + 1) % MESSAGES_QUEUE);
          m->count--;
        }
      else
        {
          m->room    = false;
          m->compact = false;
          m->hand    = m->hand && m->pushing;
          break;
        }

      if (m->failed)
        {
          if (m->current.answer)
            {
              msg_fail(m, &m->current.req, PNUT_STATUS_INTERNAL,
                       PNUT_MESSAGES_ERROR_CODE_STORE,
                       "the store cannot be read");
            }

          continue;
        }

      msg_start_job(m);
    }
}

/****************************************************************************
 * Name: msg_register, msg_transport
 *
 * Description:
 *   A transport registered on the caller's connection, in place of one
 *   of its name before (its program started again); the transport a
 *   connection carries, or NULL.
 *
 ****************************************************************************/

static void msg_register(FAR struct messages_s *m,
                         FAR const struct pnut_request_s *req,
                         FAR const char *name)
{
  FAR struct messages_transport_s *t = NULL;
  pnut_messages_reply_t reply;
  int i;

  for (i = 0; i < MESSAGES_TRANSPORTS; i++)
    {
      if (m->transports[i].used &&
          strcmp(m->transports[i].name, name) == 0)
        {
          t = &m->transports[i];
          break;
        }

      if (!m->transports[i].used && t == NULL)
        {
          t = &m->transports[i];
        }
    }

  if (t == NULL)
    {
      msg_fail(m, req, PNUT_STATUS_BUSY, PNUT_MESSAGES_ERROR_CODE_NONE,
               "too many transports");
      return;
    }

  /* Its program started again: a Take on the old connection is over */

  if (t->used && t->stream.open &&
      (t->stream.to.conn != req->from.conn ||
       t->stream.to.gen != req->from.gen))
    {
      pnut_stream_end(&t->stream, PNUT_STATUS_ENDED);
    }

  t->used     = true;
  t->from     = req->from;
  t->messages = m;
  strlcpy(t->name, name, sizeof(t->name));
  pnut_log(&m->module, PNUT_LOG_INFO, "Transport %s registered", name);

  memset(&reply, 0, sizeof(reply));
  pnut_messages_register_transport_reply(&m->server, req, PNUT_STATUS_OK,
                                         &reply);
}

static FAR struct messages_transport_s *
msg_transport(FAR struct messages_s *m, FAR const struct pnut_request_s *req)
{
  int i;

  for (i = 0; i < MESSAGES_TRANSPORTS; i++)
    {
      if (m->transports[i].used &&
          m->transports[i].from.conn == req->from.conn &&
          m->transports[i].from.gen == req->from.gen)
        {
          return &m->transports[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: msg_allowed
 *
 * Description:
 *   A caller allowed, now that Service states said who it is: here, the
 *   system UI and the developer's shell (RFC 0029), and the telephony
 *   program registering sms; apps come with the runtime.  A registration
 *   is done at once; another request waits for the store's files to be
 *   read, and for the jobs before it.
 *
 ****************************************************************************/

static void msg_allowed(FAR struct messages_s *m,
                        FAR const struct pnut_request_s *req, uint8_t kind,
                        FAR const void *in, size_t size)
{
  FAR pnut_messages_conversations_t *out;
  FAR struct messages_job_s *job;

  if (kind == MESSAGES_REGISTER)
    {
      msg_register(m, req, ((FAR const pnut_messages_register_request_t *)
                            in)->transport);
      return;
    }

  /* The conversations come from memory: at once, once it holds them */

  if (kind == MESSAGES_LIST && m->loaded && !m->failed)
    {
      out = &m->answer.conversations;
      if (store_conversations(&m->store,
                              ((FAR const pnut_messages_page_request_t *)
                               in)->after, out) < 0)
        {
          msg_fail(m, req, PNUT_STATUS_NOTFOUND,
                   PNUT_MESSAGES_ERROR_CODE_NOT_FOUND,
                   "no such conversation");
          return;
        }

      pnut_messages_conversations_reply(&m->server, req, PNUT_STATUS_OK,
                                        out);
      return;
    }

  job = msg_slot(m, false);
  if (job == NULL)
    {
      msg_fail(m, req, PNUT_STATUS_BUSY, PNUT_MESSAGES_ERROR_CODE_NONE,
               "too many requests wait");
      return;
    }

  job->kind   = kind;
  job->answer = true;
  job->req    = *req;
  memcpy(&job->in, in, size);
  msg_next(m);
}

/****************************************************************************
 * Name: msg_who
 ****************************************************************************/

static void msg_who(FAR struct pnut_services_client_s *client, int status,
                    FAR const pnut_services_who_reply_t *out,
                    FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct messages_ask_s *ask = arg;
  FAR struct messages_s *m = ask->messages;

  ask->used = false;
  if (m->stopped)
    {
      return;
    }

  /* A transport is its program's; the rest the system UI's and the
   * developer's shell's (RFC 0029)
   */

  if (status != PNUT_STATUS_OK || out == NULL ||
      (ask->job.kind == MESSAGES_REGISTER ?
       strcmp(out->name, MESSAGES_SMS_OWNER) != 0 :
       strcmp(out->name, MESSAGES_UI) != 0 && !out->shell))
    {
      bool unknown = status == PNUT_STATUS_UNAVAILABLE ||
                     status == PNUT_STATUS_TIMEOUT;

      msg_fail(m, &ask->job.req, unknown ? PNUT_STATUS_UNAVAILABLE :
               PNUT_STATUS_DENIED, PNUT_MESSAGES_ERROR_CODE_DENIED,
               unknown ? "who calls cannot be told now" :
               ask->job.kind == MESSAGES_REGISTER ?
               "sms is the telephony program's" :
               "for the system UI and the developer's shell");
    }
  else
    {
      msg_allowed(m, &ask->job.req, ask->job.kind, &ask->job.in,
                  sizeof(ask->job.in));
    }
}

/****************************************************************************
 * Name: msg_ask
 *
 * Description:
 *   Ask Service states who sent a request, keeping it meanwhile.  Without
 *   Service states configured (tests), every caller is let in.
 *
 ****************************************************************************/

static void msg_ask(FAR struct messages_s *m,
                    FAR const struct pnut_request_s *req, uint8_t kind,
                    FAR const void *in, size_t size)
{
  FAR struct messages_ask_s *ask = NULL;
  pnut_services_who_request_t who;
  int i;
  int ret;

  if (m->config.services == NULL)
    {
      msg_allowed(m, req, kind, in, size);
      return;
    }

  for (i = 0; i < MESSAGES_ASKS; i++)
    {
      if (!m->asks[i].used)
        {
          ask = &m->asks[i];
          break;
        }
    }

  if (ask == NULL)
    {
      msg_fail(m, req, PNUT_STATUS_BUSY, PNUT_MESSAGES_ERROR_CODE_NONE,
               "too many requests wait");
      return;
    }

  memset(&ask->job, 0, sizeof(ask->job));
  ask->messages = m;
  ask->job.kind = kind;
  ask->job.req  = *req;
  memcpy(&ask->job.in, in, size);

  memset(&who, 0, sizeof(who));
  who.pid = req->pid;
  ret = m->services_open ?
        pnut_services_who(&m->services, &who, 0, msg_who, ask) : -ENOTCONN;
  if (ret < 0)
    {
      msg_fail(m, req, PNUT_STATUS_UNAVAILABLE,
               PNUT_MESSAGES_ERROR_CODE_DENIED,
               "who calls cannot be told now");
      return;
    }

  ask->used = true;
}

/****************************************************************************
 * Name: msg_conversations, msg_read, msg_mark_read, msg_delete, msg_send
 ****************************************************************************/

static void msg_conversations(FAR struct pnut_messages_server_s *server,
                              FAR const struct pnut_request_s *req,
                              FAR const pnut_messages_page_request_t *in,
                              FAR void *arg)
{
  msg_ask(arg, req, MESSAGES_LIST, in, sizeof(*in));
}

static void msg_read(FAR struct pnut_messages_server_s *server,
                     FAR const struct pnut_request_s *req,
                     FAR const pnut_messages_read_request_t *in,
                     FAR void *arg)
{
  msg_ask(arg, req, MESSAGES_READ, in, sizeof(*in));
}

static void msg_mark_read(FAR struct pnut_messages_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_messages_mark_read_request_t *in,
                          FAR void *arg)
{
  msg_ask(arg, req, MESSAGES_MARK_READ, in, sizeof(*in));
}

static void msg_delete(FAR struct pnut_messages_server_s *server,
                       FAR const struct pnut_request_s *req,
                       FAR const pnut_messages_delete_request_t *in,
                       FAR void *arg)
{
  msg_ask(arg, req, MESSAGES_DELETE, in, sizeof(*in));
}

static void msg_send(FAR struct pnut_messages_server_s *server,
                     FAR const struct pnut_request_s *req,
                     FAR const pnut_messages_send_request_t *in,
                     FAR void *arg)
{
  FAR struct messages_s *m = arg;

  if (in->transport[0] == '\0' || in->peer[0] == '\0' ||
      in->text[0] == '\0')
    {
      msg_fail(m, req, PNUT_STATUS_INVALID,
               PNUT_MESSAGES_ERROR_CODE_INVALID,
               "a transport, a peer and a text");
      return;
    }

  if (strlen(in->text) > STORE_TEXT_MAX)
    {
      msg_fail(m, req, PNUT_STATUS_INVALID,
               PNUT_MESSAGES_ERROR_CODE_TOO_LONG, "the text is too long");
      return;
    }

  msg_ask(m, req, MESSAGES_SEND, in, sizeof(*in));
}

static void msg_register_transport(FAR struct pnut_messages_server_s *server,
                                   FAR const struct pnut_request_s *req,
                                   FAR const pnut_messages_register_request_t
                                   *in, FAR void *arg)
{
  FAR struct messages_s *m = arg;

  if (in->transport[0] == '\0')
    {
      msg_fail(m, req, PNUT_STATUS_INVALID,
               PNUT_MESSAGES_ERROR_CODE_INVALID, "a transport");
      return;
    }

  /* sms alone: apps' transports come with the runtime and Packages (RFC
   * 0029).  Without Service states, as in the tests, any name: a test
   * transport.
   */

  if (m->config.services != NULL && strcmp(in->transport, MESSAGES_SMS) != 0)
    {
      msg_fail(m, req, PNUT_STATUS_DENIED, PNUT_MESSAGES_ERROR_CODE_DENIED,
               "no such transport");
      return;
    }

  msg_ask(m, req, MESSAGES_REGISTER, in, sizeof(*in));
}

static void msg_deliver(FAR struct pnut_messages_server_s *server,
                        FAR const struct pnut_request_s *req,
                        FAR const pnut_messages_deliver_request_t *in,
                        FAR void *arg)
{
  FAR struct messages_s *m = arg;
  FAR struct messages_transport_s *t;
  FAR struct messages_job_s *job;

  /* The connection the transport registered on vouches for it */

  t = msg_transport(m, req);
  if (t == NULL)
    {
      msg_fail(m, req, PNUT_STATUS_DENIED,
               PNUT_MESSAGES_ERROR_CODE_UNREGISTERED,
               "no transport on this connection");
      return;
    }

  /* Without a key, a delivery repeated after a reset would be stored
   * twice
   */

  if (in->peer[0] == '\0' || in->key[0] == '\0')
    {
      msg_fail(m, req, PNUT_STATUS_INVALID,
               PNUT_MESSAGES_ERROR_CODE_INVALID, "a peer and a key");
      return;
    }

  job = msg_slot(m, false);
  if (job == NULL)
    {
      msg_fail(m, req, PNUT_STATUS_BUSY, PNUT_MESSAGES_ERROR_CODE_NONE,
               "too many requests wait");
      return;
    }

  job->kind       = MESSAGES_DELIVER;
  job->answer     = true;
  job->req        = *req;
  job->in.deliver = *in;
  strlcpy(job->transport, t->name, sizeof(job->transport));
  msg_next(m);
}

static void msg_take_ready(FAR struct pnut_stream_s *stream, FAR void *arg)
{
  FAR struct messages_transport_s *t = arg;

  t->messages->hand = true;
  msg_next(t->messages);
}

static void msg_take_closed(FAR struct pnut_stream_s *stream, int status,
                            FAR void *arg)
{
  FAR struct messages_transport_s *t = arg;

  /* What it had taken stays sending: not known to be sent at the next
   * start
   */

  pnut_log(&t->messages->module, PNUT_LOG_INFO, "%s takes no more: %d",
           t->name, status);
}

static void msg_take(FAR struct pnut_messages_server_s *server,
                     FAR const struct pnut_request_s *req,
                     FAR const pnut_messages_take_request_t *in,
                     FAR void *arg)
{
  FAR struct messages_s *m = arg;
  FAR struct messages_transport_s *t;
  int ret;

  t = msg_transport(m, req);
  if (t == NULL)
    {
      msg_fail(m, req, PNUT_STATUS_DENIED,
               PNUT_MESSAGES_ERROR_CODE_UNREGISTERED,
               "no transport on this connection");
      return;
    }

  /* A newer Take replaces it */

  pnut_stream_end(&t->stream, PNUT_STATUS_ENDED);
  ret = pnut_messages_take_accept(server, req, &t->stream, msg_take_ready,
                                  msg_take_closed, t);
  if (ret < 0)
    {
      pnut_log(&m->module, PNUT_LOG_WARNING, "%s's Take not opened: %d",
               t->name, ret);
      return;
    }

  m->hand = true;
  msg_next(m);
}

static void msg_report(FAR struct pnut_messages_server_s *server,
                       FAR const struct pnut_request_s *req,
                       FAR const pnut_messages_report_request_t *in,
                       FAR void *arg)
{
  FAR struct messages_s *m = arg;
  FAR struct messages_transport_s *t;
  FAR struct messages_job_s *job;

  t = msg_transport(m, req);
  if (t == NULL)
    {
      msg_fail(m, req, PNUT_STATUS_DENIED,
               PNUT_MESSAGES_ERROR_CODE_UNREGISTERED,
               "no transport on this connection");
      return;
    }

  if (in->status != PNUT_MESSAGES_MESSAGE_STATUS_SENT &&
      in->status != PNUT_MESSAGES_MESSAGE_STATUS_DELIVERED &&
      in->status != PNUT_MESSAGES_MESSAGE_STATUS_FAILED)
    {
      msg_fail(m, req, PNUT_STATUS_INVALID,
               PNUT_MESSAGES_ERROR_CODE_INVALID,
               "sent, delivered or failed");
      return;
    }

  job = msg_slot(m, false);
  if (job == NULL)
    {
      msg_fail(m, req, PNUT_STATUS_BUSY, PNUT_MESSAGES_ERROR_CODE_NONE,
               "too many requests wait");
      return;
    }

  job->kind      = MESSAGES_REPORT;
  job->answer    = true;
  job->req       = *req;
  job->in.report = *in;
  strlcpy(job->transport, t->name, sizeof(job->transport));
  msg_next(m);
}

static const struct pnut_messages_handlers_s g_msg_handlers =
{
  .conversations      = msg_conversations,
  .read               = msg_read,
  .mark_read          = msg_mark_read,
  .delete             = msg_delete,
  .send               = msg_send,
  .register_transport = msg_register_transport,
  .deliver            = msg_deliver,
  .take               = msg_take,
  .report             = msg_report,
};

/****************************************************************************
 * Name: msg_cleanup
 *
 * Description:
 *   What the module started given back: the service, Service states, the
 *   topic, the retry's timer.
 *
 ****************************************************************************/

static void msg_cleanup(FAR struct messages_s *m)
{
  if (m->retry != NULL)
    {
      pnut_timer_cancel(m->module.loop, m->retry);
      m->retry = NULL;
    }

  if (m->push != NULL)
    {
      pnut_timer_cancel(m->module.loop, m->push);
      m->push    = NULL;
      m->pushing = false;
    }

  if (m->hand_later != NULL)
    {
      pnut_timer_cancel(m->module.loop, m->hand_later);
      m->hand_later = NULL;
    }

  if (m->services_open)
    {
      pnut_services_disconnect(&m->services);
      m->services_open = false;
    }

  if (m->serving)
    {
      pnut_messages_close(&m->server);
      m->serving = false;
    }

  pnut_topic_unadvertise(m->changes);
  m->changes = NULL;
}

/****************************************************************************
 * Name: msg_start
 ****************************************************************************/

static int msg_start(FAR struct pnut_module_s *module,
                     FAR struct pnut_loop_s *loop)
{
  FAR struct messages_s *m = (FAR struct messages_s *)module;
  int ret;

  store_init(&m->store, m->config.dir, m->config.limit, m->config.segmax);

  ret = pnut_messages_change_advertise(loop, &m->changes);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No messages topic: %d", ret);
      m->changes = NULL;
    }

  ret = pnut_messages_serve(loop, MESSAGES_SERVICE, 0, &g_msg_handlers, m,
                            &m->server);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Cannot serve: %d", ret);
      msg_cleanup(m);
      return ret;
    }

  m->serving = true;
  if (m->config.services != NULL)
    {
      ret = pnut_services_connect(loop, m->config.services, NULL, NULL,
                                  &m->services);
      if (ret < 0)
        {
          pnut_log(module, PNUT_LOG_WARNING,
                   "No Service states: %d; callers refused", ret);
        }
      else
        {
          m->services_open = true;
        }
    }

  /* The store's files, read on a worker; the module is ready once they
   * are, and requests wait meanwhile
   */

  ret = msg_load(m);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Cannot read the store: %d", ret);
      msg_cleanup(m);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: msg_stop
 ****************************************************************************/

static void msg_stop(FAR struct pnut_module_s *module,
                     FAR struct pnut_loop_s *loop)
{
  FAR struct messages_s *m = (FAR struct messages_s *)module;

  m->stopped = true;
  msg_cleanup(m);

  /* A job a worker has keeps its part of the work until the loop is
   * destroyed, which waits for it: messages_deinit() gives it back
   */

  store_free(&m->store);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void messages_init(FAR struct messages_s *messages,
                   FAR const struct messages_config_s *config)
{
  memset(messages, 0, sizeof(*messages));
  messages->config       = *config;
  messages->module.name  = "messages";
  messages->module.start = msg_start;
  messages->module.stop  = msg_stop;
}

void messages_deinit(FAR struct messages_s *messages)
{
  msg_work_free(messages);
  store_free(&messages->store);
}
