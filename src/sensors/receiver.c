/****************************************************************************
 * pnut-os/src/sensors/receiver.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Location's receiver on the device: NuttX's GNSS driver's raw file.
 * Opening it powers the receiver; what is written goes to its UART; what
 * it says is read back, NMEA and UBX alike.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "location.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_gnss_fd = -1;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int gnss_open(FAR void *arg, FAR const char *path)
{
  g_gnss_fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  return g_gnss_fd < 0 ? -errno : g_gnss_fd;
}

static void gnss_close(FAR void *arg)
{
  if (g_gnss_fd >= 0)
    {
      close(g_gnss_fd);
      g_gnss_fd = -1;
    }
}

static ssize_t gnss_read(FAR void *arg, FAR uint8_t *buf, size_t len)
{
  ssize_t n = read(g_gnss_fd, buf, len);

  if (n < 0)
    {
      return errno == EAGAIN ? 0 : -errno;
    }

  return n;
}

static ssize_t gnss_write(FAR void *arg, FAR const uint8_t *buf,
                          size_t len)
{
  ssize_t n = write(g_gnss_fd, buf, len);

  if (n < 0)
    {
      return errno == EAGAIN ? 0 : -errno;
    }

  return n;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct location_dev_ops_s g_location_gnss =
{
  gnss_open,
  gnss_close,
  gnss_read,
  gnss_write
};
