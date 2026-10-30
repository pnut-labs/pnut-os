/****************************************************************************
 * pnut-os/src/telephony/main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <nuttx/ioexpander/gpio.h>

#include <pnut/loop.h>
#include <pnut/module.h>

#include "telephony.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Board's service and Service states', in the system program */

#define TELEPHONY_BOARD     "board"
#define TELEPHONY_SERVICES  "services"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The modem's UART: not blocking, raw where the serial driver takes
 * termios; its speed is the board's, 115200 baud on the T-Deck Max
 */

static int telephony_open_uart(FAR void *arg, FAR const char *path)
{
  int fd;
#ifdef CONFIG_SERIAL_TERMIOS
  struct termios tio;
#endif

  fd = open(path, O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
  if (fd < 0)
    {
      return -errno;
    }

#ifdef CONFIG_SERIAL_TERMIOS
  if (tcgetattr(fd, &tio) == 0)
    {
      cfmakeraw(&tio);
      cfsetspeed(&tio, B115200);
      tcsetattr(fd, TCSANOW, &tio);
    }
#endif

  return fd;
}

/* The modem's lines: GPIO pins, set and read with the GPIO driver's
 * commands
 */

static int telephony_open_line(FAR void *arg, FAR const char *path)
{
  int fd = open(path, O_RDWR | O_CLOEXEC);

  return fd < 0 ? -errno : fd;
}

static int telephony_set_line(FAR void *arg, int fd, bool high)
{
  return ioctl(fd, GPIOC_WRITE, (unsigned long)high) < 0 ? -errno : 0;
}

static int telephony_get_line(FAR void *arg, int fd, FAR bool *high)
{
  return ioctl(fd, GPIOC_READ, (unsigned long)((uintptr_t)high)) < 0 ?
         -errno : 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  struct telephony_config_s tconfig;
  FAR struct telephony_s *tel;
  FAR struct pnut_loop_s *loop;
  int ret;

  tel = calloc(1, sizeof(*tel));
  if (tel == NULL)
    {
      fprintf(stderr, "pnut_telephony: no memory\n");
      return EXIT_FAILURE;
    }

  pnut_loop_defaults(&config);
  config.name   = "telephony";
  config.levels = true;

  /* Asked to stop, the modem is powered down in order first: AT+CPOF, up
   * to 10 s for it to stop answering, 2 s more, then the rail.  NxInit's
   * gentle kill time is the ceiling.
   */

  config.finish = CONFIG_PNUT_TELEPHONY_FINISH;

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "pnut_telephony: no loop: %d\n", ret);
      free(tel);
      return EXIT_FAILURE;
    }

  memset(&tconfig, 0, sizeof(tconfig));
  tconfig.board     = TELEPHONY_BOARD;
  tconfig.services  = TELEPHONY_SERVICES;
  tconfig.open_uart = telephony_open_uart;
  tconfig.open_line = telephony_open_line;
  tconfig.set_line  = telephony_set_line;
  tconfig.get_line  = telephony_get_line;
  telephony_init(tel, &tconfig);

  ret = pnut_module_add(loop, &tel->module);
  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  pnut_loop_destroy(loop);
  free(tel);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
