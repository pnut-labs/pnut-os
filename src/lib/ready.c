/****************************************************************************
 * pnut-os/src/lib/ready.c
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
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "pnut_internal.h"

/* NxInit's control socket (apps/system/nxinit/control.h) speaks lines of
 * text: a connection gets a version line, "nxinit <n>", or why it is
 * turned away; then one answer to each command, "ok" or "error <errno>
 * <text>".  NxInit takes "ready" only from the task it started for a
 * service with the option "notify", checked with SO_PEERCRED, so it is
 * sent from the program's own connection.
 */

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void ready_try(FAR struct pnut_loop_s *loop);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ready_close
 ****************************************************************************/

static void ready_close(FAR struct pnut_loop_s *loop)
{
  FAR struct pnut_ready_s *ready = &loop->nxinit;

  if (ready->fd >= 0)
    {
      pnut_loop_unwatch(loop, ready->fd);
      close(ready->fd);
      ready->fd = -1;
    }
}

/****************************************************************************
 * Name: ready_finish
 *
 * Description:
 *   NxInit has answered, or will not be asked again.
 *
 ****************************************************************************/

static void ready_finish(FAR struct pnut_loop_s *loop)
{
  FAR struct pnut_ready_s *ready = &loop->nxinit;

  ready_close(loop);
  if (ready->timer != NULL)
    {
      pnut_timer_clear(loop, ready->timer);
    }

  ready->telling = false;
}

/****************************************************************************
 * Name: ready_later
 *
 * Description:
 *   Try again after a growing delay: NxInit could not be reached, or
 *   turned the connection away.  Only the first failure is logged.
 *
 ****************************************************************************/

static void ready_later(FAR struct pnut_loop_s *loop, FAR const char *why,
                        int err)
{
  FAR struct pnut_ready_s *ready = &loop->nxinit;

  ready_close(loop);

  if (!ready->warned)
    {
      pnut_loop_log(loop, PNUT_LOG_WARNING,
                    "Could not tell NxInit it is ready: %s %d; trying again",
                    why, err);
      ready->warned = true;
    }

  pnut_timer_set(loop, ready->timer, ready->backoff);

  ready->backoff *= 2;
  if (ready->backoff > CONFIG_PNUT_LIB_RECONNECT_MAX)
    {
      ready->backoff = CONFIG_PNUT_LIB_RECONNECT_MAX;
    }
}

/****************************************************************************
 * Name: ready_answer
 *
 * Description:
 *   A line from NxInit, without its newline.
 *
 ****************************************************************************/

static void ready_answer(FAR struct pnut_loop_s *loop, FAR const char *line)
{
  FAR struct pnut_ready_s *ready = &loop->nxinit;

  if (!ready->version)
    {
      if (strncmp(line, "nxinit ", 7) != 0)
        {
          ready_later(loop, line, -EBUSY);
          return;
        }

      ready->version = true;
      return;
    }

  if (strcmp(line, "ok") == 0)
    {
      pnut_loop_log(loop, PNUT_LOG_DEBUG, "NxInit knows it is ready");
    }
  else if (strncmp(line, "error 3 ", 8) == 0)
    {
      /* ESRCH: a program started by hand, not as NxInit's service */

      pnut_loop_log(loop, PNUT_LOG_DEBUG, "Not NxInit's service: %s",
                    line);
    }
  else
    {
      pnut_loop_log(loop, PNUT_LOG_WARNING, "NxInit: %s", line);
    }

  ready_finish(loop);
}

/****************************************************************************
 * Name: ready_read
 ****************************************************************************/

static void ready_read(FAR struct pnut_loop_s *loop, int fd,
                       uint32_t events, FAR void *arg)
{
  FAR struct pnut_ready_s *ready = &loop->nxinit;
  FAR char *nl;
  ssize_t n;

  n = read(fd, ready->in + ready->len, sizeof(ready->in) - ready->len);
  if (n < 0 && (errno == EAGAIN || errno == EINTR))
    {
      return;
    }

  if (n <= 0)
    {
      ready_later(loop, "connection closed", n < 0 ? -errno : -ECONNRESET);
      return;
    }

  ready->len += n;

  while (ready->fd >= 0 &&
         (nl = memchr(ready->in, '\n', ready->len)) != NULL)
    {
      *nl = '\0';
      if (nl > ready->in && *(nl - 1) == '\r')
        {
          *(nl - 1) = '\0';
        }

      ready_answer(loop, ready->in);

      ready->len -= nl + 1 - ready->in;
      memmove(ready->in, nl + 1, ready->len);
    }

  if (ready->fd >= 0 && ready->len == sizeof(ready->in))
    {
      ready_later(loop, "line too long", -E2BIG);
    }
}

/****************************************************************************
 * Name: ready_timer
 *
 * Description:
 *   With a connection, NxInit has not answered in time; without one, it
 *   is time to try again.
 *
 ****************************************************************************/

static void ready_timer(FAR struct pnut_loop_s *loop,
                        FAR struct pnut_timer_s *timer, FAR void *arg)
{
  if (loop->nxinit.fd >= 0)
    {
      ready_later(loop, "no answer", -ETIMEDOUT);
    }
  else
    {
      ready_try(loop);
    }
}

/****************************************************************************
 * Name: ready_try
 ****************************************************************************/

static void ready_try(FAR struct pnut_loop_s *loop)
{
  static const char command[] = "ready\n";
  FAR struct pnut_ready_s *ready = &loop->nxinit;
  struct sockaddr_un addr;
  int ret;
  int fd;

  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    {
      ready_later(loop, "socket", -errno);
      return;
    }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strlcpy(addr.sun_path, loop->config.initctl, sizeof(addr.sun_path));

  /* NxInit reads the command after the version line it sends: the socket
   * holds it until then
   */

  if (connect(fd, (FAR struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      send(fd, command, sizeof(command) - 1, MSG_NOSIGNAL) !=
      sizeof(command) - 1)
    {
      ret = -errno;
      close(fd);
      ready_later(loop, loop->config.initctl, ret);
      return;
    }

  ret = pnut_loop_watch_ready(loop, fd, EPOLLIN, ready_read, NULL);
  if (ret < 0)
    {
      close(fd);
      ready_later(loop, "watch", ret);
      return;
    }

  ready->fd      = fd;
  ready->len     = 0;
  ready->version = false;
  pnut_timer_set(loop, ready->timer, CONFIG_PNUT_LIB_CALL_TIMEOUT);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: pnut_ready_init
 ****************************************************************************/

int pnut_ready_init(FAR struct pnut_loop_s *loop)
{
  FAR struct pnut_ready_s *ready = &loop->nxinit;

  if (loop->config.initctl == NULL)
    {
      return OK;
    }

  if (strlen(loop->config.initctl) >=
      sizeof(((FAR struct sockaddr_un *)NULL)->sun_path))
    {
      return -ENAMETOOLONG;
    }

  return pnut_timer_keep(loop, ready_timer, NULL, &ready->timer);
}

/****************************************************************************
 * Name: pnut_ready_tell
 ****************************************************************************/

void pnut_ready_tell(FAR struct pnut_loop_s *loop)
{
  FAR struct pnut_ready_s *ready = &loop->nxinit;

  if (ready->timer == NULL || ready->telling)
    {
      return;
    }

  ready->telling = true;
  ready->backoff = CONFIG_PNUT_LIB_RECONNECT_MIN;
  ready->warned  = false;
  ready_try(loop);
}

/****************************************************************************
 * Name: pnut_ready_stop
 ****************************************************************************/

void pnut_ready_stop(FAR struct pnut_loop_s *loop)
{
  ready_finish(loop);
}
