/****************************************************************************
 * pnut-os/src/media/mixer.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Audio's mixer (RFC 0032): every sound playing, resampled to the
 * output's rate by linear interpolation, scaled by its use's volume, ducked
 * or muted under a higher use, summed, high-passed for the speaker, and
 * clipped.  Pure computation: the output's thread calls it with a buffer.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <string.h>

#include "mixer.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The uses, as pnut_audio_use_t numbers them */

#define USE_NOTIFICATION  2
#define USE_RING          3
#define USE_ALARM         4

#define GAIN_ONE          32768   /* Q15 */
#define GAIN_DUCKED       8192    /* -12 dB */

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* A volume's gain, Q15: 10 is 0 dB, each step 3 dB less, 0 silent */

static const uint16_t g_mixer_gain[MIXER_LEVELS] =
{
  0, 1463, 2067, 2920, 4125, 5827, 8231, 11627, 16423, 23197, 32767
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* A sample of a voice's sound at its position, interpolated: channel 0 or
 * 1 (a mono sound's one for both)
 */

static int32_t mixer_sample(FAR const struct mixer_voice_s *v, int channel)
{
  FAR const struct mixer_sound_s *s = v->sound;
  uint32_t i = (uint32_t)(v->pos >> 32);
  uint32_t j = i + 1;
  int32_t frac = (int32_t)((v->pos >> 17) & 0x7fff);   /* Q15 */
  int32_t a;
  int32_t b;
  int c = s->channels == 2 ? channel : 0;

  if (i >= s->frames)
    {
      i = s->frames - 1;          /* Never past the end, whatever pos is */
    }

  if (j >= s->frames)
    {
      j = v->loop ? 0 : i;
    }

  a = s->samples[i * s->channels + c];
  b = s->samples[j * s->channels + c];
  return a + (((b - a) * frac) >> 15);
}

/* The output's high-pass: two first-order sections in a row, 12 dB an
 * octave below hz; no trigonometry: a = RC / (RC + dt)
 */

static void mixer_hp_init(FAR struct mixer_hp_s *f, uint32_t hz)
{
  float rc = 1.0f / (2.0f * 3.14159265f * (float)hz);
  float dt = 1.0f / (float)MIXER_RATE;

  memset(f, 0, sizeof(*f));
  f->a = rc / (rc + dt);
}

static float mixer_hp(FAR struct mixer_hp_s *f, int c, float x)
{
  float y1;
  float y2;

  /* y[n] = a * (y[n-1] + x[n] - x[n-1]), twice */

  y1 = f->a * (f->y1[c] + x - f->x1[c]);
  f->x1[c] = x;
  f->y1[c] = y1;

  y2 = f->a * (f->y2[c] + y1 - f->x2[c]);
  f->x2[c] = y1;
  f->y2[c] = y2;
  return y2;
}

static int16_t mixer_clip(float x)
{
  return x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : (int16_t)x;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void mixer_init(FAR struct mixer_s *m, uint32_t hz)
{
  int i;

  memset(m, 0, sizeof(*m));
  for (i = 0; i < MIXER_USES; i++)
    {
      m->volume[i] = 6;
    }

  m->hp = hz > 0;
  if (m->hp)
    {
      mixer_hp_init(&m->filter, hz);
    }
}

int mixer_add(FAR struct mixer_s *m, FAR const struct mixer_sound_s *sound,
              uint8_t use, bool loop, uint32_t handle)
{
  FAR struct mixer_voice_s *v;
  int i;

  if (sound == NULL || sound->frames == 0 || sound->rate == 0 ||
      (sound->channels != 1 && sound->channels != 2) || use >= MIXER_USES)
    {
      return -EINVAL;
    }

  for (i = 0; i < MIXER_VOICES; i++)
    {
      v = &m->voices[i];
      if (v->sound == NULL)
        {
          v->sound  = sound;
          v->handle = handle;
          v->use    = use;
          v->loop   = loop;
          v->pos    = 0;
          v->step   = ((uint64_t)sound->rate << 32) / MIXER_RATE;
          return OK;
        }
    }

  return -ENOSPC;
}

int mixer_remove(FAR struct mixer_s *m, uint32_t handle)
{
  int i;

  for (i = 0; i < MIXER_VOICES; i++)
    {
      if (m->voices[i].sound != NULL && m->voices[i].handle == handle)
        {
          m->voices[i].sound = NULL;
          return OK;
        }
    }

  return -ENOENT;
}

uint32_t mixer_uses(FAR const struct mixer_s *m)
{
  uint32_t uses = 0;
  int i;

  for (i = 0; i < MIXER_VOICES; i++)
    {
      if (m->voices[i].sound != NULL)
        {
          uses |= 1u << m->voices[i].use;
        }
    }

  return uses;
}

int mixer_fill(FAR struct mixer_s *m, FAR int16_t *out, size_t frames,
               FAR uint32_t *ended)
{
  int32_t gain[MIXER_VOICES];
  FAR struct mixer_voice_s *v;
  uint32_t uses = mixer_uses(m);
  int nended = 0;
  int top = -1;
  int32_t acc[MIXER_CHANNELS];
  uint8_t level;
  size_t n;
  int c;
  int i;

  /* The highest use playing mutes (alarm, ring) or ducks (notification)
   * the lower ones
   */

  for (i = MIXER_USES - 1; i >= 0; i--)
    {
      if (uses & (1u << i))
        {
          top = i;
          break;
        }
    }

  for (i = 0; i < MIXER_VOICES; i++)
    {
      v = &m->voices[i];
      if (v->sound == NULL)
        {
          continue;
        }

      level = m->volume[v->use] < MIXER_LEVELS ? m->volume[v->use] :
              MIXER_LEVELS - 1;
      if (v->use == USE_ALARM && level == 0)
        {
          level = 1;              /* An alarm is never silent */
        }

      gain[i] = g_mixer_gain[level];
      if (v->use < top && top >= USE_RING)
        {
          gain[i] = 0;
        }
      else if (v->use < top && top == USE_NOTIFICATION)
        {
          gain[i] = gain[i] * GAIN_DUCKED / GAIN_ONE;
        }
    }

  for (n = 0; n < frames; n++)
    {
      acc[0] = 0;
      acc[1] = 0;

      for (i = 0; i < MIXER_VOICES; i++)
        {
          v = &m->voices[i];
          if (v->sound == NULL)
            {
              continue;
            }

          if ((v->pos >> 32) >= v->sound->frames)
            {
              if (!v->loop)
                {
                  v->sound = NULL;
                  if (nended < MIXER_VOICES)
                    {
                      ended[nended++] = v->handle;
                    }

                  continue;
                }

              /* Back into the sound, however far past its end: a sound
               * shorter than a step may be passed more than once
               */

              v->pos %= (uint64_t)v->sound->frames << 32;
            }

          for (c = 0; c < MIXER_CHANNELS; c++)
            {
              acc[c] += (mixer_sample(v, c) * gain[i]) >> 15;
            }

          v->pos += v->step;
        }

      for (c = 0; c < MIXER_CHANNELS; c++)
        {
          out[n * MIXER_CHANNELS + c] =
            mixer_clip(m->hp ? mixer_hp(&m->filter, c, (float)acc[c]) :
                       (float)acc[c]);
        }
    }

  return nended;
}
