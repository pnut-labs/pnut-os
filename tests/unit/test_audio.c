/****************************************************************************
 * pnut-os/tests/unit/test_audio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Audio (RFC 0032) on a real loop with Settings, a fake output and sounds
 * written here: what plays, the errors, the volumes.  The test mixes
 * itself, calling audio_fill() as the output's thread would.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cmocka.h>

#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

#include "pnut/settings.pnut.h"

#include "audio.h"
#include "settings.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SETTINGS_SERVICE  "settings"
#define SAMPLE            8000    /* Every sample of the test's sounds */
#define BEEP_FRAMES       160     /* 10 ms at 16 kHz: 480 at 48 kHz */

#define USE_MEDIA         PNUT_AUDIO_USE_AUDIO_USE_MEDIA
#define USE_NOTIFICATION  PNUT_AUDIO_USE_AUDIO_USE_NOTIFICATION
#define USE_RING          PNUT_AUDIO_USE_AUDIO_USE_RING
#define USE_ALARM         PNUT_AUDIO_USE_AUDIO_USE_ALARM

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct aud_s;
typedef CODE bool (*aud_step_t)(FAR struct aud_s *t);

struct aud_s
{
  FAR struct pnut_loop_s *loop;
  aud_step_t steps;
  int last;
  struct settings_s settings;
  struct audio_s audio;
  struct pnut_module_s module;
  struct pnut_audio_client_s client;
  struct pnut_settings_client_s other;
  struct pnut_audio_info_reader_s reader;
  char dir[64];
  char sdir[80];
  char sounds[80];
  int step;
  bool waiting;
  int status;                     /* The last answer's */
  uint32_t code;
  uint32_t handle;
  uint32_t handles[MIXER_VOICES];
  uint32_t uses;                  /* The topic's last */
  bool open_fails;
  bool opened;
  int wakes;
  int16_t out[BEEP_FRAMES * 3 * MIXER_CHANNELS];
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The fake output */

static int fake_open(FAR void *arg, FAR const char *path,
                     FAR struct audio_s *audio)
{
  FAR struct aud_s *t = arg;

  assert_string_equal(path, "/dev/fake_pcm");
  assert_ptr_equal(audio, &t->audio);
  t->opened = !t->open_fails;
  return t->open_fails ? -ENODEV : OK;
}

static void fake_close(FAR void *arg)
{
  FAR struct aud_s *t = arg;

  t->opened = false;
}

static void fake_wake(FAR void *arg)
{
  FAR struct aud_s *t = arg;

  t->wakes++;
}

static const struct audio_output_ops_s g_fake =
{
  fake_open,
  fake_close,
  fake_wake
};

/* A WAV file of n frames at 16 kHz, mono, 16 bits, every sample SAMPLE */

static void write_wav(FAR const char *dir, FAR const char *name,
                      uint32_t n)
{
  char path[128];
  uint8_t head[44];
  uint32_t v32;
  uint16_t v16;
  int16_t s = SAMPLE;
  FILE *f;
  uint32_t i;

  memcpy(head, "RIFF", 4);
  v32 = 36 + n * 2;
  memcpy(head + 4, &v32, 4);
  memcpy(head + 8, "WAVEfmt ", 8);
  v32 = 16;
  memcpy(head + 16, &v32, 4);
  v16 = 1;                        /* PCM */
  memcpy(head + 20, &v16, 2);
  memcpy(head + 22, &v16, 2);     /* One channel */
  v32 = 16000;
  memcpy(head + 24, &v32, 4);
  v32 = 32000;
  memcpy(head + 28, &v32, 4);
  v16 = 2;
  memcpy(head + 32, &v16, 2);
  v16 = 16;
  memcpy(head + 34, &v16, 2);
  memcpy(head + 36, "data", 4);
  v32 = n * 2;
  memcpy(head + 40, &v32, 4);

  snprintf(path, sizeof(path), "%s/%s", dir, name);
  f = fopen(path, "wb");
  assert_non_null(f);
  assert_int_equal(fwrite(head, 1, sizeof(head), f), sizeof(head));
  for (i = 0; i < n; i++)
    {
      assert_int_equal(fwrite(&s, 1, 2, f), 2);
    }

  fclose(f);
}

static void write_file(FAR const char *dir, FAR const char *name,
                       FAR const char *text)
{
  char path[128];
  FILE *f;

  snprintf(path, sizeof(path), "%s/%s", dir, name);
  f = fopen(path, "w");
  assert_non_null(f);
  fputs(text, f);
  fclose(f);
}

/* Calls to Audio and Settings */

static void aud_replied(FAR struct aud_s *t, int status,
                        FAR const pnut_error_t *err)
{
  t->status  = status;
  t->code    = err != NULL ? err->code : 0;
  t->waiting = false;
}

static void aud_played(FAR struct pnut_audio_client_s *client, int status,
                       FAR const pnut_audio_play_reply_t *out,
                       FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct aud_s *t = arg;

  t->handle = status == PNUT_STATUS_OK ? out->handle : 0;
  aud_replied(t, status, err);
}

static void aud_stopped(FAR struct pnut_audio_client_s *client, int status,
                        FAR const pnut_audio_reply_t *out,
                        FAR const pnut_error_t *err, FAR void *arg)
{
  aud_replied(arg, status, err);
}

static void aud_play(FAR struct aud_s *t, FAR const char *name, int use,
                     bool loop)
{
  pnut_audio_play_request_t req;

  memset(&req, 0, sizeof(req));
  strlcpy(req.name, name, sizeof(req.name));
  req.use    = use;
  req.loop   = loop;
  t->waiting = true;
  assert_int_equal(pnut_audio_play(&t->client, &req, 0, aud_played, t), 0);
}

static void aud_stop(FAR struct aud_s *t, uint32_t handle)
{
  pnut_audio_stop_request_t req;

  memset(&req, 0, sizeof(req));
  req.handle = handle;
  t->waiting = true;
  assert_int_equal(pnut_audio_stop(&t->client, &req, 0, aud_stopped, t),
                   0);
}

static void aud_set_done(FAR struct pnut_settings_client_s *client,
                         int status, FAR const pnut_settings_reply_t *out,
                         FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct aud_s *t = arg;

  assert_int_equal(status, PNUT_STATUS_OK);
  t->waiting = false;
}

static void aud_set(FAR struct aud_s *t, FAR const char *key, int value)
{
  pnut_settings_set_request_t req;

  memset(&req, 0, sizeof(req));
  strlcpy(req.owner, PNUT_AUDIO_SETTINGS_OWNER, sizeof(req.owner));
  strlcpy(req.key, key, sizeof(req.key));
  req.has_value             = true;
  req.value.which_value     = PNUT_SETTING_VALUE_INTEGER_TAG;
  req.value.value.integer   = value;
  t->waiting = true;
  assert_int_equal(pnut_settings_set(&t->other, &req, 0, aud_set_done, t),
                   0);
}

/* The output's thread's part: frames mixed; whether anything plays */

static bool aud_fill(FAR struct aud_s *t, size_t frames)
{
  assert_true(frames <= sizeof(t->out) / sizeof(t->out[0]) /
                        MIXER_CHANNELS);
  memset(t->out, 0x55, sizeof(t->out));
  return audio_fill(&t->audio, t->out, frames);
}

static void aud_expect(FAR struct aud_s *t, int status, uint32_t code)
{
  assert_int_equal(t->status, status);
  assert_int_equal(t->code, code);
}

/****************************************************************************
 * Name: aud_plays
 *
 * Description:
 *   A notification plays at the ring volume (6: -12 dB), both channels,
 *   resampled from 16 kHz; the topic says a notification plays, then that
 *   nothing does once the sound is over.
 *
 ****************************************************************************/

static bool aud_plays(FAR struct aud_s *t)
{
  int16_t expect = SAMPLE * 8231 / 32768;

  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        assert_true(t->opened);
        assert_int_equal(t->audio.nsounds, 2);
        assert_int_equal(t->uses, 0);
        aud_play(t, "beep", USE_NOTIFICATION, false);
        break;

      case 1:
        aud_expect(t, PNUT_STATUS_OK, 0);
        assert_int_equal(t->handle, 1);
        assert_int_equal(t->wakes, 1);
        if (t->uses != 1u << USE_NOTIFICATION)
          {
            return false;
          }

        /* 160 frames at 16 kHz: 480 at 48 kHz (and one more, the step
         * rounded down), then over
         */

        assert_true(aud_fill(t, BEEP_FRAMES * 3));
        assert_int_equal(t->out[0], expect);
        assert_int_equal(t->out[1], expect);
        assert_int_equal(t->out[(BEEP_FRAMES * 3 - 1) * 2], expect);
        assert_false(aud_fill(t, 4));
        assert_int_equal(t->out[3 * MIXER_CHANNELS], 0);
        break;

      case 2:
        if (t->uses != 0)
          {
            return false;
          }

        /* A second, looping: its own handle; over only when stopped */

        aud_play(t, "beep", USE_MEDIA, true);
        break;

      case 3:
        aud_expect(t, PNUT_STATUS_OK, 0);
        assert_int_equal(t->handle, 2);
        assert_true(aud_fill(t, BEEP_FRAMES * 3));
        assert_true(aud_fill(t, BEEP_FRAMES * 3));
        assert_int_equal(t->out[(BEEP_FRAMES * 3 - 1) * 2], expect);
        aud_stop(t, 2);
        break;

      case 4:
        aud_expect(t, PNUT_STATUS_OK, 0);
        assert_false(aud_fill(t, 4));
        if (t->uses != 0)
          {
            return false;
          }
        break;
    }

  return true;
}

/****************************************************************************
 * Name: aud_errors
 *
 * Description:
 *   No such sound, no such use, no such handle; four sounds at once and
 *   not five; the sounds that play mixed; the output's failure ends them
 *   all.
 *
 ****************************************************************************/

static bool aud_errors(FAR struct aud_s *t)
{
  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        aud_play(t, "nope", USE_MEDIA, false);
        break;

      case 1:
        aud_expect(t, PNUT_STATUS_NOTFOUND, PNUT_AUDIO_ERROR_CODE_NO_SOUND);
        aud_play(t, "beep", 9, false);
        break;

      case 2:
        aud_expect(t, PNUT_STATUS_INVALID, PNUT_AUDIO_ERROR_CODE_NONE);
        aud_stop(t, 999);
        break;

      case 3:
        aud_expect(t, PNUT_STATUS_NOTFOUND,
                   PNUT_AUDIO_ERROR_CODE_NOT_PLAYING);
        aud_play(t, "long", USE_MEDIA, true);
        break;

      case 4:
      case 5:
      case 6:
        aud_expect(t, PNUT_STATUS_OK, 0);
        t->handles[t->step - 4] = t->handle;
        aud_play(t, "long", USE_MEDIA, true);
        break;

      case 7:
        aud_expect(t, PNUT_STATUS_OK, 0);
        t->handles[3] = t->handle;
        aud_play(t, "beep", USE_ALARM, false);
        break;

      case 8:
        aud_expect(t, PNUT_STATUS_BUSY, PNUT_AUDIO_ERROR_CODE_FULL);
        aud_stop(t, t->handles[0]);
        break;

      case 9:
        aud_expect(t, PNUT_STATUS_OK, 0);

        /* Three play, each at media's 6 */

        assert_true(aud_fill(t, 8));
        assert_int_equal(t->out[0], 3 * (SAMPLE * 8231 / 32768));
        assert_int_equal(t->uses, 1u << USE_MEDIA);
        audio_failed(&t->audio, -EIO);
        break;

      case 10:
        if (t->uses != 0)
          {
            return false;
          }

        assert_false(aud_fill(t, 4));
        assert_int_equal(t->out[0], 0);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: aud_volumes
 *
 * Description:
 *   The ring volume at 0 silences a ring, but an alarm plays at 1; the
 *   media volume at 10 plays a sound as it is.
 *
 ****************************************************************************/

static bool aud_volumes(FAR struct aud_s *t)
{
  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        aud_set(t, "volume.ring", 0);
        break;

      case 1:
        if (t->audio.mixer.volume[USE_RING] != 0)
          {
            return false;           /* Not yet told */
          }

        aud_play(t, "long", USE_RING, false);
        break;

      case 2:
        aud_expect(t, PNUT_STATUS_OK, 0);
        assert_true(aud_fill(t, 8));
        assert_int_equal(t->out[0], 0);
        aud_stop(t, t->handle);
        break;

      case 3:
        aud_expect(t, PNUT_STATUS_OK, 0);
        aud_play(t, "long", USE_ALARM, false);
        break;

      case 4:
        aud_expect(t, PNUT_STATUS_OK, 0);
        assert_true(aud_fill(t, 8));
        assert_int_equal(t->out[0], SAMPLE * 1463 / 32768);
        aud_stop(t, t->handle);
        break;

      case 5:
        aud_expect(t, PNUT_STATUS_OK, 0);
        aud_set(t, "volume.media", 10);
        break;

      case 6:
        if (t->audio.mixer.volume[USE_MEDIA] != 10)
          {
            return false;
          }

        aud_play(t, "long", USE_MEDIA, false);
        break;

      case 7:
        aud_expect(t, PNUT_STATUS_OK, 0);
        assert_true(aud_fill(t, 8));
        assert_int_equal(t->out[0], SAMPLE * 32767 / 32768);
        aud_stop(t, t->handle);
        break;

      case 8:
        aud_expect(t, PNUT_STATUS_OK, 0);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: aud_no_speaker
 ****************************************************************************/

static bool aud_no_speaker(FAR struct aud_s *t)
{
  if (t->waiting)
    {
      return false;
    }

  switch (t->step)
    {
      case 0:
        assert_false(t->opened);
        aud_play(t, "beep", USE_MEDIA, false);
        break;

      case 1:
        aud_expect(t, PNUT_STATUS_UNAVAILABLE,
                   PNUT_AUDIO_ERROR_CODE_NO_SPEAKER);
        assert_int_equal(t->wakes, 0);
        break;
    }

  return true;
}

/****************************************************************************
 * Name: aud_run
 ****************************************************************************/

static void aud_tick(FAR struct pnut_loop_s *loop,
                     FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct aud_s *t = arg;

  if (t->step > t->last || !t->steps(t))
    {
      return;
    }

  if (t->step++ == t->last)
    {
      pnut_loop_stop(loop, 0);
    }
}

static void aud_state(FAR struct pnut_audio_info_reader_s *reader,
                      FAR const pnut_audio_info_t *msg, FAR void *arg)
{
  FAR struct aud_s *t = arg;

  t->uses = msg->uses;
}

static int aud_start(FAR struct pnut_module_s *module,
                     FAR struct pnut_loop_s *loop)
{
  FAR struct aud_s *t = (FAR struct aud_s *)
    ((FAR uint8_t *)module - offsetof(struct aud_s, module));

  assert_int_equal(pnut_audio_info_subscribe(loop, aud_state, t,
                                             &t->reader), 0);
  assert_int_equal(pnut_audio_connect(loop, AUDIO_SERVICE, NULL, NULL,
                                      &t->client), 0);
  assert_int_equal(pnut_settings_connect(loop, SETTINGS_SERVICE, NULL, NULL,
                                         &t->other), 0);
  assert_int_equal(pnut_timer_start(loop, 20, 20, aud_tick, t, NULL), 0);
  pnut_module_ready(module);
  return 0;
}

static void aud_stop_module(FAR struct pnut_module_s *module,
                            FAR struct pnut_loop_s *loop)
{
  FAR struct aud_s *t = (FAR struct aud_s *)
    ((FAR uint8_t *)module - offsetof(struct aud_s, module));

  pnut_audio_info_unsubscribe(&t->reader);
  pnut_audio_disconnect(&t->client);
  pnut_settings_disconnect(&t->other);
}

static void aud_timeout(FAR struct pnut_loop_s *loop,
                        FAR struct pnut_timer_s *timer, FAR void *arg)
{
  pnut_loop_stop(loop, -ETIMEDOUT);
}

/* Audio's settings are the media program's */

static int aud_who(FAR void *arg, pid_t pid, FAR const char **programp)
{
  *programp = "media";
  return OK;
}

static bool aud_runs(FAR void *arg, FAR const char *program,
                     FAR const char *service)
{
  return strcmp(program, "media") == 0;
}

static void aud_run(aud_step_t steps, int last, bool open_fails)
{
  static const struct settings_limits_s limits =
  {
    .owners   = 4,
    .settings = 16,
    .texts    = 8,
    .choices  = 16,
    .secrets  = 2,
  };

  FAR struct aud_s *t = calloc(1, sizeof(*t));
  struct pnut_loop_config_s config;
  struct settings_config_s sconfig;
  struct audio_config_s aconfig;
  char cmd[96];
  int ret;

  assert_non_null(t);
  t->steps      = steps;
  t->last       = last;
  t->open_fails = open_fails;

  snprintf(t->dir, sizeof(t->dir), "/tmp/pnut-audio-%d", (int)getpid());
  snprintf(t->sdir, sizeof(t->sdir), "%s/s", t->dir);
  snprintf(t->sounds, sizeof(t->sounds), "%s/sounds", t->dir);
  snprintf(cmd, sizeof(cmd), "rm -rf %s", t->dir);
  assert_int_equal(system(cmd), 0);
  assert_int_equal(mkdir(t->dir, 0700), 0);
  assert_int_equal(mkdir(t->sdir, 0700), 0);
  assert_int_equal(mkdir(t->sounds, 0700), 0);

  /* Two sounds; and what is not one: another file, a broken WAV, a name
   * too long
   */

  write_wav(t->sounds, "beep.wav", BEEP_FRAMES);
  write_wav(t->sounds, "long.wav", 16000);
  write_wav(t->sounds, "a-name-far-too-long.wav", BEEP_FRAMES);
  write_file(t->sounds, "bad.wav", "RIFF");
  write_file(t->sounds, "notes.txt", "not a sound");

  pnut_loop_defaults(&config);
  config.rundir = t->dir;
  assert_int_equal(pnut_loop_create(&config, &t->loop), 0);

  memset(&sconfig, 0, sizeof(sconfig));
  sconfig.dir    = t->sdir;
  sconfig.limits = limits;
  sconfig.delay  = 20;
  sconfig.who    = aud_who;
  sconfig.runs   = aud_runs;
  settings_init(&t->settings, &sconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->settings.module), 0);

  memset(&aconfig, 0, sizeof(aconfig));
  aconfig.ops      = &g_fake;
  aconfig.arg      = t;
  aconfig.device   = "/dev/fake_pcm";
  aconfig.sounds   = t->sounds;
  aconfig.highpass = 0;           /* The samples exact */
  audio_init(&t->audio, &aconfig);
  assert_int_equal(pnut_module_add(t->loop, &t->audio.module), 0);

  t->module.name  = "test";
  t->module.start = aud_start;
  t->module.stop  = aud_stop_module;
  assert_int_equal(pnut_module_add(t->loop, &t->module), 0);

  pnut_timer_start(t->loop, 10000, 0, aud_timeout, NULL, NULL);
  ret = pnut_loop_run(t->loop);
  pnut_loop_destroy(t->loop);
  settings_deinit(&t->settings);

  assert_int_equal(ret, 0);
  assert_int_equal(t->step, last + 1);
  assert_false(t->opened);        /* Closed when the module stopped */
  assert_int_equal(t->audio.nsounds, 0);
  assert_int_equal(system(cmd), 0);
  free(t);
}

static void test_audio_plays(void **state)
{
  aud_run(aud_plays, 4, false);
}

static void test_audio_errors(void **state)
{
  aud_run(aud_errors, 10, false);
}

static void test_audio_volumes(void **state)
{
  aud_run(aud_volumes, 8, false);
}

static void test_audio_no_speaker(void **state)
{
  aud_run(aud_no_speaker, 1, true);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_audio_plays),
    cmocka_unit_test(test_audio_errors),
    cmocka_unit_test(test_audio_volumes),
    cmocka_unit_test(test_audio_no_speaker),
  };

  return cmocka_run_group_tests_name("audio", tests, NULL, NULL);
}
