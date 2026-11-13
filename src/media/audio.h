/****************************************************************************
 * pnut-os/src/media/audio.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_MEDIA_AUDIO_H
#define __PNUT_OS_MEDIA_AUDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pnut/module.h>

#include "pnut/audio.pnut.h"
#include "pnut/audio.settings.h"
#include "pnut/audio.topics.h"
#include "pnut/board.pnut.h"

#include "mixer.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AUDIO_SERVICE  "audio"
#define AUDIO_SOUNDS   8          /* Built-in sounds, at most */
#define AUDIO_NAME     16         /* A sound's name, with its NUL */
#define AUDIO_PATH     32

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct audio_s;

/* The speaker's output: its driver on the device, a fake in the tests.
 * Once open, its thread asks audio_fill() for sound while it plays; wake
 * says a sound was added, so it plays if it was idle.  open and close are
 * called on the loop, wake on the loop too.
 */

struct audio_output_ops_s
{
  CODE int (*open)(FAR void *arg, FAR const char *path,
                   FAR struct audio_s *audio);
  CODE void (*close)(FAR void *arg);
  CODE void (*wake)(FAR void *arg);
};

struct audio_config_s
{
  FAR const struct audio_output_ops_s *ops;
  FAR void *arg;

  /* Board's service, asked for the speaker; or the speaker's device as
   * given, Board not asked (device non-NULL)
   */

  FAR const char *board;
  FAR const char *device;

  FAR const char *sounds;         /* The built-in sounds' directory */
  uint32_t highpass;              /* The speaker's high-pass, Hz; 0: none */
};

struct audio_sound_s
{
  char name[AUDIO_NAME];
  struct mixer_sound_s sound;
};

struct audio_s
{
  struct pnut_module_s module;    /* First */
  struct audio_config_s config;
  struct pnut_audio_server_s server;
  bool serving;
  struct pnut_audio_settings_s settings;
  bool settings_open;
  FAR struct pnut_publisher_s *topic;
  struct pnut_board_client_s board;
  bool board_open;

  /* The speaker, once known and open */

  bool speaker;
  char path[AUDIO_PATH];

  /* The built-in sounds, read whole at start */

  struct audio_sound_s sounds[AUDIO_SOUNDS];
  uint8_t nsounds;

  uint32_t handle;                /* The last given */
  uint32_t uses;                  /* As last published */

  /* The output's thread tells the loop that sounds ended, or that the
   * output failed
   */

  int eventfd;

  /* Shared with the output's thread, under lock */

  pthread_mutex_t lock;
  struct mixer_s mixer;
  int failed;                     /* The output's last failure, not yet
                                   * logged; 0 none */
  uint32_t played;                /* The output's last time playing: its */
  uint32_t played_ms;             /* buffers, the first to the last */
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* NuttX's audio driver, /dev/audio/pcmN (output.c) */

extern const struct audio_output_ops_s g_audio_output;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: audio_init
 *
 * Description:
 *   Set Audio up, as a module to add to a loop.
 *
 ****************************************************************************/

void audio_init(FAR struct audio_s *audio,
                FAR const struct audio_config_s *config);

/****************************************************************************
 * Name: audio_fill
 *
 * Description:
 *   The output's thread: mix frames of sound (stereo, 16 bits, at
 *   MIXER_RATE) into out.
 *
 * Returned Value:
 *   Whether anything still plays.
 *
 ****************************************************************************/

bool audio_fill(FAR struct audio_s *audio, FAR int16_t *out, size_t frames);

/****************************************************************************
 * Name: audio_failed
 *
 * Description:
 *   The output's thread: the output failed (a negated errno value), so
 *   every sound ends.
 *
 ****************************************************************************/

void audio_failed(FAR struct audio_s *audio, int error);

/****************************************************************************
 * Name: audio_played
 *
 * Description:
 *   The output's thread: it stopped, after so many buffers came back over
 *   so many ms from the first to the last; logged at debug, the check of
 *   the output's clock.
 *
 ****************************************************************************/

void audio_played(FAR struct audio_s *audio, uint32_t buffers,
                  uint32_t ms);

#endif /* __PNUT_OS_MEDIA_AUDIO_H */
