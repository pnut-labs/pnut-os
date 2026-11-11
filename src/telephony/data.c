/****************************************************************************
 * pnut-os/src/telephony/data.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Mobile data (RFC 0030, step 4).  With the data setting on and the
 * modem registered, channel 2 of the multiplexer dials the default bearer
 * while the AT channel is held, since the modem answers nothing on it
 * meanwhile.  Once connected, PPP's bytes go between channel 2 and the
 * master end of a pseudo-terminal, whose slave end the link (pppd) uses.
 * The link tells of PPP's state through a pipe, on the loop.  A failure
 * hangs the call up and tries again after a growing wait.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

/* posix_openpt() and ptsname_r() on the computer, for the tests */

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/timer.h>

#include "pnut/telephony.pb.h"

#include "data.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DATA_QUEUE_MS   100         /* Between looks at the AT queue */
#define DATA_PROBE_MS   1000        /* For the probe's OK on channel 2 */
#define DATA_PROBES     2           /* A fresh channel may refuse one */
#define DATA_GUARD_MS   1200        /* Silence around "+++" (S12: 1 s) */

/* The probe sets DTR's meaning on channel 2 too: dropped, it hangs up
 * (&D2).  The A7682E starts with &D0, ignoring DTR, which left a call in
 * data mode after its link stopped.
 */

#define DATA_PROBE      "AT&D2\r"
#define DATA_HANGUP     "ATH&D2\r"
#define DATA_READS      4           /* Reads of PPP's bytes per wake-up,
                                     * for one frame at most */
#define PPP_FLAG        0x7e        /* A PPP frame's start and end */

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum data_step_e
{
  DATA_STEP_OFF = 0,                /* Waits for the setting and a network */
  DATA_STEP_APN,                    /* AT+CGDCONT asked */
  DATA_STEP_QUEUE,                  /* Waits for the AT queue to empty */
  DATA_STEP_PROBE,                  /* AT&D2 on channel 2, for its OK */
  DATA_STEP_ESCAPE,                 /* No answer: "+++" between silences,
                                     * then ATH, in case a call is left in
                                     * data mode */
  DATA_STEP_DIAL,                   /* ATD*99***1#, for CONNECT */
  DATA_STEP_LINK,                   /* Connected: PPP coming up */
  DATA_STEP_UP,                     /* PPP up */
  DATA_STEP_STOPPING,               /* The link asked to stop */
  DATA_STEP_HANGUP,                 /* DTR held low */
  DATA_STEP_RETRY                   /* Waits before trying again */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void data_check(FAR struct data_s *data);
static void data_fail(FAR struct data_s *data, FAR const char *why);
static void data_to_ppp(FAR struct data_s *data, FAR const uint8_t *buf,
                        size_t len);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: data_changed
 ****************************************************************************/

static void data_changed(FAR struct data_s *data)
{
  if (data->ops->changed != NULL)
    {
      data->ops->changed(data->arg);
    }
}

/****************************************************************************
 * Name: data_untime, data_timer
 *
 * Description:
 *   The step's timer cancelled; and started, in place of the one before.
 *
 ****************************************************************************/

static void data_untime(FAR struct data_s *data)
{
  if (data->timer != NULL)
    {
      pnut_timer_cancel(data->loop, data->timer);
      data->timer = NULL;
    }
}

static void data_hold(FAR struct data_s *data, bool hold);

static bool data_timer(FAR struct data_s *data, uint32_t ms,
                       pnut_timer_handler_t handler)
{
  data_untime(data);
  if (pnut_timer_start(data->loop, ms, 0, handler, data,
                       &data->timer) < 0)
    {
      /* Nothing will end the step: at least the AT channel is not held
       * meanwhile
       */

      data->timer = NULL;
      pnut_log(data->module, PNUT_LOG_WARNING,
               "Mobile data: no timer for its step %u", data->step);
      data_hold(data, false);
      return false;
    }

  return true;
}

/****************************************************************************
 * Name: data_hold
 ****************************************************************************/

static void data_hold(FAR struct data_s *data, bool hold)
{
  if (data->held != hold)
    {
      data->held = hold;
      data->ops->hold(data->arg, hold);
    }
}

/****************************************************************************
 * Name: data_text
 *
 * Description:
 *   A command to the modem on channel 2.
 *
 ****************************************************************************/

static int data_text(FAR struct data_s *data, FAR const char *text)
{
  pnut_log(data->module, PNUT_LOG_DEBUG, "data > %s", text);
  return data->ops->send(data->arg, (FAR const uint8_t *)text,
                         strlen(text));
}

/****************************************************************************
 * Name: data_rewatch
 *
 * Description:
 *   The master end watched for what it waits for: PPP's bytes unless
 *   channel 2 refused the last ones, room while bytes wait for it.
 *
 ****************************************************************************/

static void data_rewatch(FAR struct data_s *data)
{
  uint32_t events = 0;

  if (data->master < 0)
    {
      return;
    }

  if (!data->out_held)
    {
      events |= EPOLLIN;
    }

  if (data->count > 0)
    {
      events |= EPOLLOUT;
    }

  if (pnut_loop_rewatch(data->loop, data->master, events) < 0)
    {
      pnut_log(data->module, PNUT_LOG_WARNING,
               "Mobile data: the pseudo-terminal not watched");
    }
}

/****************************************************************************
 * Name: data_pty_close
 ****************************************************************************/

static void data_pty_close(FAR struct data_s *data)
{
  if (data->flush != NULL)
    {
      pnut_timer_cancel(data->loop, data->flush);
      data->flush = NULL;
    }

  if (data->master >= 0)
    {
      pnut_loop_unwatch(data->loop, data->master);
      close(data->master);
      data->master = -1;
    }

  if (data->slave >= 0)
    {
      close(data->slave);
      data->slave = -1;
    }

  if (data->dropped > 0)
    {
      pnut_log(data->module, PNUT_LOG_INFO,
               "Mobile data: %lu bytes for PPP found no room",
               (unsigned long)data->dropped);
    }

  data->head     = 0;
  data->count    = 0;
  data->dropped  = 0;
  data->olen     = 0;
  data->out_held = false;
}

/****************************************************************************
 * Name: data_raw
 *
 * Description:
 *   A pseudo-terminal's end raw: PPP is binary, and a pseudo-terminal
 *   starts cooked (CR into NL, echo, NL into CR NL).
 *
 ****************************************************************************/

static int data_raw(int fd)
{
  struct termios tio;

  if (tcgetattr(fd, &tio) < 0)
    {
      return -errno;
    }

  tio.c_iflag = 0;
  tio.c_oflag = 0;
  tio.c_lflag = 0;
  return tcsetattr(fd, TCSANOW, &tio) < 0 ? -errno : 0;
}

/****************************************************************************
 * Name: data_master
 ****************************************************************************/

static void data_from_ppp(FAR struct data_s *data);
static void data_ring_out(FAR struct data_s *data);

static void data_master(FAR struct pnut_loop_s *loop, int fd,
                        uint32_t events, FAR void *arg)
{
  FAR struct data_s *data = arg;

  if (events & EPOLLOUT)
    {
      data_ring_out(data);
    }

  if (events & EPOLLIN)
    {
      data_from_ppp(data);
    }
}

/****************************************************************************
 * Name: data_pty_open
 *
 * Description:
 *   A pseudo-terminal for the link: the master end here, not blocking,
 *   and the slave end held open too, so that the master reads no hang-up
 *   while the link opens and closes it.
 *
 ****************************************************************************/

static int data_pty_open(FAR struct data_s *data)
{
  int ret;

  data->master = posix_openpt(O_RDWR | O_NOCTTY);
  if (data->master < 0)
    {
      return -errno;
    }

  if (grantpt(data->master) < 0 || unlockpt(data->master) < 0 ||
      ptsname_r(data->master, data->tty, sizeof(data->tty)) != 0)
    {
      ret = -errno;
      goto errout;
    }

  data->slave = open(data->tty, O_RDWR | O_NOCTTY);
  if (data->slave < 0)
    {
      ret = -errno;
      goto errout;
    }

  ret = data_raw(data->master);
  if (ret >= 0)
    {
      ret = data_raw(data->slave);
    }

  if (ret < 0)
    {
      goto errout;
    }

  ret = fcntl(data->master, F_GETFL);
  if (ret < 0 || fcntl(data->master, F_SETFL, ret | O_NONBLOCK) < 0)
    {
      ret = -errno;
      goto errout;
    }

  ret = pnut_loop_watch(data->loop, data->master, EPOLLIN, data_master,
                        data);
  if (ret < 0)
    {
      goto errout;
    }

  return 0;

errout:
  if (data->slave >= 0)
    {
      close(data->slave);
      data->slave = -1;
    }

  close(data->master);
  data->master = -1;
  return ret;
}

/****************************************************************************
 * Name: data_finish
 *
 * Description:
 *   The call over: the link has ended, or never started.  Channel 2 hangs
 *   up, and data waits, to try again or for the setting and a network.
 *
 ****************************************************************************/

static void data_retry_tick(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg);

static void data_done(FAR struct data_s *data);

static void data_dtr_tick(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct data_s *data = arg;

  data->timer = NULL;
  if (data->step == DATA_STEP_HANGUP)
    {
      data->ops->dtr(data->arg, true);
      data_done(data);
    }
}

static void data_finish(FAR struct data_s *data)
{
  data_untime(data);
  data_hold(data, false);
  data_pty_close(data);
  data->llen = 0;

  if (data->up)
    {
      data->up = false;
      pnut_log(data->module, PNUT_LOG_INFO, "Mobile data off");
    }

  /* A call made: DTR held low a moment, then raised for the next */

  /* The network's gap kept the call: there is none now */

  if (data->gap != NULL)
    {
      pnut_timer_cancel(data->loop, data->gap);
      data->gap = NULL;
    }

  if (data->calling)
    {
      data->calling = false;
      data->step    = DATA_STEP_HANGUP;
      data->ops->dtr(data->arg, false);
      if (data_timer(data, DATA_DTR_MS, data_dtr_tick))
        {
          return;
        }

      data->ops->dtr(data->arg, true);
    }

  data_done(data);
}

/****************************************************************************
 * Name: data_done
 *
 * Description:
 *   After the call: wait to try again, or for the setting and a network.
 *
 ****************************************************************************/

static void data_done(FAR struct data_s *data)
{
  if (data->retrying)
    {
      data->retrying = false;
      data->step = DATA_STEP_RETRY;
      data_timer(data, data->retry, data_retry_tick);
      data->retry = data->retry >= DATA_RETRY_MAX / 2 ? DATA_RETRY_MAX :
                    data->retry * 2;
    }
  else
    {
      data->step = DATA_STEP_OFF;
    }

  data_changed(data);
  data_check(data);
}

/****************************************************************************
 * Name: data_stop_tick
 *
 * Description:
 *   The link did not end in time (its thread gone without its last
 *   record?): go on without it.  It refuses to start again while it still
 *   runs (data_ppp.c).
 *
 ****************************************************************************/

static void data_stop_tick(FAR struct pnut_loop_s *loop,
                           FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct data_s *data = arg;

  data->timer = NULL;
  if (data->step == DATA_STEP_STOPPING)
    {
      pnut_log(data->module, PNUT_LOG_WARNING,
               "Mobile data: PPP did not end in %d s; going on without it",
               DATA_STOP_MS / 1000);
      data->linked = false;
      data_finish(data);
    }
}

/****************************************************************************
 * Name: data_end
 *
 * Description:
 *   End the call: the link asked to stop first, if it runs, and its end
 *   awaited; then data_finish().
 *
 ****************************************************************************/

static void data_end(FAR struct data_s *data, bool retry)
{
  data_untime(data);
  data_hold(data, false);
  data->retrying = retry;

  if (data->linked)
    {
      if (data->step != DATA_STEP_STOPPING)
        {
          data->step = DATA_STEP_STOPPING;
          data->link->stop(data->link->arg);
          data_timer(data, DATA_STOP_MS, data_stop_tick);
          data_changed(data);
        }

      return;                       /* DATA_LINK_ENDED: data_finish() */
    }

  data_finish(data);
}

/****************************************************************************
 * Name: data_fail
 ****************************************************************************/

static void data_fail(FAR struct data_s *data, FAR const char *why)
{
  if (data->fails < UINT8_MAX)
    {
      data->fails++;
    }

  if (data->fails >= DATA_FAILS && !data->failed)
    {
      data->failed = true;
      pnut_log(data->module, PNUT_LOG_WARNING,
               "Mobile data failed %u times, the last: %s", data->fails,
               why);
    }
  else
    {
      pnut_log(data->module, PNUT_LOG_INFO,
               "Mobile data: %s; tried again in %lu s", why,
               (unsigned long)(data->retry / 1000));
    }

  data_end(data, true);
}

/****************************************************************************
 * Name: data_retry_tick
 ****************************************************************************/

static void data_retry_tick(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct data_s *data = arg;

  data->timer = NULL;
  if (data->step == DATA_STEP_RETRY)
    {
      data->step = DATA_STEP_OFF;
      data_check(data);
    }
}

/****************************************************************************
 * Name: data_link_tick
 *
 * Description:
 *   PPP not up in time after CONNECT, or after it went down.
 *
 ****************************************************************************/

static void data_link_tick(FAR struct pnut_loop_s *loop,
                           FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct data_s *data = arg;

  data->timer = NULL;
  if (data->step == DATA_STEP_LINK)
    {
      data_fail(data, "PPP did not come up");
    }
}

/****************************************************************************
 * Name: data_connected
 *
 * Description:
 *   CONNECT: the AT channel free again, a pseudo-terminal for the link,
 *   and the link started on it.
 *
 ****************************************************************************/

static void data_connected(FAR struct data_s *data)
{
  int ret;

  data_untime(data);
  data_hold(data, false);
  pnut_log(data->module, PNUT_LOG_INFO, "Mobile data: connected");

  ret = data_pty_open(data);
  if (ret < 0)
    {
      pnut_log(data->module, PNUT_LOG_WARNING,
               "Mobile data: no pseudo-terminal: %d", ret);
      data_fail(data, "no pseudo-terminal");
      return;
    }

  ret = data->link->start(data->link->arg, data->tty, data->events[1]);
  if (ret < 0)
    {
      pnut_log(data->module, PNUT_LOG_WARNING,
               "Mobile data: PPP not started: %d", ret);
      data_fail(data, "PPP not started");
      return;
    }

  data->linked = true;
  data->alive  = true;
  data->step   = DATA_STEP_LINK;
  data_timer(data, DATA_LINK_MS, data_link_tick);
}

/****************************************************************************
 * Name: data_line
 *
 * Description:
 *   A line of channel 2's answer while it is asked and dials.
 *
 ****************************************************************************/

static void data_dial_tick(FAR struct pnut_loop_s *loop,
                           FAR struct pnut_timer_s *timer, FAR void *arg);

static void data_line(FAR struct data_s *data, FAR const char *line)
{
  static FAR const char *const fails[] =
  {
    "ERROR", "+CME ERROR", "NO CARRIER", "BUSY", "NO DIALTONE",
    "NO ANSWER"
  };

  size_t i;

  pnut_log(data->module, PNUT_LOG_DEBUG, "data < %s", line);

  if (data->step == DATA_STEP_PROBE)
    {
      if (strcmp(line, "OK") == 0)
        {
          if (data_text(data, "ATD*99***1#\r") < 0)
            {
              data_fail(data, "the dial not sent");
              return;
            }

          data->step    = DATA_STEP_DIAL;
          data->calling = true;
          data_timer(data, DATA_DIAL_MS, data_dial_tick);
        }

      return;
    }

  if (strncmp(line, "CONNECT", 7) == 0)
    {
      data_connected(data);
      return;
    }

  for (i = 0; i < sizeof(fails) / sizeof(fails[0]); i++)
    {
      if (strncmp(line, fails[i], strlen(fails[i])) == 0)
        {
          data_fail(data, line);
          return;
        }
    }
}

/****************************************************************************
 * Name: data_dial_tick, data_probe_tick
 ****************************************************************************/

static void data_dial_tick(FAR struct pnut_loop_s *loop,
                           FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct data_s *data = arg;

  data->timer = NULL;
  if (data->step == DATA_STEP_DIAL)
    {
      data_fail(data, "no CONNECT");
    }
}

static void data_escape_tick(FAR struct pnut_loop_s *loop,
                             FAR struct pnut_timer_s *timer, FAR void *arg);

static void data_probe_tick(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct data_s *data = arg;

  data->timer = NULL;
  if (data->step != DATA_STEP_PROBE)
    {
      return;
    }

  if (++data->probes >= DATA_PROBES)
    {
      if (data->escaped)
        {
          data_fail(data, "channel 2 did not answer");
          return;
        }

      /* Maybe a call left in data mode: out of it, and hung up */

      data->escaped = true;
      data->plus    = false;
      data->step    = DATA_STEP_ESCAPE;
      pnut_log(data->module, PNUT_LOG_INFO,
               "Mobile data: channel 2 silent; escaping a call left on it");
      data_timer(data, DATA_GUARD_MS, data_escape_tick);
      return;
    }

  if (data_text(data, DATA_PROBE) < 0)
    {
      data_fail(data, "channel 2 refused");
      return;
    }

  data_timer(data, DATA_PROBE_MS, data_probe_tick);
}

/****************************************************************************
 * Name: data_escape_tick
 *
 * Description:
 *   A silence, "+++", a silence, then ATH (and &D2) and the probe's wait:
 *   its OK dials.
 *
 ****************************************************************************/

static void data_escape_tick(FAR struct pnut_loop_s *loop,
                             FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct data_s *data = arg;

  data->timer = NULL;
  if (data->step != DATA_STEP_ESCAPE)
    {
      return;
    }

  if (!data->plus)
    {
      data->plus = true;
      if (data_text(data, "+++") < 0)
        {
          data_fail(data, "channel 2 refused");
          return;
        }

      data_timer(data, DATA_GUARD_MS, data_escape_tick);
      return;
    }

  data->llen   = 0;
  data->step   = DATA_STEP_PROBE;
  data->probes = 0;
  if (data_text(data, DATA_HANGUP) < 0)
    {
      data_fail(data, "channel 2 refused");
      return;
    }

  data_timer(data, DATA_PROBE_MS, data_probe_tick);
}

/****************************************************************************
 * Name: data_queue
 *
 * Description:
 *   Dial once nothing waits on the AT channel, which is held meanwhile.
 *
 ****************************************************************************/

static void data_queue_tick(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg);

static void data_queue(FAR struct data_s *data)
{
  if (!data->ops->idle(data->arg))
    {
      data->step = DATA_STEP_QUEUE;
      data_timer(data, DATA_QUEUE_MS, data_queue_tick);
      return;
    }

  data_hold(data, true);
  data->step    = DATA_STEP_PROBE;
  data->probes  = 0;
  data->escaped = false;
  data->llen    = 0;
  pnut_log(data->module, PNUT_LOG_INFO, "Mobile data: dialling");

  if (data_text(data, DATA_PROBE) < 0)
    {
      data_fail(data, "channel 2 refused");
      return;
    }

  data_timer(data, DATA_PROBE_MS, data_probe_tick);
}

static void data_queue_tick(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct data_s *data = arg;

  data->timer = NULL;
  if (data->step == DATA_STEP_QUEUE)
    {
      data_queue(data);
    }
}

/****************************************************************************
 * Name: data_begin
 ****************************************************************************/

static void data_begin(FAR struct data_s *data)
{
  if (data->apn[0] == '\0')
    {
      data_queue(data);
    }
  else
    {
      data->step = DATA_STEP_APN;
      if (data->ops->apn(data->arg, data->apn) < 0)
        {
          data_fail(data, "the access point not set");
          return;
        }
    }

  data_changed(data);             /* Connecting, now that a step runs */
}

/****************************************************************************
 * Name: data_check
 *
 * Description:
 *   Start or end, as the setting and the network allow.  A network lost
 *   for less than DATA_GAP_MS keeps a call.
 *
 ****************************************************************************/

static void data_check(FAR struct data_s *data)
{
  bool keep = data->started && data->link != NULL && data->want &&
              (data->ready || data->gap != NULL);

  if (keep)
    {
      /* A new call only with the network; the gap only keeps one */

      if (data->step == DATA_STEP_OFF && data->ready)
        {
          data_begin(data);
        }

      return;
    }

  switch (data->step)
    {
      case DATA_STEP_OFF:
      case DATA_STEP_STOPPING:
      case DATA_STEP_HANGUP:
        break;

      case DATA_STEP_RETRY:
        data_untime(data);
        data->step = DATA_STEP_OFF;
        data_changed(data);
        break;

      default:
        data_end(data, false);
        break;
    }
}

/****************************************************************************
 * Name: data_gap_tick
 ****************************************************************************/

static void data_gap_tick(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct data_s *data = arg;

  data->gap = NULL;
  data_check(data);
}

/****************************************************************************
 * Name: data_event
 *
 * Description:
 *   One of the link's records.
 *
 ****************************************************************************/

static void data_event(FAR struct data_s *data,
                       FAR const struct data_event_s *ev)
{
  switch (ev->event)
    {
      case DATA_LINK_UP:
        if (data->step != DATA_STEP_LINK)
          {
            break;
          }

        data_untime(data);
        data->link_info = *ev;
        data->link_info.ifname[DATA_IFNAME - 1] = '\0';
        data->step   = DATA_STEP_UP;
        data->up     = true;
        data->fails  = 0;
        data->failed = false;
        data->retry  = DATA_RETRY_MS;
        pnut_log(data->module, PNUT_LOG_INFO, "Mobile data on, %s",
                 data->link_info.ifname);
        data_changed(data);
        break;

      case DATA_LINK_DOWN:
        if (data->step != DATA_STEP_UP)
          {
            break;
          }

        /* PPP renegotiates, or the call is gone: up again in time, or
         * the call ends
         */

        data->up   = false;
        data->step = DATA_STEP_LINK;
        pnut_log(data->module, PNUT_LOG_INFO, "Mobile data: PPP down");
        data_timer(data, DATA_LINK_MS, data_link_tick);
        data_changed(data);
        break;

      case DATA_LINK_ENDED:
        data->linked = false;
        data->alive  = false;
        if (data->step == DATA_STEP_STOPPING)
          {
            data_finish(data);
          }
        else if (data->step == DATA_STEP_LINK ||
                 data->step == DATA_STEP_UP)
          {
            data_fail(data, "PPP ended");
          }
        break;

      default:
        break;
    }
}

/****************************************************************************
 * Name: data_events
 *
 * Description:
 *   The link's pipe: whole records, however they come.
 *
 ****************************************************************************/

static void data_events(FAR struct pnut_loop_s *loop, int fd,
                        uint32_t events, FAR void *arg)
{
  FAR struct data_s *data = arg;
  struct data_event_s ev;
  ssize_t n;

  for (; ; )
    {
      n = read(fd, data->ebuf + data->elen,
               sizeof(data->ebuf) - data->elen);
      if (n <= 0)
        {
          return;
        }

      data->elen += n;
      if (data->elen == sizeof(data->ebuf))
        {
          memcpy(&ev, data->ebuf, sizeof(ev));
          data->elen = 0;
          data_event(data, &ev);
        }
    }
}

/****************************************************************************
 * Name: data_flush_out
 *
 * Description:
 *   PPP's bytes held to channel 2, in one frame.
 *
 ****************************************************************************/

static void data_flush_out(FAR struct data_s *data)
{
  size_t len = data->olen;
  bool held = data->out_held;
  int ret;

  if (data->flush != NULL)
    {
      pnut_timer_cancel(data->loop, data->flush);
      data->flush = NULL;
    }

  if (len == 0)
    {
      return;
    }

  /* Taken before the send, as the AT engine does: room told from inside
   * it (channel 2's queue drained by the write) must find nothing to send
   * again, and bytes read meanwhile start the next frame
   */

  data->olen     = 0;
  data->out_held = false;
  ret = data->ops->send(data->arg, data->out, len);
  if (ret == -EAGAIN)
    {
      /* Refused: nothing was written, so nothing was told or read */

      data->olen     = len;
      data->out_held = true;
      if (!held)
        {
          data_rewatch(data);
        }

      return;                       /* data_room() sends them */
    }

  if (held)
    {
      data_rewatch(data);
    }
}

/****************************************************************************
 * Name: data_flush_tick
 ****************************************************************************/

static void data_flush_tick(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct data_s *data = arg;

  data->flush = NULL;
  data_flush_out(data);
}

/****************************************************************************
 * Name: data_from_ppp
 *
 * Description:
 *   PPP's bytes, which pppd writes one at a time, gathered into frames for
 *   channel 2: a frame goes when full, or at a PPP frame's end, or after
 *   DATA_FLUSH_MS.  A frame for every byte would cut the speed to an
 *   eighth.
 *
 ****************************************************************************/

static void data_from_ppp(FAR struct data_s *data)
{
  ssize_t n;
  int reads;

  for (reads = 0; reads < DATA_READS && !data->out_held; reads++)
    {
      n = read(data->master, data->out + data->olen,
               DATA_FRAME - data->olen);
      if (n <= 0)
        {
          break;
        }

      data->olen += n;
      if (data->olen == DATA_FRAME ||
          (data->olen > 1 && data->out[data->olen - 1] == PPP_FLAG))
        {
          /* One frame a wake-up: under load a UART write takes about the
           * frame's time on the line, and the master stays readable for
           * the next
           */

          data_flush_out(data);
          break;
        }
    }

  if (data->olen > 0 && !data->out_held && data->flush == NULL &&
      pnut_timer_start(data->loop, DATA_FLUSH_MS, 0, data_flush_tick,
                       data, &data->flush) < 0)
    {
      data->flush = NULL;
      data_flush_out(data);
    }
}

/****************************************************************************
 * Name: data_ring_out
 *
 * Description:
 *   The modem's bytes to PPP, as the pseudo-terminal takes them.  A write
 *   to it may come back short.
 *
 ****************************************************************************/

static void data_ring_out(FAR struct data_s *data)
{
  size_t chunk;
  ssize_t n;
  bool waiting = data->count > 0;

  while (data->count > 0)
    {
      chunk = DATA_RING - data->head;
      if (chunk > data->count)
        {
          chunk = data->count;
        }

      n = write(data->master, data->ring + data->head, chunk);
      if (n <= 0)
        {
          break;
        }

      data->head   = (data->head + n) % DATA_RING;
      data->count -= n;
    }

  if (data->count == 0)
    {
      data->head = 0;
    }

  if (waiting != (data->count > 0) || data->count > 0)
    {
      data_rewatch(data);
    }
}

/****************************************************************************
 * Name: data_to_ppp
 ****************************************************************************/

static void data_to_ppp(FAR struct data_s *data, FAR const uint8_t *buf,
                        size_t len)
{
  size_t tail;
  size_t room;
  size_t chunk;

  if (data->master < 0 || data->ring == NULL)
    {
      return;
    }

  room = DATA_RING - data->count;
  if (len > room)
    {
      data->dropped += len - room;
      len = room;
    }

  while (len > 0)
    {
      tail  = (data->head + data->count) % DATA_RING;
      chunk = DATA_RING - tail;
      if (chunk > len)
        {
          chunk = len;
        }

      memcpy(data->ring + tail, buf, chunk);
      data->count += chunk;
      buf         += chunk;
      len         -= chunk;
    }

  data_ring_out(data);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: data_init
 ****************************************************************************/

void data_init(FAR struct data_s *data, FAR struct pnut_module_s *module,
               FAR const struct data_ops_s *ops, FAR void *arg,
               FAR const struct data_link_ops_s *link)
{
  memset(data, 0, sizeof(*data));
  data->module    = module;
  data->ops       = ops;
  data->arg       = arg;
  data->link      = link;
  data->master    = -1;
  data->slave     = -1;
  data->events[0] = -1;
  data->events[1] = -1;
  data->retry     = DATA_RETRY_MS;
}

/****************************************************************************
 * Name: data_start
 ****************************************************************************/

int data_start(FAR struct data_s *data, FAR struct pnut_loop_s *loop)
{
  int flags;
  int ret;

  data->loop = loop;
  if (data->link == NULL)
    {
      return 0;
    }

  data->ring = malloc(DATA_RING);
  if (data->ring == NULL)
    {
      return -ENOMEM;
    }

  if (pipe(data->events) < 0)
    {
      ret = -errno;
      goto errout;
    }

  flags = fcntl(data->events[0], F_GETFL);
  if (flags < 0 ||
      fcntl(data->events[0], F_SETFL, flags | O_NONBLOCK) < 0)
    {
      ret = -errno;
      goto errout_pipe;
    }

  ret = pnut_loop_watch(loop, data->events[0], EPOLLIN, data_events,
                        data);
  if (ret < 0)
    {
      goto errout_pipe;
    }

  data->started = true;
  data_check(data);
  return 0;

errout_pipe:
  close(data->events[0]);
  close(data->events[1]);
  data->events[0] = -1;
  data->events[1] = -1;

errout:
  free(data->ring);
  data->ring = NULL;
  return ret;
}

/****************************************************************************
 * Name: data_stop
 *
 * Description:
 *   The program finishes: the link asked to stop, and left to end with
 *   the program.  The pipe stays open, for the link's last records.
 *
 ****************************************************************************/

void data_stop(FAR struct data_s *data)
{
  if (!data->started)
    {
      return;
    }

  data->started = false;
  data_untime(data);
  if (data->gap != NULL)
    {
      pnut_timer_cancel(data->loop, data->gap);
      data->gap = NULL;
    }

  if (data->flush != NULL)
    {
      pnut_timer_cancel(data->loop, data->flush);
      data->flush = NULL;
    }

  if (data->linked)
    {
      data->link->stop(data->link->arg);
    }

  if (data->master >= 0)
    {
      pnut_loop_unwatch(data->loop, data->master);
    }

  pnut_loop_unwatch(data->loop, data->events[0]);
  data->step = DATA_STEP_OFF;
}

/****************************************************************************
 * Name: data_deinit
 ****************************************************************************/

void data_deinit(FAR struct data_s *data)
{
  FAR const struct data_event_s *ev;
  ssize_t n;

  /* The records the link wrote since data_stop(): its end among them? */

  while (data->alive && data->events[0] >= 0 &&
         (n = read(data->events[0], data->ebuf + data->elen,
                   sizeof(data->ebuf) - data->elen)) > 0)
    {
      data->elen += n;
      if (data->elen == sizeof(data->ebuf))
        {
          data->elen = 0;
          ev = (FAR const struct data_event_s *)data->ebuf;
          if (ev->event == DATA_LINK_ENDED)
            {
              data->linked = false;
              data->alive  = false;
            }
        }
    }

  if (data->alive)
    {
      return;                       /* Its thread may still write */
    }

  if (data->master >= 0)
    {
      close(data->master);
      data->master = -1;
    }

  if (data->slave >= 0)
    {
      close(data->slave);
      data->slave = -1;
    }

  if (data->events[0] >= 0)
    {
      close(data->events[0]);
      close(data->events[1]);
      data->events[0] = -1;
      data->events[1] = -1;
    }

  free(data->ring);
  data->ring = NULL;
}

/****************************************************************************
 * Name: data_want
 ****************************************************************************/

void data_want(FAR struct data_s *data, bool want, FAR const char *apn)
{
  strlcpy(data->apn, apn != NULL ? apn : "", sizeof(data->apn));
  if (want == data->want)
    {
      return;
    }

  data->want = want;
  pnut_log(data->module, PNUT_LOG_INFO, "Mobile data %s",
           want ? "wanted" : "not wanted");

  /* Off and on again: a fresh start, without a failure's wait */

  data->fails  = 0;
  data->failed = false;
  data->retry  = DATA_RETRY_MS;
  if (want && data->step == DATA_STEP_RETRY)
    {
      data_untime(data);
      data->step = DATA_STEP_OFF;
    }

  data_changed(data);
  data_check(data);
}

/****************************************************************************
 * Name: data_ready
 ****************************************************************************/

void data_ready(FAR struct data_s *data, bool ready, bool keep)
{
  bool was = data->ready;

  data->ready = ready;

  /* A gap keeps a call only while ready may come back by itself */

  if ((ready || !keep) && data->gap != NULL)
    {
      pnut_timer_cancel(data->loop, data->gap);
      data->gap = NULL;
    }

  if (ready == was && (ready || keep))
    {
      return;
    }

  if (!ready && keep && data->gap == NULL &&
      (data->step == DATA_STEP_UP || data->step == DATA_STEP_LINK) &&
      data->started &&
      pnut_timer_start(data->loop, DATA_GAP_MS, 0, data_gap_tick,
                       data, &data->gap) < 0)
    {
      data->gap = NULL;
    }

  data_check(data);
}

/****************************************************************************
 * Name: data_apn_done
 ****************************************************************************/

void data_apn_done(FAR struct data_s *data, int result)
{
  if (data->step != DATA_STEP_APN)
    {
      return;
    }

  if (result < 0)
    {
      data_fail(data, "the access point not set");
      return;
    }

  data_queue(data);
}

/****************************************************************************
 * Name: data_reset
 ****************************************************************************/

void data_reset(FAR struct data_s *data)
{
  data->ready   = false;
  data->calling = false;                /* Channel 2 went with the modem */
  if (data->gap != NULL)
    {
      pnut_timer_cancel(data->loop, data->gap);
      data->gap = NULL;
    }

  if (data->step != DATA_STEP_OFF && data->step != DATA_STEP_STOPPING)
    {
      data_end(data, false);    /* A hang-up under way: over, its DTR too */
    }
}

/****************************************************************************
 * Name: data_input
 ****************************************************************************/

void data_input(FAR struct data_s *data, FAR const uint8_t *buf,
                size_t len)
{
  size_t i;

  switch (data->step)
    {
      case DATA_STEP_PROBE:
      case DATA_STEP_DIAL:
        for (i = 0; i < len; i++)
          {
            if (buf[i] == '\r' || buf[i] == '\n')
              {
                if (data->llen == 0)
                  {
                    continue;
                  }

                data->line[data->llen] = '\0';
                data->llen = 0;
                data_line(data, data->line);

                /* CONNECT: the rest is PPP's, after the line's end */

                if (data->step == DATA_STEP_LINK)
                  {
                    for (i++; i < len && (buf[i] == '\r' || buf[i] == '\n');
                         i++)
                      {
                      }

                    data_to_ppp(data, buf + i, len - i);
                    return;
                  }

                if (data->step != DATA_STEP_PROBE &&
                    data->step != DATA_STEP_DIAL)
                  {
                    return;
                  }
              }
            else if (data->llen < DATA_LINE - 1)
              {
                data->line[data->llen++] = (char)buf[i];
              }
          }
        break;

      case DATA_STEP_LINK:
      case DATA_STEP_UP:
      case DATA_STEP_STOPPING:
        data_to_ppp(data, buf, len);
        break;

      default:
        pnut_log(data->module, PNUT_LOG_DEBUG,
                 "Mobile data: %zu bytes on channel 2, unasked", len);
        break;
    }
}

/****************************************************************************
 * Name: data_room
 ****************************************************************************/

void data_room(FAR struct data_s *data)
{
  if (data->out_held)
    {
      data_flush_out(data);
      if (!data->out_held && data->master >= 0)
        {
          data_from_ppp(data);
        }
    }
}

/****************************************************************************
 * Name: data_state
 ****************************************************************************/

int data_state(FAR const struct data_s *data)
{
  if (!data->want || data->link == NULL)
    {
      return PNUT_TELEPHONY_INFO_DATA_DATA_OFF;
    }

  if (data->step == DATA_STEP_UP)
    {
      return PNUT_TELEPHONY_INFO_DATA_DATA_ON;
    }

  if (data->failed)
    {
      return PNUT_TELEPHONY_INFO_DATA_DATA_FAILED;
    }

  if (data->step == DATA_STEP_OFF)
    {
      return PNUT_TELEPHONY_INFO_DATA_DATA_OFF;
    }

  return PNUT_TELEPHONY_INFO_DATA_CONNECTING;
}
