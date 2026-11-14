/****************************************************************************
 * pnut-os/src/media/motor.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Haptics' motor on the device: NuttX's force-feedback driver
 * (/dev/input_ffN).  Two effects are kept on it, each uploaded once and
 * updated after: a level held for a time (FF_CONSTANT), and a sequence of
 * the chip's library effects (FF_PERIODIC, FF_CUSTOM, as the DRV2605's
 * driver takes them).  A write of the effect's id starts it, or stops it.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/input/ff.h>

#include "haptics.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct motor_ff_s
{
  int fd;
  int16_t level_id;               /* -1 until uploaded */
  int16_t sequence_id;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct motor_ff_s g_motor =
{
  -1, -1, -1
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int motor_play(int fd, int16_t id, int value)
{
  struct ff_event_s event;
  ssize_t n;

  event.code  = id;
  event.value = value;
  n = write(fd, &event, sizeof(event));
  if (n < 0)
    {
      return -errno;
    }

  return n == sizeof(event) ? OK : -EIO;   /* A short write: none expected */
}

static int motor_upload(FAR struct motor_ff_s *m, FAR struct ff_effect *e,
                        FAR int16_t *id)
{
  e->id = *id;
  if (ioctl(m->fd, EVIOCSFF, (unsigned long)(uintptr_t)e) < 0)
    {
      return -errno;
    }

  *id = e->id;
  return motor_play(m->fd, *id, 1);
}

static int motor_open(FAR void *arg, FAR const char *path)
{
  FAR struct motor_ff_s *m = &g_motor;

  m->fd = open(path, O_RDWR | O_CLOEXEC);
  m->level_id    = -1;
  m->sequence_id = -1;
  return m->fd < 0 ? -errno : OK;
}

/* The effects removed before the device is closed.  The driver's close
 * erases a file's own too (ff_upper), but not if the closing thread is
 * cancelled, and the device has few
 */

static void motor_close(FAR void *arg)
{
  FAR struct motor_ff_s *m = &g_motor;

  if (m->fd < 0)
    {
      return;
    }

  if (m->level_id >= 0)
    {
      ioctl(m->fd, EVIOCRMFF, (unsigned long)m->level_id);
      m->level_id = -1;
    }

  if (m->sequence_id >= 0)
    {
      ioctl(m->fd, EVIOCRMFF, (unsigned long)m->sequence_id);
      m->sequence_id = -1;
    }

  close(m->fd);
  m->fd = -1;
}

static int motor_level(FAR void *arg, uint8_t level, uint32_t ms)
{
  FAR struct motor_ff_s *m = &g_motor;
  struct ff_effect e;

  memset(&e, 0, sizeof(e));
  e.type               = FF_CONSTANT;
  e.replay.length      = ms > UINT16_MAX ? UINT16_MAX : (uint16_t)ms;
  e.u.constant.level   = (int16_t)((int32_t)level * 0x7fff / 100);
  return motor_upload(m, &e, &m->level_id);
}

static int motor_sequence(FAR void *arg, FAR const uint8_t *effects,
                          size_t count)
{
  FAR struct motor_ff_s *m = &g_motor;
  int16_t data[HAPTICS_SEQUENCE];
  struct ff_effect e;
  size_t i;

  if (count == 0 || count > HAPTICS_SEQUENCE)
    {
      return -EINVAL;
    }

  for (i = 0; i < count; i++)
    {
      data[i] = effects[i];
    }

  memset(&e, 0, sizeof(e));
  e.type                    = FF_PERIODIC;
  e.u.periodic.waveform     = FF_CUSTOM;
  e.u.periodic.custom_len   = count;
  e.u.periodic.custom_data  = data;
  return motor_upload(m, &e, &m->sequence_id);
}

static int motor_stop(FAR void *arg)
{
  FAR struct motor_ff_s *m = &g_motor;
  int ret = OK;

  if (m->level_id >= 0)
    {
      ret = motor_play(m->fd, m->level_id, 0);
    }

  if (m->sequence_id >= 0)
    {
      int r = motor_play(m->fd, m->sequence_id, 0);

      ret = ret < 0 ? ret : r;
    }

  return ret;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct haptics_motor_ops_s g_haptics_ff =
{
  motor_open,
  motor_close,
  motor_level,
  motor_sequence,
  motor_stop
};
