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
  uint16_t reserve;               /* The library's: room kept to answer */
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

#endif /* __PNUT_OS_LIB_PNUT_SERVICE_H */
