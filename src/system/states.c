/****************************************************************************
 * pnut-os/src/system/states.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/msg.h>

#include "states.h"

/* NxInit's control socket (apps/system/nxinit/control.h) speaks lines of
 * text.  A connection gets a version line, "nxinit <n>", or why it is
 * turned away; then one answer to each command, in their order: "ok",
 * "ok <n>" before the n lines of a list, or "error <errno> <text>".  Once
 * the connection watches, "event <service> <state> <pid>" lines come
 * between the answers, never inside a list.
 */

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define STATES_PAGE  8                  /* ServicesPage's max_count */

#define STATES_NSTATES \
  ((int)(sizeof(g_states_names) / sizeof(g_states_names[0])))

/* The wait before watching NxInit again, growing from the first to the
 * longest, in milliseconds
 */

#define STATES_BACKOFF_MIN  100
#define STATES_BACKOFF_MAX  5000

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void states_list(FAR struct pnut_services_server_s *server,
                        FAR const struct pnut_request_s *req,
                        FAR const pnut_services_page_request_t *in,
                        FAR void *arg);
static void states_get(FAR struct pnut_services_server_s *server,
                       FAR const struct pnut_request_s *req,
                       FAR const pnut_services_name_request_t *in,
                       FAR void *arg);
static void states_start_service(FAR struct pnut_services_server_s *server,
                                 FAR const struct pnut_request_s *req,
                                 FAR const pnut_services_name_request_t *in,
                                 FAR void *arg);
static void states_stop_service(FAR struct pnut_services_server_s *server,
                                FAR const struct pnut_request_s *req,
                                FAR const pnut_services_name_request_t *in,
                                FAR void *arg);
static void states_who_is(FAR struct pnut_services_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_services_who_request_t *in,
                          FAR void *arg);
static void states_connect(FAR struct states_s *states);
static int states_lookup(FAR struct states_s *states, pid_t pid,
                         bool remember, FAR const char **namep);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct pnut_services_handlers_s g_states_handlers =
{
  .list  = states_list,
  .get   = states_get,
  .start = states_start_service,
  .stop  = states_stop_service,
  .who   = states_who_is,
};

/* NxInit's names of the states, in pnut.ServiceState.State's order */

static FAR const char *const g_states_names[] =
{
  "stopped",
  "starting",
  "ready",
  "stopping",
  "restarting",
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: states_now
 *
 * Description:
 *   Milliseconds, monotonic.
 *
 ****************************************************************************/

static int64_t states_now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/****************************************************************************
 * Name: states_goodname
 *
 * Description:
 *   Whether a name can be sent to NxInit as a word of a command.
 *
 ****************************************************************************/

static bool states_goodname(FAR const char *name)
{
  size_t len = strlen(name);
  size_t i;

  if (len == 0 || len > STATES_NAME_MAX)
    {
      return false;
    }

  for (i = 0; i < len; i++)
    {
      char c = name[i];

      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
        {
          return false;
        }
    }

  return true;
}

/****************************************************************************
 * Name: states_find
 ****************************************************************************/

static FAR struct states_service_s *
states_find(FAR struct states_s *states, FAR const char *name)
{
  uint8_t i;

  for (i = 0; i < states->nservices; i++)
    {
      if (strcmp(states->services[i].name, name) == 0)
        {
          return &states->services[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: states_insert
 *
 * Description:
 *   A service NxInit has named, kept in the order of the names.
 *
 ****************************************************************************/

static FAR struct states_service_s *
states_insert(FAR struct states_s *states, FAR const char *name)
{
  FAR struct states_service_s *service = states_find(states, name);
  uint8_t i;

  if (service != NULL)
    {
      return service;
    }

  if (states->nservices == states->config.services)
    {
      pnut_log(&states->module, PNUT_LOG_WARNING,
               "No room for service %s", name);
      return NULL;
    }

  for (i = 0; i < states->nservices; i++)
    {
      if (strcmp(states->services[i].name, name) > 0)
        {
          break;
        }
    }

  memmove(&states->services[i + 1], &states->services[i],
          (states->nservices - i) * sizeof(states->services[0]));
  states->nservices++;

  service = &states->services[i];
  memset(service, 0, sizeof(*service));
  strlcpy(service->name, name, sizeof(service->name));
  return service;
}

/****************************************************************************
 * Name: states_known
 *
 * Description:
 *   A pid the watch shows for a service is no stranger.
 *
 ****************************************************************************/

static void states_known(FAR struct states_s *states, pid_t pid)
{
  uint8_t i;

  for (i = 0; pid > 0 && i < STATES_STRANGERS; i++)
    {
      if (states->strangers[i] == pid)
        {
          states->strangers[i] = 0;
        }
    }
}

/****************************************************************************
 * Name: states_publish
 ****************************************************************************/

static void states_publish(FAR struct states_s *states,
                           FAR const struct states_service_s *service)
{
  pnut_service_change_t msg;
  int ret;

  if (states->changes == NULL)
    {
      return;
    }

  memset(&msg, 0, sizeof(msg));
  strlcpy(msg.name, service->name, sizeof(msg.name));
  msg.state = service->state;
  msg.pid   = service->pid;
  msg.seq   = ++states->seq;

  ret = pnut_service_change_publish(states->changes, &msg);
  if (ret < 0)
    {
      pnut_log(&states->module, PNUT_LOG_WARNING,
               "Cannot announce a change: %d", ret);
    }
}

/****************************************************************************
 * Name: states_update
 *
 * Description:
 *   A line "<service> <state> <pid>" of NxInit's, as a list's line or an
 *   event's: changes are published once a whole list has come.
 *
 ****************************************************************************/

static void states_update(FAR struct states_s *states, FAR char *line)
{
  FAR struct states_service_s *service;
  FAR char *words[3];
  FAR char *save;
  FAR char *end;
  long pid;
  int state;

  words[0] = strtok_r(line, " ", &save);
  words[1] = words[0] != NULL ? strtok_r(NULL, " ", &save) : NULL;
  words[2] = words[1] != NULL ? strtok_r(NULL, " ", &save) : NULL;

  if (words[2] == NULL || strtok_r(NULL, " ", &save) != NULL)
    {
      pnut_log(&states->module, PNUT_LOG_WARNING, "NxInit: odd line");
      return;
    }

  for (state = 0; state < STATES_NSTATES; state++)
    {
      if (strcmp(words[1], g_states_names[state]) == 0)
        {
          break;
        }
    }

  pid = strtol(words[2], &end, 10);
  if (state == STATES_NSTATES || *end != '\0' || pid < 0 ||
      !states_goodname(words[0]))
    {
      pnut_log(&states->module, PNUT_LOG_WARNING,
               "NxInit: odd state of %s", words[0]);
      return;
    }

  service = states_insert(states, words[0]);
  if (service == NULL)
    {
      return;
    }

  service->seen = true;
  states_known(states, (pid_t)pid);
  if (service->state != state || service->pid != (pid_t)pid)
    {
      service->state = state;
      service->pid   = (pid_t)pid;
      if (states->listed)
        {
          states_publish(states, service);
        }
    }
}

/****************************************************************************
 * Name: states_listed
 *
 * Description:
 *   A whole list has come: a service it did not name has gone from NxInit
 *   (a oneshot that ended), and counts as stopped.
 *
 ****************************************************************************/

static void states_listed(FAR struct states_s *states)
{
  FAR struct states_service_s *service;
  bool first = !states->listed;
  uint8_t i;

  for (i = 0; i < states->nservices; i++)
    {
      service = &states->services[i];
      if (!service->seen &&
          (service->state != PNUT_SERVICE_STATE_STATE_STOPPED ||
           service->pid != 0))
        {
          service->state = PNUT_SERVICE_STATE_STATE_STOPPED;
          service->pid   = 0;
          if (!first)
            {
              states_publish(states, service);
            }
        }
    }

  states->listed = true;
  if (first)
    {
      pnut_module_ready(&states->module);
    }
}

/****************************************************************************
 * Name: states_answer
 *
 * Description:
 *   Answer a caller's start or stop as NxInit answered it.
 *
 ****************************************************************************/

static void states_answer(FAR struct states_s *states,
                          FAR const struct states_command_s *command,
                          int status, uint32_t code, FAR const char *text)
{
  if (!states->serving)
    {
      return;
    }

  if (status != PNUT_STATUS_OK)
    {
      pnut_services_fail(&states->server, &command->req, status, code, text);
    }
  else if (command->kind == STATES_START)
    {
      pnut_services_start_reply(&states->server, &command->req,
                                PNUT_STATUS_OK, NULL);
    }
  else
    {
      pnut_services_stop_reply(&states->server, &command->req,
                               PNUT_STATUS_OK, NULL);
    }
}

/****************************************************************************
 * Name: states_close
 *
 * Description:
 *   Close the watch; the starts and stops still owed fail.
 *
 ****************************************************************************/

static void states_close(FAR struct states_s *states)
{
  FAR struct states_command_s *command;

  if (states->fd >= 0)
    {
      pnut_loop_unwatch(states->module.loop, states->fd);
      close(states->fd);
      states->fd = -1;
    }

  while (states->npending > 0)
    {
      command = &states->pending[states->head];
      states->head = (states->head + 1) % STATES_PENDING;
      states->npending--;

      if (command->kind == STATES_START || command->kind == STATES_STOP)
        {
          states_answer(states, command, PNUT_STATUS_UNAVAILABLE,
                        PNUT_SERVICES_ERROR_CODE_NONE, "NxInit went away");
        }
    }
}

/****************************************************************************
 * Name: states_retry
 ****************************************************************************/

static void states_retry(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct states_s *states = arg;

  states->timer = NULL;
  states_connect(states);
}

/****************************************************************************
 * Name: states_lost
 *
 * Description:
 *   The watch failed: try again after a growing delay.  Only the first
 *   failure in a row is logged.
 *
 ****************************************************************************/

static void states_lost(FAR struct states_s *states, FAR const char *why,
                        int err)
{
  int ret;

  states_close(states);

  if (!states->warned)
    {
      pnut_log(&states->module, PNUT_LOG_WARNING,
               "No watch on NxInit: %s %d; trying again", why, err);
      states->warned = true;
    }

  ret = pnut_timer_start(states->module.loop, states->backoff, 0,
                         states_retry, states, &states->timer);
  if (ret < 0)
    {
      states->timer = NULL;
      pnut_log(&states->module, PNUT_LOG_ERROR,
               "Cannot watch NxInit again: no timer %d", ret);
    }

  states->backoff *= 2;
  if (states->backoff > STATES_BACKOFF_MAX)
    {
      states->backoff = STATES_BACKOFF_MAX;
    }
}

/****************************************************************************
 * Name: states_send
 *
 * Description:
 *   Send NxInit a command, whose answer comes later; req for a start or a
 *   stop, copied.
 *
 ****************************************************************************/

static int states_send(FAR struct states_s *states, uint8_t kind,
                       FAR const char *line,
                       FAR const struct pnut_request_s *req)
{
  FAR struct states_command_s *command;
  size_t len = strlen(line);

  if (states->fd < 0)
    {
      return -ENOTCONN;
    }

  if (states->npending == STATES_PENDING)
    {
      return -EBUSY;
    }

  if (send(states->fd, line, len, MSG_NOSIGNAL) != (ssize_t)len)
    {
      return -EIO;
    }

  command = &states->pending[(states->head + states->npending) %
                             STATES_PENDING];
  command->kind = kind;
  if (req != NULL)
    {
      command->req = *req;
    }

  states->npending++;
  return OK;
}

/****************************************************************************
 * Name: states_line
 *
 * Description:
 *   A line from NxInit, without its newline.
 *
 ****************************************************************************/

static void states_line(FAR struct states_s *states, FAR char *line)
{
  FAR struct states_command_s *command;
  FAR char *text;
  FAR char *end;
  long count;
  uint8_t i;
  bool ok;

  if (!states->version)
    {
      if (strncmp(line, "nxinit ", 7) != 0)
        {
          states_lost(states, line, -EBUSY);
          return;
        }

      states->version = true;
      return;
    }

  if (states->listing > 0)
    {
      states_update(states, line);
      if (--states->listing == 0)
        {
          states_listed(states);
        }

      return;
    }

  if (strncmp(line, "event ", 6) == 0)
    {
      states_update(states, line + 6);
      return;
    }

  /* An answer, to the oldest command */

  if (states->npending == 0)
    {
      pnut_log(&states->module, PNUT_LOG_WARNING, "NxInit: odd answer");
      return;
    }

  command = &states->pending[states->head];
  states->head = (states->head + 1) % STATES_PENDING;
  states->npending--;

  ok = strcmp(line, "ok") == 0 || strncmp(line, "ok ", 3) == 0;

  switch (command->kind)
    {
      case STATES_WATCH:
        if (!ok)
          {
            states_lost(states, line, -EIO);
          }
        break;

      case STATES_LIST:
        if (!ok)
          {
            states_lost(states, line, -EIO);
            break;
          }

        for (i = 0; i < states->nservices; i++)
          {
            states->services[i].seen = false;
          }

        count = line[2] == ' ' ? strtol(line + 3, &end, 10) : -1;
        if (count < 0 || *end != '\0' || count > UINT16_MAX)
          {
            states_lost(states, "odd list", -EIO);
            break;
          }

        states->listing = (uint16_t)count;
        if (states->listing == 0)
          {
            states_listed(states);
          }
        break;

      default:
        if (ok)
          {
            states_answer(states, command, PNUT_STATUS_OK, 0, NULL);
            break;
          }

        /* "error <errno> <text>": ENOENT is an unknown service */

        text = strncmp(line, "error ", 6) == 0 ? strchr(line + 6, ' ') :
               NULL;
        text = text != NULL ? text + 1 : line;
        if (strncmp(line, "error 2 ", 8) == 0)
          {
            states_answer(states, command, PNUT_STATUS_NOTFOUND,
                          PNUT_SERVICES_ERROR_CODE_UNKNOWN_SERVICE, text);
          }
        else
          {
            states_answer(states, command, PNUT_STATUS_INVALID,
                          PNUT_SERVICES_ERROR_CODE_REFUSED, text);
          }
        break;
    }
}

/****************************************************************************
 * Name: states_read
 ****************************************************************************/

static void states_read(FAR struct pnut_loop_s *loop, int fd,
                        uint32_t events, FAR void *arg)
{
  FAR struct states_s *states = arg;
  FAR char *nl;
  ssize_t n;

  n = read(fd, states->in + states->len, sizeof(states->in) - states->len);
  if (n < 0 && (errno == EAGAIN || errno == EINTR))
    {
      return;
    }

  if (n <= 0)
    {
      states_lost(states, "connection closed",
                  n < 0 ? -errno : -ECONNRESET);
      return;
    }

  /* A connection that has answered is a good one */

  states->backoff = STATES_BACKOFF_MIN;
  states->warned  = false;
  states->len    += n;

  while (states->fd >= 0 &&
         (nl = memchr(states->in, '\n', states->len)) != NULL)
    {
      *nl = '\0';
      if (nl > states->in && *(nl - 1) == '\r')
        {
          *(nl - 1) = '\0';
        }

      states_line(states, states->in);
      if (states->fd < 0)
        {
          break;
        }

      states->len -= nl + 1 - states->in;
      memmove(states->in, nl + 1, states->len);
    }

  if (states->fd >= 0 && states->len == sizeof(states->in))
    {
      states_lost(states, "line too long", -E2BIG);
    }
}

/****************************************************************************
 * Name: states_connect
 *
 * Description:
 *   Open the watch: "watch" first, so that no change between the list and
 *   the watch is missed, then "state" for the list.
 *
 ****************************************************************************/

static void states_connect(FAR struct states_s *states)
{
  struct sockaddr_un addr;
  int ret;
  int fd;

  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    {
      states_lost(states, "socket", -errno);
      return;
    }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strlcpy(addr.sun_path, states->config.initctl, sizeof(addr.sun_path));

  if (connect(fd, (FAR struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
      ret = -errno;
      close(fd);
      states_lost(states, states->config.initctl, ret);
      return;
    }

  ret = pnut_loop_watch(states->module.loop, fd, EPOLLIN, states_read,
                        states);
  if (ret < 0)
    {
      close(fd);
      states_lost(states, "watch", ret);
      return;
    }

  states->fd      = fd;
  states->len     = 0;
  states->version = false;
  states->listing = 0;

  ret = states_send(states, STATES_WATCH, "watch\n", NULL);
  if (ret >= 0)
    {
      ret = states_send(states, STATES_LIST, "state\n", NULL);
    }

  if (ret < 0)
    {
      states_lost(states, "write", ret);
    }
}

/****************************************************************************
 * Name: states_ask
 *
 * Description:
 *   Ask NxInit "who <pid>" on a connection of its own, waiting
 *   STATES_ASK_MS at most; a connection turned away is tried again within
 *   that time.
 *
 * Returned Value:
 *   Zero (OK) with the name; -ESRCH when the task is no running service's;
 *   another negated errno value when NxInit could not be asked.
 *
 ****************************************************************************/

static int states_ask(FAR const char *path, pid_t pid, FAR char *name,
                      size_t size)
{
  int64_t deadline = states_now() + STATES_ASK_MS;
  struct sockaddr_un addr;
  struct pollfd pfd;
  char in[STATES_LINE_MAX];
  char command[24];
  FAR char *nl;
  size_t len;
  int64_t left;
  ssize_t n;
  int ret;
  bool version;

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
  snprintf(command, sizeof(command), "who %d\n", (int)pid);

  for (; ; )
    {
      pfd.fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                      0);
      if (pfd.fd < 0)
        {
          return -errno;
        }

      if (connect(pfd.fd, (FAR struct sockaddr *)&addr, sizeof(addr)) < 0 ||
          send(pfd.fd, command, strlen(command), MSG_NOSIGNAL) !=
          (ssize_t)strlen(command))
        {
          ret = -errno;
          close(pfd.fd);
          return ret;
        }

      pfd.events = POLLIN;
      version    = false;
      len        = 0;
      ret        = -ETIMEDOUT;

      while (ret == -ETIMEDOUT)
        {
          nl = memchr(in, '\n', len);
          if (nl != NULL)
            {
              *nl = '\0';
              if (!version)
                {
                  version = strncmp(in, "nxinit ", 7) == 0;
                  ret     = version ? -ETIMEDOUT : -EBUSY;
                }
              else if (strncmp(in, "ok ", 3) == 0)
                {
                  /* A name too long for a program's is no program's */

                  ret = strlcpy(name, in + 3, size) < size ? OK : -ESRCH;
                }
              else
                {
                  ret = strncmp(in, "error 3 ", 8) == 0 ? -ESRCH : -EIO;
                }

              len -= nl + 1 - in;
              memmove(in, nl + 1, len);
              continue;
            }

          left = deadline - states_now();
          if (left <= 0 || len == sizeof(in))
            {
              ret = len == sizeof(in) ? -E2BIG : -ETIMEDOUT;
              break;
            }

          if (poll(&pfd, 1, (int)left) <= 0)
            {
              continue;
            }

          n = read(pfd.fd, in + len, sizeof(in) - len);
          if (n < 0 && (errno == EAGAIN || errno == EINTR))
            {
              continue;
            }

          if (n <= 0)
            {
              ret = n < 0 ? -errno : -ECONNRESET;
              break;
            }

          len += n;
        }

      close(pfd.fd);

      /* Turned away: too many connections at once; try again soon, if
       * there is time
       */

      if (ret != -EBUSY || deadline - states_now() < 50)
        {
          return ret;
        }

      poll(NULL, 0, 50);
    }
}

/****************************************************************************
 * Name: states_caller
 *
 * Description:
 *   Whether a request comes from the system UI or the system program,
 *   which alone may start and stop services: zero (OK), -EPERM, or
 *   -EAGAIN when NxInit cannot tell now.
 *
 ****************************************************************************/

static int states_caller(FAR struct states_s *states,
                         FAR const struct pnut_request_s *req)
{
  FAR const char *program;
  int ret;

  ret = states_who(states, req->pid, &program);
  if (ret == -EAGAIN)
    {
      return ret;
    }

  return ret == OK && (strcmp(program, "ui") == 0 ||
                       strcmp(program, "system") == 0) ? OK : -EPERM;
}

/****************************************************************************
 * Name: states_list
 ****************************************************************************/

static void states_list(FAR struct pnut_services_server_s *server,
                        FAR const struct pnut_request_s *req,
                        FAR const pnut_services_page_request_t *in,
                        FAR void *arg)
{
  FAR struct states_s *states = arg;
  FAR pnut_services_page_t *page = &states->out.page;
  FAR const struct states_service_s *service;
  uint8_t i;

  memset(page, 0, sizeof(*page));

  for (i = 0; i < states->nservices; i++)
    {
      service = &states->services[i];
      if (strcmp(service->name, in->after) <= 0)
        {
          continue;
        }

      if (page->services_count == STATES_PAGE)
        {
          page->more = true;
          break;
        }

      strlcpy(page->services[page->services_count].name, service->name,
              sizeof(page->services[0].name));
      page->services[page->services_count].state = service->state;
      page->services[page->services_count].pid   = service->pid;
      page->services_count++;
    }

  pnut_services_list_reply(server, req, PNUT_STATUS_OK, page);
}

/****************************************************************************
 * Name: states_get
 ****************************************************************************/

static void states_get(FAR struct pnut_services_server_s *server,
                       FAR const struct pnut_request_s *req,
                       FAR const pnut_services_name_request_t *in,
                       FAR void *arg)
{
  FAR struct states_s *states = arg;
  FAR const struct states_service_s *service;

  service = states_find(states, in->name);
  if (service == NULL)
    {
      pnut_services_fail(server, req, PNUT_STATUS_NOTFOUND,
                         PNUT_SERVICES_ERROR_CODE_UNKNOWN_SERVICE,
                         "no such service");
      return;
    }

  memset(&states->out.state, 0, sizeof(states->out.state));
  strlcpy(states->out.state.name, service->name,
          sizeof(states->out.state.name));
  states->out.state.state = service->state;
  states->out.state.pid   = service->pid;
  pnut_services_get_reply(server, req, PNUT_STATUS_OK, &states->out.state);
}

/****************************************************************************
 * Name: states_command
 *
 * Description:
 *   A caller's start or stop, passed to NxInit and answered with its
 *   answer.
 *
 ****************************************************************************/

static void states_command(FAR struct states_s *states,
                           FAR const struct pnut_request_s *req,
                           uint8_t kind, FAR const char *name)
{
  char line[STATES_LINE_MAX];
  int ret;

  ret = states_caller(states, req);
  if (ret < 0)
    {
      pnut_services_fail(&states->server, req,
                         ret == -EAGAIN ? PNUT_STATUS_UNAVAILABLE :
                         PNUT_STATUS_DENIED,
                         PNUT_SERVICES_ERROR_CODE_NONE,
                         ret == -EAGAIN ? "who calls is not known yet" :
                         "only the system UI starts and stops services");
      return;
    }

  if (!states_goodname(name))
    {
      pnut_services_fail(&states->server, req, PNUT_STATUS_INVALID,
                         PNUT_SERVICES_ERROR_CODE_UNKNOWN_SERVICE,
                         "not a service's name");
      return;
    }

  snprintf(line, sizeof(line), "%s %s\n",
           kind == STATES_START ? "start" : "stop", name);

  ret = states->version ? states_send(states, kind, line, req) : -ENOTCONN;
  if (ret == -EIO)
    {
      states_lost(states, "write", ret);
    }

  if (ret < 0)
    {
      pnut_services_fail(&states->server, req,
                         ret == -EBUSY ? PNUT_STATUS_BUSY :
                         PNUT_STATUS_UNAVAILABLE,
                         PNUT_SERVICES_ERROR_CODE_NONE,
                         "NxInit cannot be asked now");
    }
}

/****************************************************************************
 * Name: states_start_service, states_stop_service
 ****************************************************************************/

static void states_start_service(FAR struct pnut_services_server_s *server,
                                 FAR const struct pnut_request_s *req,
                                 FAR const pnut_services_name_request_t *in,
                                 FAR void *arg)
{
  states_command(arg, req, STATES_START, in->name);
}

static void states_stop_service(FAR struct pnut_services_server_s *server,
                                FAR const struct pnut_request_s *req,
                                FAR const pnut_services_name_request_t *in,
                                FAR void *arg)
{
  states_command(arg, req, STATES_STOP, in->name);
}

/****************************************************************************
 * Name: states_who_is
 ****************************************************************************/

static void states_who_is(FAR struct pnut_services_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_services_who_request_t *in,
                          FAR void *arg)
{
  FAR struct states_s *states = arg;
  FAR const char *name;
  int ret;

  /* A pid named by the caller may be no task's yet: it is not remembered
   * as a stranger, lest it be one of a program about to start
   */

  ret = states_lookup(states, (pid_t)in->pid, false, &name);
  if (ret < 0)
    {
      pnut_services_fail(server, req,
                         ret == -EAGAIN ? PNUT_STATUS_UNAVAILABLE :
                         PNUT_STATUS_NOTFOUND,
                         ret == -EAGAIN ? PNUT_SERVICES_ERROR_CODE_NONE :
                         PNUT_SERVICES_ERROR_CODE_UNKNOWN_SERVICE,
                         ret == -EAGAIN ? "NxInit cannot tell now" :
                         "no service's task");
      return;
    }

  memset(&states->out.who, 0, sizeof(states->out.who));
  strlcpy(states->out.who.name, name, sizeof(states->out.who.name));
  pnut_services_who_reply(server, req, PNUT_STATUS_OK, &states->out.who);
}

/****************************************************************************
 * Name: states_start
 ****************************************************************************/

static int states_start(FAR struct pnut_module_s *module,
                        FAR struct pnut_loop_s *loop)
{
  FAR struct states_s *states = (FAR struct states_s *)module;
  int line = 0;
  int ret;

  ret = programs_load(&states->programs, states->config.programs, &line);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING,
               "No programs' table %s (line %d): %d; every program is a "
               "stranger to Settings", states->config.programs, line, ret);
    }

  states->services = calloc(states->config.services,
                            sizeof(states->services[0]));
  if (states->services == NULL)
    {
      return -ENOMEM;
    }

  /* Changes are announced where there are topics; the states are served
   * without them
   */

  ret = pnut_service_change_advertise(loop, &states->changes);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No services topic: %d", ret);
      states->changes = NULL;
    }

  ret = pnut_services_serve(loop, STATES_SERVICE, 0, &g_states_handlers,
                            states, &states->server);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Cannot serve: %d", ret);
      pnut_topic_unadvertise(states->changes);
      states->changes = NULL;
      free(states->services);
      states->services = NULL;
      return ret;
    }

  states->serving = true;
  states->backoff = STATES_BACKOFF_MIN;

  /* Without NxInit's socket there is nothing to watch, and no task is
   * known
   */

  if (states->config.initctl == NULL)
    {
      states->listed = true;
      pnut_module_ready(module);
      return OK;
    }

  states_connect(states);
  return OK;
}

/****************************************************************************
 * Name: states_stop
 ****************************************************************************/

static void states_stop(FAR struct pnut_module_s *module,
                        FAR struct pnut_loop_s *loop)
{
  FAR struct states_s *states = (FAR struct states_s *)module;

  if (states->timer != NULL)
    {
      pnut_timer_cancel(loop, states->timer);
      states->timer = NULL;
    }

  states_close(states);

  if (states->serving)
    {
      pnut_services_close(&states->server);
      states->serving = false;
    }

  pnut_topic_unadvertise(states->changes);
  states->changes = NULL;

  free(states->services);
  states->services  = NULL;
  states->nservices = 0;
  states->listed    = false;

  memset(states->strangers, 0, sizeof(states->strangers));
  states->holdoff = 0;
}

/****************************************************************************
 * Name: states_lookup
 *
 * Description:
 *   states_who(), with the strangers remembered or not: a caller's own pid
 *   is a task's, but a pid another names may be no task's yet, and may be
 *   one of a program about to start.
 *
 ****************************************************************************/

static int states_lookup(FAR struct states_s *states, pid_t pid,
                         bool remember, FAR const char **namep)
{
  FAR struct states_service_s *service;
  char name[STATES_NAME_MAX + 1];
  uint8_t i;
  int ret;

  *namep = NULL;

  if (pid <= 0 || states->services == NULL)
    {
      return -ESRCH;
    }

  for (i = 0; i < states->nservices; i++)
    {
      service = &states->services[i];
      if (service->pid == pid &&
          service->state != PNUT_SERVICE_STATE_STATE_STOPPED &&
          service->state != PNUT_SERVICE_STATE_STATE_RESTARTING)
        {
          *namep = service->name;
          return OK;
        }
    }

  for (i = 0; i < STATES_STRANGERS; i++)
    {
      if (states->strangers[i] == pid)
        {
          return -ESRCH;
        }
    }

  if (states->config.initctl == NULL)
    {
      return -ESRCH;
    }

  /* A task the watch has not shown yet: NxInit knows it already.  After
   * an ask that failed, NxInit is given a moment before the next.
   */

  if (states_now() < states->holdoff)
    {
      return -EAGAIN;
    }

  ret = states_ask(states->config.initctl, pid, name, sizeof(name));
  if (ret >= 0 && !states_goodname(name))
    {
      ret = -ESRCH;               /* A service, but no program's name */
    }

  if (ret == -ESRCH)
    {
      if (remember)
        {
          states->strangers[states->nextstranger] = pid;
          states->nextstranger = (states->nextstranger + 1) %
                                 STATES_STRANGERS;
        }

      return -ESRCH;
    }

  if (ret < 0)
    {
      pnut_log(&states->module, PNUT_LOG_WARNING,
               "Cannot ask NxInit who %d is: %d", (int)pid, ret);
      states->holdoff = states_now() + STATES_ASK_MS;
      return -EAGAIN;
    }

  /* The watch's event about it comes later, and publishes it.  With no
   * room left in the table the name is kept aside, for this answer.
   */

  service = states_insert(states, name);
  if (service == NULL)
    {
      strlcpy(states->asked, name, sizeof(states->asked));
      *namep = states->asked;
      return OK;
    }

  service->pid = pid;
  if (service->state == PNUT_SERVICE_STATE_STATE_STOPPED ||
      service->state == PNUT_SERVICE_STATE_STATE_RESTARTING)
    {
      service->state = PNUT_SERVICE_STATE_STATE_STARTING;
    }

  *namep = service->name;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void states_init(FAR struct states_s *states,
                 FAR const struct states_config_s *config)
{
  memset(states, 0, sizeof(*states));
  states->module.name  = "states";
  states->module.start = states_start;
  states->module.stop  = states_stop;
  states->config       = *config;
  states->fd           = -1;
}

void states_deinit(FAR struct states_s *states)
{
  free(states->services);
  states->services = NULL;
}

int states_who(FAR struct states_s *states, pid_t pid,
               FAR const char **namep)
{
  return states_lookup(states, pid, true, namep);
}

bool states_runs(FAR struct states_s *states, FAR const char *program,
                 FAR const char *service)
{
  return programs_runs(&states->programs, program, service);
}
