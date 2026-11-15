/****************************************************************************
 * pnut-os/src/commands/keys.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* keys: keys pressed from the shell, for developers (RFC 0027, Testing).
 * They are written to the keyboard's device as its driver reports keys,
 * and Input reads them as the driver's; the function keys too (F1 to F3
 * from any keyboard are the function keys).
 *
 *   keys [-d <device>] <key>...
 *
 * A key: one character (w, q, 1...); enter, bs, space, find, f1, f2, f3;
 * a chord, alt+<key>, shift+<key> or find+<key>; or "text <word>", which
 * types the word's characters.  The default device is /dev/kbd0.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nuttx/input/kbd_codec.h>
#include <nuttx/input/keyboard.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define KEYS_DEVICE  "/dev/kbd0"
#define KEYS_PAUSE   20000        /* Between keys, in us */

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The keys named, and their codes */

static FAR const char * const g_keys_names[] =
{
  "enter", "bs", "find", "f1", "f2", "f3"
};

static const uint32_t g_keys_codes[] =
{
  KEYCODE_ENTER, KEYCODE_BACKDEL, KEYCODE_FIND, KEYCODE_F1, KEYCODE_F2,
  KEYCODE_F3
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void keys_add(FAR struct keyboard_event_s *ev, FAR int *n,
                     uint32_t type, uint32_t code)
{
  ev[*n].type = type;
  ev[*n].code = code;
  (*n)++;
}

/* A key, without its chord: its press, and with release its release */

static int keys_one(FAR const char *key, FAR struct keyboard_event_s *ev,
                    FAR int *n, bool release)
{
  size_t i;

  if (strcmp(key, "space") == 0)
    {
      keys_add(ev, n, release ? KEYBOARD_RELEASE : KEYBOARD_PRESS, ' ');
      return 0;
    }

  for (i = 0; i < sizeof(g_keys_names) / sizeof(g_keys_names[0]); i++)
    {
      if (strcmp(key, g_keys_names[i]) == 0)
        {
          keys_add(ev, n, release ? KEYBOARD_SPECREL : KEYBOARD_SPECPRESS,
                   g_keys_codes[i]);
          return 0;
        }
    }

  if (strlen(key) == 1 && (unsigned char)key[0] >= 0x20)
    {
      keys_add(ev, n, release ? KEYBOARD_RELEASE : KEYBOARD_PRESS,
               (unsigned char)key[0]);
      return 0;
    }

  return -EINVAL;
}

static int keys_write(int fd, FAR const struct keyboard_event_s *ev, int n)
{
  ssize_t len = n * sizeof(ev[0]);

  if (write(fd, ev, len) != len)
    {
      return -errno;
    }

  usleep(KEYS_PAUSE);
  return 0;
}

/* A key or a chord, pressed and released */

static int keys_press(int fd, FAR const char *arg)
{
  struct keyboard_event_s ev[4];
  FAR const char *plus = strchr(arg, '+');
  uint32_t mod = 0;
  int n = 0;

  if (plus != NULL && plus != arg && plus[1] != '\0')
    {
      if (strncmp(arg, "alt+", 4) == 0)
        {
          mod = KEYCODE_LALT;
        }
      else if (strncmp(arg, "shift+", 6) == 0)
        {
          mod = KEYCODE_LSHIFT;
        }
      else if (strncmp(arg, "find+", 5) == 0)
        {
          mod = KEYCODE_FIND;
        }
      else
        {
          return -EINVAL;
        }

      arg = plus + 1;
      keys_add(ev, &n, KEYBOARD_SPECPRESS, mod);
    }

  if (keys_one(arg, ev, &n, false) < 0 || keys_one(arg, ev, &n, true) < 0)
    {
      return -EINVAL;
    }

  if (mod != 0)
    {
      keys_add(ev, &n, KEYBOARD_SPECREL, mod);
    }

  return keys_write(fd, ev, n);
}

static int keys_text(int fd, FAR const char *text)
{
  struct keyboard_event_s ev[2];
  int ret = 0;
  int n;

  for (; *text != '\0' && ret == 0; text++)
    {
      n = 0;
      keys_add(ev, &n, KEYBOARD_PRESS, (unsigned char)*text);
      keys_add(ev, &n, KEYBOARD_RELEASE, (unsigned char)*text);
      ret = keys_write(fd, ev, n);
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *device = KEYS_DEVICE;
  int ret = 0;
  int fd;
  int i = 1;

  if (argc > 2 && strcmp(argv[1], "-d") == 0)
    {
      device = argv[2];
      i = 3;
    }

  if (i >= argc)
    {
      fprintf(stderr, "usage: keys [-d <device>] <key>...\n"
              "  a character; enter bs space find f1 f2 f3;\n"
              "  alt+<key> shift+<key> find+<key>; text <word>\n");
      return EXIT_FAILURE;
    }

  fd = open(device, O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    {
      fprintf(stderr, "keys: %s: %d\n", device, -errno);
      return EXIT_FAILURE;
    }

  for (; i < argc && ret == 0; i++)
    {
      if (strcmp(argv[i], "text") == 0 && i + 1 < argc)
        {
          ret = keys_text(fd, argv[++i]);
        }
      else
        {
          ret = keys_press(fd, argv[i]);
        }

      if (ret == -EINVAL)
        {
          fprintf(stderr, "keys: not a key: %s\n", argv[i]);
        }
      else if (ret < 0)
        {
          fprintf(stderr, "keys: %s: %d\n", device, ret);
        }
    }

  close(fd);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
