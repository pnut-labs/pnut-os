/****************************************************************************
 * pnut-os/src/media/audio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Audio (RFC 0032), a module of the media program: it serves pnut.Audio,
 * playing the built-in sounds by name, each with a use, mixed together
 * (mixer.c) and sent to the speaker by the output's thread (output.c).
 * The sounds are read whole at start; the volumes are its settings.  So
 * far no files handed over, no streams, no microphone and no call route.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/msg.h>
#include <pnut/topic.h>

#include "audio.h"
#include "wav.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Frames mixed under the lock at a time, so the loop never waits long */

#define AUDIO_CHUNK  256

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: audio_publish
 *
 * Description:
 *   Say what plays, if it changed (or always, with force).
 *
 ****************************************************************************/

static void audio_publish(FAR struct audio_s *audio, bool force)
{
  pnut_audio_info_t msg;
  uint32_t uses;

  pthread_mutex_lock(&audio->lock);
  uses = mixer_uses(&audio->mixer);
  pthread_mutex_unlock(&audio->lock);

  if (audio->topic == NULL || (uses == audio->uses && !force))
    {
      return;
    }

  audio->uses = uses;
  memset(&msg, 0, sizeof(msg));
  msg.uses = uses;
  pnut_audio_info_publish(audio->topic, &msg);
}

/****************************************************************************
 * Name: audio_volumes
 *
 * Description:
 *   The volume settings into the mixer: ring for every use but media
 *   (RFC 0032); calls' is not the mixer's.
 *
 ****************************************************************************/

static void audio_volumes(FAR struct audio_s *audio)
{
  FAR const pnut_audio_settings_t *v = &audio->settings.values;
  uint8_t ring  = v->volume_ring < MIXER_LEVELS ? v->volume_ring :
                  MIXER_LEVELS - 1;
  uint8_t media = v->volume_media < MIXER_LEVELS ? v->volume_media :
                  MIXER_LEVELS - 1;

  pthread_mutex_lock(&audio->lock);
  audio->mixer.volume[PNUT_AUDIO_USE_AUDIO_USE_MEDIA]        = media;
  audio->mixer.volume[PNUT_AUDIO_USE_AUDIO_USE_INTERFACE]    = ring;
  audio->mixer.volume[PNUT_AUDIO_USE_AUDIO_USE_NOTIFICATION] = ring;
  audio->mixer.volume[PNUT_AUDIO_USE_AUDIO_USE_RING]         = ring;
  audio->mixer.volume[PNUT_AUDIO_USE_AUDIO_USE_ALARM]        = ring;
  pthread_mutex_unlock(&audio->lock);
}

static void audio_changed(FAR struct pnut_settings_cache_s *cache,
                          int index, FAR void *arg)
{
  audio_volumes(arg);
}

/****************************************************************************
 * Name: audio_sound
 ****************************************************************************/

static FAR const struct mixer_sound_s *
audio_sound(FAR struct audio_s *audio, FAR const char *name)
{
  int i;

  for (i = 0; i < audio->nsounds; i++)
    {
      if (strcmp(audio->sounds[i].name, name) == 0)
        {
          return &audio->sounds[i].sound;
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: audio_load
 *
 * Description:
 *   Read the built-in sounds: each <name>.wav in the directory.
 *
 ****************************************************************************/

static void audio_load(FAR struct audio_s *audio)
{
  FAR struct audio_sound_s *s;
  FAR struct dirent *de;
  FAR DIR *dir;
  char path[96];
  size_t len;
  int ret;

  if (audio->config.sounds == NULL)
    {
      return;
    }

  dir = opendir(audio->config.sounds);
  if (dir == NULL)
    {
      pnut_log(&audio->module, PNUT_LOG_WARNING, "No sounds in %s: %d",
               audio->config.sounds, -errno);
      return;
    }

  while ((de = readdir(dir)) != NULL)
    {
      len = strlen(de->d_name);
      if (len <= 4 || len - 4 >= AUDIO_NAME ||
          strcmp(de->d_name + len - 4, ".wav") != 0)
        {
          continue;
        }

      if (audio->nsounds == AUDIO_SOUNDS)
        {
          pnut_log(&audio->module, PNUT_LOG_WARNING,
                   "More than %d sounds: %s left out", AUDIO_SOUNDS,
                   de->d_name);
          continue;
        }

      s = &audio->sounds[audio->nsounds];
      if (snprintf(path, sizeof(path), "%s/%s", audio->config.sounds,
                   de->d_name) >= (int)sizeof(path))
        {
          continue;               /* The directory's name is too long */
        }

      ret = wav_load(path, &s->sound);
      if (ret < 0)
        {
          pnut_log(&audio->module, PNUT_LOG_WARNING,
                   "The sound %s does not read: %d", path, ret);
          continue;
        }

      memcpy(s->name, de->d_name, len - 4);
      s->name[len - 4] = '\0';
      audio->nsounds++;
    }

  closedir(dir);
}

/****************************************************************************
 * Name: audio_on_play, audio_on_stop
 ****************************************************************************/

static void audio_on_play(FAR struct pnut_audio_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_audio_play_request_t *in,
                          FAR void *arg)
{
  FAR struct audio_s *audio = arg;
  FAR const struct mixer_sound_s *sound;
  pnut_audio_play_reply_t reply;
  uint32_t handle;
  int ret;

  if (in->use < PNUT_AUDIO_USE_AUDIO_USE_MEDIA ||
      in->use > PNUT_AUDIO_USE_AUDIO_USE_ALARM)
    {
      pnut_audio_fail(server, req, PNUT_STATUS_INVALID,
                      PNUT_AUDIO_ERROR_CODE_NONE, "no such use");
      return;
    }

  sound = audio_sound(audio, in->name);
  if (sound == NULL)
    {
      pnut_audio_fail(server, req, PNUT_STATUS_NOTFOUND,
                      PNUT_AUDIO_ERROR_CODE_NO_SOUND, "no such sound");
      return;
    }

  if (!audio->speaker)
    {
      pnut_audio_fail(server, req, PNUT_STATUS_UNAVAILABLE,
                      PNUT_AUDIO_ERROR_CODE_NO_SPEAKER, "no speaker");
      return;
    }

  /* Handles go on from 1, never 0 */

  handle = ++audio->handle;
  if (handle == 0)
    {
      handle = ++audio->handle;
    }

  pthread_mutex_lock(&audio->lock);
  ret = mixer_add(&audio->mixer, sound, (uint8_t)in->use, in->loop,
                  handle);
  pthread_mutex_unlock(&audio->lock);

  if (ret < 0)
    {
      pnut_audio_fail(server, req, PNUT_STATUS_BUSY,
                      PNUT_AUDIO_ERROR_CODE_FULL, "as many sounds play "
                      "as can");
      return;
    }

  audio->config.ops->wake(audio->config.arg);
  audio_publish(audio, false);

  memset(&reply, 0, sizeof(reply));
  reply.handle = handle;
  pnut_audio_play_reply(server, req, PNUT_STATUS_OK, &reply);
}

static void audio_on_stop(FAR struct pnut_audio_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_audio_stop_request_t *in,
                          FAR void *arg)
{
  FAR struct audio_s *audio = arg;
  pnut_audio_reply_t reply;
  int ret;

  pthread_mutex_lock(&audio->lock);
  ret = mixer_remove(&audio->mixer, in->handle);
  pthread_mutex_unlock(&audio->lock);

  if (ret < 0)
    {
      pnut_audio_fail(server, req, PNUT_STATUS_NOTFOUND,
                      PNUT_AUDIO_ERROR_CODE_NOT_PLAYING,
                      "no sound of that handle plays");
      return;
    }

  audio_publish(audio, false);

  memset(&reply, 0, sizeof(reply));
  pnut_audio_stop_reply(server, req, PNUT_STATUS_OK, &reply);
}

static const struct pnut_audio_handlers_s g_audio_handlers =
{
  audio_on_play,
  audio_on_stop
};

/****************************************************************************
 * Name: audio_event
 *
 * Description:
 *   The output's thread said sounds ended, or that the output failed.
 *
 ****************************************************************************/

static void audio_event(FAR struct pnut_loop_s *loop, int fd,
                        uint32_t events, FAR void *arg)
{
  FAR struct audio_s *audio = arg;
  eventfd_t value;
  uint32_t played;
  uint32_t ms;
  int failed;

  eventfd_read(fd, &value);

  pthread_mutex_lock(&audio->lock);
  failed           = audio->failed;
  played           = audio->played;
  ms               = audio->played_ms;
  audio->failed    = 0;
  audio->played    = 0;
  audio->played_ms = 0;
  pthread_mutex_unlock(&audio->lock);

  if (failed < 0)
    {
      pnut_log(&audio->module, PNUT_LOG_WARNING,
               "The speaker failed: %d; its sounds ended", failed);
    }

  if (played > 1)
    {
      pnut_log(&audio->module, PNUT_LOG_DEBUG,
               "Played %" PRIu32 " buffers, %" PRIu32 " us apart",
               played, (uint32_t)((uint64_t)ms * 1000 / (played - 1)));
    }

  audio_publish(audio, false);
}

/****************************************************************************
 * Name: audio_speaker
 *
 * Description:
 *   The speaker known: open its output, and say Audio is ready, with or
 *   without.
 *
 ****************************************************************************/

static void audio_speaker(FAR struct audio_s *audio, FAR const char *path)
{
  int ret;

  if (path == NULL || path[0] == '\0')
    {
      pnut_log(&audio->module, PNUT_LOG_INFO, "No speaker");
      pnut_module_ready(&audio->module);
      return;
    }

  strlcpy(audio->path, path, sizeof(audio->path));
  ret = audio->config.ops->open(audio->config.arg, audio->path, audio);
  if (ret < 0)
    {
      pnut_log(&audio->module, PNUT_LOG_ERROR,
               "The speaker %s does not open: %d", audio->path, ret);
    }
  else
    {
      audio->speaker = true;
      pnut_log(&audio->module, PNUT_LOG_INFO, "Speaker: %s, %d sounds",
               audio->path, audio->nsounds);
    }

  pnut_module_ready(&audio->module);
}

/****************************************************************************
 * Name: audio_board, audio_described
 *
 * Description:
 *   Board asked for its description, once: the first speaker.
 *
 ****************************************************************************/

static void audio_board(FAR struct pnut_board_client_s *client,
                        bool connected, FAR void *arg);

static void audio_described(FAR struct pnut_board_client_s *client,
                            int status,
                            FAR const pnut_board_description_t *out,
                            FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct audio_s *audio = arg;
  FAR const pnut_board_part_t *part = NULL;
  pb_size_t i;

  if (status == PNUT_STATUS_TIMEOUT)
    {
      pnut_log(&audio->module, PNUT_LOG_WARNING,
               "No answer from Board: asking again");
      audio_board(client, true, audio);
      return;
    }

  if (status == PNUT_STATUS_UNAVAILABLE && err == NULL)
    {
      pnut_log(&audio->module, PNUT_LOG_WARNING,
               "Board went away: asked again once it is back");
      return;
    }

  pnut_board_disconnect(&audio->board);
  audio->board_open = false;

  if (status == PNUT_STATUS_OK && out != NULL)
    {
      for (i = 0; i < out->parts_count; i++)
        {
          if (out->parts[i].kind == PNUT_BOARD_PART_KIND_SPEAKER &&
              out->parts[i].device[0] != '\0')
            {
              part = &out->parts[i];
              break;
            }
        }
    }

  audio_speaker(audio, part != NULL ? part->device : NULL);
}

static void audio_board(FAR struct pnut_board_client_s *client,
                        bool connected, FAR void *arg)
{
  FAR struct audio_s *audio = arg;
  pnut_board_describe_request_t req;
  int ret;

  if (!connected)
    {
      return;
    }

  memset(&req, 0, sizeof(req));
  ret = pnut_board_describe(client, &req, 0, audio_described, audio);
  if (ret < 0)
    {
      pnut_log(&audio->module, PNUT_LOG_WARNING, "Cannot ask Board: %d",
               ret);
      pnut_board_disconnect(&audio->board);
      audio->board_open = false;
      audio_speaker(audio, NULL);
    }
}

/****************************************************************************
 * Name: audio_module_start
 ****************************************************************************/

static int audio_module_start(FAR struct pnut_module_s *module,
                              FAR struct pnut_loop_s *loop)
{
  FAR struct audio_s *audio = (FAR struct audio_s *)module;
  int ret;

  audio->eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (audio->eventfd < 0)
    {
      ret = -errno;
      pnut_log(module, PNUT_LOG_ERROR, "No eventfd: %d", ret);
      return ret;
    }

  ret = pnut_loop_watch(loop, audio->eventfd, EPOLLIN, audio_event, audio);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Cannot watch: %d", ret);
      close(audio->eventfd);
      audio->eventfd = -1;
      return ret;
    }

  ret = pnut_audio_settings_open(module, audio_changed, audio,
                                 &audio->settings);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No settings: %d", ret);
    }
  else
    {
      audio->settings_open = true;
    }

  audio_volumes(audio);
  audio_load(audio);

  ret = pnut_audio_info_advertise(loop, &audio->topic);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No audio topic: %d", ret);
      audio->topic = NULL;
    }

  audio_publish(audio, true);

  ret = pnut_audio_serve(loop, AUDIO_SERVICE, 0, &g_audio_handlers, audio,
                         &audio->server);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Cannot serve: %d", ret);
      return ret;
    }

  audio->serving = true;

  if (audio->config.device != NULL)
    {
      audio_speaker(audio, audio->config.device);
      return OK;
    }

  ret = pnut_board_connect(loop, audio->config.board, audio_board, audio,
                           &audio->board);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No Board: %d; no speaker", ret);
      audio_speaker(audio, NULL);
      return OK;
    }

  audio->board_open = true;
  return OK;
}

/****************************************************************************
 * Name: audio_module_stop
 ****************************************************************************/

static void audio_module_stop(FAR struct pnut_module_s *module,
                              FAR struct pnut_loop_s *loop)
{
  FAR struct audio_s *audio = (FAR struct audio_s *)module;
  int i;

  /* The output first: its thread uses the sounds and the mixer */

  if (audio->speaker)
    {
      audio->config.ops->close(audio->config.arg);
      audio->speaker = false;
    }

  if (audio->board_open)
    {
      pnut_board_disconnect(&audio->board);
      audio->board_open = false;
    }

  if (audio->serving)
    {
      pnut_audio_close(&audio->server);
      audio->serving = false;
    }

  if (audio->settings_open)
    {
      pnut_audio_settings_close(&audio->settings);
      audio->settings_open = false;
    }

  if (audio->topic != NULL)
    {
      pnut_topic_unadvertise(audio->topic);
      audio->topic = NULL;
    }

  if (audio->eventfd >= 0)
    {
      pnut_loop_unwatch(loop, audio->eventfd);
      close(audio->eventfd);
      audio->eventfd = -1;
    }

  mixer_init(&audio->mixer, audio->config.highpass);
  for (i = 0; i < audio->nsounds; i++)
    {
      wav_free(&audio->sounds[i].sound);
    }

  audio->nsounds = 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: audio_init
 ****************************************************************************/

void audio_init(FAR struct audio_s *audio,
                FAR const struct audio_config_s *config)
{
  memset(audio, 0, sizeof(*audio));
  audio->config       = *config;
  audio->eventfd      = -1;
  audio->module.name  = "audio";
  audio->module.start = audio_module_start;
  audio->module.stop  = audio_module_stop;
  pthread_mutex_init(&audio->lock, NULL);
  mixer_init(&audio->mixer, config->highpass);
}

/****************************************************************************
 * Name: audio_fill
 ****************************************************************************/

bool audio_fill(FAR struct audio_s *audio, FAR int16_t *out, size_t frames)
{
  uint32_t ended[MIXER_VOICES];
  bool playing = false;
  int nended = 0;
  size_t n;

  while (frames > 0)
    {
      n = frames < AUDIO_CHUNK ? frames : AUDIO_CHUNK;

      pthread_mutex_lock(&audio->lock);
      nended += mixer_fill(&audio->mixer, out, n, ended);
      playing = mixer_uses(&audio->mixer) != 0;
      pthread_mutex_unlock(&audio->lock);

      out    += n * MIXER_CHANNELS;
      frames -= n;
    }

  if (nended > 0)
    {
      eventfd_write(audio->eventfd, 1);
    }

  return playing;
}

/****************************************************************************
 * Name: audio_failed
 ****************************************************************************/

void audio_failed(FAR struct audio_s *audio, int error)
{
  int i;

  pthread_mutex_lock(&audio->lock);
  for (i = 0; i < MIXER_VOICES; i++)
    {
      audio->mixer.voices[i].sound = NULL;
    }

  audio->failed = error;
  pthread_mutex_unlock(&audio->lock);

  eventfd_write(audio->eventfd, 1);
}

/****************************************************************************
 * Name: audio_played
 ****************************************************************************/

void audio_played(FAR struct audio_s *audio, uint32_t buffers, uint32_t ms)
{
  pthread_mutex_lock(&audio->lock);
  audio->played    = buffers;
  audio->played_ms = ms;
  pthread_mutex_unlock(&audio->lock);

  eventfd_write(audio->eventfd, 1);
}
