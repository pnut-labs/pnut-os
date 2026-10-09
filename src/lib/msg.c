/****************************************************************************
 * pnut-os/src/lib/msg.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>

#include <pnut/msg.h>

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR const char * const g_status[] =
{
  "ok",
  "busy",
  "denied",
  "not found",
  "invalid",
  "unavailable",
  "timeout",
  "internal",
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void put16(FAR uint8_t *buf, uint16_t value)
{
  buf[0] = value & 0xff;
  buf[1] = value >> 8;
}

static uint16_t get16(FAR const uint8_t *buf)
{
  return (uint16_t)(buf[0] | (buf[1] << 8));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void pnut_msg_encode(FAR const struct pnut_msghdr_s *hdr, FAR uint8_t *buf)
{
  buf[0] = hdr->version;
  buf[1] = hdr->kind;
  put16(&buf[2], hdr->iface);
  put16(&buf[4], hdr->method);
  buf[6] = hdr->mversion;
  buf[7] = hdr->status;
  put16(&buf[8], hdr->seq);
  put16(&buf[10], hdr->len);
  put16(&buf[12], hdr->app);
  put16(&buf[14], hdr->reserved);
}

int pnut_msg_decode(FAR const uint8_t *buf, FAR struct pnut_msghdr_s *hdr)
{
  hdr->version  = buf[0];
  hdr->kind     = buf[1];
  hdr->iface    = get16(&buf[2]);
  hdr->method   = get16(&buf[4]);
  hdr->mversion = buf[6];
  hdr->status   = buf[7];
  hdr->seq      = get16(&buf[8]);
  hdr->len      = get16(&buf[10]);
  hdr->app      = get16(&buf[12]);
  hdr->reserved = get16(&buf[14]);

  if (hdr->version != PNUT_MSG_VERSION ||
      hdr->kind < PNUT_KIND_REQUEST || hdr->kind > PNUT_KIND_STREAM ||
      hdr->len > PNUT_MSG_PAYLOAD_MAX)
    {
      return -EPROTO;
    }

  return OK;
}

FAR const char *pnut_status_name(int status)
{
  if (status < 0 || status >= (int)(sizeof(g_status) / sizeof(g_status[0])))
    {
      return "unknown";
    }

  return g_status[status];
}
