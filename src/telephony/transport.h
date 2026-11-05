/****************************************************************************
 * pnut-os/src/telephony/transport.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_TELEPHONY_TRANSPORT_H
#define __PNUT_OS_TELEPHONY_TRANSPORT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/compiler.h>
#include <pnut/loop.h>
#include <pnut/timer.h>

#include "pnut/messages.pnut.h"

#include "sms.h"
#include "smsjoin.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TRANSPORT_NAME    "sms"     /* Messages' name for it (RFC 0029) */
#define TRANSPORT_QUEUE   32        /* Slots announced, waiting */
#define TRANSPORT_SLOTS   1024      /* Slots told apart once read */
#define TRANSPORT_TICK    600000    /* ms between looks at what waits */

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct telephony_s;

/* Telephony's sms transport, its arriving half (RFC 0030, step 2): SMS
 * stay in the modem's memory until Messages has them on flash, then go
 * from it.  One thing at a time: a command to the modem, or a delivery.
 */

struct transport_s
{
  FAR struct telephony_s *tel;
  FAR const char *service;        /* Messages', or NULL: no transport */
  uint32_t tick;                  /* TRANSPORT_TICK, or the tests' */

  /* Messages */

  struct pnut_messages_client_s messages;
  bool open;
  bool connected;
  bool registering;               /* RegisterTransport out */
  bool registered;                /* sms carried on this connection */

  /* The modem, and where the work is */

  bool modem;                     /* On, its SIM ready */
  bool storage;                   /* Its memory chosen and counted */
  bool sim;                       /* The memory read is the SIM's, being
                                   * emptied into Messages */
  bool sim_check;                 /* The SIM's to look at, at the next
                                   * choice */
  bool busy;                      /* A command or a delivery out */
  bool paused;                    /* Something failed: until the tick */
  bool again;                     /* Read through again at the tick */
  uint8_t tries;                  /* The memory chosen, without success */
  bool refused;                   /* Messages refused the last one */
  uint8_t what;                   /* enum transport_what_e, out */
  uint32_t gen;                   /* Changes when the modem goes */
  uint32_t sent_gen;              /* The one out was sent in */
  uint16_t used;                  /* The memory's SMS, as counted */
  uint16_t total;                 /* and its slots */
  bool scanning;
  bool scan_read;                 /* The slot read is the scan's */
  uint16_t next;                  /* The scan's next slot */
  uint16_t found;                 /* SMS it found */
  uint16_t queue[TRANSPORT_QUEUE];
  uint8_t head;
  uint8_t count;
  uint8_t seen[TRANSPORT_SLOTS / 8];  /* Read, and held or kept */
  int16_t slot;                       /* Being read or deleted */
  FAR struct pnut_timer_s *timer;     /* The tick */
  FAR struct pnut_timer_s *later;     /* A moment, to try again */

  /* What was read, and what is delivered */

  struct sms_deliver_s part;
  struct smsjoin_s join;
  struct smsjoin_msg_s msg;
  bool holding;                   /* msg is to deliver, then delete */
  bool delivered;                 /* All of it is with Messages */
  uint8_t chunk;                  /* Its text's piece being delivered */
  uint8_t chunks;
  size_t offset;                  /* Where that piece begins */
  uint8_t deleting;               /* Its slot being deleted */
  pnut_messages_deliver_request_t deliver;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: transport_init, transport_start, transport_stop
 *
 * Description:
 *   Set up the transport; connect to Messages (service, or NULL for no
 *   transport) and register sms at each connection; let everything go.
 *
 ****************************************************************************/

void transport_init(FAR struct transport_s *tr, FAR struct telephony_s *tel,
                    FAR const char *service, uint32_t tick);
int transport_start(FAR struct transport_s *tr,
                    FAR struct pnut_loop_s *loop);
void transport_stop(FAR struct transport_s *tr);

/****************************************************************************
 * Name: transport_ready, transport_gone
 *
 * Description:
 *   The modem is on with its SIM ready: its memory is chosen and read.
 *   It went: nothing out is waited for, and what was being joined is
 *   read again once it is back.  Both may be called again in the same
 *   state.
 *
 ****************************************************************************/

void transport_ready(FAR struct transport_s *tr);
void transport_gone(FAR struct transport_s *tr);

/****************************************************************************
 * Name: transport_urc
 *
 * Description:
 *   A line the modem sent unasked.
 *
 * Returned Value:
 *   True when it was the transport's (+CMTI, SMS DONE).
 *
 ****************************************************************************/

bool transport_urc(FAR struct transport_s *tr, FAR const char *line);

#endif /* __PNUT_OS_TELEPHONY_TRANSPORT_H */
