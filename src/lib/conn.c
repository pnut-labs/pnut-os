/****************************************************************************
 * pnut-os/src/lib/conn.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "pnut_internal.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: conn_lost
 *
 * Description:
 *   The peer has gone, or the connection failed: close it, and tell the
 *   owner, which may go too: nothing touches the connection after.
 *
 ****************************************************************************/

static void conn_lost(FAR struct pnut_conn_s *conn)
{
  pnut_conn_close(conn);
  if (conn->closed != NULL)
    {
      conn->closed(conn);
    }
}

/****************************************************************************
 * Name: conn_watch
 *
 * Description:
 *   Have the loop watch for what the connection waits for: messages, and
 *   room to send what is left.  A held connection reads nothing: it waits
 *   for room to send, which is what lets it go on, or, with nothing left
 *   to send, for the owner's word that room was given back, shown by the
 *   socket being ready to write.  Watching nothing, it sees its peer go
 *   at once on the computer, but on NuttX only once it is watched again:
 *   at the owner's next answer, which it owes.
 *
 * Returned Value:
 *   Zero (OK), or a negated errno value from the loop.
 *
 ****************************************************************************/

static int conn_watch(FAR struct pnut_conn_s *conn)
{
  uint32_t events;
  int ret;

  if (conn->held)
    {
      events = conn->txlen > 0 || conn->recheck ? EPOLLOUT : 0;
    }
  else
    {
      events = EPOLLIN | (conn->txlen > 0 ? EPOLLOUT : 0);
    }

  if (events == conn->events)
    {
      return OK;
    }

  ret = pnut_loop_rewatch(conn->loop, conn->fd, events);
  if (ret < 0)
    {
      return ret;
    }

  conn->events = events;
  return OK;
}

/****************************************************************************
 * Name: conn_flush
 *
 * Description:
 *   Send what the socket takes now, and watch for room for the rest.
 *
 * Returned Value:
 *   Zero (OK), or a negated errno value when the connection failed.
 *
 ****************************************************************************/

static int conn_flush(FAR struct pnut_conn_s *conn)
{
  ssize_t n;

  while (conn->txlen > 0)
    {
      n = send(conn->fd, conn->tx, conn->txlen, MSG_NOSIGNAL);
      if (n < 0)
        {
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              break;
            }

          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      conn->txlen -= n;
      memmove(conn->tx, conn->tx + n, conn->txlen);
    }

  return conn_watch(conn);
}

/****************************************************************************
 * Name: conn_receive
 *
 * Description:
 *   Hand each whole message there is to the owner, reading more as it
 *   goes; stop, held, at one the owner leaves for later.
 *
 ****************************************************************************/

static void conn_receive(FAR struct pnut_conn_s *conn)
{
  struct pnut_msghdr_s hdr;
  uint32_t gen = conn->gen;
  size_t size;
  ssize_t n;
  int next;

  for (; ; )
    {
      while (conn->rxlen >= PNUT_MSG_HEADER)
        {
          if (pnut_msg_decode(conn->rx, &hdr) < 0)
            {
              pnut_loop_log(conn->loop, PNUT_LOG_WARNING,
                            "A malformed message: closing its connection");
              conn_lost(conn);
              return;
            }

          size = PNUT_MSG_HEADER + hdr.len;
          if (conn->rxlen < size)
            {
              break;
            }

          next = conn->message(conn, &hdr, conn->rx + PNUT_MSG_HEADER);

          /* Left for later: it stays first in the buffer, and the
           * messages after it wait in the socket
           */

          if (next == PNUT_CONN_WAIT)
            {
              conn->held    = true;
              conn->recheck = false;
              if (conn_watch(conn) < 0)
                {
                  conn_lost(conn);
                }

              return;
            }

          /* The owner may have gone, or closed the connection */

          if (next != PNUT_CONN_NEXT || conn->fd < 0 || conn->gen != gen)
            {
              return;
            }

          conn->rxlen -= size;
          memmove(conn->rx, conn->rx + size, conn->rxlen);
        }

      /* Never asked for nothing: a full buffer holds a whole message */

      n = read(conn->fd, conn->rx + conn->rxlen,
               PNUT_MSG_MAX - conn->rxlen);
      if (n == 0)
        {
          conn_lost(conn);
          return;
        }

      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
              conn_lost(conn);
            }

          return;
        }

      conn->rxlen += n;
    }
}

/****************************************************************************
 * Name: conn_ready
 ****************************************************************************/

static void conn_ready(FAR struct pnut_loop_s *loop, int fd,
                       uint32_t events, FAR void *arg)
{
  FAR struct pnut_conn_s *conn = arg;
  uint32_t gen = conn->gen;

  if ((events & EPOLLOUT) != 0 && conn_flush(conn) < 0)
    {
      conn_lost(conn);
      return;
    }

  if (conn->fd < 0 || conn->gen != gen)
    {
      return;
    }

  if (conn->held)
    {
      /* A peer gone reads no answer: drop it, rather than wait for room
       * that never comes
       */

      if ((events & (EPOLLHUP | EPOLLERR)) != 0)
        {
          conn_lost(conn);
          return;
        }

      if ((events & EPOLLOUT) == 0)
        {
          return;
        }

      /* Room, maybe: the message left first, which the owner may leave
       * again
       */

      conn->held    = false;
      conn->recheck = false;
      if (conn_watch(conn) < 0)
        {
          conn_lost(conn);
          return;
        }

      conn_receive(conn);
      return;
    }

  if ((events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0)
    {
      conn_receive(conn);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int pnut_conn_init(FAR struct pnut_conn_s *conn,
                   FAR struct pnut_loop_s *loop, size_t txsize)
{
  memset(conn, 0, sizeof(*conn));
  conn->loop   = loop;
  conn->fd     = -1;
  conn->txsize = txsize < PNUT_MSG_MAX ? PNUT_MSG_MAX : txsize;
  conn->rx     = malloc(PNUT_MSG_MAX);
  conn->tx     = malloc(conn->txsize);

  if (conn->rx == NULL || conn->tx == NULL)
    {
      pnut_conn_deinit(conn);
      return -ENOMEM;
    }

  return OK;
}

void pnut_conn_deinit(FAR struct pnut_conn_s *conn)
{
  pnut_conn_close(conn);
  free(conn->rx);
  free(conn->tx);
  conn->rx = NULL;
  conn->tx = NULL;
}

/****************************************************************************
 * Name: pnut_conn_attach
 *
 * Description:
 *   Take a connected, non-blocking socket, and watch it.
 *
 ****************************************************************************/

int pnut_conn_attach(FAR struct pnut_conn_s *conn, int fd)
{
  int ret;

  conn->rxlen    = 0;
  conn->txlen    = 0;
  conn->held     = false;
  conn->recheck  = false;
  conn->pending  = 0;
  conn->reserved = 0;

  ret = pnut_loop_watch(conn->loop, fd, EPOLLIN, conn_ready, conn);
  if (ret < 0)
    {
      return ret;
    }

  conn->fd     = fd;
  conn->events = EPOLLIN;
  return OK;
}

/****************************************************************************
 * Name: pnut_conn_close
 *
 * Description:
 *   Close the socket, if open, and drop what was not sent.  The owner is
 *   not told.
 *
 ****************************************************************************/

void pnut_conn_close(FAR struct pnut_conn_s *conn)
{
  if (conn->fd >= 0)
    {
      pnut_loop_unwatch(conn->loop, conn->fd);
      close(conn->fd);
      conn->fd = -1;
      conn->gen++;
    }

  conn->rxlen    = 0;
  conn->txlen    = 0;
  conn->events   = 0;
  conn->held     = false;
  conn->recheck  = false;
  conn->pending  = 0;
  conn->reserved = 0;
}

/****************************************************************************
 * Name: pnut_conn_send
 *
 * Description:
 *   Queue a message and send what the socket takes now.
 *
 * Returned Value:
 *   Zero (OK); -ENOTCONN; -EMSGSIZE; -EBUSY when the send buffer has no
 *   room for it; another negated errno value if the connection failed,
 *   and then it is closed and its owner told.
 *
 ****************************************************************************/

int pnut_conn_send(FAR struct pnut_conn_s *conn,
                   FAR const struct pnut_msghdr_s *hdr,
                   FAR const void *payload)
{
  size_t size = PNUT_MSG_HEADER + hdr->len;
  int ret;

  if (conn->fd < 0)
    {
      return -ENOTCONN;
    }

  if (hdr->len > PNUT_MSG_PAYLOAD_MAX)
    {
      return -EMSGSIZE;
    }

  if (conn->txsize - conn->txlen < size)
    {
      return -EBUSY;
    }

  pnut_msg_encode(hdr, conn->tx + conn->txlen);
  if (hdr->len > 0)
    {
      memcpy(conn->tx + conn->txlen + PNUT_MSG_HEADER, payload, hdr->len);
    }

  conn->txlen += size;

  ret = conn_flush(conn);
  if (ret < 0)
    {
      conn_lost(conn);
    }

  return ret;
}

/****************************************************************************
 * Name: pnut_conn_recheck
 *
 * Description:
 *   The owner gave back room a message left for later may have waited
 *   for: offer it again, from the loop, once the socket is ready to write.
 *   That waits for the peer to read only when the socket's own buffer is
 *   full of answers to it, which it must read anyway.
 *
 ****************************************************************************/

void pnut_conn_recheck(FAR struct pnut_conn_s *conn)
{
  if (conn->fd < 0 || !conn->held || conn->recheck)
    {
      return;
    }

  conn->recheck = true;
  if (conn_watch(conn) < 0)
    {
      conn_lost(conn);
    }
}
