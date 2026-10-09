/****************************************************************************
 * pnut-os/src/lib/include/pnut/msg.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_LIB_PNUT_MSG_H
#define __PNUT_OS_LIB_PNUT_MSG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>

#include <pnut/compiler.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* A message on a socket: a 16-byte header, little-endian, then its body
 * (RFC 0023).  A message is at most 4 KB, header included.
 */

#define PNUT_MSG_VERSION      1
#define PNUT_MSG_HEADER       16
#define PNUT_MSG_MAX          4096
#define PNUT_MSG_PAYLOAD_MAX  (PNUT_MSG_MAX - PNUT_MSG_HEADER)

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum pnut_kind_e
{
  PNUT_KIND_REQUEST = 1,
  PNUT_KIND_REPLY,
  PNUT_KIND_EVENT,
  PNUT_KIND_STREAM,
};

/* The status of a reply (RFC 0023) */

enum pnut_status_e
{
  PNUT_STATUS_OK = 0,             /* Done */
  PNUT_STATUS_BUSY,               /* A queue is full; try later */
  PNUT_STATUS_DENIED,             /* The caller lacks the permission */
  PNUT_STATUS_NOTFOUND,           /* No such interface, method or thing */
  PNUT_STATUS_INVALID,            /* Does not parse, or asks the impossible */
  PNUT_STATUS_UNAVAILABLE,        /* Not ready, or its hardware is absent */
  PNUT_STATUS_TIMEOUT,            /* No reply in time */
  PNUT_STATUS_INTERNAL,           /* The service failed */
};

struct pnut_msghdr_s
{
  uint8_t version;                /* PNUT_MSG_VERSION */
  uint8_t kind;                   /* enum pnut_kind_e */
  uint16_t iface;                 /* The interface's number */
  uint16_t method;                /* The method's number */
  uint8_t mversion;               /* The method's version (RFC 0005) */
  uint8_t status;                 /* enum pnut_status_e, in replies */
  uint16_t seq;                   /* A reply carries its request's */
  uint16_t len;                   /* The body's length */
  uint16_t app;                   /* The app, on the runtime's connections */
  uint16_t reserved;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: pnut_msg_encode
 *
 * Description:
 *   Write a header's 16 bytes.
 *
 ****************************************************************************/

void pnut_msg_encode(FAR const struct pnut_msghdr_s *hdr, FAR uint8_t *buf);

/****************************************************************************
 * Name: pnut_msg_decode
 *
 * Description:
 *   Read a header's 16 bytes.
 *
 * Returned Value:
 *   Zero (OK); -EPROTO for another version, an unknown kind, or a body
 *   longer than PNUT_MSG_PAYLOAD_MAX.
 *
 ****************************************************************************/

int pnut_msg_decode(FAR const uint8_t *buf, FAR struct pnut_msghdr_s *hdr);

/****************************************************************************
 * Name: pnut_status_name
 *
 * Description:
 *   A status's name, for the log.
 *
 ****************************************************************************/

FAR const char *pnut_status_name(int status);

#endif /* __PNUT_OS_LIB_PNUT_MSG_H */
