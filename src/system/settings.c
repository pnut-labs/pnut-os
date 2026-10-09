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

/* A file read as it is decoded, and whether reading it failed, which a
 * file that does not decode is told from
 */

struct settings_input_s
{
  int fd;
  int error;                      /* Zero, or the errno of a failed read */
};

/****************************************************************************
 * Name: settings_read
 *
 * Description:
 *   nanopb's input from a file.
 *
 ****************************************************************************/

static bool settings_read(FAR pb_istream_t *stream, FAR pb_byte_t *buf,
                          size_t count)
{
  FAR struct settings_input_s *input = stream->state;
  ssize_t n;

  while (count > 0)
    {
      n = read(input->fd, buf, count);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          input->error = n < 0 ? errno : EIO;
          return false;
        }

      buf   += n;
      count -= n;
    }

  return true;
}

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
 * Name: settings_load
 *
 * Description:
 *   Read an owner's values from its file, once its schema is registered.
 *   It runs on the loop, once for each owner, as the service it belongs
 *   to starts: the file is small, and read as it is decoded.
 *
 ****************************************************************************/

static void settings_load(FAR struct settings_s *settings,
                          FAR const char *owner)
{
  struct settings_input_s input;
  char path[SETTINGS_PATH_MAX];
  char temp[SETTINGS_PATH_MAX];
  pb_istream_t stream;
  struct stat st;

  if (settings_path(settings, owner, "", path) < 0 ||
      settings_path(settings, owner, ".new", temp) < 0)
    {
      settings_store_hold(&settings->store, owner);
      return;
    }

  input.error = 0;
  input.fd    = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (input.fd < 0 && errno == ENOENT &&
      lstat(temp, &st) == 0 && S_ISREG(st.st_mode) &&
      rename(temp, path) == 0)
    {
      /* The device stopped while the file written beside it was being
       * put in its place, after the old one was removed (see
       * settings_write_file()): the new one is whole
       */

      input.fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    }

  if (input.fd < 0)
    {
      if (errno == ENOENT)
        {
          return;
        }

      input.error = errno;
    }
  else if (fstat(input.fd, &st) < 0 || !S_ISREG(st.st_mode))
    {
      input.error = EISDIR;
    }
  else
    {
      memset(&stream, 0, sizeof(stream));
      stream.callback   = settings_read;
      stream.state      = &input;
      stream.bytes_left = st.st_size;

      if (!settings_store_decode(&settings->store, owner, &stream) &&
          input.error == 0)
        {
          /* Read whole, but not as values: kept aside, for whoever wants
           * to look, and written again from what could be read
           */

          if (settings_path(settings, owner, ".bad", temp) == 0 &&
              settings_replaceable(temp) && rename(path, temp) == 0)
            {
              pnut_log(&settings->module, PNUT_LOG_WARNING,
                       "%s: its file does not decode: kept as %s", owner,
                       temp);
              settings_store_touch(&settings->store, owner);
            }
          else
            {
              input.error = EIO;
            }
        }
    }

  if (input.fd >= 0)
    {
      close(input.fd);
    }

  /* There, but not read: never written over while the program runs */

  if (input.error != 0)
    {
      pnut_log(&settings->module, PNUT_LOG_ERROR,
               "%s: cannot read its file (%d): not written until a restart",
               owner, input.error);
      settings_store_hold(&settings->store, owner);
    }
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
 * Name: settings_register_schema
 ****************************************************************************/

static void
settings_register_schema(FAR struct pnut_settings_server_s *server,
                         FAR const struct pnut_request_s *req,
                         FAR const pnut_settings_register_request_t *in,
                         FAR void *arg)
{
  FAR struct settings_s *settings = arg;
  FAR const char *key;
  uint32_t code;
  bool load;
  int ret;

  ret = settings_store_register(&settings->store, in, &load, &key, &code);
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

  if (load)
    {
      settings_load(settings, in->owner);
    }

  /* A schema whole: values may have come from the file, gone back to
   * their defaults, or gone
   */

  if (in->last)
    {
      settings_changed(settings, in->owner, "");
    }

  settings_schedule(settings);
  pnut_settings_register_schema_reply(server, req, PNUT_STATUS_OK, NULL);
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
  int ret;

  ret = settings_store_get(&settings->store, in->owner, in->key,
                           &settings->out.value, &code);
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

  if (in->has_value)
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
  uint32_t code;
  bool changed;
  int ret;

  ret = settings_store_reset(&settings->store, in->owner, in->key,
                             &changed, &code);
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
  int ret;

  ret = settings_store_list(&settings->store, in->owner, in->after,
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
  int ret;

  ret = settings_store_describe(&settings->store, in->owner, in->after,
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
  free(settings->buffer);
  settings->buffer = NULL;
  settings_store_deinit(&settings->store);
}
