/****************************************************************************
 * pnut-os/src/sensors/ubx.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* u-blox's UBX protocol (RFC 0035): frames built and parsed, the messages
 * Location uses read and written.  Everything is little endian.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <string.h>

#include "ubx.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The parser's states */

#define UBX_WAIT_SYNC1   0
#define UBX_WAIT_SYNC2   1
#define UBX_WAIT_CLASS   2
#define UBX_WAIT_ID      3
#define UBX_WAIT_LEN1    4
#define UBX_WAIT_LEN2    5
#define UBX_PAYLOAD      6
#define UBX_WAIT_CK_A    7
#define UBX_WAIT_CK_B    8

#define UBX_MGA_INI_TIME_UTC  0x10
#define UBX_MGA_INI_POS_LLH   0x01

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void put_u16(FAR uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put_u32(FAR uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint16_t get_u16(FAR const uint8_t *p)
{
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(FAR const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/* A key's value's size in bytes: bits 30 to 28 of its id; 0 unknown */

static size_t ubx_key_size(uint32_t key)
{
  switch ((key >> 28) & 0x07)
    {
      case 1:
      case 2:
        return 1;

      case 3:
        return 2;

      case 4:
        return 4;

      default:
        return 0;
    }
}

/* Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm) */

static int64_t ubx_days(int64_t y, unsigned m, unsigned d)
{
  int64_t era;
  unsigned yoe;
  unsigned doy;
  unsigned doe;

  y   -= m <= 2;
  era  = (y >= 0 ? y : y - 399) / 400;
  yoe  = (unsigned)(y - era * 400);
  doy  = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe  = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ubx_checksum(FAR const uint8_t *data, size_t len, FAR uint8_t *ck_a,
                  FAR uint8_t *ck_b)
{
  uint8_t a = 0;
  uint8_t b = 0;
  size_t i;

  for (i = 0; i < len; i++)
    {
      a += data[i];
      b += a;
    }

  *ck_a = a;
  *ck_b = b;
}

size_t ubx_build(FAR uint8_t *buf, size_t size, uint8_t cls, uint8_t id,
                 FAR const uint8_t *payload, uint16_t len)
{
  if (size < (size_t)len + UBX_OVERHEAD)
    {
      return 0;
    }

  buf[0] = UBX_SYNC1;
  buf[1] = UBX_SYNC2;
  buf[2] = cls;
  buf[3] = id;
  put_u16(&buf[4], len);
  if (len > 0)
    {
      memmove(&buf[6], payload, len);
    }

  ubx_checksum(&buf[2], (size_t)len + 4, &buf[6 + len], &buf[7 + len]);
  return (size_t)len + UBX_OVERHEAD;
}

size_t ubx_valset(FAR uint8_t *buf, size_t size,
                  FAR const struct ubx_item_s *items, size_t n)
{
  uint8_t payload[UBX_PAYLOAD_MAX];
  size_t len = 4;
  size_t vsize;
  size_t i;

  memset(payload, 0, 4);
  payload[1] = 0x01;              /* The RAM layer */

  for (i = 0; i < n; i++)
    {
      vsize = ubx_key_size(items[i].key);
      if (vsize == 0 || len + 4 + vsize > sizeof(payload))
        {
          return 0;
        }

      put_u32(&payload[len], items[i].key);
      len += 4;
      if (vsize == 1)
        {
          payload[len] = (uint8_t)items[i].value;
        }
      else if (vsize == 2)
        {
          put_u16(&payload[len], (uint16_t)items[i].value);
        }
      else
        {
          put_u32(&payload[len], items[i].value);
        }

      len += vsize;
    }

  return ubx_build(buf, size, UBX_CLASS_CFG, UBX_CFG_VALSET, payload,
                   (uint16_t)len);
}

size_t ubx_valget(FAR uint8_t *buf, size_t size, FAR const uint32_t *keys,
                  size_t n)
{
  uint8_t payload[UBX_PAYLOAD_MAX];
  size_t i;

  if (4 + 4 * n > sizeof(payload))
    {
      return 0;
    }

  memset(payload, 0, 4);          /* Version 0, the RAM layer, from 0 */
  for (i = 0; i < n; i++)
    {
      put_u32(&payload[4 + 4 * i], keys[i]);
    }

  return ubx_build(buf, size, UBX_CLASS_CFG, UBX_CFG_VALGET, payload,
                   (uint16_t)(4 + 4 * n));
}

/* The receiver's time on receipt of the message; leap seconds unknown */

size_t ubx_ini_time(FAR uint8_t *buf, size_t size, int64_t utc,
                    uint32_t ns, uint32_t acc_ms)
{
  uint8_t p[24];
  int64_t days = utc >= 0 ? utc / 86400 : (utc - 86399) / 86400;
  int64_t secs = utc - days * 86400;
  int64_t z = days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned day = doy - (153 * mp + 2) / 5 + 1;
  unsigned month = mp < 10 ? mp + 3 : mp - 9;
  int64_t year = (int64_t)yoe + era * 400 + (month <= 2);

  memset(p, 0, sizeof(p));
  p[0] = UBX_MGA_INI_TIME_UTC;
  p[3] = 0x80;                    /* Leap seconds unknown */
  put_u16(&p[4], (uint16_t)year);
  p[6]  = (uint8_t)month;
  p[7]  = (uint8_t)day;
  p[8]  = (uint8_t)(secs / 3600);
  p[9]  = (uint8_t)(secs / 60 % 60);
  p[10] = (uint8_t)(secs % 60);
  put_u32(&p[12], ns);
  put_u16(&p[16], (uint16_t)(acc_ms / 1000));
  put_u32(&p[20], (acc_ms % 1000) * 1000000);
  return ubx_build(buf, size, UBX_CLASS_MGA, UBX_MGA_INI, p, sizeof(p));
}

size_t ubx_ini_pos(FAR uint8_t *buf, size_t size, int32_t lat,
                   int32_t lon, int32_t alt_cm, uint32_t acc_cm)
{
  uint8_t p[20];

  memset(p, 0, sizeof(p));
  p[0] = UBX_MGA_INI_POS_LLH;
  put_u32(&p[4], (uint32_t)lat);
  put_u32(&p[8], (uint32_t)lon);
  put_u32(&p[12], (uint32_t)alt_cm);
  put_u32(&p[16], acc_cm);
  return ubx_build(buf, size, UBX_CLASS_MGA, UBX_MGA_INI, p, sizeof(p));
}

void ubx_parser_init(FAR struct ubx_parser_s *parser)
{
  memset(parser, 0, sizeof(*parser));
}

bool ubx_parse(FAR struct ubx_parser_s *parser, uint8_t byte)
{
  FAR struct ubx_frame_s *f = &parser->frame;

  if (parser->state > UBX_WAIT_SYNC2 && parser->state < UBX_WAIT_CK_A)
    {
      parser->ck_a += byte;
      parser->ck_b += parser->ck_a;
    }

  switch (parser->state)
    {
      case UBX_WAIT_SYNC1:
        if (byte == UBX_SYNC1)
          {
            parser->state = UBX_WAIT_SYNC2;
          }

        break;

      case UBX_WAIT_SYNC2:
        parser->state = byte == UBX_SYNC2 ? UBX_WAIT_CLASS :
                        byte == UBX_SYNC1 ? UBX_WAIT_SYNC2 : UBX_WAIT_SYNC1;
        parser->ck_a = 0;
        parser->ck_b = 0;
        break;

      case UBX_WAIT_CLASS:
        f->cls = byte;
        parser->state = UBX_WAIT_ID;
        break;

      case UBX_WAIT_ID:
        f->id = byte;
        parser->state = UBX_WAIT_LEN1;
        break;

      case UBX_WAIT_LEN1:
        parser->len = byte;
        parser->state = UBX_WAIT_LEN2;
        break;

      case UBX_WAIT_LEN2:
        parser->len |= (uint16_t)byte << 8;
        parser->pos  = 0;
        if (parser->len > UBX_PAYLOAD_MAX)
          {
            parser->errors++;
            parser->state = UBX_WAIT_SYNC1;
            break;
          }

        parser->state = parser->len > 0 ? UBX_PAYLOAD : UBX_WAIT_CK_A;
        break;

      case UBX_PAYLOAD:
        f->payload[parser->pos] = byte;
        if (++parser->pos == parser->len)
          {
            parser->state = UBX_WAIT_CK_A;
          }

        break;

      case UBX_WAIT_CK_A:
        parser->state = byte == parser->ck_a ? UBX_WAIT_CK_B :
                        UBX_WAIT_SYNC1;
        if (byte != parser->ck_a)
          {
            parser->errors++;
          }

        break;

      case UBX_WAIT_CK_B:
        parser->state = UBX_WAIT_SYNC1;
        if (byte != parser->ck_b)
          {
            parser->errors++;
            return false;
          }

        f->len = parser->len;
        return true;
    }

  return false;
}

bool ubx_nav_pvt(FAR const struct ubx_frame_s *frame,
                 FAR struct ubx_pvt_s *pvt)
{
  FAR const uint8_t *p = frame->payload;

  if (frame->cls != UBX_CLASS_NAV || frame->id != UBX_NAV_PVT ||
      frame->len < 92)
    {
      return false;
    }

  memset(pvt, 0, sizeof(*pvt));
  pvt->itow       = get_u32(&p[0]);
  pvt->year       = get_u16(&p[4]);
  pvt->month      = p[6];
  pvt->day        = p[7];
  pvt->hour       = p[8];
  pvt->min        = p[9];
  pvt->sec        = p[10];
  pvt->valid      = p[11];
  pvt->tacc_ns    = get_u32(&p[12]);
  pvt->nano       = (int32_t)get_u32(&p[16]);
  pvt->fix        = p[20];
  pvt->flags      = p[21];
  pvt->flags2     = p[22];
  pvt->numsv      = p[23];
  pvt->lon        = (int32_t)get_u32(&p[24]);
  pvt->lat        = (int32_t)get_u32(&p[28]);
  pvt->height_mm  = (int32_t)get_u32(&p[32]);
  pvt->hmsl_mm    = (int32_t)get_u32(&p[36]);
  pvt->hacc_mm    = get_u32(&p[40]);
  pvt->vacc_mm    = get_u32(&p[44]);
  pvt->gspeed_mms = (int32_t)get_u32(&p[60]);
  pvt->head_e5    = (int32_t)get_u32(&p[64]);
  pvt->sacc_mms   = get_u32(&p[68]);
  pvt->headacc_e5 = get_u32(&p[72]);
  pvt->pdop       = get_u16(&p[76]);
  pvt->flags3     = get_u16(&p[78]);
  return true;
}

bool ubx_ack(FAR const struct ubx_frame_s *frame, FAR uint8_t *cls,
             FAR uint8_t *id, FAR bool *ack)
{
  if (frame->cls != UBX_CLASS_ACK || frame->len < 2 ||
      (frame->id != UBX_ACK_ACK && frame->id != UBX_ACK_NAK))
    {
      return false;
    }

  *cls = frame->payload[0];
  *id  = frame->payload[1];
  *ack = frame->id == UBX_ACK_ACK;
  return true;
}

bool ubx_mga_ack(FAR const struct ubx_frame_s *frame, FAR bool *accepted,
                 FAR uint8_t *info, FAR uint8_t *msgid,
                 FAR uint32_t *start)
{
  if (frame->cls != UBX_CLASS_MGA || frame->id != UBX_MGA_ACK ||
      frame->len < 8)
    {
      return false;
    }

  *accepted = frame->payload[0] == 1;
  *info     = frame->payload[2];
  *msgid    = frame->payload[3];
  *start    = get_u32(&frame->payload[4]);
  return true;
}

bool ubx_pvt_utc(FAR const struct ubx_pvt_s *pvt, FAR int64_t *utc)
{
  uint8_t need = UBX_PVT_VALID_DATE | UBX_PVT_VALID_TIME | UBX_PVT_RESOLVED;

  if ((pvt->valid & need) != need || pvt->month < 1 || pvt->month > 12 ||
      pvt->day < 1 || pvt->day > 31)
    {
      return false;
    }

  *utc = ubx_days(pvt->year, pvt->month, pvt->day) * 86400 +
         pvt->hour * 3600 + pvt->min * 60 + pvt->sec;
  return true;
}
