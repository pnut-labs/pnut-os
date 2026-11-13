/****************************************************************************
 * pnut-os/src/media/output.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Audio's output (RFC 0032): NuttX's audio driver, /dev/audio/pcmN, fed by
 * a thread of its own.  The device stays open with its buffers; it is
 * reserved only while sounds play and for a while after, since the board
 * switches the amplifier on and keeps the chip from light sleep while it
 * is.  Each time: reserved, set to 48 kHz stereo (the codec's reset
 * after a stop takes its volume back to the driver's own), the buffers
 * filled from the mixer and queued, started, the volume set; each buffer
 * that comes back filled again, until two seconds of silence: stopped,
 * the buffers back, released.  Stereo even though the speaker is one:
 * mono plays twice as fast on this I2S driver (BACKLOG).
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <nuttx/audio/audio.h>

#include "audio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The buffers: 4 of 1024 frames, 21 ms each, so a sound starts at most
 * 85 ms after it is asked for (the driver's own default, 8 of 8192 bytes,
 * would be 340 ms), with the I2S driver's two in flight and the time of
 * one to fill.  Then the frames of silence before the output stops.
 */

#define OUTPUT_BUFFERS   4
#define OUTPUT_BYTES     (1024 * MIXER_CHANNELS * 2)
#define OUTPUT_HOLD      (2 * MIXER_RATE)
#define OUTPUT_VOLUME    730      /* The driver's 0 dB: volume is the
                                   * mixer's */
#define OUTPUT_STACK     4096     /* As nxplayer's: the start's path */
#define OUTPUT_DRAIN_MS  2000     /* A stop's buffers back by then */

/* The thread's own messages, on the queue the driver writes to */

#define OUTPUT_MSG_WAKE  (AUDIO_MSG_USER + 0)
#define OUTPUT_MSG_QUIT  (AUDIO_MSG_USER + 1)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct output_s
{
  FAR struct audio_s *audio;
  int fd;
  mqd_t mq;                       /* The thread's: the driver's and ours */
  mqd_t wake;                     /* The loop's end, never blocking */
  char mqname[24];
  pthread_t thread;
  bool running;
  FAR struct ap_buffer_s *buffers[OUTPUT_BUFFERS];
  bool queued[OUTPUT_BUFFERS];    /* With the driver */
  int nbuffers;
  uint32_t bytes;                 /* A buffer's */

  /* A wake waits in the queue: one at most, so that the queue always
   * has room for the driver, which waits for it (and would, inside a
   * stop, for ever).  Set by the loop alone, cleared by the thread alone,
   * as it takes the wake and before it acts on it: so a wake skipped
   * because one waits finds its sound mixed all the same.  Atomic loads
   * and stores.
   */

  bool woken;

  /* The thread's: a stop asked meanwhile for a start or the end; the
   * driver kept buffers past a stop, so none can be queued again
   */

  bool rewake;
  bool quit;
  bool broken;

  /* The buffers back, and when the first and last came: the clock */

  uint32_t back;
  struct timespec first;
  struct timespec last;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int output_open(FAR void *arg, FAR const char *path,
                       FAR struct audio_s *audio);
static void output_close(FAR void *arg);
static void output_wake(FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct output_s g_output =
{
  .fd   = -1,
  .mq   = (mqd_t)-1,
  .wake = (mqd_t)-1,
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct audio_output_ops_s g_audio_output =
{
  output_open,
  output_close,
  output_wake
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int output_index(FAR struct output_s *o,
                        FAR struct ap_buffer_s *apb)
{
  int i;

  for (i = 0; i < o->nbuffers; i++)
    {
      if (o->buffers[i] == apb)
        {
          return i;
        }
    }

  return -1;
}

static int output_ioctl(FAR struct output_s *o, int cmd, unsigned long arg)
{
  return ioctl(o->fd, cmd, arg) < 0 ? -errno : OK;
}

/* A buffer filled from the mixer; whether anything still plays */

static bool output_fill(FAR struct output_s *o, int i)
{
  FAR struct ap_buffer_s *apb = o->buffers[i];

  apb->nbytes  = o->bytes;
  apb->curbyte = 0;
  apb->flags   = 0;
  return audio_fill(o->audio, (FAR int16_t *)apb->samp,
                    o->bytes / (MIXER_CHANNELS * sizeof(int16_t)));
}

/* A filled buffer given to the driver */

static int output_queue(FAR struct output_s *o, int i)
{
  FAR struct ap_buffer_s *apb = o->buffers[i];
  struct audio_buf_desc_s desc;
  int ret;

  memset(&desc, 0, sizeof(desc));
  desc.numbytes = o->bytes;
  desc.u.buffer = apb;
  ret = output_ioctl(o, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&desc);
  if (ret >= 0)
    {
      o->queued[i] = true;
    }

  return ret;
}

/* A buffer back from the driver */

static int output_back(FAR struct output_s *o, FAR struct ap_buffer_s *apb)
{
  int i = output_index(o, apb);

  if (i >= 0)
    {
      o->queued[i] = false;
      clock_gettime(CLOCK_MONOTONIC, &o->last);
      if (o->back++ == 0)
        {
          o->first = o->last;
        }
    }

  return i;
}

/****************************************************************************
 * Name: output_begin
 *
 * Description:
 *   Reserve, configure, fill, queue and start.
 *
 ****************************************************************************/

static int output_begin(FAR struct output_s *o)
{
  struct audio_caps_desc_s caps;
  int ret;
  int i;

  if (o->broken)
    {
      return -EIO;
    }

  ret = output_ioctl(o, AUDIOIOC_RESERVE, 0);
  if (ret < 0)
    {
      return ret;
    }

  memset(&caps, 0, sizeof(caps));
  caps.caps.ac_len            = sizeof(struct audio_caps_s);
  caps.caps.ac_type           = AUDIO_TYPE_OUTPUT;
  caps.caps.ac_channels       = MIXER_CHANNELS;
  caps.caps.ac_controls.hw[0] = MIXER_RATE & 0xffff;
  caps.caps.ac_controls.b[3]  = MIXER_RATE >> 16;
  caps.caps.ac_controls.b[2]  = 16;
  caps.caps.ac_subtype        = AUDIO_FMT_PCM;
  ret = output_ioctl(o, AUDIOIOC_CONFIGURE, (unsigned long)&caps);
  if (ret < 0)
    {
      goto err;
    }

  o->back = 0;
  for (i = 0; i < o->nbuffers; i++)
    {
      if (!o->queued[i])
        {
          output_fill(o, i);
          ret = output_queue(o, i);
          if (ret < 0)
            {
              goto err;
            }
        }
    }

  ret = output_ioctl(o, AUDIOIOC_START, 0);
  if (ret < 0)
    {
      goto err;
    }

  /* After the start, as nxplayer does: the stop before set it to mute */

  memset(&caps, 0, sizeof(caps));
  caps.caps.ac_len            = sizeof(struct audio_caps_s);
  caps.caps.ac_type           = AUDIO_TYPE_FEATURE;
  caps.caps.ac_format.hw      = AUDIO_FU_VOLUME;
  caps.caps.ac_controls.hw[0] = OUTPUT_VOLUME;
  output_ioctl(o, AUDIOIOC_CONFIGURE, (unsigned long)&caps);
  return OK;

err:
  output_ioctl(o, AUDIOIOC_RELEASE, 0);
  return ret;
}

/****************************************************************************
 * Name: output_end
 *
 * Description:
 *   Stop, wait for the buffers to come back, release; and say how fast
 *   they came.  Much faster than they play is the I2S failing: the
 *   driver hands such buffers back as if played, and says nothing else.
 *
 ****************************************************************************/

static void output_end(FAR struct output_s *o)
{
  struct audio_msg_s msg;
  struct timespec until;
  unsigned int prio;
  uint32_t ms = 0;
  ssize_t n;
  int i;

  if (o->back > 1)
    {
      ms = (uint32_t)((o->last.tv_sec - o->first.tv_sec) * 1000 +
                      (o->last.tv_nsec - o->first.tv_nsec) / 1000000);
    }

  audio_played(o->audio, o->back, ms);
  if (o->back > 8 &&
      (uint64_t)ms * 2 * MIXER_RATE <
      (uint64_t)(o->back - 1) * (o->bytes / (MIXER_CHANNELS * 2)) * 1000)
    {
      audio_failed(o->audio, -EIO);
    }

  output_ioctl(o, AUDIOIOC_STOP, 0);

  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += OUTPUT_DRAIN_MS / 1000;
  for (; ; )
    {
      n = mq_timedreceive(o->mq, (FAR char *)&msg, sizeof(msg), &prio,
                          &until);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n < 0)
        {
          /* Not all back: those the driver keeps cannot be queued again,
           * nor freed while it may use them
           */

          o->broken = true;
          output_ioctl(o, AUDIOIOC_RELEASE, 0);
          audio_failed(o->audio, -ETIMEDOUT);
          return;
        }

      if (n != sizeof(msg) || msg.msg_id == AUDIO_MSG_COMPLETE)
        {
          break;
        }

      if (msg.msg_id == AUDIO_MSG_DEQUEUE)
        {
          output_back(o, msg.u.ptr);
        }
      else if (msg.msg_id == OUTPUT_MSG_WAKE)
        {
          __atomic_store_n(&o->woken, false, __ATOMIC_RELEASE);
          o->rewake = true;
        }
      else if (msg.msg_id == OUTPUT_MSG_QUIT)
        {
          o->quit = true;
        }
    }

  output_ioctl(o, AUDIOIOC_RELEASE, 0);

  /* After the driver's own end, every buffer is back */

  for (i = 0; i < o->nbuffers; i++)
    {
      o->queued[i] = false;
    }
}

/****************************************************************************
 * Name: output_thread
 ****************************************************************************/

static FAR void *output_thread(FAR void *arg)
{
  FAR struct output_s *o = arg;
  struct audio_msg_s msg;
  unsigned int prio;
  uint32_t silent = 0;            /* Frames of it, playing */
  uint32_t frames = o->bytes / (MIXER_CHANNELS * sizeof(int16_t));
  bool playing = false;
  bool sound;
  ssize_t n;
  int ret;
  int i;

  while (!o->quit)
    {
      if (o->rewake && !playing)
        {
          msg.msg_id = OUTPUT_MSG_WAKE;
          o->rewake  = false;
        }
      else
        {
          n = mq_receive(o->mq, (FAR char *)&msg, sizeof(msg), &prio);
          if (n != sizeof(msg))
            {
              continue;
            }
        }

      switch (msg.msg_id)
        {
          case OUTPUT_MSG_WAKE:
            __atomic_store_n(&o->woken, false, __ATOMIC_RELEASE);
            if (!playing)
              {
                ret = output_begin(o);
                if (ret < 0)
                  {
                    audio_failed(o->audio, ret);
                    break;
                  }

                playing = true;
                silent  = 0;
              }
            break;

          case OUTPUT_MSG_QUIT:
            o->quit = true;
            break;

          case AUDIO_MSG_DEQUEUE:
            i = output_back(o, msg.u.ptr);
            if (!playing || i < 0)
              {
                break;
              }

            /* Filled first: a sound added meanwhile plays on, even in the
             * hold's last moment, when its wake was taken while playing
             */

            sound  = output_fill(o, i);
            silent = sound ? 0 : silent + frames;
            if (silent >= OUTPUT_HOLD)
              {
                output_end(o);
                playing = false;
                break;
              }

            ret = output_queue(o, i);
            if (ret < 0)
              {
                output_end(o);
                playing = false;
                audio_failed(o->audio, ret);
              }
            break;

          case AUDIO_MSG_COMPLETE:
            if (playing)
              {
                /* Ended by itself: not by us */

                output_end(o);
                playing = false;
                audio_failed(o->audio, -EIO);
              }
            break;

          default:
            break;
        }
    }

  if (playing)
    {
      output_end(o);
    }

  return NULL;
}

/****************************************************************************
 * Name: output_open
 ****************************************************************************/

static int output_open(FAR void *arg, FAR const char *path,
                       FAR struct audio_s *audio)
{
  FAR struct output_s *o = &g_output;
  struct audio_buf_desc_s desc;
  struct sched_param param;
  struct mq_attr attr;
  pthread_attr_t tattr;
  int ret;
  int i;

  memset(o, 0, sizeof(*o));
  o->audio = audio;
  o->mq    = (mqd_t)-1;
  o->wake  = (mqd_t)-1;

  o->fd = open(path, O_RDWR | O_CLOEXEC);
  if (o->fd < 0)
    {
      return -errno;
    }

  o->nbuffers = OUTPUT_BUFFERS;
  o->bytes    = OUTPUT_BYTES;

  /* The queue: every buffer, the driver's end, and our own messages */

  snprintf(o->mqname, sizeof(o->mqname), "/pnut-audio-%d", (int)getpid());
  memset(&attr, 0, sizeof(attr));
  attr.mq_maxmsg  = o->nbuffers + 8;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  o->mq = mq_open(o->mqname, O_RDWR | O_CREAT, 0600, &attr);
  if (o->mq == (mqd_t)-1)
    {
      ret = -errno;
      goto err;
    }

  o->wake = mq_open(o->mqname, O_WRONLY | O_NONBLOCK);
  if (o->wake == (mqd_t)-1)
    {
      ret = -errno;
      goto err;
    }

  ret = output_ioctl(o, AUDIOIOC_REGISTERMQ, (unsigned long)o->mq);
  if (ret < 0)
    {
      goto err;
    }

  for (i = 0; i < o->nbuffers; i++)
    {
      memset(&desc, 0, sizeof(desc));
      desc.numbytes  = OUTPUT_BYTES;
      desc.u.pbuffer = &o->buffers[i];
      if (ioctl(o->fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&desc) !=
          sizeof(desc))
        {
          ret = -ENOMEM;
          goto err;
        }
    }

  pthread_attr_init(&tattr);
  param.sched_priority = sched_get_priority_max(SCHED_FIFO) - 9;
  pthread_attr_setinheritsched(&tattr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&tattr, SCHED_FIFO);
  pthread_attr_setschedparam(&tattr, &param);
  pthread_attr_setstacksize(&tattr, OUTPUT_STACK);
  ret = -pthread_create(&o->thread, &tattr, output_thread, o);
  pthread_attr_destroy(&tattr);
  if (ret < 0)
    {
      goto err;
    }

  pthread_setname_np(o->thread, "audio");
  o->running = true;
  return OK;

err:
  output_close(o);
  return ret;
}

/****************************************************************************
 * Name: output_close
 ****************************************************************************/

static void output_close(FAR void *arg)
{
  FAR struct output_s *o = &g_output;
  struct audio_buf_desc_s desc;
  struct audio_msg_s msg;
  int i;

  if (o->running)
    {
      memset(&msg, 0, sizeof(msg));
      msg.msg_id = OUTPUT_MSG_QUIT;
      mq_send(o->mq, (FAR const char *)&msg, sizeof(msg), 0);
      pthread_join(o->thread, NULL);
      o->running = false;
    }

  /* Every buffer freed, but those a broken driver may still use: left */

  for (i = 0; i < o->nbuffers; i++)
    {
      if (o->buffers[i] != NULL && !(o->broken && o->queued[i]))
        {
          memset(&desc, 0, sizeof(desc));
          desc.u.buffer = o->buffers[i];
          ioctl(o->fd, AUDIOIOC_FREEBUFFER, (unsigned long)&desc);
          o->buffers[i] = NULL;
        }
    }

  if (o->fd >= 0)
    {
      ioctl(o->fd, AUDIOIOC_UNREGISTERMQ, 0);
      close(o->fd);
      o->fd = -1;
    }

  if (o->wake != (mqd_t)-1)
    {
      mq_close(o->wake);
      o->wake = (mqd_t)-1;
    }

  if (o->mq != (mqd_t)-1)
    {
      mq_close(o->mq);
      mq_unlink(o->mqname);
      o->mq = (mqd_t)-1;
    }
}

/****************************************************************************
 * Name: output_wake
 *
 * Description:
 *   The loop: a sound was added.  Never blocks.  A wake not yet taken
 *   is enough: the thread mixes every sound added before it takes it.
 *
 ****************************************************************************/

static void output_wake(FAR void *arg)
{
  FAR struct output_s *o = &g_output;
  struct audio_msg_s msg;

  if (o->wake == (mqd_t)-1 || __atomic_load_n(&o->woken, __ATOMIC_ACQUIRE))
    {
      return;
    }

  __atomic_store_n(&o->woken, true, __ATOMIC_RELEASE);
  memset(&msg, 0, sizeof(msg));
  msg.msg_id = OUTPUT_MSG_WAKE;
  if (mq_send(o->wake, (FAR const char *)&msg, sizeof(msg), 0) < 0)
    {
      __atomic_store_n(&o->woken, false, __ATOMIC_RELEASE);
    }
}
