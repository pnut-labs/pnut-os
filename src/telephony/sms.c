/****************************************************************************
 * pnut-os/src/telephony/sms.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <string.h>

#include "parse.h"
#include "sms.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SMS_ESC           0x1b      /* GSM 7-bit: the extension table's */
#define SMS_REPLACEMENT   0xfffd
#define SMS_UNMAPPED      0xffff

/* TP-MTI, and the first octet's flags (TS 23.040 9.2.3) */

#define SMS_MTI_MASK      0x03
#define SMS_MTI_DELIVER   0x00
#define SMS_MTI_SUBMIT    0x01
#define SMS_SRR           0x20      /* SMS-SUBMIT: a report requested */
#define SMS_UDHI          0x40

/* The alphabets a DCS names */

#define SMS_GSM7          0
#define SMS_8BIT          1
#define SMS_UCS2          2

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* GSM 7-bit's default alphabet (TS 23.038 6.2.1), as Unicode */

static const uint16_t g_sms_gsm7[128] =
{
  0x0040, 0x00a3, 0x0024, 0x00a5, 0x00e8, 0x00e9, 0x00f9, 0x00ec,
  0x00f2, 0x00c7, 0x000a, 0x00d8, 0x00f8, 0x000d, 0x00c5, 0x00e5,
  0x0394, 0x005f, 0x03a6, 0x0393, 0x039b, 0x03a9, 0x03a0, 0x03a8,
  0x03a3, 0x0398, 0x039e, 0x00a0, 0x00c6, 0x00e6, 0x00df, 0x00c9,
  0x0020, 0x0021, 0x0022, 0x0023, 0x00a4, 0x0025, 0x0026, 0x0027,
  0x0028, 0x0029, 0x002a, 0x002b, 0x002c, 0x002d, 0x002e, 0x002f,
  0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
  0x0038, 0x0039, 0x003a, 0x003b, 0x003c, 0x003d, 0x003e, 0x003f,
  0x00a1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
  0x0048, 0x0049, 0x004a, 0x004b, 0x004c, 0x004d, 0x004e, 0x004f,
  0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
  0x0058, 0x0059, 0x005a, 0x00c4, 0x00d6, 0x00d1, 0x00dc, 0x00a7,
  0x00bf, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
  0x0068, 0x0069, 0x006a, 0x006b, 0x006c, 0x006d, 0x006e, 0x006f,
  0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
  0x0078, 0x0079, 0x007a, 0x00e4, 0x00f6, 0x00f1, 0x00fc, 0x00e0
};

/* Its extension table (TS 23.038 6.2.1.1): after the escape */

struct sms_ext_s
{
  uint8_t code;
  uint16_t ch;
};

static const struct sms_ext_s g_sms_ext[] =
{
  { 0x0a, 0x000c },               /* Form feed */
  { 0x14, 0x005e },               /* ^ */
  { 0x28, 0x007b },               /* { */
  { 0x29, 0x007d },               /* } */
  { 0x2f, 0x005c },               /* \ */
  { 0x3c, 0x005b },               /* [ */
  { 0x3d, 0x007e },               /* ~ */
  { 0x3e, 0x005d },               /* ] */
  { 0x40, 0x007c },               /* | */
  { 0x65, 0x20ac },               /* The euro sign */
};

#define SMS_EXT_COUNT (sizeof(g_sms_ext) / sizeof(g_sms_ext[0]))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sms_nibble, sms_hex
 *
 * Description:
 *   A hex digit's value, or -1; hex into octets.
 *
 * Returned Value:
 *   sms_hex(): the octets; -EINVAL for an odd length or a character that
 *   is not hex.
 *
 ****************************************************************************/

static int sms_nibble(char c)
{
  if (c >= '0' && c <= '9')
    {
      return c - '0';
    }

  if (c >= 'A' && c <= 'F')
    {
      return c - 'A' + 10;
    }

  if (c >= 'a' && c <= 'f')
    {
      return c - 'a' + 10;
    }

  return -1;
}

static int sms_hex(FAR const char *hex, FAR uint8_t *out, size_t size)
{
  size_t n = 0;
  int hi;
  int lo;

  while (hex[0] != '\0' && hex[0] != '\r' && hex[0] != '\n')
    {
      hi = sms_nibble(hex[0]);
      lo = hex[1] != '\0' ? sms_nibble(hex[1]) : -1;
      if (hi < 0 || lo < 0 || n == size)
        {
          return -EINVAL;
        }

      out[n++] = (uint8_t)((hi << 4) | lo);
      hex += 2;
    }

  return (int)n;
}

/****************************************************************************
 * Name: sms_put_hex
 ****************************************************************************/

static void sms_put_hex(FAR char *out, FAR const uint8_t *in, size_t n)
{
  static const char digits[] = "0123456789ABCDEF";
  size_t i;

  for (i = 0; i < n; i++)
    {
      out[2 * i]     = digits[in[i] >> 4];
      out[2 * i + 1] = digits[in[i] & 0x0f];
    }

  out[2 * n] = '\0';
}

/****************************************************************************
 * Name: sms_utf8_put
 *
 * Description:
 *   A character as UTF-8 at *pos in out, if it fits with a NUL after it;
 *   once one does not, none after it goes in either, so the text is cut,
 *   never holed.  A NUL is no character here.
 *
 ****************************************************************************/

static void sms_utf8_put(FAR char *out, size_t size, FAR size_t *pos,
                         uint32_t ch)
{
  uint8_t b[4];
  size_t n;

  if (ch == 0 || ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff))
    {
      ch = SMS_REPLACEMENT;
    }

  if (ch < 0x80)
    {
      b[0] = (uint8_t)ch;
      n = 1;
    }
  else if (ch < 0x800)
    {
      b[0] = (uint8_t)(0xc0 | (ch >> 6));
      b[1] = (uint8_t)(0x80 | (ch & 0x3f));
      n = 2;
    }
  else if (ch < 0x10000)
    {
      b[0] = (uint8_t)(0xe0 | (ch >> 12));
      b[1] = (uint8_t)(0x80 | ((ch >> 6) & 0x3f));
      b[2] = (uint8_t)(0x80 | (ch & 0x3f));
      n = 3;
    }
  else
    {
      b[0] = (uint8_t)(0xf0 | (ch >> 18));
      b[1] = (uint8_t)(0x80 | ((ch >> 12) & 0x3f));
      b[2] = (uint8_t)(0x80 | ((ch >> 6) & 0x3f));
      b[3] = (uint8_t)(0x80 | (ch & 0x3f));
      n = 4;
    }

  if (*pos + n < size)
    {
      memcpy(out + *pos, b, n);
      *pos += n;
      out[*pos] = '\0';
    }
  else if (*pos + 1 < size)
    {
      out[*pos] = '\0';
      *pos = size - 1;            /* Full: nothing more goes in */
    }
}

/****************************************************************************
 * Name: sms_utf8_get
 *
 * Description:
 *   The next character of a UTF-8 text.
 *
 * Returned Value:
 *   Its bytes; 0 at the end; -EINVAL for bytes that are not UTF-8.
 *
 ****************************************************************************/

static int sms_utf8_get(FAR const char *s, FAR uint32_t *ch)
{
  FAR const uint8_t *p = (FAR const uint8_t *)s;
  int n;
  int i;

  if (p[0] == 0)
    {
      return 0;
    }

  if (p[0] < 0x80)
    {
      *ch = p[0];
      return 1;
    }

  if ((p[0] & 0xe0) == 0xc0)
    {
      *ch = p[0] & 0x1f;
      n = 2;
    }
  else if ((p[0] & 0xf0) == 0xe0)
    {
      *ch = p[0] & 0x0f;
      n = 3;
    }
  else if ((p[0] & 0xf8) == 0xf0)
    {
      *ch = p[0] & 0x07;
      n = 4;
    }
  else
    {
      return -EINVAL;
    }

  for (i = 1; i < n; i++)
    {
      if ((p[i] & 0xc0) != 0x80)
        {
          return -EINVAL;
        }

      *ch = (*ch << 6) | (p[i] & 0x3f);
    }

  /* Overlong forms, surrogates and what Unicode does not have */

  if ((n == 2 && *ch < 0x80) || (n == 3 && *ch < 0x800) ||
      (n == 4 && *ch < 0x10000) || *ch > 0x10ffff ||
      (*ch >= 0xd800 && *ch <= 0xdfff))
    {
      return -EINVAL;
    }

  return n;
}

/****************************************************************************
 * Name: sms_gsm7_code
 *
 * Description:
 *   A character's GSM 7-bit code: one septet, or two with the escape.
 *
 * Returned Value:
 *   Its septets, 1 or 2; 0 when GSM 7-bit has no code for it.
 *
 ****************************************************************************/

static int sms_gsm7_code(uint32_t ch, FAR uint8_t *code)
{
  size_t i;

  for (i = 0; i < 128; i++)
    {
      if (g_sms_gsm7[i] == ch && i != SMS_ESC)
        {
          code[0] = (uint8_t)i;
          return 1;
        }
    }

  for (i = 0; i < SMS_EXT_COUNT; i++)
    {
      if (g_sms_ext[i].ch == ch)
        {
          code[0] = SMS_ESC;
          code[1] = g_sms_ext[i].code;
          return 2;
        }
    }

  return 0;
}

/****************************************************************************
 * Name: sms_unpack
 *
 * Description:
 *   Septet n of 7-bit packed octets.
 *
 ****************************************************************************/

static uint8_t sms_unpack(FAR const uint8_t *p, size_t n)
{
  size_t bit = n * 7;
  size_t i = bit / 8;
  unsigned int shift = bit % 8;
  unsigned int v = p[i] >> shift;

  if (shift > 1)
    {
      v |= (unsigned int)p[i + 1] << (8 - shift);
    }

  return (uint8_t)(v & 0x7f);
}

/****************************************************************************
 * Name: sms_pack
 *
 * Description:
 *   Septets packed, from bit offset `start` of out (which is zeroed).
 *
 * Returned Value:
 *   The octets used, from the start of out.
 *
 ****************************************************************************/

static size_t sms_pack(FAR const uint8_t *septets, size_t n,
                       FAR uint8_t *out, size_t start)
{
  size_t bit = start;
  size_t i;

  for (i = 0; i < n; i++, bit += 7)
    {
      out[bit / 8] |= (uint8_t)(septets[i] << (bit % 8));
      if (bit % 8 > 1)
        {
          out[bit / 8 + 1] |= (uint8_t)(septets[i] >> (8 - bit % 8));
        }
    }

  return (bit + 7) / 8;
}

/****************************************************************************
 * Name: sms_gsm7_text
 *
 * Description:
 *   Septets from `from` to `n` of packed octets into UTF-8.
 *
 ****************************************************************************/

static void sms_gsm7_text(FAR const uint8_t *p, size_t from, size_t n,
                          FAR char *out, size_t size)
{
  size_t pos = 0;
  size_t i;
  size_t j;
  uint8_t c;

  out[0] = '\0';
  for (i = from; i < n; i++)
    {
      c = sms_unpack(p, i);
      if (c != SMS_ESC)
        {
          sms_utf8_put(out, size, &pos, g_sms_gsm7[c]);
          continue;
        }

      /* The extension table; a code it does not have is the default
       * table's, as TS 23.038 lets a receiver show it (an escape twice
       * is the default table's 0x1b, a space); an escape at the end is
       * nothing
       */

      if (++i == n)
        {
          break;
        }

      c = sms_unpack(p, i);
      for (j = 0; j < SMS_EXT_COUNT; j++)
        {
          if (g_sms_ext[j].code == c)
            {
              break;
            }
        }

      sms_utf8_put(out, size, &pos, j < SMS_EXT_COUNT ? g_sms_ext[j].ch :
                   g_sms_gsm7[c]);
    }
}

/****************************************************************************
 * Name: sms_ucs2_text
 *
 * Description:
 *   UTF-16BE into UTF-8, surrogate pairs joined.
 *
 ****************************************************************************/

static void sms_ucs2_text(FAR const uint8_t *p, size_t n, FAR char *out,
                          size_t size)
{
  size_t pos = 0;
  size_t i;
  uint32_t u;
  uint32_t lo;

  out[0] = '\0';
  for (i = 0; i + 1 < n; i += 2)
    {
      u = ((uint32_t)p[i] << 8) | p[i + 1];
      if (u >= 0xd800 && u <= 0xdbff && i + 3 < n)
        {
          lo = ((uint32_t)p[i + 2] << 8) | p[i + 3];
          if (lo >= 0xdc00 && lo <= 0xdfff)
            {
              u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
              i += 2;
            }
        }

      sms_utf8_put(out, size, &pos, u);
    }
}

/****************************************************************************
 * Name: sms_alphabet
 *
 * Description:
 *   The alphabet a DCS names (TS 23.038 4).
 *
 * Returned Value:
 *   SMS_GSM7, SMS_8BIT, SMS_UCS2; -ENOTSUP for compressed text.
 *
 ****************************************************************************/

static int sms_alphabet(uint8_t dcs)
{
  if ((dcs & 0x80) == 0)
    {
      /* General data coding, and marked for deletion */

      if (dcs & 0x20)
        {
          return -ENOTSUP;
        }

      switch ((dcs >> 2) & 0x03)
        {
          case 1:
            return SMS_8BIT;

          case 2:
            return SMS_UCS2;

          default:
            return SMS_GSM7;
        }
    }

  switch (dcs & 0xf0)
    {
      case 0xe0:
        return SMS_UCS2;          /* Message waiting, stored, UCS-2 */

      case 0xf0:
        return (dcs & 0x04) ? SMS_8BIT : SMS_GSM7;

      default:
        return SMS_GSM7;          /* Message waiting, and reserved */
    }
}

/****************************************************************************
 * Name: sms_address
 *
 * Description:
 *   An address field (TS 23.040 9.1.2.5): digits in semi-octets, '+'
 *   first when international, or a name in GSM 7-bit.
 *
 * Returned Value:
 *   The octets it takes; -EINVAL when cut short.
 *
 ****************************************************************************/

static int sms_address(FAR const uint8_t *p, size_t len, FAR char *out,
                       size_t size)
{
  static const char digits[] = "0123456789*#abc";
  uint8_t ndigits;
  uint8_t type;
  size_t octets;
  size_t pos = 0;
  size_t i;
  size_t n;
  uint8_t d;

  if (len < 2)
    {
      return -EINVAL;
    }

  ndigits = p[0];
  type    = p[1];
  octets  = (ndigits + 1) / 2;
  if (2 + octets > len)
    {
      return -EINVAL;
    }

  out[0] = '\0';
  if (((type >> 4) & 0x07) == 5)
    {
      /* Alphanumeric: GSM 7-bit, its septets in the semi-octets.  Some
       * networks count the semi-octets of whole octets: when the septets
       * fill those exactly, a last one of zero bits is their padding, not
       * an '@'
       */

      n = ndigits * 4 / 7;
      if (n > 0 && ndigits % 2 == 0 && n * 7 == (size_t)ndigits * 4 &&
          sms_unpack(p + 2, n - 1) == 0)
        {
          n--;
        }

      sms_gsm7_text(p + 2, 0, n, out, size);
      return (int)(2 + octets);
    }

  if (((type >> 4) & 0x07) == 1 && pos + 1 < size)
    {
      out[pos++] = '+';
    }

  for (i = 0; i < ndigits && pos + 1 < size; i++)
    {
      d = (i & 1) ? (p[2 + i / 2] >> 4) : (p[2 + i / 2] & 0x0f);
      if (d == 0x0f)
        {
          break;
        }

      out[pos++] = digits[d];
    }

  out[pos] = '\0';
  return (int)(2 + octets);
}

/****************************************************************************
 * Name: sms_bcd
 *
 * Description:
 *   Two swapped semi-octets as a number.
 *
 ****************************************************************************/

static int sms_bcd(uint8_t o)
{
  return (o & 0x0f) * 10 + (o >> 4);
}

/****************************************************************************
 * Name: sms_utf16_units
 *
 * Description:
 *   A character's UTF-16 units.
 *
 ****************************************************************************/

static int sms_utf16_units(uint32_t ch, FAR uint16_t *u)
{
  if (ch < 0x10000)
    {
      u[0] = (uint16_t)ch;
      return 1;
    }

  ch -= 0x10000;
  u[0] = (uint16_t)(0xd800 + (ch >> 10));
  u[1] = (uint16_t)(0xdc00 + (ch & 0x3ff));
  return 2;
}

/****************************************************************************
 * Name: sms_part
 *
 * Description:
 *   One SMS-SUBMIT PDU: the first octet, the message reference left to
 *   the modem, the destination, PID and DCS, no validity period, and the
 *   user data, with the header that joins a long message's parts.
 *
 ****************************************************************************/

static void sms_part(FAR struct sms_part_s *part, FAR const uint8_t *da,
                     size_t dalen, bool ucs2, bool report, uint8_t ref,
                     uint8_t total, uint8_t seq, FAR const uint8_t *body,
                     size_t blen)
{
  uint8_t pdu[1 + SMS_TPDU_MAX];
  size_t n = 0;
  size_t start;
  size_t udl;

  memset(pdu, 0, sizeof(pdu));
  pdu[n++] = 0x00;                /* The SMSC's address: the SIM's */
  pdu[n++] = (uint8_t)(SMS_MTI_SUBMIT | (report ? SMS_SRR : 0) |
                       (total > 1 ? SMS_UDHI : 0));
  pdu[n++] = 0x00;                /* TP-MR: the modem's */
  memcpy(pdu + n, da, dalen);
  n += dalen;
  pdu[n++] = 0x00;                /* TP-PID */
  pdu[n++] = ucs2 ? 0x08 : 0x00;  /* TP-DCS */

  start = n + 1;                  /* TP-UD, after TP-UDL */
  if (total > 1)
    {
      pdu[start + 0] = 0x05;      /* UDHL */
      pdu[start + 1] = 0x00;      /* Concatenated, 8-bit reference */
      pdu[start + 2] = 0x03;
      pdu[start + 3] = ref;
      pdu[start + 4] = total;
      pdu[start + 5] = seq;
    }

  if (ucs2)
    {
      memcpy(pdu + start + (total > 1 ? 6 : 0), body, blen);
      udl = blen + (total > 1 ? 6 : 0);
      pdu[n] = (uint8_t)udl;
      n = start + udl;
    }
  else
    {
      /* Septets, after the header's 7 when there is one: 48 bits, and a
       * fill bit
       */

      udl = blen + (total > 1 ? 7 : 0);
      pdu[n] = (uint8_t)udl;
      n = start + sms_pack(body, blen, pdu + start, total > 1 ? 49 : 0);
    }

  part->tpdu = (uint8_t)(n - 1);
  sms_put_hex(part->hex, pdu, n);
}

/****************************************************************************
 * Name: sms_next_part
 *
 * Description:
 *   The next part's characters from *pp, as many as room takes, a
 *   character never split: its septets, or its UTF-16BE octets, into
 *   body.  *pp moves past them.
 *
 * Returned Value:
 *   The part's septets or UTF-16 units.
 *
 ****************************************************************************/

static size_t sms_next_part(FAR const char **pp, bool ucs2, size_t room,
                            FAR uint8_t *body)
{
  FAR const char *p = *pp;
  uint16_t units[2];
  uint8_t code[2];
  size_t used = 0;
  size_t i;
  uint32_t ch;
  int n;
  int k;

  while ((n = sms_utf8_get(p, &ch)) > 0)
    {
      if (ucs2)
        {
          k = sms_utf16_units(ch, units);
          if (used + (size_t)k > room)
            {
              break;
            }

          for (i = 0; i < (size_t)k; i++)
            {
              body[2 * (used + i)]     = (uint8_t)(units[i] >> 8);
              body[2 * (used + i) + 1] = (uint8_t)units[i];
            }
        }
      else
        {
          k = sms_gsm7_code(ch, code);
          if (used + (size_t)k > room)
            {
              break;
            }

          memcpy(body + used, code, (size_t)k);
        }

      used += (size_t)k;
      p    += n;
    }

  *pp = p;
  return used;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sms_user_data(FAR const uint8_t *ud, size_t len, uint8_t udl,
                  uint8_t dcs, bool udhi, FAR struct sms_deliver_s *out)
{
  size_t header = 0;
  size_t octets;
  size_t i;
  int alphabet;

  out->binary = false;
  out->ref16  = false;
  out->ref    = 0;
  out->total  = 0;
  out->part   = 0;
  out->text[0] = '\0';

  alphabet = sms_alphabet(dcs);
  if (alphabet < 0)
    {
      return alphabet;
    }

  octets = alphabet == SMS_GSM7 ? ((size_t)udl * 7 + 7) / 8 : udl;
  if (octets > len)
    {
      return -EINVAL;
    }

  /* The header: a long message's reference, its parts, and this one */

  if (udhi)
    {
      if (octets < 1 || 1 + (size_t)ud[0] > octets)
        {
          return -EINVAL;
        }

      header = 1 + ud[0];
      for (i = 1; i + 1 < header; i += 2 + ud[i + 1])
        {
          if (i + 2 + ud[i + 1] > header)
            {
              return -EINVAL;
            }

          if (ud[i] == 0x00 && ud[i + 1] == 3)
            {
              out->ref   = ud[i + 2];
              out->total = ud[i + 3];
              out->part  = ud[i + 4];
            }
          else if (ud[i] == 0x08 && ud[i + 1] == 4)
            {
              out->ref16 = true;
              out->ref   = (uint16_t)((ud[i + 2] << 8) | ud[i + 3]);
              out->total = ud[i + 4];
              out->part  = ud[i + 5];
            }
        }

      if (out->total < 2 || out->part < 1 || out->part > out->total)
        {
          out->total = 0;           /* No part of anything */
          out->part  = 0;
        }
    }

  switch (alphabet)
    {
      case SMS_GSM7:

        /* The text begins at the septet after the header and its fill */

        sms_gsm7_text(ud, (header * 8 + 6) / 7, udl, out->text,
                      sizeof(out->text));
        break;

      case SMS_UCS2:
        sms_ucs2_text(ud + header, octets - header, out->text,
                      sizeof(out->text));
        break;

      default:
        out->binary = true;
        break;
    }

  return OK;
}

int sms_decode(FAR const char *hex, FAR struct sms_deliver_s *out,
               FAR uint8_t *mti)
{
  uint8_t pdu[12 + SMS_TPDU_MAX];
  size_t pos;
  uint8_t first;
  int n;
  int len;
  int i;
  int64_t local;
  int quarters;
  bool bcd = true;

  memset(out, 0, sizeof(*out));
  *mti = 0xff;
  len = sms_hex(hex, pdu, sizeof(pdu));
  if (len < 1)
    {
      return -EINVAL;
    }

  /* The SMSC's address, then the TPDU */

  pos = 1 + pdu[0];
  if (pos + 1 > (size_t)len)
    {
      return -EINVAL;
    }

  first = pdu[pos++];
  *mti  = first & SMS_MTI_MASK;
  if (*mti != SMS_MTI_DELIVER)
    {
      return -ENOMSG;
    }

  n = sms_address(pdu + pos, len - pos, out->sender, sizeof(out->sender));
  if (n < 0)
    {
      return n;
    }

  pos += (size_t)n;
  if (pos + 2 + 7 + 1 > (size_t)len)
    {
      return -EINVAL;
    }

  out->pid = pdu[pos++];
  out->dcs = pdu[pos++];

  /* The time stamp: local time, its zone in quarter hours, the sign in
   * the zone's first semi-octet.  One that is not BCD, or names no date,
   * is none: 0, in UTC.
   */

  for (i = 0; i < 7; i++)
    {
      if ((pdu[pos + i] & (i == 6 ? 0x07 : 0x0f)) > 9 ||
          (pdu[pos + i] >> 4) > 9)
        {
          bcd = false;
        }
    }

  quarters = (pdu[pos + 6] & 0x07) * 10 + (pdu[pos + 6] >> 4);
  local = bcd ? tel_utc(sms_bcd(pdu[pos]), sms_bcd(pdu[pos + 1]),
                        sms_bcd(pdu[pos + 2]), sms_bcd(pdu[pos + 3]),
                        sms_bcd(pdu[pos + 4]), sms_bcd(pdu[pos + 5])) : -1;
  if (local >= 0 && quarters <= 4 * 18)
    {
      out->offset = (int32_t)(quarters * 15 * 60);
      if (pdu[pos + 6] & 0x08)
        {
          out->offset = -out->offset;
        }

      out->sent = local - out->offset;
    }

  pos += 7;

  return sms_user_data(pdu + pos + 1, len - pos - 1, pdu[pos], out->dcs,
                       (first & SMS_UDHI) != 0, out);
}

int sms_parts(FAR const char *text, FAR bool *ucs2)
{
  uint8_t body[SMS_SEPTETS];
  FAR const char *p;
  size_t total = 0;
  size_t room;
  uint16_t units[2];
  uint8_t code[2];
  uint32_t ch;
  int count;
  int n;
  int k;

  *ucs2 = false;
  if (text[0] == '\0')
    {
      return -EINVAL;
    }

  /* GSM 7-bit if every character has a code there; how long in all */

  for (p = text; (n = sms_utf8_get(p, &ch)) > 0; p += n)
    {
      k = sms_gsm7_code(ch, code);
      *ucs2  = *ucs2 || k == 0;
      total += (size_t)k;
    }

  if (n < 0)
    {
      return -EINVAL;
    }

  if (*ucs2)
    {
      total = 0;
      for (p = text; (n = sms_utf8_get(p, &ch)) > 0; p += n)
        {
          total += (size_t)sms_utf16_units(ch, units);
        }
    }

  room = *ucs2 ? (total <= SMS_UNITS ? SMS_UNITS : SMS_UNITS_CONCAT) :
         (total <= SMS_SEPTETS ? SMS_SEPTETS : SMS_SEPTETS_CONCAT);

  for (count = 0, p = text; *p != '\0'; count++)
    {
      if (count == SMS_PARTS_MAX)
        {
          return -E2BIG;
        }

      sms_next_part(&p, *ucs2, room, body);
    }

  return count;
}

int sms_encode(FAR const char *number, FAR const char *text, uint8_t ref,
               bool report, uint8_t k, FAR struct sms_part_s *part)
{
  uint8_t da[2 + 10];
  uint8_t body[SMS_SEPTETS];        /* A part's septets, or its octets:
                                     * 160 and 140 at most */
  FAR const char *p;
  size_t ndigits = 0;
  size_t dalen;
  size_t room;
  size_t used;
  size_t i;
  bool international;
  bool ucs2;
  int count;

  memset(part, 0, sizeof(*part));

  /* The destination: digits, '+' first when international */

  international = number[0] == '+';
  p = number + (international ? 1 : 0);
  memset(da, 0, sizeof(da));
  for (i = 0; p[i] != '\0'; i++)
    {
      if (p[i] < '0' || p[i] > '9' || i == 20)
        {
          return -EINVAL;
        }

      da[2 + i / 2] |= (uint8_t)((p[i] - '0') << ((i & 1) ? 4 : 0));
      ndigits++;
    }

  if (ndigits == 0)
    {
      return -EINVAL;
    }

  if (ndigits & 1)
    {
      da[2 + ndigits / 2] |= 0xf0;
    }

  da[0] = (uint8_t)ndigits;
  da[1] = international ? 0x91 : 0x81;
  dalen = 2 + (ndigits + 1) / 2;

  count = sms_parts(text, &ucs2);
  if (count < 0)
    {
      return count;
    }

  if (k < 1 || k > count)
    {
      return -EINVAL;
    }

  /* Part k: the parts before it passed over, as sms_parts() split them */

  room = count == 1 ? (ucs2 ? SMS_UNITS : SMS_SEPTETS) :
         (ucs2 ? SMS_UNITS_CONCAT : SMS_SEPTETS_CONCAT);
  p = text;
  used = 0;
  for (i = 1; i <= k; i++)
    {
      used = sms_next_part(&p, ucs2, room, body);
    }

  sms_part(part, da, dalen, ucs2, report, ref, (uint8_t)count, k, body,
           ucs2 ? 2 * used : used);
  part->ucs2 = ucs2;
  return count;
}
