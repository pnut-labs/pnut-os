/****************************************************************************
 * pnut-os/src/radios/packetradio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Radio (RFC 0034): the LoRa radio Board's description names.  One caller
 * at a time opens it with a configuration and a way of listening, and
 * receives the packets heard as a stream with credit (a slow reader loses
 * the oldest); its sends are checked against the region's band plan
 * (rules.c).  The device is open only while a caller holds the radio and
 * airplane mode is off.  Opening it, setting it up and sending take the
 * chip milliseconds to seconds: they run on a worker, one at a time,
 * their requests answered when they end.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>

#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/msg.h>
#include <pnut/worker.h>

#include "packetradio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RADIO_SERVICE    "radio"
#define RADIO_PREAMBLE   6        /* Symbols, at least */
#define RADIO_RETRY_MS   20       /* Pushing again, no room in the
                                   * caller's connection */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t radio_now(FAR struct radio_s *radio)
{
  struct timespec ts;

  if (radio->config.now != NULL)
    {
      return radio->config.now();
    }

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint32_t radio_minute(FAR struct radio_s *radio)
{
  return (uint32_t)(radio_now(radio) / 60000);
}

/* The gain of the antenna chosen */

static int radio_gain(FAR struct radio_s *radio)
{
  bool external = radio->part.antenna[0] != '\0' &&
                  strcmp(radio->settings.values.lora_antenna,
                         "external") == 0;

  return external ? radio->part.external_cdbi : radio->part.internal_cdbi;
}

/****************************************************************************
 * Name: radio_publish
 ****************************************************************************/

static void radio_publish(FAR struct radio_s *radio)
{
  pnut_radio_info_t msg;
  uint32_t now;
  int i;

  if (radio->topic == NULL)
    {
      return;
    }

  memset(&msg, 0, sizeof(msg));
  if (radio->present)
    {
      now = radio_minute(radio);
      msg.radios_count = 1;
      strlcpy(msg.radios[0].name, radio->part.name,
              sizeof(msg.radios[0].name));
      msg.radios[0].open = radio->held;
      if (radio->held)
        {
          msg.radios[0].pid    = (uint32_t)radio->pid;
          msg.radios[0].app    = radio->app;
          msg.radios[0].listen = radio->listen;
        }

      for (i = 0; radio->plan != NULL && i < radio->plan->nbands; i++)
        {
          msg.radios[0].air_ms += radio_air_used(&radio->air[i], now);
        }
    }

  if (radio->plan != NULL)
    {
      strlcpy(msg.region, radio->plan->name, sizeof(msg.region));
    }

  msg.airplane = radio->airplane;
  pnut_radio_info_publish(radio->topic, &msg);
}

/****************************************************************************
 * Name: radio_plan
 *
 * Description:
 *   The band plan the region setting gives: auto, the one matching the
 *   radio's band.  A new plan counts air time anew.
 *
 ****************************************************************************/

static void radio_plan(FAR struct radio_s *radio)
{
  FAR const struct radio_plan_s *plan = NULL;
  FAR const char *region = radio->settings.values.region;

  if (radio->present)
    {
      plan = strcmp(region, "auto") == 0 ?
             radio_plan_for(radio->part.min_hz, radio->part.max_hz) :
             radio_plan_find(region);
    }

  if (plan == radio->plan)
    {
      return;
    }

  radio->plan = plan;
  memset(radio->air, 0, sizeof(radio->air));
  if (radio->present)
    {
      pnut_log(&radio->module, plan != NULL ? PNUT_LOG_INFO :
               PNUT_LOG_WARNING, "Band plan: %s",
               plan != NULL ? plan->name : "none, nothing is sent");
    }
}

/****************************************************************************
 * Name: radio_antenna
 *
 * Description:
 *   The antenna setting's line switched, on the worker, between sends.
 *
 ****************************************************************************/

static void radio_reconcile(FAR struct radio_s *radio);

static void radio_antenna(FAR struct radio_s *radio)
{
  if (!radio->present || radio->part.antenna[0] == '\0')
    {
      return;
    }

  radio->antenna_internal = strcmp(radio->settings.values.lora_antenna,
                                   "external") != 0;
  radio->antenna_due      = true;
  radio_reconcile(radio);
}

/****************************************************************************
 * Name: radio_push
 *
 * Description:
 *   The packets heard to the caller, as its credit allows.  A connection
 *   with no room keeps them, and they are pushed again shortly.
 *
 ****************************************************************************/

static void radio_release(FAR struct radio_s *radio);
static void radio_push(FAR struct radio_s *radio);

static void radio_retry(FAR struct pnut_loop_s *loop,
                        FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct radio_s *radio = arg;

  radio->retry = NULL;
  radio_push(radio);
}

static void radio_push(FAR struct radio_s *radio)
{
  FAR const struct radio_heard_s *h;
  pnut_radio_packet_t item;
  int ret;

  while (radio->held && radio->heard_count > 0)
    {
      h = &radio->heard[radio->heard_head];
      memset(&item, 0, sizeof(item));
      item.data.size = h->len;
      memcpy(item.data.bytes, h->data, h->len);
      item.rssi_dbm  = h->rssi_dbm;
      item.snr_cdb   = h->snr_cdb;
      item.crc_error = h->crc_error;
      item.ended_ms  = h->ended_ms;

      ret = pnut_radio_open_push(&radio->server, &radio->stream, &item);
      if (ret == -EAGAIN)
        {
          return;                 /* No credit: ready() goes on */
        }

      if (ret == -EBUSY)
        {
          if (radio->retry == NULL &&
              pnut_timer_start(radio->module.loop, RADIO_RETRY_MS, 0,
                               radio_retry, radio, &radio->retry) < 0)
            {
              radio->retry = NULL;
            }

          return;                 /* No room in the connection: later */
        }

      if (ret == -ENOTCONN)
        {
          radio_release(radio);   /* The stream is over */
          return;
        }

      /* Dropped: an item that does not encode, or the connection failing,
       * which closes the stream and tells closed()
       */

      if (ret < 0)
        {
          pnut_log(&radio->module, PNUT_LOG_WARNING,
                   "A packet not given: %d", ret);
          if (!radio->held)
            {
              return;             /* closed() ran: the packets are gone */
            }
        }

      radio->heard_head = (radio->heard_head + 1) % RADIO_HEARD;
      radio->heard_count--;
    }
}

/****************************************************************************
 * Name: radio_readable
 *
 * Description:
 *   The device has packets heard: keep them, the oldest dropped when the
 *   caller reads too slowly.
 *
 ****************************************************************************/

static void radio_readable(FAR struct pnut_loop_s *loop, int fd,
                           uint32_t events, FAR void *arg)
{
  FAR struct radio_s *radio = arg;
  struct radio_heard_s h;
  int ret;

  for (; ; )
    {
      ret = radio->config.ops->receive(radio->config.arg, &h);
      if (ret <= 0)
        {
          if (ret < 0)
            {
              pnut_log(&radio->module, PNUT_LOG_WARNING,
                       "Receiving failed: %d", ret);
            }

          break;
        }

      h.ended_ms = radio_now(radio);
      pnut_log(&radio->module, PNUT_LOG_DEBUG,
               "Heard %u bytes, %d dBm, SNR %d cdB%s", h.len, h.rssi_dbm,
               h.snr_cdb, h.crc_error ? ", CRC wrong" : "");

      if (radio->heard_count == RADIO_HEARD)
        {
          radio->heard_head = (radio->heard_head + 1) % RADIO_HEARD;
          radio->heard_count--;
          if (radio->dropped++ == 0)
            {
              pnut_log(&radio->module, PNUT_LOG_WARNING,
                       "The caller reads too slowly: packets dropped");
            }
        }

      radio->heard[(radio->heard_head + radio->heard_count) %
                   RADIO_HEARD] = h;
      radio->heard_count++;
    }

  radio_push(radio);
}

/****************************************************************************
 * Name: radio_job_run
 *
 * Description:
 *   The job, on the worker.  An open sets the device up, and closes it
 *   again if that fails; a setup that fails sets the configuration before
 *   it again, so that the radio still listens.
 *
 ****************************************************************************/

static void radio_job_run(FAR void *arg)
{
  FAR struct radio_s *radio = arg;
  FAR const struct radio_dev_ops_s *ops = radio->config.ops;
  int fd;

  switch (radio->job)
    {
      case RADIO_JOB_OPEN:
        fd = ops->open(radio->config.arg, radio->part.device);
        if (fd < 0)
          {
            radio->job_ret = fd;
            break;
          }

        radio->job_ret = ops->setup(radio->config.arg, &radio->job_lora,
                                    radio->job_listen,
                                    radio->job_listen_us,
                                    radio->job_sleep_us);
        if (radio->job_ret < 0)
          {
            ops->close(radio->config.arg);
            break;
          }

        radio->job_fd = fd;
        break;

      case RADIO_JOB_SETUP:
        radio->job_ret = ops->setup(radio->config.arg, &radio->job_lora,
                                    radio->job_listen,
                                    radio->job_listen_us,
                                    radio->job_sleep_us);
        if (radio->job_ret < 0)
          {
            radio->job_old_ret = ops->setup(radio->config.arg,
                                            &radio->job_old,
                                            radio->job_old_listen,
                                            radio->job_old_listen_us,
                                            radio->job_old_sleep_us);
          }

        break;

      case RADIO_JOB_SEND:
        radio->job_ret = ops->send(radio->config.arg, radio->job_data,
                                   radio->job_len);
        break;

      case RADIO_JOB_ANTENNA:
        radio->job_ret = ops->antenna(radio->config.arg,
                                      radio->part.antenna,
                                      radio->job_internal);
        break;
    }
}

/****************************************************************************
 * Name: radio_dev_close
 *
 * Description:
 *   Close the device, no job running.
 *
 ****************************************************************************/

static void radio_dev_close(FAR struct radio_s *radio)
{
  if (radio->fd >= 0)
    {
      pnut_loop_unwatch(radio->module.loop, radio->fd);
    }

  radio->config.ops->close(radio->config.arg);
  radio->dev_open = false;
  radio->fd       = -1;
  pnut_log(&radio->module, PNUT_LOG_DEBUG, "Device closed");
}

/****************************************************************************
 * Name: radio_submit
 *
 * Description:
 *   Start a job, its fields set.
 *
 ****************************************************************************/

static void radio_job_done(FAR struct pnut_loop_s *loop, FAR void *arg);

static int radio_submit(FAR struct radio_s *radio, uint8_t job)
{
  int ret;

  radio->job     = job;
  radio->job_fd  = -1;
  radio->job_ret = OK;
  radio->job_old_ret = OK;
  ret = pnut_job_submit(radio->module.loop, radio_job_run, radio_job_done,
                        radio);
  if (ret < 0)
    {
      radio->job = RADIO_JOB_NONE;
      pnut_log(&radio->module, PNUT_LOG_ERROR, "No worker: %d", ret);
    }

  return ret;
}

/* An open's or a setup's fields: the caller's configuration */

static void radio_job_config(FAR struct radio_s *radio)
{
  radio->job_lora      = radio->lora;
  radio->job_listen    = radio->listen;
  radio->job_listen_us = radio->listen_us;
  radio->job_sleep_us  = radio->sleep_us;
}

/****************************************************************************
 * Name: radio_reconcile
 *
 * Description:
 *   The antenna line switched when the setting changed; the device open
 *   while a caller holds the radio and airplane mode is off, closed
 *   otherwise: once no job runs.
 *
 ****************************************************************************/

static void radio_reconcile(FAR struct radio_s *radio)
{
  bool want = radio->present && radio->held && !radio->airplane;

  if (radio->job != RADIO_JOB_NONE)
    {
      return;
    }

  /* The antenna first: the open waits for it */

  if (radio->antenna_due)
    {
      radio->antenna_due = false;
      radio->job_internal = radio->antenna_internal;
      if (radio_submit(radio, RADIO_JOB_ANTENNA) >= 0)
        {
          return;
        }
    }

  if (radio->dev_open && !want)
    {
      radio_dev_close(radio);
    }
  else if (!radio->dev_open && want)
    {
      radio_job_config(radio);
      radio->opening = false;
      if (radio_submit(radio, RADIO_JOB_OPEN) < 0)
        {
          radio_release(radio);
        }
    }
}

/****************************************************************************
 * Name: radio_release
 *
 * Description:
 *   The caller gave the radio back, or went, or the radio failed it: its
 *   stream ends if it has not.
 *
 ****************************************************************************/

static void radio_release(FAR struct radio_s *radio)
{
  if (!radio->held)
    {
      return;
    }

  radio->held = false;
  if (radio->stream.open)
    {
      pnut_stream_end(&radio->stream, PNUT_STATUS_UNAVAILABLE);
    }

  if (radio->retry != NULL)
    {
      pnut_timer_cancel(radio->module.loop, radio->retry);
      radio->retry = NULL;
    }

  radio->heard_count = 0;
  radio->heard_head  = 0;
  if (radio->dropped > 0)
    {
      pnut_log(&radio->module, PNUT_LOG_INFO, "%lu packets dropped",
               (unsigned long)radio->dropped);
      radio->dropped = 0;
    }

  pnut_log(&radio->module, PNUT_LOG_INFO, "Given back by %d",
           (int)radio->pid);
  radio_reconcile(radio);
  radio_publish(radio);
}

static void radio_ready(FAR struct pnut_stream_s *stream, FAR void *arg)
{
  radio_push(arg);
}

static void radio_closed(FAR struct pnut_stream_s *stream, int status,
                         FAR void *arg)
{
  radio_release(arg);
}

/****************************************************************************
 * Name: radio_hold
 *
 * Description:
 *   An Open accepted: the caller holds the radio, set up as it asked.
 *   False if the caller went meanwhile.
 *
 ****************************************************************************/

static bool radio_hold(FAR struct radio_s *radio,
                       FAR const struct pnut_request_s *req)
{
  memset(&radio->stream, 0, sizeof(radio->stream));
  if (pnut_radio_open_accept(&radio->server, req, &radio->stream,
                             radio_ready, radio_closed, radio) < 0)
    {
      return false;
    }

  radio->held        = true;
  radio->pid         = req->pid;
  radio->app         = req->app;
  radio->lora        = radio->job_lora;
  radio->listen      = radio->job_listen;
  radio->listen_us   = radio->job_listen_us;
  radio->sleep_us    = radio->job_sleep_us;
  radio->heard_count = 0;
  radio->heard_head  = 0;
  radio->dropped     = 0;
  pnut_log(&radio->module, PNUT_LOG_INFO, "Opened by %d", (int)req->pid);
  return true;
}

/****************************************************************************
 * Name: radio_failure
 *
 * Description:
 *   A request failed by the radio: a configuration it does not take, or
 *   the radio itself.
 *
 ****************************************************************************/

static void radio_failure(FAR struct radio_s *radio,
                          FAR const struct pnut_request_s *req, int ret)
{
  if (ret == -EINVAL)
    {
      pnut_radio_fail(&radio->server, req, PNUT_STATUS_INVALID,
                      PNUT_RADIO_ERROR_CODE_CONFIG, "the radio does not "
                      "take it");
    }
  else
    {
      pnut_radio_fail(&radio->server, req, PNUT_STATUS_INTERNAL,
                      PNUT_RADIO_ERROR_CODE_FAILED, "radio failed");
    }
}

/****************************************************************************
 * Name: radio_job_done
 *
 * Description:
 *   A job's end, on the loop: its request answered, the device's state
 *   taken over, then made to fit what is wanted now.
 *
 ****************************************************************************/

static void radio_job_done(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct radio_s *radio = arg;
  pnut_radio_send_reply_t sent;
  pnut_radio_reply_t reply;
  uint8_t job = radio->job;
  int ret = radio->job_ret;

  radio->job = RADIO_JOB_NONE;
  switch (job)
    {
      case RADIO_JOB_OPEN:
        if (ret >= 0)
          {
            radio->dev_open = true;
            radio->fd       = radio->job_fd;
            radio->job_fd   = -1;
            ret = pnut_loop_watch(loop, radio->fd, EPOLLIN, radio_readable,
                                  radio);
            if (ret < 0)
              {
                radio->fd = -1;
                radio_dev_close(radio);
              }
          }

        if (ret < 0)
          {
            pnut_log(&radio->module, PNUT_LOG_ERROR,
                     "The radio does not listen: %d", ret);
          }
        else
          {
            pnut_log(&radio->module, PNUT_LOG_DEBUG, "Listening on %lu Hz",
                     (unsigned long)radio->job_lora.freq_hz);
          }

        if (radio->opening)
          {
            radio->opening = false;
            if (ret < 0)
              {
                radio_failure(radio, &radio->job_req, ret);
              }
            else
              {
                radio_hold(radio, &radio->job_req);
              }
          }
        else if (ret < 0)
          {
            radio_release(radio);
          }

        break;

      case RADIO_JOB_SETUP:
        if (ret >= 0)
          {
            radio->lora      = radio->job_lora;
            radio->listen    = radio->job_listen;
            radio->listen_us = radio->job_listen_us;
            radio->sleep_us  = radio->job_sleep_us;
            memset(&reply, 0, sizeof(reply));
            pnut_radio_configure_reply(&radio->server, &radio->job_req,
                                       PNUT_STATUS_OK, &reply);
          }
        else
          {
            pnut_log(&radio->module, PNUT_LOG_WARNING,
                     "The radio does not take it: %d", ret);
            radio_failure(radio, &radio->job_req, ret);
            if (radio->job_old_ret < 0)
              {
                pnut_log(&radio->module, PNUT_LOG_ERROR,
                         "Nor the configuration before: %d",
                         radio->job_old_ret);
                radio_release(radio);
              }
          }

        break;

      case RADIO_JOB_ANTENNA:
        if (ret < 0)
          {
            pnut_log(&radio->module, PNUT_LOG_WARNING,
                     "The antenna line %s failed: %d", radio->part.antenna,
                     ret);
          }

        break;

      case RADIO_JOB_SEND:
        if (ret < 0)
          {
            pnut_log(&radio->module, PNUT_LOG_WARNING, "Sending failed: %d",
                     ret);
            pnut_radio_fail(&radio->server, &radio->job_req,
                            PNUT_STATUS_INTERNAL,
                            PNUT_RADIO_ERROR_CODE_FAILED, "radio failed");
          }
        else
          {
            memset(&sent, 0, sizeof(sent));
            sent.air_us = radio->job_air_us;
            pnut_radio_send_reply(&radio->server, &radio->job_req,
                                  PNUT_STATUS_OK, &sent);
          }

        break;
    }

  radio_reconcile(radio);
  radio_publish(radio);
}

/****************************************************************************
 * Name: radio_named, radio_mine, radio_idle
 *
 * Description:
 *   Whether a request names the radio, comes from its caller, finds no
 *   job running; if not, it is answered.
 *
 ****************************************************************************/

static bool radio_named(FAR struct radio_s *radio,
                        FAR const struct pnut_request_s *req,
                        FAR const char *name)
{
  if (!radio->present || strcmp(name, radio->part.name) != 0)
    {
      pnut_radio_fail(&radio->server, req, PNUT_STATUS_NOTFOUND,
                      PNUT_RADIO_ERROR_CODE_NO_RADIO, "no such radio");
      return false;
    }

  return true;
}

static bool radio_mine(FAR struct radio_s *radio,
                       FAR const struct pnut_request_s *req,
                       FAR const char *name)
{
  if (!radio_named(radio, req, name))
    {
      return false;
    }

  if (!radio->held || req->from.conn != radio->stream.to.conn ||
      req->from.gen != radio->stream.to.gen)
    {
      pnut_radio_fail(&radio->server, req, PNUT_STATUS_DENIED,
                      PNUT_RADIO_ERROR_CODE_NOT_OPEN, "not opened by you");
      return false;
    }

  return true;
}

static bool radio_idle(FAR struct radio_s *radio,
                       FAR const struct pnut_request_s *req)
{
  if (radio->job != RADIO_JOB_NONE)
    {
      pnut_radio_fail(&radio->server, req, PNUT_STATUS_BUSY,
                      PNUT_RADIO_ERROR_CODE_SENDING,
                      "the radio is busy: a send, or setting up");
      return false;
    }

  return true;
}

/****************************************************************************
 * Name: radio_check
 *
 * Description:
 *   A configuration and a way of listening as asked, checked against the
 *   radio; if they do not fit, the request is answered.  The region's
 *   rules are checked at each send.
 *
 ****************************************************************************/

static bool radio_check(FAR struct radio_s *radio,
                        FAR const struct pnut_request_s *req,
                        bool has_lora, FAR const pnut_radio_lora_t *in,
                        int listen, uint32_t listen_us, uint32_t sleep_us,
                        FAR struct radio_lora_s *lora)
{
  FAR const char *why = NULL;
  uint32_t code = PNUT_RADIO_ERROR_CODE_CONFIG;
  uint32_t half;

  memset(lora, 0, sizeof(*lora));
  if (!has_lora)
    {
      why = "no LoRa configuration";
    }
  else if (!radio_bandwidth(in->bandwidth_hz, &lora->bw_hz,
                            &lora->bw_code))
    {
      why = "no such bandwidth";
    }
  else if (in->spreading_factor < 5 || in->spreading_factor > 12)
    {
      why = "spreading factor 5 to 12";
    }
  else if (in->coding_rate < 5 || in->coding_rate > 8)
    {
      why = "coding rate 5 to 8";
    }
  else if (in->preamble < RADIO_PREAMBLE || in->preamble > UINT16_MAX)
    {
      why = "preamble 6 to 65535";
    }
  else if (in->sync_word > 0xff)
    {
      why = "a sync word of one byte";
    }
  else if (listen != PNUT_RADIO_LISTEN_RADIO_LISTEN_ALWAYS &&
           (listen != PNUT_RADIO_LISTEN_RADIO_LISTEN_DUTY_CYCLE ||
            listen_us == 0 || listen_us > RADIO_DUTY_MAX ||
            sleep_us == 0 || sleep_us > RADIO_DUTY_MAX))
    {
      why = "listen always, or on a duty cycle up to 262 s";
    }
  else
    {
      half = lora->bw_hz / 2;
      if (in->frequency_hz < half ||
          in->frequency_hz - half < radio->part.min_hz ||
          (uint64_t)in->frequency_hz + half > radio->part.max_hz)
        {
          why  = "outside the radio's band";
          code = PNUT_RADIO_ERROR_CODE_BAND;
        }
      else if (in->power_dbm < radio->part.min_dbm ||
               in->power_dbm > radio->part.max_dbm)
        {
          why  = "outside the radio's power";
          code = PNUT_RADIO_ERROR_CODE_POWER;
        }
    }

  if (why != NULL)
    {
      pnut_radio_fail(&radio->server, req, PNUT_STATUS_INVALID, code, why);
      return false;
    }

  lora->freq_hz   = in->frequency_hz;
  lora->sf        = (uint8_t)in->spreading_factor;
  lora->cr        = (uint8_t)in->coding_rate;
  lora->sync      = (uint8_t)in->sync_word;
  lora->preamble  = (uint16_t)in->preamble;
  lora->power_dbm = (int8_t)in->power_dbm;
  lora->crc       = in->crc;
  lora->invert_iq = in->invert_iq;
  lora->ldro      = radio_ldro(lora->sf, lora->bw_hz);
  return true;
}

/****************************************************************************
 * Name: radio_on_open
 *
 * Description:
 *   The radio taken: set up on the worker, answered once it listens (or
 *   failed with why); in airplane mode at once, set up when it ends.
 *
 ****************************************************************************/

static void radio_on_open(FAR struct pnut_radio_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_radio_open_request_t *in,
                          FAR void *arg)
{
  FAR struct radio_s *radio = arg;
  struct radio_lora_s lora;

  if (!radio_named(radio, req, in->radio))
    {
      return;
    }

  if (radio->held || radio->opening)
    {
      pnut_radio_fail(server, req, PNUT_STATUS_BUSY,
                      PNUT_RADIO_ERROR_CODE_BUSY, "another caller has it");
      return;
    }

  if (!radio_idle(radio, req) ||
      !radio_check(radio, req, in->has_lora, &in->lora, in->listen,
                   in->listen_us, in->sleep_us, &lora))
    {
      return;
    }

  radio->job_lora      = lora;
  radio->job_listen    = (uint8_t)in->listen;
  radio->job_listen_us = in->listen_us;
  radio->job_sleep_us  = in->sleep_us;

  if (radio->airplane)
    {
      radio_hold(radio, req);
      radio_publish(radio);
      return;
    }

  radio->job_req = *req;
  radio->opening = true;
  if (radio_submit(radio, RADIO_JOB_OPEN) < 0)
    {
      radio->opening = false;
      pnut_radio_fail(server, req, PNUT_STATUS_INTERNAL,
                      PNUT_RADIO_ERROR_CODE_FAILED, "radio failed");
    }
}

/****************************************************************************
 * Name: radio_on_configure
 *
 * Description:
 *   A new configuration, set up on the worker and answered once the radio
 *   listens with it; one the radio does not take leaves the one before.
 *
 ****************************************************************************/

static void radio_on_configure(FAR struct pnut_radio_server_s *server,
                               FAR const struct pnut_request_s *req,
                               FAR const pnut_radio_configure_request_t *in,
                               FAR void *arg)
{
  FAR struct radio_s *radio = arg;
  pnut_radio_reply_t reply;
  struct radio_lora_s lora;

  if (!radio_mine(radio, req, in->radio) || !radio_idle(radio, req) ||
      !radio_check(radio, req, in->has_lora, &in->lora, in->listen,
                   in->listen_us, in->sleep_us, &lora))
    {
      return;
    }

  if (!radio->dev_open)
    {
      /* Airplane mode: set up when it ends */

      radio->lora      = lora;
      radio->listen    = (uint8_t)in->listen;
      radio->listen_us = in->listen_us;
      radio->sleep_us  = in->sleep_us;
      radio_publish(radio);
      memset(&reply, 0, sizeof(reply));
      pnut_radio_configure_reply(server, req, PNUT_STATUS_OK, &reply);
      return;
    }

  radio->job_old           = radio->lora;
  radio->job_old_listen    = radio->listen;
  radio->job_old_listen_us = radio->listen_us;
  radio->job_old_sleep_us  = radio->sleep_us;
  radio->job_lora          = lora;
  radio->job_listen        = (uint8_t)in->listen;
  radio->job_listen_us     = in->listen_us;
  radio->job_sleep_us      = in->sleep_us;
  radio->job_req           = *req;
  if (radio_submit(radio, RADIO_JOB_SETUP) < 0)
    {
      pnut_radio_fail(server, req, PNUT_STATUS_INTERNAL,
                      PNUT_RADIO_ERROR_CODE_FAILED, "radio failed");
    }
}

/****************************************************************************
 * Name: radio_on_send
 *
 * Description:
 *   Checked against the region's rules: the channel wholly in a sub-band,
 *   the power at the antenna within its limit, its air time within the
 *   sub-band's share of the last hour.  The air time is counted as it is
 *   sent, on the worker.
 *
 ****************************************************************************/

static void radio_on_send(FAR struct pnut_radio_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_radio_send_request_t *in,
                          FAR void *arg)
{
  FAR struct radio_s *radio = arg;
  FAR const struct radio_band_s *band;
  char text[40];
  uint32_t minute;
  uint32_t wait;
  uint32_t ms;
  int index;

  if (!radio_mine(radio, req, in->radio))
    {
      return;
    }

  if (radio->airplane)
    {
      pnut_radio_fail(server, req, PNUT_STATUS_UNAVAILABLE,
                      PNUT_RADIO_ERROR_CODE_AIRPLANE, "airplane mode");
      return;
    }

  if (!radio_idle(radio, req))
    {
      return;
    }

  if (in->data.size == 0)
    {
      pnut_radio_fail(server, req, PNUT_STATUS_INVALID,
                      PNUT_RADIO_ERROR_CODE_NONE, "an empty packet");
      return;
    }

  if (!radio->dev_open)
    {
      pnut_radio_fail(server, req, PNUT_STATUS_INTERNAL,
                      PNUT_RADIO_ERROR_CODE_FAILED, "radio failed");
      return;
    }

  if (radio->plan == NULL)
    {
      pnut_radio_fail(server, req, PNUT_STATUS_UNAVAILABLE,
                      PNUT_RADIO_ERROR_CODE_NO_PLAN, "no band plan");
      return;
    }

  index = radio_band_of(radio->plan, radio->lora.freq_hz,
                        radio->lora.bw_hz);
  if (index < 0)
    {
      pnut_radio_fail(server, req, PNUT_STATUS_DENIED,
                      PNUT_RADIO_ERROR_CODE_BAND, "not in a sub-band");
      return;
    }

  band = &radio->plan->bands[index];
  if (!radio_power_ok(band, radio->lora.power_dbm, radio_gain(radio)))
    {
      pnut_radio_fail(server, req, PNUT_STATUS_DENIED,
                      PNUT_RADIO_ERROR_CODE_POWER, "over the power limit");
      return;
    }

  radio->job_air_us = radio_airtime_us(&radio->lora, in->data.size);
  ms     = radio->job_air_us / 1000 + (radio->job_air_us % 1000 != 0);
  minute = radio_minute(radio);
  wait   = radio_air_wait(&radio->air[index], minute,
                          (uint32_t)band->duty_permille * 3600, ms);
  if (wait > 0)
    {
      if (wait == UINT32_MAX)
        {
          strlcpy(text, "longer than the hour's share", sizeof(text));
        }
      else
        {
          snprintf(text, sizeof(text), "air time used: fits in %lu min",
                   (unsigned long)wait);
        }

      pnut_radio_fail(server, req, PNUT_STATUS_BUSY,
                      PNUT_RADIO_ERROR_CODE_AIR_TIME, text);
      return;
    }

  radio->job_req = *req;
  radio->job_len = in->data.size;
  memcpy(radio->job_data, in->data.bytes, in->data.size);
  if (radio_submit(radio, RADIO_JOB_SEND) < 0)
    {
      pnut_radio_fail(server, req, PNUT_STATUS_INTERNAL,
                      PNUT_RADIO_ERROR_CODE_FAILED, "radio failed");
      return;
    }

  radio_air_add(&radio->air[index], minute, ms);
  pnut_log(&radio->module, PNUT_LOG_DEBUG, "Sending %u bytes, %lu us",
           (unsigned)in->data.size, (unsigned long)radio->job_air_us);
}

static const struct pnut_radio_handlers_s g_radio_handlers =
{
  radio_on_open,
  radio_on_send,
  radio_on_configure
};

/****************************************************************************
 * Name: radio_settings_changed, radio_airplane
 ****************************************************************************/

static void radio_settings_changed(FAR struct pnut_settings_cache_s *cache,
                                   int index, FAR void *arg)
{
  FAR struct radio_s *radio = arg;

  radio_plan(radio);
  radio_antenna(radio);
  radio_publish(radio);
}

static void radio_airplane(FAR struct pnut_airplane_mode_reader_s *reader,
                           FAR const pnut_airplane_mode_t *msg,
                           FAR void *arg)
{
  FAR struct radio_s *radio = arg;

  if (msg->on == radio->airplane)
    {
      return;
    }

  radio->airplane = msg->on;
  pnut_log(&radio->module, PNUT_LOG_INFO, "Airplane mode %s",
           msg->on ? "on: off the air" : "off");
  radio_reconcile(radio);
  radio_publish(radio);
}

/****************************************************************************
 * Name: radio_found
 *
 * Description:
 *   The radio known, or none: say Radio is ready.
 *
 ****************************************************************************/

static void radio_found(FAR struct radio_s *radio,
                        FAR const struct radio_part_s *part)
{
  if (part == NULL || part->device[0] == '\0')
    {
      pnut_log(&radio->module, PNUT_LOG_INFO, "No LoRa radio");
    }
  else
    {
      radio->part    = *part;
      radio->present = true;
      pnut_log(&radio->module, PNUT_LOG_INFO,
               "LoRa radio %s: %s, %lu to %lu kHz", part->name,
               part->device, (unsigned long)(part->min_hz / 1000),
               (unsigned long)(part->max_hz / 1000));
      radio_plan(radio);
      radio_antenna(radio);
    }

  radio_publish(radio);
  pnut_module_ready(&radio->module);
}

/****************************************************************************
 * Name: radio_board, radio_described
 *
 * Description:
 *   Board asked for its description, once: the first LoRa radio, and its
 *   antenna line.
 *
 ****************************************************************************/

static void radio_board(FAR struct pnut_board_client_s *client,
                        bool connected, FAR void *arg);

static void radio_described(FAR struct pnut_board_client_s *client,
                            int status,
                            FAR const pnut_board_description_t *out,
                            FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct radio_s *radio = arg;
  FAR const pnut_board_part_t *p;
  FAR const pnut_board_line_t *l;
  struct radio_part_s part;
  bool found = false;
  pb_size_t i;

  if (status == PNUT_STATUS_TIMEOUT)
    {
      pnut_log(&radio->module, PNUT_LOG_WARNING,
               "No answer from Board: asking again");
      radio_board(client, true, radio);
      return;
    }

  if (status == PNUT_STATUS_UNAVAILABLE && err == NULL)
    {
      pnut_log(&radio->module, PNUT_LOG_WARNING,
               "Board went away: asked again once it is back");
      return;
    }

  memset(&part, 0, sizeof(part));
  for (i = 0; status == PNUT_STATUS_OK && out != NULL &&
              i < out->parts_count && !found; i++)
    {
      p = &out->parts[i];
      if (p->kind == PNUT_BOARD_PART_KIND_LORA && p->device[0] != '\0' &&
          p->which_detail == PNUT_BOARD_PART_RADIO_TAG)
        {
          strlcpy(part.name, p->name, sizeof(part.name));
          strlcpy(part.device, p->device, sizeof(part.device));
          part.min_hz        = p->detail.radio.min_khz * 1000;
          part.max_hz        = p->detail.radio.max_khz * 1000;
          part.min_dbm       = (int8_t)p->detail.radio.min_dbm;
          part.max_dbm       = (int8_t)p->detail.radio.max_dbm;
          part.internal_cdbi = (int16_t)p->detail.radio.internal_gain_cdbi;
          part.external_cdbi = (int16_t)p->detail.radio.external_gain_cdbi;
          found = true;
        }
    }

  for (i = 0; found && i < out->lines_count; i++)
    {
      l = &out->lines[i];
      if (l->kind == PNUT_BOARD_PART_KIND_LORA &&
          l->role == PNUT_BOARD_LINE_ROLE_ANTENNA &&
          strcmp(l->part, part.name) == 0)
        {
          strlcpy(part.antenna, l->device, sizeof(part.antenna));
        }
    }

  pnut_board_disconnect(&radio->board);
  radio->board_open = false;
  radio_found(radio, found ? &part : NULL);
}

static void radio_board(FAR struct pnut_board_client_s *client,
                        bool connected, FAR void *arg)
{
  FAR struct radio_s *radio = arg;
  pnut_board_describe_request_t req;
  int ret;

  if (!connected)
    {
      return;
    }

  memset(&req, 0, sizeof(req));
  ret = pnut_board_describe(client, &req, 0, radio_described, radio);
  if (ret < 0)
    {
      pnut_log(&radio->module, PNUT_LOG_WARNING, "Cannot ask Board: %d",
               ret);
      pnut_board_disconnect(&radio->board);
      radio->board_open = false;
      radio_found(radio, NULL);
    }
}

/****************************************************************************
 * Name: radio_module_start
 ****************************************************************************/

static int radio_module_start(FAR struct pnut_module_s *module,
                              FAR struct pnut_loop_s *loop)
{
  FAR struct radio_s *radio = (FAR struct radio_s *)module;
  int ret;

  ret = pnut_radio_settings_open(module, radio_settings_changed, radio,
                                 &radio->settings);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No settings: %d", ret);
    }
  else
    {
      radio->settings_open = true;
    }

  ret = pnut_radio_info_advertise(loop, &radio->topic);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No radio topic: %d", ret);
      radio->topic = NULL;
    }

  ret = pnut_airplane_mode_subscribe(loop, radio_airplane, radio,
                                     &radio->airplane_reader);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No airplane mode topic: %d",
               ret);
    }
  else
    {
      radio->airplane_open = true;
    }

  ret = pnut_radio_serve(loop, RADIO_SERVICE, 0, &g_radio_handlers, radio,
                         &radio->server);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Cannot serve: %d", ret);
      return ret;
    }

  radio->serving = true;

  if (radio->config.part != NULL)
    {
      radio_found(radio, radio->config.part);
      return OK;
    }

  ret = pnut_board_connect(loop, radio->config.board, radio_board, radio,
                           &radio->board);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No Board: %d; no radio", ret);
      radio_found(radio, NULL);
      return OK;
    }

  radio->board_open = true;
  return OK;
}

/****************************************************************************
 * Name: radio_module_stop
 *
 * Description:
 *   A send still on the worker finishes as the loop is destroyed, which
 *   waits for it; the device is closed after.
 *
 ****************************************************************************/

static void radio_module_stop(FAR struct pnut_module_s *module,
                              FAR struct pnut_loop_s *loop)
{
  FAR struct radio_s *radio = (FAR struct radio_s *)module;

  if (radio->retry != NULL)
    {
      pnut_timer_cancel(loop, radio->retry);
      radio->retry = NULL;
    }

  if (radio->held)
    {
      radio->held = false;
      pnut_stream_end(&radio->stream, PNUT_STATUS_UNAVAILABLE);
    }

  if (radio->board_open)
    {
      pnut_board_disconnect(&radio->board);
      radio->board_open = false;
    }

  if (radio->serving)
    {
      pnut_radio_close(&radio->server);
      radio->serving = false;
    }

  if (radio->airplane_open)
    {
      pnut_airplane_mode_unsubscribe(&radio->airplane_reader);
      radio->airplane_open = false;
    }

  if (radio->settings_open)
    {
      pnut_radio_settings_close(&radio->settings);
      radio->settings_open = false;
    }

  if (radio->topic != NULL)
    {
      pnut_topic_unadvertise(radio->topic);
      radio->topic = NULL;
    }

  /* Closed now, or by radio_deinit() once a job on the worker is done */

  if (radio->dev_open && radio->job == RADIO_JOB_NONE)
    {
      radio_dev_close(radio);
    }
  else if (radio->dev_open && radio->fd >= 0)
    {
      pnut_loop_unwatch(loop, radio->fd);
      radio->fd = -1;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: radio_init
 ****************************************************************************/

void radio_init(FAR struct radio_s *radio,
                FAR const struct radio_config_s *config)
{
  memset(radio, 0, sizeof(*radio));
  radio->config       = *config;
  radio->fd           = -1;
  radio->job_fd       = -1;
  radio->module.name  = "radio";
  radio->module.start = radio_module_start;
  radio->module.stop  = radio_module_stop;
}

/****************************************************************************
 * Name: radio_deinit
 *
 * Description:
 *   After the loop is destroyed (its workers stopped): close the device,
 *   open still, or opened by a job whose end the loop no longer took.
 *
 ****************************************************************************/

void radio_deinit(FAR struct radio_s *radio)
{
  if (radio->dev_open || radio->job_fd >= 0)
    {
      radio->config.ops->close(radio->config.arg);
      radio->dev_open = false;
      radio->job_fd   = -1;
    }
}
