/****************************************************************************
 * pnut-os/src/sensors/location.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Location (RFC 0035): the GNSS receiver Board's description names, over
 * u-blox's UBX.  The receiver is on only while a caller waits for a
 * position or follows them, and a little after.  Switched off, it forgets
 * everything, so at each start it is given the time (when the clock came
 * from a network or the satellites), the last position, and the orbit
 * data it dumped before it was switched off last, which the state file in
 * config.dir keeps; nothing is fetched or sent.  Positions are for those
 * allowed (rounded unless precise); the location topic says only whether
 * the receiver is on and has a fix, and gnss_time gives Time the
 * satellites' time.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/msg.h>
#include <pnut/worker.h>

#include "location.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LOCATION_SERVICE   "location"
#define LOCATION_TICK_MS   250
#define LOCATION_FLUSH_MS  20
#define LOCATION_CFG_MS    1000     /* For the configuration's ack */
#define LOCATION_CFG_TRIES 5
#define LOCATION_MUTE_MS   5000     /* For the receiver's first word */
#define LOCATION_TIMEOUT   60000    /* A Current's, when none is asked */
#define LOCATION_DUMP_MS   10000    /* For the orbit data's dump */
#define LOCATION_TIME_MS   60000    /* Between gnss_time readings */
#define LOCATION_ACK_MS    3000     /* For the saved orbit data's acks */
#define LOCATION_SLACK_MS  500      /* Half an epoch: a Follow's next fix */
#define LOCATION_ARG_MAX   86400    /* A caller's seconds, at most */

/* The clock's accuracy given to the receiver: NTP's or the satellites'
 * time drifted since it was read; the network's is often further off.
 * Claiming better than the clock is slows the fix.
 */

#define LOCATION_CLOCK_MS  10000
#define LOCATION_NITZ_MS   60000
#define LOCATION_SPEED     30       /* m/s: how far a position strays */

/* A last position strayed further (mm), or orbit data older (s), is not
 * given to the receiver
 */

#define LOCATION_POS_MAX   300000000
#define LOCATION_DUMP_AGE  (3 * 86400)
#define LOCATION_FILE      "state"
#define LOCATION_MAGIC     0x434c4e50 /* "PNLC" */
#define LOCATION_VERSION   1
#define LOCATION_HEADER    44

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t location_now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* A caller's metres in mm, held at the largest count */

static uint32_t location_mm(uint32_t m)
{
  return m < UINT32_MAX / 1000 ? m * 1000 : UINT32_MAX;
}

static void put32(FAR uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(FAR const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void put64(FAR uint8_t *p, uint64_t v)
{
  put32(p, (uint32_t)v);
  put32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t get64(FAR const uint8_t *p)
{
  return get32(p) | ((uint64_t)get32(p + 4) << 32);
}

/* A u-blox receiver of a generation that speaks UBX's configuration
 * interface (CFG-VALSET): M8 and later, by the module's name
 */

static bool location_is_ubx(FAR const char *chip)
{
  return strstr(chip, "-M10") != NULL || strstr(chip, "-M9") != NULL ||
         strstr(chip, "-F9") != NULL || strstr(chip, "-F10") != NULL;
}

static bool location_wanted(FAR struct location_s *loc)
{
  int i;

  for (i = 0; i < LOCATION_WAITERS; i++)
    {
      if (loc->waiters[i].used)
        {
          return true;
        }
    }

  for (i = 0; i < LOCATION_FOLLOWS; i++)
    {
      if (loc->follows[i].used)
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Name: location_publish
 ****************************************************************************/

static void location_publish(FAR struct location_s *loc)
{
  pnut_location_info_t msg;

  if (loc->topic == NULL)
    {
      return;
    }

  memset(&msg, 0, sizeof(msg));
  msg.on         = loc->state != LOCATION_OFF;
  msg.fix        = msg.on && loc->has_fix;
  msg.satellites = msg.on ? loc->sats : 0;
  pnut_location_info_publish(loc->topic, &msg);
}

/****************************************************************************
 * Name: location_position
 *
 * Description:
 *   The last fix as a caller is given it.
 *
 ****************************************************************************/

static void location_position(FAR struct location_s *loc, bool precise,
                              FAR pnut_location_position_t *pos)
{
  FAR const struct location_fix_s *f = &loc->fix;

  memset(pos, 0, sizeof(*pos));
  pos->lat          = f->lat;
  pos->lon          = f->lon;
  pos->alt_mm       = f->alt_mm;
  pos->accuracy_mm  = f->hacc_mm;
  pos->vaccuracy_mm = f->vacc_mm;
  pos->speed_mms    = f->speed_mms;
  pos->heading_e5   = f->heading_e5;
  pos->satellites   = f->sats;
  pos->utc          = f->utc;
  pos->taken_ms     = f->taken_ms;
  if (!precise)
    {
      location_round(pos);
    }
}

/****************************************************************************
 * Name: location_reply
 ****************************************************************************/

static void location_reply(FAR struct location_s *loc,
                           FAR struct location_waiter_s *w, bool fresh)
{
  pnut_location_current_reply_t reply;

  memset(&reply, 0, sizeof(reply));
  reply.fresh        = fresh;
  reply.has_position = true;
  location_position(loc, w->precise, &reply.position);
  pnut_location_current_reply(&loc->server, &w->req, PNUT_STATUS_OK,
                              &reply);
  w->used = false;
}

/* Every waiter failed, every stream ended: the receiver failed */

static void location_fail_all(FAR struct location_s *loc)
{
  int i;

  for (i = 0; i < LOCATION_WAITERS; i++)
    {
      if (loc->waiters[i].used)
        {
          pnut_location_fail(&loc->server, &loc->waiters[i].req,
                             PNUT_STATUS_UNAVAILABLE,
                             PNUT_LOCATION_ERROR_CODE_NO_RECEIVER,
                             "the receiver failed");
          loc->waiters[i].used = false;
        }
    }

  for (i = 0; i < LOCATION_FOLLOWS; i++)
    {
      if (loc->follows[i].used)
        {
          loc->follows[i].used = false;
          pnut_stream_end(&loc->follows[i].stream, PNUT_STATUS_UNAVAILABLE);
        }
    }
}

/****************************************************************************
 * Name: location_flush
 *
 * Description:
 *   Write what the receiver has not taken, then the saved orbit data's
 *   frames as room comes; again shortly while any is left.
 *
 ****************************************************************************/

static void location_flush_tick(FAR struct pnut_loop_s *loop,
                                FAR struct pnut_timer_s *timer,
                                FAR void *arg);

static void location_flush(FAR struct location_s *loc)
{
  FAR const uint8_t *frame;
  size_t len;
  ssize_t n;

  if (loc->state == LOCATION_OFF)
    {
      loc->out_len  = 0;
      loc->blob_pos = loc->blob_len;
      return;
    }

  for (; ; )
    {
      /* The saved frames, whole, while they fit */

      while (loc->blob != NULL && loc->blob_pos + UBX_OVERHEAD <=
             loc->blob_len)
        {
          frame = &loc->blob[loc->blob_pos];
          len   = (size_t)(frame[4] | (frame[5] << 8)) + UBX_OVERHEAD;
          if (frame[0] != UBX_SYNC1 || frame[1] != UBX_SYNC2 ||
              len > UBX_PAYLOAD_MAX + UBX_OVERHEAD ||
              loc->blob_pos + len > loc->blob_len)
            {
              pnut_log(&loc->module, PNUT_LOG_WARNING,
                       "The saved orbit data is damaged: the rest left");
              loc->blob_pos = loc->blob_len;
              break;
            }

          if (loc->out_len + len > sizeof(loc->out))
            {
              break;
            }

          memcpy(&loc->out[loc->out_len], frame, len);
          loc->out_len   += len;
          loc->blob_pos  += len;
          loc->restored++;
          loc->restore_ms = location_now();
        }

      if (loc->out_len == 0)
        {
          break;
        }

      n = loc->config.ops->write(loc->config.arg, loc->out, loc->out_len);
      if (n <= 0)
        {
          break;
        }

      memmove(loc->out, &loc->out[n], loc->out_len - n);
      loc->out_len -= n;
    }

  if ((loc->out_len > 0 || (loc->blob != NULL &&
       loc->blob_pos < loc->blob_len)) && loc->flush == NULL &&
      pnut_timer_start(loc->module.loop, LOCATION_FLUSH_MS, 0,
                       location_flush_tick, loc, &loc->flush) < 0)
    {
      loc->flush = NULL;
    }
}

static void location_flush_tick(FAR struct pnut_loop_s *loop,
                                FAR struct pnut_timer_s *timer,
                                FAR void *arg)
{
  FAR struct location_s *loc = arg;

  loc->flush = NULL;
  location_flush(loc);
}

/* A frame for the receiver, after those queued */

static void location_send(FAR struct location_s *loc,
                          FAR const uint8_t *frame, size_t len)
{
  if (len == 0 || loc->out_len + len > sizeof(loc->out))
    {
      pnut_log(&loc->module, PNUT_LOG_WARNING,
               "A message for the receiver dropped");
      return;
    }

  memcpy(&loc->out[loc->out_len], frame, len);
  loc->out_len += len;
  location_flush(loc);
}

/****************************************************************************
 * Name: location_close
 *
 * Description:
 *   Switch the receiver off: the board cuts its rail once nobody has its
 *   device open.
 *
 ****************************************************************************/

static void location_close(FAR struct location_s *loc)
{
  if (loc->fd >= 0)
    {
      pnut_loop_unwatch(loc->module.loop, loc->fd);
      loc->fd = -1;
    }

  if (loc->state != LOCATION_OFF)
    {
      loc->config.ops->close(loc->config.arg);
    }

  if (loc->flush != NULL)
    {
      pnut_timer_cancel(loc->module.loop, loc->flush);
      loc->flush = NULL;
    }

  free(loc->blob);
  free(loc->dump);
  free(loc->job_blob);
  loc->blob     = NULL;
  loc->dump     = NULL;
  loc->job_blob = NULL;
  loc->blob_len = 0;
  loc->blob_pos = 0;
  loc->dump_len = 0;
  loc->out_len  = 0;
  loc->restored = 0;
  loc->restore_acks = 0;
  loc->has_fix  = false;
  loc->sats     = 0;
  loc->state    = LOCATION_OFF;
  pnut_log(&loc->module, PNUT_LOG_DEBUG, "Receiver off");
  location_publish(loc);
}

/****************************************************************************
 * Name: location_load_run, location_save_run
 *
 * Description:
 *   The state file, on the worker: its header (when it was written, the
 *   last fix) and the orbit data's frames.  Written beside, then renamed.
 *
 ****************************************************************************/

static void location_load_run(FAR void *arg)
{
  FAR struct location_s *loc = arg;
  char path[96];
  struct stat st;
  FAR uint8_t *blob = NULL;
  ssize_t n = -1;
  int fd;

  snprintf(path, sizeof(path), "%s/%s", loc->config.dir, LOCATION_FILE);
  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size >= LOCATION_HEADER &&
      st.st_size <= LOCATION_HEADER + LOCATION_DUMP_MAX)
    {
      blob = malloc(st.st_size);
      if (blob != NULL)
        {
          n = read(fd, blob, st.st_size);
        }
    }

  if (fd >= 0)
    {
      close(fd);
    }

  if (blob == NULL || n < LOCATION_HEADER ||
      get32(blob) != LOCATION_MAGIC || blob[4] != LOCATION_VERSION ||
      get32(&blob[40]) != (uint32_t)n - LOCATION_HEADER)
    {
      free(blob);
      loc->job_ret = -ENOENT;
      return;
    }

  loc->job_blob = blob;
  loc->job_len  = n;
  loc->job_ret  = OK;
}

static void location_save_run(FAR void *arg)
{
  FAR struct location_s *loc = arg;
  FAR const struct location_fix_s *f = &loc->job_fix;
  uint8_t head[LOCATION_HEADER];
  char path[96];
  char tmp[100];
  int fd;

  memset(head, 0, sizeof(head));
  put32(&head[0], LOCATION_MAGIC);
  head[4] = LOCATION_VERSION;
  head[5] = f->valid;
  put64(&head[8], (uint64_t)time(NULL));
  put32(&head[16], (uint32_t)f->lat);
  put32(&head[20], (uint32_t)f->lon);
  put32(&head[24], (uint32_t)f->alt_mm);
  put32(&head[28], f->hacc_mm);
  put64(&head[32], (uint64_t)f->utc);
  put32(&head[40], (uint32_t)loc->dump_len);

  mkdir(loc->config.dir, 0700);
  snprintf(path, sizeof(path), "%s/%s", loc->config.dir, LOCATION_FILE);
  snprintf(tmp, sizeof(tmp), "%s.new", path);
  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0)
    {
      loc->job_ret = -errno;
      return;
    }

  loc->job_ret = write(fd, head, sizeof(head)) == sizeof(head) &&
                 (loc->dump_len == 0 ||
                  write(fd, loc->dump, loc->dump_len) ==
                  (ssize_t)loc->dump_len) ? OK : -EIO;
  if (fsync(fd) < 0 && loc->job_ret >= 0)
    {
      loc->job_ret = -errno;
    }

  close(fd);
  if (loc->job_ret >= 0 && rename(tmp, path) < 0)
    {
      loc->job_ret = -errno;
    }

  if (loc->job_ret < 0)
    {
      unlink(tmp);
    }
}

/****************************************************************************
 * Name: location_restore
 *
 * Description:
 *   The saved state read: give the receiver the time if the clock is
 *   good, then the last position with how far it may have strayed, then
 *   the orbit data, which needs the time.
 *
 ****************************************************************************/

static void location_restore(FAR struct location_s *loc)
{
  FAR uint8_t *b = loc->job_blob;
  struct timespec ts;
  uint8_t frame[64];
  uint64_t acc;
  int64_t saved;
  int64_t age;
  size_t n;

  if (!loc->clock_good)
    {
      pnut_log(&loc->module, PNUT_LOG_DEBUG,
               "The clock is not set from a source: a cold start");
      free(b);
      loc->job_blob = NULL;
      return;
    }

  clock_gettime(CLOCK_REALTIME, &ts);
  n = ubx_ini_time(frame, sizeof(frame), ts.tv_sec, ts.tv_nsec,
                   loc->clock_source == PNUT_TIME_INFO_SOURCE_NETWORK ?
                   LOCATION_NITZ_MS : LOCATION_CLOCK_MS);
  location_send(loc, frame, n);
  pnut_log(&loc->module, PNUT_LOG_DEBUG, "The receiver given the time");

  if (b == NULL)
    {
      return;
    }

  /* The last position, its accuracy grown by how far it may have gone */

  saved = (int64_t)get64(&b[32]);
  age   = ts.tv_sec - saved;
  if (b[5] && saved > 0 && age >= 0)
    {
      acc = get32(&b[28]) + (uint64_t)age * LOCATION_SPEED * 1000;
      if (acc <= LOCATION_POS_MAX)
        {
          n = ubx_ini_pos(frame, sizeof(frame), (int32_t)get32(&b[16]),
                          (int32_t)get32(&b[20]),
                          (int32_t)get32(&b[24]) / 10,
                          (uint32_t)(acc / 10));
          location_send(loc, frame, n);
        }
    }

  /* The orbit data, sent as room comes; too old, not at all */

  loc->job_blob = NULL;
  saved = (int64_t)get64(&b[8]);
  if (loc->job_len > LOCATION_HEADER && saved > 0 &&
      ts.tv_sec - saved < LOCATION_DUMP_AGE && ts.tv_sec >= saved)
    {
      loc->blob     = b;
      loc->blob_len = loc->job_len;
      loc->blob_pos = LOCATION_HEADER;
      pnut_log(&loc->module, PNUT_LOG_DEBUG, "Orbit data given: %u bytes",
               (unsigned)(loc->blob_len - LOCATION_HEADER));
      location_flush(loc);
    }
  else
    {
      free(b);
    }
}

/****************************************************************************
 * Name: location_configure, location_started
 *
 * Description:
 *   Once the receiver says something, it is up: ask it for NAV-PVT,
 *   acknowledgements of the aiding (which end the orbit data's dump) and
 *   its own orbit prediction, again until it acknowledges.  Configured,
 *   and the saved state read, it is given the state.
 *
 ****************************************************************************/

static void location_configure(FAR struct location_s *loc)
{
  static const struct ubx_item_s items[] =
  {
    { UBX_KEY_MSGOUT_NAV_PVT_UART1, 1 },
    { UBX_KEY_NAVSPG_ACKAIDING, 1 },
    { UBX_KEY_ANA_USE_ANA, 1 }
  };

  uint8_t frame[64];
  size_t n;

  n = ubx_valset(frame, sizeof(frame), items,
                 sizeof(items) / sizeof(items[0]));
  loc->tries++;
  loc->sent_ms = location_now();
  location_send(loc, frame, n);
}

static void location_started(FAR struct location_s *loc)
{
  if (loc->state == LOCATION_STARTING && loc->configured && loc->loaded)
    {
      loc->state = LOCATION_ON;
      location_restore(loc);
    }
}

/****************************************************************************
 * Name: location_job_done
 ****************************************************************************/

static void location_reconcile(FAR struct location_s *loc);

static void location_job_done(FAR struct pnut_loop_s *loop, FAR void *arg)
{
  FAR struct location_s *loc = arg;

  loc->job = false;
  if (loc->state == LOCATION_STARTING)
    {
      loc->loaded = true;
      location_started(loc);
    }
  else if (loc->state == LOCATION_CLOSING)
    {
      if (loc->job_ret < 0)
        {
          pnut_log(&loc->module, PNUT_LOG_WARNING,
                   "The state not kept: %d", loc->job_ret);
        }

      location_close(loc);
    }

  location_reconcile(loc);
}

/****************************************************************************
 * Name: location_save
 *
 * Description:
 *   Nobody asks any more: have the receiver dump its orbit data, then keep
 *   it with the last fix, then switch off.
 *
 ****************************************************************************/

static void location_write(FAR struct location_s *loc)
{
  loc->job_fix = loc->fix;
  loc->state   = LOCATION_CLOSING;
  loc->job     = true;
  if (pnut_job_submit(loc->module.loop, location_save_run,
                      location_job_done, loc) < 0)
    {
      loc->job = false;
      location_close(loc);
    }
}

static void location_save(FAR struct location_s *loc)
{
  uint8_t frame[UBX_OVERHEAD];
  size_t n;

  loc->dump       = malloc(LOCATION_DUMP_MAX);
  loc->dump_len   = 0;
  loc->dump_count = 0;
  if (loc->dump == NULL || !loc->ubx)
    {
      location_write(loc);
      return;
    }

  loc->state         = LOCATION_SAVING;
  loc->dump_deadline = location_now() + LOCATION_DUMP_MS;
  n = ubx_build(frame, sizeof(frame), UBX_CLASS_MGA, UBX_MGA_DBD, NULL, 0);
  location_send(loc, frame, n);
}

/****************************************************************************
 * Name: location_start
 *
 * Description:
 *   Switch the receiver on, forget what it said before (the driver gives a
 *   new reader what it kept), and read the saved state meanwhile.
 *
 ****************************************************************************/

static void location_readable(FAR struct pnut_loop_s *loop, int fd,
                              uint32_t events, FAR void *arg);

static void location_start(FAR struct location_s *loc)
{
  uint8_t drop[64];
  int ret;

  ret = loc->config.ops->open(loc->config.arg, loc->path);
  if (ret < 0)
    {
      pnut_log(&loc->module, PNUT_LOG_ERROR,
               "The receiver %s does not open: %d", loc->path, ret);
      location_fail_all(loc);
      return;
    }

  loc->fd    = ret;
  loc->state = LOCATION_STARTING;
  while (loc->config.ops->read(loc->config.arg, drop, sizeof(drop)) > 0);

  ret = pnut_loop_watch(loc->module.loop, loc->fd, EPOLLIN,
                        location_readable, loc);
  if (ret < 0)
    {
      pnut_log(&loc->module, PNUT_LOG_ERROR, "Cannot watch it: %d", ret);
      loc->fd = -1;
      location_close(loc);
      location_fail_all(loc);
      return;
    }

  ubx_parser_init(&loc->parser);
  loc->has_fix    = false;
  loc->sats       = 0;
  loc->heard      = false;
  loc->configured = false;
  loc->loaded     = false;
  loc->tries      = 0;
  loc->start_ms   = location_now();
  pnut_log(&loc->module, PNUT_LOG_DEBUG, "Receiver on");
  location_publish(loc);

  loc->job = true;
  if (pnut_job_submit(loc->module.loop, location_load_run,
                      location_job_done, loc) < 0)
    {
      loc->job    = false;
      loc->loaded = true;
    }
}

/****************************************************************************
 * Name: location_reconcile
 *
 * Description:
 *   The receiver on while asked; off a linger after the last caller,
 *   through the save.  A caller during the save keeps it on.
 *
 ****************************************************************************/

static void location_tick(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg);

static void location_reconcile(FAR struct location_s *loc)
{
  bool wanted = location_wanted(loc);

  if (loc->job)
    {
      return;
    }

  switch (loc->state)
    {
      case LOCATION_OFF:
        if (wanted && loc->present)
          {
            location_start(loc);
          }

        break;

      case LOCATION_ON:
        loc->idle_ms = wanted ? 0 : loc->idle_ms != 0 ? loc->idle_ms :
                       location_now();
        break;

      case LOCATION_SAVING:
        if (wanted)
          {
            free(loc->dump);
            loc->dump    = NULL;
            loc->state   = LOCATION_ON;
            loc->idle_ms = 0;
          }

        break;

      default:
        break;
    }

  if (loc->state != LOCATION_OFF && loc->tick == NULL &&
      pnut_timer_start(loc->module.loop, LOCATION_TICK_MS, LOCATION_TICK_MS,
                       location_tick, loc, &loc->tick) < 0)
    {
      loc->tick = NULL;
    }
}

/****************************************************************************
 * Name: location_tick
 *
 * Description:
 *   Callers whose time ran out answered with the best known; the linger
 *   over; a dump that does not end given up.
 *
 ****************************************************************************/

static void location_tick(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct location_s *loc = arg;
  uint64_t now = location_now();
  int i;

  for (i = 0; i < LOCATION_WAITERS; i++)
    {
      FAR struct location_waiter_s *w = &loc->waiters[i];

      if (!w->used || now < w->deadline_ms)
        {
          continue;
        }

      if (loc->fix.valid)
        {
          location_reply(loc, w, false);
        }
      else
        {
          pnut_location_fail(&loc->server, &w->req, PNUT_STATUS_TIMEOUT,
                             PNUT_LOCATION_ERROR_CODE_NO_POSITION,
                             "no position yet");
          w->used = false;
        }
    }

  /* Starting: the receiver mute, or not taking its configuration */

  if (loc->state == LOCATION_STARTING && !loc->heard &&
      now - loc->start_ms >= LOCATION_MUTE_MS)
    {
      pnut_log(&loc->module, PNUT_LOG_ERROR, "The receiver says nothing");
      location_fail_all(loc);
      if (!loc->job)
        {
          location_close(loc);
        }
    }
  else if (loc->state == LOCATION_STARTING && loc->heard &&
           !loc->configured && now - loc->sent_ms >= LOCATION_CFG_MS)
    {
      if (loc->tries < LOCATION_CFG_TRIES)
        {
          location_configure(loc);
        }
      else
        {
          pnut_log(&loc->module, PNUT_LOG_ERROR,
                   "The receiver does not take its configuration");
          loc->configured = true;
          location_started(loc);
        }
    }

  /* Not while the saved orbit data is still given, nor before the
   * receiver has acknowledged it (or for a while): its acknowledgements
   * are the same message as a dump's end
   */

  if (loc->state == LOCATION_ON && !location_wanted(loc) &&
      loc->idle_ms != 0 && now - loc->idle_ms >= loc->config.linger_ms &&
      loc->blob_pos >= loc->blob_len && loc->out_len == 0 &&
      (loc->restore_acks >= loc->restored ||
       now - loc->restore_ms >= LOCATION_ACK_MS))
    {
      loc->idle_ms = 0;
      location_save(loc);
    }
  else if (loc->state == LOCATION_SAVING && now >= loc->dump_deadline)
    {
      /* Each entry stands alone: those heard are kept */

      pnut_log(&loc->module, PNUT_LOG_WARNING,
               "The orbit data's dump did not end: %lu entries kept",
               (unsigned long)loc->dump_count);
      location_write(loc);
    }

  location_reconcile(loc);
  if (loc->state == LOCATION_OFF && !location_wanted(loc) &&
      loc->tick != NULL)
    {
      pnut_timer_cancel(loop, loc->tick);
      loc->tick = NULL;
    }
}

/****************************************************************************
 * Name: location_pvt
 *
 * Description:
 *   A solution: the topic told of a change; a good fix kept, given to the
 *   callers it suits, and its time to Time now and then.
 *
 ****************************************************************************/

static void location_pvt(FAR struct location_s *loc,
                         FAR const struct ubx_pvt_s *pvt)
{
  FAR struct location_fix_s *f = &loc->fix;
  pnut_location_position_t pos;
  pnut_gnss_time_t gt;
  uint64_t now = location_now();
  bool ok;
  int64_t utc;
  int i;
  int ret;

  ok = (pvt->flags & UBX_PVT_FIX_OK) != 0 && pvt->fix >= 2 &&
       pvt->fix <= 4 && (pvt->flags3 & UBX_PVT_INVALID_LLH) == 0;
  if (ok != loc->has_fix || pvt->numsv != loc->sats)
    {
      if (ok != loc->has_fix)
        {
          pnut_log(&loc->module, PNUT_LOG_INFO, ok ? "Fix, %u satellites" :
                   "Fix lost", pvt->numsv);
        }

      loc->has_fix = ok;
      loc->sats    = pvt->numsv;
      location_publish(loc);
    }

  /* The satellites' time, not the receiver's own clock (given it by
   * Location): with a fix, or confirmed
   */

  if (ubx_pvt_utc(pvt, &utc) && pvt->tacc_ns < 1000000000 &&
      (ok || (pvt->flags2 & UBX_PVT_CONFIRMED) == UBX_PVT_CONFIRMED) &&
      loc->gnss_time != NULL &&
      (loc->time_sent_ms == 0 || now - loc->time_sent_ms >=
       LOCATION_TIME_MS))
    {
      /* The epoch's whole second; the message comes after it */

      memset(&gt, 0, sizeof(gt));
      gt.utc         = utc;
      gt.uncertainty = pvt->tacc_ns / 1000000 + 1000;
      gt.taken       = now;
      if (pnut_gnss_time_publish(loc->gnss_time, &gt) >= 0)
        {
          loc->time_sent_ms = now;
        }
    }

  if (!ok)
    {
      return;
    }

  f->valid      = true;
  f->lat        = pvt->lat;
  f->lon        = pvt->lon;
  f->alt_mm     = pvt->hmsl_mm;
  f->hacc_mm    = pvt->hacc_mm;
  f->vacc_mm    = pvt->vacc_mm;
  f->speed_mms  = pvt->gspeed_mms > 0 ? (uint32_t)pvt->gspeed_mms : 0;
  f->heading_e5 = pvt->head_e5 > 0 ? (uint32_t)pvt->head_e5 : 0;
  f->sats       = pvt->numsv;
  f->utc        = ubx_pvt_utc(pvt, &utc) ? utc : 0;
  f->taken_ms   = now;

  for (i = 0; i < LOCATION_WAITERS; i++)
    {
      if (loc->waiters[i].used && (loc->waiters[i].accuracy_mm == 0 ||
          f->hacc_mm <= loc->waiters[i].accuracy_mm))
        {
          location_reply(loc, &loc->waiters[i], true);
        }
    }

  for (i = 0; i < LOCATION_FOLLOWS; i++)
    {
      FAR struct location_follow_s *fl = &loc->follows[i];

      if (!fl->used || (fl->last_ms != 0 && now - fl->last_ms +
                        LOCATION_SLACK_MS < fl->interval_ms) ||
          (fl->accuracy_mm != 0 && f->hacc_mm > fl->accuracy_mm))
        {
          continue;
        }

      location_position(loc, fl->precise, &pos);
      ret = pnut_location_follow_push(&loc->server, &fl->stream, &pos);
      if (ret == -ENOTCONN)
        {
          fl->used = false;
        }
      else if (ret >= 0)
        {
          fl->last_ms = now;
        }
    }
}

/****************************************************************************
 * Name: location_frame
 ****************************************************************************/

static void location_frame(FAR struct location_s *loc,
                           FAR const struct ubx_frame_s *frame)
{
  struct ubx_pvt_s pvt;
  uint32_t count;
  uint8_t info;
  uint8_t msgid;
  uint8_t cls;
  uint8_t id;
  bool ok;

  if (ubx_nav_pvt(frame, &pvt))
    {
      location_pvt(loc, &pvt);
    }
  else if (ubx_ack(frame, &cls, &id, &ok))
    {
      if (!ok)
        {
          pnut_log(&loc->module, PNUT_LOG_WARNING,
                   "The receiver refused 0x%02x 0x%02x", cls, id);
        }

      if (cls == UBX_CLASS_CFG && id == UBX_CFG_VALSET &&
          !loc->configured)
        {
          loc->configured = true;
          location_started(loc);
        }
    }
  else if (loc->state == LOCATION_SAVING && frame->cls == UBX_CLASS_MGA &&
           frame->id == UBX_MGA_DBD)
    {
      loc->dump_count++;
      if (loc->dump_len + frame->len + UBX_OVERHEAD <= LOCATION_DUMP_MAX)
        {
          loc->dump_len += ubx_build(&loc->dump[loc->dump_len],
                                     LOCATION_DUMP_MAX - loc->dump_len,
                                     frame->cls, frame->id, frame->payload,
                                     frame->len);
        }
    }
  else if (ubx_mga_ack(frame, &ok, &info, &msgid, &count) &&
           msgid == UBX_MGA_DBD)
    {
      /* The dump's end counts its entries.  The acknowledgement of a
       * saved entry given back is the same message, its count the
       * entry's first bytes: it ends nothing, unless every one came
       * already and so it cannot be one (an empty dump's end)
       */

      if (loc->state == LOCATION_SAVING && count == loc->dump_count &&
          (count > 0 || loc->restore_acks >= loc->restored))
        {
          pnut_log(&loc->module, PNUT_LOG_DEBUG,
                   "Orbit data dumped: %lu entries, %u bytes",
                   (unsigned long)count, (unsigned)loc->dump_len);
          location_write(loc);
        }
      else
        {
          loc->restore_acks++;
        }
    }
}

static void location_readable(FAR struct pnut_loop_s *loop, int fd,
                              uint32_t events, FAR void *arg)
{
  FAR struct location_s *loc = arg;
  uint8_t buf[128];
  ssize_t n;
  ssize_t i;

  while (loc->state != LOCATION_OFF &&
         (n = loc->config.ops->read(loc->config.arg, buf,
                                    sizeof(buf))) > 0)
    {
      if (!loc->heard)
        {
          loc->heard = true;
          location_configure(loc);
        }

      for (i = 0; i < n && loc->state != LOCATION_OFF; i++)
        {
          if (ubx_parse(&loc->parser, buf[i]))
            {
              location_frame(loc, &loc->parser.frame);
            }
        }
    }
}

/****************************************************************************
 * Name: location_on_current, location_on_follow
 ****************************************************************************/

static bool location_can(FAR struct location_s *loc,
                         FAR const struct pnut_request_s *req)
{
  if (!loc->present || !loc->ubx)
    {
      pnut_location_fail(&loc->server, req, PNUT_STATUS_UNAVAILABLE,
                         PNUT_LOCATION_ERROR_CODE_NO_RECEIVER,
                         loc->present ? "a receiver of no protocol known" :
                         "no receiver");
      return false;
    }

  return true;
}

static void location_on_current(FAR struct pnut_location_server_s *server,
                                FAR const struct pnut_request_s *req,
                                FAR const pnut_location_current_request_t
                                *in, FAR void *arg)
{
  FAR struct location_s *loc = arg;
  FAR struct location_waiter_s *w = NULL;
  uint64_t now = location_now();
  int i;

  if (!location_can(loc, req))
    {
      return;
    }

  for (i = 0; i < LOCATION_WAITERS; i++)
    {
      if (!loc->waiters[i].used)
        {
          w = &loc->waiters[i];
          break;
        }
    }

  if (w == NULL)
    {
      pnut_location_fail(server, req, PNUT_STATUS_BUSY,
                         PNUT_LOCATION_ERROR_CODE_BUSY, "too many waiting");
      return;
    }

  memset(w, 0, sizeof(*w));
  w->req         = *req;
  w->accuracy_mm = location_mm(in->accuracy_m);
  w->precise     = in->precise;
  w->deadline_ms = now + (in->timeout_s > 0 ?
                          (uint64_t)MIN(in->timeout_s, LOCATION_ARG_MAX) *
                          1000 : LOCATION_TIMEOUT);

  /* A recent enough position: no need to switch the receiver on */

  if (in->max_age_s > 0 && loc->fix.valid &&
      now - loc->fix.taken_ms <= (uint64_t)in->max_age_s * 1000 &&
      (w->accuracy_mm == 0 || loc->fix.hacc_mm <= w->accuracy_mm))
    {
      location_reply(loc, w, true);
      return;
    }

  w->used = true;
  location_reconcile(loc);
}

static void location_closed(FAR struct pnut_stream_s *stream, int status,
                            FAR void *arg)
{
  FAR struct location_s *loc = arg;
  int i;

  for (i = 0; i < LOCATION_FOLLOWS; i++)
    {
      if (&loc->follows[i].stream == stream)
        {
          loc->follows[i].used = false;
        }
    }

  location_reconcile(loc);
}

static void location_on_follow(FAR struct pnut_location_server_s *server,
                               FAR const struct pnut_request_s *req,
                               FAR const pnut_location_follow_request_t *in,
                               FAR void *arg)
{
  FAR struct location_s *loc = arg;
  FAR struct location_follow_s *fl = NULL;
  int i;

  if (!location_can(loc, req))
    {
      return;
    }

  for (i = 0; i < LOCATION_FOLLOWS; i++)
    {
      if (!loc->follows[i].used)
        {
          fl = &loc->follows[i];
          break;
        }
    }

  if (fl == NULL)
    {
      pnut_location_fail(server, req, PNUT_STATUS_BUSY,
                         PNUT_LOCATION_ERROR_CODE_BUSY,
                         "too many following");
      return;
    }

  memset(fl, 0, sizeof(*fl));
  if (pnut_location_follow_accept(server, req, &fl->stream, NULL,
                                  location_closed, loc) < 0)
    {
      return;
    }

  fl->used        = true;
  fl->interval_ms = (in->interval_s > 0 ?
                     MIN(in->interval_s, LOCATION_ARG_MAX) : 1) * 1000;
  fl->accuracy_mm = location_mm(in->accuracy_m);
  fl->precise     = in->precise;
  location_reconcile(loc);
}

static const struct pnut_location_handlers_s g_location_handlers =
{
  location_on_current,
  location_on_follow
};

/****************************************************************************
 * Name: location_time
 *
 * Description:
 *   The clock's source, as Time announces it: one from a network or the
 *   satellites is good enough to give the receiver.
 *
 ****************************************************************************/

static void location_time(FAR struct pnut_time_change_reader_s *reader,
                          FAR const pnut_time_change_t *msg, FAR void *arg)
{
  FAR struct location_s *loc = arg;

  loc->clock_source = (uint8_t)msg->source;
  loc->clock_good   = msg->source == PNUT_TIME_INFO_SOURCE_NETWORK ||
                      msg->source == PNUT_TIME_INFO_SOURCE_NTP ||
                      msg->source == PNUT_TIME_INFO_SOURCE_GNSS;
}

/****************************************************************************
 * Name: location_found
 ****************************************************************************/

static void location_found(FAR struct location_s *loc,
                           FAR const char *path, FAR const char *chip)
{
  if (path == NULL || path[0] == '\0')
    {
      pnut_log(&loc->module, PNUT_LOG_INFO, "No GNSS receiver");
    }
  else
    {
      strlcpy(loc->path, path, sizeof(loc->path));
      strlcpy(loc->chip, chip != NULL ? chip : "", sizeof(loc->chip));
      loc->present = true;
      loc->ubx     = location_is_ubx(loc->chip);
      pnut_log(&loc->module, loc->ubx ? PNUT_LOG_INFO : PNUT_LOG_WARNING,
               "Receiver: %s, %s%s", loc->chip[0] != '\0' ? loc->chip :
               "unnamed", loc->path, loc->ubx ? "" :
               "; no protocol known for it");
    }

  location_publish(loc);
  pnut_module_ready(&loc->module);
}

static void location_board(FAR struct pnut_board_client_s *client,
                           bool connected, FAR void *arg);

static void location_described(FAR struct pnut_board_client_s *client,
                               int status,
                               FAR const pnut_board_description_t *out,
                               FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct location_s *loc = arg;
  FAR const pnut_board_part_t *part = NULL;
  pb_size_t i;

  if (status == PNUT_STATUS_TIMEOUT)
    {
      pnut_log(&loc->module, PNUT_LOG_WARNING,
               "No answer from Board: asking again");
      location_board(client, true, loc);
      return;
    }

  if (status == PNUT_STATUS_UNAVAILABLE && err == NULL)
    {
      pnut_log(&loc->module, PNUT_LOG_WARNING,
               "Board went away: asked again once it is back");
      return;
    }

  pnut_board_disconnect(&loc->board);
  loc->board_open = false;

  for (i = 0; status == PNUT_STATUS_OK && out != NULL &&
              i < out->parts_count; i++)
    {
      if (out->parts[i].kind == PNUT_BOARD_PART_KIND_GNSS &&
          out->parts[i].device[0] != '\0')
        {
          part = &out->parts[i];
          break;
        }
    }

  location_found(loc, part != NULL ? part->device : NULL,
                 part != NULL ? part->chip : NULL);
}

static void location_board(FAR struct pnut_board_client_s *client,
                           bool connected, FAR void *arg)
{
  FAR struct location_s *loc = arg;
  pnut_board_describe_request_t req;
  int ret;

  if (!connected)
    {
      return;
    }

  memset(&req, 0, sizeof(req));
  ret = pnut_board_describe(client, &req, 0, location_described, loc);
  if (ret < 0)
    {
      pnut_log(&loc->module, PNUT_LOG_WARNING, "Cannot ask Board: %d",
               ret);
      pnut_board_disconnect(&loc->board);
      loc->board_open = false;
      location_found(loc, NULL, NULL);
    }
}

/****************************************************************************
 * Name: location_module_start, location_module_stop
 ****************************************************************************/

static int location_module_start(FAR struct pnut_module_s *module,
                                 FAR struct pnut_loop_s *loop)
{
  FAR struct location_s *loc = (FAR struct location_s *)module;
  int ret;

  ret = pnut_location_info_advertise(loop, &loc->topic);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No location topic: %d", ret);
      loc->topic = NULL;
    }

  ret = pnut_gnss_time_advertise(loop, &loc->gnss_time);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No gnss_time topic: %d", ret);
      loc->gnss_time = NULL;
    }

  ret = pnut_time_change_subscribe(loop, location_time, loc,
                                   &loc->time_reader);
  loc->time_open = ret >= 0;
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No time topic: %d", ret);
    }

  ret = pnut_location_serve(loop, LOCATION_SERVICE, 0, &g_location_handlers,
                            loc, &loc->server);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Cannot serve: %d", ret);
      return ret;
    }

  loc->serving = true;
  if (loc->config.device != NULL)
    {
      location_found(loc, loc->config.device, loc->config.chip);
      return OK;
    }

  ret = pnut_board_connect(loop, loc->config.board, location_board, loc,
                           &loc->board);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No Board: %d; no receiver", ret);
      location_found(loc, NULL, NULL);
      return OK;
    }

  loc->board_open = true;
  return OK;
}

static void location_module_stop(FAR struct pnut_module_s *module,
                                 FAR struct pnut_loop_s *loop)
{
  FAR struct location_s *loc = (FAR struct location_s *)module;
  int i;

  for (i = 0; i < LOCATION_FOLLOWS; i++)
    {
      if (loc->follows[i].used)
        {
          loc->follows[i].used = false;
          pnut_stream_end(&loc->follows[i].stream, PNUT_STATUS_UNAVAILABLE);
        }
    }

  memset(loc->waiters, 0, sizeof(loc->waiters));
  if (loc->tick != NULL)
    {
      pnut_timer_cancel(loop, loc->tick);
      loc->tick = NULL;
    }

  if (loc->board_open)
    {
      pnut_board_disconnect(&loc->board);
      loc->board_open = false;
    }

  if (loc->serving)
    {
      pnut_location_close(&loc->server);
      loc->serving = false;
    }

  if (loc->time_open)
    {
      pnut_time_change_unsubscribe(&loc->time_reader);
      loc->time_open = false;
    }

  /* A job running keeps the receiver until location_deinit() */

  if (!loc->job && loc->state != LOCATION_OFF)
    {
      location_close(loc);
    }
  else if (loc->fd >= 0)
    {
      pnut_loop_unwatch(loop, loc->fd);
      loc->fd = -1;
    }

  if (loc->topic != NULL)
    {
      pnut_topic_unadvertise(loc->topic);
      loc->topic = NULL;
    }

  if (loc->gnss_time != NULL)
    {
      pnut_topic_unadvertise(loc->gnss_time);
      loc->gnss_time = NULL;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void location_init(FAR struct location_s *loc,
                   FAR const struct location_config_s *config)
{
  memset(loc, 0, sizeof(*loc));
  loc->config       = *config;
  loc->fd           = -1;
  loc->module.name  = "location";
  loc->module.start = location_module_start;
  loc->module.stop  = location_module_stop;
}

void location_deinit(FAR struct location_s *loc)
{
  if (loc->state != LOCATION_OFF)
    {
      loc->config.ops->close(loc->config.arg);
      loc->state = LOCATION_OFF;
    }

  free(loc->blob);
  free(loc->dump);
  free(loc->job_blob);
  loc->blob     = NULL;
  loc->dump     = NULL;
  loc->job_blob = NULL;
}

/* Rounded to the grid's nearest point, half away from zero */

void location_round(FAR pnut_location_position_t *pos)
{
  int32_t half = LOCATION_GRID / 2;

  pos->lat = (pos->lat >= 0 ? (pos->lat + half) : (pos->lat - half)) /
             LOCATION_GRID * LOCATION_GRID;
  pos->lon = (pos->lon >= 0 ? (pos->lon + half) : (pos->lon - half)) /
             LOCATION_GRID * LOCATION_GRID;
  pos->alt_mm       = 0;
  pos->accuracy_mm  = LOCATION_GRID_MM;
  pos->vaccuracy_mm = 0;
  pos->speed_mms    = 0;
  pos->heading_e5   = 0;
  pos->approximate  = true;
}
