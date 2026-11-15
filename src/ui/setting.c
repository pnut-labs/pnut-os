/****************************************************************************
 * pnut-os/src/ui/setting.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Other programs' settings, as the system UI reads and changes them
 * (RFC 0025): Settings' Get and Set, and its settings topic.  The values
 * are read one at a time, as Settings is reached and whenever the topic
 * tells of a change to their owner; a change asked comes back through the
 * topic, never by itself.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <string.h>

#include "setting.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Settings' service, and how long it may take to answer */

#define SETTING_SERVICE  "settings"
#define SETTING_TIMEOUT  5000

/* A read Settings could not answer now (its owner registering, the caller
 * not told yet), or that went unanswered, is tried again this much later
 */

#define SETTING_RETRY_MS  1000

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void setting_next(FAR struct setting_watch_s *w);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool setting_same(FAR const pnut_setting_value_t *a,
                         FAR const pnut_setting_value_t *b)
{
  if (a->which_value != b->which_value)
    {
      return false;
    }

  switch (a->which_value)
    {
      case PNUT_SETTING_VALUE_BOOLEAN_TAG:
        return a->value.boolean == b->value.boolean;

      case PNUT_SETTING_VALUE_INTEGER_TAG:
        return a->value.integer == b->value.integer;

      case PNUT_SETTING_VALUE_TEXT_TAG:
        return strcmp(a->value.text, b->value.text) == 0;

      default:
        return true;
    }
}

static void setting_retry(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct setting_watch_s *w = arg;

  w->retry    = NULL;
  w->pending |= w->again;
  w->again    = 0;
  setting_next(w);
}

static void setting_got(FAR struct pnut_settings_client_s *client,
                        int status, FAR const pnut_setting_value_t *out,
                        FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct setting_watch_s *w = arg;
  FAR struct setting_s *s = &w->list[w->current];
  bool changed;

  w->reading = false;
  if (status == PNUT_STATUS_UNAVAILABLE || status == PNUT_STATUS_TIMEOUT)
    {
      w->again |= 1u << w->current;
      if (w->retry == NULL)
        {
          pnut_timer_start(w->loop, SETTING_RETRY_MS, 0, setting_retry, w,
                           &w->retry);
        }
    }

  if (status == PNUT_STATUS_OK && out != NULL)
    {
      changed  = !s->known || !setting_same(&s->value, out);
      s->value = *out;
      s->known = true;
      if (changed)
        {
          w->changed(w->arg, w->current);
        }
    }

  setting_next(w);
}

/* Read the next setting waiting, one at a time */

static void setting_next(FAR struct setting_watch_s *w)
{
  pnut_settings_get_request_t req;
  int i;

  if (w->reading || !pnut_client_connected(w->client.client))
    {
      return;
    }

  for (i = 0; i < w->count; i++)
    {
      if ((w->pending & (1u << i)) == 0)
        {
          continue;
        }

      memset(&req, 0, sizeof(req));
      strlcpy(req.owner, w->list[i].owner, sizeof(req.owner));
      strlcpy(req.key, w->list[i].key, sizeof(req.key));
      if (pnut_settings_get(&w->client, &req, SETTING_TIMEOUT, setting_got,
                            w) < 0)
        {
          return;
        }

      w->pending &= ~(1u << i);
      w->current  = i;
      w->reading  = true;
      return;
    }
}

/* Settings reached (again): every value read anew */

static void setting_state(FAR struct pnut_settings_client_s *client,
                          bool connected, FAR void *arg)
{
  FAR struct setting_watch_s *w = arg;

  if (connected)
    {
      w->pending = (1u << w->count) - 1;
      w->reading = false;
      setting_next(w);
    }
}

/* An owner's settings changed: its watched ones read again */

static void setting_topic(FAR struct pnut_settings_change_reader_s *reader,
                          FAR const pnut_settings_change_t *msg,
                          FAR void *arg)
{
  FAR struct setting_watch_s *w = arg;
  int i;

  for (i = 0; i < w->count; i++)
    {
      if (strcmp(w->list[i].owner, msg->owner) == 0 &&
          (msg->key[0] == '\0' || strcmp(w->list[i].key, msg->key) == 0))
        {
          w->pending |= 1u << i;
        }
    }

  setting_next(w);
}

static void setting_set_done(FAR struct pnut_settings_client_s *client,
                             int status,
                             FAR const pnut_settings_reply_t *out,
                             FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct setting_call_s *call = arg;

  call->used = false;
  if (call->done != NULL)
    {
      call->done(call->arg, status, err != NULL ? err->code : 0);
    }
}

static int setting_set(FAR struct setting_watch_s *w, int index,
                       FAR const pnut_setting_value_t *value,
                       setting_done_t done, FAR void *arg)
{
  pnut_settings_set_request_t req;
  FAR struct setting_call_s *call = NULL;
  int ret;
  int i;

  if (index < 0 || index >= w->count)
    {
      return -EINVAL;
    }

  for (i = 0; i < PNUT_CLIENT_INFLIGHT && call == NULL; i++)
    {
      if (!w->calls[i].used)
        {
          call = &w->calls[i];
        }
    }

  if (call == NULL)
    {
      return -EBUSY;
    }

  memset(&req, 0, sizeof(req));
  strlcpy(req.owner, w->list[index].owner, sizeof(req.owner));
  strlcpy(req.key, w->list[index].key, sizeof(req.key));
  req.has_value = true;
  req.value     = *value;

  call->w    = w;
  call->done = done;
  call->arg  = arg;
  ret = pnut_settings_set(&w->client, &req, SETTING_TIMEOUT,
                          setting_set_done, call);
  call->used = ret >= 0;
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int setting_open(FAR struct setting_watch_s *w, FAR struct pnut_loop_s *loop,
                 FAR struct setting_s *list, uint8_t count,
                 setting_changed_t changed, FAR void *arg)
{
  int ret;

  if (count > SETTING_MAX)
    {
      return -EINVAL;
    }

  memset(w, 0, sizeof(*w));
  w->list    = list;
  w->count   = count;
  w->changed = changed;
  w->arg     = arg;
  w->loop    = loop;

  ret = pnut_settings_change_subscribe(loop, setting_topic, w, &w->reader);
  if (ret < 0)
    {
      return ret;
    }

  w->reader_open = true;
  ret = pnut_settings_connect(loop, SETTING_SERVICE, setting_state, w,
                              &w->client);
  if (ret < 0)
    {
      setting_close(w);
      return ret;
    }

  w->client_open = true;
  return 0;
}

void setting_close(FAR struct setting_watch_s *w)
{
  if (w->retry != NULL)
    {
      pnut_timer_cancel(w->loop, w->retry);
      w->retry = NULL;
    }

  if (w->client_open)
    {
      pnut_settings_disconnect(&w->client);
      w->client_open = false;
    }

  if (w->reader_open)
    {
      pnut_settings_change_unsubscribe(&w->reader);
      w->reader_open = false;
    }
}

int setting_set_bool(FAR struct setting_watch_s *w, int index, bool value,
                     setting_done_t done, FAR void *arg)
{
  pnut_setting_value_t v;

  memset(&v, 0, sizeof(v));
  v.which_value   = PNUT_SETTING_VALUE_BOOLEAN_TAG;
  v.value.boolean = value;
  return setting_set(w, index, &v, done, arg);
}

int setting_set_text(FAR struct setting_watch_s *w, int index,
                     FAR const char *text, setting_done_t done,
                     FAR void *arg)
{
  pnut_setting_value_t v;

  memset(&v, 0, sizeof(v));
  v.which_value = PNUT_SETTING_VALUE_TEXT_TAG;
  strlcpy(v.value.text, text, sizeof(v.value.text));
  return setting_set(w, index, &v, done, arg);
}
