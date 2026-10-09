/****************************************************************************
 * pnut-os/src/lib/topic.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pnut/topic.h>

#include "pnut_internal.h"

#if defined(__NuttX__) && defined(CONFIG_UORB)
#  include <uORB/uORB.h>
#elif !defined(__NuttX__)
#  include <dirent.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/un.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* On NuttX without uORB, topics are not there */

#if defined(__NuttX__) && !defined(CONFIG_UORB)
#  define TOPIC_NONE
#endif

/* On the computer, a topic is a directory in the run directory, with a
 * socket for each reader and the latest message in a file
 */

/* Where a reader's message goes, after its struct: aligned for any type */

#define TOPIC_MSG_OFFSET \
  ((sizeof(struct pnut_subscriber_s) + alignof(max_align_t) - 1) & \
   ~(alignof(max_align_t) - 1))

#ifndef __NuttX__
#  define TOPIC_PATH_MAX      108     /* sockaddr_un's sun_path */
#  define TOPIC_LAST          "last"
#  define TOPIC_READER        "r."
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct pnut_publisher_s
{
  FAR const struct pnut_topic_s *topic;
  int fd;
#ifndef __NuttX__
  char dir[TOPIC_PATH_MAX];
#endif
};

struct pnut_subscriber_s
{
  FAR struct pnut_loop_s *loop;
  FAR const struct pnut_topic_s *topic;
  pnut_topic_handler_t handler;
  FAR void *arg;
  int fd;
  bool dispatching;               /* Its handler runs */
  bool closing;                   /* Unsubscribed by it: freed after */
  bool failed;                    /* Reading failed: no longer watched */
#ifndef __NuttX__
  bool live;                      /* A publisher's message has come */
  char path[TOPIC_PATH_MAX];
#endif
  FAR uint8_t *msg;               /* A message read, topic->size bytes */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifndef TOPIC_NONE

/****************************************************************************
 * Name: topic_free
 ****************************************************************************/

static void topic_free(FAR struct pnut_subscriber_s *sub)
{
  if (sub->fd >= 0)
    {
      if (!sub->failed)
        {
          pnut_loop_unwatch(sub->loop, sub->fd);
        }

#if defined(__NuttX__)
      orb_unsubscribe(sub->fd);
#else
      close(sub->fd);
      unlink(sub->path);
#endif
    }

  free(sub);
}

#if defined(__NuttX__)

/****************************************************************************
 * Name: topic_read
 *
 * Description:
 *   The next message for a reader: 1 when there was one, 0 when none
 *   waits, a negated errno value on failure.
 *
 ****************************************************************************/

static int topic_read(FAR struct pnut_subscriber_s *sub)
{
  bool updated = false;
  ssize_t n;

  if (orb_check(sub->fd, &updated) < 0)
    {
      return -errno;
    }

  if (!updated)
    {
      return 0;
    }

  n = orb_copy_multi(sub->fd, sub->msg, sub->topic->size);
  if (n < 0)
    {
      return -errno;
    }

  return n == sub->topic->size ? 1 : -EIO;
}

#else

/****************************************************************************
 * Name: topic_dir
 *
 * Description:
 *   A topic's directory: <rundir>/topic.<name>, made if it is not there.
 *
 ****************************************************************************/

static int topic_dir(FAR struct pnut_loop_s *loop,
                     FAR const struct pnut_topic_s *topic, FAR char *dir)
{
  int ret;

  ret = snprintf(dir, TOPIC_PATH_MAX, "%s/topic.%s", loop->config.rundir,
                 topic->name);

  /* With room for a reader's socket, /r.<pid>.<fd> */

  if (ret < 0 || ret >= TOPIC_PATH_MAX - 24)
    {
      return -ENAMETOOLONG;
    }

  if (mkdir(dir, 0700) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  return OK;
}

/****************************************************************************
 * Name: topic_path
 *
 * Description:
 *   A file in a topic's directory.
 *
 ****************************************************************************/

static int topic_path(FAR char *path, FAR const char *dir,
                      FAR const char *name)
{
  int ret;

  ret = snprintf(path, TOPIC_PATH_MAX, "%s/%s", dir, name);
  return ret < 0 || ret >= TOPIC_PATH_MAX ? -ENAMETOOLONG : OK;
}

/****************************************************************************
 * Name: topic_address
 ****************************************************************************/

static socklen_t topic_address(FAR struct sockaddr_un *addr,
                               FAR const char *path)
{
  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;
  strlcpy(addr->sun_path, path, sizeof(addr->sun_path));
  return sizeof(*addr);
}

/****************************************************************************
 * Name: topic_read
 *
 * Description:
 *   As on NuttX.  A datagram of another size is not one of the topic's,
 *   and is dropped.  The latest message a reader sent itself as it
 *   subscribed is dropped too if a publisher's came first: it is older.
 *
 ****************************************************************************/

static int topic_read(FAR struct pnut_subscriber_s *sub)
{
  struct sockaddr_un from;
  socklen_t len;
  ssize_t n;
  bool self;

  for (; ; )
    {
      memset(&from, 0, sizeof(from));
      len = sizeof(from);
      n   = recvfrom(sub->fd, sub->msg, sub->topic->size,
                     MSG_DONTWAIT | MSG_TRUNC,
                     (FAR struct sockaddr *)&from, &len);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n < 0)
        {
          return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -errno;
        }

      if (n != sub->topic->size)
        {
          continue;
        }

      /* A publisher's socket is not bound: only the reader's own has a
       * name
       */

      self = len > offsetof(struct sockaddr_un, sun_path) &&
             strcmp(from.sun_path, sub->path) == 0;
      if (self && sub->live)
        {
          continue;
        }

      sub->live |= !self;
      return 1;
    }
}

#endif /* __NuttX__ */

/****************************************************************************
 * Name: topic_readable
 *
 * Description:
 *   Messages wait for a reader: hand each to its handler, one more than
 *   the topic keeps at most, so that a busy topic does not hold the loop;
 *   the rest wake the loop again, which watches the descriptor until it
 *   has nothing to read.  A reader whose reads fail is no longer watched,
 *   or the loop would wake for it again and again: logged, it waits for
 *   its owner to unsubscribe.
 *
 ****************************************************************************/

static void topic_readable(FAR struct pnut_loop_s *loop, int fd,
                           uint32_t events, FAR void *arg)
{
  FAR struct pnut_subscriber_s *sub = arg;
  int ret;
  int i;

  sub->dispatching = true;

  for (i = 0; i <= sub->topic->depth && !sub->closing; i++)
    {
      ret = topic_read(sub);
      if (ret < 0)
        {
          pnut_loop_log(loop, PNUT_LOG_ERROR,
                        "Topic %s: cannot read: %d", sub->topic->name, ret);
          pnut_loop_unwatch(loop, sub->fd);
          sub->failed = true;
          break;
        }

      if (ret == 0)
        {
          break;
        }

      sub->handler(sub, sub->msg, sub->arg);
    }

  sub->dispatching = false;
  if (sub->closing)
    {
      topic_free(sub);
    }
}

#endif /* !TOPIC_NONE */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int pnut_topic_advertise(FAR struct pnut_loop_s *loop,
                         FAR const struct pnut_topic_s *topic,
                         FAR struct pnut_publisher_s **pubp)
{
#ifdef TOPIC_NONE
  *pubp = NULL;
  return -ENOSYS;
#else
  FAR struct pnut_publisher_s *pub;
  int ret = OK;
#  if defined(__NuttX__)
  int instance = 0;
#  endif

  *pubp = NULL;

  pub = calloc(1, sizeof(*pub));
  if (pub == NULL)
    {
      return -ENOMEM;
    }

  pub->topic = topic;

#  if defined(__NuttX__)
  /* Persistent: a reader who subscribes later gets the latest message */

  pub->fd = topic->native != NULL ?
            orb_advertise_multi_queue_persist(topic->native, NULL,
                                              &instance, topic->depth) : -1;
  if (pub->fd < 0)
    {
      ret = topic->native != NULL ? -errno : -EINVAL;
    }
#  else
  ret = topic_dir(loop, topic, pub->dir);
  if (ret >= 0)
    {
      pub->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
      ret = pub->fd < 0 ? -errno : OK;
    }
#  endif

  if (ret < 0)
    {
      free(pub);
      return ret;
    }

  *pubp = pub;
  return OK;
#endif
}

void pnut_topic_unadvertise(FAR struct pnut_publisher_s *pub)
{
#ifndef TOPIC_NONE
  if (pub != NULL)
    {
#  if defined(__NuttX__)
      orb_unadvertise(pub->fd);
#  else
      close(pub->fd);
#  endif
      free(pub);
    }
#endif
}

int pnut_topic_publish(FAR struct pnut_publisher_s *pub,
                       FAR const void *msg)
{
#ifdef TOPIC_NONE
  return -ENOSYS;
#elif defined(__NuttX__)
  ssize_t n;

  n = orb_publish_multi(pub->fd, msg, pub->topic->size);
  if (n < 0)
    {
      return -errno;
    }

  return n == pub->topic->size ? OK : -EIO;
#else
  FAR struct dirent *entry;
  struct sockaddr_un addr;
  char path[TOPIC_PATH_MAX];
  char temp[TOPIC_PATH_MAX];
  FAR DIR *dir;
  int fd;

  /* The latest, for readers to come: written beside, and put in place */

  if (topic_path(path, pub->dir, TOPIC_LAST) < 0 ||
      topic_path(temp, pub->dir, TOPIC_LAST ".new") < 0)
    {
      return -ENAMETOOLONG;
    }

  fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0)
    {
      return -errno;
    }

  if (write(fd, msg, pub->topic->size) != pub->topic->size)
    {
      close(fd);
      unlink(temp);
      return -EIO;
    }

  close(fd);
  if (rename(temp, path) < 0)
    {
      return -errno;
    }

  /* Then to every reader.  One whose queue is full misses it; one gone
   * without unsubscribing leaves its socket, which is removed.
   */

  dir = opendir(pub->dir);
  if (dir == NULL)
    {
      return -errno;
    }

  while ((entry = readdir(dir)) != NULL)
    {
      if (strncmp(entry->d_name, TOPIC_READER, strlen(TOPIC_READER)) != 0)
        {
          continue;
        }

      if (topic_path(path, pub->dir, entry->d_name) < 0)
        {
          continue;
        }

      if (sendto(pub->fd, msg, pub->topic->size, MSG_DONTWAIT,
                 (FAR struct sockaddr *)&addr,
                 topic_address(&addr, path)) < 0 &&
          (errno == ECONNREFUSED || errno == ENOENT))
        {
          unlink(path);
        }
    }

  closedir(dir);
  return OK;
#endif
}

int pnut_topic_subscribe(FAR struct pnut_loop_s *loop,
                         FAR const struct pnut_topic_s *topic,
                         pnut_topic_handler_t handler, FAR void *arg,
                         FAR struct pnut_subscriber_s **subp)
{
#ifdef TOPIC_NONE
  *subp = NULL;
  return -ENOSYS;
#else
  FAR struct pnut_subscriber_s *sub;
  int ret;
#  ifndef __NuttX__
  struct sockaddr_un addr;
  char dir[TOPIC_PATH_MAX];
  char last[TOPIC_PATH_MAX];
  ssize_t n;
  int fd;
#  endif

  *subp = NULL;

  sub = calloc(1, TOPIC_MSG_OFFSET + topic->size);
  if (sub == NULL)
    {
      return -ENOMEM;
    }

  sub->loop    = loop;
  sub->topic   = topic;
  sub->handler = handler;
  sub->arg     = arg;
  sub->msg     = (FAR uint8_t *)sub + TOPIC_MSG_OFFSET;
  sub->fd      = -1;

#  if defined(__NuttX__)
  if (topic->native == NULL)
    {
      free(sub);
      return -EINVAL;
    }

  /* A topic advertised as persistent gives the latest message at once */

  sub->fd = orb_subscribe_multi(topic->native, 0);
  if (sub->fd < 0)
    {
      ret = -errno;
      free(sub);
      return ret;
    }
#  else
  ret = topic_dir(loop, topic, dir);
  if (ret < 0)
    {
      free(sub);
      return ret;
    }

  sub->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (sub->fd < 0)
    {
      ret = -errno;
      free(sub);
      return ret;
    }

  snprintf(last, sizeof(last), TOPIC_READER "%d.%d", (int)getpid(),
           sub->fd);
  if (topic_path(sub->path, dir, last) < 0)
    {
      close(sub->fd);
      free(sub);
      return -ENAMETOOLONG;
    }

  unlink(sub->path);
  if (bind(sub->fd, (FAR struct sockaddr *)&addr,
           topic_address(&addr, sub->path)) < 0)
    {
      ret = -errno;
      close(sub->fd);
      free(sub);
      return ret;
    }

  /* The latest message, sent to itself, so that it arrives on the loop as
   * the others do
   */

  fd = topic_path(last, dir, TOPIC_LAST) < 0 ? -1 :
       open(last, O_RDONLY | O_CLOEXEC);
  if (fd >= 0)
    {
      n = read(fd, sub->msg, topic->size);
      close(fd);
      if (n == topic->size)
        {
          sendto(sub->fd, sub->msg, topic->size, MSG_DONTWAIT,
                 (FAR struct sockaddr *)&addr,
                 topic_address(&addr, sub->path));
        }
    }
#  endif

  ret = pnut_loop_watch(loop, sub->fd, EPOLLIN, topic_readable, sub);
  if (ret < 0)
    {
      topic_free(sub);
      return ret;
    }

  *subp = sub;
  return OK;
#endif
}

void pnut_topic_unsubscribe(FAR struct pnut_subscriber_s *sub)
{
#ifndef TOPIC_NONE
  if (sub == NULL)
    {
      return;
    }

  if (sub->dispatching)
    {
      sub->closing = true;
      return;
    }

  topic_free(sub);
#endif
}
