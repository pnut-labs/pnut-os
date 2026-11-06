/****************************************************************************
 * pnut-os/src/system/logs.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>
#include <pnut/worker.h>

#include "logs.h"
#include "programs.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LOGS_FILE         "system.log"
#define LOGS_CRASH        "crash-"

/* The files' own lines, written by Log rather than copied: they start so,
 * which no line of the RAM log does
 */

#define LOGS_MARK         "---- "

/* A crash's dump: lines at the alert level, which NuttX's assertion and
 * its stack and task dumps write (_alert()), the emergency level alike;
 * the assertion's line, the dump's first, which a small RAM log may have
 * lost; and the line Log logs once it has saved the report, which tells
 * a later start that the dump before it is saved: the dump that ends at
 * the time it names, so that a second dump, come while the first's
 * report was written, is not taken as saved with it
 */

#define LOGS_ASSERTION    "Assertion failed"
#define LOGS_SAVED        "log: Crash report saved"
#define LOGS_SAVED_TO     " (to "

/* How long a crash's dump must be quiet to be over, in ms */

#define LOGS_QUIET_MS     1000

/* How far back a start looks in system.log for its last line copied */

#define LOGS_TAIL         2048

/* A drop in the lines' times, in microseconds, that marks a new start of
 * the device rather than lines of two tasks out of order, which differ by
 * a preemption at most; a start before that logged nothing after its
 * first half second is common
 */

#define LOGS_BOOT_DROP    50000

/* A path of Log's: its directory, and a file's name in it, the longest
 * a crash report's.  Small, as the writes run on a worker's stack;
 * logs_start() checks the directory fits.
 */

#define LOGS_PATH_MAX     128

/* Room past the lines waiting for Log's own marks: the count of lines
 * dropped, and the stop's
 */

#define LOGS_MARKS_ROOM   (2 * LOGS_LINE_MAX)

/* How long a stop waits for a write still running, in ms */

#define LOGS_STOP_WAIT_MS 2000

/* A read of the interface's: what it reads, and its bounds.  A page scans
 * that many bytes at most, so that a filter matching little does not hold
 * the worker long; it says there is more, and the reader asks again.
 */

#define LOGS_READ_LINES   1
#define LOGS_READ_CRASHES 2
#define LOGS_READ_CRASH   3
#define LOGS_READ_SCAN    16384
#define LOGS_STOP_STEP_MS 10

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* syslog's levels, as CONFIG_SYSLOG_PRIORITY names them */

enum logs_level_e
{
  LOGS_EMERG = 0,
  LOGS_ALERT,
  LOGS_CRIT,
  LOGS_ERROR,
  LOGS_WARN,
  LOGS_NOTICE,
  LOGS_INFO,
  LOGS_DEBUG,
};

/* A line of the RAM log, taken apart */

struct logs_line_s
{
  int64_t us;                     /* Its time since boot, us; -1 none */
  uint8_t level;                  /* enum logs_level_e */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void logs_mark(FAR struct logs_s *logs, FAR const char *fmt, ...)
  printf_like(2, 3);
static void logs_write_done(FAR struct pnut_loop_s *loop, FAR void *arg);
static void logs_read_start(FAR struct logs_s *logs);
static bool logs_scan(FAR const char **cursor, FAR const char *end,
                      FAR char *line, FAR size_t *lenp);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR const char * const g_logs_levels[] =
{
  "EMERG", "ALERT", "CRIT", "ERROR", "WARN", "NOTICE", "INFO", "DEBUG"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: logs_parse
 *
 * Description:
 *   Take a line's time and level from its start, as NuttX's syslog writes
 *   it: "[   12.345678] [CPU0] [  WARN] task: ...", the CPU there with
 *   several.  A line without them is information, of no time.
 *
 ****************************************************************************/

static void logs_parse(FAR const char *line, FAR struct logs_line_s *out)
{
  FAR const char *p = line;
  FAR char *end;
  long long sec;
  long usec;
  size_t n;
  uint8_t i;

  out->us    = -1;
  out->level = LOGS_INFO;

  if (*p != '[')
    {
      return;
    }

  sec = strtoll(p + 1, &end, 10);
  if (*end == '.')
    {
      usec = strtol(end + 1, &end, 10);
      if (*end == ']' && sec >= 0 && usec >= 0 && usec < 1000000)
        {
          out->us = (int64_t)sec * 1000000 + usec;
          p = end + 1;
        }
    }

  while (*p == ' ')
    {
      p++;
    }

  if (strncmp(p, "[CPU", 4) == 0)
    {
      p = strchr(p, ']');
      if (p == NULL)
        {
          return;
        }

      p++;
      while (*p == ' ')
        {
          p++;
        }
    }

  if (*p != '[')
    {
      return;
    }

  p++;
  while (*p == ' ')
    {
      p++;
    }

  for (i = 0; i < sizeof(g_logs_levels) / sizeof(g_logs_levels[0]); i++)
    {
      n = strlen(g_logs_levels[i]);
      if (strncmp(p, g_logs_levels[i], n) == 0 && p[n] == ']')
        {
          out->level = i;
          return;
        }
    }
}

/****************************************************************************
 * Name: logs_dump
 *
 * Description:
 *   Whether a line is a crash's dump's.  _alert() writes NuttX's
 *   assertion, its stack and task dumps, and the address and undefined
 *   behaviour sanitizers' reports; little else in the tree uses it, and a
 *   line that does makes a crash report too.
 *
 ****************************************************************************/

static bool logs_dump(FAR const struct logs_line_s *parsed)
{
  return parsed->level <= LOGS_ALERT;
}

/****************************************************************************
 * Name: logs_kept
 *
 * Description:
 *   Whether system.log keeps a line: warnings and errors.  Of a crash's
 *   dump only its assertion line: the dump is the crash report's.
 *
 ****************************************************************************/

static bool logs_kept(FAR const char *line,
                      FAR const struct logs_line_s *parsed)
{
  if (logs_dump(parsed))
    {
      return strstr(line, LOGS_ASSERTION) != NULL;
    }

  return parsed->level <= LOGS_WARN;
}

/****************************************************************************
 * Name: logs_now
 ****************************************************************************/

static void logs_now(FAR struct logs_s *logs, FAR struct timespec *ts)
{
  int ret;

  if (logs->config.gettime != NULL)
    {
      ret = logs->config.gettime(logs->config.clock, ts);
    }
  else
    {
      ret = clock_gettime(CLOCK_REALTIME, ts) < 0 ? -errno : OK;
    }

  if (ret < 0)
    {
      ts->tv_sec  = 0;
      ts->tv_nsec = 0;
    }
}

/****************************************************************************
 * Name: logs_add
 *
 * Description:
 *   Add a line to those waiting, with its newline.  The RAM log's lines
 *   take up to twice the batch; one that finds no room is dropped, and
 *   counted.  Log's own marks have LOGS_MARKS_ROOM more, so that the
 *   count, and the stop's mark, are not dropped in turn.
 *
 ****************************************************************************/

static void logs_add(FAR struct logs_s *logs, FAR const char *line,
                     size_t len, bool mark)
{
  size_t room = 2 * (size_t)logs->config.batch;

  if (mark)
    {
      room += LOGS_MARKS_ROOM;
    }

  if (logs->pending == NULL || logs->len + len + 1 > room)
    {
      if (logs->dropped < UINT32_MAX)
        {
          logs->dropped++;
        }

      return;
    }

  memcpy(logs->pending + logs->len, line, len);
  logs->len += len;
  logs->pending[logs->len++] = '\n';
}

/* One of Log's own lines, a mark, into those waiting */

static void logs_mark(FAR struct logs_s *logs, FAR const char *fmt, ...)
{
  char line[LOGS_LINE_MAX];
  va_list ap;
  int len;

  memcpy(line, LOGS_MARK, sizeof(LOGS_MARK) - 1);
  va_start(ap, fmt);
  len = vsnprintf(line + sizeof(LOGS_MARK) - 1,
                  sizeof(line) - sizeof(LOGS_MARK) + 1, fmt, ap);
  va_end(ap);

  if (len < 0)
    {
      return;
    }

  len += sizeof(LOGS_MARK) - 1;
  if (len >= (int)sizeof(line))
    {
      len = sizeof(line) - 1;
    }

  logs_add(logs, line, len, true);
}

/* The start's mark: the time, and the firmware */

static void logs_mark_start(FAR struct logs_s *logs)
{
  struct utsname name;
  struct timespec ts;
  struct tm tm;
  char when[24];

  logs_now(logs, &ts);
  gmtime_r(&ts.tv_sec, &tm);
  strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);

  if (uname(&name) < 0)
    {
      logs_mark(logs, "start %s ----", when);
      return;
    }

  logs_mark(logs, "start %s, %s %s %s ----", when, name.sysname,
            name.release, name.version);
}

/****************************************************************************
 * Name: logs_lines_out
 *
 * Description:
 *   Write a whole buffer to a descriptor.
 *
 ****************************************************************************/

static int logs_lines_out(int fd, FAR const char *data, size_t len)
{
  ssize_t n;

  while (len > 0)
    {
      n = write(fd, data, len);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          return n < 0 ? -errno : -EIO;
        }

      data += n;
      len  -= n;
    }

  return OK;
}

/****************************************************************************
 * Name: logs_rotate
 *
 * Description:
 *   Move system.log up one: the oldest file goes, each other moves up,
 *   and system.log becomes system.log.1.  On littlefs a rename only
 *   changes metadata, at once: a reset in between leaves whole files.
 *
 ****************************************************************************/

static int logs_rotate(FAR const char *dir, uint8_t files)
{
  char from[LOGS_PATH_MAX];
  char to[LOGS_PATH_MAX];
  uint8_t i;

  if (files > 1)
    {
      snprintf(to, sizeof(to), "%s/%s.%u", dir, LOGS_FILE, files - 1u);
    }
  else
    {
      snprintf(to, sizeof(to), "%s/%s", dir, LOGS_FILE);
    }

  if (unlink(to) < 0 && errno != ENOENT)
    {
      return -errno;
    }

  for (i = files - 1; i >= 1; i--)
    {
      if (i > 1)
        {
          snprintf(from, sizeof(from), "%s/%s.%u", dir, LOGS_FILE, i - 1u);
        }
      else
        {
          snprintf(from, sizeof(from), "%s/%s", dir, LOGS_FILE);
        }

      snprintf(to, sizeof(to), "%s/%s.%u", dir, LOGS_FILE, (unsigned)i);
      if (rename(from, to) < 0 && errno != ENOENT)
        {
          return -errno;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: logs_append
 *
 * Description:
 *   Append lines to system.log, a file at most file_size bytes: whole
 *   lines go in while they fit, then the files rotate.  A line longer
 *   than a file would be left out; none is, as logs_init() makes a file
 *   hold two of the longest.
 *
 ****************************************************************************/

static int logs_append(FAR const char *dir, uint32_t file_size,
                       uint8_t files, FAR const char *data, size_t len,
                       FAR uint32_t *rotated)
{
  char path[LOGS_PATH_MAX];
  struct stat st;
  size_t room;
  size_t take;
  size_t line;
  int ret;
  int fd;

  if (snprintf(path, sizeof(path), "%s/%s", dir, LOGS_FILE) >=
      (int)sizeof(path))
    {
      return -ENAMETOOLONG;
    }

  while (len > 0)
    {
      if (stat(path, &st) < 0)
        {
          if (errno != ENOENT)
            {
              return -errno;
            }

          st.st_size = 0;
        }

      room = (size_t)st.st_size < file_size ?
             file_size - (size_t)st.st_size : 0;

      /* As many whole lines as fit */

      for (take = 0; take < len; take += line)
        {
          FAR const char *nl = memchr(data + take, '\n', len - take);

          line = nl != NULL ? (size_t)(nl - (data + take)) + 1 : len - take;
          if (take + line > room)
            {
              break;
            }
        }

      if (take == 0)
        {
          if (st.st_size == 0)
            {
              /* A line longer than a file: none is, as logs_init() makes
               * a file hold the longest line
               */

              take = line;
              data += take;
              len  -= take;
              continue;
            }

          ret = logs_rotate(dir, files);
          if (ret < 0)
            {
              return ret;
            }

          (*rotated)++;
          continue;
        }

      fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC,
                0600);
      if (fd < 0)
        {
          return -errno;
        }

      ret = logs_lines_out(fd, data, take);
      if (close(fd) < 0 && ret == OK)
        {
          ret = -errno;
        }

      if (ret < 0)
        {
          return ret;
        }

      data += take;
      len  -= take;
    }

  return OK;
}

/****************************************************************************
 * Name: logs_name_cmp
 ****************************************************************************/

static int logs_name_cmp(FAR const void *a, FAR const void *b)
{
  return strcmp(*(FAR char * const *)a, *(FAR char * const *)b);
}

/****************************************************************************
 * Name: logs_crash_trim
 *
 * Description:
 *   Keep the newest crash reports, as many as crashes says, the oldest
 *   deleted first: their names sort by their time.
 *
 ****************************************************************************/

static void logs_crash_trim(FAR const char *dir, uint8_t crashes)
{
  FAR struct dirent *entry;
  FAR char **names = NULL;
  FAR char **more;
  char path[LOGS_PATH_MAX];
  size_t count = 0;
  size_t i;
  FAR DIR *d;

  d = opendir(dir);
  if (d == NULL)
    {
      return;
    }

  while ((entry = readdir(d)) != NULL)
    {
      if (strncmp(entry->d_name, LOGS_CRASH, sizeof(LOGS_CRASH) - 1) != 0)
        {
          continue;
        }

      more = realloc(names, (count + 1) * sizeof(*names));
      if (more == NULL)
        {
          break;
        }

      names = more;
      names[count] = strdup(entry->d_name);
      if (names[count] == NULL)
        {
          break;
        }

      count++;
    }

  closedir(d);

  if (count > crashes)
    {
      qsort(names, count, sizeof(*names), logs_name_cmp);
      for (i = 0; i < count - crashes; i++)
        {
          snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
          unlink(path);
        }
    }

  for (i = 0; i < count; i++)
    {
      free(names[i]);
    }

  free(names);
}

/****************************************************************************
 * Name: logs_crash_save
 *
 * Description:
 *   Write a crash report, made afresh: a name taken already gets a number.
 *   Then trim the reports to the number kept.
 *
 ****************************************************************************/

static int logs_crash_save(FAR struct logs_write_s *write)
{
  char path[LOGS_PATH_MAX];
  char name[LOGS_CRASH_NAME];
  int ret = -EEXIST;
  int fd = -1;
  int i;

  for (i = 0; i < 10 && fd < 0; i++)
    {
      if (i == 0)
        {
          strlcpy(name, write->crash_name, sizeof(name));
        }
      else
        {
          /* One digit: the compiler then knows the name fits */

          snprintf(name, sizeof(name), "%.*s-%c",
                   (int)(sizeof(name) - 3), write->crash_name, '0' + i);
        }

      snprintf(path, sizeof(path), "%s/%s", write->dir, name);
      fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                0600);
      if (fd < 0 && errno != EEXIST)
        {
          return -errno;
        }
    }

  if (fd < 0)
    {
      return ret;
    }

  ret = logs_lines_out(fd, write->crash, write->crash_len);
  if (close(fd) < 0 && ret == OK)
    {
      ret = -errno;
    }

  if (ret < 0)
    {
      unlink(path);
      return ret;
    }

  strlcpy(write->crash_name, name, sizeof(write->crash_name));
  logs_crash_trim(write->dir, write->crashes);
  return OK;
}

/****************************************************************************
 * Name: logs_write_run
 *
 * Description:
 *   A write, on a worker.
 *
 ****************************************************************************/

static void logs_write_run(FAR void *arg)
{
  FAR struct logs_write_s *write = arg;

  write->ret     = OK;
  write->rotated = 0;
  if (write->len > 0)
    {
      write->ret = logs_append(write->dir, write->file_size, write->files,
                               write->data, write->len, &write->rotated);
    }

  write->crash_ret = OK;
  if (write->crash != NULL)
    {
      write->crash_ret = logs_crash_save(write);
    }

  __atomic_store_n(&write->finished, true, __ATOMIC_RELEASE);
}

/****************************************************************************
 * Name: logs_mark_dropped
 *
 * Description:
 *   The count of lines dropped since the last, as a mark after the lines
 *   waiting.
 *
 ****************************************************************************/

static void logs_mark_dropped(FAR struct logs_s *logs)
{
  uint32_t dropped = logs->dropped;

  if (dropped > 0)
    {
      logs->dropped = 0;
      logs_mark(logs, "%" PRIu32 " lines not kept: no room ----", dropped);
    }
}

/****************************************************************************
 * Name: logs_prepare
 *
 * Description:
 *   Hand the lines waiting, the count of lines dropped after them, and a
 *   crash report, to the write, and take the spare buffer for the lines
 *   to come.
 *
 ****************************************************************************/

static void logs_prepare(FAR struct logs_s *logs)
{
  FAR struct logs_write_s *write = &logs->write;
  FAR char *buffer;

  logs_mark_dropped(logs);

  write->dir       = logs->config.dir;
  write->file_size = logs->config.file_size;
  write->files     = logs->config.files;
  write->crashes   = logs->config.crashes;
  write->data      = logs->pending;
  write->len       = logs->len;
  write->crash     = logs->crash;
  write->crash_len = logs->crash_len;
  write->ret       = OK;
  write->crash_ret = OK;
  __atomic_store_n(&write->finished, false, __ATOMIC_RELAXED);
  strlcpy(write->crash_name, logs->crash_name, sizeof(write->crash_name));

  buffer        = logs->spare;
  logs->spare   = logs->pending;
  logs->pending = buffer;
  logs->len     = 0;
  logs->crash   = NULL;
}

/****************************************************************************
 * Name: logs_report_end
 *
 * Description:
 *   The time of a crash report's last line, its dump's end, in us since
 *   the device's start; -1 with none.
 *
 ****************************************************************************/

static int64_t logs_report_end(FAR const char *report, size_t len)
{
  struct logs_line_s parsed;
  char line[LOGS_LINE_MAX];
  FAR const char *cursor = report;
  int64_t us = -1;
  size_t n;

  while (logs_scan(&cursor, report + len, line, &n))
    {
      if (n > 0)
        {
          logs_parse(line, &parsed);
          us = parsed.us;
        }
    }

  return us;
}

/****************************************************************************
 * Name: logs_written
 *
 * Description:
 *   A write has finished: say what failed, once until a write works again,
 *   and that a crash report is saved.  The line saying so tells a later
 *   start that the dump before it is saved.
 *
 ****************************************************************************/

static void logs_written(FAR struct logs_s *logs)
{
  FAR struct logs_write_s *write = &logs->write;
  int64_t us;

  logs->rotations += write->rotated;
  write->rotated   = 0;

  if (write->ret < 0)
    {
      if (!logs->failing)
        {
          pnut_log(&logs->module, PNUT_LOG_ERROR,
                   "Cannot write %s/%s: %d; lines lost", write->dir,
                   LOGS_FILE, write->ret);
          logs->failing = true;
        }
    }
  else if (write->len > 0 && logs->failing)
    {
      pnut_log(&logs->module, PNUT_LOG_INFO, "Writing %s/%s again",
               write->dir, LOGS_FILE);
      logs->failing = false;
    }

  if (write->crash != NULL)
    {
      if (write->crash_ret < 0)
        {
          pnut_log(&logs->module, PNUT_LOG_ERROR,
                   "Cannot save a crash report: %d", write->crash_ret);
        }
      else
        {
          /* The dump it holds named by its last line's time (LOGS_SAVED) */

          us = logs_report_end(write->crash, write->crash_len);
          if (us >= 0)
            {
              pnut_log(&logs->module, PNUT_LOG_WARNING,
                       "Crash report saved: %s/%s" LOGS_SAVED_TO
                       "%lld.%06ld)", write->dir, write->crash_name,
                       (long long)(us / 1000000), (long)(us % 1000000));
            }
          else
            {
              pnut_log(&logs->module, PNUT_LOG_WARNING,
                       "Crash report saved: %s/%s", write->dir,
                       write->crash_name);
            }
        }

      free(write->crash);
      write->crash = NULL;
    }
}

/****************************************************************************
 * Name: logs_flush
 *
 * Description:
 *   Start a write of what is waiting, unless one runs.
 *
 ****************************************************************************/

static void logs_flush(FAR struct logs_s *logs)
{
  int ret;

  if (logs->writing || logs->reading ||
      (logs->len == 0 && logs->crash == NULL && logs->dropped == 0))
    {
      return;
    }

  if (logs->timer != NULL)
    {
      pnut_timer_cancel(logs->module.loop, logs->timer);
      logs->timer = NULL;
    }

  logs_prepare(logs);
  ret = pnut_job_submit(logs->module.loop, logs_write_run, logs_write_done,
                        &logs->write);
  if (ret < 0)
    {
      /* Written on the loop instead: rare, and better than lost */

      logs_write_run(&logs->write);
      logs_written(logs);
      return;
    }

  logs->writing = true;
}

static void logs_timer(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct logs_s *logs = arg;

  logs->timer = NULL;
  logs_flush(logs);
}

/* Write what waits at once once enough has gathered, after the delay
 * otherwise
 */

static void logs_schedule(FAR struct logs_s *logs)
{
  if (logs->writing)
    {
      return;
    }

  if (logs->len >= logs->config.batch || logs->crash != NULL)
    {
      logs_flush(logs);
      return;
    }

  if ((logs->len > 0 || logs->dropped > 0) && logs->timer == NULL &&
      pnut_timer_start(logs->module.loop, logs->config.delay, 0,
                       logs_timer, logs, &logs->timer) < 0)
    {
      logs->timer = NULL;
      logs_flush(logs);
    }
}

static void logs_write_done(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct logs_s *logs =
    (FAR struct logs_s *)((FAR char *)arg - offsetof(struct logs_s, write));

  logs->writing = false;
  logs_written(logs);

  /* A read that waited for it first */

  if (logs->read_waiting)
    {
      logs->read_waiting = false;
      logs_read_start(logs);
      return;
    }

  logs_schedule(logs);
}

/****************************************************************************
 * Name: logs_read_all
 *
 * Description:
 *   Read all the RAM log holds, from its oldest line: through fd, the
 *   module's as it starts, or through a descriptor of its own when fd is
 *   negative.  malloc()ed, NUL-terminated, or NULL with nothing.
 *
 ****************************************************************************/

static FAR char *logs_read_all(FAR const char *source, int fd,
                               FAR size_t *lenp)
{
  FAR char *buffer;
  size_t size;
  size_t len = 0;
  ssize_t n;
  bool own = fd < 0;
  int avail = 0;

  if (own)
    {
      fd = open(source, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (fd < 0)
        {
          return NULL;
        }
    }

  if (ioctl(fd, FIONREAD, (unsigned long)&avail) < 0 || avail <= 0)
    {
      if (own)
        {
          close(fd);
        }

      return NULL;
    }

  size   = (size_t)avail;
  buffer = malloc(size + 1);
  if (buffer == NULL)
    {
      if (own)
        {
          close(fd);
        }

      return NULL;
    }

  while (len < size)
    {
      n = read(fd, buffer + len, size - len);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          break;
        }

      len += n;
    }

  if (own)
    {
      close(fd);
    }

  buffer[len] = '\0';
  *lenp = len;
  return buffer;
}

/****************************************************************************
 * Name: logs_scan
 *
 * Description:
 *   The next whole line of a buffer read whole, copied into line (cut to
 *   LOGS_LINE_MAX, without its carriage return and newline): false at the
 *   end, or before a last line without its newline, still being written.
 *   The buffer is left as it is.
 *
 ****************************************************************************/

static bool logs_scan(FAR const char **cursor, FAR const char *end,
                      FAR char *line, FAR size_t *lenp)
{
  FAR const char *start = *cursor;
  FAR const char *nl;
  size_t len;

  if (start >= end)
    {
      return false;
    }

  nl = memchr(start, '\n', end - start);
  if (nl == NULL)
    {
      return false;
    }

  *cursor = nl + 1;

  len = nl - start;
  if (len > 0 && start[len - 1] == '\r')
    {
      len--;
    }

  if (len >= LOGS_LINE_MAX)
    {
      len = LOGS_LINE_MAX - 1;
    }

  memcpy(line, start, len);
  line[len] = '\0';
  *lenp = len;
  return true;
}

/****************************************************************************
 * Name: logs_last_saved
 *
 * Description:
 *   The last line system.log (or, just after a rotation, system.log.1)
 *   copied from the RAM log, from its end; empty with none.
 *
 ****************************************************************************/

static void logs_last_saved(FAR struct logs_s *logs, FAR char *last)
{
  char path[LOGS_PATH_MAX];
  char tail[LOGS_TAIL];
  char line[LOGS_LINE_MAX];
  FAR const char *cursor;
  FAR const char *nl;
  size_t len;
  off_t end;
  ssize_t n;
  int pass;
  int fd;

  last[0] = '\0';

  for (pass = 0; pass < 2 && last[0] == '\0'; pass++)
    {
      snprintf(path, sizeof(path), pass == 0 ? "%s/%s" : "%s/%s.1",
               logs->config.dir, LOGS_FILE);

      fd = open(path, O_RDONLY | O_CLOEXEC);
      if (fd < 0)
        {
          continue;
        }

      end = lseek(fd, 0, SEEK_END);
      n   = -1;
      if (end >= 0 &&
          lseek(fd, end > LOGS_TAIL ? end - LOGS_TAIL : 0, SEEK_SET) >= 0)
        {
          n = read(fd, tail, sizeof(tail));
        }

      close(fd);
      if (n <= 0)
        {
          continue;
        }

      /* A tail from the middle of the file starts with part of a line */

      cursor = tail;
      if (end > LOGS_TAIL)
        {
          nl = memchr(tail, '\n', n);
          cursor = nl != NULL ? nl + 1 : tail + n;
        }

      while (logs_scan(&cursor, tail + n, line, &len))
        {
          if (len > 0 &&
              strncmp(line, LOGS_MARK, sizeof(LOGS_MARK) - 1) != 0)
            {
              memcpy(last, line, len + 1);
            }
        }
    }
}

/****************************************************************************
 * Name: logs_crash_take
 *
 * Description:
 *   A crash report in the RAM log as read whole: everything up to the end
 *   of the last crash's dump (its last line at the alert level or above),
 *   if no line after the dump says it is saved already.  Its length, or 0
 *   with none.
 *
 *   A line saying a report is saved names the time of the dump's last line
 *   it saved, and covers that dump alone: a dump after it, come while the
 *   report was written, is still to save.  One that names no time, as
 *   older ones do, covers whatever dump is before it.
 *
 ****************************************************************************/

static size_t logs_crash_take(FAR const char *buffer, size_t len)
{
  struct logs_line_s parsed;
  char line[LOGS_LINE_MAX];
  FAR const char *cursor = buffer;
  FAR const char *dump_end = NULL;
  FAR const char *saved;
  FAR char *p;
  int64_t dump_us = -1;
  int64_t to;
  long long sec;
  long usec;
  size_t n;
  bool in_dump = false;

  while (logs_scan(&cursor, buffer + len, line, &n))
    {
      logs_parse(line, &parsed);
      if (logs_dump(&parsed))
        {
          in_dump  = true;
          dump_end = cursor;
          dump_us  = parsed.us;
          continue;
        }

      saved = in_dump ? strstr(line, LOGS_SAVED) : NULL;
      if (saved == NULL)
        {
          continue;
        }

      /* The time it names, "<s>.<us>)"; -1 with none */

      to    = -1;
      saved = strstr(saved, LOGS_SAVED_TO);
      if (saved != NULL)
        {
          sec = strtoll(saved + sizeof(LOGS_SAVED_TO) - 1, &p, 10);
          if (*p == '.')
            {
              usec = strtol(p + 1, &p, 10);
              if (*p == ')' && sec >= 0 && usec >= 0 && usec < 1000000)
                {
                  to = (int64_t)sec * 1000000 + usec;
                }
            }
        }

      if (to < 0 || to == dump_us)
        {
          in_dump  = false;
          dump_end = NULL;
        }
    }

  return dump_end != NULL ? (size_t)(dump_end - buffer) : 0;
}

/****************************************************************************
 * Name: logs_crash_from
 *
 * Description:
 *   Where a crash report that ends at end starts, in the RAM log read
 *   whole: its last crash_size bytes at most, from a whole line.
 *
 ****************************************************************************/

static size_t logs_crash_from(FAR struct logs_s *logs,
                              FAR const char *buffer, size_t end)
{
  FAR const char *nl;
  size_t start;

  if (end <= logs->config.crash_size)
    {
      return 0;
    }

  /* From the line after the cut; one that starts right at the cut, after
   * the newline before it, whole
   */

  start = end - logs->config.crash_size;
  nl    = memchr(buffer + start - 1, '\n', end - start + 1);
  return nl != NULL ? (size_t)(nl - buffer) + 1 : end;
}

/****************************************************************************
 * Name: logs_crash_known
 *
 * Description:
 *   Whether a report kept already ends as this one does, with the same
 *   last line, its time to the microsecond: the line saying it is saved
 *   may be missing from the RAM log, lost to a reset that did not write
 *   the data cache back.
 *
 ****************************************************************************/

static bool logs_crash_known(FAR struct logs_s *logs,
                             FAR const char *report, size_t len)
{
  char path[LOGS_PATH_MAX];
  char tail[LOGS_LINE_MAX + 2];
  FAR struct dirent *entry;
  FAR const char *last;
  FAR DIR *d;
  bool known = false;
  ssize_t got;
  off_t end;
  size_t n;
  int fd;

  if (len == 0)
    {
      return false;
    }

  /* The report's last line, with its newline */

  last = report + len - 1;
  while (last > report && *(last - 1) != '\n')
    {
      last--;
    }

  n = report + len - last;
  if (n > sizeof(tail))
    {
      last = report + len - sizeof(tail);
      n    = sizeof(tail);
    }

  d = opendir(logs->config.dir);
  if (d == NULL)
    {
      return false;
    }

  while (!known && (entry = readdir(d)) != NULL)
    {
      if (strncmp(entry->d_name, LOGS_CRASH, sizeof(LOGS_CRASH) - 1) != 0)
        {
          continue;
        }

      if (snprintf(path, sizeof(path), "%s/%s", logs->config.dir,
                   entry->d_name) >= (int)sizeof(path))
        {
          continue;
        }

      fd = open(path, O_RDONLY | O_CLOEXEC);
      if (fd < 0)
        {
          continue;
        }

      end = lseek(fd, 0, SEEK_END);
      if (end >= (off_t)n && lseek(fd, end - n, SEEK_SET) >= 0)
        {
          got   = read(fd, tail, n);
          known = got == (ssize_t)n && memcmp(tail, last, n) == 0;
        }

      close(fd);
    }

  closedir(d);
  return known;
}

/****************************************************************************
 * Name: logs_crash_name
 ****************************************************************************/

static void logs_crash_name(FAR struct logs_s *logs)
{
  struct timespec ts;
  struct tm tm;

  logs_now(logs, &ts);
  gmtime_r(&ts.tv_sec, &tm);
  strftime(logs->crash_name, sizeof(logs->crash_name),
           LOGS_CRASH "%Y%m%d-%H%M%S", &tm);
}

/****************************************************************************
 * Name: logs_crash_live
 *
 * Description:
 *   A crash's dump came in while running, and has been quiet a moment: a
 *   task's assertion, which ended the task, not the device.  Read the
 *   RAM log again for the report.
 *
 ****************************************************************************/

static void logs_crash_live(FAR struct pnut_loop_s *loop,
                            FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct logs_s *logs = arg;
  FAR char *buffer;
  size_t from;
  size_t len = 0;

  logs->quiet = NULL;

  /* Another report waits for its write: this one after it */

  if (logs->crash != NULL)
    {
      if (pnut_timer_start(loop, LOGS_QUIET_MS, 0, logs_crash_live, logs,
                           &logs->quiet) < 0)
        {
          logs->quiet = NULL;
        }

      return;
    }

  buffer = logs_read_all(logs->config.reread != NULL ?
                         logs->config.reread : logs->config.source, -1,
                         &len);
  if (buffer == NULL)
    {
      return;
    }

  len = logs_crash_take(buffer, len);
  if (len == 0)
    {
      free(buffer);
      return;
    }

  from = logs_crash_from(logs, buffer, len);
  if (len <= from || logs_crash_known(logs, buffer + from, len - from))
    {
      free(buffer);
      return;
    }

  memmove(buffer, buffer + from, len - from);
  logs->crash     = buffer;
  logs->crash_len = len - from;
  logs_crash_name(logs);
  logs_schedule(logs);
}

/****************************************************************************
 * Name: logs_line
 *
 * Description:
 *   A line read from the RAM log, while running.
 *
 ****************************************************************************/

static void logs_line(FAR struct logs_s *logs, FAR char *line, size_t len)
{
  struct logs_line_s parsed;

  if (len > 0 && line[len - 1] == '\r')
    {
      line[--len] = '\0';
    }

  logs_parse(line, &parsed);

  /* A crash's dump: the report once its lines stop */

  if (logs_dump(&parsed))
    {
      if (logs->quiet != NULL)
        {
          pnut_timer_cancel(logs->module.loop, logs->quiet);
          logs->quiet = NULL;
        }

      if (pnut_timer_start(logs->module.loop, LOGS_QUIET_MS, 0,
                           logs_crash_live, logs, &logs->quiet) < 0)
        {
          logs->quiet = NULL;
        }
    }

  if (logs_kept(line, &parsed))
    {
      logs_add(logs, line, len, false);
    }
}

/****************************************************************************
 * Name: logs_drain
 *
 * Description:
 *   Read what the RAM log has, a line at a time.
 *
 ****************************************************************************/

static void logs_drain(FAR struct logs_s *logs)
{
  char chunk[128];
  ssize_t n;
  ssize_t i;

  for (; ; )
    {
      n = read(logs->fd, chunk, sizeof(chunk));
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          break;
        }

      for (i = 0; i < n; i++)
        {
          if (chunk[i] == '\n')
            {
              logs->line[logs->linelen] = '\0';
              logs_line(logs, logs->line, logs->linelen);
              logs->linelen = 0;
            }
          else if (logs->linelen < sizeof(logs->line) - 1)
            {
              logs->line[logs->linelen++] = chunk[i];
            }
        }
    }
}

static void logs_readable(FAR struct pnut_loop_s *loop, int fd,
                          uint32_t events, FAR void *arg)
{
  FAR struct logs_s *logs = arg;

  logs_drain(logs);
  logs_schedule(logs);
}

/****************************************************************************
 * Name: logs_catch_up
 *
 * Description:
 *   As Log starts: copy the lines the RAM log kept that the files have
 *   not, those after the last line copied, with the start's mark before
 *   this start's first line; and take a crash report from a dump not
 *   saved yet.  A reset keeps the RAM log, so the lines of the start
 *   before, up to the reset, are there too.
 *
 ****************************************************************************/

static void logs_catch_up(FAR struct logs_s *logs)
{
  struct logs_line_s parsed;
  char last[LOGS_LINE_MAX];
  char line[LOGS_LINE_MAX];
  FAR const char *cursor;
  FAR const char *start;
  FAR const char *from;
  FAR const char *boot = NULL;
  FAR const char *end;
  FAR char *buffer;
  int64_t before = -1;
  size_t report;
  size_t first;
  size_t len = 0;
  size_t n;

  logs_last_saved(logs, last);

  buffer = logs_read_all(logs->config.source, logs->fd, &len);
  if (buffer == NULL)
    {
      logs_mark_start(logs);
      return;
    }

  end = buffer + len;

  /* A crash's dump not saved yet: the report, copied out before the
   * buffer goes
   */

  report = logs_crash_take(buffer, len);
  first  = logs_crash_from(logs, buffer, report);
  if (report > first &&
      !logs_crash_known(logs, buffer + first, report - first))
    {
      logs->crash = malloc(report - first);
      if (logs->crash != NULL)
        {
          memcpy(logs->crash, buffer + first, report - first);
          logs->crash_len = report - first;
          logs_crash_name(logs);
        }
    }

  /* Past the last line copied, if the RAM log still holds it.  The last
   * line alike: a start's first lines repeat, but with a time to the
   * microsecond, two equal ones are unlikely.
   */

  cursor = buffer;
  from   = buffer;
  if (last[0] != '\0')
    {
      while (logs_scan(&cursor, end, line, &n))
        {
          if (strcmp(line, last) == 0)
            {
              from = cursor;
            }
        }
    }

  /* This start's first line: the last whose time drops well below the
   * line's before.  None: the lines are all of this start.  The lines of
   * a start between, one Log never ran in, follow the earlier one's
   * unmarked.
   */

  cursor = from;
  start  = cursor;
  while (logs_scan(&cursor, end, line, &n))
    {
      logs_parse(line, &parsed);
      if (parsed.us >= 0)
        {
          if (before >= 0 && parsed.us + LOGS_BOOT_DROP < before)
            {
              boot = start;
            }

          before = parsed.us;
        }

      start = cursor;
    }

  if (boot == NULL)
    {
      logs_mark_start(logs);
    }

  cursor = from;
  start  = cursor;
  while (logs_scan(&cursor, end, line, &n))
    {
      if (start == boot)
        {
          logs_mark_start(logs);
        }

      logs_parse(line, &parsed);
      if (logs_kept(line, &parsed))
        {
          logs_add(logs, line, n, false);
        }

      start = cursor;
    }

  /* A last line still being written: the reading goes on with it */

  n = end - cursor;
  if (n >= sizeof(logs->line))
    {
      n = sizeof(logs->line) - 1;
    }

  memcpy(logs->line, cursor, n);
  logs->linelen = n;
  free(buffer);
}

/****************************************************************************
 * Name: logs_file_path
 *
 * Description:
 *   The path of file number n (log.proto), system.log's being rotations;
 *   false when it is not kept.
 *
 ****************************************************************************/

static bool logs_file_path(FAR const struct logs_read_s *r, uint32_t n,
                           FAR char *path, size_t size)
{
  uint32_t back;

  if (n > r->rotations || r->rotations - n >= r->files)
    {
      return false;
    }

  back = r->rotations - n;
  if (back == 0)
    {
      snprintf(path, size, "%s/%s", r->dir, LOGS_FILE);
    }
  else
    {
      snprintf(path, size, "%s/%s.%" PRIu32, r->dir, LOGS_FILE, back);
    }

  return true;
}

/****************************************************************************
 * Name: logs_read_match
 *
 * Description:
 *   Whether a line passes the reader's filter: its level at least the
 *   one asked for, and the service or program named in it.  Log's own
 *   marks always pass, as the starts and stops between lines.
 *
 ****************************************************************************/

static bool logs_read_match(FAR const pnut_log_read_request_t *in,
                            FAR const char *line)
{
  struct logs_line_s parsed;
  FAR const char *p = line;
  FAR const char *q;
  size_t len = strlen(in->service);
  int i;

  if (strncmp(line, LOGS_MARK, sizeof(LOGS_MARK) - 1) == 0)
    {
      return true;
    }

  logs_parse(line, &parsed);

  /* Asked: 1 debug ... 8 emergency; parsed: 0 emergency ... 7 debug */

  if (in->level > 0 && in->level <= 8 &&
      parsed.level > 8 - in->level)
    {
      return false;
    }

  if (len == 0)
    {
      return true;
    }

  /* Past the time, the CPU and the level in brackets: "program: module:
   * text", either name the one asked for
   */

  while (*p == '[' && (q = strchr(p, ']')) != NULL)
    {
      p = q + 1;
      while (*p == ' ')
        {
          p++;
        }
    }

  for (i = 0; i < 2; i++)
    {
      q = strchr(p, ':');
      if (q == NULL)
        {
          return false;
        }

      if ((size_t)(q - p) == len && strncmp(p, in->service, len) == 0)
        {
          return true;
        }

      p = q + 1;
      while (*p == ' ')
        {
          p++;
        }
    }

  return false;
}

/****************************************************************************
 * Name: logs_read_lines
 *
 * Description:
 *   A page of lines from a position on, on a worker: the files read as one
 *   sequence, oldest first.
 *
 ****************************************************************************/

static void logs_read_lines(FAR struct logs_read_s *r)
{
  FAR const pnut_log_read_request_t *in = &r->in.read;
  FAR pnut_log_page_t *page = &r->out.page;
  FAR char *buf = r->buf;
  char path[LOGS_PATH_MAX];
  FAR const char *nl;
  struct stat st;
  bool skipping = false;          /* A line longer than a piece: its rest */
  uint32_t oldest;
  uint32_t start;
  uint32_t file;
  uint32_t off;
  size_t scanned = 0;
  size_t pos;
  size_t len;
  ssize_t n;
  int fd;

  oldest = r->rotations >= r->files - 1u ? r->rotations - (r->files - 1u) :
           0;
  file = oldest;
  off  = 0;
  if (in->has_from)
    {
      if (in->from.file < oldest || in->from.file > r->rotations)
        {
          page->lost = true;      /* Rotated away, or another start's */
        }
      else
        {
          file = in->from.file;
          off  = in->from.offset;
        }
    }

  for (; ; )
    {
      if (!logs_file_path(r, file, path, sizeof(path)))
        {
          break;
        }

      fd = open(path, O_RDONLY | O_CLOEXEC);
      if (fd < 0)
        {
          if (errno == ENOENT && file < r->rotations)
            {
              file++;             /* Not made yet: the next */
              off = 0;
              continue;
            }

          if (errno != ENOENT)
            {
              r->ret = -errno;
              return;
            }

          break;
        }

      /* An offset past the end: another file's, from another start: from
       * the oldest, said lost
       */

      if (fstat(fd, &st) == 0 && (off_t)off > st.st_size && !page->lost)
        {
          close(fd);
          page->lost = true;
          file = oldest;
          off  = 0;
          continue;
        }

      n     = -1;
      start = off;
      if (lseek(fd, off, SEEK_SET) >= 0)
        {
          n = read(fd, buf, LOGS_READ_CHUNK);
        }

      close(fd);
      if (n < 0)
        {
          r->ret = -EIO;
          return;
        }

      if (n == 0)
        {
          if (file < r->rotations)
            {
              file++;
              off      = 0;
              skipping = false;   /* A long line ends with its file */
              continue;
            }

          break;                  /* The newest line read */
        }

      /* The whole lines in what was read; one longer than a chunk is
       * passed over
       */

      for (pos = 0; pos < (size_t)n; )
        {
          nl = memchr(buf + pos, '\n', (size_t)n - pos);
          if (nl == NULL)
            {
              if (pos == 0 && n == LOGS_READ_CHUNK)
                {
                  off     += (uint32_t)n;
                  skipping = true;
                }

              break;
            }

          len = (size_t)(nl - (buf + pos));
          if (skipping)
            {
              /* The rest of a line longer than a piece: passed over */

              off     += (uint32_t)len + 1;
              pos     += len + 1;
              skipping = false;
              continue;
            }

          buf[pos + len] = '\0';
          if (len > 0 && buf[pos + len - 1] == '\r')
            {
              buf[pos + len - 1] = '\0';
            }

          off += (uint32_t)len + 1;
          if (logs_read_match(in, buf + pos))
            {
              strlcpy(page->lines[page->lines_count], buf + pos,
                      sizeof(page->lines[0]));
              page->lines_count++;
            }

          pos += len + 1;
          if (page->lines_count ==
              sizeof(page->lines) / sizeof(page->lines[0]))
            {
              page->more = true;
              goto done;
            }
        }

      scanned += (size_t)n;

      /* Nothing whole: a line still being written, the end; or a rotated
       * file's cut tail, then the next file
       */

      if (off == start)
        {
          if (file < r->rotations)
            {
              file++;
              off      = 0;
              skipping = false;
              continue;
            }

          break;
        }

      if (scanned >= LOGS_READ_SCAN)
        {
          page->more = true;
          break;
        }
    }

done:
  page->has_next    = true;
  page->next.file   = file;
  page->next.offset = off;
}

/****************************************************************************
 * Name: logs_read_crashes, logs_read_crash
 *
 * Description:
 *   The crash reports kept, newest first; and a part of one.
 *
 ****************************************************************************/

static void logs_read_crashes(FAR struct logs_read_s *r)
{
  FAR pnut_log_crashes_t *out = &r->out.crashes;
  size_t max = sizeof(out->names) / sizeof(out->names[0]);
  FAR struct dirent *de;
  FAR DIR *d;
  size_t i;
  size_t j;

  d = opendir(r->dir);
  if (d == NULL)
    {
      r->ret = errno == ENOENT ? OK : -errno;
      return;
    }

  while ((de = readdir(d)) != NULL)
    {
      if (strncmp(de->d_name, LOGS_CRASH, sizeof(LOGS_CRASH) - 1) != 0 ||
          strlen(de->d_name) >= sizeof(out->names[0]))
        {
          continue;
        }

      /* Kept sorted, newest (greatest name) first, the oldest dropped */

      for (i = 0; i < out->names_count; i++)
        {
          if (strcmp(de->d_name, out->names[i]) > 0)
            {
              break;
            }
        }

      if (i >= max)
        {
          continue;
        }

      j = out->names_count < max ? out->names_count : max - 1;
      for (; j > i; j--)
        {
          strlcpy(out->names[j], out->names[j - 1], sizeof(out->names[0]));
        }

      strlcpy(out->names[i], de->d_name, sizeof(out->names[0]));
      if (out->names_count < max)
        {
          out->names_count++;
        }
    }

  closedir(d);
}

static void logs_read_crash(FAR struct logs_read_s *r)
{
  FAR const pnut_log_crash_request_t *in = &r->in.crash;
  FAR pnut_log_crash_t *out = &r->out.crash;
  char path[LOGS_PATH_MAX];
  struct stat st;
  ssize_t n;
  int fd;

  /* A report's own name, nothing else in the directory */

  if (strncmp(in->name, LOGS_CRASH, sizeof(LOGS_CRASH) - 1) != 0 ||
      strchr(in->name, '/') != NULL)
    {
      r->ret = -ENOENT;
      return;
    }

  snprintf(path, sizeof(path), "%s/%s", r->dir, in->name);
  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      r->ret = -errno;
      return;
    }

  n = -1;
  if (fstat(fd, &st) == 0 && lseek(fd, in->offset, SEEK_SET) >= 0)
    {
      n = read(fd, out->text.bytes, sizeof(out->text.bytes));
    }

  close(fd);
  if (n < 0)
    {
      r->ret = -EIO;
      return;
    }

  out->text.size = (pb_size_t)n;
  out->more      = (off_t)in->offset + n < st.st_size;
}

/****************************************************************************
 * Name: logs_read_run, logs_read_done
 *
 * Description:
 *   A request read on a worker; answered on the loop.
 *
 ****************************************************************************/

static void logs_read_run(FAR void *arg)
{
  FAR struct logs_read_s *r = arg;

  r->ret = OK;
  memset(&r->out, 0, sizeof(r->out));
  switch (r->kind)
    {
      case LOGS_READ_LINES:
        logs_read_lines(r);
        break;

      case LOGS_READ_CRASHES:
        logs_read_crashes(r);
        break;

      default:
        logs_read_crash(r);
        break;
    }

  __atomic_store_n(&r->finished, true, __ATOMIC_RELEASE);
}

static void logs_read_answer(FAR struct logs_s *logs)
{
  FAR struct logs_read_s *r = &logs->read;

  if (!logs->serving)
    {
      return;
    }

  if (r->ret < 0)
    {
      pnut_log_fail(&logs->server, &r->req,
                    r->ret == -ENOENT ? PNUT_STATUS_NOTFOUND :
                    PNUT_STATUS_INTERNAL,
                    r->ret == -ENOENT ? PNUT_LOG_ERROR_CODE_NOT_FOUND :
                    PNUT_LOG_ERROR_CODE_UNREADABLE, NULL);
      return;
    }

  switch (r->kind)
    {
      case LOGS_READ_LINES:
        pnut_log_read_reply(&logs->server, &r->req, PNUT_STATUS_OK,
                            &r->out.page);
        break;

      case LOGS_READ_CRASHES:
        pnut_log_crashes_reply(&logs->server, &r->req, PNUT_STATUS_OK,
                               &r->out.crashes);
        break;

      default:
        pnut_log_read_crash_reply(&logs->server, &r->req, PNUT_STATUS_OK,
                                  &r->out.crash);
        break;
    }
}

static void logs_read_done(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct logs_s *logs =
    (FAR struct logs_s *)((FAR char *)arg - offsetof(struct logs_s, read));

  logs->reading = false;
  logs_read_answer(logs);
  logs_schedule(logs);            /* Lines that waited for it */
}

static void logs_read_start(FAR struct logs_s *logs)
{
  FAR struct logs_read_s *r = &logs->read;

  r->dir       = logs->config.dir;
  r->files     = logs->config.files;
  r->rotations = logs->rotations;
  __atomic_store_n(&r->finished, false, __ATOMIC_RELAXED);
  logs->reading = true;
  if (pnut_job_submit(logs->module.loop, logs_read_run, logs_read_done,
                      r) < 0)
    {
      /* Read on the loop instead: rare */

      logs_read_run(r);
      logs->reading = false;
      logs_read_answer(logs);
      logs_schedule(logs);
    }
}

/****************************************************************************
 * Name: logs_allowed, logs_request
 *
 * Description:
 *   Who may call (RFC 0026): the system UI, and a command typed in one of
 *   the shells (RFC 0012); then the request is read, once no write runs.
 *   One at a time: another meanwhile is answered busy.
 *
 ****************************************************************************/

static int logs_allowed(FAR struct logs_s *logs,
                        FAR const struct pnut_request_s *req)
{
  FAR const struct logs_config_s *config = &logs->config;
  FAR const char *program;
  int ret;

  if (config->who == NULL)
    {
      return OK;
    }

  ret = config->who(config->identity, req->pid, &program);
  if (ret == -ESRCH && config->origin != NULL && config->shells != NULL &&
      config->shells[0] != '\0')
    {
      ret = config->origin(config->identity, req->pid, &program);
      if (ret == OK)
        {
          return programs_listed(config->shells, program) ? OK : -EPERM;
        }
    }

  /* No program's, nor a shell's: refused; not told now: try again */

  if (ret == -ESRCH)
    {
      return -EPERM;
    }

  if (ret < 0)
    {
      return -EAGAIN;
    }

  return strcmp(program, "ui") == 0 ? OK : -EPERM;
}

static void logs_request(FAR struct logs_s *logs, uint8_t kind,
                         FAR const struct pnut_request_s *req,
                         FAR const void *in, size_t size)
{
  int ret;

  ret = logs_allowed(logs, req);
  if (ret < 0)
    {
      pnut_log_fail(&logs->server, req,
                    ret == -EAGAIN ? PNUT_STATUS_UNAVAILABLE :
                    PNUT_STATUS_DENIED,
                    ret == -EAGAIN ? 0 : PNUT_LOG_ERROR_CODE_DENIED, NULL);
      return;
    }

  if (logs->reading || logs->read_waiting)
    {
      pnut_log_fail(&logs->server, req, PNUT_STATUS_BUSY, 0, NULL);
      return;
    }

  memset(&logs->read.in, 0, sizeof(logs->read.in));
  memcpy(&logs->read.in, in, size);
  logs->read.kind = kind;
  logs->read.req  = *req;
  if (logs->writing)
    {
      logs->read_waiting = true;  /* After the write: it may rotate */
      return;
    }

  logs_read_start(logs);
}

static void logs_on_read(FAR struct pnut_log_server_s *server,
                         FAR const struct pnut_request_s *req,
                         FAR const pnut_log_read_request_t *in,
                         FAR void *arg)
{
  logs_request(arg, LOGS_READ_LINES, req, in, sizeof(*in));
}

static void logs_on_crashes(FAR struct pnut_log_server_s *server,
                            FAR const struct pnut_request_s *req,
                            FAR const pnut_log_crashes_request_t *in,
                            FAR void *arg)
{
  logs_request(arg, LOGS_READ_CRASHES, req, in, sizeof(*in));
}

static void logs_on_read_crash(FAR struct pnut_log_server_s *server,
                               FAR const struct pnut_request_s *req,
                               FAR const pnut_log_crash_request_t *in,
                               FAR void *arg)
{
  logs_request(arg, LOGS_READ_CRASH, req, in, sizeof(*in));
}

static const struct pnut_log_handlers_s g_logs_handlers =
{
  .read       = logs_on_read,
  .crashes    = logs_on_crashes,
  .read_crash = logs_on_read_crash,
};

/****************************************************************************
 * Name: logs_start

 ****************************************************************************/

static int logs_start(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct logs_s *logs = (FAR struct logs_s *)module;
  size_t size = 2 * (size_t)logs->config.batch + LOGS_MARKS_ROOM;
  int ret;

  /* Nothing here fails the program: it serves the others all the same */

  pnut_module_ready(module);

  /* The interface: the files are read whatever else follows */

  if (logs->config.service != NULL)
    {
      ret = pnut_log_serve(loop, logs->config.service, 0, &g_logs_handlers,
                           logs, &logs->server);
      if (ret < 0)
        {
          pnut_log(module, PNUT_LOG_WARNING, "Cannot serve %s: %d",
                   logs->config.service, ret);
        }
      else
        {
          logs->serving = true;
        }
    }

  if (logs->config.source == NULL)
    {
      return OK;
    }

  if (strlen(logs->config.dir) + 1 + LOGS_CRASH_NAME >= LOGS_PATH_MAX)
    {
      pnut_log(module, PNUT_LOG_ERROR, "%s: too long; nothing kept",
               logs->config.dir);
      return OK;
    }

  logs->fd = open(logs->config.source, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (logs->fd < 0)
    {
      pnut_log(module, errno == ENOENT ? PNUT_LOG_INFO : PNUT_LOG_WARNING,
               "No RAM log at %s: %d; nothing kept", logs->config.source,
               -errno);
      return OK;
    }

  logs->pending = malloc(size);
  logs->spare   = malloc(size);
  if (logs->pending == NULL || logs->spare == NULL)
    {
      pnut_log(module, PNUT_LOG_ERROR, "No memory: nothing kept");
      free(logs->pending);
      free(logs->spare);
      logs->pending = NULL;
      logs->spare   = NULL;
      close(logs->fd);
      logs->fd = -1;
      return OK;
    }

  if (mkdir(logs->config.dir, 0700) < 0 && errno != EEXIST)
    {
      pnut_log(module, PNUT_LOG_WARNING, "Cannot make %s: %d",
               logs->config.dir, -errno);
    }

  logs_catch_up(logs);

  ret = pnut_loop_watch(loop, logs->fd, EPOLLIN, logs_readable, logs);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Cannot follow %s: %d",
               logs->config.source, ret);
    }
  else
    {
      logs->watching = true;
    }

  logs_drain(logs);
  logs_schedule(logs);
  return OK;
}

/****************************************************************************
 * Name: logs_stop
 *
 * Description:
 *   Write what is left, on the loop: the lines up to now, after the write
 *   still running if one is.
 *
 ****************************************************************************/

static void logs_stop(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct logs_s *logs = (FAR struct logs_s *)module;
  struct timespec ts;
  struct tm tm;
  char when[24];
  int waited;

  if (logs->serving)
    {
      pnut_log_close(&logs->server);
      logs->serving = false;
    }

  /* A read in flight answers nobody now; its worker is let finish */

  for (waited = 0;
       logs->reading && waited < LOGS_STOP_WAIT_MS &&
       !__atomic_load_n(&logs->read.finished, __ATOMIC_ACQUIRE);
       waited += LOGS_STOP_STEP_MS)
    {
      usleep(LOGS_STOP_STEP_MS * 1000);
    }

  if (logs->fd < 0)
    {
      return;
    }

  if (logs->quiet != NULL)
    {
      pnut_timer_cancel(loop, logs->quiet);
      logs->quiet = NULL;
    }

  if (logs->timer != NULL)
    {
      pnut_timer_cancel(loop, logs->timer);
      logs->timer = NULL;
    }

  if (logs->watching)
    {
      pnut_loop_unwatch(loop, logs->fd);
      logs->watching = false;
    }

  for (waited = 0;
       logs->writing && waited < LOGS_STOP_WAIT_MS &&
       !__atomic_load_n(&logs->write.finished, __ATOMIC_ACQUIRE);
       waited += LOGS_STOP_STEP_MS)
    {
      usleep(LOGS_STOP_STEP_MS * 1000);
    }

  if (logs->writing)
    {
      if (!__atomic_load_n(&logs->write.finished, __ATOMIC_ACQUIRE))
        {
          /* Its buffers stay until logs_deinit(), after the workers */

          pnut_log(module, PNUT_LOG_ERROR,
                   "A write runs on: the last lines not kept");
          close(logs->fd);
          logs->fd = -1;
          return;
        }

      logs->writing = false;
      logs_written(logs);
    }

  /* The lines up to now, then the stop's mark */

  logs_drain(logs);

  logs_now(logs, &ts);
  gmtime_r(&ts.tv_sec, &tm);
  strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
  logs_mark_dropped(logs);
  logs_mark(logs, "stop %s ----", when);

  logs_prepare(logs);
  logs_write_run(&logs->write);
  logs_written(logs);

  close(logs->fd);
  logs->fd = -1;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: logs_init
 ****************************************************************************/

void logs_init(FAR struct logs_s *logs,
               FAR const struct logs_config_s *config)
{
  memset(logs, 0, sizeof(*logs));
  logs->config       = *config;
  logs->fd           = -1;
  logs->module.name  = "log";
  logs->module.start = logs_start;
  logs->module.stop  = logs_stop;

  if (logs->config.files == 0)
    {
      logs->config.files = 1;
    }

  /* The oldest file kept is number 0 at start (log.proto) */

  logs->rotations = logs->config.files - 1u;

  if (logs->config.batch < LOGS_LINE_MAX)
    {
      logs->config.batch = LOGS_LINE_MAX;
    }

  if (logs->config.file_size < 2 * LOGS_LINE_MAX)
    {
      logs->config.file_size = 2 * LOGS_LINE_MAX;
    }

  if (logs->config.crash_size < LOGS_LINE_MAX)
    {
      logs->config.crash_size = LOGS_LINE_MAX;
    }
}

/****************************************************************************
 * Name: logs_deinit
 ****************************************************************************/

void logs_deinit(FAR struct logs_s *logs)
{
  free(logs->pending);
  free(logs->spare);
  free(logs->crash);
  free(logs->write.crash);
  logs->pending     = NULL;
  logs->spare       = NULL;
  logs->crash       = NULL;
  logs->write.crash = NULL;
}
