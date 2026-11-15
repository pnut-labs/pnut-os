/****************************************************************************
 * pnut-os/src/ui/input_dev.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Input's devices on the device: NuttX's keyboard and touchscreen upper
 * halves, read on the loop.  A keyboard gives characters as printed and
 * special keys (RFC 0024); the function keys are a keyboard of F1 to F3,
 * and F1 to F3 from any keyboard are the function keys.  Events written to
 * a device by a test reach Input as the driver's do (RFC 0027).
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <nuttx/input/kbd_codec.h>
#include <nuttx/input/keyboard.h>
#include <nuttx/input/touchscreen.h>

#include "input.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Input reads keyboard events, not the byte stream */

#ifdef CONFIG_INPUT_KEYBOARD_BYTESTREAM
#  error "Input needs keyboard events: CONFIG_INPUT_KEYBOARD_BYTESTREAM off"
#endif

/* The most points of a touch sample (the CST3530's five, with room) */

#define INPUT_TOUCH_POINTS  10

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* NuttX's special keys that mean something to the system UI */

static const uint32_t g_input_specials[][2] =
{
  { KEYCODE_ENTER,   KEYMAP_ENTER },
  { KEYCODE_BACKDEL, KEYMAP_BACKSPACE },
  { KEYCODE_FIND,    KEYMAP_FIND },
  { KEYCODE_LSHIFT,  KEYMAP_SHIFT },
  { KEYCODE_RSHIFT,  KEYMAP_SHIFT },
  { KEYCODE_LALT,    KEYMAP_ALT },
  { KEYCODE_RALT,    KEYMAP_ALT },
  { KEYCODE_F1,      KEYMAP_F1 },
  { KEYCODE_F2,      KEYMAP_F2 },
  { KEYCODE_F3,      KEYMAP_F3 },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void input_dev_keys(FAR struct input_s *in, int fd)
{
  struct keyboard_event_s ev[8];
  ssize_t n;
  size_t i;
  size_t j;

  n = read(fd, ev, sizeof(ev));
  for (i = 0; n > 0 && i < (size_t)n / sizeof(ev[0]); i++)
    {
      switch (ev[i].type)
        {
          case KEYBOARD_PRESS:
          case KEYBOARD_RELEASE:
            input_key(in, ev[i].code, false,
                      ev[i].type == KEYBOARD_PRESS);
            break;

          case KEYBOARD_SPECPRESS:
          case KEYBOARD_SPECREL:
            for (j = 0; j < sizeof(g_input_specials) /
                            sizeof(g_input_specials[0]); j++)
              {
                if (g_input_specials[j][0] == ev[i].code)
                  {
                    input_key(in, g_input_specials[j][1], true,
                              ev[i].type == KEYBOARD_SPECPRESS);
                    break;
                  }
              }
            break;
        }
    }
}

/* The touch screen's samples, read whole: a sample is as long as its
 * points (SIZEOF_TOUCH_SAMPLE_S), the driver's of up to five, and the
 * upper half hands out bytes.  Its count is read first, then the rest; a
 * count out of range is a sample cut by the queue overflowing, and what
 * is queued is dropped.
 */

static void input_dev_touch(FAR struct input_s *in, int fd)
{
  union
  {
    struct touch_sample_s sample;
    uint8_t bytes[SIZEOF_TOUCH_SAMPLE_S(INPUT_TOUCH_POINTS)];
  } u;

  FAR struct touch_point_s *p;
  size_t rest;
  int npoints;
  int i;

  for (; ; )
    {
      if (read(fd, &npoints, sizeof(npoints)) != sizeof(npoints))
        {
          return;
        }

      if (npoints < 1 || npoints > INPUT_TOUCH_POINTS)
        {
          while (read(fd, u.bytes, sizeof(u.bytes)) > 0);

          /* The followed finger's lift may have been cut: lift it */

          if (in->finger >= 0)
            {
              input_touch_point(in, in->finger, false, false, 0, 0);
            }

          return;
        }

      rest = SIZEOF_TOUCH_SAMPLE_S(npoints) - sizeof(npoints);
      if (read(fd, u.bytes + sizeof(npoints), rest) != (ssize_t)rest)
        {
          return;
        }

      for (i = 0; i < npoints; i++)
        {
          p = &u.sample.point[i];
          if ((p->flags & (TOUCH_DOWN | TOUCH_MOVE | TOUCH_UP)) != 0)
            {
              input_touch_point(in, p->id, (p->flags & TOUCH_UP) == 0,
                                (p->flags & TOUCH_POS_VALID) != 0,
                                p->x, p->y);
            }
        }
    }
}

static void input_dev_ready(FAR struct pnut_loop_s *loop, int fd,
                            uint32_t events, FAR void *arg)
{
  FAR struct input_s *in = arg;
  uint8_t i;

  for (i = 0; i < in->ndevs; i++)
    {
      if (in->devs[i].fd == fd)
        {
          if (in->devs[i].touch)
            {
              input_dev_touch(in, fd);
            }
          else
            {
              input_dev_keys(in, fd);
            }

          return;
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int input_dev_open(FAR struct input_s *in, FAR const char *path,
                   bool touch)
{
  int ret;
  int fd;

  if (in->loop == NULL)
    {
      return -EAGAIN;
    }

  if (in->ndevs >= INPUT_DEVICES)
    {
      return -ENOSPC;
    }

  fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0)
    {
      return -errno;
    }

  ret = pnut_loop_watch(in->loop, fd, EPOLLIN, input_dev_ready, in);
  if (ret < 0)
    {
      close(fd);
      return ret;
    }

  in->devs[in->ndevs].fd    = fd;
  in->devs[in->ndevs].touch = touch;
  in->ndevs++;
  return 0;
}
