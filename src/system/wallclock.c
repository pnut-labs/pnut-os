/****************************************************************************
 * pnut-os/src/system/wallclock.c
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
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/service.h>
#include <pnut/tz.h>

#include "wallclock.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The state file's first word, and its version */

#define WALLCLOCK_MAGIC    "pnut-time"
#define WALLCLOCK_VERSION  1

/* The zone when the setting names none the table has */

#define WALLCLOCK_UTC      "UTC"
#define WALLCLOCK_UTC_RULE "UTC0"

/* The times taken, from a source, the owner or the state file: from 1
 * January 2020 to 1 January 2100
 */

#define WALLCLOCK_EARLIEST 1577836800
#define WALLCLOCK_LATEST   4102444800

/* The furthest a network's offset may be from UTC, in seconds */

#define WALLCLOCK_OFFSET_MAX (18 * 3600)

/* An unchanged reading writes the state file this often, in ms */

#define WALLCLOCK_SAVE_EVERY 3600000

/* How fast the clock may drift between readings, as a fraction of the
 * time since the last: the sleep clock gains about 0.85 % of the time
 * slept (RFC 0028); 1/64 is about 1.6 %
 */

#define WALLCLOCK_DRIFT    64

/* A reading older than this, in milliseconds, is left aside: a topic
 * gives a new reader its last message, which may be hours old
 */

#define WALLCLOCK_STALE    60000

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void wallclock_get(FAR struct pnut_time_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_time_get_request_t *in,
                          FAR void *arg);
static void wallclock_set(FAR struct pnut_time_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_time_set_request_t *in,
                          FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct pnut_time_handlers_s g_wallclock_handlers =
{
  .get = wallclock_get,
  .set = wallclock_set,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: wallclock_now
 ****************************************************************************/

static int wallclock_gettime(FAR struct wallclock_s *wall, clockid_t id,
                             FAR struct timespec *ts)
{
  if (wall->config.gettime != NULL)
    {
      return wall->config.gettime(wall->config.clock, id, ts);
    }

  return clock_gettime(id, ts) < 0 ? -errno : OK;
}

static int wallclock_now(FAR struct wallclock_s *wall,
                         FAR struct timespec *ts)
{
  return wallclock_gettime(wall, CLOCK_REALTIME, ts);
}

/****************************************************************************
 * Name: wallclock_age
 *
 * Description:
 *   A reading's age, in milliseconds, from when it was taken: -EINVAL for
 *   one taken after now, which is not of this boot.
 *
 ****************************************************************************/

static int wallclock_age(FAR struct wallclock_s *wall, uint64_t taken,
                         FAR uint64_t *age)
{
  struct timespec ts;
  uint64_t now;
  int ret;

  ret = wallclock_gettime(wall, CLOCK_MONOTONIC, &ts);
  if (ret < 0)
    {
      return ret;
    }

  now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
  if (taken > now)
    {
      return -EINVAL;
    }

  *age = now - taken;
  return OK;
}

/****************************************************************************
 * Name: wallclock_settime
 ****************************************************************************/

static int wallclock_settime(FAR struct wallclock_s *wall, int64_t utc)
{
  struct timespec ts;

  ts.tv_sec  = (time_t)utc;
  ts.tv_nsec = 0;

  if (wall->config.settime != NULL)
    {
      return wall->config.settime(wall->config.clock, &ts);
    }

  return clock_settime(CLOCK_REALTIME, &ts) < 0 ? -errno : OK;
}

/****************************************************************************
 * Name: wallclock_fixed_rule
 *
 * Description:
 *   A POSIX TZ rule for a fixed offset east of UTC, named as the tz
 *   database names one: <+0545>-5:45, <-03>3.
 *
 ****************************************************************************/

static void wallclock_fixed_rule(int32_t offset, FAR char *rule,
                                 size_t size)
{
  int32_t east = offset < 0 ? -offset : offset;
  int hours = (int)(east / 3600);
  int minutes = (int)(east % 3600 / 60);
  char sign = offset < 0 ? '-' : '+';

  /* POSIX counts west of UTC: east is negative */

  if (minutes != 0)
    {
      snprintf(rule, size, "<%c%02d%02d>%s%d:%02d", sign, hours, minutes,
               offset > 0 ? "-" : "", hours, minutes);
    }
  else
    {
      snprintf(rule, size, "<%c%02d>%s%d", sign, hours,
               offset > 0 ? "-" : "", hours);
    }
}

/****************************************************************************
 * Name: wallclock_offset
 *
 * Description:
 *   The offset east of UTC, and summer time, at an instant.
 *
 ****************************************************************************/

static void wallclock_offset(FAR struct wallclock_s *wall, int64_t utc,
                             FAR int32_t *offset, FAR bool *dst)
{
  struct tm tm;

  if (wall->from_network)
    {
      *offset = wall->net_offset;
      *dst    = wall->net_dst;
      return;
    }

  pnut_tz_local(&wall->tz, (time_t)utc, &tm, offset);
  *dst = tm.tm_isdst > 0;
}

/****************************************************************************
 * Name: wallclock_publish
 *
 * Description:
 *   Announce the zone, and a step of the clock, on the time topic.
 *
 ****************************************************************************/

static void wallclock_publish(FAR struct wallclock_s *wall, int64_t step)
{
  pnut_time_change_t msg;
  struct timespec ts;
  int ret;

  if (wall->changes == NULL)
    {
      return;
    }

  memset(&msg, 0, sizeof(msg));
  ts.tv_sec = 0;
  wallclock_now(wall, &ts);

  strlcpy(msg.zone, wall->zone, sizeof(msg.zone));
  strlcpy(msg.rule, wall->rule, sizeof(msg.rule));
  wallclock_offset(wall, ts.tv_sec, &msg.offset, &msg.dst);
  msg.from_network = wall->from_network;
  msg.step         = step;
  msg.source       = (pnut_time_info_source_t)wall->source;
  msg.seq          = ++wall->seq;

  ret = pnut_time_change_publish(wall->changes, &msg);
  if (ret < 0)
    {
      pnut_log(&wall->module, PNUT_LOG_WARNING, "Cannot publish: %d", ret);
    }
}

/****************************************************************************
 * Name: wallclock_zone
 *
 * Description:
 *   Work out the zone in use, from the settings and the network's offset,
 *   and announce it if it changed.
 *
 ****************************************************************************/

static void wallclock_zone(FAR struct wallclock_s *wall)
{
  FAR const pnut_time_settings_t *values = &wall->settings.values;
  FAR const char *name = values->zone;
  FAR const char *rule;
  char fixed[sizeof(wall->rule)];
  bool from_network;
  int ret;

  rule = pnut_tz_find(name);
  if (rule == NULL)
    {
      if (name[0] != '\0' && strcmp(name, wall->named) != 0)
        {
          pnut_log(&wall->module, PNUT_LOG_WARNING,
                   "No zone %s: UTC instead", name);
        }

      name = WALLCLOCK_UTC;
      rule = WALLCLOCK_UTC_RULE;
    }

  strlcpy(wall->named, values->zone, sizeof(wall->named));

  /* Following the network, the zone named stays, for its name and for
   * when the network has given no offset yet
   */

  from_network = values->zone_auto && wall->has_offset;
  if (from_network)
    {
      wallclock_fixed_rule(wall->net_offset, fixed, sizeof(fixed));
      rule = fixed;
    }

  if (strcmp(name, wall->zone) == 0 && strcmp(rule, wall->rule) == 0 &&
      from_network == wall->from_network)
    {
      return;
    }

  ret = pnut_tz_parse(rule, &wall->tz);
  if (ret < 0)
    {
      /* The table's rules all parse (its tests check), and so do fixed
       * ones
       */

      pnut_log(&wall->module, PNUT_LOG_ERROR, "%s: rule %s: %d", name,
               rule, ret);
      pnut_tz_parse(WALLCLOCK_UTC_RULE, &wall->tz);
      name = WALLCLOCK_UTC;
      rule = WALLCLOCK_UTC_RULE;
      from_network = false;
    }

  strlcpy(wall->zone, name, sizeof(wall->zone));
  strlcpy(wall->rule, rule, sizeof(wall->rule));
  wall->from_network = from_network;

  pnut_log(&wall->module, PNUT_LOG_INFO, "Zone %s, %s%s", wall->zone,
           wall->rule, from_network ? " (the network's)" : "");
  wallclock_publish(wall, 0);
}

/****************************************************************************
 * Name: wallclock_save
 *
 * Description:
 *   Write the state file: the clock's time, and where it came from, so
 *   that after a power loss the clock starts near it.  Its few bytes are
 *   written on the loop, with a change of the clock or its source, at
 *   most hourly for readings that change nothing, and as the module
 *   stops.  A clock never set has nothing worth keeping.  A crash leaves
 *   the old file or the new one.
 *
 ****************************************************************************/

static void wallclock_save(FAR struct wallclock_s *wall)
{
  FAR const char *path = wall->config.state;
  char temp[PATH_MAX];
  char line[96];
  struct timespec ts;
  struct stat st;
  ssize_t n;
  int len;
  int ret = OK;
  int fd;

  if (path == NULL || wall->source == PNUT_TIME_INFO_SOURCE_NONE ||
      wallclock_now(wall, &ts) < 0)
    {
      return;
    }

  len = snprintf(line, sizeof(line), "%s %d %" PRId64 " %u %" PRId64 "\n",
                 WALLCLOCK_MAGIC, WALLCLOCK_VERSION, (int64_t)ts.tv_sec,
                 (unsigned int)wall->source, wall->since);
  if (snprintf(temp, sizeof(temp), "%s.new", path) >= (int)sizeof(temp))
    {
      return;
    }

  /* Made afresh, never through a link left at its name */

  unlink(temp);
  fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
            0600);
  if (fd < 0)
    {
      ret = -errno;
    }
  else
    {
      n = write(fd, line, len);
      if (n != len)
        {
          ret = n < 0 ? -errno : -EIO;
        }
      else if (fsync(fd) < 0 && errno != EINVAL && errno != ENOSYS)
        {
          ret = -errno;
        }

      close(fd);
    }

  /* NuttX moves a file into a directory found at the name it is renamed
   * to
   */

  if (ret == OK && lstat(path, &st) == 0 && !S_ISREG(st.st_mode))
    {
      ret = -EISDIR;
    }

  if (ret == OK && rename(temp, path) < 0)
    {
      ret = -errno;
    }

  if (ret < 0)
    {
      unlink(temp);
      pnut_log(&wall->module, PNUT_LOG_WARNING, "Cannot write %s: %d", path,
               ret);
      return;
    }

  wall->saved = wallclock_gettime(wall, CLOCK_MONOTONIC, &ts) == OK;
  wall->saved_at = (uint64_t)ts.tv_sec * 1000 +
                   (uint64_t)ts.tv_nsec / 1000000;
}

/****************************************************************************
 * Name: wallclock_load
 *
 * Description:
 *   Read the state file as the module starts.  A clock behind its time
 *   lost power, or is on a board that keeps no time across a reset: it is
 *   put at that time, not confirmed.  One that kept counting, as when only
 *   the system program restarted or the board's RTC kept the time across
 *   a reset (the T-Deck Max's does), keeps where its time came from.
 *
 ****************************************************************************/

static void wallclock_load(FAR struct wallclock_s *wall)
{
  FAR const char *path = wall->config.state;
  char temp[PATH_MAX];
  char magic[16];
  char line[96];
  struct timespec ts;
  int64_t utc;
  int64_t since;
  unsigned int source;
  int version;
  ssize_t n;
  int ret;
  int fd;

  if (path == NULL)
    {
      return;
    }

  /* NuttX removes the old file before renaming the new one over it: a
   * crash between the two leaves the new one under its temporary name
   */

  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0 && errno == ENOENT &&
      snprintf(temp, sizeof(temp), "%s.new", path) < (int)sizeof(temp))
    {
      fd = open(temp, O_RDONLY | O_CLOEXEC);
    }

  if (fd < 0)
    {
      if (errno != ENOENT)
        {
          pnut_log(&wall->module, PNUT_LOG_WARNING, "Cannot read %s: %d",
                   path, -errno);
        }

      return;
    }

  n = read(fd, line, sizeof(line) - 1);
  close(fd);
  if (n < 0)
    {
      pnut_log(&wall->module, PNUT_LOG_WARNING, "Cannot read %s: %d", path,
               -errno);
      return;
    }

  line[n] = '\0';
  if (sscanf(line, "%15s %d %" SCNd64 " %u %" SCNd64, magic, &version,
             &utc, &source, &since) != 5 ||
      strcmp(magic, WALLCLOCK_MAGIC) != 0 ||
      version != WALLCLOCK_VERSION ||
      source == PNUT_TIME_INFO_SOURCE_NONE ||
      source > PNUT_TIME_INFO_SOURCE_GNSS ||
      utc < WALLCLOCK_EARLIEST || utc > WALLCLOCK_LATEST ||
      since < 0 || since > utc)
    {
      pnut_log(&wall->module, PNUT_LOG_WARNING, "%s does not parse", path);
      return;
    }

  ret = wallclock_now(wall, &ts);
  if (ret < 0)
    {
      return;
    }

  if ((int64_t)ts.tv_sec >= utc)
    {
      wall->source = (uint8_t)source;
      wall->since  = since;
      return;
    }

  ret = wallclock_settime(wall, utc);
  if (ret < 0)
    {
      pnut_log(&wall->module, PNUT_LOG_ERROR,
               "Cannot set the clock: %d", ret);
      return;
    }

  wall->source = PNUT_TIME_INFO_SOURCE_SAVED;
  wall->since  = utc;
  pnut_log(&wall->module, PNUT_LOG_INFO,
           "The clock was behind its last time: put at %" PRId64, utc);
}

/****************************************************************************
 * Name: wallclock_saved_lately
 *
 * Description:
 *   Whether the state file was written within the last hour.
 *
 ****************************************************************************/

static bool wallclock_saved_lately(FAR struct wallclock_s *wall)
{
  uint64_t age;

  return wall->saved && wallclock_age(wall, wall->saved_at, &age) == OK &&
         age < WALLCLOCK_SAVE_EVERY;
}

/****************************************************************************
 * Name: wallclock_step
 *
 * Description:
 *   Set the clock to a time from a source, announce a step or a new
 *   source, and write the state file.
 *
 ****************************************************************************/

static int wallclock_step(FAR struct wallclock_s *wall, int64_t utc,
                          int64_t diff, uint8_t source)
{
  bool changed = diff != 0 || source != wall->source;
  int ret;

  if (diff != 0)
    {
      ret = wallclock_settime(wall, utc);
      if (ret < 0)
        {
          pnut_log(&wall->module, PNUT_LOG_ERROR,
                   "Cannot set the clock: %d", ret);
          return ret;
        }

      pnut_log(&wall->module, PNUT_LOG_INFO,
               "The clock moved %" PRId64 " s (source %u)", diff,
               (unsigned int)source);
    }

  wall->source      = source;
  wall->since       = utc;
  wall->held.source = 0;

  if (changed)
    {
      wallclock_publish(wall, diff);
    }

  if (changed || !wallclock_saved_lately(wall))
    {
      wallclock_save(wall);
    }

  return OK;
}

/****************************************************************************
 * Name: wallclock_take
 *
 * Description:
 *   A reading from a source.  The first after the clock was set otherwise
 *   (by the owner, or from the state file, or not at all) is taken as it
 *   is.  After that, one further from the clock than the drift since the
 *   last reading allows is held until a reading from another source
 *   agrees with it: a network, or a false base station, may give a wrong
 *   time (RFC 0028), and its own readings agreeing prove nothing.  With
 *   one source only, the owner's own time lets its next reading in.
 *
 ****************************************************************************/

static void wallclock_take(FAR struct wallclock_s *wall, int64_t utc,
                           uint8_t source)
{
  struct timespec ts;
  int64_t allowed;
  int64_t diff;
  bool confirmed;

  if (wallclock_now(wall, &ts) < 0)
    {
      return;
    }

  diff = utc - (int64_t)ts.tv_sec;
  confirmed = wall->source >= PNUT_TIME_INFO_SOURCE_NETWORK;

  if (confirmed)
    {
      allowed = (int64_t)wall->config.tolerance;
      if ((int64_t)ts.tv_sec > wall->since)
        {
          allowed += ((int64_t)ts.tv_sec - wall->since) / WALLCLOCK_DRIFT;
        }

      if (diff > allowed || diff < -allowed)
        {
          if (wall->held.source == 0 || wall->held.source == source ||
              wall->held.diff - diff > (int64_t)wall->config.tolerance ||
              diff - wall->held.diff > (int64_t)wall->config.tolerance)
            {
              pnut_log(&wall->module, PNUT_LOG_WARNING,
                       "A reading %" PRId64 " s off (source %u): held",
                       diff, (unsigned int)source);
              wall->held.diff   = diff;
              wall->held.source = source;
              return;
            }
        }
    }

  wallclock_step(wall, utc, diff, source);
}

/****************************************************************************
 * Name: wallclock_reading
 *
 * Description:
 *   A source's reading, its age added; one out of range, or older than a
 *   minute, is left aside.
 *
 ****************************************************************************/

static void wallclock_reading(FAR struct wallclock_s *wall, int64_t utc,
                              uint64_t age, uint8_t source)
{
  if (utc == 0)
    {
      return;
    }

  if (utc < WALLCLOCK_EARLIEST || utc > WALLCLOCK_LATEST)
    {
      pnut_log(&wall->module, PNUT_LOG_DEBUG,
               "A reading at %" PRId64 " (source %u): left aside", utc,
               (unsigned int)source);
      return;
    }

  if (age > WALLCLOCK_STALE)
    {
      pnut_log(&wall->module, PNUT_LOG_DEBUG,
               "A reading %" PRIu64 " ms old (source %u): left aside", age,
               (unsigned int)source);
      return;
    }

  wallclock_take(wall, utc + (int64_t)((age + 500) / 1000), source);
}

/****************************************************************************
 * Name: wallclock_network
 *
 * Description:
 *   A reading of the network's time, with its offset if it gave one: the
 *   offset first, so that a reading that changes both announces the step
 *   in the new zone.  An old reading's time is left aside, its offset
 *   not: the network tells of each change of it.  One from another boot
 *   is left aside whole.
 *
 ****************************************************************************/

static void wallclock_network(FAR struct pnut_network_time_reader_s *reader,
                              FAR const pnut_network_time_t *msg,
                              FAR void *arg)
{
  FAR struct wallclock_s *wall = arg;
  uint64_t age;
  bool dst_only;

  if (wallclock_age(wall, msg->taken, &age) < 0)
    {
      pnut_log(&wall->module, PNUT_LOG_DEBUG,
               "A network reading from another boot: left aside");
      return;
    }

  if (msg->has_offset && (msg->offset > WALLCLOCK_OFFSET_MAX ||
                          msg->offset < -WALLCLOCK_OFFSET_MAX))
    {
      pnut_log(&wall->module, PNUT_LOG_WARNING,
               "The network's offset %" PRId32 " s: left aside",
               msg->offset);
    }
  else if (msg->has_offset &&
           (!wall->has_offset || msg->offset != wall->net_offset ||
            msg->dst != wall->net_dst))
    {
      dst_only = wall->has_offset && msg->offset == wall->net_offset;

      wall->has_offset = true;
      wall->net_offset = msg->offset;
      wall->net_dst    = msg->dst;

      /* The rule stays when only summer time's flag changes */

      if (!dst_only)
        {
          wallclock_zone(wall);
        }
      else if (wall->from_network)
        {
          wallclock_publish(wall, 0);
        }
    }

  wallclock_reading(wall, msg->utc, age, PNUT_TIME_INFO_SOURCE_NETWORK);
}

/****************************************************************************
 * Name: wallclock_gnss
 ****************************************************************************/

static void wallclock_gnss(FAR struct pnut_gnss_time_reader_s *reader,
                           FAR const pnut_gnss_time_t *msg, FAR void *arg)
{
  FAR struct wallclock_s *wall = arg;
  uint64_t age;

  if (wallclock_age(wall, msg->taken, &age) < 0)
    {
      pnut_log(&wall->module, PNUT_LOG_DEBUG,
               "A GNSS reading from another boot: left aside");
      return;
    }

  wallclock_reading(wall, msg->utc, age, PNUT_TIME_INFO_SOURCE_GNSS);
}

/****************************************************************************
 * Name: wallclock_changed
 *
 * Description:
 *   Time's settings changed, or were read.
 *
 ****************************************************************************/

static void wallclock_changed(FAR struct pnut_settings_cache_s *cache,
                              int index, FAR void *arg)
{
  wallclock_zone(arg);
}

/****************************************************************************
 * Name: wallclock_get
 ****************************************************************************/

static void wallclock_get(FAR struct pnut_time_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_time_get_request_t *in,
                          FAR void *arg)
{
  FAR struct wallclock_s *wall = arg;
  pnut_time_info_t info;
  struct timespec ts;
  int ret;

  ret = wallclock_now(wall, &ts);
  if (ret < 0)
    {
      pnut_time_fail(server, req, PNUT_STATUS_INTERNAL, 0, NULL);
      return;
    }

  memset(&info, 0, sizeof(info));
  info.utc = ts.tv_sec;
  strlcpy(info.zone, wall->zone, sizeof(info.zone));
  strlcpy(info.rule, wall->rule, sizeof(info.rule));
  wallclock_offset(wall, ts.tv_sec, &info.offset, &info.dst);
  info.from_network = wall->from_network;
  info.source       = (pnut_time_info_source_t)wall->source;
  info.since        = wall->since;

  pnut_time_get_reply(server, req, PNUT_STATUS_OK, &info);
}

/****************************************************************************
 * Name: wallclock_set
 *
 * Description:
 *   The owner's own time, from the system UI alone; a fallback, which the
 *   next reading from a source replaces.
 *
 ****************************************************************************/

static void wallclock_set(FAR struct pnut_time_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_time_set_request_t *in,
                          FAR void *arg)
{
  FAR struct wallclock_s *wall = arg;
  FAR const char *program;
  struct timespec ts;
  int ret;

  if (wall->config.who != NULL)
    {
      ret = wall->config.who(wall->config.identity, req->pid, &program);
      if (ret == -EAGAIN)
        {
          pnut_time_fail(server, req, PNUT_STATUS_UNAVAILABLE, 0, NULL);
          return;
        }

      if (ret < 0 || strcmp(program, "ui") != 0)
        {
          pnut_time_fail(server, req, PNUT_STATUS_DENIED, 0, NULL);
          return;
        }
    }

  if (in->utc < WALLCLOCK_EARLIEST || in->utc > WALLCLOCK_LATEST)
    {
      pnut_time_fail(server, req, PNUT_STATUS_INVALID, 0, NULL);
      return;
    }

  ret = wallclock_now(wall, &ts);
  if (ret >= 0)
    {
      ret = wallclock_step(wall, in->utc, in->utc - (int64_t)ts.tv_sec,
                           PNUT_TIME_INFO_SOURCE_OWNER);
    }

  if (ret < 0)
    {
      pnut_time_fail(server, req, PNUT_STATUS_INTERNAL,
                     PNUT_TIME_ERROR_CODE_REFUSED, NULL);
      return;
    }

  pnut_time_set_reply(server, req, PNUT_STATUS_OK, NULL);
}

/****************************************************************************
 * Name: wallclock_start
 ****************************************************************************/

static int wallclock_start(FAR struct pnut_module_s *module,
                           FAR struct pnut_loop_s *loop)
{
  FAR struct wallclock_s *wall = (FAR struct wallclock_s *)module;
  int ret;

  wallclock_load(wall);

  /* Changes are announced where there are topics; the time is served
   * without them
   */

  ret = pnut_time_change_advertise(loop, &wall->changes);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No time topic: %d", ret);
      wall->changes = NULL;
    }

  /* The settings hold their defaults until Settings' values are read; the
   * zone follows them from now on
   */

  ret = pnut_time_settings_open(module, wallclock_changed, wall,
                                &wall->settings);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No settings: %d", ret);
    }
  else
    {
      wall->settings_open = true;
    }

  wallclock_zone(wall);

  ret = pnut_network_time_subscribe(loop, wallclock_network, wall,
                                    &wall->network);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No network_time topic: %d", ret);
    }
  else
    {
      wall->network_open = true;
    }

  ret = pnut_gnss_time_subscribe(loop, wallclock_gnss, wall, &wall->gnss);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No gnss_time topic: %d", ret);
    }
  else
    {
      wall->gnss_open = true;
    }

  ret = pnut_time_serve(loop, WALLCLOCK_SERVICE, 0, &g_wallclock_handlers,
                        wall, &wall->server);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Cannot serve: %d", ret);
      module->stop(module, loop);
      return ret;
    }

  wall->serving = true;
  pnut_module_ready(module);
  return OK;
}

/****************************************************************************
 * Name: wallclock_stop
 ****************************************************************************/

static void wallclock_stop(FAR struct pnut_module_s *module,
                           FAR struct pnut_loop_s *loop)
{
  FAR struct wallclock_s *wall = (FAR struct wallclock_s *)module;

  wallclock_save(wall);

  if (wall->serving)
    {
      pnut_time_close(&wall->server);
      wall->serving = false;
    }

  if (wall->gnss_open)
    {
      pnut_gnss_time_unsubscribe(&wall->gnss);
      wall->gnss_open = false;
    }

  if (wall->network_open)
    {
      pnut_network_time_unsubscribe(&wall->network);
      wall->network_open = false;
    }

  if (wall->settings_open)
    {
      pnut_time_settings_close(&wall->settings);
      wall->settings_open = false;
    }

  pnut_topic_unadvertise(wall->changes);
  wall->changes = NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: wallclock_init
 ****************************************************************************/

void wallclock_init(FAR struct wallclock_s *wall,
                    FAR const struct wallclock_config_s *config)
{
  memset(wall, 0, sizeof(*wall));
  wall->config       = *config;
  wall->module.name  = "time";
  wall->module.start = wallclock_start;
  wall->module.stop  = wallclock_stop;
}
