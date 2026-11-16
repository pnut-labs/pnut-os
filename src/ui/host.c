/****************************************************************************
 * pnut-os/src/ui/host.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The system UI's service for apps (RFC 0037): an app opens it, is named
 * by Service states' Who from its connection's task, and may show the
 * templates its manifest lists.  Events go down Open's stream as the
 * app's credit allows; an app that leaves HOST_EVENTS of them waiting is
 * taken off.  One app at a time, until the life cycle (RFC 0037, step 2).
 * An app's text is never logged.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <string.h>

#include <pnut/log.h>
#include <pnut/msg.h>

#include "app.h"
#include "host.h"
#include "ui.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void host_flush(FAR struct host_s *h);

/* Whether a request comes from the open app's connection */

static bool host_from_app(FAR struct host_s *h,
                          FAR const struct pnut_request_s *req)
{
  return h->app.used && h->app.stream.open &&
         req->from.conn == h->app.stream.to.conn &&
         req->from.gen == h->app.stream.to.gen;
}

/* The app is no longer open: its screens off, its events dropped */

static void host_forget(FAR struct host_s *h)
{
  if (h->retry != NULL)
    {
      pnut_timer_cancel(h->loop, h->retry);
      h->retry = NULL;
    }

  app_gone(h->ui);
  memset(&h->app, 0, sizeof(h->app));
}

/* Take the app off, ending its stream with a status */

static void host_drop(FAR struct host_s *h, int status)
{
  if (!h->app.used)
    {
      return;
    }

  pnut_log(&h->ui->module, PNUT_LOG_INFO, "app %s %s", h->app.id,
           status == PNUT_STATUS_ENDED ? "left" :
           status == PNUT_STATUS_BUSY ? "taken off: events wait" :
           "taken off: an event was not sent");
  if (h->app.stream.open)
    {
      pnut_stream_end(&h->app.stream, status);
    }

  host_forget(h);
}

static void host_retry(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct host_s *h = arg;

  h->retry = NULL;
  host_flush(h);
}

/* Send what waits, as credit allows */

static void host_flush(FAR struct host_s *h)
{
  int ret;

  while (h->app.used && h->app.count > 0)
    {
      ret = pnut_ui_open_push(&h->server, &h->app.stream,
                              &h->app.queue[h->app.head]);
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

          host_drop(h, PNUT_STATUS_INTERNAL);
          return;
        }

      h->app.head = (h->app.head + 1) % HOST_EVENTS;
      h->app.count--;
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
  FAR struct host_s *h = arg;

  if (h->app.used)
    {
      pnut_log(&h->ui->module, PNUT_LOG_INFO, "app %s gone", h->app.id);
      host_forget(h);
    }
}

static void host_refuse(FAR struct host_s *h, int status, uint32_t code,
                        FAR const char *why)
{
  pnut_ui_fail(&h->server, &h->asked, status, code, why);
}

/* Who answered: the app is named, or refused */

static void host_who(FAR struct pnut_services_client_s *client, int status,
                     FAR const pnut_services_who_reply_t *out,
                     FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct host_s *h = arg;
  FAR struct host_app_s *app = &h->app;
  pnut_ui_event_t ev;
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

  memset(app, 0, sizeof(*app));
  if (out->name[0] != '\0')
    {
      /* A service's: its name is the app's id, and it has a manifest.  A
       * system service ("ui", "telephony") has neither a reverse-DNS name
       * nor a manifest, and is refused.
       */

      strlcpy(app->id, out->name, sizeof(app->id));
      ret = manifest_load(h->apps, app->id, &app->manifest);
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

      strlcpy(app->id, h->claimed, sizeof(app->id));
      app->shell = true;
      ret = manifest_load(h->apps, app->id, &app->manifest);
      if (ret == -ENOENT)
        {
          manifest_any(&app->manifest);
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

  ret = pnut_ui_open_accept(&h->server, &h->asked, &app->stream,
                            host_ready, host_closed, h);
  if (ret < 0)
    {
      memset(app, 0, sizeof(*app));
      return;                     /* It went meanwhile */
    }

  app->used = true;
  pnut_log(&h->ui->module, PNUT_LOG_INFO, "app %s opened%s", app->id,
           app->shell ? " from the developer's shell" : "");

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
  host_event(h, &ev);
}

static void host_open_app(FAR struct pnut_ui_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_ui_open_request_t *in,
                          FAR void *arg)
{
  FAR struct host_s *h = arg;
  pnut_services_who_request_t who;
  int ret;

  /* One app at a time; one being named (Who answers within HOST_WHO_MS)
   * counts
   */

  if (h->asking || h->app.used)
    {
      pnut_ui_fail(server, req, PNUT_STATUS_BUSY,
                   PNUT_UI_ERROR_CODE_ANOTHER_APP,
                   h->asking ? "an app is opening" : "another app is open");
      return;
    }

  h->asked = *req;
  strlcpy(h->claimed, in->app, sizeof(h->claimed));
  h->asking = true;

  memset(&who, 0, sizeof(who));
  who.pid = req->pid;
  ret = pnut_services_who(&h->services, &who, HOST_WHO_MS, host_who, h);
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

static void host_show(FAR struct pnut_ui_server_s *server,
                      FAR const struct pnut_request_s *req,
                      FAR const pnut_ui_show_request_t *in, FAR void *arg)
{
  FAR struct host_s *h = arg;
  int tmpl;

  if (!host_from_app(h, req))
    {
      pnut_ui_fail(server, req, PNUT_STATUS_DENIED,
                   PNUT_UI_ERROR_CODE_NOT_OPEN, "not the open app");
      return;
    }

  tmpl = host_template(in);
  if (tmpl == PNUT_UI_TEMPLATE_UI_TEMPLATE_NONE)
    {
      pnut_ui_fail(server, req, PNUT_STATUS_INVALID,
                   PNUT_UI_ERROR_CODE_UNKNOWN_TEMPLATE, "no template");
      return;
    }

  if (!manifest_allows(&h->app.manifest, tmpl))
    {
      pnut_ui_fail(server, req, PNUT_STATUS_DENIED,
                   PNUT_UI_ERROR_CODE_NOT_LISTED,
                   "a template its manifest does not list");
      return;
    }

  host_answer(server, req, pnut_ui_show_reply, app_show(h->ui, in));
}

static void host_update(FAR struct pnut_ui_server_s *server,
                        FAR const struct pnut_request_s *req,
                        FAR const pnut_ui_update_request_t *in,
                        FAR void *arg)
{
  FAR struct host_s *h = arg;

  if (!host_from_app(h, req))
    {
      pnut_ui_fail(server, req, PNUT_STATUS_DENIED,
                   PNUT_UI_ERROR_CODE_NOT_OPEN, "not the open app");
      return;
    }

  host_answer(server, req, pnut_ui_update_reply, app_update(h->ui, in));
}

static void host_rows(FAR struct pnut_ui_server_s *server,
                      FAR const struct pnut_request_s *req,
                      FAR const pnut_ui_rows_request_t *in, FAR void *arg)
{
  FAR struct host_s *h = arg;

  if (!host_from_app(h, req))
    {
      pnut_ui_fail(server, req, PNUT_STATUS_DENIED,
                   PNUT_UI_ERROR_CODE_NOT_OPEN, "not the open app");
      return;
    }

  host_answer(server, req, pnut_ui_rows_reply, app_rows(h->ui, in));
}

static void host_leave(FAR struct pnut_ui_server_s *server,
                       FAR const struct pnut_request_s *req,
                       FAR const pnut_ui_leave_request_t *in,
                       FAR void *arg)
{
  FAR struct host_s *h = arg;

  if (!host_from_app(h, req))
    {
      pnut_ui_fail(server, req, PNUT_STATUS_DENIED,
                   PNUT_UI_ERROR_CODE_NOT_OPEN, "not the open app");
      return;
    }

  host_drop(h, PNUT_STATUS_ENDED);
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

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int host_open(FAR struct host_s *h, FAR struct ui_s *ui,
              FAR struct pnut_loop_s *loop, FAR const char *apps,
              FAR const char *services)
{
  int ret;

  memset(h, 0, sizeof(*h));
  h->ui   = ui;
  h->loop = loop;
  h->apps = apps;

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
  if (h->app.used)
    {
      if (h->app.stream.open)
        {
          pnut_stream_end(&h->app.stream, PNUT_STATUS_UNAVAILABLE);
        }

      host_forget(h);
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

int host_event(FAR struct host_s *h, FAR const pnut_ui_event_t *ev)
{
  FAR struct host_app_s *app = &h->app;

  if (!app->used)
    {
      return -ENOTCONN;
    }

  if (app->count == HOST_EVENTS)
    {
      host_drop(h, PNUT_STATUS_BUSY);
      return -ENOSPC;
    }

  app->queue[(app->head + app->count) % HOST_EVENTS] = *ev;
  app->count++;
  host_flush(h);
  return 0;
}

FAR struct host_app_s *host_app(FAR struct host_s *h)
{
  return h->app.used ? &h->app : NULL;
}
