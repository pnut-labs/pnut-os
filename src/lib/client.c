/****************************************************************************
 * pnut-os/src/lib/client.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "pnut_internal.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A call in flight */

struct pnut_call_s
{
  FAR struct pnut_client_s *client;
  uint64_t deadline;              /* Milliseconds, monotonic */
  pnut_reply_t reply;
  pnut_item_t item;               /* A stream's items, or NULL */
  FAR void *arg;
  uint16_t seq;
  uint16_t iface;                 /* A stream's, for its credit */
  uint16_t method;
  uint8_t mversion;
  bool used;
  bool streaming;                 /* Its stream is open: no deadline */
};

/* The client keeps one timer: while it is not connected, for its next try;
 * while it is, for the soonest call's deadline.
 */

struct pnut_client_s
{
  FAR struct pnut_loop_s *loop;
  struct sockaddr_un addr;
  struct pnut_conn_s conn;
  bool connected;
  uint64_t since;                 /* When it connected */
  uint32_t backoff;               /* The next wait before reconnecting */
  FAR struct pnut_timer_s *timer;
  FAR struct pnut_call_s *calls;
  uint8_t ncalls;
  uint16_t seq;
  pnut_event_t event;
  pnut_state_t state;
  FAR void *arg;
  uint8_t dispatching;            /* Handlers running */
  bool closing;                   /* Closed by a handler: freed after */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void client_connect(FAR struct pnut_client_s *client);
static void client_schedule(FAR struct pnut_client_s *client);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: client_free
 ****************************************************************************/

static void client_free(FAR struct pnut_client_s *client)
{
  pnut_timer_drop(client->loop, client->timer);
  pnut_conn_deinit(&client->conn);
  free(client->calls);
  free(client);
}

/****************************************************************************
 * Name: client_enter, client_leave
 *
 * Description:
 *   Around a call of the program's handlers, which may close the client:
 *   it is freed once the last handler has returned.
 *
 * Returned Value (client_leave):
 *   true if the client is gone.
 *
 ****************************************************************************/

static void client_enter(FAR struct pnut_client_s *client)
{
  client->dispatching++;
}

static bool client_leave(FAR struct pnut_client_s *client)
{
  if (--client->dispatching == 0 && client->closing)
    {
      client_free(client);
      return true;
    }

  return client->closing;
}

/****************************************************************************
 * Name: client_finish
 *
 * Description:
 *   End a call: free its place, then give its handler the answer.
 *
 * Returned Value:
 *   true if the client is gone.
 *
 ****************************************************************************/

static bool client_finish(FAR struct pnut_call_s *call, int status,
                          FAR const uint8_t *payload, uint16_t len)
{
  FAR struct pnut_client_s *client = call->client;
  pnut_reply_t reply = call->reply;
  FAR void *arg = call->arg;

  call->used      = false;
  call->streaming = false;

  client_enter(client);
  reply(client, status, payload, len, arg);
  return client_leave(client);
}

/****************************************************************************
 * Name: client_opened, client_item
 *
 * Description:
 *   A stream's call answered OK: open, without a deadline, its reply
 *   handler told.  An item, or the stream's end, which frees the call.
 *   Like client_finish(), true when the client was closed meanwhile.
 *
 ****************************************************************************/

static bool client_opened(FAR struct pnut_call_s *call)
{
  FAR struct pnut_client_s *client = call->client;

  call->streaming = true;
  call->deadline  = UINT64_MAX;
  client_schedule(client);

  client_enter(client);
  call->reply(client, PNUT_STATUS_OK, NULL, 0, call->arg);
  return client_leave(client);
}

static bool client_item(FAR struct pnut_call_s *call, int status,
                        FAR const uint8_t *payload, uint16_t len)
{
  FAR struct pnut_client_s *client = call->client;
  pnut_item_t item = call->item;
  FAR void *arg = call->arg;

  if (status != PNUT_STATUS_OK)
    {
      call->used      = false;
      call->streaming = false;
      payload         = NULL;
      len             = 0;
    }

  client_enter(client);
  item(client, status, payload, len, arg);
  return client_leave(client);
}

/****************************************************************************
 * Name: client_schedule
 *
 * Description:
 *   Set the timer for the soonest call's deadline, or let it idle.  Only
 *   while connected: otherwise the timer is the next try.
 *
 ****************************************************************************/

static void client_schedule(FAR struct pnut_client_s *client)
{
  uint64_t soonest = UINT64_MAX;
  uint64_t now;
  uint8_t i;

  if (!client->connected)
    {
      return;
    }

  for (i = 0; i < client->ncalls; i++)
    {
      if (client->calls[i].used && client->calls[i].deadline < soonest)
        {
          soonest = client->calls[i].deadline;
        }
    }

  if (soonest == UINT64_MAX)
    {
      pnut_timer_clear(client->loop, client->timer);
      return;
    }

  now = pnut_now();
  pnut_timer_set(client->loop, client->timer,
                 soonest > now ? (uint32_t)(soonest - now) : 0);
}

/****************************************************************************
 * Name: client_timer
 *
 * Description:
 *   Try to connect again; or, connected, end the calls past their
 *   deadline.
 *
 ****************************************************************************/

static void client_timer(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct pnut_client_s *client = arg;
  uint64_t now;
  uint8_t i;

  if (!client->connected)
    {
      client_connect(client);
      return;
    }

  now = pnut_now();
  for (i = 0; i < client->ncalls; i++)
    {
      if (client->calls[i].used && client->calls[i].deadline <= now &&
          client_finish(&client->calls[i], PNUT_STATUS_TIMEOUT, NULL, 0))
        {
          return;
        }
    }

  client_schedule(client);
}

/****************************************************************************
 * Name: client_later
 *
 * Description:
 *   Try to connect again after a growing delay.
 *
 ****************************************************************************/

static void client_later(FAR struct pnut_client_s *client)
{
  pnut_timer_set(client->loop, client->timer, client->backoff);

  client->backoff *= 2;
  if (client->backoff > CONFIG_PNUT_LIB_RECONNECT_MAX)
    {
      client->backoff = CONFIG_PNUT_LIB_RECONNECT_MAX;
    }
}

/****************************************************************************
 * Name: client_closed
 *
 * Description:
 *   The connection is lost: the calls in flight fail, the program is told,
 *   and the client tries again.
 *
 ****************************************************************************/

static void client_closed(FAR struct pnut_conn_s *conn)
{
  FAR struct pnut_client_s *client = conn->owner;
  uint8_t i;

  client->connected = false;

  for (i = 0; i < client->ncalls; i++)
    {
      if (client->calls[i].used &&
          (client->calls[i].streaming ?
           client_item(&client->calls[i], PNUT_STATUS_UNAVAILABLE, NULL,
                       0) :
           client_finish(&client->calls[i], PNUT_STATUS_UNAVAILABLE, NULL,
                         0)))
        {
          return;
        }
    }

  if (client->state != NULL)
    {
      client_enter(client);
      client->state(client, false, client->arg);
      if (client_leave(client))
        {
          return;
        }
    }

  /* Only a connection that lasted starts the delays again: one a service
   * closes at once, when it has as many as it takes, makes them grow
   */

  if (!client->closing)
    {
      if (pnut_now() - client->since >= client->backoff)
        {
          client->backoff = CONFIG_PNUT_LIB_RECONNECT_MIN;
        }

      client_later(client);
    }
}

/****************************************************************************
 * Name: client_message
 ****************************************************************************/

static int client_message(FAR struct pnut_conn_s *conn,
                          FAR const struct pnut_msghdr_s *hdr,
                          FAR const uint8_t *payload)
{
  FAR struct pnut_client_s *client = conn->owner;
  uint8_t i;

  if (hdr->kind == PNUT_KIND_REPLY)
    {
      for (i = 0; i < client->ncalls; i++)
        {
          if (client->calls[i].used && !client->calls[i].streaming &&
              client->calls[i].seq == hdr->seq)
            {
              /* A stream's call stays, open, on an OK answer */

              if (client->calls[i].item != NULL &&
                  hdr->status == PNUT_STATUS_OK)
                {
                  return client_opened(&client->calls[i]) ?
                         PNUT_CONN_GONE : PNUT_CONN_NEXT;
                }

              /* Its deadline goes with it; the timer finds it gone */

              return client_finish(&client->calls[i], hdr->status,
                                   payload, hdr->len) ?
                     PNUT_CONN_GONE : PNUT_CONN_NEXT;
            }
        }

      /* A reply after its call timed out is dropped */
    }
  else if (hdr->kind == PNUT_KIND_STREAM)
    {
      for (i = 0; i < client->ncalls; i++)
        {
          if (client->calls[i].used && client->calls[i].streaming &&
              client->calls[i].seq == hdr->seq)
            {
              return client_item(&client->calls[i], hdr->status, payload,
                                 hdr->len) ?
                     PNUT_CONN_GONE : PNUT_CONN_NEXT;
            }
        }

      /* An item for a stream the client no longer has (cancelled, or
       * its opening timed out): dropped, and the service told it is over
       */

      if (hdr->status == PNUT_STATUS_OK)
        {
          struct pnut_msghdr_s end = *hdr;

          end.status = PNUT_STATUS_ENDED;
          end.credit = 0;
          end.len    = 0;
          client_enter(client);
          pnut_conn_send(&client->conn, &end, NULL);
          if (client_leave(client))
            {
              return PNUT_CONN_GONE;
            }
        }
    }
  else if (hdr->kind == PNUT_KIND_EVENT && client->event != NULL)
    {
      client_enter(client);
      client->event(client, hdr->iface, hdr->method, payload, hdr->len,
                    client->arg);
      return client_leave(client) ? PNUT_CONN_GONE : PNUT_CONN_NEXT;
    }

  return PNUT_CONN_NEXT;
}

/****************************************************************************
 * Name: client_connect
 ****************************************************************************/

static void client_connect(FAR struct pnut_client_s *client)
{
  int fd;

  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    {
      client_later(client);
      return;
    }

  if (connect(fd, (FAR struct sockaddr *)&client->addr,
              sizeof(client->addr)) < 0 ||
      pnut_conn_attach(&client->conn, fd) < 0)
    {
      close(fd);
      client_later(client);
      return;
    }

  client->connected = true;
  client->since     = pnut_now();

  if (client->state != NULL)
    {
      client_enter(client);
      client->state(client, true, client->arg);
      client_leave(client);
    }
}

/****************************************************************************
 * Name: client_seq
 *
 * Description:
 *   The next request's number, past those of the streams still open: a
 *   stream keeps its number while it lasts.
 *
 ****************************************************************************/

static uint16_t client_seq(FAR struct pnut_client_s *client)
{
  uint8_t i;

  for (; ; )
    {
      client->seq++;
      for (i = 0; i < client->ncalls; i++)
        {
          if (client->calls[i].used && client->calls[i].seq == client->seq)
            {
              break;
            }
        }

      if (i == client->ncalls)
        {
          return client->seq;
        }
    }
}

/****************************************************************************
 * Name: client_call
 *
 * Description:
 *   A call, or a stream's when item is not NULL.
 *
 ****************************************************************************/

static int client_call(FAR struct pnut_client_s *client, uint16_t iface,
                       uint16_t method, uint8_t mversion,
                       FAR const void *payload, uint16_t len,
                       uint16_t credit, uint32_t timeout,
                       pnut_reply_t reply, pnut_item_t item, FAR void *arg,
                       FAR uint16_t *seqp)
{
  FAR struct pnut_call_s *call = NULL;
  struct pnut_msghdr_s hdr;
  uint8_t i;
  int ret;

  if (reply == NULL)
    {
      return -EINVAL;
    }

  if (!client->connected || client->closing)
    {
      return -ENOTCONN;
    }

  if (len > PNUT_MSG_PAYLOAD_MAX)
    {
      return -EMSGSIZE;
    }

  for (i = 0; i < client->ncalls; i++)
    {
      if (!client->calls[i].used)
        {
          call = &client->calls[i];
          break;
        }
    }

  if (call == NULL)
    {
      return -EBUSY;
    }

  memset(&hdr, 0, sizeof(hdr));
  hdr.version  = PNUT_MSG_VERSION;
  hdr.kind     = PNUT_KIND_REQUEST;
  hdr.iface    = iface;
  hdr.method   = method;
  hdr.mversion = mversion;
  hdr.seq      = client_seq(client);
  hdr.len      = len;
  hdr.credit   = credit;

  /* A connection that fails while sending tells the program through the
   * state handler, which may close the client: hold it until the end.
   * The call counts only once sent, so that a failure is reported once,
   * by the returned value.
   */

  client_enter(client);

  ret = pnut_conn_send(&client->conn, &hdr, payload);
  if (ret == OK)
    {
      call->used      = true;
      call->streaming = false;
      call->seq       = hdr.seq;
      call->iface     = iface;
      call->method    = method;
      call->mversion  = mversion;
      call->reply     = reply;
      call->item      = item;
      call->arg       = arg;
      call->deadline  = pnut_now() + (timeout > 0 ? timeout :
                                      CONFIG_PNUT_LIB_CALL_TIMEOUT);
      client_schedule(client);
      if (seqp != NULL)
        {
          *seqp = hdr.seq;
        }
    }

  client_leave(client);
  return ret < 0 ? ret : OK;
}

/****************************************************************************
 * Name: client_tell
 *
 * Description:
 *   A word to the service about an open stream: credit, or its end.
 *
 ****************************************************************************/

static int client_tell(FAR struct pnut_client_s *client, uint16_t seq,
                       uint16_t credit, int status)
{
  FAR struct pnut_call_s *call = NULL;
  struct pnut_msghdr_s hdr;
  uint8_t i;
  int ret;

  if (!client->connected || client->closing)
    {
      return -ENOTCONN;
    }

  for (i = 0; i < client->ncalls; i++)
    {
      if (client->calls[i].used && client->calls[i].streaming &&
          client->calls[i].seq == seq)
        {
          call = &client->calls[i];
          break;
        }
    }

  if (call == NULL)
    {
      return -ENOENT;
    }

  memset(&hdr, 0, sizeof(hdr));
  hdr.version  = PNUT_MSG_VERSION;
  hdr.kind     = PNUT_KIND_STREAM;
  hdr.iface    = call->iface;
  hdr.method   = call->method;
  hdr.mversion = call->mversion;
  hdr.seq      = seq;
  hdr.status   = status;
  hdr.credit   = credit;

  /* Cancelled, the stream is over here whether the service hears of it
   * or not: an item that still comes is answered with its end
   */

  if (status != PNUT_STATUS_OK)
    {
      call->used      = false;
      call->streaming = false;
    }

  client_enter(client);
  ret = pnut_conn_send(&client->conn, &hdr, NULL);
  client_leave(client);
  return ret < 0 ? ret : OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int pnut_client_open(FAR struct pnut_loop_s *loop, FAR const char *name,
                     uint8_t inflight, pnut_event_t event,
                     pnut_state_t state, FAR void *arg,
                     FAR struct pnut_client_s **clientp)
{
  FAR struct pnut_client_s *client;
  uint8_t i;
  int ret;

  *clientp = NULL;

  client = calloc(1, sizeof(*client));
  if (client == NULL)
    {
      return -ENOMEM;
    }

  client->loop    = loop;
  client->event   = event;
  client->state   = state;
  client->arg     = arg;
  client->backoff = CONFIG_PNUT_LIB_RECONNECT_MIN;
  client->ncalls  = inflight > 0 ? inflight : CONFIG_PNUT_LIB_INFLIGHT;

  client->addr.sun_family = AF_UNIX;
  ret = snprintf(client->addr.sun_path, sizeof(client->addr.sun_path),
                 "%s/%s", loop->config.rundir, name);
  if (ret < 0 || ret >= (int)sizeof(client->addr.sun_path))
    {
      free(client);
      return -ENAMETOOLONG;
    }

  client->calls = calloc(client->ncalls, sizeof(struct pnut_call_s));
  ret = pnut_conn_init(&client->conn, loop, CONFIG_PNUT_LIB_SENDBUF);
  if (client->calls == NULL || ret < 0)
    {
      client_free(client);
      return -ENOMEM;
    }

  for (i = 0; i < client->ncalls; i++)
    {
      client->calls[i].client = client;
    }

  client->conn.message = client_message;
  client->conn.closed  = client_closed;
  client->conn.owner   = client;

  ret = pnut_timer_keep(loop, client_timer, client, &client->timer);
  if (ret < 0)
    {
      client_free(client);
      return ret;
    }

  /* The first try is made from the loop, so that the state handler never
   * runs before the program has the client.
   */

  pnut_timer_set(loop, client->timer, 0);

  *clientp = client;
  return OK;
}

void pnut_client_close(FAR struct pnut_client_s *client)
{
  uint8_t i;

  if (client == NULL || client->closing)
    {
      return;
    }

  pnut_timer_clear(client->loop, client->timer);

  for (i = 0; i < client->ncalls; i++)
    {
      client->calls[i].used = false;
    }

  pnut_conn_close(&client->conn);
  client->connected = false;

  if (client->dispatching > 0)
    {
      client->closing = true;
      return;
    }

  client_free(client);
}

bool pnut_client_connected(FAR struct pnut_client_s *client)
{
  return client->connected;
}

int pnut_call(FAR struct pnut_client_s *client, uint16_t iface,
              uint16_t method, uint8_t mversion, FAR const void *payload,
              uint16_t len, uint32_t timeout, pnut_reply_t reply,
              FAR void *arg)
{
  return client_call(client, iface, method, mversion, payload, len, 0,
                     timeout, reply, NULL, arg, NULL);
}

int pnut_stream_call(FAR struct pnut_client_s *client, uint16_t iface,
                     uint16_t method, uint8_t mversion,
                     FAR const void *payload, uint16_t len, uint16_t credit,
                     uint32_t timeout, pnut_reply_t reply, pnut_item_t item,
                     FAR void *arg, FAR uint16_t *seqp)
{
  if (item == NULL)
    {
      return -EINVAL;
    }

  return client_call(client, iface, method, mversion, payload, len, credit,
                     timeout, reply, item, arg, seqp);
}

int pnut_stream_credit(FAR struct pnut_client_s *client, uint16_t seq,
                       uint16_t credit)
{
  if (credit == 0)
    {
      return OK;
    }

  return client_tell(client, seq, credit, PNUT_STATUS_OK);
}

int pnut_stream_cancel(FAR struct pnut_client_s *client, uint16_t seq)
{
  return client_tell(client, seq, 0, PNUT_STATUS_ENDED);
}
