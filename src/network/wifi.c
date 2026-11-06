/****************************************************************************
 * pnut-os/src/network/wifi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Wi-Fi (RFC 0031), a module of the network program: the radio up while
 * wifi.enabled is, a scan streamed to whoever asked, a network joined and
 * an address taken, the link watched and a lost network joined again,
 * the state on the wifi topic.  Every call into the driver blocks, so it
 * runs on a worker, one at a time; the loop keeps the state.
 *
 * As the system before this one found on the same hardware: each join
 * starts from a radio taken down and brought up again, since after a
 * failed join every scan and join failed until it was.
 *
 * Known networks, kept with their passphrases in Settings, come with
 * Settings' secrets: until then a passphrase comes with each join, and is
 * kept in memory only, to join a lost network again.  A join asked for
 * that fails is not tried again: the driver cannot tell a wrong
 * passphrase from a network out of reach, so the owner is told.  A lost
 * network, whose passphrase did join it, is tried again, the wait
 * doubling after each failure.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>
#include <pnut/worker.h>

#include "wifi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIFI_UI           "ui"    /* The system UI's program */
#define WIFI_LINK_MS      10000
#define WIFI_AGAIN_MS     30000
#define WIFI_AGAIN_SHIFT  4       /* The wait doubled at most this often */
#define WIFI_KICK_MS      1000    /* A job the worker would not take */

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void wifi_next(FAR struct wifi_s *wifi);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t wifi_ms(uint32_t value, uint32_t fallback)
{
  return value != 0 ? value : fallback;
}

/****************************************************************************
 * Name: wifi_publish
 ****************************************************************************/

static void wifi_publish(FAR struct wifi_s *wifi)
{
  if (wifi->topic != NULL)
    {
      pnut_wifi_info_publish(wifi->topic, &wifi->state);
    }
}

static void wifi_set_state(FAR struct wifi_s *wifi, uint8_t state)
{
  wifi->state.state = state;
  if (state != PNUT_WIFI_INFO_STATE_JOINED)
    {
      wifi->state.signal  = 0;
      wifi->state.address = 0;
    }

  if (state == PNUT_WIFI_INFO_STATE_OFF ||
      state == PNUT_WIFI_INFO_STATE_ON)
    {
      wifi->state.name[0] = '\0';
    }

  wifi_publish(wifi);
}

/****************************************************************************
 * Name: wifi_forget_pass
 *
 * Description:
 *   The passphrase held for joining again, wiped.
 *
 ****************************************************************************/

static void wifi_forget_pass(FAR struct wifi_s *wifi)
{
  explicit_bzero(wifi->passphrase, sizeof(wifi->passphrase));
  wifi->name[0] = '\0';
}

/****************************************************************************
 * Name: wifi_fail
 ****************************************************************************/

static void wifi_fail(FAR struct wifi_s *wifi,
                      FAR const struct pnut_request_s *req, int status,
                      uint32_t code, FAR const char *text)
{
  if (wifi->serving)
    {
      pnut_wifi_fail(&wifi->server, req, status, code, text);
    }
}

/****************************************************************************
 * Name: wifi_join_answer
 *
 * Description:
 *   The join request waiting, if one does, answered: OK, or failed with
 *   a code.
 *
 ****************************************************************************/

static void wifi_join_answer(FAR struct wifi_s *wifi, uint32_t code)
{
  pnut_wifi_reply_t reply;

  if (!wifi->joining_req)
    {
      return;
    }

  wifi->joining_req = false;
  if (code == PNUT_WIFI_ERROR_CODE_NONE)
    {
      memset(&reply, 0, sizeof(reply));
      if (wifi->serving)
        {
          pnut_wifi_join_reply(&wifi->server, &wifi->join_req,
                               PNUT_STATUS_OK, &reply);
        }
    }
  else
    {
      wifi_fail(wifi, &wifi->join_req, PNUT_STATUS_UNAVAILABLE, code,
                code == PNUT_WIFI_ERROR_CODE_CANCELLED ?
                "left before it joined" : "the join failed");
    }
}

/* Whether a join runs on the worker now */

static bool wifi_joining(FAR struct wifi_s *wifi)
{
  return wifi->working && wifi->work.job == WIFI_JOB_JOIN;
}

/****************************************************************************
 * Name: wifi_scan_push
 *
 * Description:
 *   The scan's networks to its stream, as its credit allows; then its end.
 *
 ****************************************************************************/

static void wifi_scan_push(FAR struct wifi_s *wifi)
{
  pnut_wifi_network_t item;
  FAR struct wifi_found_s *f;

  if (!wifi->scan.open || !wifi->scanned)
    {
      return;
    }

  while (wifi->pushed < wifi->nfound)
    {
      f = &wifi->found[wifi->pushed];
      memset(&item, 0, sizeof(item));
      strlcpy(item.name, f->name, sizeof(item.name));
      item.signal   = f->signal;
      item.channel  = f->channel;
      item.security = f->secured ? PNUT_WIFI_SECURITY_WIFI_SECURITY_SECURED :
                      PNUT_WIFI_SECURITY_WIFI_SECURITY_OPEN;
      if (pnut_wifi_scan_push(&wifi->server, &wifi->scan, &item) < 0)
        {
          return;                 /* No credit: ready() goes on */
        }

      wifi->pushed++;
    }

  pnut_stream_end(&wifi->scan, PNUT_STATUS_ENDED);
  wifi->scanned = false;
}

/* Credit came: the rest; a stream over may let the radio down */

static void wifi_scan_ready(FAR struct pnut_stream_s *stream, FAR void *arg)
{
  wifi_scan_push(arg);
  wifi_next(arg);
}

static void wifi_scan_closed(FAR struct pnut_stream_s *stream, int status,
                             FAR void *arg)
{
  FAR struct wifi_s *wifi = arg;

  /* The caller went: the scan runs on, its networks for nobody */

  wifi->scanned = false;
  wifi_next(wifi);
}

/****************************************************************************
 * Name: wifi_run
 *
 * Description:
 *   A job, on a worker: the driver's calls, which block.
 *
 ****************************************************************************/

static void wifi_run(FAR void *arg)
{
  FAR struct wifi_work_s *w = arg;
  FAR const struct wifi_ops_s *ops = w->ops;
  int n;

  w->ret    = OK;
  w->radio  = w->up;
  w->linked = false;
  switch (w->job)
    {
      case WIFI_JOB_UP:
        w->ret   = ops->up(w->arg);
        w->radio = w->ret >= 0;
        break;

      case WIFI_JOB_DOWN:
        ops->down(w->arg);
        w->radio = false;
        break;

      case WIFI_JOB_SCAN:
        if (!w->up && ops->up(w->arg) < 0)
          {
            w->ret = -ENODEV;     /* The radio would not come up */
            break;
          }

        n = ops->scan(w->arg, w->found, WIFI_SCAN_MAX);
        w->ret    = n < 0 ? n : OK;
        w->nfound = n < 0 ? 0 : n;
        w->radio  = true;
        if (!w->keep)
          {
            ops->down(w->arg);
            w->radio = false;
          }

        break;

      case WIFI_JOB_JOIN:

        /* From a radio brought up afresh: one left from a failed join
         * fails every join after it
         */

        ops->down(w->arg);
        w->radio = false;
        if (ops->up(w->arg) < 0)
          {
            w->ret = -ENODEV;
            break;
          }

        w->radio = true;
        w->ret   = ops->join(w->arg, w->name, w->passphrase, &w->signal);
        if (w->ret == OK)
          {
            w->ret = ops->address(w->arg, &w->addr);
            w->linked = w->ret == OK;
          }

        if (w->ret < 0)
          {
            /* Not left as a failed join leaves it */

            ops->down(w->arg);
            w->radio = false;
          }

        break;

      case WIFI_JOB_LINK:
        w->linked = ops->linked(w->arg, &w->signal);
        break;

      default:
        break;
    }

  explicit_bzero(w->passphrase, sizeof(w->passphrase));
}

/****************************************************************************
 * Name: wifi_link_timer, wifi_again_timer
 ****************************************************************************/

static void wifi_link_timer(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct wifi_s *wifi = arg;

  wifi->link     = NULL;
  wifi->link_due = true;
  wifi_next(wifi);
}

static void wifi_kick_timer(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct wifi_s *wifi = arg;

  wifi->kick = NULL;
  wifi_next(wifi);
}

static void wifi_again_timer(FAR struct pnut_loop_s *loop,
                             FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct wifi_s *wifi = arg;

  wifi->again = NULL;
  if (wifi->enabled && wifi->name[0] != '\0' &&
      wifi->state.state == PNUT_WIFI_INFO_STATE_ON)
    {
      wifi->join_wanted = true;
      wifi_next(wifi);
    }
}

static void wifi_timer(FAR struct wifi_s *wifi,
                       FAR struct pnut_timer_s **timer, uint32_t ms,
                       pnut_timer_handler_t cb)
{
  int ret;

  if (*timer != NULL)
    {
      return;
    }

  ret = pnut_timer_start(wifi->module.loop, ms, 0, cb, wifi, timer);
  if (ret < 0)
    {
      pnut_log(&wifi->module, PNUT_LOG_WARNING, "No timer: %d", ret);
      *timer = NULL;
    }
}

static void wifi_cancel(FAR struct wifi_s *wifi,
                        FAR struct pnut_timer_s **timer)
{
  if (*timer != NULL)
    {
      pnut_timer_cancel(wifi->module.loop, *timer);
      *timer = NULL;
    }
}

/* The wait before a lost network is tried again, doubled per failure */

static uint32_t wifi_again_ms(FAR struct wifi_s *wifi)
{
  uint8_t shift = wifi->tries < WIFI_AGAIN_SHIFT ? wifi->tries :
                  WIFI_AGAIN_SHIFT;

  return wifi_ms(wifi->config.again_ms, WIFI_AGAIN_MS) << shift;
}

/****************************************************************************
 * Name: wifi_done
 *
 * Description:
 *   A job's end, on the loop: the state follows, the request waiting is
 *   answered, the scan's networks go out.
 *
 ****************************************************************************/

static void wifi_done(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct wifi_s *wifi =
    (FAR struct wifi_s *)((FAR char *)arg - offsetof(struct wifi_s, work));
  FAR struct wifi_work_s *w = &wifi->work;
  uint8_t job = w->job;
  uint32_t code;
  bool cancelled;
  int i;

  /* What follows may start nothing: wifi_next() at the end does */

  wifi->working = false;
  wifi->in_done = true;
  wifi->radio   = w->radio;
  w->job        = WIFI_JOB_NONE;
  if (w->radio)
    {
      wifi->radio_failed = false;
    }

  switch (job)
    {
      case WIFI_JOB_UP:
        if (w->ret < 0)
          {
            /* Not tried again on its own: the owner may turn Wi-Fi off
             * and on, or ask for a scan or a join
             */

            pnut_log(&wifi->module, PNUT_LOG_ERROR,
                     "The radio would not come up: %d", w->ret);
            wifi->radio_failed  = true;
            wifi->state.failure = PNUT_WIFI_ERROR_CODE_NO_RADIO;
            wifi_set_state(wifi, PNUT_WIFI_INFO_STATE_OFF);
          }

        break;

      case WIFI_JOB_DOWN:
        wifi_set_state(wifi, wifi->enabled ? PNUT_WIFI_INFO_STATE_ON :
                       PNUT_WIFI_INFO_STATE_OFF);
        break;

      case WIFI_JOB_SCAN:
        wifi->nfound = w->nfound;
        for (i = 0; i < w->nfound; i++)
          {
            wifi->found[i] = w->found[i];
          }

        wifi->pushed  = 0;
        wifi->scanned = wifi->scan.open;
        if (w->ret < 0)
          {
            pnut_log(&wifi->module, PNUT_LOG_WARNING, "A scan failed: %d",
                     w->ret);
            if (w->ret == -ENODEV && wifi->enabled)
              {
                wifi->radio_failed  = true;
                wifi->state.failure = PNUT_WIFI_ERROR_CODE_NO_RADIO;
                wifi_set_state(wifi, PNUT_WIFI_INFO_STATE_OFF);
              }

            if (wifi->scan.open)
              {
                pnut_stream_end(&wifi->scan, PNUT_STATUS_UNAVAILABLE);
              }

            wifi->scanned = false;
          }

        wifi_scan_push(wifi);
        break;

      case WIFI_JOB_JOIN:

        /* Wi-Fi turned off, or left, while it ran: the request is told,
         * and the radio goes down next
         */

        cancelled = !wifi->enabled || wifi->leave_wanted;
        if (cancelled)
          {
            if (w->ret == OK)
              {
                pnut_log(&wifi->module, PNUT_LOG_INFO,
                         "Joined \"%s\", left at once", w->name);
              }

            wifi_set_state(wifi, wifi->enabled ? PNUT_WIFI_INFO_STATE_ON :
                           PNUT_WIFI_INFO_STATE_OFF);
            wifi_join_answer(wifi, wifi->enabled ?
                             PNUT_WIFI_ERROR_CODE_CANCELLED :
                             PNUT_WIFI_ERROR_CODE_OFF);
          }
        else if (w->ret == OK)
          {
            wifi->state.signal  = w->signal;
            wifi->state.address = w->addr;
            wifi->state.failure = PNUT_WIFI_ERROR_CODE_NONE;
            strlcpy(wifi->state.name, w->name, sizeof(wifi->state.name));
            pnut_log(&wifi->module, PNUT_LOG_INFO, "Joined \"%s\", %d dBm",
                     w->name, w->signal);
            wifi->tries    = 0;
            wifi->link_due = false;
            wifi_set_state(wifi, PNUT_WIFI_INFO_STATE_JOINED);
            wifi_timer(wifi, &wifi->link,
                       wifi_ms(wifi->config.link_ms, WIFI_LINK_MS),
                       wifi_link_timer);
            wifi_join_answer(wifi, PNUT_WIFI_ERROR_CODE_NONE);
          }
        else
          {
            code = w->ret == -EADDRNOTAVAIL || w->ret == -EAGAIN ?
                   PNUT_WIFI_ERROR_CODE_NO_ADDRESS :
                   w->ret == -ENOENT ? PNUT_WIFI_ERROR_CODE_NOT_FOUND :
                   w->ret == -ENODEV || w->ret == -EIO ?
                   PNUT_WIFI_ERROR_CODE_NO_RADIO :
                   PNUT_WIFI_ERROR_CODE_REFUSED;
            pnut_log(&wifi->module, PNUT_LOG_WARNING,
                     "Joining \"%s\" failed: %d", w->name, w->ret);
            wifi->state.failure = code;
            wifi->radio_failed  = code == PNUT_WIFI_ERROR_CODE_NO_RADIO;
            wifi_set_state(wifi, wifi->radio_failed ?
                           PNUT_WIFI_INFO_STATE_OFF :
                           PNUT_WIFI_INFO_STATE_ON);

            /* A network asked for: the owner is told, and it is not
             * tried again until asked.  A lost one: again, later.
             */

            if (wifi->joining_req)
              {
                wifi_forget_pass(wifi);
                wifi_join_answer(wifi, code);
              }
            else
              {
                wifi_timer(wifi, &wifi->again, wifi_again_ms(wifi),
                           wifi_again_timer);
                if (wifi->tries < UINT8_MAX)
                  {
                    wifi->tries++;
                  }
              }
          }

        break;

      case WIFI_JOB_LINK:
        if (wifi->state.state != PNUT_WIFI_INFO_STATE_JOINED)
          {
            break;
          }

        if (w->linked)
          {
            if (w->signal != wifi->state.signal)
              {
                wifi->state.signal = w->signal;
                wifi_publish(wifi);
              }

            wifi_timer(wifi, &wifi->link,
                       wifi_ms(wifi->config.link_ms, WIFI_LINK_MS),
                       wifi_link_timer);
          }
        else
          {
            pnut_log(&wifi->module, PNUT_LOG_INFO, "\"%s\" lost",
                     wifi->state.name);
            wifi_set_state(wifi, PNUT_WIFI_INFO_STATE_ON);
            wifi->join_wanted = wifi->name[0] != '\0';
          }

        break;

      default:
        break;
    }

  /* On, the radio up (by any job): no longer off for want of it */

  if (w->radio && wifi->enabled &&
      wifi->state.state == PNUT_WIFI_INFO_STATE_OFF)
    {
      if (wifi->state.failure == PNUT_WIFI_ERROR_CODE_NO_RADIO)
        {
          wifi->state.failure = PNUT_WIFI_ERROR_CODE_NONE;
        }

      wifi_set_state(wifi, PNUT_WIFI_INFO_STATE_ON);
    }

  wifi->in_done = false;
  wifi_next(wifi);
}

/****************************************************************************
 * Name: wifi_submit
 ****************************************************************************/

static int wifi_submit(FAR struct wifi_s *wifi, uint8_t job)
{
  FAR struct wifi_work_s *w = &wifi->work;
  int ret;

  w->ops  = wifi->config.ops;
  w->arg  = wifi->config.arg;
  w->job  = job;
  w->up   = wifi->radio;
  w->keep = wifi->enabled;
  if (job == WIFI_JOB_JOIN)
    {
      strlcpy(w->name, wifi->name, sizeof(w->name));
      strlcpy(w->passphrase, wifi->passphrase, sizeof(w->passphrase));
    }

  ret = pnut_job_submit(wifi->module.loop, wifi_run, wifi_done, w);
  if (ret < 0)
    {
      /* Never on the loop: the driver's calls block for seconds */

      explicit_bzero(w->passphrase, sizeof(w->passphrase));
      w->job = WIFI_JOB_NONE;
      return ret;
    }

  wifi->working = true;
  return OK;
}

/****************************************************************************
 * Name: wifi_next
 *
 * Description:
 *   The next job, once none runs: off first, then leaving, a join, a
 *   scan, the radio up, a look at the link.
 *
 ****************************************************************************/

static void wifi_next(FAR struct wifi_s *wifi)
{
  FAR bool *wanted = NULL;
  uint8_t job;
  int ret;

  if (wifi->working || wifi->in_done || wifi->config.ops == NULL)
    {
      return;
    }

  if (!wifi->enabled && wifi->radio && !wifi->scan.open)
    {
      job    = WIFI_JOB_DOWN;
      wanted = &wifi->leave_wanted;   /* Left, too, once down */
    }
  else if (wifi->leave_wanted)
    {
      job    = WIFI_JOB_DOWN;
      wanted = &wifi->leave_wanted;
    }
  else if (wifi->join_wanted && wifi->enabled)
    {
      job    = WIFI_JOB_JOIN;
      wanted = &wifi->join_wanted;
    }
  else if (wifi->scan_wanted)
    {
      job    = WIFI_JOB_SCAN;
      wanted = &wifi->scan_wanted;
    }
  else if (wifi->enabled && !wifi->radio && !wifi->radio_failed)
    {
      job = WIFI_JOB_UP;
    }
  else if (wifi->state.state == PNUT_WIFI_INFO_STATE_JOINED &&
           wifi->link_due)
    {
      job    = WIFI_JOB_LINK;
      wanted = &wifi->link_due;
    }
  else
    {
      return;
    }

  ret = wifi_submit(wifi, job);
  if (ret < 0)
    {
      pnut_log(&wifi->module, PNUT_LOG_WARNING,
               "The worker took no job: %d; again in a second", ret);
      wifi_timer(wifi, &wifi->kick, WIFI_KICK_MS, wifi_kick_timer);
      return;
    }

  if (wanted != NULL)
    {
      *wanted = false;
    }

  if (job == WIFI_JOB_JOIN)
    {
      strlcpy(wifi->state.name, wifi->name, sizeof(wifi->state.name));
      wifi_set_state(wifi, PNUT_WIFI_INFO_STATE_JOINING);
    }
}

/****************************************************************************
 * Name: wifi_do_scan, wifi_do_join, wifi_do_leave
 *
 * Description:
 *   A request from the system UI or the shell, once Service states said
 *   it is.
 *
 ****************************************************************************/

static void wifi_do_scan(FAR struct wifi_s *wifi,
                         FAR const struct pnut_request_s *req)
{
  int ret;

  if (wifi->scan.open)
    {
      wifi_fail(wifi, req, PNUT_STATUS_BUSY, PNUT_WIFI_ERROR_CODE_NONE,
                "a scan runs");
      return;
    }

  ret = pnut_wifi_scan_accept(&wifi->server, req, &wifi->scan,
                              wifi_scan_ready, wifi_scan_closed, wifi);
  if (ret < 0)
    {
      return;
    }

  wifi->scanned     = false;
  wifi->scan_wanted = true;
  wifi_next(wifi);
}

static void wifi_do_join(FAR struct wifi_s *wifi,
                         FAR const struct pnut_request_s *req,
                         FAR const pnut_wifi_join_request_t *in)
{
  size_t len = strnlen(in->passphrase, sizeof(in->passphrase));

  if (!wifi->enabled)
    {
      wifi_fail(wifi, req, PNUT_STATUS_UNAVAILABLE, PNUT_WIFI_ERROR_CODE_OFF,
                "Wi-Fi is off");
      return;
    }

  if (in->name[0] == '\0' || (len > 0 && (len < 8 || len > 63)))
    {
      wifi_fail(wifi, req, PNUT_STATUS_INVALID, PNUT_WIFI_ERROR_CODE_NONE,
                "a name, and a passphrase of 8 to 63 characters or none");
      return;
    }

  /* One join at a time, a lost network's too: its end would answer this
   * request
   */

  if (wifi->joining_req || wifi_joining(wifi))
    {
      wifi_fail(wifi, req, PNUT_STATUS_BUSY, PNUT_WIFI_ERROR_CODE_NONE,
                "a join runs");
      return;
    }

  strlcpy(wifi->name, in->name, sizeof(wifi->name));
  strlcpy(wifi->passphrase, in->passphrase, sizeof(wifi->passphrase));
  wifi->join_req     = *req;
  wifi->joining_req  = true;
  wifi->join_wanted  = true;
  wifi->leave_wanted = false;
  wifi->tries        = 0;
  wifi_cancel(wifi, &wifi->again);
  wifi_next(wifi);
}

/* A join waiting, not running, dropped: its request told; a running
 * one's is told as it ends
 */

static void wifi_drop_join(FAR struct wifi_s *wifi, uint32_t code)
{
  wifi_forget_pass(wifi);
  wifi->join_wanted = false;
  wifi->tries       = 0;
  wifi_cancel(wifi, &wifi->again);
  if (!wifi_joining(wifi))
    {
      wifi_join_answer(wifi, code);
    }
}

static void wifi_do_leave(FAR struct wifi_s *wifi,
                          FAR const struct pnut_request_s *req)
{
  pnut_wifi_reply_t reply;

  wifi_drop_join(wifi, PNUT_WIFI_ERROR_CODE_CANCELLED);
  wifi->leave_wanted = wifi->radio || wifi_joining(wifi);
  if (wifi->state.state == PNUT_WIFI_INFO_STATE_JOINED)
    {
      wifi_cancel(wifi, &wifi->link);
      wifi->link_due = false;
    }

  memset(&reply, 0, sizeof(reply));
  pnut_wifi_leave_reply(&wifi->server, req, PNUT_STATUS_OK, &reply);
  wifi_next(wifi);
}

/****************************************************************************
 * Name: wifi_who, wifi_ask
 *
 * Description:
 *   Who may call (RFC 0031): the system UI, and a command typed in the
 *   developer's shell; Service states says who sent a request.
 *
 ****************************************************************************/

static void wifi_dispatch(FAR struct wifi_s *wifi,
                          FAR struct wifi_ask_s *ask)
{
  if (ask->method == PNUT_WIFI_SCAN)
    {
      wifi_do_scan(wifi, &ask->req);
    }
  else if (ask->method == PNUT_WIFI_JOIN)
    {
      wifi_do_join(wifi, &ask->req, &ask->join);
    }
  else
    {
      wifi_do_leave(wifi, &ask->req);
    }

  explicit_bzero(&ask->join, sizeof(ask->join));
}

static void wifi_who(FAR struct pnut_services_client_s *client, int status,
                     FAR const pnut_services_who_reply_t *out,
                     FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct wifi_ask_s *ask = arg;
  FAR struct wifi_s *wifi = ask->wifi;

  ask->used = false;
  if (status == PNUT_STATUS_UNAVAILABLE || status == PNUT_STATUS_TIMEOUT)
    {
      wifi_fail(wifi, &ask->req, PNUT_STATUS_UNAVAILABLE,
                PNUT_WIFI_ERROR_CODE_DENIED, "who calls cannot be told now");
      explicit_bzero(&ask->join, sizeof(ask->join));
      return;
    }

  if (status != PNUT_STATUS_OK || out == NULL ||
      (strcmp(out->name, WIFI_UI) != 0 && !out->shell))
    {
      wifi_fail(wifi, &ask->req, PNUT_STATUS_DENIED,
                PNUT_WIFI_ERROR_CODE_DENIED,
                "for the system UI and the developer's shell");
      explicit_bzero(&ask->join, sizeof(ask->join));
      return;
    }

  wifi_dispatch(wifi, ask);
}

static void wifi_ask(FAR struct wifi_s *wifi, uint8_t method,
                     FAR const struct pnut_request_s *req,
                     FAR const pnut_wifi_join_request_t *join)
{
  pnut_services_who_request_t who;
  FAR struct wifi_ask_s *ask = NULL;
  int ret;
  int i;

  for (i = 0; i < 2 && ask == NULL; i++)
    {
      if (!wifi->asks[i].used)
        {
          ask = &wifi->asks[i];
        }
    }

  if (ask == NULL)
    {
      wifi_fail(wifi, req, PNUT_STATUS_BUSY, PNUT_WIFI_ERROR_CODE_NONE,
                "too many requests wait");
      return;
    }

  memset(ask, 0, sizeof(*ask));
  ask->wifi   = wifi;
  ask->method = method;
  ask->req    = *req;
  if (join != NULL)
    {
      ask->join = *join;
    }

  if (wifi->config.services == NULL)
    {
      wifi_dispatch(wifi, ask);
      return;
    }

  memset(&who, 0, sizeof(who));
  who.pid = req->pid;
  ask->used = true;
  ret = wifi->services_open ?
        pnut_services_who(&wifi->services, &who, 0, wifi_who, ask) :
        -ENOTCONN;
  if (ret < 0)
    {
      ask->used = false;
      explicit_bzero(&ask->join, sizeof(ask->join));
      wifi_fail(wifi, req, PNUT_STATUS_UNAVAILABLE,
                PNUT_WIFI_ERROR_CODE_DENIED, "who calls cannot be told now");
    }
}

static void wifi_on_scan(FAR struct pnut_wifi_server_s *server,
                         FAR const struct pnut_request_s *req,
                         FAR const pnut_wifi_scan_request_t *in,
                         FAR void *arg)
{
  wifi_ask(arg, PNUT_WIFI_SCAN, req, NULL);
}

static void wifi_on_join(FAR struct pnut_wifi_server_s *server,
                         FAR const struct pnut_request_s *req,
                         FAR const pnut_wifi_join_request_t *in,
                         FAR void *arg)
{
  wifi_ask(arg, PNUT_WIFI_JOIN, req, in);
}

static void wifi_on_leave(FAR struct pnut_wifi_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_wifi_leave_request_t *in,
                          FAR void *arg)
{
  wifi_ask(arg, PNUT_WIFI_LEAVE, req, NULL);
}

static const struct pnut_wifi_handlers_s g_wifi_handlers =
{
  .scan  = wifi_on_scan,
  .join  = wifi_on_join,
  .leave = wifi_on_leave,
};

/****************************************************************************
 * Name: wifi_settings
 *
 * Description:
 *   The settings were read, or one changed: the radio follows
 *   wifi.enabled.
 *
 ****************************************************************************/

static void wifi_settings(FAR struct pnut_settings_cache_s *cache,
                          int index, FAR void *arg)
{
  FAR struct wifi_s *wifi = arg;
  bool enabled = wifi->settings.values.enabled;

  if (enabled == wifi->enabled)
    {
      return;
    }

  wifi->enabled = enabled;
  pnut_log(&wifi->module, PNUT_LOG_INFO, "Wi-Fi %s",
           enabled ? "on" : "off");
  if (!enabled)
    {
      /* Off on the topic at once, for its readers: the radio goes down
       * after a job that holds it, a scan, has ended
       */

      wifi_drop_join(wifi, PNUT_WIFI_ERROR_CODE_OFF);
      wifi_cancel(wifi, &wifi->link);
      wifi->link_due = false;

      /* A network joined, or being joined, is left even if Wi-Fi is on
       * again before the radio goes down: its passphrase is gone
       */

      if (wifi->state.state == PNUT_WIFI_INFO_STATE_JOINED ||
          wifi_joining(wifi))
        {
          wifi->leave_wanted = true;
        }

      wifi_set_state(wifi, PNUT_WIFI_INFO_STATE_OFF);
    }
  else
    {
      wifi->radio_failed = false;   /* Off and on: tried again */

      /* On again before the radio went down */

      if (wifi->radio && wifi->state.state == PNUT_WIFI_INFO_STATE_OFF)
        {
          wifi_set_state(wifi, PNUT_WIFI_INFO_STATE_ON);
        }
    }

  wifi_next(wifi);
}

/****************************************************************************
 * Name: wifi_start, wifi_stop
 ****************************************************************************/

static int wifi_start(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct wifi_s *wifi = (FAR struct wifi_s *)module;
  int ret;

  ret = pnut_wifi_info_advertise(loop, &wifi->topic);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No wifi topic: %d", ret);
      wifi->topic = NULL;
    }

  wifi_publish(wifi);

  ret = pnut_wifi_serve(loop, WIFI_SERVICE, 0, &g_wifi_handlers, wifi,
                        &wifi->server);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Cannot serve %s: %d", WIFI_SERVICE,
               ret);
      return ret;
    }

  wifi->serving = true;
  if (wifi->config.services != NULL)
    {
      ret = pnut_services_connect(loop, wifi->config.services, NULL, NULL,
                                  &wifi->services);
      if (ret < 0)
        {
          pnut_log(module, PNUT_LOG_WARNING,
                   "No Service states: %d; callers refused", ret);
        }
      else
        {
          wifi->services_open = true;
        }
    }

  ret = pnut_wifi_settings_open(module, wifi_settings, wifi,
                                &wifi->settings);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No settings: %d; Wi-Fi off",
               ret);
    }
  else
    {
      wifi->settings_open = true;
    }

  pnut_module_ready(module);
  return OK;
}

static void wifi_stop(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct wifi_s *wifi = (FAR struct wifi_s *)module;

  wifi_forget_pass(wifi);
  explicit_bzero(wifi->asks, sizeof(wifi->asks));
  wifi_cancel(wifi, &wifi->link);
  wifi_cancel(wifi, &wifi->again);
  wifi_cancel(wifi, &wifi->kick);

  if (wifi->settings_open)
    {
      pnut_wifi_settings_close(&wifi->settings);
      wifi->settings_open = false;
    }

  if (wifi->services_open)
    {
      pnut_services_disconnect(&wifi->services);
      wifi->services_open = false;
    }

  if (wifi->serving)
    {
      pnut_wifi_close(&wifi->server);
      wifi->serving = false;
    }

  if (wifi->topic != NULL)
    {
      pnut_topic_unadvertise(wifi->topic);
      wifi->topic = NULL;
    }

  /* The radio down, on the loop now: the program ends.  With a job
   * running, the program does it once the worker is done (wifi_end()).
   */

  if (wifi->radio && !wifi->working && wifi->config.ops != NULL)
    {
      wifi->config.ops->down(wifi->config.arg);
      wifi->radio = false;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void wifi_init(FAR struct wifi_s *wifi,
               FAR const struct wifi_config_s *config)
{
  memset(wifi, 0, sizeof(*wifi));
  wifi->config       = *config;
  wifi->module.name  = "wifi";
  wifi->module.start = wifi_start;
  wifi->module.stop  = wifi_stop;
  wifi->state.state  = PNUT_WIFI_INFO_STATE_OFF;
}

void wifi_end(FAR struct wifi_s *wifi)
{
  /* The loop is gone, its worker joined: a job left running had its
   * radio up, or half up
   */

  if ((wifi->radio || wifi->working) && wifi->config.ops != NULL)
    {
      wifi->config.ops->down(wifi->config.arg);
      wifi->radio   = false;
      wifi->working = false;
    }

  explicit_bzero(&wifi->work, sizeof(wifi->work));
}
