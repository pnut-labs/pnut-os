/****************************************************************************
 * pnut-os/src/telephony/data.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_TELEPHONY_DATA_H
#define __PNUT_OS_TELEPHONY_DATA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <netinet/in.h>

#include <pnut/compiler.h>
#include <pnut/loop.h>
#include <pnut/timer.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DATA_IFNAME     16          /* An interface's name, its end too */
#define DATA_APN        64          /* An access point's name, its end too */
#define DATA_RING       4096        /* Bytes for PPP, waiting for room */
#define DATA_FRAME      127         /* PPP's bytes framed together at most:
                                     * the multiplexer's MUX_N1 */
#define DATA_LINE       64          /* A line of the dial's answer, kept */

/* Times, in ms */

#define DATA_DIAL_MS    30000       /* For ATD*99***1#'s CONNECT */
#define DATA_LINK_MS    60000       /* From CONNECT to PPP up */
#define DATA_FLUSH_MS   20          /* PPP's bytes held for a whole frame */
#define DATA_DTR_MS     1000        /* DTR held low to hang up */
#define DATA_STOP_MS    15000       /* For the link's end, once stopped */
#define DATA_RETRY_MS   30000       /* After a failure, doubling */
#define DATA_RETRY_MAX  600000      /* up to this */
#define DATA_GAP_MS     30000       /* The network lost, before stopping */
#define DATA_FAILS      3           /* Failures in a row, then "failed" */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* What the link tells, as records on its descriptor (data_link_ops_s) */

enum data_event_e
{
  DATA_LINK_UP = 1,                 /* PPP is up: the addresses */
  DATA_LINK_DOWN,                   /* It went down */
  DATA_LINK_ENDED                   /* Stopped, everything let go */
};

struct data_event_s
{
  uint8_t event;                    /* enum data_event_e */
  char ifname[DATA_IFNAME];         /* As "ppp0" */
  struct in_addr local;
  struct in_addr peer;
  struct in_addr dns[2];            /* 0.0.0.0 for one not given */
};

/* The link: PPP over a pseudo-terminal's slave end, on a thread of its
 * own.  pppd on the device (data_ppp.c), a fake in the tests.  start()
 * writes struct data_event_s records to fd, from its thread, the last one
 * DATA_LINK_ENDED once stop() has been called or it failed; it gives the
 * interface a host-only netmask before DATA_LINK_UP.
 */

struct data_link_ops_s
{
  CODE int (*start)(FAR void *arg, FAR const char *tty, int fd);
  CODE void (*stop)(FAR void *arg);
  FAR void *arg;
};

/* What mobile data needs of Telephony */

struct data_ops_s
{
  /* PPP's bytes and the dial's text to channel 2, all or none: -EAGAIN
   * when there is no room, data_room() then
   */

  CODE int (*send)(FAR void *arg, FAR const uint8_t *buf, size_t len);

  /* Channel 2's DTR, low or high: low for DATA_DTR_MS ends the data call
   * where the modem takes DTR so (&D2)
   */

  CODE void (*dtr)(FAR void *arg, bool high);

  /* The AT channel held (nothing sent, nothing timed) while channel 2
   * dials, since the modem answers nothing on it meanwhile; and whether
   * nothing is waiting there, to dial
   */

  CODE void (*hold)(FAR void *arg, bool hold);
  CODE bool (*idle)(FAR void *arg);

  /* AT+CGDCONT for an access point's name, on the AT channel;
   * data_apn_done() once answered
   */

  CODE int (*apn)(FAR void *arg, FAR const char *apn);

  /* The state changed: data_state() tells it */

  CODE void (*changed)(FAR void *arg);
};

/* Mobile data (RFC 0030, step 4): with the data setting on and the modem
 * registered, channel 2 dials the default bearer (ATD*99***1#) and, once
 * connected, carries PPP between the modem and the link through a
 * pseudo-terminal.  The link's events come back to the loop through a
 * pipe.  A link that fails is tried again after a growing wait.
 */

struct data_s
{
  FAR const struct data_ops_s *ops;
  FAR void *arg;                    /* The ops' */
  FAR const struct data_link_ops_s *link;
  FAR struct pnut_module_s *module;
  FAR struct pnut_loop_s *loop;

  /* What is wanted, and what allows it */

  bool want;                        /* The data setting */
  bool ready;                       /* Registered, on the air */
  char apn[DATA_APN];

  bool started;                     /* data_start() ran */
  uint8_t step;                     /* enum data_step_e (data.c) */
  uint8_t probes;                   /* Probes sent on channel 2 */
  bool escaped;                     /* "+++" tried, this attempt */
  bool plus;                        /* and sent */
  uint8_t fails;                    /* In a row */
  bool failed;                      /* DATA_FAILS reached */
  bool held;                        /* The AT channel held */
  bool calling;                     /* ATD sent: hang up at the end */
  bool linked;                      /* The link runs: DATA_LINK_ENDED due */
  bool alive;                       /* Its thread has not sent ENDED (a
                                     * link given up on stays alive) */
  bool retrying;                    /* After the end, wait and try again */
  bool up;                          /* PPP up */
  uint32_t retry;                   /* The next wait, in ms */
  FAR struct pnut_timer_s *timer;   /* The step's */
  FAR struct pnut_timer_s *gap;     /* The network lost */
  FAR struct pnut_timer_s *flush;   /* PPP's bytes held */
  struct data_event_s link_info;    /* As DATA_LINK_UP gave it */

  /* The dial's answer, a line at a time */

  char line[DATA_LINE];
  size_t llen;

  /* The pseudo-terminal: the master end here, the slave end the link's */

  int master;
  int slave;
  char tty[32];

  /* The link's events */

  int events[2];                    /* The pipe: read end, write end */
  size_t elen;
  uint8_t ebuf[sizeof(struct data_event_s)];

  /* From the modem to PPP, waiting for the pseudo-terminal's room */

  FAR uint8_t *ring;                /* DATA_RING bytes */
  size_t head;
  size_t count;
  uint32_t dropped;                 /* Bytes that found no room */

  /* From PPP to the modem, held for a whole frame */

  uint8_t out[DATA_FRAME];
  size_t olen;
  bool out_held;                    /* Refused for room: data_room() */
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifdef CONFIG_NETUTILS_PPPD
/* pppd, on the device (data_ppp.c) */

extern const struct data_link_ops_s g_data_ppp;
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: data_init
 *
 * Description:
 *   Set mobile data up, off.  link NULL: none on this device.
 *
 ****************************************************************************/

void data_init(FAR struct data_s *data, FAR struct pnut_module_s *module,
               FAR const struct data_ops_s *ops, FAR void *arg,
               FAR const struct data_link_ops_s *link);

/****************************************************************************
 * Name: data_start, data_stop
 *
 * Description:
 *   Start with the module's loop; stop at once, the link with it (the
 *   program finishing).
 *
 ****************************************************************************/

int data_start(FAR struct data_s *data, FAR struct pnut_loop_s *loop);
void data_stop(FAR struct data_s *data);

/****************************************************************************
 * Name: data_deinit
 *
 * Description:
 *   After data_stop() and the loop's end: the pseudo-terminal, the pipe and
 *   the ring let go, once the link has ended (its last records read from
 *   the pipe).  While its thread may still write, they stay, for the
 *   program's end to close.
 *
 ****************************************************************************/

void data_deinit(FAR struct data_s *data);

/****************************************************************************
 * Name: data_want
 *
 * Description:
 *   The data setting and the access point's name (empty: the network's).
 *
 ****************************************************************************/

void data_want(FAR struct data_s *data, bool want, FAR const char *apn);

/****************************************************************************
 * Name: data_ready
 *
 * Description:
 *   Whether the modem can carry data: on, its SIM ready, registered, on
 *   the air.  Not ready but keep (on the air, its SIM ready, only the
 *   network lost): a call is kept for DATA_GAP_MS; not keep (airplane
 *   mode, the SIM gone): a call ends at once.
 *
 ****************************************************************************/

void data_ready(FAR struct data_s *data, bool ready, bool keep);

/****************************************************************************
 * Name: data_apn_done
 *
 * Description:
 *   AT+CGDCONT answered: OK, or a negated errno value.
 *
 ****************************************************************************/

void data_apn_done(FAR struct data_s *data, int result);

/****************************************************************************
 * Name: data_reset
 *
 * Description:
 *   The modem went, or started again: channel 2 is gone with it.  The
 *   link stops, and data waits for data_ready().
 *
 ****************************************************************************/

void data_reset(FAR struct data_s *data);

/****************************************************************************
 * Name: data_input, data_room
 *
 * Description:
 *   Channel 2's bytes; and room on it again after a refusal.
 *
 ****************************************************************************/

void data_input(FAR struct data_s *data, FAR const uint8_t *buf,
                size_t len);
void data_room(FAR struct data_s *data);

/****************************************************************************
 * Name: data_state
 *
 * Description:
 *   The state to publish: PNUT_TELEPHONY_INFO_DATA_* (telephony.proto).
 *
 ****************************************************************************/

int data_state(FAR const struct data_s *data);

/****************************************************************************
 * Name: data_link
 *
 * Description:
 *   The link's addresses and name servers while data is on (data_state()
 *   PNUT_TELEPHONY_INFO_DATA_DATA_ON); NULL otherwise.
 *
 ****************************************************************************/

FAR const struct data_event_s *data_link(FAR const struct data_s *data);

#endif /* __PNUT_OS_TELEPHONY_DATA_H */
