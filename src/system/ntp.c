/****************************************************************************
 * pnut-os/src/system/ntp.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* An SNTP client (RFC 4330), for Time (RFC 0028): one question, one
 * answer, the time it gives taken at the answer's arrival plus half the
 * round trip.  The caller judges the reading as it does every source's.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "ntp.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NTP_LI(b)         ((b) >> 6)
#define NTP_VN(b)         (((b) >> 3) & 7)
#define NTP_MODE(b)       ((b) & 7)

#define NTP_MODE_CLIENT   3
#define NTP_MODE_SERVER   4
#define NTP_LI_ALARM      3       /* The server's clock not synchronised */

#define NTP_RX            32      /* The server's receive time */
#define NTP_ORIGINATE     24      /* The request's transmit time, back */
#define NTP_TX            40      /* The server's transmit time */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t ntp_be32(FAR const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* An NTP time as ms since 1970.  Its seconds wrap in February 2036: one
 * with the top bit clear is taken as after that (RFC 4330, section 3)
 */

static int64_t ntp_ms(FAR const uint8_t *p)
{
  uint64_t secs = ntp_be32(p);
  uint64_t frac = ntp_be32(p + 4);

  if ((secs & 0x80000000u) == 0)
    {
      secs += 0x100000000ull;
    }

  return (int64_t)(secs - NTP_UNIX_OFFSET) * 1000 +
         (int64_t)((frac * 1000) >> 32);
}

static uint64_t ntp_now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* A nonce: random, else the monotonic clock's nanoseconds mixed */

static void ntp_nonce(FAR uint8_t nonce[NTP_NONCE])
{
  static uint32_t count;
  struct timespec ts;
  uint64_t x;
  int i;

  if (getrandom(nonce, NTP_NONCE, GRND_NONBLOCK) == NTP_NONCE)
    {
      return;
    }

  clock_gettime(CLOCK_MONOTONIC, &ts);
  x = ((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec) ^
      ((uint64_t)__atomic_add_fetch(&count, 1, __ATOMIC_RELAXED) << 40) ^
      (uint64_t)(uintptr_t)&ts;
  for (i = 0; i < NTP_NONCE; i++)
    {
      x ^= x >> 33;
      x *= 0xff51afd7ed558ccdull;
      nonce[i] = (uint8_t)(x >> 56);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ntp_request(FAR uint8_t pkt[NTP_PACKET],
                 FAR const uint8_t nonce[NTP_NONCE])
{
  memset(pkt, 0, NTP_PACKET);
  pkt[0] = (4 << 3) | NTP_MODE_CLIENT;
  memcpy(pkt + NTP_TX, nonce, NTP_NONCE);
}

int ntp_parse(FAR const uint8_t *pkt, size_t len,
              FAR const uint8_t nonce[NTP_NONCE], FAR int64_t *rx_ms,
              FAR int64_t *tx_ms)
{
  static const uint8_t zero[8];

  if (len < NTP_PACKET || NTP_MODE(pkt[0]) != NTP_MODE_SERVER ||
      NTP_VN(pkt[0]) < 3 || NTP_VN(pkt[0]) > 4 ||
      memcmp(pkt + NTP_ORIGINATE, nonce, NTP_NONCE) != 0)
    {
      return -EPROTO;             /* Not an answer to this request */
    }

  if (pkt[1] == 0)
    {
      return -ECONNREFUSED;       /* A kiss-o'-death */
    }

  if (pkt[1] > 15 || NTP_LI(pkt[0]) == NTP_LI_ALARM ||
      memcmp(pkt + NTP_TX, zero, sizeof(zero)) == 0 ||
      memcmp(pkt + NTP_RX, zero, sizeof(zero)) == 0)
    {
      return -EPROTO;
    }

  *rx_ms = ntp_ms(pkt + NTP_RX);
  *tx_ms = ntp_ms(pkt + NTP_TX);
  return OK;
}

int ntp_query(FAR const char *server, uint16_t port, uint32_t wait_ms,
              int tries, FAR struct ntp_result_s *out)
{
  struct addrinfo hints;
  FAR struct addrinfo *ai = NULL;
  struct timeval tv;
  uint8_t nonce[NTP_NONCE];
  uint8_t pkt[NTP_PACKET];
  uint8_t in[NTP_PACKET + 4];
  char service[8];
  uint64_t sent = 0;
  uint64_t now = 0;
  int64_t rx = 0;
  int64_t tx = 0;
  int64_t delay;
  ssize_t n;
  int sock;
  int ret;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  snprintf(service, sizeof(service), "%u", (unsigned int)port);
  if (getaddrinfo(server, service, &hints, &ai) != 0 || ai == NULL)
    {
      return -EHOSTUNREACH;
    }

  sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (sock < 0)
    {
      ret = -errno;
      freeaddrinfo(ai);
      return ret;
    }

  /* Connected: replies from anywhere else are not read */

  ret = connect(sock, ai->ai_addr, ai->ai_addrlen) < 0 ? -errno : OK;
  freeaddrinfo(ai);

  tv.tv_sec  = wait_ms / 1000;
  tv.tv_usec = (wait_ms % 1000) * 1000;
  if (ret == OK &&
      setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
    {
      ret = -errno;
    }

  for (; ret == OK && tries > 0; tries--)
    {
      ret = -ETIMEDOUT;
      ntp_nonce(nonce);
      ntp_request(pkt, nonce);
      sent = ntp_now();
      if (send(sock, pkt, sizeof(pkt), 0) != sizeof(pkt))
        {
          ret = errno != 0 ? -errno : -EIO;
          continue;
        }

      /* Until the answer, or the wait is over: strays are read past */

      for (; ; )
        {
          n = recv(sock, in, sizeof(in), 0);
          now = ntp_now();
          if (n < 0 && errno == EINTR && now - sent < wait_ms)
            {
              continue;
            }

          /* A port closed (ICMP, where the stack reports it) is not a
           * kiss-o'-death, which alone is -ECONNREFUSED
           */

          if (n < 0)
            {
              ret = errno == EAGAIN || errno == EWOULDBLOCK ? -ETIMEDOUT :
                    errno == ECONNREFUSED ? -EHOSTUNREACH : -errno;
              break;
            }

          ret = ntp_parse(in, (size_t)n, nonce, &rx, &tx);
          if (ret != -EPROTO || now - sent >= wait_ms)
            {
              break;
            }
        }

      if (ret == OK || ret == -ECONNREFUSED)
        {
          break;
        }

      if (tries > 1)
        {
          ret = OK;               /* Once more */
        }
    }

  explicit_bzero(nonce, sizeof(nonce));
  close(sock);
  if (ret < 0)
    {
      return ret;
    }

  /* The round trip, less the server's own time over it; half of it on
   * top of what the server sent
   */

  delay = (int64_t)(now - sent) - (tx - rx);
  if (delay < 0)
    {
      delay = 0;
    }

  out->utc_ms         = tx + delay / 2;
  out->taken_ms       = now;
  out->uncertainty_ms = (uint32_t)(delay / 2) + 1;
  return OK;
}
