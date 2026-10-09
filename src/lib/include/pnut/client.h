/****************************************************************************
 * pnut-os/src/lib/include/pnut/client.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_LIB_PNUT_CLIENT_H
#define __PNUT_OS_LIB_PNUT_CLIENT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/compiler.h>
#include <pnut/loop.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Calls a client has in flight at once, unless it says otherwise */

#ifdef CONFIG_PNUT_LIB_INFLIGHT
#  define PNUT_CLIENT_INFLIGHT  CONFIG_PNUT_LIB_INFLIGHT
#else
#  define PNUT_CLIENT_INFLIGHT  8
#endif

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A client of a service (RFC 0023): it connects to <rundir>/<name>, and
 * reconnects with a growing delay whenever the connection is lost; the
 * delay starts again from the shortest only after a connection that
 * lasted.  A client takes one of the loop's timers while it is open.
 */

struct pnut_client_s;

/* Called on the loop with a call's answer: the service's status and body,
 * or PNUT_STATUS_TIMEOUT, or PNUT_STATUS_UNAVAILABLE if the connection was
 * lost first.
 */

typedef CODE void (*pnut_reply_t)(FAR struct pnut_client_s *client,
                                  int status, FAR const uint8_t *payload,
                                  uint16_t len, FAR void *arg);

/* Called on the loop for an event from the service */

typedef CODE void (*pnut_event_t)(FAR struct pnut_client_s *client,
                                  uint16_t iface, uint16_t method,
                                  FAR const uint8_t *payload, uint16_t len,
                                  FAR void *arg);

/* Called on the loop when the connection comes up or is lost: on its
 * return, read the service's state again (RFC 0004).
 */

typedef CODE void (*pnut_state_t)(FAR struct pnut_client_s *client,
                                  bool connected, FAR void *arg);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: pnut_client_open
 *
 * Description:
 *   Make a client of the service <name>.  It connects at once if it can,
 *   and keeps trying otherwise.
 *
 * Input Parameters:
 *   loop     - The loop.
 *   name     - The service's name.
 *   inflight - Calls in flight at once; zero for
 *              CONFIG_PNUT_LIB_INFLIGHT.
 *   event    - Called for events, or NULL.
 *   state    - Called when the connection comes up or is lost, or NULL.
 *   arg      - Passed to event and state.
 *   clientp  - Where to return the client.
 *
 * Returned Value:
 *   Zero (OK) on success; -EBUSY when the loop has no timer left; another
 *   negated errno value on failure.
 *
 ****************************************************************************/

int pnut_client_open(FAR struct pnut_loop_s *loop, FAR const char *name,
                     uint8_t inflight, pnut_event_t event,
                     pnut_state_t state, FAR void *arg,
                     FAR struct pnut_client_s **clientp);

/****************************************************************************
 * Name: pnut_client_close
 *
 * Description:
 *   Close the client.  Calls in flight are dropped without their reply
 *   handlers.  It may be called from the client's own handlers.
 *
 ****************************************************************************/

void pnut_client_close(FAR struct pnut_client_s *client);

/****************************************************************************
 * Name: pnut_client_connected
 ****************************************************************************/

bool pnut_client_connected(FAR struct pnut_client_s *client);

/****************************************************************************
 * Name: pnut_call
 *
 * Description:
 *   Call a method; reply is called on the loop with its answer.
 *
 * Input Parameters:
 *   client   - The client.
 *   iface    - The interface's number.
 *   method   - The method's number.
 *   mversion - The method's version.
 *   payload  - The body, copied.
 *   len      - Its length.
 *   timeout  - Milliseconds; zero for CONFIG_PNUT_LIB_CALL_TIMEOUT.
 *   reply    - Called with the answer.
 *   arg      - Passed to reply.
 *
 * Returned Value:
 *   Zero (OK), and reply is called later; -ENOTCONN while the client is
 *   not connected, or is closing; -EBUSY when as many calls as allowed
 *   are in flight, or the send buffer is full; -EMSGSIZE for a body over
 *   PNUT_MSG_PAYLOAD_MAX; another negated errno value if the connection
 *   failed while sending, and then the state handler has been told.
 *
 ****************************************************************************/

int pnut_call(FAR struct pnut_client_s *client, uint16_t iface,
              uint16_t method, uint8_t mversion, FAR const void *payload,
              uint16_t len, uint32_t timeout, pnut_reply_t reply,
              FAR void *arg);

#endif /* __PNUT_OS_LIB_PNUT_CLIENT_H */
