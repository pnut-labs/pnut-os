/****************************************************************************
 * pnut-os/src/radios/device.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Radio's LoRa radio on the device: NuttX's SX126x driver, which listens
 * in the background (SX126XIOC_LISTENSET) and keeps the packets heard for
 * read(); its antenna line, a GPIO.
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

#include <nuttx/ioexpander/gpio.h>
#include <nuttx/wireless/ioctl.h>
#include <nuttx/wireless/lpwan/sx126x.h>

#include "packetradio.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_sx126x_fd = -1;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int sx126x_ioctl(int cmd, FAR void *arg)
{
  return ioctl(g_sx126x_fd, cmd, (unsigned long)(uintptr_t)arg) < 0 ?
         -errno : OK;
}

static int sx126x_open_dev(FAR void *arg, FAR const char *path)
{
  g_sx126x_fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  return g_sx126x_fd < 0 ? -errno : g_sx126x_fd;
}

static void sx126x_close_dev(FAR void *arg)
{
  if (g_sx126x_fd >= 0)
    {
      close(g_sx126x_fd);
      g_sx126x_fd = -1;
    }
}

/* Configure, then listen as asked.  Listening goes on meanwhile, the
 * driver taking each change at once: stopping would drop the packets it
 * keeps.  The chip's sync word register takes a one byte word 0xXY as
 * 0xX4Y4.
 */

static int sx126x_setup(FAR void *arg, FAR const struct radio_lora_s *lora,
                        uint8_t listen, uint32_t listen_us,
                        uint32_t sleep_us)
{
  struct sx126x_lora_config_s config;
  struct sx126x_listen_s how;
  uint16_t sync;
  uint32_t freq;
  int8_t power;
  int ret;

  memset(&config, 0, sizeof(config));
  config.modulation.spreading_factor          = lora->sf;
  config.modulation.bandwidth                 = lora->bw_code;
  config.modulation.coding_rate               = lora->cr - 4;
  config.modulation.low_datarate_optimization = lora->ldro;
  config.packet.preambles                     = lora->preamble;
  config.packet.fixed_length_header           = false;
  config.packet.payload_length                = RADIO_PAYLOAD;
  config.packet.crc_enable                    = lora->crc;
  config.packet.invert_iq                     = lora->invert_iq;

  sync  = ((lora->sync & 0xf0) << 8) | ((lora->sync & 0x0f) << 4) | 0x0404;
  freq  = lora->freq_hz;
  power = lora->power_dbm;

  ret = sx126x_ioctl(SX126XIOC_LORACONFIGSET, &config);
  if (ret >= 0)
    {
      ret = sx126x_ioctl(SX126XIOC_SYNCWORDSET, &sync);
    }

  if (ret >= 0)
    {
      ret = sx126x_ioctl(WLIOC_SETRADIOFREQ, &freq);
    }

  if (ret >= 0)
    {
      ret = sx126x_ioctl(WLIOC_SETTXPOWER, &power);
    }

  if (ret < 0)
    {
      return ret;
    }

  memset(&how, 0, sizeof(how));
  how.mode     = listen == PNUT_RADIO_LISTEN_RADIO_LISTEN_DUTY_CYCLE ?
                 SX126X_LISTEN_DUTY_CYCLE : SX126X_LISTEN_CONTINUOUS;
  how.rx_us    = listen_us;
  how.sleep_us = sleep_us;
  return sx126x_ioctl(SX126XIOC_LISTENSET, &how);
}

/* The driver's SNR is in quarters of a dB */

static int sx126x_receive(FAR void *arg, FAR struct radio_heard_s *heard)
{
  struct sx126x_read_header_s header;
  ssize_t n;

  n = read(g_sx126x_fd, &header, sizeof(header));
  if (n < 0)
    {
      /* Nothing heard yet; -ECANCELED, listening stopped: the chip
       * failed to listen again after a change or a send
       */

      return errno == EAGAIN ? 0 : -errno;
    }

  if (n != sizeof(header))
    {
      return -EIO;
    }

  memset(heard, 0, sizeof(*heard));
  heard->len       = header.payload_length;
  heard->rssi_dbm  = header.rssi_db;
  heard->snr_cdb   = (int16_t)(header.snr * 25);
  heard->crc_error = header.crc_error != 0;
  memcpy(heard->data, header.payload, header.payload_length);
  return 1;
}

static int sx126x_send(FAR void *arg, FAR const uint8_t *data, size_t len)
{
  ssize_t n;

  n = write(g_sx126x_fd, data, len);
  if (n < 0)
    {
      return -errno;
    }

  return (size_t)n == len ? OK : -EIO;
}

static int sx126x_antenna(FAR void *arg, FAR const char *line,
                          bool internal)
{
  int ret = OK;
  int fd;

  fd = open(line, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    {
      return -errno;
    }

  if (ioctl(fd, GPIOC_WRITE, (unsigned long)internal) < 0)
    {
      ret = -errno;
    }

  close(fd);
  return ret;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct radio_dev_ops_s g_radio_sx126x =
{
  sx126x_open_dev,
  sx126x_close_dev,
  sx126x_setup,
  sx126x_receive,
  sx126x_send,
  sx126x_antenna
};
