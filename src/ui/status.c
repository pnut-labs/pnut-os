/****************************************************************************
 * pnut-os/src/ui/status.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The device's state the system UI's header shows: Telephony's, Wi-Fi's
 * and Time's topics (RFC 0005), any native program's to read.  A part is
 * unknown until its topic's first message, and the screens are told of a
 * message only when it changes something, so that the same state again
 * redraws nothing.  The clock's minutes come from a timer, set to the next
 * minute's start.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <string.h>

#include "status.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void status_arm(FAR struct ui_status_s *st);
static void status_rearm(FAR struct ui_status_s *st);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void status_tel(FAR struct pnut_telephony_info_reader_s *reader,
                       FAR const pnut_telephony_info_t *msg, FAR void *arg)
{
  FAR struct ui_status_s *st = arg;

  if (st->has_tel && memcmp(&st->tel, msg, sizeof(*msg)) == 0)
    {
      return;
    }

  st->tel     = *msg;
  st->has_tel = true;
  status_rearm(st);
  st->changed(st->arg, STATUS_TEL);
}

static void status_wifi(FAR struct pnut_wifi_info_reader_s *reader,
                        FAR const pnut_wifi_info_t *msg, FAR void *arg)
{
  FAR struct ui_status_s *st = arg;

  if (st->has_wifi && memcmp(&st->wifi, msg, sizeof(*msg)) == 0)
    {
      return;
    }

  st->wifi     = *msg;
  st->has_wifi = true;
  status_rearm(st);
  st->changed(st->arg, STATUS_WIFI);
}

/* A new zone, or the clock stepped: the minute's timer again */

static void status_time(FAR struct pnut_time_change_reader_s *reader,
                        FAR const pnut_time_change_t *msg, FAR void *arg)
{
  FAR struct ui_status_s *st = arg;
  struct pnut_tz_s tz;

  if (pnut_tz_parse(msg->rule, &tz) == 0)
    {
      st->tz       = tz;
      st->has_time = true;
    }

  status_arm(st);
  st->changed(st->arg, STATUS_TIME);
}

static void status_minute(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct ui_status_s *st = arg;

  st->minute = NULL;
  status_arm(st);
  st->changed(st->arg, STATUS_TIME);
}

static void status_arm(FAR struct ui_status_s *st)
{
  time_t now = status_now(st);

  if (st->minute != NULL)
    {
      pnut_timer_cancel(st->loop, st->minute);
      st->minute = NULL;
    }

  pnut_timer_start(st->loop, (uint32_t)(60 - now % 60) * 1000, 0,
                   status_minute, st, &st->minute);
}

/* The minute's timer, if the loop had none to spare when it was set */

static void status_rearm(FAR struct ui_status_s *st)
{
  if (st->minute == NULL)
    {
      status_arm(st);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int status_start(FAR struct ui_status_s *st, FAR struct pnut_loop_s *loop,
                 CODE time_t (*now)(void), status_changed_t changed,
                 FAR void *arg)
{
  int ret;

  memset(st, 0, sizeof(*st));
  st->loop    = loop;
  st->now     = now;
  st->changed = changed;
  st->arg     = arg;
  pnut_tz_parse("UTC0", &st->tz);

  ret = pnut_telephony_info_subscribe(loop, status_tel, st,
                                      &st->tel_reader);
  st->tel_open = ret >= 0;
  if (ret >= 0)
    {
      ret = pnut_wifi_info_subscribe(loop, status_wifi, st,
                                     &st->wifi_reader);
      st->wifi_open = ret >= 0;
    }

  if (ret >= 0)
    {
      ret = pnut_time_change_subscribe(loop, status_time, st,
                                       &st->time_reader);
      st->time_open = ret >= 0;
    }

  if (ret < 0)
    {
      status_stop(st);
      return ret;
    }

  status_arm(st);
  return 0;
}

void status_stop(FAR struct ui_status_s *st)
{
  if (st->minute != NULL)
    {
      pnut_timer_cancel(st->loop, st->minute);
      st->minute = NULL;
    }

  if (st->tel_open)
    {
      pnut_telephony_info_unsubscribe(&st->tel_reader);
      st->tel_open = false;
    }

  if (st->wifi_open)
    {
      pnut_wifi_info_unsubscribe(&st->wifi_reader);
      st->wifi_open = false;
    }

  if (st->time_open)
    {
      pnut_time_change_unsubscribe(&st->time_reader);
      st->time_open = false;
    }
}

time_t status_now(FAR const struct ui_status_s *st)
{
  return st->now != NULL ? st->now() : time(NULL);
}

void status_local(FAR const struct ui_status_s *st, time_t t,
                  FAR struct tm *tm)
{
  int32_t offset;

  pnut_tz_local(&st->tz, t, tm, &offset);
}
