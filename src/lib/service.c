/****************************************************************************
 * pnut-os/src/lib/service.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

/* accept4() and struct ucred, on the computer */

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "pnut_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* How long accepting stops after it failed for want of descriptors or
 * memory: the connection waits in the backlog meanwhile, rather than the
 * loop spinning on it.
 */

#define SERVICE_ACCEPT_PAUSE  100

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct pnut_service_s
{
  FAR struct pnut_loop_s *loop;
  int fd;
  struct sockaddr_un addr;
  FAR const struct pnut_method_s *methods;
  size_t nmethods;
  FAR void *arg;
  FAR struct pnut_conn_s *conns;
  uint8_t nconns;
  uint8_t dispatching;            /* Handlers running */
  bool closing;                   /* Closed by a handler: freed after */
  bool failing;                   /* Accepting fails; logged once */
  FAR struct pnut_timer_s *pause; /* Accepting again after a failure */

  /* The streams open on its connections */

  FAR struct pnut_stream_s *streams;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: service_free
 ****************************************************************************/

static void service_free(FAR struct pnut_service_s *service)
{
  uint8_t i;

  if (service->conns != NULL)
    {
      for (i = 0; i < service->nconns; i++)
        {
          pnut_conn_deinit(&service->conns[i]);
        }
    }

  pnut_timer_drop(service->loop, service->pause);
  free(service->conns);
  free(service);
}

/****************************************************************************
 * Name: service_hold, service_release
 *
 * Description:
 *   Keep the service while the library works on it: a send that fails
 *   tells its streams' modules (service_closed()), which may close the
 *   service; it is freed once nothing holds it.
 *
 ****************************************************************************/

static FAR struct pnut_service_s *service_hold(FAR struct pnut_conn_s *conn)
{
  FAR struct pnut_service_s *service = conn->owner;

  service->dispatching++;
  return service;
}

static bool service_release(FAR struct pnut_service_s *service)
{
  service->dispatching--;
  if (service->closing && service->dispatching == 0)
    {
      service_free(service);
      return true;
    }

  return false;
}

/****************************************************************************
 * Name: service_answer
 *
 * Description:
 *   Answer a request with a status and no body, for the library's own
 *   answers.
 *
 ****************************************************************************/

static void service_answer(FAR struct pnut_conn_s *conn,
                           FAR const struct pnut_msghdr_s *req, int status)
{
  struct pnut_msghdr_s hdr;

  memset(&hdr, 0, sizeof(hdr));
  hdr.version  = PNUT_MSG_VERSION;
  hdr.kind     = PNUT_KIND_REPLY;
  hdr.iface    = req->iface;
  hdr.method   = req->method;
  hdr.mversion = req->mversion;
  hdr.status   = status;
  hdr.seq      = req->seq;

  if (pnut_conn_send(conn, &hdr, NULL) == -EBUSY)
    {
      pnut_conn_close(conn);
    }
}

/****************************************************************************
 * Name: service_room
 *
 * Description:
 *   Whether the connection's send buffer has room for an answer of size
 *   bytes, beside the room kept for the answers it owes.
 *
 ****************************************************************************/

static bool service_room(FAR struct pnut_conn_s *conn, size_t size)
{
  return conn->txsize - conn->txlen >= conn->reserved + size;
}

/****************************************************************************
 * Name: service_unlink
 *
 * Description:
 *   A stream taken off its service's list, and closed.
 *
 ****************************************************************************/

static void service_unlink(FAR struct pnut_stream_s *stream)
{
  FAR struct pnut_conn_s *conn = stream->to.conn;
  FAR struct pnut_stream_s **pp;

  if (stream->service != NULL)
    {
      for (pp = &stream->service->streams; *pp != NULL; pp = &(*pp)->next)
        {
          if (*pp == stream)
            {
              *pp = stream->next;
              break;
            }
        }

      /* The room kept for its end is given back */

      if (conn->fd >= 0 && conn->gen == stream->to.gen)
        {
          conn->reserved -= conn->reserved > PNUT_MSG_HEADER ?
                            PNUT_MSG_HEADER : conn->reserved;
        }
    }

  stream->open    = false;
  stream->service = NULL;
  stream->next    = NULL;
}

/****************************************************************************
 * Name: service_over
 *
 * Description:
 *   A stream over, not by its module: unlinked, and the module told.
 *
 ****************************************************************************/

static void service_over(FAR struct pnut_stream_s *stream, int status)
{
  pnut_stream_closed_t closed = stream->closed;
  FAR void *arg = stream->arg;

  service_unlink(stream);
  if (closed != NULL)
    {
      closed(stream, status, arg);
    }
}

/****************************************************************************
 * Name: service_stream
 *
 * Description:
 *   A caller's word about one of its streams: credit, or its end.
 *
 ****************************************************************************/

static void service_stream(FAR struct pnut_service_s *service,
                           FAR struct pnut_conn_s *conn,
                           FAR const struct pnut_msghdr_s *hdr)
{
  FAR struct pnut_stream_s *s;
  uint32_t credit;

  for (s = service->streams; s != NULL; s = s->next)
    {
      if (s->to.conn == conn && s->to.gen == conn->gen &&
          s->seq == hdr->seq && s->iface == hdr->iface &&
          s->method == hdr->method)
        {
          break;
        }
    }

  if (s == NULL)
    {
      /* Over already: credit for it is answered with its end, which the
       * caller may have missed
       */

      if (hdr->status == PNUT_STATUS_OK &&
          service_room(conn, PNUT_MSG_HEADER))
        {
          struct pnut_msghdr_s end = *hdr;

          end.kind   = PNUT_KIND_STREAM;
          end.status = PNUT_STATUS_ENDED;
          end.credit = 0;
          end.len    = 0;
          pnut_conn_send(conn, &end, NULL);
        }

      return;
    }

  if (hdr->status != PNUT_STATUS_OK)
    {
      service_over(s, hdr->status);
    }
  else if (hdr->credit > 0)
    {
      credit    = (uint32_t)s->credit + hdr->credit;
      s->credit = credit > UINT16_MAX ? UINT16_MAX : (uint16_t)credit;
      if (s->ready != NULL)
        {
          s->ready(s, s->arg);
        }
    }
}

/****************************************************************************
 * Name: service_closed
 *
 * Description:
 *   A caller has gone: its streams are over.
 *
 ****************************************************************************/

static void service_closed(FAR struct pnut_conn_s *conn)
{
  FAR struct pnut_service_s *service = service_hold(conn);
  FAR struct pnut_stream_s *s;

  do
    {
      for (s = service->streams; s != NULL; s = s->next)
        {
          if (s->to.conn == conn)
            {
              service_over(s, PNUT_STATUS_UNAVAILABLE);
              break;              /* The list changed: from its start */
            }
        }
    }
  while (s != NULL && !service->closing);

  service_release(service);
}

/****************************************************************************
 * Name: service_dispatch
 *
 * Description:
 *   A message on one of the service's connections: hand a request to its
 *   method's handler, with room kept for its answer.  One there is no
 *   room for yet is left for later.
 *
 ****************************************************************************/

static int service_dispatch(FAR struct pnut_conn_s *conn,
                            FAR const struct pnut_msghdr_s *hdr,
                            FAR const uint8_t *payload)
{
  FAR struct pnut_service_s *service = conn->owner;
  FAR const struct pnut_method_s *method = NULL;
  struct pnut_request_s req;
  bool other = false;
  uint16_t reserve;
  size_t i;

  if (hdr->kind == PNUT_KIND_STREAM)
    {
      service_stream(service, conn, hdr);
      return PNUT_CONN_NEXT;
    }

  if (hdr->kind != PNUT_KIND_REQUEST)
    {
      return PNUT_CONN_NEXT;
    }

  for (i = 0; i < service->nmethods; i++)
    {
      if (service->methods[i].iface == hdr->iface &&
          service->methods[i].method == hdr->method)
        {
          if (service->methods[i].mversion == hdr->mversion)
            {
              method = &service->methods[i];
              break;
            }

          other = true;
        }
    }

  if (method == NULL)
    {
      if (!service_room(conn, PNUT_MSG_HEADER))
        {
          return PNUT_CONN_WAIT;
        }

      service_answer(conn, hdr, other ? PNUT_STATUS_INVALID :
                                        PNUT_STATUS_NOTFOUND);
      return PNUT_CONN_NEXT;
    }

  /* As many requests as a client may have in flight are not answered yet:
   * one more is answered busy at once (RFC 0023)
   */

  if (conn->pending >= CONFIG_PNUT_LIB_INFLIGHT)
    {
      if (!service_room(conn, PNUT_MSG_HEADER))
        {
          return PNUT_CONN_WAIT;
        }

      service_answer(conn, hdr, PNUT_STATUS_BUSY);
      return PNUT_CONN_NEXT;
    }

  /* Room for its answer, kept until it is given, now or later */

  reserve = method->replymax;
  if (reserve == 0 || reserve > PNUT_MSG_PAYLOAD_MAX)
    {
      reserve = PNUT_MSG_PAYLOAD_MAX;
    }

  reserve += PNUT_MSG_HEADER;
  if (!service_room(conn, reserve))
    {
      return PNUT_CONN_WAIT;
    }

  memset(&req, 0, sizeof(req));
  req.from.conn = conn;
  req.from.gen  = conn->gen;
  req.iface     = hdr->iface;
  req.method    = hdr->method;
  req.mversion  = hdr->mversion;
  req.seq       = hdr->seq;
  req.pid       = conn->pid;
  req.uid       = conn->uid;
  req.gid       = conn->gid;
  req.credit    = hdr->credit;
  req.reserve   = reserve;

  /* req.app names the app on the runtime's connections only (RFC 0023),
   * which cannot be told apart yet: left zero until they can
   */

  conn->pending++;
  conn->reserved += reserve;
  method->handler(service, &req, payload, hdr->len, service->arg);
  return PNUT_CONN_NEXT;
}

/****************************************************************************
 * Name: service_message
 *
 * Description:
 *   A message on one of the service's connections, the service held while
 *   it is dealt with: a handler, or a send that fails, may close it, or
 *   the connection.
 *
 ****************************************************************************/

static int service_message(FAR struct pnut_conn_s *conn,
                           FAR const struct pnut_msghdr_s *hdr,
                           FAR const uint8_t *payload)
{
  FAR struct pnut_service_s *service = service_hold(conn);
  int ret;

  ret = service_dispatch(conn, hdr, payload);

  /* Closed: freed now, with the connection */

  if (service_release(service) || service->closing)
    {
      return PNUT_CONN_GONE;
    }

  return conn->fd < 0 ? PNUT_CONN_GONE : ret;
}

/****************************************************************************
 * Name: service_held
 *
 * Description:
 *   How many of the service's connections a program holds.
 *
 ****************************************************************************/

static int service_held(FAR struct pnut_service_s *service, pid_t pid)
{
  int held = 0;
  uint8_t i;

  for (i = 0; i < service->nconns; i++)
    {
      if (service->conns[i].fd >= 0 && service->conns[i].pid == pid)
        {
          held++;
        }
    }

  return held;
}

/****************************************************************************
 * Name: service_resume
 *
 * Description:
 *   Accept again, after the pause that followed a failure.
 *
 ****************************************************************************/

static void service_resume(FAR struct pnut_loop_s *loop,
                           FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct pnut_service_s *service = arg;

  if (pnut_loop_rewatch(loop, service->fd, EPOLLIN) < 0)
    {
      pnut_timer_set(loop, timer, SERVICE_ACCEPT_PAUSE);
    }
}

/****************************************************************************
 * Name: service_accept
 ****************************************************************************/

static void service_accept(FAR struct pnut_loop_s *loop, int fd,
                           uint32_t events, FAR void *arg)
{
  FAR struct pnut_service_s *service = arg;
  FAR struct pnut_conn_s *conn;
  struct ucred cred;
  socklen_t len;
  uint8_t i;
  int cfd;

  for (; ; )
    {
      cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (cfd < 0)
        {
          /* A connection reset before it was accepted is not a failure */

          if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO)
            {
              continue;
            }

          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              return;
            }

          /* Out of descriptors or memory: the connection stays readable,
           * so stop watching it for a while rather than spin
           */

          if (!service->failing)
            {
              pnut_loop_log(loop, PNUT_LOG_ERROR,
                            "%s: cannot accept: %d",
                            service->addr.sun_path, errno);
              service->failing = true;
            }

          if (pnut_loop_rewatch(loop, fd, 0) == OK)
            {
              pnut_timer_set(loop, service->pause, SERVICE_ACCEPT_PAUSE);
            }

          return;
        }

      service->failing = false;

      memset(&cred, 0, sizeof(cred));
      len = sizeof(cred);
      if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
        {
          cred.pid = -1;
          cred.uid = -1;
          cred.gid = -1;
        }

      /* A program holds a few of the service's connections at most, so
       * that it cannot take them all (RFC 0023).  A caller not known by
       * its pid is not counted: it cannot be told from the others.  A
       * refused client's tries grow apart, and so do these lines in the
       * log.
       */

      if (cred.pid > 0 &&
          service_held(service, cred.pid) >= CONFIG_PNUT_LIB_PERCALLER)
        {
          pnut_loop_log(loop, PNUT_LOG_WARNING,
                        "%s: pid %d holds as many connections as it may",
                        service->addr.sun_path, (int)cred.pid);
          close(cfd);
          continue;
        }

      conn = NULL;
      for (i = 0; i < service->nconns; i++)
        {
          if (service->conns[i].fd < 0)
            {
              conn = &service->conns[i];
              break;
            }
        }

      if (conn == NULL)
        {
          pnut_loop_log(loop, PNUT_LOG_WARNING,
                        "%s: as many connections as it takes",
                        service->addr.sun_path);
          close(cfd);
          continue;
        }

      conn->pid = cred.pid;
      conn->uid = cred.uid;
      conn->gid = cred.gid;

      if (pnut_conn_attach(conn, cfd) < 0)
        {
          close(cfd);
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int pnut_service_open(FAR struct pnut_loop_s *loop, FAR const char *name,
                      FAR const struct pnut_method_s *methods,
                      size_t nmethods, uint8_t conns, FAR void *arg,
                      FAR struct pnut_service_s **servicep)
{
  FAR struct pnut_service_s *service;
  uint8_t i;
  int ret;

  *servicep = NULL;

  service = calloc(1, sizeof(*service));
  if (service == NULL)
    {
      return -ENOMEM;
    }

  service->loop     = loop;
  service->fd       = -1;
  service->methods  = methods;
  service->nmethods = nmethods;
  service->arg      = arg;
  service->nconns   = conns > 0 ? conns : CONFIG_PNUT_LIB_CONNS;

  service->addr.sun_family = AF_UNIX;
  ret = snprintf(service->addr.sun_path, sizeof(service->addr.sun_path),
                 "%s/%s", loop->config.rundir, name);
  if (ret < 0 || ret >= (int)sizeof(service->addr.sun_path))
    {
      free(service);
      return -ENAMETOOLONG;
    }

  service->conns = calloc(service->nconns, sizeof(struct pnut_conn_s));
  if (service->conns == NULL)
    {
      free(service);
      return -ENOMEM;
    }

  for (i = 0; i < service->nconns; i++)
    {
      service->conns[i].fd = -1;
    }

  for (i = 0; i < service->nconns; i++)
    {
      ret = pnut_conn_init(&service->conns[i], loop,
                           CONFIG_PNUT_LIB_SENDBUF);
      if (ret < 0)
        {
          goto errout;
        }

      service->conns[i].message = service_message;
      service->conns[i].closed  = service_closed;
      service->conns[i].owner   = service;
    }

  ret = pnut_timer_keep(loop, service_resume, service, &service->pause);
  if (ret < 0)
    {
      goto errout;
    }

  service->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                       0);
  if (service->fd < 0)
    {
      ret = -errno;
      goto errout;
    }

  /* A socket left by an earlier run of the service would stop the bind.
   * On the computer this also takes the name of a service still running,
   * which keeps a socket nobody reaches; on NuttX a local socket leaves no
   * file, and a second bind fails with -EADDRINUSE.
   */

  unlink(service->addr.sun_path);

  if (bind(service->fd, (FAR struct sockaddr *)&service->addr,
           sizeof(service->addr)) < 0 ||
      listen(service->fd, service->nconns) < 0)
    {
      ret = -errno;
      goto errout;
    }

  ret = pnut_loop_watch(loop, service->fd, EPOLLIN, service_accept,
                        service);
  if (ret < 0)
    {
      goto errout;
    }

  *servicep = service;
  return OK;

errout:
  if (service->fd >= 0)
    {
      close(service->fd);
      unlink(service->addr.sun_path);
    }

  service_free(service);
  return ret;
}

void pnut_service_close(FAR struct pnut_service_s *service)
{
  uint8_t i;

  if (service == NULL || service->closing)
    {
      return;
    }

  pnut_loop_unwatch(service->loop, service->fd);
  close(service->fd);
  unlink(service->addr.sun_path);
  service->fd = -1;
  pnut_timer_clear(service->loop, service->pause);

  /* Its streams go with it, without a word: its module is closing it */

  while (service->streams != NULL)
    {
      service_unlink(service->streams);
    }

  for (i = 0; i < service->nconns; i++)
    {
      pnut_conn_close(&service->conns[i]);
    }

  if (service->dispatching > 0)
    {
      service->closing = true;
      return;
    }

  service_free(service);
}

int pnut_reply(FAR const struct pnut_request_s *req, int status,
               FAR const void *payload, uint16_t len)
{
  FAR struct pnut_conn_s *conn = req->from.conn;
  FAR struct pnut_service_s *service;
  struct pnut_msghdr_s hdr;
  int ret;

  if (status < 0 || status > UINT8_MAX)
    {
      return -EINVAL;
    }

  if (conn->fd < 0 || conn->gen != req->from.gen)
    {
      return -ENOTCONN;
    }

  /* Larger than its method said, it would take room kept for others */

  if (PNUT_MSG_HEADER + (size_t)len > req->reserve)
    {
      return -EMSGSIZE;
    }

  memset(&hdr, 0, sizeof(hdr));
  hdr.version  = PNUT_MSG_VERSION;
  hdr.kind     = PNUT_KIND_REPLY;
  hdr.iface    = req->iface;
  hdr.method   = req->method;
  hdr.mversion = req->mversion;
  hdr.status   = status;
  hdr.seq      = req->seq;
  hdr.len      = len;
  hdr.app      = req->app;

  /* Answered, even when it does not fit, which an answer within its
   * method's replymax never finds: the caller is slow, not gone, and its
   * call times out.  Its room is given back, and a request left for want
   * of it offered again.
   */

  service = service_hold(conn);
  ret = pnut_conn_send(conn, &hdr, payload);
  if (ret == OK || ret == -EBUSY)
    {
      if (conn->pending > 0)
        {
          conn->pending--;
        }

      conn->reserved -= conn->reserved > req->reserve ? req->reserve :
                                                        conn->reserved;
      pnut_conn_recheck(conn);
    }

  service_release(service);
  return ret;
}

int pnut_event(FAR const struct pnut_endpoint_s *to, uint16_t iface,
               uint16_t method, FAR const void *payload, uint16_t len)
{
  FAR struct pnut_conn_s *conn = to->conn;
  FAR struct pnut_service_s *service;
  struct pnut_msghdr_s hdr;
  int ret;

  if (conn->fd < 0 || conn->gen != to->gen)
    {
      return -ENOTCONN;
    }

  /* Never in the room kept for answers */

  if (!service_room(conn, PNUT_MSG_HEADER + (size_t)len))
    {
      return len > PNUT_MSG_PAYLOAD_MAX ? -EMSGSIZE : -EBUSY;
    }

  memset(&hdr, 0, sizeof(hdr));
  hdr.version = PNUT_MSG_VERSION;
  hdr.kind    = PNUT_KIND_EVENT;
  hdr.iface   = iface;
  hdr.method  = method;
  hdr.len     = len;

  service = service_hold(conn);
  ret     = pnut_conn_send(conn, &hdr, payload);
  service_release(service);
  return ret;
}

int pnut_stream_accept(FAR struct pnut_service_s *service,
                       FAR const struct pnut_request_s *req,
                       FAR struct pnut_stream_s *stream,
                       pnut_stream_ready_t ready,
                       pnut_stream_closed_t closed, FAR void *arg)
{
  FAR struct pnut_conn_s *conn = req->from.conn;
  int ret;

  if (service == NULL || service->closing)
    {
      return -ENOTCONN;
    }

  if (stream->open)
    {
      return -EBUSY;              /* Another's, still */
    }

  /* A failed answer may have freed the service: nothing of it is read
   * then, only the return value
   */

  ret = pnut_reply(req, PNUT_STATUS_OK, NULL, 0);
  if (ret < 0 || conn->fd < 0 || conn->gen != req->from.gen)
    {
      return ret < 0 ? ret : -ENOTCONN;
    }

  /* Room kept for its end, so that the end always fits */

  conn->reserved += PNUT_MSG_HEADER;

  memset(stream, 0, sizeof(*stream));
  stream->to       = req->from;
  stream->iface    = req->iface;
  stream->method   = req->method;
  stream->mversion = req->mversion;
  stream->seq      = req->seq;
  stream->credit   = req->credit;
  stream->open     = true;
  stream->ready    = ready;
  stream->closed   = closed;
  stream->arg      = arg;
  stream->service  = service;
  stream->next     = service->streams;
  service->streams = stream;
  return OK;
}

int pnut_stream_push(FAR struct pnut_stream_s *stream,
                     FAR const void *payload, uint16_t len)
{
  FAR struct pnut_conn_s *conn = stream->to.conn;
  FAR struct pnut_service_s *service;
  struct pnut_msghdr_s hdr;
  int ret;

  if (!stream->open)
    {
      return -ENOTCONN;
    }

  if (conn->fd < 0 || conn->gen != stream->to.gen)
    {
      service_unlink(stream);
      return -ENOTCONN;
    }

  if (stream->credit == 0)
    {
      return -EAGAIN;
    }

  if (len > PNUT_MSG_PAYLOAD_MAX)
    {
      return -EMSGSIZE;
    }

  /* Never in the room kept for answers */

  if (!service_room(conn, PNUT_MSG_HEADER + (size_t)len))
    {
      return -EBUSY;
    }

  memset(&hdr, 0, sizeof(hdr));
  hdr.version  = PNUT_MSG_VERSION;
  hdr.kind     = PNUT_KIND_STREAM;
  hdr.iface    = stream->iface;
  hdr.method   = stream->method;
  hdr.mversion = stream->mversion;
  hdr.seq      = stream->seq;
  hdr.len      = len;

  /* A send that fails ends the stream, its closed handler told, within
   * this call: the stream is not touched after
   */

  service = service_hold(conn);
  ret     = pnut_conn_send(conn, &hdr, payload);
  if (ret >= 0)
    {
      stream->credit--;
    }

  service_release(service);
  return ret;
}

void pnut_stream_end(FAR struct pnut_stream_s *stream, int status)
{
  FAR struct pnut_conn_s *conn = stream->to.conn;
  FAR struct pnut_service_s *service;
  struct pnut_msghdr_s hdr;
  uint32_t gen = stream->to.gen;

  if (!stream->open)
    {
      return;
    }

  memset(&hdr, 0, sizeof(hdr));
  hdr.version  = PNUT_MSG_VERSION;
  hdr.kind     = PNUT_KIND_STREAM;
  hdr.iface    = stream->iface;
  hdr.method   = stream->method;
  hdr.mversion = stream->mversion;
  hdr.seq      = stream->seq;
  hdr.status   = status == PNUT_STATUS_OK ? PNUT_STATUS_ENDED :
                 status < 0 || status > UINT8_MAX ? PNUT_STATUS_INTERNAL :
                 status;

  /* Unlinked first, its closed handler never called for it; the end
   * needs no credit, and takes the room kept for it since it was
   * accepted, which the unlinking gave back
   */

  service_unlink(stream);
  if (conn->fd >= 0 && conn->gen == gen)
    {
      service = service_hold(conn);
      pnut_conn_send(conn, &hdr, NULL);
      service_release(service);
    }
}
