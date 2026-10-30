/****************************************************************************
 * pnut-os/src/telephony/mux.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_TELEPHONY_MUX_H
#define __PNUT_OS_TELEPHONY_MUX_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <pnut/compiler.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MUX_N1          127         /* The longest information field sent */
#define MUX_RX_MAX      1510        /* and taken: the modem's may be longer */
#define MUX_DLCIS       3           /* Control, AT, data */
#define MUX_QUEUE       2048        /* Bytes waiting for the UART */
#define MUX_TEXT        16          /* Text between frames, kept */

/* The channels (RFC 0030) */

#define MUX_CONTROL     0
#define MUX_AT          1
#define MUX_DATA        2

/* V.24 signals in an MSC: EA, RTC (DTR), RTR (RTS), DV.  Without RTC, DTR
 * is dropped, which hangs up a data call.
 */

#define MUX_V24         0x8d
#define MUX_V24_HANGUP  0x89

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* What the multiplexer tells of */

enum mux_event_e
{
  MUX_OPENED = 0,                 /* The modem took a channel (UA) */
  MUX_CLOSED,                     /* It closed one, or refused it (DM) */
  MUX_ENDED,                      /* It closed the multiplexer down (CLD) */
  MUX_RESTARTED,                  /* "RDY" came unframed: the modem
                                   * started again, in plain AT */
  MUX_ROOM,                       /* A send refused for want of room, or
                                   * held by flow control, may go now */
};

struct mux_ops_s
{
  /* Bytes to the UART, not blocking: how many it took, or a negated
   * errno value; -EAGAIN when it takes none now
   */

  CODE ssize_t (*write)(FAR void *arg, FAR const uint8_t *buf, size_t len);

  /* A channel's data, as it comes */

  CODE void (*data)(FAR void *arg, uint8_t dlci, FAR const uint8_t *buf,
                    size_t len);

  /* An event of a channel's, or of the multiplexer's (dlci 0) */

  CODE void (*event)(FAR void *arg, uint8_t dlci, int event);
};

/* 3GPP 27.010's basic option on the modem's UART (AT+CMUX), run on the
 * event loop: frames to send wait whole in one queue, in order, and go
 * out as the UART takes them; the modem's bytes are read as they come.
 * The queue carries plain bytes too, outside the multiplexer, so that
 * everything for the UART takes one path.
 */

struct mux_s
{
  FAR const struct mux_ops_s *ops;
  FAR void *arg;

  bool active;                    /* The UART carries frames */
  bool held;                      /* The modem asked to wait: flow control */
  bool refused;                   /* A send was refused: tell of room */
  uint8_t opened;                 /* The channels the modem took, a bit
                                   * each */

  /* The queue: bytes from off to len wait for the UART */

  uint16_t off;
  uint16_t len;
  uint8_t queue[MUX_QUEUE];

  /* The frame being read */

  uint8_t state;
  uint8_t addr;
  uint8_t ctrl;
  uint8_t fcs;
  uint16_t flen;
  uint16_t got;
  uint32_t bad;                   /* Frames dropped, bad or too long */
  uint8_t frame[MUX_RX_MAX];

  /* The text read between frames, for "RDY" */

  uint8_t tlen;
  char text[MUX_TEXT];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: mux_init
 *
 * Description:
 *   Set up the multiplexer, inactive: the UART in plain AT.
 *
 ****************************************************************************/

void mux_init(FAR struct mux_s *mux, FAR const struct mux_ops_s *ops,
              FAR void *arg);

/****************************************************************************
 * Name: mux_start
 *
 * Description:
 *   The modem has answered AT+CMUX: the UART carries frames from now on.
 *   No channel is open yet.
 *
 ****************************************************************************/

void mux_start(FAR struct mux_s *mux);

/****************************************************************************
 * Name: mux_reset
 *
 * Description:
 *   Back to plain AT, without telling the modem: it restarted, or went
 *   off.  What waits for the UART is dropped.
 *
 ****************************************************************************/

void mux_reset(FAR struct mux_s *mux);

/****************************************************************************
 * Name: mux_open
 *
 * Description:
 *   Ask the modem for a channel (SABM); MUX_OPENED tells of its answer.
 *
 * Returned Value:
 *   Zero (OK); -EAGAIN when the queue has no room; -ENOTCONN outside the
 *   multiplexer.
 *
 ****************************************************************************/

int mux_open(FAR struct mux_s *mux, uint8_t dlci);

/****************************************************************************
 * Name: mux_signals
 *
 * Description:
 *   Send a channel's V.24 signals (MSC): MUX_V24 when it opens,
 *   MUX_V24_HANGUP to hang up a data call.
 *
 * Returned Value:
 *   As mux_open().
 *
 ****************************************************************************/

int mux_signals(FAR struct mux_s *mux, uint8_t dlci, uint8_t signals);

/****************************************************************************
 * Name: mux_send
 *
 * Description:
 *   Send bytes on a channel, in frames of MUX_N1 bytes at most, all of them
 *   or none.  Outside the multiplexer, the bytes of MUX_AT go to the UART
 *   as they are.
 *
 * Returned Value:
 *   Zero (OK); -EAGAIN when the queue has no room for all of them, or the
 *   modem asked to wait, and MUX_ROOM tells when they may go; -ENOTCONN
 *   for a channel not open, or another than MUX_AT outside the
 *   multiplexer.
 *
 ****************************************************************************/

int mux_send(FAR struct mux_s *mux, uint8_t dlci, FAR const void *buf,
             size_t len);

/****************************************************************************
 * Name: mux_room
 *
 * Description:
 *   How many bytes of a channel mux_send() takes now, in whole frames.
 *
 ****************************************************************************/

size_t mux_room(FAR const struct mux_s *mux);

/****************************************************************************
 * Name: mux_close
 *
 * Description:
 *   Close the multiplexer down (CLD): the modem goes back to plain AT.
 *   The multiplexer is inactive once the close-down is written.
 *
 * Returned Value:
 *   As mux_open().
 *
 ****************************************************************************/

int mux_close(FAR struct mux_s *mux);

/****************************************************************************
 * Name: mux_kick
 *
 * Description:
 *   Whatever the modem is in, leave it in plain AT: a close-down, which
 *   ends a multiplexer left from before (a telephony program that stopped
 *   without closing it), then a carriage return, which ends the close-down
 *   as a line where the modem is in plain AT already.
 *
 * Returned Value:
 *   Zero (OK); -EAGAIN when the queue has no room.
 *
 ****************************************************************************/

int mux_kick(FAR struct mux_s *mux);

/****************************************************************************
 * Name: mux_input
 *
 * Description:
 *   The modem's bytes, while the multiplexer is active.
 *
 ****************************************************************************/

void mux_input(FAR struct mux_s *mux, FAR const uint8_t *buf, size_t len);

/****************************************************************************
 * Name: mux_flush
 *
 * Description:
 *   Write what waits, as far as the UART takes it.
 *
 * Returned Value:
 *   Zero (OK) when nothing is left; -EAGAIN when bytes still wait for the
 *   UART to take more; another negated errno value if the UART failed.
 *
 ****************************************************************************/

int mux_flush(FAR struct mux_s *mux);

/****************************************************************************
 * Name: mux_waiting
 *
 * Description:
 *   Whether bytes wait for the UART.
 *
 ****************************************************************************/

bool mux_waiting(FAR const struct mux_s *mux);

#endif /* __PNUT_OS_TELEPHONY_MUX_H */
