/****************************************************************************
 * pnut-os/src/lib/include/pnut/service.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_LIB_PNUT_SERVICE_H
#define __PNUT_OS_LIB_PNUT_SERVICE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <pnut/compiler.h>
#include <pnut/loop.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A service: a module's interface, served on a local stream socket at
 * <rundir>/<name> (RFC 0005, RFC 0010).
 *
 * A connection takes a request only when its send buffer has room for
 * its answer beside the room kept for the answers it owes, now or later:
 * each method says how large its answers are (replymax), and that much is
 * kept from the request until its answer; the requests after one with no
 * room yet wait in the socket.  Up to CONFIG_PNUT_LIB_INFLIGHT requests
 * of a connection may wait for their answers; one more is answered busy
 * at once.  Answer every request, and once: one never answered keeps its
 * room and counts against that bound until the connection closes; one
 * answered twice lets one more past it.  An event needs room beside every
 * answer owed: with two answers of a whole message owed on a connection
 * of the default size, pnut_event() returns -EBUSY until one is given.
 */

struct pnut_service_s;
struct pnut_conn_s;

/* One end of a connection.  A copy may be kept while the service is
 * open: once the connection has closed, a reply or event to it returns
 * -ENOTCONN.  After pnut_service_close() it names freed memory: a program
 * drops the requests it keeps before closing their service.
 */

struct pnut_endpoint_s
{
  FAR struct pnut_conn_s *conn;
  uint32_t gen;
};

/* A request, as its handler sees it */

struct pnut_request_s
{
  struct pnut_endpoint_s from;
  uint16_t iface;
  uint16_t method;
  uint8_t mversion;
  uint16_t seq;
  uint16_t app;                   /* The app, on the runtime's connections
                                   * (RFC 0023); zero until those can be
                                   * told apart */
  pid_t pid;                      /* The caller, from SO_PEERCRED */
  uid_t uid;
  gid_t gid;
  uint16_t credit;                /* A stream's first items, at most */
  uint16_t reserve;               /* The library's: room kept to answer */
};

/* A stream a method answers with (RFC 0004, RFC 0005): once accepted, its
 * items go to the caller as the caller's credit allows, until either end
 * ends it or the connection goes.  The module keeps it, zeroed before its
 * first use; the library links it to the service while it is open.
 */

struct pnut_stream_s;

/* Called on the loop when credit came (push more), and when the stream is
 * over: the caller ended it, or its connection went; then the stream is
 * the module's again, unlinked.  A connection that fails while the
 * module sends to it (a push, an answer, an event) calls closed from
 * inside that call.  An open stream keeps room for its end, which
 * therefore always goes.
 */

typedef CODE void (*pnut_stream_ready_t)(FAR struct pnut_stream_s *stream,
                                         FAR void *arg);
typedef CODE void (*pnut_stream_closed_t)(FAR struct pnut_stream_s *stream,
                                          int status, FAR void *arg);

struct pnut_stream_s
{
  struct pnut_endpoint_s to;
  uint16_t iface;
  uint16_t method;
  uint8_t mversion;
  uint16_t seq;
  uint16_t credit;                /* Items it may push now */
  bool open;
  pnut_stream_ready_t ready;
  pnut_stream_closed_t closed;
  FAR void *arg;

  /* The library's */

  FAR struct pnut_service_s *service;
  FAR struct pnut_stream_s *next;
};

/* Called on the loop for a request.  It answers with pnut_reply(), now or
 * later from a copy of *req.
 */

typedef CODE void (*pnut_handler_t)(FAR struct pnut_service_s *service,
                                    FAR const struct pnut_request_s *req,
                                    FAR const uint8_t *payload,
                                    uint16_t len, FAR void *arg);

/* A method the service serves.  A request for another method is answered
 * "not found", one for another version of it "invalid", by the library.
 * replymax is the largest body its answers have, an error's detail
 * included, which the generator takes from the interface's limits: the
 * larger of its answer's and pnut.Error's, PNUT_ERROR_SIZE.  Zero, a whole
 * message.
 */

struct pnut_method_s
{
  uint16_t iface;
  uint16_t method;
  uint8_t mversion;
  pnut_handler_t handler;
  uint16_t replymax;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: pnut_service_open
 *
 * Description:
 *   Serve methods on <rundir>/<name>.
 *
 * Input Parameters:
 *   loop     - The loop.
 *   name     - The service's name: its socket's file name.
 *   methods  - The methods served; kept, not copied.
 *   nmethods - How many.
 *   conns    - Connections at once; zero for CONFIG_PNUT_LIB_CONNS.  One
 *              more is closed as soon as it is accepted, and so is one
 *              more than CONFIG_PNUT_LIB_PERCALLER from one program.
 *   arg      - Passed to the handlers.
 *   servicep - Where to return the service.
 *
 * Returned Value:
 *   Zero (OK) on success; -EBUSY when the loop has no timer left; another
 *   negated errno value on failure.
 *
 ****************************************************************************/

int pnut_service_open(FAR struct pnut_loop_s *loop, FAR const char *name,
                      FAR const struct pnut_method_s *methods,
                      size_t nmethods, uint8_t conns, FAR void *arg,
                      FAR struct pnut_service_s **servicep);

/****************************************************************************
 * Name: pnut_service_close
 *
 * Description:
 *   Close the service and its connections.  It may be called from one of
 *   its handlers.
 *
 ****************************************************************************/

void pnut_service_close(FAR struct pnut_service_s *service);

/****************************************************************************
 * Name: pnut_reply
 *
 * Description:
 *   Answer a request with a status and a body.
 *
 * Input Parameters:
 *   req     - The request, or a copy kept to answer it later.
 *   status  - A PNUT_STATUS_ value, or another from 0 to 255.
 *   payload - The body, copied.
 *   len     - Its length.
 *
 * Returned Value:
 *   Zero (OK); -EINVAL for a status out of range; -ENOTCONN if the
 *   caller's connection has closed; -EMSGSIZE for a body over its
 *   method's replymax, an error's detail included; after these the
 *   request is not answered yet, and keeps its room until it is.  -EBUSY
 *   if the connection's send buffer has no room, which an answer within
 *   replymax never finds: the answer is lost, the request counts as
 *   answered, and the caller's call times out; another negated errno
 *   value if the connection failed while sending, and then it is closed.
 *
 ****************************************************************************/

int pnut_reply(FAR const struct pnut_request_s *req, int status,
               FAR const void *payload, uint16_t len);

/****************************************************************************
 * Name: pnut_event
 *
 * Description:
 *   Send an event to one connection (RFC 0005); events for everyone go on
 *   uORB topics instead.
 *
 * Returned Value:
 *   As pnut_reply(), except that -EBUSY says the send buffer has no room
 *   beside what is kept for answers, and the event is not sent.
 *
 ****************************************************************************/

int pnut_event(FAR const struct pnut_endpoint_s *to, uint16_t iface,
               uint16_t method, FAR const void *payload, uint16_t len);

/****************************************************************************
 * Name: pnut_stream_accept
 *
 * Description:
 *   Answer a request with a stream: an OK answer, with no body, and the
 *   stream open with the credit the request gave.
 *
 * Input Parameters:
 *   service - The request's service.
 *   req     - The request.
 *   stream  - The stream, kept by the module while it is open.
 *   ready   - Called when credit comes, or NULL.
 *   closed  - Called when the stream is over but not by the module, or
 *             NULL.
 *   arg     - Passed to both.
 *
 * Returned Value:
 *   As pnut_reply(), and -EBUSY for a stream still open; the stream is
 *   open only on success.
 *
 ****************************************************************************/

int pnut_stream_accept(FAR struct pnut_service_s *service,
                       FAR const struct pnut_request_s *req,
                       FAR struct pnut_stream_s *stream,
                       pnut_stream_ready_t ready,
                       pnut_stream_closed_t closed, FAR void *arg);

/****************************************************************************
 * Name: pnut_stream_push
 *
 * Description:
 *   An item to the caller, for one of its credit.
 *
 * Returned Value:
 *   Zero (OK); -EAGAIN with no credit left, until ready is called;
 *   -EBUSY when the send buffer has no room beside what is kept for
 *   answers, which nothing announces: try again later; -EMSGSIZE;
 *   -ENOTCONN when the stream is over, which the stream then is,
 *   unlinked; another negated errno value if the connection failed while
 *   sending, and then closed has been called.
 *
 ****************************************************************************/

int pnut_stream_push(FAR struct pnut_stream_s *stream,
                     FAR const void *payload, uint16_t len);

/****************************************************************************
 * Name: pnut_stream_end
 *
 * Description:
 *   End a stream: the caller is told the status, PNUT_STATUS_ENDED when
 *   it simply ends.  The stream is unlinked, and closed is not called.
 *
 ****************************************************************************/

void pnut_stream_end(FAR struct pnut_stream_s *stream, int status);

#endif /* __PNUT_OS_LIB_PNUT_SERVICE_H */
