/****************************************************************************
 * pnut-os/src/telephony/mux.c
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

#include "mux.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MUX_FLAG        0xf9
#define MUX_EA          0x01
#define MUX_CR          0x02
#define MUX_PF          0x10

/* Frame types, without P/F */

#define MUX_SABM        0x2f
#define MUX_UA          0x63
#define MUX_DM          0x0f
#define MUX_DISC        0x43
#define MUX_UIH         0xef
#define MUX_UI          0x03

/* Control channel messages, without C/R */

#define MUX_MSC         0xe1        /* Modem status */
#define MUX_CLD         0xc1        /* Close down */
#define MUX_FCON        0xa1        /* Flow control on: send again */
#define MUX_FCOFF       0x61        /* Flow control off: wait */
#define MUX_NSC         0x11        /* Not understood */

#define MUX_V24_FC      0x02        /* In an MSC: wait */

/* A frame's bytes besides its information: flag, address, control,
 * length, FCS, flag
 */

#define MUX_OVERHEAD    6

/* What the FCS over the header and the FCS itself leaves */

#define MUX_FCS_GOOD    0xcf

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum mux_rx_e
{
  RX_FLAG = 0,                    /* Before a frame: text, or its flag */
  RX_ADDR,
  RX_CTRL,
  RX_LEN,
  RX_LEN2,
  RX_DATA,
  RX_FCS,
  RX_END,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mux_crc
 *
 * Description:
 *   27.010's FCS: CRC-8, polynomial x^8 + x^2 + x + 1, reflected.
 *
 ****************************************************************************/

static uint8_t mux_crc(uint8_t crc, FAR const uint8_t *p, size_t n)
{
  int i;

  while (n-- > 0)
    {
      crc ^= *p++;
      for (i = 0; i < 8; i++)
        {
          crc = crc & 1 ? (crc >> 1) ^ 0xe0 : crc >> 1;
        }
    }

  return crc;
}

/****************************************************************************
 * Name: mux_free
 *
 * Description:
 *   The queue's room, made at its end.
 *
 ****************************************************************************/

static size_t mux_free(FAR struct mux_s *mux)
{
  if (mux->off > 0)
    {
      memmove(mux->queue, mux->queue + mux->off, mux->len - mux->off);
      mux->len -= mux->off;
      mux->off  = 0;
    }

  return MUX_QUEUE - mux->len;
}

/****************************************************************************
 * Name: mux_put
 *
 * Description:
 *   Queue bytes; the caller has made sure of the room.
 *
 ****************************************************************************/

static void mux_put(FAR struct mux_s *mux, FAR const void *buf, size_t n)
{
  memcpy(mux->queue + mux->len, buf, n);
  mux->len += n;
}

/****************************************************************************
 * Name: mux_frame
 *
 * Description:
 *   Queue one frame, its information MUX_N1 bytes at most.
 *
 ****************************************************************************/

static int mux_frame(FAR struct mux_s *mux, uint8_t dlci, uint8_t ctrl,
                     bool command, FAR const void *info, size_t n)
{
  uint8_t hdr[4];
  uint8_t tail[2];

  if (mux_free(mux) < n + MUX_OVERHEAD)
    {
      return -EAGAIN;
    }

  hdr[0] = MUX_FLAG;
  hdr[1] = (dlci << 2) | (command ? MUX_CR : 0) | MUX_EA;
  hdr[2] = ctrl;
  hdr[3] = (n << 1) | MUX_EA;

  /* The FCS covers the address, control and length: of UIH frames, and
   * of the others, which carry no information here
   */

  tail[0] = 0xff - mux_crc(0xff, hdr + 1, 3);
  tail[1] = MUX_FLAG;

  mux_put(mux, hdr, sizeof(hdr));
  mux_put(mux, info, n);
  mux_put(mux, tail, sizeof(tail));
  return OK;
}

/****************************************************************************
 * Name: mux_control
 *
 * Description:
 *   Queue a control channel message.
 *
 ****************************************************************************/

static int mux_control(FAR struct mux_s *mux, uint8_t type, bool command,
                       FAR const uint8_t *value, size_t n)
{
  uint8_t msg[8];

  if (n > sizeof(msg) - 2)
    {
      n = sizeof(msg) - 2;
    }

  msg[0] = type | (command ? MUX_CR : 0) | MUX_EA;
  msg[1] = (n << 1) | MUX_EA;
  memcpy(msg + 2, value, n);
  return mux_frame(mux, MUX_CONTROL, MUX_UIH, true, msg, n + 2);
}

/****************************************************************************
 * Name: mux_tell
 ****************************************************************************/

static void mux_tell(FAR struct mux_s *mux, uint8_t dlci, int event)
{
  if (mux->ops->event != NULL)
    {
      mux->ops->event(mux->arg, dlci, event);
    }
}

/****************************************************************************
 * Name: mux_hold
 *
 * Description:
 *   The modem's flow control: wait, or send again.
 *
 ****************************************************************************/

static void mux_hold(FAR struct mux_s *mux, bool held)
{
  bool was = mux->held;

  mux->held = held;
  if (was && !held && mux->refused)
    {
      mux->refused = false;
      mux_tell(mux, MUX_CONTROL, MUX_ROOM);
    }
}

/****************************************************************************
 * Name: mux_on_control
 *
 * Description:
 *   A message on the control channel from the modem.  Its commands are
 *   answered in kind; its answers to ours are taken as they come.
 *
 ****************************************************************************/

static void mux_on_control(FAR struct mux_s *mux, FAR const uint8_t *p,
                           size_t n)
{
  uint8_t type;
  uint8_t nsc;
  size_t len;

  if (n < 2)
    {
      return;
    }

  type = p[0] & ~MUX_CR;
  len  = p[1] >> 1;
  if (len > n - 2)
    {
      len = n - 2;
    }

  if (!(p[0] & MUX_CR))
    {
      return;
    }

  switch (type)
    {
      case MUX_MSC:

        /* Its signals for a channel: flow control among them */

        if (len >= 2)
          {
            mux_hold(mux, (p[3] & MUX_V24_FC) != 0);
          }

        mux_control(mux, MUX_MSC, false, p + 2, len);
        break;

      case MUX_FCON:
      case MUX_FCOFF:
        mux_hold(mux, type == MUX_FCOFF);
        mux_control(mux, type, false, NULL, 0);
        break;

      case MUX_CLD:
        mux_control(mux, MUX_CLD, false, NULL, 0);
        mux->active = false;
        mux->opened = 0;
        mux_tell(mux, MUX_CONTROL, MUX_ENDED);
        break;

      default:

        /* Not understood: 27.010 5.4.6.3.8 */

        nsc = p[0];
        mux_control(mux, MUX_NSC, false, &nsc, 1);
        break;
    }
}

/****************************************************************************
 * Name: mux_on_frame
 *
 * Description:
 *   A whole frame has come.
 *
 ****************************************************************************/

static void mux_on_frame(FAR struct mux_s *mux)
{
  uint8_t dlci = mux->addr >> 2;
  uint8_t ctrl = mux->ctrl & ~MUX_PF;

  if (ctrl == MUX_UIH || ctrl == MUX_UI)
    {
      if (dlci == MUX_CONTROL)
        {
          mux_on_control(mux, mux->frame, mux->flen);
        }
      else if (mux->ops->data != NULL)
        {
          mux->ops->data(mux->arg, dlci, mux->frame, mux->flen);
        }
    }
  else if (ctrl == MUX_UA)
    {
      if (!(mux->opened & (1 << dlci)))
        {
          mux->opened |= 1 << dlci;
          mux_tell(mux, dlci, MUX_OPENED);
        }
    }
  else if (ctrl == MUX_DM || ctrl == MUX_DISC)
    {
      if (ctrl == MUX_DISC)
        {
          mux_frame(mux, dlci, MUX_UA | MUX_PF, false, NULL, 0);
        }

      mux->opened &= ~(1 << dlci);
      mux_tell(mux, dlci, MUX_CLOSED);
    }
}

/****************************************************************************
 * Name: mux_text
 *
 * Description:
 *   A byte read between frames: the modem's lines when it has restarted,
 *   in plain AT, of which "RDY" and "*ATREADY" tell.
 *
 ****************************************************************************/

static void mux_text(FAR struct mux_s *mux, uint8_t c)
{
  if (c == '\r' || c == '\n')
    {
      mux->text[mux->tlen] = '\0';
      if (strcmp(mux->text, "RDY") == 0 ||
          strncmp(mux->text, "*ATREADY", 8) == 0)
        {
          mux->tlen = 0;
          mux_tell(mux, MUX_CONTROL, MUX_RESTARTED);
          return;
        }

      mux->tlen = 0;
    }
  else if (c >= ' ' && c < 0x7f && mux->tlen < MUX_TEXT - 1)
    {
      mux->text[mux->tlen++] = c;
    }
  else
    {
      mux->tlen = 0;
    }
}

/****************************************************************************
 * Name: mux_goodaddr
 *
 * Description:
 *   Whether a byte can be a frame's address here: one octet, a channel of
 *   ours.  Anything else after a flag is text.
 *
 ****************************************************************************/

static bool mux_goodaddr(uint8_t c)
{
  return (c & MUX_EA) != 0 && (c >> 2) < MUX_DLCIS;
}

/****************************************************************************
 * Name: mux_goodctrl
 ****************************************************************************/

static bool mux_goodctrl(uint8_t c)
{
  c &= ~MUX_PF;
  return c == MUX_SABM || c == MUX_UA || c == MUX_DM || c == MUX_DISC ||
         c == MUX_UIH || c == MUX_UI;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void mux_init(FAR struct mux_s *mux, FAR const struct mux_ops_s *ops,
              FAR void *arg)
{
  memset(mux, 0, sizeof(*mux));
  mux->ops = ops;
  mux->arg = arg;
}

void mux_start(FAR struct mux_s *mux)
{
  mux->active  = true;
  mux->held    = false;
  mux->refused = false;
  mux->opened  = 0;
  mux->state   = RX_FLAG;
  mux->tlen    = 0;
}

void mux_reset(FAR struct mux_s *mux)
{
  mux->active  = false;
  mux->held    = false;
  mux->refused = false;
  mux->opened  = 0;
  mux->state   = RX_FLAG;
  mux->tlen    = 0;
  mux->off     = 0;
  mux->len     = 0;
}

int mux_open(FAR struct mux_s *mux, uint8_t dlci)
{
  if (!mux->active || dlci >= MUX_DLCIS)
    {
      return -ENOTCONN;
    }

  return mux_frame(mux, dlci, MUX_SABM | MUX_PF, true, NULL, 0);
}

int mux_signals(FAR struct mux_s *mux, uint8_t dlci, uint8_t signals)
{
  uint8_t v[2];

  if (!mux->active)
    {
      return -ENOTCONN;
    }

  v[0] = (dlci << 2) | MUX_CR | MUX_EA;
  v[1] = signals;
  return mux_control(mux, MUX_MSC, true, v, sizeof(v));
}

int mux_send(FAR struct mux_s *mux, uint8_t dlci, FAR const void *buf,
             size_t len)
{
  FAR const uint8_t *p = buf;
  size_t frames;
  size_t chunk;

  if (!mux->active)
    {
      if (dlci != MUX_AT)
        {
          return -ENOTCONN;
        }

      if (mux_free(mux) < len)
        {
          mux->refused = true;
          return -EAGAIN;
        }

      mux_put(mux, buf, len);
      return OK;
    }

  if (dlci == MUX_CONTROL || dlci >= MUX_DLCIS ||
      !(mux->opened & (1 << dlci)))
    {
      return -ENOTCONN;
    }

  frames = (len + MUX_N1 - 1) / MUX_N1;
  if (mux->held || mux_free(mux) < len + frames * MUX_OVERHEAD)
    {
      mux->refused = true;
      return -EAGAIN;
    }

  while (len > 0)
    {
      chunk = len < MUX_N1 ? len : MUX_N1;
      mux_frame(mux, dlci, MUX_UIH, true, p, chunk);
      p   += chunk;
      len -= chunk;
    }

  return OK;
}

size_t mux_room(FAR const struct mux_s *mux)
{
  size_t room = MUX_QUEUE - (mux->len - mux->off);
  size_t frames;

  if (!mux->active)
    {
      return room;
    }

  if (mux->held)
    {
      return 0;
    }

  frames = room / (MUX_N1 + MUX_OVERHEAD);
  room  -= frames * (MUX_N1 + MUX_OVERHEAD);
  return frames * MUX_N1 +
         (room > MUX_OVERHEAD ? room - MUX_OVERHEAD : 0);
}

int mux_close(FAR struct mux_s *mux)
{
  int ret;

  if (!mux->active)
    {
      return -ENOTCONN;
    }

  ret = mux_control(mux, MUX_CLD, true, NULL, 0);
  if (ret >= 0)
    {
      mux->active = false;
      mux->opened = 0;
      mux->state  = RX_FLAG;
    }

  return ret;
}

int mux_kick(FAR struct mux_s *mux)
{
  if (mux_free(mux) < 2 + 2 + MUX_OVERHEAD + 1)
    {
      return -EAGAIN;
    }

  mux_control(mux, MUX_CLD, true, NULL, 0);
  mux_put(mux, "\r", 1);
  mux->active = false;
  mux->opened = 0;
  mux->state  = RX_FLAG;
  return OK;
}

void mux_input(FAR struct mux_s *mux, FAR const uint8_t *buf, size_t len)
{
  uint8_t c;
  size_t i;

  for (i = 0; i < len && mux->active; i++)
    {
      c = buf[i];
      switch (mux->state)
        {
          case RX_FLAG:
            if (c == MUX_FLAG)
              {
                mux->tlen  = 0;
                mux->state = RX_ADDR;
              }
            else
              {
                mux_text(mux, c);
              }
            break;

          case RX_ADDR:
            if (c == MUX_FLAG)
              {
                break;                  /* Flags between frames */
              }

            if (!mux_goodaddr(c))
              {
                mux->state = RX_FLAG;
                mux_text(mux, c);
                break;
              }

            mux->addr  = c;
            mux->fcs   = mux_crc(0xff, &c, 1);
            mux->state = RX_CTRL;
            break;

          case RX_CTRL:
            if (!mux_goodctrl(c))
              {
                mux->bad++;
                mux->state = RX_FLAG;
                mux_text(mux, c);
                break;
              }

            mux->ctrl  = c;
            mux->fcs   = mux_crc(mux->fcs, &c, 1);
            mux->state = RX_LEN;
            break;

          case RX_LEN:
            mux->fcs   = mux_crc(mux->fcs, &c, 1);
            mux->flen  = c >> 1;
            mux->got   = 0;
            mux->state = (c & MUX_EA) ? (mux->flen ? RX_DATA : RX_FCS) :
                         RX_LEN2;
            break;

          case RX_LEN2:
            mux->fcs   = mux_crc(mux->fcs, &c, 1);
            mux->flen |= (uint16_t)c << 7;
            mux->state = mux->flen ? RX_DATA : RX_FCS;
            break;

          case RX_DATA:
            if (mux->got < MUX_RX_MAX)
              {
                mux->frame[mux->got] = c;
              }

            if (++mux->got >= mux->flen)
              {
                mux->state = RX_FCS;
              }
            break;

          case RX_FCS:
            if (mux_crc(mux->fcs, &c, 1) == MUX_FCS_GOOD &&
                mux->flen <= MUX_RX_MAX)
              {
                mux->state = RX_END;
              }
            else
              {
                mux->bad++;
                mux->state = RX_FLAG;
              }
            break;

          case RX_END:
            if (c == MUX_FLAG)
              {
                mux->state = RX_ADDR;   /* This flag may open the next */
                mux_on_frame(mux);
              }
            else
              {
                mux->bad++;
                mux->state = RX_FLAG;
                mux_text(mux, c);
              }
            break;
        }
    }
}

int mux_flush(FAR struct mux_s *mux)
{
  ssize_t n;

  while (mux->off < mux->len)
    {
      n = mux->ops->write(mux->arg, mux->queue + mux->off,
                          mux->len - mux->off);
      if (n < 0)
        {
          return n;
        }

      if (n == 0)
        {
          return -EAGAIN;
        }

      mux->off += n;
    }

  mux->off = 0;
  mux->len = 0;

  /* Room again, after a send was refused */

  if (mux->refused && !mux->held)
    {
      mux->refused = false;
      mux_tell(mux, MUX_CONTROL, MUX_ROOM);
    }

  return OK;
}

bool mux_waiting(FAR const struct mux_s *mux)
{
  return mux->off < mux->len;
}
