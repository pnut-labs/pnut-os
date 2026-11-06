/****************************************************************************
 * pnut-os/src/system/ntp.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_SYSTEM_NTP_H
#define __PNUT_OS_SYSTEM_NTP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>

#include <pnut/compiler.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NTP_PORT          123
#define NTP_PACKET        48      /* An SNTP message, no extensions */
#define NTP_NONCE         8       /* The transmit time a request carries */

/* Seconds from NTP's epoch (1900) to the Unix epoch (1970) */

#define NTP_UNIX_OFFSET   2208988800ull

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A server's answer: the time when it arrived */

struct ntp_result_s
{
  int64_t utc_ms;                 /* The time, ms since 1970, at taken */
  uint64_t taken_ms;              /* CLOCK_MONOTONIC, ms, at its arrival */
  uint32_t uncertainty_ms;        /* Half the round trip */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: ntp_request
 *
 * Description:
 *   An SNTP request (RFC 4330): version 4, client, its transmit time the
 *   nonce, which a reply must give back.  Not the clock: it may be years
 *   off, and would tell the network what it reads.
 *
 ****************************************************************************/

void ntp_request(FAR uint8_t pkt[NTP_PACKET],
                 FAR const uint8_t nonce[NTP_NONCE]);

/****************************************************************************
 * Name: ntp_parse
 *
 * Description:
 *   A server's reply to the request that carried nonce: its receive and
 *   transmit times, ms since 1970.
 *
 * Returned Value:
 *   0; -EPROTO, not a reply to it, or not one to trust (not a server's,
 *   no time, the server not synchronised); -ECONNREFUSED, the server says
 *   to go away (a kiss-o'-death, stratum 0).
 *
 ****************************************************************************/

int ntp_parse(FAR const uint8_t *pkt, size_t len,
              FAR const uint8_t nonce[NTP_NONCE], FAR int64_t *rx_ms,
              FAR int64_t *tx_ms);

/****************************************************************************
 * Name: ntp_query
 *
 * Description:
 *   Ask a server the time: its name looked up, a request sent, a reply
 *   awaited wait_ms, up to tries times.  It blocks: for a worker.
 *
 * Returned Value:
 *   0 and the result; -EHOSTUNREACH, the name not found; -ETIMEDOUT, no
 *   reply; -ECONNREFUSED, a kiss-o'-death; or another negated errno
 *   value.
 *
 ****************************************************************************/

int ntp_query(FAR const char *server, uint16_t port, uint32_t wait_ms,
              int tries, FAR struct ntp_result_s *out);

#endif /* __PNUT_OS_SYSTEM_NTP_H */
