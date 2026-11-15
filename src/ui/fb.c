/****************************************************************************
 * pnut-os/src/ui/fb.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The screen on the device: a NuttX framebuffer, 1 bit, as Board's
 * description names it.  Drawing goes into its memory; FBIO_UPDATE sends
 * an area to the glass.  On the UC8253's e-paper a refresh is partial
 * unless UC8253_IOC_FULLREFRESH asks for a full one, which clears ghosting.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <nuttx/video/fb.h>
#ifdef CONFIG_LCD_UC8253
#  include <nuttx/lcd/uc8253.h>
#endif

#include "display.h"
#include "fb.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int fb_update(FAR void *arg, int16_t x, int16_t y, int16_t w,
                     int16_t h, bool full)
{
  int fd = (int)(intptr_t)arg;
  struct fb_area_s area;

#ifdef CONFIG_LCD_UC8253
  if (full && ioctl(fd, UC8253_IOC_FULLREFRESH, 0) < 0)
    {
      return -errno;
    }
#endif

  area.x = x;
  area.y = y;
  area.w = w;
  area.h = h;
  return ioctl(fd, FBIO_UPDATE, (unsigned long)&area) < 0 ? -errno : 0;
}

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct display_ops_s g_fb_ops =
{
  fb_update
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int fb_open(FAR const char *path, FAR struct display_config_s *config)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  FAR void *mem;
  int ret;
  int fd;

  fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    {
      return -errno;
    }

  if (ioctl(fd, FBIOGET_VIDEOINFO, (unsigned long)&vinfo) < 0 ||
      ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)&pinfo) < 0)
    {
      ret = -errno;
      goto errout;
    }

  if (vinfo.fmt != FB_FMT_Y1 || pinfo.bpp != 1 || vinfo.xres % 8 != 0)
    {
      ret = -ENOTSUP;
      goto errout;
    }

  mem = mmap(NULL, pinfo.fblen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mem == MAP_FAILED)
    {
      ret = -errno;
      goto errout;
    }

  config->frame  = mem;
  config->stride = pinfo.stride;
  config->width  = vinfo.xres;
  config->height = vinfo.yres;
  config->ops    = &g_fb_ops;
  config->arg    = (FAR void *)(intptr_t)fd;
  return fd;

errout:
  close(fd);
  return ret;
}
