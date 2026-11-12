/****************************************************************************
 * pnut-os/src/media/mixer.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_MEDIA_MIXER_H
#define __PNUT_OS_MEDIA_MIXER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pnut/compiler.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MIXER_RATE      48000     /* The output's, every sound resampled */
#define MIXER_CHANNELS  2         /* Stereo, both the same (BACKLOG: mono) */
#define MIXER_VOICES    4         /* Sounds at once */
#define MIXER_USES      5         /* pnut_audio_use_t */
#define MIXER_LEVELS    11        /* A volume: 0 to 10 */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A sound in memory: 16-bit samples, interleaved, at its own rate */

struct mixer_sound_s
{
  FAR int16_t *samples;
  uint32_t frames;
  uint32_t rate;
  uint8_t channels;               /* 1 or 2 */
};

struct mixer_voice_s
{
  FAR const struct mixer_sound_s *sound;   /* NULL: free */
  uint32_t handle;
  uint8_t use;
  bool loop;
  uint64_t pos;                   /* In the sound's frames, 32.32 */
  uint64_t step;                  /* Its frames for one of the output's */
};

/* The output's high-pass for the speaker: two first-order sections in a
 * row, a channel
 */

struct mixer_hp_s
{
  float a;                        /* RC / (RC + dt) */
  float x1[MIXER_CHANNELS];
  float x2[MIXER_CHANNELS];
  float y1[MIXER_CHANNELS];
  float y2[MIXER_CHANNELS];
};

struct mixer_s
{
  struct mixer_voice_s voices[MIXER_VOICES];
  uint8_t volume[MIXER_USES];     /* 0 to 10, each use's */
  bool hp;                        /* The high-pass on */
  struct mixer_hp_s filter;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: mixer_init
 *
 * Description:
 *   Set a mixer up, silent, its volumes at 6, with a high-pass at hz (0
 *   for none).
 *
 ****************************************************************************/

void mixer_init(FAR struct mixer_s *m, uint32_t hz);

/****************************************************************************
 * Name: mixer_add
 *
 * Description:
 *   Start a sound of a use, once or looping, under a handle.
 *
 * Returned Value:
 *   OK, or -ENOSPC with every voice taken.
 *
 ****************************************************************************/

int mixer_add(FAR struct mixer_s *m, FAR const struct mixer_sound_s *sound,
              uint8_t use, bool loop, uint32_t handle);

/****************************************************************************
 * Name: mixer_remove
 *
 * Description:
 *   Stop a sound by its handle.
 *
 * Returned Value:
 *   OK, or -ENOENT if none plays under it.
 *
 ****************************************************************************/

int mixer_remove(FAR struct mixer_s *m, uint32_t handle);

/****************************************************************************
 * Name: mixer_uses
 *
 * Description:
 *   A bit for each use playing (1 << use).
 *
 ****************************************************************************/

uint32_t mixer_uses(FAR const struct mixer_s *m);

/****************************************************************************
 * Name: mixer_fill
 *
 * Description:
 *   Mix frames of the output: stereo, 16 bits, interleaved, at MIXER_RATE.
 *   Alarm and ring mute the lower uses while they play, a notification
 *   ducks them by 12 dB; each sound is scaled by its use's volume.  The
 *   sounds that end are freed, and their handles given in ended (up to
 *   MIXER_VOICES).
 *
 * Returned Value:
 *   How many ended.
 *
 ****************************************************************************/

int mixer_fill(FAR struct mixer_s *m, FAR int16_t *out, size_t frames,
               FAR uint32_t *ended);

#endif /* __PNUT_OS_MEDIA_MIXER_H */
