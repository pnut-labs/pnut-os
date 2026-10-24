/****************************************************************************
 * pnut-os/src/system/settings.c
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include <pnut/log.h>
#include <pnut/msg.h>
#include <pnut/worker.h>

#include "settings.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* How long the module waits, when it stops, for a write on a worker */

#define SETTINGS_STOP_WAIT_MS  2000
#define SETTINGS_STOP_STEP_MS  10

/* The largest file read, as a multiple of the largest written: a file
 * from a configuration with larger limits is still read, and its values
 * the schema keeps are kept; a larger one is set aside
 */

#define SETTINGS_READ_MAX      4

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void
settings_register_schema(FAR struct pnut_settings_server_s *server,
                         FAR const struct pnut_request_s *req,
                         FAR const pnut_settings_register_request_t *in,
                         FAR void *arg);
static void settings_get(FAR struct pnut_settings_server_s *server,
                         FAR const struct pnut_request_s *req,
                         FAR const pnut_settings_get_request_t *in,
                         FAR void *arg);
static void settings_set(FAR struct pnut_settings_server_s *server,
                         FAR const struct pnut_request_s *req,
                         FAR const pnut_settings_set_request_t *in,
                         FAR void *arg);
static void settings_reset(FAR struct pnut_settings_server_s *server,
                           FAR const struct pnut_request_s *req,
                           FAR const pnut_settings_reset_request_t *in,
                           FAR void *arg);
static void settings_list(FAR struct pnut_settings_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_settings_page_request_t *in,
                          FAR void *arg);
static void settings_describe(FAR struct pnut_settings_server_s *server,
                              FAR const struct pnut_request_s *req,
                              FAR const pnut_settings_page_request_t *in,
                              FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct pnut_settings_handlers_s g_settings_handlers =
{
  .register_schema = settings_register_schema,
  .get             = settings_get,
  .set             = settings_set,
  .reset           = settings_reset,
  .list            = settings_list,
  .describe        = settings_describe,
};

/* The errors' text, for the log and developers, by code */

static const char * const g_settings_errors[] =
{
  "",
  "unknown owner",
  "unknown key",
  "wrong type",
  "out of range",
  "too long",
  "not a choice",
  "secrets are not stored yet",
  "no room left",
  "bad schema",
  "not registering",
  "bad name",
  "registering",
  "not the caller's",
  "who calls is not known yet",
};

/* What a caller may do with an owner's settings (RFC 0025) */

enum settings_access_e
{
  SETTINGS_UNKNOWN = -1,          /* Who calls cannot be told now */
  SETTINGS_PUBLIC = 0,            /* Read those marked public */
  SETTINGS_OWNER,                 /* All, and register their schema */
  SETTINGS_ALL,                   /* All, as the system UI */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: settings_fail
 *
 * Description:
 *   Answer with an error and its detail: the setting it is about, which is
 *   a name from a schema, never a value.
 *
 ****************************************************************************/

static void settings_fail(FAR struct settings_s *settings,
                          FAR const struct pnut_request_s *req, int status,
                          uint32_t code, FAR const char *owner,
                          FAR const char *key)
{
  char text[64];

  snprintf(text, sizeof(text), "%s%s%s: %s", owner,
           key != NULL && key[0] != '\0' ? "/" : "",
           key != NULL ? key : "",
           code < sizeof(g_settings_errors) / sizeof(g_settings_errors[0]) ?
           g_settings_errors[code] : "error");
  pnut_settings_fail(&settings->server, req, status, code, text);
}

/****************************************************************************
 * Name: settings_access
 *
 * Description:
 *   What the caller may do with the owner's settings: a program those of
 *   the services it runs, the system UI every owner's, and anyone those
 *   marked public (RFC 0025).  The runtime will mark an app's calls with
 *   its identity (RFC 0008); until then a caller is a program.  A request
 *   whose caller cannot be told now is answered unavailable, to be tried
 *   again: neither denied nor allowed.
 *
 ****************************************************************************/

static int settings_access(FAR struct settings_s *settings,
                           FAR const struct pnut_request_s *req,
                           FAR const char *owner)
{
  FAR const struct settings_config_s *config = &settings->config;
  FAR const char *program;
  int ret;

  if (config->who == NULL)
    {
      return SETTINGS_ALL;
    }

  ret = config->who(config->identity, req->pid, &program);
  if (ret == -ESRCH)
    {
      return SETTINGS_PUBLIC;
    }
  else if (ret < 0)
    {
      return SETTINGS_UNKNOWN;
    }

  /* Owner first: the system UI owns settings of its own too */

  if (config->runs != NULL &&
      config->runs(config->identity, program, owner))
    {
      return SETTINGS_OWNER;
    }

  return strcmp(program, "ui") == 0 ? SETTINGS_ALL : SETTINGS_PUBLIC;
}

/****************************************************************************
 * Name: settings_allowed
 *
 * Description:
 *   The caller's access, or an answer for a caller who cannot be told now,
 *   and -1.
 *
 ****************************************************************************/

static int settings_allowed(FAR struct settings_s *settings,
                            FAR const struct pnut_request_s *req,
                            FAR const char *owner)
{
  int access = settings_access(settings, req, owner);

  if (access == SETTINGS_UNKNOWN)
    {
      settings_fail(settings, req, PNUT_STATUS_UNAVAILABLE,
                    PNUT_SETTINGS_ERROR_CODE_CALLER_UNKNOWN, owner, NULL);
    }

  return access;
}

/****************************************************************************
 * Name: settings_path
 *
 * Description:
 *   An owner's file, and the one written beside it.
 *
 ****************************************************************************/

static int settings_path(FAR struct settings_s *settings,
                         FAR const char *owner, FAR const char *suffix,
                         FAR char *path)
{
  int ret;

  ret = snprintf(path, SETTINGS_PATH_MAX, "%s/%s.pb%s",
                 settings->config.dir, owner, suffix);
  return ret < 0 || ret >= SETTINGS_PATH_MAX ? -ENAMETOOLONG : OK;
}

/* A default looked for in the defaults file */

struct settings_lookup_s
{
  FAR struct settings_s *settings;
  FAR const char *owner;
  FAR const char *key;
  FAR pnut_setting_value_t *value;
  bool found;
};

/****************************************************************************
 * Name: settings_replaceable
 *
 * Description:
 *   Whether a file may be renamed over: absent, or a regular file.  NuttX
 *   moves a file into a directory found at the name it is renamed to.
 *
 ****************************************************************************/

static bool settings_replaceable(FAR const char *path)
{
  struct stat st;

  return lstat(path, &st) < 0 ? errno == ENOENT : S_ISREG(st.st_mode);
}

/****************************************************************************
 * Name: settings_defaults_load
 *
 * Description:
 *   Read the target's defaults file (RFC 0025) whole, once, as Settings
 *   starts: it is small, and looked up as each schema registers.
 *
 ****************************************************************************/

static void settings_defaults_load(FAR struct settings_s *settings)
{
  FAR const char *path = settings->config.defaults;
  struct stat st;
  size_t done = 0;
  ssize_t n;
  int fd;

  if (path == NULL)
    {
      return;
    }

  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      if (errno != ENOENT)
        {
          pnut_log(&settings->module, PNUT_LOG_WARNING,
                   "Cannot open %s: %d", path, -errno);
        }

      return;
    }

  if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size == 0 ||
      st.st_size > SETTINGS_DEFAULTS_MAX)
    {
      pnut_log(&settings->module, PNUT_LOG_WARNING,
               "%s: not a file of 1 to %d bytes", path,
               SETTINGS_DEFAULTS_MAX);
      close(fd);
      return;
    }

  settings->defaults = malloc(st.st_size);
  if (settings->defaults == NULL)
    {
      pnut_log(&settings->module, PNUT_LOG_WARNING,
               "No memory for %s", path);
      close(fd);
      return;
    }

  while (done < (size_t)st.st_size)
    {
      n = read(fd, settings->defaults + done, st.st_size - done);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          pnut_log(&settings->module, PNUT_LOG_WARNING,
                   "Cannot read %s: %d", path, n < 0 ? -errno : -EIO);
          free(settings->defaults);
          settings->defaults = NULL;
          close(fd);
          return;
        }

      done += n;
    }

  close(fd);
  settings->ndefaults = done;
}

/****************************************************************************
 * Name: settings_default_entry
 *
 * Description:
 *   One of the defaults file's entries: the one looked for is kept.
 *
 ****************************************************************************/

static bool settings_default_entry(FAR pb_istream_t *stream,
                                   FAR const pb_field_t *field,
                                   FAR void **arg)
{
  FAR struct settings_lookup_s *lookup = *arg;
  FAR pnut_settings_default_t *in = &lookup->settings->entry;

  memset(in, 0, sizeof(*in));
  if (!pb_decode(stream, PNUT_SETTINGS_DEFAULT_FIELDS, in))
    {
      return false;
    }

  if (!lookup->found && in->has_value &&
      strcmp(in->owner, lookup->owner) == 0 &&
      strcmp(in->key, lookup->key) == 0)
    {
      *lookup->value = in->value;
      lookup->found  = true;
    }

  return true;
}

/****************************************************************************
 * Name: settings_default
 *
 * Description:
 *   The target's default for a setting, from its defaults file.
 *
 ****************************************************************************/

static bool settings_default(FAR void *arg, FAR const char *owner,
                             FAR const char *key,
                             FAR pnut_setting_value_t *value)
{
  FAR struct settings_s *settings = arg;
  struct settings_lookup_s lookup;
  pnut_settings_defaults_t msg;
  pb_istream_t stream;

  if (settings->defaults == NULL)
    {
      return false;
    }

  lookup.settings = settings;
  lookup.owner    = owner;
  lookup.key      = key;
  lookup.value    = value;
  lookup.found    = false;

  memset(&msg, 0, sizeof(msg));
  msg.defaults.funcs.decode = settings_default_entry;
  msg.defaults.arg          = &lookup;

  stream = pb_istream_from_buffer(settings->defaults, settings->ndefaults);
  if (!pb_decode(&stream, PNUT_SETTINGS_DEFAULTS_FIELDS, &msg))
    {
      pnut_log(&settings->module, PNUT_LOG_WARNING,
               "Cannot decode %s: %s", settings->config.defaults,
               PB_GET_ERROR(&stream));
      return false;
    }

  return lookup.found;
}

/****************************************************************************
 * Name: settings_read_file
 *
 * Description:
 *   Read an owner's file whole, into memory: on a worker, or on the loop
 *   when no worker takes it.  It touches nothing but the load.
 *
 ****************************************************************************/

static void settings_read_file(FAR struct settings_load_s *load)
{
  struct stat st;
  size_t done = 0;
  ssize_t n;
  int fd;

  fd = open(load->path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0 && errno == ENOENT &&
      lstat(load->temp, &st) == 0 && S_ISREG(st.st_mode) &&
      rename(load->temp, load->path) == 0)
    {
      /* The device stopped while the file written beside it was being
       * put in its place, after the old one was removed (see
       * settings_write_file()): the new one is whole
       */

      fd = open(load->path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    }

  if (fd < 0)
    {
      load->error = errno == ENOENT ? 0 : errno;
      return;
    }

  if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode))
    {
      load->error = EISDIR;
    }
  else if ((size_t)st.st_size > load->max)
    {
      load->error = EFBIG;
    }
  else
    {
      load->data = malloc(st.st_size > 0 ? st.st_size : 1);
      if (load->data == NULL)
        {
          load->error = ENOMEM;
        }

      while (load->data != NULL && done < (size_t)st.st_size)
        {
          n = read(fd, load->data + done, st.st_size - done);
          if (n < 0 && errno == EINTR)
            {
              continue;
            }

          if (n <= 0)
            {
              load->error = n < 0 ? errno : EIO;
              break;
            }

          done += n;
        }

      load->len = done;
    }

  close(fd);
}

/****************************************************************************
 * Name: settings_apply
 *
 * Description:
 *   An owner's file, read whole, decoded into its values, on the loop;
 *   then its values may be read, changed and written.
 *
 ****************************************************************************/

static void settings_apply(FAR struct settings_s *settings,
                           FAR struct settings_load_s *load)
{
  FAR const char *why = NULL;
  char bad[SETTINGS_PATH_MAX];
  pb_istream_t stream;
  int error = load->error;

  if (error == 0 && load->data != NULL)
    {
      stream = pb_istream_from_buffer(load->data, load->len);
      if (!settings_store_decode(&settings->store, load->owner, &stream))
        {
          why = "does not decode";
        }
    }
  else if (error == EFBIG)
    {
      why   = "is too large";
      error = 0;
    }

  /* Read whole but not as values, or too large to read: kept aside, for
   * whoever wants to look, and written again from what could be read
   */

  if (why != NULL)
    {
      if (settings_path(settings, load->owner, ".bad", bad) == 0 &&
          settings_replaceable(bad) && rename(load->path, bad) == 0)
        {
          pnut_log(&settings->module, PNUT_LOG_WARNING,
                   "%s: its file %s: kept as %s", load->owner, why, bad);
          settings_store_touch(&settings->store, load->owner);
        }
      else
        {
          error = EIO;
        }
    }

  /* There, but not read: never written over while the program runs */

  if (error != 0)
    {
      pnut_log(&settings->module, PNUT_LOG_ERROR,
               "%s: cannot read its file (%d): not written until a restart",
               load->owner, error);
      settings_store_hold(&settings->store, load->owner);
    }

  settings_store_loaded(&settings->store, load->owner);
}

/****************************************************************************
 * Name: settings_load_run
 *
 * Description:
 *   A read, on a worker.
 *
 ****************************************************************************/

static void settings_load_run(FAR void *arg)
{
  settings_read_file(arg);
}

static void settings_registered(FAR struct settings_s *settings,
                                FAR const char *owner,
                                FAR const struct pnut_request_s *req,
                                bool last);

/****************************************************************************
 * Name: settings_load_done
 *
 * Description:
 *   A read has finished, on the loop: the schema's registration is done.
 *
 ****************************************************************************/

static void settings_load_done(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct settings_load_s *load = arg;
  FAR struct settings_s *settings = load->settings;
  FAR struct settings_load_s **link;

  for (link = &settings->loads; *link != NULL && *link != load;
       link = &(*link)->next)
    {
    }

  if (*link != NULL)
    {
      *link = load->next;
    }

  settings_apply(settings, load);
  settings_registered(settings, load->owner, &load->req, true);
  free(load->data);
  free(load);
}

/****************************************************************************
 * Name: settings_load
 *
 * Description:
 *   Read an owner's values from its file, once its schema is whole and
 *   before anyone reads or changes them.  The file is read on a worker,
 *   and the registration answered once it is; when no worker takes it, it
 *   is read here, on the loop.
 *
 * Returned Value:
 *   true when the answer is left to the read; false when the file has been
 *   dealt with already, and the registration is to be answered now.
 *
 ****************************************************************************/

static bool settings_load(FAR struct settings_s *settings,
                          FAR const char *owner,
                          FAR const struct pnut_request_s *req)
{
  FAR struct settings_load_s *load;

  load = calloc(1, sizeof(*load));
  if (load == NULL)
    {
      pnut_log(&settings->module, PNUT_LOG_ERROR,
               "%s: no memory to read its file: not written until a "
               "restart", owner);
      settings_store_hold(&settings->store, owner);
      settings_store_loaded(&settings->store, owner);
      return false;
    }

  load->settings = settings;
  load->req      = *req;
  load->max      = settings->size * SETTINGS_READ_MAX;
  strlcpy(load->owner, owner, sizeof(load->owner));

  if (settings_path(settings, owner, "", load->path) < 0 ||
      settings_path(settings, owner, ".new", load->temp) < 0)
    {
      settings_store_hold(&settings->store, owner);
      settings_store_loaded(&settings->store, owner);
      free(load);
      return false;
    }

  if (pnut_job_submit(settings->module.loop, settings_load_run,
                      settings_load_done, load) >= 0)
    {
      load->next      = settings->loads;
      settings->loads = load;
      return true;
    }

  settings_read_file(load);
  settings_apply(settings, load);
  free(load->data);
  free(load);
  return false;
}

/****************************************************************************
 * Name: settings_write_file
 *
 * Description:
 *   Write a file beside an owner's, then put it in its place, so that a
 *   crash leaves the old file or the new one (on NuttX, the old one is
 *   removed first, and a crash between the two leaves the new one beside
 *   it, under its temporary name, which settings_load() puts in place).
 *
 ****************************************************************************/

static int settings_write_file(FAR const char *path, FAR const char *temp,
                               FAR const uint8_t *data, size_t len)
{
  ssize_t n;
  int ret = OK;
  int fd;

  /* Made afresh, never through a link left at its name */

  unlink(temp);
  fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
            0600);
  if (fd < 0)
    {
      return -errno;
    }

  while (len > 0)
    {
      n = write(fd, data, len);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n < 0)
        {
          ret = -errno;
          break;
        }

      data += n;
      len  -= n;
    }

  /* A file system without sync has nothing to flush */

  if (ret == OK && fsync(fd) < 0 && errno != EINVAL && errno != ENOSYS)
    {
      ret = -errno;
    }

  close(fd);

  if (ret == OK && !settings_replaceable(path))
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
    }

  return ret;
}

/****************************************************************************
 * Name: settings_write_run
 *
 * Description:
 *   A write, on a worker.
 *
 ****************************************************************************/

static void settings_write_run(FAR void *arg)
{
  FAR struct settings_write_s *write = arg;

  write->ret = settings_write_file(write->path, write->temp, write->data,
                                   write->len);
  __atomic_store_n(&write->finished, true, __ATOMIC_RELEASE);
}

/****************************************************************************
 * Name: settings_encode
 *
 * Description:
 *   Encode an owner's file into the buffer, and name the write.
 *
 ****************************************************************************/

static int settings_encode(FAR struct settings_s *settings,
                           FAR const char *owner)
{
  FAR struct settings_write_s *write = &settings->write;
  pb_ostream_t stream;

  stream = pb_ostream_from_buffer(settings->buffer, settings->size);
  strlcpy(settings->owner, owner, sizeof(settings->owner));

  if (settings_path(settings, owner, "", write->path) < 0 ||
      settings_path(settings, owner, ".new", write->temp) < 0)
    {
      return -ENAMETOOLONG;
    }

  if (!settings_store_encode(&settings->store, owner, &stream))
    {
      return -E2BIG;
    }

  write->data     = settings->buffer;
  write->len      = stream.bytes_written;
  write->ret      = OK;
  write->finished = false;
  return OK;
}

static void settings_schedule(FAR struct settings_s *settings);

/****************************************************************************
 * Name: settings_now
 *
 * Description:
 *   Milliseconds, for the store's turns.
 *
 ****************************************************************************/

static uint64_t settings_now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/****************************************************************************
 * Name: settings_failed
 *
 * Description:
 *   The owner's file was not written: it is tried again later and later
 *   (settings_store_failed()).  Only the first failure in a row is logged.
 *
 ****************************************************************************/

static void settings_failed(FAR struct settings_s *settings, int ret)
{
  if (settings_store_failed(&settings->store, settings->owner,
                            settings_now(), settings->config.delay) == 1)
    {
      pnut_log(&settings->module, PNUT_LOG_ERROR, "%s: cannot write: %d",
               settings->owner, ret);
    }
}

/****************************************************************************
 * Name: settings_write_done
 *
 * Description:
 *   A write has finished, on the loop: a failed one is tried again later.
 *
 ****************************************************************************/

static void settings_write_done(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct settings_s *settings =
    (FAR struct settings_s *)((FAR char *)arg -
                              offsetof(struct settings_s, write));

  settings->writing = false;
  if (settings->write.ret < 0)
    {
      settings_failed(settings, settings->write.ret);
    }
  else
    {
      settings_store_written(&settings->store, settings->owner);
    }

  settings_schedule(settings);
}

/****************************************************************************
 * Name: settings_write_next
 *
 * Description:
 *   Hand the next owner's file to a worker.
 *
 ****************************************************************************/

static void settings_write_next(FAR struct settings_s *settings)
{
  FAR const char *owner;
  int ret;

  if (settings->writing)
    {
      return;
    }

  /* None due now: perhaps one is later */

  owner = settings_store_dirty(&settings->store, settings_now());
  if (owner == NULL)
    {
      settings_schedule(settings);
      return;
    }

  ret = settings_encode(settings, owner);
  if (ret >= 0)
    {
      settings->writing = true;
      ret = pnut_job_submit(settings->module.loop, settings_write_run,
                            settings_write_done, &settings->write);
      settings->writing = ret >= 0;
    }

  if (ret < 0)
    {
      settings_failed(settings, ret);
      settings_schedule(settings);
    }
}

/****************************************************************************
 * Name: settings_timeout
 ****************************************************************************/

static void settings_timeout(FAR struct pnut_loop_s *loop,
                             FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct settings_s *settings = arg;

  settings->timer = NULL;
  settings_write_next(settings);
}

/****************************************************************************
 * Name: settings_schedule
 *
 * Description:
 *   Write the changed owners' files a moment from now, so that a burst of
 *   changes makes one write (RFC 0025), or once the first whose writes
 *   failed may be tried again.
 *
 ****************************************************************************/

static void settings_schedule(FAR struct settings_s *settings)
{
  uint64_t now = settings_now();
  uint32_t wait;
  int64_t due;
  int ret;

  if (settings->writing)
    {
      return;
    }

  due = settings_store_due(&settings->store, now);
  if (due < 0)
    {
      return;
    }

  wait = settings->config.delay;
  if (due > wait)
    {
      wait = (uint32_t)due;
    }

  /* A timer set for an owner whose writes failed may be far off: a change
   * to another owner comes sooner
   */

  if (settings->timer != NULL)
    {
      if (settings->timer_at <= now + wait)
        {
          return;
        }

      pnut_timer_cancel(settings->module.loop, settings->timer);
      settings->timer = NULL;
    }

  settings->timer_at = now + wait;
  ret = pnut_timer_start(settings->module.loop, wait, 0, settings_timeout,
                         settings, &settings->timer);
  if (ret < 0)
    {
      settings->timer = NULL;
      pnut_log(&settings->module, PNUT_LOG_WARNING,
               "No timer to write with: %d", ret);
    }
}

/****************************************************************************
 * Name: settings_flush
 *
 * Description:
 *   Write every changed owner's file now, as the module stops: after the
 *   write a worker may still be running, which the buffer is lent to.  A
 *   write still running after the wait keeps the buffer, and the changes
 *   not written are lost; so are those of an owner whose write fails,
 *   which does not stop the others'.  Both are logged.
 *
 ****************************************************************************/

static void settings_flush(FAR struct settings_s *settings)
{
  FAR const char *owner;
  uint8_t i;
  int waited;
  int ret;

  for (waited = 0;
       settings->writing && waited < SETTINGS_STOP_WAIT_MS &&
       !__atomic_load_n(&settings->write.finished, __ATOMIC_ACQUIRE);
       waited += SETTINGS_STOP_STEP_MS)
    {
      usleep(SETTINGS_STOP_STEP_MS * 1000);
    }

  if (settings->writing)
    {
      if (!__atomic_load_n(&settings->write.finished, __ATOMIC_ACQUIRE))
        {
          pnut_log(&settings->module, PNUT_LOG_ERROR,
                   "A write runs on: changes not written");
          return;
        }

      settings->writing = false;
      if (settings->write.ret < 0)
        {
          settings_store_touch(&settings->store, settings->owner);
        }
    }

  /* Each owner once at most: encoding one moves the turn past it */

  for (i = 0; i < settings->store.nowners; i++)
    {
      owner = settings_store_dirty(&settings->store, UINT64_MAX);
      if (owner == NULL)
        {
          break;
        }

      ret = settings_encode(settings, owner);
      if (ret >= 0)
        {
          ret = settings_write_file(settings->write.path,
                                    settings->write.temp,
                                    settings->write.data,
                                    settings->write.len);
        }

      if (ret < 0)
        {
          pnut_log(&settings->module, PNUT_LOG_ERROR,
                   "%s: cannot write: %d", owner, ret);
        }
    }
}

/****************************************************************************
 * Name: settings_changed
 *
 * Description:
 *   Announce a change on the settings topic: the owner and the key, or
 *   none when several of its settings may have changed (RFC 0025).
 *
 ****************************************************************************/

static void settings_changed(FAR struct settings_s *settings,
                             FAR const char *owner, FAR const char *key)
{
  pnut_settings_change_t msg;
  int ret;

  if (settings->changes == NULL)
    {
      return;
    }

  memset(&msg, 0, sizeof(msg));
  strlcpy(msg.owner, owner, sizeof(msg.owner));
  strlcpy(msg.key, key, sizeof(msg.key));
  msg.version = ++settings->version;

  ret = pnut_settings_change_publish(settings->changes, &msg);
  if (ret < 0)
    {
      pnut_log(&settings->module, PNUT_LOG_WARNING,
               "Cannot announce a change: %d", ret);
    }
}

/****************************************************************************
 * Name: settings_registered
 *
 * Description:
 *   A page of a schema taken: the last one's, once the owner's file is
 *   read, announced.
 *
 ****************************************************************************/

static void settings_registered(FAR struct settings_s *settings,
                                FAR const char *owner,
                                FAR const struct pnut_request_s *req,
                                bool last)
{
  /* A schema whole: values may have come from the file, gone back to
   * their defaults, or gone
   */

  if (last)
    {
      settings_changed(settings, owner, "");
    }

  settings_schedule(settings);
  pnut_settings_register_schema_reply(&settings->server, req,
                                      PNUT_STATUS_OK, NULL);
}

/****************************************************************************
 * Name: settings_register_schema
 ****************************************************************************/

static void
settings_register_schema(FAR struct pnut_settings_server_s *server,
                         FAR const struct pnut_request_s *req,
                         FAR const pnut_settings_register_request_t *in,
                         FAR void *arg)
{
  FAR struct settings_s *settings = arg;
  FAR const char *key = NULL;
  uint32_t code = PNUT_SETTINGS_ERROR_CODE_DENIED;
  bool load;
  int ret = PNUT_STATUS_DENIED;
  int access;

  /* Only a program that runs the service registers its schema; without
   * an identity, anyone
   */

  access = settings_allowed(settings, req, in->owner);
  if (access == SETTINGS_UNKNOWN)
    {
      return;
    }

  if (settings->config.who == NULL || access == SETTINGS_OWNER)
    {
      ret = settings_store_register(&settings->store, in, &load, &key,
                                    &code);
    }

  if (ret != PNUT_STATUS_OK)
    {
      settings_fail(settings, req, ret, code, in->owner, key);
      return;
    }

  if (in->last)
    {
      pnut_log(&settings->module, PNUT_LOG_DEBUG, "%s: registered",
               in->owner);
    }

  /* The last page answered once the owner's file is read, if it is read
   * on a worker
   */

  if (load && settings_load(settings, in->owner, req))
    {
      return;
    }

  settings_registered(settings, in->owner, req, in->last);
}

/****************************************************************************
 * Name: settings_get
 ****************************************************************************/

static void settings_get(FAR struct pnut_settings_server_s *server,
                         FAR const struct pnut_request_s *req,
                         FAR const pnut_settings_get_request_t *in,
                         FAR void *arg)
{
  FAR struct settings_s *settings = arg;
  uint32_t code;
  int access;
  int ret;

  access = settings_allowed(settings, req, in->owner);
  if (access == SETTINGS_UNKNOWN)
    {
      return;
    }

  ret = settings_store_get(&settings->store, in->owner, in->key,
                           access != SETTINGS_PUBLIC, &settings->out.value,
                           &code);
  if (ret != PNUT_STATUS_OK)
    {
      settings_fail(settings, req, ret, code, in->owner, in->key);
      return;
    }

  pnut_settings_get_reply(server, req, PNUT_STATUS_OK,
                          &settings->out.value);
}

/****************************************************************************
 * Name: settings_set
 ****************************************************************************/

static void settings_set(FAR struct pnut_settings_server_s *server,
                         FAR const struct pnut_request_s *req,
                         FAR const pnut_settings_set_request_t *in,
                         FAR void *arg)
{
  FAR struct settings_s *settings = arg;
  uint32_t code = PNUT_SETTINGS_ERROR_CODE_WRONG_TYPE;
  bool changed;
  int ret = PNUT_STATUS_INVALID;
  int access;

  access = settings_allowed(settings, req, in->owner);
  if (access == SETTINGS_UNKNOWN)
    {
      return;
    }

  if (access == SETTINGS_PUBLIC)
    {
      code = PNUT_SETTINGS_ERROR_CODE_DENIED;
      ret  = PNUT_STATUS_DENIED;
    }
  else if (in->has_value)
    {
      ret = settings_store_set(&settings->store, in->owner, in->key,
                               &in->value, &changed, &code);
    }

  if (ret != PNUT_STATUS_OK)
    {
      settings_fail(settings, req, ret, code, in->owner, in->key);
      return;
    }

  if (changed)
    {
      settings_changed(settings, in->owner, in->key);
      settings_schedule(settings);
    }

  pnut_settings_set_reply(server, req, PNUT_STATUS_OK, NULL);
}

/****************************************************************************
 * Name: settings_reset
 ****************************************************************************/

static void settings_reset(FAR struct pnut_settings_server_s *server,
                           FAR const struct pnut_request_s *req,
                           FAR const pnut_settings_reset_request_t *in,
                           FAR void *arg)
{
  FAR struct settings_s *settings = arg;
  uint32_t code = PNUT_SETTINGS_ERROR_CODE_DENIED;
  bool changed;
  int ret = PNUT_STATUS_DENIED;
  int access;

  access = settings_allowed(settings, req, in->owner);
  if (access == SETTINGS_UNKNOWN)
    {
      return;
    }

  if (access != SETTINGS_PUBLIC)
    {
      ret = settings_store_reset(&settings->store, in->owner, in->key,
                                 &changed, &code);
    }

  if (ret != PNUT_STATUS_OK)
    {
      settings_fail(settings, req, ret, code, in->owner, in->key);
      return;
    }

  if (changed)
    {
      settings_changed(settings, in->owner, in->key);
      settings_schedule(settings);
    }

  pnut_settings_reset_reply(server, req, PNUT_STATUS_OK, NULL);
}

/****************************************************************************
 * Name: settings_list
 ****************************************************************************/

static void settings_list(FAR struct pnut_settings_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_settings_page_request_t *in,
                          FAR void *arg)
{
  FAR struct settings_s *settings = arg;
  uint32_t code;
  int access;
  int ret;

  access = settings_allowed(settings, req, in->owner);
  if (access == SETTINGS_UNKNOWN)
    {
      return;
    }

  ret = settings_store_list(&settings->store, in->owner, in->after,
                            access != SETTINGS_PUBLIC,
                            &settings->out.values, &code);
  if (ret != PNUT_STATUS_OK)
    {
      settings_fail(settings, req, ret, code, in->owner, NULL);
      return;
    }

  pnut_settings_list_reply(server, req, PNUT_STATUS_OK,
                           &settings->out.values);
}

/****************************************************************************
 * Name: settings_describe
 ****************************************************************************/

static void settings_describe(FAR struct pnut_settings_server_s *server,
                              FAR const struct pnut_request_s *req,
                              FAR const pnut_settings_page_request_t *in,
                              FAR void *arg)
{
  FAR struct settings_s *settings = arg;
  uint32_t code;
  int access;
  int ret;

  access = settings_allowed(settings, req, in->owner);
  if (access == SETTINGS_UNKNOWN)
    {
      return;
    }

  ret = settings_store_describe(&settings->store, in->owner, in->after,
                                access != SETTINGS_PUBLIC,
                                &settings->out.schema, &code);
  if (ret != PNUT_STATUS_OK)
    {
      settings_fail(settings, req, ret, code, in->owner, NULL);
      return;
    }

  pnut_settings_describe_reply(server, req, PNUT_STATUS_OK,
                               &settings->out.schema);
}

/****************************************************************************
 * Name: settings_start
 ****************************************************************************/

static int settings_start(FAR struct pnut_module_s *module,
                          FAR struct pnut_loop_s *loop)
{
  FAR struct settings_s *settings = (FAR struct settings_s *)module;
  FAR const struct settings_limits_s *limits = &settings->config.limits;
  int ret;

  ret = settings_store_init(&settings->store, limits);
  if (ret < 0)
    {
      return ret;
    }

  settings->store.defaults     = settings_default;
  settings->store.defaults_arg = settings;
  settings_defaults_load(settings);

  settings->size   = SETTINGS_FILE_MAX(limits->settings, limits->texts);
  settings->buffer = malloc(settings->size);
  if (settings->buffer == NULL)
    {
      ret = -ENOMEM;
      goto errout;
    }

  if (mkdir(settings->config.dir, 0700) < 0 && errno != EEXIST)
    {
      ret = -errno;
      pnut_log(module, PNUT_LOG_ERROR, "Cannot make %s: %d",
               settings->config.dir, ret);
      goto errout;
    }

  /* Changes are announced where there are topics; Settings serves without
   * them
   */

  ret = pnut_settings_change_advertise(loop, &settings->changes);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No settings topic: %d", ret);
    }

  ret = pnut_settings_serve(loop, SETTINGS_SERVICE, 0, &g_settings_handlers,
                            settings, &settings->server);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Cannot serve: %d", ret);
      goto errout;
    }

  settings->serving = true;
  pnut_module_ready(module);
  return OK;

errout:
  pnut_topic_unadvertise(settings->changes);
  settings->changes = NULL;
  free(settings->buffer);
  settings->buffer = NULL;
  free(settings->defaults);
  settings->defaults = NULL;
  settings_store_deinit(&settings->store);
  return ret;
}

/****************************************************************************
 * Name: settings_stop
 ****************************************************************************/

static void settings_stop(FAR struct pnut_module_s *module,
                          FAR struct pnut_loop_s *loop)
{
  FAR struct settings_s *settings = (FAR struct settings_s *)module;

  if (settings->timer != NULL)
    {
      pnut_timer_cancel(loop, settings->timer);
      settings->timer = NULL;
    }

  settings_flush(settings);

  /* The reads still on workers keep their requests past the close: their
   * done handlers never run once the loop has stopped, and settings_deinit
   * frees them, after the workers are gone
   */

  if (settings->serving)
    {
      pnut_settings_close(&settings->server);
      settings->serving = false;
    }

  pnut_topic_unadvertise(settings->changes);
  settings->changes = NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void settings_init(FAR struct settings_s *settings,
                   FAR const struct settings_config_s *config)
{
  memset(settings, 0, sizeof(*settings));
  settings->module.name  = "settings";
  settings->module.start = settings_start;
  settings->module.stop  = settings_stop;
  settings->config       = *config;
}

void settings_deinit(FAR struct settings_s *settings)
{
  FAR struct settings_load_s *load;

  /* Reads that had not finished as the loop stopped: their registrations
   * were never answered
   */

  while ((load = settings->loads) != NULL)
    {
      settings->loads = load->next;
      free(load->data);
      free(load);
    }

  free(settings->buffer);
  settings->buffer = NULL;
  free(settings->defaults);
  settings->defaults = NULL;
  settings_store_deinit(&settings->store);
}
