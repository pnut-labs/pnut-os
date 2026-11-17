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
 * comes.  Events go down Open's stream as the app's credit allows; an app
 * that leaves HOST_EVENTS of them waiting is taken off.  An app's text is
 * never logged.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
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

/* The app is gone: its screen off if it was open, its slot free */

static void host_forget(FAR struct host_app_s *app)
{
  FAR struct host_s *h = app->host;
  bool open = app->state == HOST_OPEN;

  if (app->busy != NULL)
    {
      pnut_timer_cancel(h->loop, app->busy);
    }

  if (app->due != NULL)
    {
      pnut_timer_cancel(h->loop, app->due);
    }

  if (app->connected && app->stream.open)
    {
      pnut_stream_end(&app->stream, PNUT_STATUS_UNAVAILABLE);
    }

  app_opening_gone(h->ui, app);
  memset(app, 0, sizeof(*app));
  app->host = h;
  if (open)
    {
      app_gone(h->ui);
    }
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

/* Nothing drawn since the app was started: the UI's Busy */

static void host_busy_due(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct host_app_s *app = arg;

  app->busy = NULL;
  if (!app->drawn &&
      (app->state == HOST_OPENING || app->state == HOST_OPEN))
    {
      app_opening(app->host->ui, app);
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

  pnut_log(&h->ui->module, PNUT_LOG_WARNING, "app %s did not stop",
           app->id);

  /* Service states stops the task by its name: never a shell's program,
   * nor a task of a name another slot holds alive
   */

  if (!app->shell && host_twin(app) == NULL)
    {
      host_stop_task(h, app->id);
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

  app_opening_gone(h->ui, app);
  if (app->state == HOST_OPEN)
    {
      app_gone(h->ui);
    }

  app->state = HOST_STOPPING;
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
      host_stop_task(h, app->id);
      host_forget(app);
    }
}

/* Take an app off, ending its stream with a status */

static void host_drop(FAR struct host_app_s *app, int status)
{
  FAR struct host_s *h = app->host;

  if (app->state == HOST_FREE)
    {
      return;
    }

  pnut_log(&h->ui->module, PNUT_LOG_INFO, "app %s %s", app->id,
           status == PNUT_STATUS_ENDED ? "left" :
           status == PNUT_STATUS_BUSY ? "taken off: events wait" :
           "taken off: an event was not sent");
  if (app->connected && app->stream.open)
    {
      pnut_stream_end(&app->stream, status);
    }

  host_forget(app);
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
          return;                 /* No credit: ready comes with it */
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
    }
}

static void host_ready(FAR struct pnut_stream_s *stream, FAR void *arg)
{
  host_flush(arg);
}

/* The app ended its stream, or its connection went */

static void host_closed(FAR struct pnut_stream_s *stream, int status,
                        FAR void *arg)
{
  FAR struct host_app_s *app = arg;

  if (app->state != HOST_FREE)
    {
      pnut_log(&app->host->ui->module, PNUT_LOG_INFO, "app %s gone",
               app->id);
      host_forget(app);
    }
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
          h->slots[i].state = HOST_WARM;
          app_gone(h->ui);
        }
    }

  app->state = HOST_OPEN;
  app->order = ++h->orders;
  host_one_warm(h);
}

static void host_welcome(FAR struct host_app_s *app)
{
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
      /* Typed in a developer's shell: under the id it gives, with its
       * manifest if there is one, every template if not
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
      if (ret == -ENOENT)
        {
          manifest_any(&manifest);
        }
      else if (ret == -EINVAL)
        {
          host_refuse(h, PNUT_STATUS_DENIED, PNUT_UI_ERROR_CODE_NOT_AN_APP,
                      "its manifest is not one");
          return;
        }
      else if (ret < 0)
        {
          host_refuse(h, PNUT_STATUS_UNAVAILABLE, PNUT_UI_ERROR_CODE_NONE,
                      "its manifest cannot be read");
          return;
        }
    }
  else
    {
      host_refuse(h, PNUT_STATUS_DENIED, PNUT_UI_ERROR_CODE_NOT_AN_APP,
                  "not an app");
      return;
    }

  /* A native app the system UI started is opening; any other task of a
   * native app is told to stop (RFC 0037: after the system UI's own
   * restart, an app's Open is answered with Stop); a shell's program
   * opens as it comes
   */

  app = host_find(h, id);
  if (!shell && (app == NULL || app->state != HOST_OPENING))
    {
      app = host_free_slot(h);
      if (app == NULL || host_accept(app) < 0)
        {
          host_refuse(h, PNUT_STATUS_BUSY, PNUT_UI_ERROR_CODE_ANOTHER_APP,
                      "no room for another app");
          return;
        }

      strlcpy(app->id, id, sizeof(app->id));
      pnut_log(&h->ui->module, PNUT_LOG_INFO,
               "app %s not started here: told to stop", id);
      host_stop(app);
      return;
    }

  if (shell)
    {
      app = app != NULL ? NULL : host_free_slot(h);
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
          host_stop_task(h, app->id);
          host_forget(app);
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

  ret = app_show(h->ui, in);
  if (ret == 0 && !in->sheet)
    {
      app->drawn = true;
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

  if (host_open_from(server, req, h) != NULL)
    {
      host_answer(server, req, pnut_ui_update_reply,
                  app_update(h->ui, in));
    }
}

static void host_rows(FAR struct pnut_ui_server_s *server,
                      FAR const struct pnut_request_s *req,
                      FAR const pnut_ui_rows_request_t *in, FAR void *arg)
{
  FAR struct host_s *h = arg;

  if (host_open_from(server, req, h) != NULL)
    {
      host_answer(server, req, pnut_ui_rows_reply, app_rows(h->ui, in));
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

  host_drop(app, PNUT_STATUS_ENDED);
  host_answer(server, req, pnut_ui_leave_reply, 0);
}

static const struct pnut_ui_handlers_s g_host_handlers =
{
  .open   = host_open_app,
  .show   = host_show,
  .update = host_update,
  .rows   = host_rows,
  .leave  = host_leave
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
  for (i = 0; i < HOST_APPS; i++)
    {
      h->slots[i].host = h;
    }

  ret = pnut_services_connect(loop, services, NULL, NULL, &h->services);
  if (ret < 0)
    {
      return ret;
    }

  h->services_open = true;
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
  FAR struct host_app_s *app;
  struct manifest_s manifest;
  pnut_services_name_request_t in;
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
      if (stopping)
        {
          return -EBUSY;          /* Its task still exits */
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
   * start, is cancelled
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

  app = host_free_slot(h);
  if (app == NULL)
    {
      return -EBUSY;
    }

  app->state    = HOST_OPENING;
  app->manifest = manifest;
  app->start    = ++h->starts;
  strlcpy(app->id, id, sizeof(app->id));

  memset(&in, 0, sizeof(in));
  strlcpy(in.name, id, sizeof(in.name));
  ret = pnut_services_start(&h->services, &in, 0, host_started,
                            (FAR void *)(uintptr_t)app->start);
  if (ret >= 0)
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

void host_cancel(FAR struct host_app_s *app)
{
  FAR struct host_s *h = app->host;

  if (app->state == HOST_FREE || app->state == HOST_STOPPING)
    {
      return;
    }

  pnut_log(&h->ui->module, PNUT_LOG_INFO, "app %s cancelled", app->id);
  if (!app->connected)
    {
      host_stop_task(h, app->id);
      host_forget(app);
      return;
    }

  host_stop(app);                 /* It has opened */
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
          app->state = HOST_WARM;
          app_gone(h->ui);
        }
      else if (app->state == HOST_OPENING || app->state == HOST_OPEN)
        {
          host_cancel(app);
        }
    }

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
