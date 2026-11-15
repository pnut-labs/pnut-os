/****************************************************************************
 * pnut-os/src/commands/screen.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* screen: what the screen shows, from the shell, for developers: the
 * framebuffer's frame as a 1-bit PBM picture (P4), in base64 between two
 * lines, for a computer to read from the console.  What the system UI has
 * drawn last; on e-paper, the glass catches up within a refresh.
 *
 *   screen [<device>]          /dev/fb0 by default
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <nuttx/video/fb.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SCREEN_DEVICE  "/dev/fb0"
#define SCREEN_LINE    57         /* Bytes a line: 76 base64 characters */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char g_screen_b64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Base64 of up to SCREEN_LINE bytes, one line */

static void screen_line(FAR const uint8_t *in, size_t n)
{
  char out[SCREEN_LINE / 3 * 4 + 2];
  size_t i;
  size_t o = 0;
  uint32_t v;

  for (i = 0; i < n; i += 3)
    {
      v = (uint32_t)in[i] << 16;
      v |= i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0;
      v |= i + 2 < n ? in[i + 2] : 0;
      out[o++] = g_screen_b64[(v >> 18) & 63];
      out[o++] = g_screen_b64[(v >> 12) & 63];
      out[o++] = i + 1 < n ? g_screen_b64[(v >> 6) & 63] : '=';
      out[o++] = i + 2 < n ? g_screen_b64[v & 63] : '=';
    }

  out[o++] = '\n';
  fwrite(out, 1, o, stdout);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *device = argc > 1 ? argv[1] : SCREEN_DEVICE;
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  FAR uint8_t *pic;
  FAR uint8_t *fb;
  size_t row;
  size_t len;
  size_t i;
  int head;
  int fd;
  int x;
  int y;

  fd = open(device, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      fprintf(stderr, "screen: %s: %d\n", device, -errno);
      return EXIT_FAILURE;
    }

  if (ioctl(fd, FBIOGET_VIDEOINFO, (unsigned long)&vinfo) < 0 ||
      ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)&pinfo) < 0 ||
      vinfo.fmt != FB_FMT_Y1 || pinfo.bpp != 1)
    {
      fprintf(stderr, "screen: %s is not a 1-bit framebuffer\n", device);
      close(fd);
      return EXIT_FAILURE;
    }

  fb = mmap(NULL, pinfo.fblen, PROT_READ, MAP_SHARED, fd, 0);
  if (fb == MAP_FAILED)
    {
      fprintf(stderr, "screen: mmap: %d\n", -errno);
      close(fd);
      return EXIT_FAILURE;
    }

  /* PBM: rows of whole bytes, a set bit black (the panel's is white) */

  row = (vinfo.xres + 7) / 8;
  pic = malloc(32 + row * vinfo.yres);
  if (pic == NULL)
    {
      fprintf(stderr, "screen: no memory\n");
      close(fd);
      return EXIT_FAILURE;
    }

  head = snprintf((FAR char *)pic, 32, "P4\n%d %d\n", vinfo.xres,
                  vinfo.yres);
  for (y = 0; y < vinfo.yres; y++)
    {
      for (x = 0; x < (int)row; x++)
        {
          pic[head + y * row + x] = ~fb[y * pinfo.stride + x];
        }
    }

  close(fd);

  len = head + row * vinfo.yres;
  printf("-----BEGIN SCREEN-----\n");
  for (i = 0; i < len; i += SCREEN_LINE)
    {
      screen_line(pic + i, len - i < SCREEN_LINE ? len - i : SCREEN_LINE);
    }

  printf("-----END SCREEN-----\n");
  free(pic);
  return EXIT_SUCCESS;
}
