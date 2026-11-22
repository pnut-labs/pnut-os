/****************************************************************************
 * pnut-os/src/ui/host.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The system UI's service for apps (RFC 0037): the system UI starts an app
 * through Service states, the app opens this service, is named by Service
 * states' Who from its connection's task, and may show the templates its
 * manifest lists.  Up to HOST_APPS apps at once: the open one, one kept
 * warm, one being opened.  An app the system UI did not start (after the
 * system UI's own restart, or a second task of one running) has its Open
 * answered with Stop; a program typed in a developer's shell opens as it
 * comes, under an id no app has, and is never given a place or counted
 * crashes.  An app's slot is kept until its task has exited, as Service
 * states' services topic tells, or Stop's deadline passes; the app is
 * started again only once its old task is gone, past the deadline too:
 * the topic, or Service states' Get, says its service stopped.  A tile
 * pressed meanwhile, or with no slot free, waits with the UI's Busy up.
 * Events go down Open's stream as the app's credit allows; an app that
 * leaves HOST_EVENTS of them waiting is taken off.  An app's text is never
 * logged.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <pnut/log.h>
#include <pnut/msg.h>

#include "app.h"
#include "host.h"
#include "ui.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void host_flush(FAR struct host_app_s *app);
static void host_next_due(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg);
static void host_stop_due(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg);
static void host_owe(FAR struct host_app_s *app);
static void host_unowe(FAR struct host_app_s *app);
static void host_got(FAR struct pnut_services_client_s *client, int status,
                     FAR const pnut_service_state_t *out,
                     FAR const pnut_error_t *err, FAR void *arg);
static void host_resume_forget(FAR struct host_s *h, FAR const char *id);

/* The app whose connection a request comes from, or NULL */

static FAR struct host_app_s *host_from(FAR struct host_s *h,
                                        FAR const struct pnut_request_s *req)
{
  int i;

  for (i = 0; i < HOST_APPS; i++)
    {
      FAR struct host_app_s *app = &h->slots[i];

      if (app->connected && app->stream.open &&
          req->from.conn == app->stream.to.conn &&
          req->from.gen == app->stream.to.gen)
        {
          return app;
        }
    }

  return NULL;
}

/* The app of an id, or NULL */

static FAR struct host_app_s *host_find(FAR struct host_s *h,
                                        FAR const char *id)
{
  int i;

  for (i = 0; i < HOST_APPS; i++)
    {
      if (h->slots[i].state != HOST_FREE &&
          strcmp(h->slots[i].id, id) == 0)
        {
          return &h->slots[i];
        }
    }

  return NULL;
}

/* The slot of an id in a state, not connected, or NULL */

static FAR struct host_app_s *host_unconnected(FAR struct host_s *h,
                                               FAR const char *id,
                                               uint8_t state)
{
  int i;

  for (i = 0; i < HOST_APPS; i++)
    {
      if (h->slots[i].state == state && !h->slots[i].connected &&
          strcmp(h->slots[i].id, id) == 0)
        {
          return &h->slots[i];
        }
    }

  return NULL;
}

static FAR struct host_app_s *host_free_slot(FAR struct host_s *h)
{
  int i;

  for (i = 0; i < HOST_APPS; i++)
    {
      if (h->slots[i].state == HOST_FREE)
        {
          return &h->slots[i];
        }
    }

  return NULL;
}

/* An app whose old task may still exit, or NULL */

static FAR struct host_exiting_s *host_exiting(FAR struct host_s *h,
                                               FAR const char *id)
{
  int i;

  for (i = 0; i < HOST_EXITING; i++)
    {
      if (h->exiting[i].id[0] != '\0' &&
          strcmp(h->exiting[i].id, id) == 0)
        {
          return &h->exiting[i];
        }
    }

  return NULL;
}

/* Mark an app whose old task may still exit, in place of the one marked
 * longest ago when the table is full (that app starts at once again)
 */

static void host_exiting_mark(FAR struct host_s *h, FAR const char *id)
{
  FAR struct host_exiting_s *e = host_exiting(h, id);
  int i;

  for (i = 0; e == NULL && i < HOST_EXITING; i++)
    {
      if (h->exiting[i].id[0] == '\0')
        {
          e = &h->exiting[i];
        }
    }

  if (e == NULL)
    {
      e = &h->exiting[0];
      for (i = 1; i < HOST_EXITING; i++)
        {
          if (h->exiting[i].order < e->order)
            {
              e = &h->exiting[i];
            }
        }

      pnut_log(&h->ui->module, PNUT_LOG_INFO,
               "app %s not waited for: no room", e->id);
    }

  strlcpy(e->id, id, sizeof(e->id));
  e->order = ++h->exiting_orders;

  /* Marked anew for the app waiting: Service states is asked again */

  if (strcmp(h->next.id, id) == 0)
    {
      h->getting = false;
    }
}

/* An app waiting to start tries again, from the loop: a slot is free, or
 * a task gone
 */

static void host_kick(FAR struct host_s *h)
{
  if (h->next.state != HOST_FREE && h->kick == NULL &&
      pnut_timer_start(h->loop, 0, 0, host_next_due, h, &h->kick) < 0)
    {
      h->kick = NULL;             /* The next slot freed tries again */
    }
}

/* The app is gone: its screen off if it was open, its slot free */

static void host_forget(FAR struct host_app_s *app)
{
  FAR struct host_s *h = app->host;
  FAR pnut_ui_event_t *queue = app->queue;
  bool open = app->state == HOST_OPEN;

  if (app->busy != NULL)
    {
      pnut_timer_cancel(h->loop, app->busy);
    }

  if (app->due != NULL)
    {
      pnut_timer_cancel(h->loop, app->due);
    }

  if (app->owe != NULL)
    {
      pnut_timer_cancel(h->loop, app->owe);
    }

  if (app->stall != NULL)
    {
      pnut_timer_cancel(h->loop, app->stall);
    }

  if (app->connected && app->stream.open)
    {
      pnut_stream_end(&app->stream, PNUT_STATUS_UNAVAILABLE);
    }

  app_opening_gone(h->ui, app);
  memset(app, 0, sizeof(*app));
  app->host  = h;
  app->queue = queue;
  if (open)
    {
      app_gone(h->ui);
    }

  host_kick(h);
}

/* Service states answered a Stop */

static void host_stopped(FAR struct pnut_services_client_s *client,
                         int status, FAR const pnut_services_reply_t *out,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct host_s *h = arg;

  if (status != PNUT_STATUS_OK)
    {
      pnut_log(&h->ui->module, PNUT_LOG_WARNING, "an app not stopped: %d",
               status);
    }
}

/* Service states stops an app's task */

static void host_stop_task(FAR struct host_s *h, FAR const char *id)
{
  pnut_services_name_request_t in;
  int ret;

  memset(&in, 0, sizeof(in));
  strlcpy(in.name, id, sizeof(in.name));
  ret = pnut_services_stop(&h->services, &in, 0, host_stopped, h);
  if (ret < 0)
    {
      pnut_log(&h->ui->module, PNUT_LOG_WARNING, "app %s not stopped: %d",
               id, ret);
    }
}

/* ... an app's, once for its slot */

static void host_stop_slot(FAR struct host_app_s *app)
{
  if (!app->asked)
    {
      app->asked = true;
      host_stop_task(app->host, app->id);
    }
}

/* The app is done with the UI (it left, was taken off, crashed, or never
 * opened): its screen and Busy off, its stream ended.  Its slot is kept,
 * stopping, until its task is gone, as the services topic tells, or Stop's
 * deadline passes: the same app is started again only once its task has
 * exited (NxInit's Start of a task still running starts nothing).  A
 * shell's program's (no service's), one whose task is gone, and every
 * slot without the topic, are free at once.
 */

static void host_end(FAR struct host_app_s *app)
{
  FAR struct host_s *h = app->host;
  bool open = app->state == HOST_OPEN;

  if (app->shell || app->gone || !h->changes_open)
    {
      host_forget(app);
      return;
    }

  if (app->busy != NULL)
    {
      pnut_timer_cancel(h->loop, app->busy);
      app->busy = NULL;
    }

  if (app->owe != NULL)
    {
      pnut_timer_cancel(h->loop, app->owe);
      app->owe = NULL;
    }

  if (app->stall != NULL)
    {
      pnut_timer_cancel(h->loop, app->stall);
      app->stall = NULL;
    }

  if (app->state != HOST_STOPPING || app->due == NULL)
    {
      if (app->due != NULL)
        {
          pnut_timer_cancel(h->loop, app->due);
        }

      if (pnut_timer_start(h->loop, h->ui->config.times.stop, 0,
                           host_stop_due, app, &app->due) < 0)
        {
          app->due = NULL;
          host_forget(app);         /* No deadline: none waited for */
          return;
        }
    }

  if (app->connected && app->stream.open)
    {
      pnut_stream_end(&app->stream, PNUT_STATUS_UNAVAILABLE);
    }

  app->connected = false;
  app->count     = 0;
  app->head      = 0;
  app->wait      = HOST_WAIT_NONE;
  app->state     = HOST_STOPPING;
  app_opening_gone(h->ui, app);
  if (open)
    {
      app_gone(h->ui);
    }
}

/* Nothing drawn since the app was started: the UI's Busy */

static void host_busy_due(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct host_app_s *app = arg;

  app->busy = NULL;
  if (!app->drawn && app->wait == HOST_WAIT_NONE &&
      (app->state == HOST_OPENING || app->state == HOST_OPEN))
    {
      app->wait = HOST_WAIT_OPENING;
      app_opening(app->host->ui, app);
    }
}

/* An answer owed for 10 s: "<app> isn't responding", Wait or Close */

static void host_owe_hung(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct host_app_s *app = arg;

  app->owe = NULL;
  if (app->state == HOST_OPEN)
    {
      pnut_log(&app->host->ui->module, PNUT_LOG_WARNING,
               "app %s not responding", app->id);
      app->wait = HOST_WAIT_HUNG;
      app_opening_mode(app->host->ui, app);
    }
}

/* An answer owed for 3 s: "Waiting for <app>…", unless its Opening Busy
 * is up; then the 10 s
 */

static void host_owe_busy(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct host_app_s *app = arg;
  FAR struct host_s *h = app->host;
  FAR const struct ui_times_s *t = &h->ui->config.times;

  app->owe = NULL;
  if (app->state != HOST_OPEN)
    {
      return;
    }

  if (app->wait == HOST_WAIT_NONE)
    {
      app->wait = HOST_WAIT_WAITING;
      app_opening_mode(h->ui, app);
    }

  pnut_timer_start(h->loop, t->hung > t->busy ? t->hung - t->busy : 0, 0,
                   host_owe_hung, app, &app->owe);
}

/* The open app owes an answer: the times run from the first one owed */

static void host_owe(FAR struct host_app_s *app)
{
  FAR struct host_s *h = app->host;

  if (app->owe == NULL &&
      pnut_timer_start(h->loop, h->ui->config.times.busy, 0, host_owe_busy,
                       app, &app->owe) < 0)
    {
      app->owe = NULL;            /* No timer: nothing drawn for it */
    }
}

/* Answered, or no longer open: the times stop, the Busy they drew off */

static void host_unowe(FAR struct host_app_s *app)
{
  FAR struct host_s *h = app->host;

  if (app->owe != NULL)
    {
      pnut_timer_cancel(h->loop, app->owe);
      app->owe = NULL;
    }

  if (app->wait == HOST_WAIT_WAITING || app->wait == HOST_WAIT_HUNG)
    {
      app->wait = HOST_WAIT_NONE;
      app_opening_gone(h->ui, app);
    }
}

/* Another slot of the same app, not stopping, or NULL */

static FAR struct host_app_s *host_twin(FAR struct host_app_s *app)
{
  FAR struct host_s *h = app->host;
  int i;

  for (i = 0; i < HOST_APPS; i++)
    {
      if (&h->slots[i] != app && h->slots[i].state != HOST_FREE &&
          h->slots[i].state != HOST_STOPPING &&
          strcmp(h->slots[i].id, app->id) == 0)
        {
          return &h->slots[i];
        }
    }

  return NULL;
}

/* Stop's deadline passed (RFC 0008: 3 s): Service states stops the task,
 * the app's stream ended, its slot free
 */

static void host_stop_due(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct host_app_s *app = arg;
  FAR struct host_s *h = app->host;

  app->due = NULL;
  if (app->state != HOST_STOPPING)
    {
      return;
    }

  /* Told to stop (Stop sent, or Service states asked), it did not; one
   * that left of itself, or whose connection closed, was never told
   */

  pnut_log(&h->ui->module, PNUT_LOG_WARNING, "app %s %s", app->id,
           app->told || app->asked ? "did not stop" : "still running");

  /* Service states stops the task by its name: never a shell's program,
   * nor a task of a name another slot holds alive.  Its slot is free for
   * other apps; a new start of this one waits until its task is gone (the
   * owner, 2026-11-22), unless nothing would tell (no services topic).
   */

  if (!app->shell && host_twin(app) == NULL)
    {
      host_stop_slot(app);
      if (!app->gone && h->changes_open)
        {
          host_exiting_mark(h, app->id);
        }
    }

  host_forget(app);               /* Its stream ended */
}

/* Tell a connected app to stop: its screen off, Stop sent, its deadline
 * started; its slot is free when its connection closes, it leaves, or the
 * deadline passes
 */

static void host_stop(FAR struct host_app_s *app)
{
  FAR struct host_s *h = app->host;
  pnut_ui_event_t ev;

  if (app->busy != NULL)
    {
      pnut_timer_cancel(h->loop, app->busy);
      app->busy = NULL;
    }

  if (app->due != NULL)
    {
      pnut_timer_cancel(h->loop, app->due);
      app->due = NULL;
    }

  host_unowe(app);
  app_opening_gone(h->ui, app);
  if (app->state == HOST_OPEN)
    {
      app_gone(h->ui);
    }

  app->state = HOST_STOPPING;
  app->told  = true;
  if (pnut_timer_start(h->loop, h->ui->config.times.stop, 0, host_stop_due,
                       app, &app->due) < 0)
    {
      app->due = NULL;            /* Its connection closing frees it */
    }

  memset(&ev, 0, sizeof(ev));
  ev.which_kind = PNUT_UI_EVENT_STOP_TAG;
  host_event_to(app, &ev);
}

/* One app kept warm: the others, open longest ago first, stopped */

static void host_one_warm(FAR struct host_s *h)
{
  FAR struct host_app_s *oldest;
  int warm;
  int i;

  for (; ; )
    {
      oldest = NULL;
      warm = 0;
      for (i = 0; i < HOST_APPS; i++)
        {
          if (h->slots[i].state == HOST_WARM)
            {
              warm++;
              if (oldest == NULL || h->slots[i].order < oldest->order)
                {
                  oldest = &h->slots[i];
                }
            }
        }

      if (warm <= 1)
        {
          return;
        }

      pnut_log(&h->ui->module, PNUT_LOG_INFO, "app %s stopped: another "
               "is warm", oldest->id);
      host_stop(oldest);
    }
}

/* No Open since the app was started: stopped, its Busy off */

static void host_open_due(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct host_app_s *app = arg;
  FAR struct host_s *h = app->host;

  app->due = NULL;
  if (app->state == HOST_OPENING && !app->connected)
    {
      pnut_log(&h->ui->module, PNUT_LOG_WARNING, "app %s did not open",
               app->id);
      host_stop_slot(app);
      host_end(app);
    }
}

/* Take an app off, ending its stream with a status.  One taken off (not
 * leaving) has its task stopped by Service states, by its name: never a
 * shell's program's, nor a task of a name another slot holds alive.
 */

static void host_drop(FAR struct host_app_s *app, int status)
{
  FAR struct host_s *h = app->host;

  if (app->state == HOST_FREE || !app->connected)
    {
      return;                     /* Its connection went meanwhile */
    }

  pnut_log(&h->ui->module, PNUT_LOG_INFO, "app %s %s", app->id,
           status == PNUT_STATUS_ENDED ? "left" :
           status == PNUT_STATUS_BUSY ? "taken off: events wait" :
           "taken off: an event was not sent");
  if (app->connected && app->stream.open)
    {
      pnut_stream_end(&app->stream, status);
    }

  if (status != PNUT_STATUS_ENDED && !app->shell && host_twin(app) == NULL)
    {
      host_stop_slot(app);
    }

  host_end(app);
}

static void host_retry(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct host_s *h = arg;
  int i;

  h->retry = NULL;
  for (i = 0; i < HOST_APPS; i++)
    {
      host_flush(&h->slots[i]);
    }
}

/* No event read for the times' stall while one waits: the app stopped
 * reading (RFC 0037, Failures; RFC 0004: a client that stays behind is
 * disconnected).  Its stream ended, its task stopped, its screen off.
 */

static void host_stall_due(FAR struct pnut_loop_s *loop,
                           FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct host_app_s *app = arg;

  app->stall = NULL;
  if (app->state == HOST_FREE || app->count == 0)
    {
      return;
    }

  host_drop(app, PNUT_STATUS_BUSY);
}

/* Send what waits, as credit allows */

static void host_flush(FAR struct host_app_s *app)
{
  FAR struct host_s *h = app->host;
  int ret;

  while (app->connected && app->count > 0)
    {
      ret = pnut_ui_open_push(&h->server, &app->stream,
                              &app->queue[app->head]);
      if (ret == -EAGAIN)
        {
          /* No credit: ready comes with it, or the stall's time ends */

          if (app->stall == NULL &&
              pnut_timer_start(h->loop, h->ui->config.times.stall, 0,
                               host_stall_due, app, &app->stall) < 0)
            {
              app->stall = NULL;
            }

          return;
        }

      if (ret == -EBUSY)
        {
          /* The send buffer is full, which nothing announces.  With no
           * timer left, the next credit or event tries again.
           */

          if (h->retry == NULL)
            {
              pnut_timer_start(h->loop, HOST_RETRY_MS, 0, host_retry, h,
                               &h->retry);
            }

          return;
        }

      if (ret < 0)
        {
          /* The stream is over, or the connection failed: closed has
           * been called, or the stream is unlinked, and ending it does
           * nothing; an event that does not encode ends it
           */

          host_drop(app, PNUT_STATUS_INTERNAL);
          return;
        }

      app->head = (app->head + 1) % HOST_EVENTS;
      app->count--;

      /* An event read: the stall's time starts again at the next one
       * that waits
       */

      if (app->stall != NULL)
        {
          pnut_timer_cancel(h->loop, app->stall);
          app->stall = NULL;
        }
    }
}

static void host_ready(FAR struct pnut_stream_s *stream, FAR void *arg)
{
  host_flush(arg);
}

/* An app's crash count, or NULL; made when make is set (in place of one
 * not disabled, when the table is full)
 */

static FAR struct host_crash_s *host_crash(FAR struct host_s *h,
                                           FAR const char *id, bool make)
{
  FAR struct host_crash_s *c = NULL;
  int i;

  for (i = 0; i < HOST_CRASHES; i++)
    {
      if (h->crashes[i].id[0] != '\0' && strcmp(h->crashes[i].id, id) == 0)
        {
          return &h->crashes[i];
        }
    }

  if (!make)
    {
      return NULL;
    }

  /* A free entry, else the one not disabled with the fewest crashes;
   * never a disabled one: with every one disabled, none
   */

  for (i = 0; c == NULL && i < HOST_CRASHES; i++)
    {
      if (h->crashes[i].id[0] == '\0')
        {
          c = &h->crashes[i];
        }
    }

  for (i = 0; i < HOST_CRASHES; i++)
    {
      if (h->crashes[i].count < HOST_CRASHED &&
          (c == NULL || (c->id[0] != '\0' &&
                         h->crashes[i].count < c->count)))
        {
          c = &h->crashes[i];
        }
    }

  if (c == NULL)
    {
      return NULL;
    }

  strlcpy(c->id, id, sizeof(c->id));
  c->count = 0;
  return c;
}

/* The app ended well (it left, or exited after Stop): its count anew */

static void host_crash_clear(FAR struct host_s *h, FAR const char *id)
{
  FAR struct host_crash_s *c = host_crash(h, id, false);

  if (c != NULL)
    {
      memset(c, 0, sizeof(*c));
    }
}

/* The app crashed: counted (HOST_CRASHED in a row disable it), its
 * place forgotten
 */

static void host_crashed(FAR struct host_app_s *app)
{
  FAR struct host_s *h = app->host;
  FAR struct host_crash_s *c;

  c = host_crash(h, app->id, true);
  if (c != NULL)
    {
      c->count++;
    }

  pnut_log(&h->ui->module, PNUT_LOG_WARNING, "app %s crashed (%u)%s",
           app->id, c != NULL ? c->count : 0,
           c != NULL && c->count >= HOST_CRASHED ? ": disabled" : "");
  host_resume_forget(h, app->id);
}

/* The app ended its stream, or its connection went: told to stop, it
 * ended well; otherwise it crashed (a shell's program never counts)
 */

static void host_closed(FAR struct pnut_stream_s *stream, int status,
                        FAR void *arg)
{
  FAR struct host_app_s *app = arg;
  FAR struct host_s *h = app->host;

  if (app->state == HOST_FREE || !app->connected)
    {
      return;
    }

  if (app->state == HOST_STOPPING || app->shell)
    {
      pnut_log(&h->ui->module, PNUT_LOG_INFO, "app %s gone", app->id);
      if (!app->shell)
        {
          host_crash_clear(h, app->id);
        }
    }
  else
    {
      host_crashed(app);
    }

  app->connected = false;         /* Its stream is over */
  host_end(app);
}

static void host_refuse(FAR struct host_s *h, int status, uint32_t code,
                        FAR const char *why)
{
  pnut_ui_fail(&h->server, &h->asked, status, code, why);
}

/* The app becomes the open one: the one open before is kept warm, its
 * screen off
 */

static void host_bring(FAR struct host_app_s *app)
{
  FAR struct host_s *h = app->host;
  int i;

  for (i = 0; i < HOST_APPS; i++)
    {
      if (&h->slots[i] != app && h->slots[i].state == HOST_OPEN)
        {
          host_unowe(&h->slots[i]);
          h->slots[i].state = HOST_WARM;
          app_gone(h->ui);
        }
    }

  app->state = HOST_OPEN;
  app->order = ++h->orders;
  app->switching = true;          /* Its first screen: a full refresh */
  host_one_warm(h);
}

/* An app's kept resume token, or NULL */

static FAR struct host_resume_s *host_resume_find(FAR struct host_s *h,
                                                 FAR const char *id)
{
  int i;

  for (i = 0; i < HOST_RESUMES; i++)
    {
      if (h->resumes[i].id[0] != '\0' && strcmp(h->resumes[i].id, id) == 0)
        {
          return &h->resumes[i];
        }
    }

  return NULL;
}

/* An app is done (it left, was closed, or failed): its token forgotten */

static void host_resume_forget(FAR struct host_s *h, FAR const char *id)
{
  FAR struct host_resume_s *r = host_resume_find(h, id);

  if (r != NULL)
    {
      memset(r, 0, sizeof(*r));
    }
}

/* Keep an app's token, in place of the one given longest ago when the
 * table is full
 */

static void host_resume_keep(FAR struct host_s *h, FAR const char *id,
                             FAR const uint8_t *token, uint16_t len)
{
  FAR struct host_resume_s *r = host_resume_find(h, id);
  int i;

  for (i = 0; r == NULL && i < HOST_RESUMES; i++)
    {
      if (h->resumes[i].id[0] == '\0')
        {
          r = &h->resumes[i];
        }
    }

  if (r == NULL)
    {
      r = &h->resumes[0];
      for (i = 1; i < HOST_RESUMES; i++)
        {
          if (h->resumes[i].order < r->order)
            {
              r = &h->resumes[i];
            }
        }
    }

  strlcpy(r->id, id, sizeof(r->id));
  r->order = ++h->resume_orders;
  r->len = len;
  memcpy(r->token, token, len);
}

static void host_welcome(FAR struct host_app_s *app)
{
  FAR struct host_resume_s *r;
  pnut_ui_event_t ev;

  memset(&ev, 0, sizeof(ev));
  ev.which_kind = PNUT_UI_EVENT_WELCOME_TAG;
  ev.kind.welcome.level = HOST_LEVEL;
  ev.kind.welcome.display = PNUT_UI_WELCOME_DISPLAY_EPAPER;
  ev.kind.welcome.rows_per_page = HOST_ROWS_PAGE;
  ev.kind.welcome.rows_kept = HOST_ROWS_KEPT;

  /* The owner's language and region come with Languages (#88) */

  ev.kind.welcome.languages_count = 1;
  strlcpy(ev.kind.welcome.languages[0], "en",
          sizeof(ev.kind.welcome.languages[0]));
  ev.kind.welcome.clock24 = true;
  ev.kind.welcome.reason = PNUT_UI_WELCOME_REASON_OPENED;

  /* Started again after it was stopped behind: its token, kept until it
   * gives another or clears it, leaves of itself, or crashes
   */

  r = app->shell ? NULL : host_resume_find(app->host, app->id);
  if (r != NULL)
    {
      ev.kind.welcome.reason = PNUT_UI_WELCOME_REASON_RESUMED;
      ev.kind.welcome.token.size = r->len;
      memcpy(ev.kind.welcome.token.bytes, r->token, r->len);
    }

  host_event_to(app, &ev);
}

/* Accept the Open asked into an app's slot: its stream open */

static int host_accept(FAR struct host_app_s *app)
{
  FAR struct host_s *h = app->host;
  int ret;

  ret = pnut_ui_open_accept(&h->server, &h->asked, &app->stream,
                            host_ready, host_closed, app);
  if (ret < 0)
    {
      return ret;                 /* It went meanwhile */
    }

  app->connected = true;
  return 0;
}

/* Who answered: the app is named, or refused */

static void host_who(FAR struct pnut_services_client_s *client, int status,
                     FAR const pnut_services_who_reply_t *out,
                     FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct host_s *h = arg;
  FAR struct host_app_s *app;
  struct manifest_s manifest;
  char id[MANIFEST_ID_MAX + 1];
  bool shell = false;
  int ret;

  if (!h->asking)
    {
      return;
    }

  h->asking = false;
  if (status == PNUT_STATUS_NOTFOUND)
    {
      host_refuse(h, PNUT_STATUS_DENIED, PNUT_UI_ERROR_CODE_NOT_AN_APP,
                  "not an app");
      return;
    }

  if (status != PNUT_STATUS_OK || out == NULL)
    {
      host_refuse(h, PNUT_STATUS_UNAVAILABLE, PNUT_UI_ERROR_CODE_NONE,
                  "who it is cannot be told");
      return;
    }

  if (out->name[0] != '\0')
    {
      /* A service's: its name is the app's id, and it has a manifest.  A
       * system service ("ui", "telephony") has neither a reverse-DNS name
       * nor a manifest, and is refused.
       */

      strlcpy(id, out->name, sizeof(id));
      ret = manifest_load(h->apps, id, &manifest);
      if (ret == -ENOENT || ret == -EINVAL)
        {
          host_refuse(h, PNUT_STATUS_DENIED, PNUT_UI_ERROR_CODE_NOT_AN_APP,
                      "not an app");
          return;
        }
      else if (ret < 0)
        {
          host_refuse(h, PNUT_STATUS_UNAVAILABLE, PNUT_UI_ERROR_CODE_NONE,
                      "its manifest cannot be read");
          return;
        }
    }
  else if (out->shell)
    {
      /* Typed in a developer's shell: under the id it gives, with every
       * template; never an app's id, one with a manifest (the owner,
       * 2026-11-22), so that it touches no app's place or crashes
       */

      if (!manifest_good_id(h->claimed))
        {
          host_refuse(h, PNUT_STATUS_INVALID, PNUT_UI_ERROR_CODE_BAD_ID,
                      "not an app's id");
          return;
        }

      strlcpy(id, h->claimed, sizeof(id));
      shell = true;
      ret = manifest_load(h->apps, id, &manifest);
      if (ret == 0 || ret == -EINVAL)
        {
          host_refuse(h, PNUT_STATUS_DENIED, PNUT_UI_ERROR_CODE_BAD_ID,
                      "an app's id");
          return;
        }
      else if (ret != -ENOENT)
        {
          host_refuse(h, PNUT_STATUS_UNAVAILABLE, PNUT_UI_ERROR_CODE_NONE,
                      "its manifest cannot be read");
          return;
        }

      manifest_any(&manifest);
    }
  else
    {
      host_refuse(h, PNUT_STATUS_DENIED, PNUT_UI_ERROR_CODE_NOT_AN_APP,
                  "not an app");
      return;
    }

  /* A native app the system UI started is opening; any other task of a
   * native app is told to stop (RFC 0037: after the system UI's own
   * restart, an app's Open is answered with Stop), in the slot of a task
   * cancelled before it opened if it is that one, never in its place; a
   * shell's program opens as it comes
   */

  if (!shell)
    {
      app = host_unconnected(h, id, HOST_OPENING);
      if (app == NULL)
        {
          app = host_unconnected(h, id, HOST_STOPPING);
          if (app == NULL)
            {
              app = host_free_slot(h);
            }

          if (app == NULL || host_accept(app) < 0)
            {
              host_refuse(h, PNUT_STATUS_BUSY,
                          PNUT_UI_ERROR_CODE_ANOTHER_APP,
                          "no room for another app");
              return;
            }

          strlcpy(app->id, id, sizeof(app->id));
          pnut_log(&h->ui->module, PNUT_LOG_INFO,
                   "app %s not started here: told to stop", id);
          host_stop(app);
          return;
        }
    }
  else
    {
      app = host_find(h, id) != NULL ? NULL : host_free_slot(h);
      if (app == NULL)
        {
          host_refuse(h, PNUT_STATUS_BUSY, PNUT_UI_ERROR_CODE_ANOTHER_APP,
                      "another app is open");
          return;
        }
    }

  if (host_accept(app) < 0)
    {
      if (!shell)
        {
          host_stop_slot(app);
          host_end(app);
        }

      return;
    }

  if (app->due != NULL)
    {
      pnut_timer_cancel(h->loop, app->due);
      app->due = NULL;
    }

  strlcpy(app->id, id, sizeof(app->id));
  app->manifest = manifest;
  app->shell = shell;
  host_bring(app);
  pnut_log(&h->ui->module, PNUT_LOG_INFO, "app %s opened%s", app->id,
           shell ? " from the developer's shell" : "");
  host_welcome(app);
}

static void host_open_app(FAR struct pnut_ui_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_ui_open_request_t *in,
                          FAR void *arg)
{
  FAR struct host_s *h = arg;
  pnut_services_who_request_t who;
  int ret;

  /* One Open named at a time (Who answers within its time) */

  if (h->asking)
    {
      pnut_ui_fail(server, req, PNUT_STATUS_BUSY,
                   PNUT_UI_ERROR_CODE_ANOTHER_APP, "an app is opening");
      return;
    }

  h->asked = *req;
  strlcpy(h->claimed, in->app, sizeof(h->claimed));
  h->asking = true;

  memset(&who, 0, sizeof(who));
  who.pid = req->pid;
  ret = pnut_services_who(&h->services, &who, h->ui->config.times.who,
                          host_who, h);
  if (ret < 0)
    {
      h->asking = false;
      pnut_ui_fail(server, req, PNUT_STATUS_UNAVAILABLE,
                   PNUT_UI_ERROR_CODE_NONE, "who it is cannot be told");
    }
}

/* The template a Show names, or UI_TEMPLATE_NONE */

static int host_template(FAR const pnut_ui_show_request_t *in)
{
  switch (in->which_content)
    {
      case PNUT_UI_SHOW_REQUEST_LIST_TAG:
        return PNUT_UI_TEMPLATE_UI_TEMPLATE_LIST;

      case PNUT_UI_SHOW_REQUEST_DETAIL_TAG:
        return PNUT_UI_TEMPLATE_UI_TEMPLATE_DETAIL;

      case PNUT_UI_SHOW_REQUEST_CONFIRM_TAG:
        return PNUT_UI_TEMPLATE_UI_TEMPLATE_CONFIRM;

      case PNUT_UI_SHOW_REQUEST_EMPTY_TAG:
        return PNUT_UI_TEMPLATE_UI_TEMPLATE_EMPTY;

      case PNUT_UI_SHOW_REQUEST_BUSY_TAG:
        return PNUT_UI_TEMPLATE_UI_TEMPLATE_BUSY;

      default:
        return PNUT_UI_TEMPLATE_UI_TEMPLATE_NONE;
    }
}

/* Answer a request from the app with its method's reply: OK, or invalid
 * for -EINVAL
 */

typedef CODE int (*host_reply_t)(FAR struct pnut_ui_server_s *server,
                                 FAR const struct pnut_request_s *req,
                                 int status, FAR const pnut_ui_reply_t *out);

static void host_answer(FAR struct pnut_ui_server_s *server,
                        FAR const struct pnut_request_s *req,
                        host_reply_t reply, int ret)
{
  pnut_ui_reply_t out;

  if (ret == -EINVAL)
    {
      pnut_ui_fail(server, req, PNUT_STATUS_INVALID,
                   PNUT_UI_ERROR_CODE_MALFORMED, "does not hold together");
    }
  else if (ret < 0)
    {
      pnut_ui_fail(server, req, PNUT_STATUS_INTERNAL,
                   PNUT_UI_ERROR_CODE_NONE, "not drawn");
    }
  else
    {
      memset(&out, 0, sizeof(out));
      reply(server, req, PNUT_STATUS_OK, &out);
    }
}

/* How an app's change is drawn on e-paper (RFC 0037): an answer owed (to
 * a key, Shown, its first screen) at once, so a late one costs one more
 * redraw, not a gathering; otherwise as it asks: at once, quietly, or
 * gathered.  In a key's window it joins that redraw (redraw.c).
 */

static void host_draw_begin(FAR struct host_app_s *app, bool now, bool quiet)
{
  FAR struct ui_s *ui = app->host->ui;
  int how;

  how = app->owe != NULL || app->wait != HOST_WAIT_NONE || now ?
        REDRAW_APP_NOW : quiet ? REDRAW_APP_QUIET : REDRAW_APP_GATHER;
  display_app(&ui->display, how, ui->config.times.gather,
              ui->config.times.spacing);
}

static void host_draw_end(FAR struct host_app_s *app)
{
  display_app_end(&app->host->ui->display);
}

/* The open app's, a request that draws; another's is refused */

static FAR struct host_app_s *
host_open_from(FAR struct pnut_ui_server_s *server,
               FAR const struct pnut_request_s *req, FAR struct host_s *h)
{
  FAR struct host_app_s *app = host_from(h, req);

  if (app == NULL || app->state != HOST_OPEN)
    {
      pnut_ui_fail(server, req, PNUT_STATUS_DENIED,
                   PNUT_UI_ERROR_CODE_NOT_OPEN, "not the open app");
      return NULL;
    }

  return app;
}

static void host_show(FAR struct pnut_ui_server_s *server,
                      FAR const struct pnut_request_s *req,
                      FAR const pnut_ui_show_request_t *in, FAR void *arg)
{
  FAR struct host_s *h = arg;
  FAR struct host_app_s *app = host_open_from(server, req, h);
  int tmpl;
  int ret;

  if (app == NULL)
    {
      return;
    }

  /* A Show refused here answers nothing owed: the app may send another,
   * in an older template
   */

  tmpl = host_template(in);
  if (tmpl == PNUT_UI_TEMPLATE_UI_TEMPLATE_NONE)
    {
      pnut_ui_fail(server, req, PNUT_STATUS_INVALID,
                   PNUT_UI_ERROR_CODE_UNKNOWN_TEMPLATE, "no template");
      return;
    }

  if (!manifest_allows(&app->manifest, tmpl))
    {
      pnut_ui_fail(server, req, PNUT_STATUS_DENIED,
                   PNUT_UI_ERROR_CODE_NOT_LISTED,
                   "a template its manifest does not list");
      return;
    }

  /* Only a Show drawn answers what is owed: one refused leaves the times
   * running
   */

  host_draw_begin(app, in->now, in->quiet);
  ret = app_show(h->ui, in);
  if (ret == 0)
    {
      host_unowe(app);            /* Answered */
    }

  host_draw_end(app);
  if (ret == 0 && !in->sheet)
    {
      if (app->switching)
        {
          display_switch(&h->ui->display);  /* Switching apps */
          app->switching = false;
        }

      app->drawn = true;
      app->wait  = HOST_WAIT_NONE;  /* Its Busy replaced */
      if (app->busy != NULL)
        {
          pnut_timer_cancel(h->loop, app->busy);
          app->busy = NULL;
        }
    }

  host_answer(server, req, pnut_ui_show_reply, ret);
}

static void host_update(FAR struct pnut_ui_server_s *server,
                        FAR const struct pnut_request_s *req,
                        FAR const pnut_ui_update_request_t *in,
                        FAR void *arg)
{
  FAR struct host_s *h = arg;
  FAR struct host_app_s *app = host_open_from(server, req, h);
  int ret;

  if (app != NULL)
    {
      host_draw_begin(app, in->now, in->quiet);
      ret = app_update(h->ui, in);
      host_draw_end(app);
      host_answer(server, req, pnut_ui_update_reply, ret);
    }
}

static void host_rows(FAR struct pnut_ui_server_s *server,
                      FAR const struct pnut_request_s *req,
                      FAR const pnut_ui_rows_request_t *in, FAR void *arg)
{
  FAR struct host_s *h = arg;
  FAR struct host_app_s *app = host_open_from(server, req, h);
  int ret;

  if (app != NULL)
    {
      /* Only a page taken answers the page owed (the owner, 2026-11-22,
       * as for Show): one dropped leaves the times running
       */

      host_draw_begin(app, false, false);
      ret = app_rows(h->ui, in);
      if (ret > 0)
        {
          host_unowe(app);        /* Answered */
        }

      host_draw_end(app);
      host_answer(server, req, pnut_ui_rows_reply, ret);
    }
}

/* Any app connected may leave: the open one's screen goes with it */

static void host_leave(FAR struct pnut_ui_server_s *server,
                       FAR const struct pnut_request_s *req,
                       FAR const pnut_ui_leave_request_t *in,
                       FAR void *arg)
{
  FAR struct host_s *h = arg;
  FAR struct host_app_s *app = host_from(h, req);

  if (app == NULL)
    {
      pnut_ui_fail(server, req, PNUT_STATUS_DENIED,
                   PNUT_UI_ERROR_CODE_NOT_OPEN, "not an app's");
      return;
    }

  /* Leaving of itself, the app is done; told to stop, it will be back.  A
   * shell's program has neither a place kept nor crashes counted.
   */

  if (!app->shell)
    {
      if (app->state != HOST_STOPPING)
        {
          host_resume_forget(h, app->id);
        }

      host_crash_clear(h, app->id);
    }

  host_drop(app, PNUT_STATUS_ENDED);
  host_answer(server, req, pnut_ui_leave_reply, 0);
}

/* The app's place: kept, or forgotten when empty; from any app connected,
 * one told to stop too (Stop is when it saves), unless the owner closed
 * it.  A shell's program's is answered, never kept.
 */

static void host_resume(FAR struct pnut_ui_server_s *server,
                        FAR const struct pnut_request_s *req,
                        FAR const pnut_ui_resume_request_t *in,
                        FAR void *arg)
{
  FAR struct host_s *h = arg;
  FAR struct host_app_s *app = host_from(h, req);

  if (app == NULL)
    {
      pnut_ui_fail(server, req, PNUT_STATUS_DENIED,
                   PNUT_UI_ERROR_CODE_NOT_OPEN, "not an app's");
      return;
    }

  if (app->closed || app->shell)
    {
      host_answer(server, req, pnut_ui_resume_reply, 0);
      return;                     /* Done with: nothing to come back to */
    }

  if (in->token.size == 0)
    {
      host_resume_forget(h, app->id);
    }
  else
    {
      host_resume_keep(h, app->id, in->token.bytes, in->token.size);
    }

  host_answer(server, req, pnut_ui_resume_reply, 0);
}

static const struct pnut_ui_handlers_s g_host_handlers =
{
  .open   = host_open_app,
  .show   = host_show,
  .update = host_update,
  .rows   = host_rows,
  .leave  = host_leave,
  .resume = host_resume
};

/* Service states answered a Start: a failure frees the app's slot */

/* Service states answered a Start, which may be late (it answers when
 * NxInit does): a failure frees the slot of the app it started, found by
 * its Start's number, if still opening and not connected
 */

static void host_started(FAR struct pnut_services_client_s *client,
                         int status, FAR const pnut_services_reply_t *out,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct host_s *h = (FAR struct host_s *)
    ((FAR char *)client - offsetof(struct host_s, services));
  uint32_t start = (uint32_t)(uintptr_t)arg;
  FAR struct host_app_s *app;
  int i;

  if (status == PNUT_STATUS_OK)
    {
      return;
    }

  for (i = 0; i < HOST_APPS; i++)
    {
      app = &h->slots[i];
      if (app->start == start && app->state == HOST_OPENING &&
          !app->connected)
        {
          pnut_log(&h->ui->module, PNUT_LOG_WARNING,
                   "app %s not started: %d", app->id, status);
          host_forget(app);
          return;
        }
    }
}

/* Start an app in a free slot: Service states' Start, the UI's Busy at
 * the times' opening unless it is up already, the Open's deadline
 */

static int host_launch(FAR struct host_app_s *app, FAR const char *id,
                       FAR const struct manifest_s *manifest)
{
  FAR struct host_s *h = app->host;
  pnut_services_name_request_t in;
  int ret = 0;

  app->state    = HOST_OPENING;
  app->manifest = *manifest;
  app->start    = ++h->starts;
  strlcpy(app->id, id, sizeof(app->id));

  memset(&in, 0, sizeof(in));
  strlcpy(in.name, id, sizeof(in.name));
  ret = pnut_services_start(&h->services, &in, 0, host_started,
                            (FAR void *)(uintptr_t)app->start);
  if (ret >= 0 && app->wait == HOST_WAIT_NONE)
    {
      ret = pnut_timer_start(h->loop, h->ui->config.times.opening, 0,
                             host_busy_due, app, &app->busy);
    }

  if (ret >= 0)
    {
      ret = pnut_timer_start(h->loop, h->ui->config.times.open, 0,
                             host_open_due, app, &app->due);
    }

  if (ret < 0)
    {
      pnut_log(&h->ui->module, PNUT_LOG_WARNING, "app %s not started: %d",
               id, ret);
      host_stop_task(h, id);
      host_forget(app);
      return ret;
    }

  pnut_log(&h->ui->module, PNUT_LOG_INFO, "app %s starting", id);
  return 0;
}

/* The app waiting to start forgotten, its Busy off */

static void host_next_drop(FAR struct host_s *h)
{
  app_opening_gone(h->ui, &h->next);
  memset(&h->next, 0, sizeof(h->next));
  h->next.host = h;
  h->getting = false;
}

/* Service states answered Get about the old task of the app waiting: one
 * stopped, and still marked as when asked (by its order), is gone, and
 * the app may start.  Its answer may be older than the topic's news: it
 * only takes a mark off.  No answer: the app waits for the topic.  Either
 * way the next Get may go, if one was wanted meanwhile.
 */

static void host_got(FAR struct pnut_services_client_s *client, int status,
                     FAR const pnut_service_state_t *out,
                     FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct host_s *h = (FAR struct host_s *)
    ((FAR char *)client - offsetof(struct host_s, services));
  uint32_t order = (uint32_t)(uintptr_t)arg;
  FAR struct host_exiting_s *e;

  h->get_out = false;
  if (status != PNUT_STATUS_OK || out == NULL)
    {
      pnut_log(&h->ui->module, PNUT_LOG_WARNING,
               "an app's state not told: %d", status);
    }
  else
    {
      e = host_exiting(h, out->name);
      if (e != NULL && e->order == order &&
          out->state == PNUT_SERVICE_STATE_STATE_STOPPED)
        {
          pnut_log(&h->ui->module, PNUT_LOG_INFO, "app %s stopped (Get)",
                   e->id);
          memset(e, 0, sizeof(*e));
        }
    }

  host_kick(h);
}

/* The app waiting to start, started once no task of its id is left and a
 * slot is free: in that slot, its Busy (up since the tile) moved with it.
 * Its old task last seen exiting, past its slot's deadline: the topic
 * tells when it stops, and Service states' Get is asked once for the wait
 * (again at a gap in the topic, or a tap), as the topic may have missed
 * it; one Get at a time, so that the services' calls stay free for
 * starts.
 */

static void host_next_due(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct host_s *h = arg;
  FAR struct host_app_s *n = &h->next;
  FAR struct host_exiting_s *e;
  FAR struct host_app_s *app;
  pnut_services_name_request_t in;
  struct manifest_s manifest;
  char id[MANIFEST_ID_MAX + 1];
  int ret;

  h->kick = NULL;
  if (n->state == HOST_FREE || host_find(h, n->id) != NULL)
    {
      return;                     /* None, or its task still exits */
    }

  e = host_exiting(h, n->id);
  if (e != NULL)
    {
      if (!h->getting && !h->get_out)
        {
          memset(&in, 0, sizeof(in));
          strlcpy(in.name, n->id, sizeof(in.name));
          ret = pnut_services_get(&h->services, &in,
                                  h->ui->config.times.who, host_got,
                                  (FAR void *)(uintptr_t)e->order);
          if (ret < 0)
            {
              pnut_log(&h->ui->module, PNUT_LOG_WARNING,
                       "app %s: its state not asked: %d", n->id, ret);
            }

          h->getting = ret >= 0;
          h->get_out = ret >= 0;
        }

      return;                     /* Its old task not seen gone */
    }

  app = host_free_slot(h);
  if (app == NULL)
    {
      return;                     /* No room yet */
    }

  strlcpy(id, n->id, sizeof(id));
  manifest = n->manifest;
  app->wait = HOST_WAIT_OPENING;
  app_opening_move(h->ui, n, app);
  memset(n, 0, sizeof(*n));
  n->host = h;
  h->getting = false;
  host_launch(app, id, &manifest);
}

/* A change of a service's state, on Service states' services topic.  An
 * app's task seen starting or ready since its start is that start's; seen
 * gone before the app opened, the app crashed (the owner, 2026-11-22: its
 * Busy off at once); seen gone after, its slot is free once the app is
 * done with the UI.  A gone task of a start not seen running is an older
 * task's end, and changes nothing but the mark of an app whose old task
 * outlived its slot.  Changes missed (the sequence's gap): an app waiting
 * for its old task asks Service states' Get again; otherwise the Open's
 * and Stop's deadlines stand in.
 */

static void host_service_change(FAR struct pnut_service_change_reader_s
                                *reader,
                                FAR const pnut_service_change_t *msg,
                                FAR void *arg)
{
  FAR struct host_s *h = arg;
  FAR struct host_exiting_s *e;
  FAR struct host_app_s *app;
  bool gone = msg->state == PNUT_SERVICE_STATE_STATE_STOPPED ||
              msg->state == PNUT_SERVICE_STATE_STATE_RESTARTING;
  int i;

  if (h->seq != 0 && msg->seq != h->seq + 1)
    {
      pnut_log(&h->ui->module, PNUT_LOG_INFO,
               "services' changes missed: %" PRIu32 " to %" PRIu32,
               h->seq, msg->seq);
      h->getting = false;
      host_kick(h);
    }

  h->seq = msg->seq;
  e = gone ? host_exiting(h, msg->name) : NULL;
  if (e != NULL)
    {
      memset(e, 0, sizeof(*e));   /* Its old task gone */
      host_kick(h);
    }

  for (i = 0; i < HOST_APPS; i++)
    {
      app = &h->slots[i];
      if (app->state == HOST_FREE || app->shell ||
          strcmp(app->id, msg->name) != 0)
        {
          continue;
        }

      if (!gone)
        {
          /* Starting or ready: this start's task.  Stopping is an older
           * one's, still exiting.
           */

          if (app->state == HOST_OPENING && msg->pid != 0 &&
              (msg->state == PNUT_SERVICE_STATE_STATE_STARTING ||
               msg->state == PNUT_SERVICE_STATE_STATE_READY))
            {
              app->pid = msg->pid;
            }

          continue;
        }

      if (app->state == HOST_OPENING && !app->connected)
        {
          if (app->pid != 0)
            {
              pnut_log(&h->ui->module, PNUT_LOG_WARNING,
                       "app %s exited before it opened", app->id);
              host_crashed(app);
              host_forget(app);
            }
        }
      else if (app->connected)
        {
          app->gone = true;         /* Free once its connection closes */
        }
      else
        {
          host_forget(app);         /* Done with, its task gone */
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int host_open(FAR struct host_s *h, FAR struct ui_s *ui,
              FAR struct pnut_loop_s *loop, FAR const char *apps,
              FAR const char *services)
{
  int ret;
  int i;

  memset(h, 0, sizeof(*h));
  h->ui   = ui;
  h->loop = loop;
  h->apps = apps;
  h->next.host = h;
  for (i = 0; i < HOST_APPS; i++)
    {
      h->slots[i].host  = h;
      h->slots[i].queue = h->queues[i];
    }

  ret = pnut_services_connect(loop, services, NULL, NULL, &h->services);
  if (ret < 0)
    {
      return ret;
    }

  h->services_open = true;

  /* Without the services topic, apps' slots are free as they are done
   * with, and the deadlines stand in for what it would tell
   */

  ret = pnut_service_change_subscribe(loop, host_service_change, h,
                                      &h->changes);
  if (ret < 0)
    {
      pnut_log(&ui->module, PNUT_LOG_WARNING, "No services topic: %d", ret);
    }

  h->changes_open = ret >= 0;
  ret = pnut_ui_serve(loop, HOST_SERVICE, 0, &g_host_handlers, h,
                      &h->server);
  if (ret < 0)
    {
      pnut_services_disconnect(&h->services);
      h->services_open = false;
      return ret;
    }

  h->serving = true;
  return 0;
}

void host_close(FAR struct host_s *h)
{
  int i;

  if (h->retry != NULL)
    {
      pnut_timer_cancel(h->loop, h->retry);
      h->retry = NULL;
    }

  if (h->kick != NULL)
    {
      pnut_timer_cancel(h->loop, h->kick);
      h->kick = NULL;
    }

  memset(&h->next, 0, sizeof(h->next));
  h->next.host = h;
  h->getting = false;
  if (h->changes_open)
    {
      pnut_service_change_unsubscribe(&h->changes);
      h->changes_open = false;
    }

  for (i = 0; i < HOST_APPS; i++)
    {
      FAR struct host_app_s *app = &h->slots[i];

      if (app->connected && app->stream.open)
        {
          pnut_stream_end(&app->stream, PNUT_STATUS_UNAVAILABLE);
        }

      if (app->state != HOST_FREE)
        {
          host_forget(app);
        }
    }

  h->asking = false;
  if (h->serving)
    {
      pnut_ui_close(&h->server);
      h->serving = false;
    }

  if (h->services_open)
    {
      pnut_services_disconnect(&h->services);
      h->services_open = false;
    }
}

int host_start(FAR struct host_s *h, FAR const char *id)
{
  FAR struct host_app_s *found = NULL;
  FAR struct host_app_s *n = &h->next;
  FAR struct host_app_s *app;
  FAR struct host_crash_s *c;
  struct manifest_s manifest;
  pnut_ui_event_t ev;
  bool stopping = false;
  int ret;
  int i;

  if (!manifest_good_id(id))
    {
      return -EINVAL;
    }

  for (i = 0; i < HOST_APPS; i++)
    {
      app = &h->slots[i];
      if (app->state != HOST_FREE && strcmp(app->id, id) == 0)
        {
          if (app->state == HOST_STOPPING)
            {
              stopping = true;
            }
          else
            {
              found = app;
            }
        }
    }

  if (found != NULL && found->state != HOST_WARM)
    {
      return 0;                   /* Opening, or open already */
    }

  if (found == NULL)
    {
      if (n->state != HOST_FREE && strcmp(n->id, id) == 0)
        {
          /* Waiting to start already: tapped again, Service states is
           * asked again about an old task last seen exiting
           */

          h->getting = false;
          host_kick(h);
          return 0;
        }

      c = host_crash(h, id, false);
      if (c != NULL && c->count >= HOST_CRASHED)
        {
          return -EACCES;         /* The owner turns it on again */
        }

      ret = manifest_load(h->apps, id, &manifest);
      if (ret < 0)
        {
          pnut_log(&h->ui->module, PNUT_LOG_WARNING,
                   "app %s: no manifest: %d", id, ret);
          return ret;
        }
    }

  /* The newer tap wins: an app still opening, nothing drawn since its
   * start, is cancelled; so is one waiting to start
   */

  for (i = 0; i < HOST_APPS; i++)
    {
      app = &h->slots[i];
      if (app != found && !app->shell && !app->drawn &&
          (app->state == HOST_OPENING || app->state == HOST_OPEN))
        {
          host_cancel(app);
        }
    }

  if (n->state != HOST_FREE)
    {
      host_cancel(n);
    }

  /* A warm app is brought back: Shown, which it answers with Show of its
   * current screen
   */

  if (found != NULL)
    {
      host_bring(found);
      pnut_log(&h->ui->module, PNUT_LOG_INFO, "app %s shown", id);
      memset(&ev, 0, sizeof(ev));
      ev.which_kind = PNUT_UI_EVENT_SHOWN_TAG;
      host_event_to(found, &ev);
      return 0;
    }

  /* A task of the app still exits (NxInit would start nothing), in a
   * slot or past its deadline, or no slot is free: "Opening" at once, and
   * the app started as soon as it can (the owner, 2026-11-22)
   */

  app = host_free_slot(h);
  if (stopping || app == NULL || host_exiting(h, id) != NULL)
    {
      memset(n, 0, sizeof(*n));
      h->getting  = false;
      n->host     = h;
      n->state    = HOST_OPENING;
      n->wait     = HOST_WAIT_OPENING;
      n->manifest = manifest;
      strlcpy(n->id, id, sizeof(n->id));
      pnut_log(&h->ui->module, PNUT_LOG_INFO, "app %s waits to start", id);
      app_opening(h->ui, n);
      host_kick(h);
      return 0;
    }

  return host_launch(app, id, &manifest);
}

void host_cancel(FAR struct host_app_s *app)
{
  FAR struct host_s *h = app->host;

  if (app->state == HOST_FREE || app->state == HOST_STOPPING)
    {
      return;
    }

  pnut_log(&h->ui->module, PNUT_LOG_INFO, "app %s cancelled", app->id);
  if (app == &h->next)
    {
      host_next_drop(h);          /* Never started */
      return;
    }

  if (!app->connected)
    {
      host_stop_slot(app);
      host_end(app);              /* Its task exits */
      return;
    }

  host_stop(app);                 /* It has opened */
}

void host_wait(FAR struct host_app_s *app)
{
  FAR struct host_s *h = app->host;

  if (app->state != HOST_OPEN || app->wait != HOST_WAIT_HUNG)
    {
      return;
    }

  if (app->owe != NULL)
    {
      pnut_timer_cancel(h->loop, app->owe);
    }

  app->wait = HOST_WAIT_WAITING;
  app_opening_mode(h->ui, app);
  if (pnut_timer_start(h->loop, h->ui->config.times.hung, 0, host_owe_hung,
                       app, &app->owe) < 0)
    {
      app->owe = NULL;
    }
}

void host_enable(FAR struct host_s *h, FAR const char *id)
{
  pnut_log(&h->ui->module, PNUT_LOG_INFO, "app %s on again", id);
  host_crash_clear(h, id);
}

void host_close_app(FAR struct host_app_s *app)
{
  app->closed = true;
  if (!app->shell)
    {
      host_resume_forget(app->host, app->id);
    }

  host_cancel(app);
}

FAR const struct host_resume_s *host_token(FAR struct host_s *h,
                                           FAR const char *id)
{
  return host_resume_find(h, id);
}

void host_home(FAR struct host_s *h)
{
  FAR struct host_app_s *app;
  int i;

  for (i = 0; i < HOST_APPS; i++)
    {
      app = &h->slots[i];
      if (app->state == HOST_OPEN && (app->drawn || app->shell))
        {
          pnut_log(&h->ui->module, PNUT_LOG_INFO, "app %s warm", app->id);
          host_unowe(app);
          app->state = HOST_WARM;
          app_gone(h->ui);
        }
      else if (app->state == HOST_OPENING || app->state == HOST_OPEN)
        {
          host_cancel(app);
        }
    }

  host_cancel(&h->next);
  host_one_warm(h);
}

int host_event_to(FAR struct host_app_s *app, FAR const pnut_ui_event_t *ev)
{
  if (!app->connected)
    {
      return -ENOTCONN;
    }

  if (app->count == HOST_EVENTS)
    {
      host_drop(app, PNUT_STATUS_BUSY);
      return -ENOSPC;
    }

  app->queue[(app->head + app->count) % HOST_EVENTS] = *ev;
  app->count++;

  /* An answer owed to what the open app is sent */

  if (app->state == HOST_OPEN &&
      (ev->which_kind == PNUT_UI_EVENT_WELCOME_TAG ||
       ev->which_kind == PNUT_UI_EVENT_SHOWN_TAG ||
       ev->which_kind == PNUT_UI_EVENT_PAGE_TAG ||
       (ev->which_kind == PNUT_UI_EVENT_INPUT_TAG &&
        ev->kind.input.action == PNUT_UI_INPUT_ACTION_BACK)))
    {
      host_owe(app);
    }

  host_flush(app);
  return 0;
}

int host_event(FAR struct host_s *h, FAR const pnut_ui_event_t *ev)
{
  FAR struct host_app_s *app = host_app(h);

  return app != NULL ? host_event_to(app, ev) : -ENOTCONN;
}

FAR struct host_app_s *host_app(FAR struct host_s *h)
{
  int i;

  for (i = 0; i < HOST_APPS; i++)
    {
      if (h->slots[i].state == HOST_OPEN)
        {
          return &h->slots[i];
        }
    }

  return NULL;
}
