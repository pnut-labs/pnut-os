/****************************************************************************
 * pnut-os/tests/unit/test_mixer.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Audio's mixer and WAV reader (RFC 0032): pure computation, no device. */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "mixer.h"
#include "wav.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define USE_MEDIA         0
#define USE_INTERFACE     1
#define USE_NOTIFICATION  2
#define USE_RING          3
#define USE_ALARM         4

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* A WAV file in memory: a header, an optional chunk before "fmt ", and the
 * data
 */

static size_t make_wav(FAR uint8_t *buf, uint16_t format, uint16_t channels,
                       uint32_t rate, uint16_t bits,
                       FAR const uint8_t *pcm, uint32_t size, bool list)
{
  size_t off = 0;
  uint32_t riff;

  memcpy(buf + off, "RIFF", 4);
  off += 8;
  memcpy(buf + off, "WAVE", 4);
  off += 4;

  if (list)
    {
      memcpy(buf + off, "LIST", 4);
      buf[off + 4] = 3;           /* An odd size: padded */
      buf[off + 5] = 0;
      buf[off + 6] = 0;
      buf[off + 7] = 0;
      off += 8 + 4;
    }

  memcpy(buf + off, "fmt ", 4);
  buf[off + 4]  = 16;
  buf[off + 5]  = 0;
  buf[off + 6]  = 0;
  buf[off + 7]  = 0;
  buf[off + 8]  = format & 0xff;
  buf[off + 9]  = format >> 8;
  buf[off + 10] = channels;
  buf[off + 11] = 0;
  memcpy(buf + off + 12, &rate, 4);
  memset(buf + off + 16, 0, 6);
  buf[off + 22] = bits;
  buf[off + 23] = 0;
  off += 24;

  memcpy(buf + off, "data", 4);
  memcpy(buf + off + 4, &size, 4);
  memcpy(buf + off + 8, pcm, size);
  off += 8 + size;

  riff = (uint32_t)(off - 8);
  memcpy(buf + 4, &riff, 4);
  return off;
}

/* A sound of n frames at a rate, every sample v, mono */

static void make_sound(FAR struct mixer_sound_s *s, FAR int16_t *samples,
                       uint32_t n, uint32_t rate, int16_t v)
{
  uint32_t i;

  for (i = 0; i < n; i++)
    {
      samples[i] = v;
    }

  s->samples  = samples;
  s->frames   = n;
  s->rate     = rate;
  s->channels = 1;
}

/****************************************************************************
 * Name: test_wav_formats
 ****************************************************************************/

static void test_wav_formats(void **state)
{
  static const uint8_t pcm16[] =
  {
    0x00, 0x10, 0x00, 0xf0           /* 4096, -4096 */
  };

  static const uint8_t pcm8[] =
  {
    0x80, 0xc0, 0x40                 /* 0, 16384, -16384 */
  };

  static const uint8_t pcm24[] =
  {
    0x00, 0x00, 0x20, 0xff, 0xff, 0xff   /* 0x200000, -1 */
  };

  struct mixer_sound_s s;
  uint8_t buf[128];
  size_t n;

  n = make_wav(buf, 1, 1, 16000, 16, pcm16, sizeof(pcm16), true);
  assert_int_equal(wav_parse(buf, n, &s), 0);
  assert_int_equal(s.frames, 2);
  assert_int_equal(s.rate, 16000);
  assert_int_equal(s.channels, 1);
  assert_int_equal(s.samples[0], 4096);
  assert_int_equal(s.samples[1], -4096);
  wav_free(&s);

  n = make_wav(buf, 1, 1, 8000, 8, pcm8, sizeof(pcm8), false);
  assert_int_equal(wav_parse(buf, n, &s), 0);
  assert_int_equal(s.frames, 3);
  assert_int_equal(s.samples[0], 0);
  assert_int_equal(s.samples[1], 16384);
  assert_int_equal(s.samples[2], -16384);
  wav_free(&s);

  n = make_wav(buf, 1, 2, 48000, 24, pcm24, sizeof(pcm24), false);
  assert_int_equal(wav_parse(buf, n, &s), 0);
  assert_int_equal(s.frames, 1);
  assert_int_equal(s.channels, 2);
  assert_int_equal(s.samples[0], 0x2000);
  assert_int_equal(s.samples[1], -1);
  wav_free(&s);

  /* Refused: not PCM, extensible with no sub-format to say PCM, three
   * channels, no data, not RIFF
   */

  n = make_wav(buf, 3, 1, 16000, 16, pcm16, sizeof(pcm16), false);
  assert_int_equal(wav_parse(buf, n, &s), -EINVAL);
  n = make_wav(buf, 0xfffe, 1, 16000, 16, pcm16, sizeof(pcm16), false);
  assert_int_equal(wav_parse(buf, n, &s), -EINVAL);
  n = make_wav(buf, 1, 3, 16000, 16, pcm16, sizeof(pcm16), false);
  assert_int_equal(wav_parse(buf, n, &s), -EINVAL);
  n = make_wav(buf, 1, 1, 16000, 16, pcm16, 0, false);
  assert_int_equal(wav_parse(buf, n, &s), -EINVAL);
  memcpy(buf, "RIFX", 4);
  assert_int_equal(wav_parse(buf, n, &s), -EINVAL);
}

/****************************************************************************
 * Name: test_mixer_basic
 *
 * Description:
 *   A sound at the output's rate at volume 10 passes as it is, stereo, and
 *   ends with its handle; one at half the rate lasts twice as long, its
 *   samples interpolated.
 *
 ****************************************************************************/

static void test_mixer_basic(void **state)
{
  struct mixer_s m;
  struct mixer_sound_s s;
  struct mixer_sound_s r;
  int16_t samples[8];
  int16_t ramp[2] =
    {
      0, 1000
    };

  int16_t out[16 * MIXER_CHANNELS];
  uint32_t ended[MIXER_VOICES];

  mixer_init(&m, 0);
  m.volume[USE_MEDIA] = 10;
  make_sound(&s, samples, 4, MIXER_RATE, 10000);
  assert_int_equal(mixer_add(&m, &s, USE_MEDIA, false, 7), 0);
  assert_int_equal(mixer_uses(&m), 1u << USE_MEDIA);

  assert_int_equal(mixer_fill(&m, out, 6, ended), 1);
  assert_int_equal(ended[0], 7);
  assert_int_equal(out[0], 9999);
  assert_int_equal(out[1], 9999);
  assert_int_equal(out[3 * MIXER_CHANNELS], 9999);
  assert_int_equal(out[4 * MIXER_CHANNELS], 0);   /* Over */
  assert_int_equal(mixer_uses(&m), 0);

  /* Two frames at 24 kHz: four at 48 kHz, the second halfway */

  r.samples  = ramp;
  r.frames   = 2;
  r.rate     = MIXER_RATE / 2;
  r.channels = 1;
  assert_int_equal(mixer_add(&m, &r, USE_MEDIA, false, 8), 0);
  assert_int_equal(mixer_fill(&m, out, 5, ended), 1);
  assert_int_equal(out[0], 0);
  assert_int_equal(out[1 * MIXER_CHANNELS], 499);
  assert_int_equal(out[2 * MIXER_CHANNELS], 999);
  assert_int_equal(out[4 * MIXER_CHANNELS], 0);
}

/****************************************************************************
 * Name: test_mixer_uses
 *
 * Description:
 *   Volumes scale each use; a ring mutes media, a notification ducks it by
 *   12 dB, the interface mixes in as it is; an alarm at volume 0 still
 *   sounds.
 *
 ****************************************************************************/

static void test_mixer_uses(void **state)
{
  struct mixer_s m;
  struct mixer_sound_s a;
  struct mixer_sound_s b;
  int16_t sa[64];
  int16_t sb[64];
  int16_t out[4 * MIXER_CHANNELS];
  uint32_t ended[MIXER_VOICES];

  mixer_init(&m, 0);
  make_sound(&a, sa, 64, MIXER_RATE, 10000);
  make_sound(&b, sb, 64, MIXER_RATE, 1000);

  /* Volume 6: -12 dB */

  assert_int_equal(mixer_add(&m, &a, USE_MEDIA, true, 1), 0);
  mixer_fill(&m, out, 1, ended);
  assert_int_equal(out[0], 10000 * 8231 / 32768);

  /* With a ring: muted, the ring alone */

  m.volume[USE_RING] = 10;
  assert_int_equal(mixer_add(&m, &b, USE_RING, true, 2), 0);
  mixer_fill(&m, out, 1, ended);
  assert_int_equal(out[0], 999);
  assert_int_equal(mixer_remove(&m, 2), 0);

  /* With a notification: ducked, both */

  assert_int_equal(mixer_add(&m, &b, USE_NOTIFICATION, true, 3), 0);
  mixer_fill(&m, out, 1, ended);
  assert_int_equal(out[0], (10000 * (8231 * 8192 / 32768)) / 32768 +
                           1000 * 8231 / 32768);
  assert_int_equal(mixer_remove(&m, 3), 0);

  /* With the interface's: both as they are */

  assert_int_equal(mixer_add(&m, &b, USE_INTERFACE, true, 4), 0);
  mixer_fill(&m, out, 1, ended);
  assert_int_equal(out[0], 10000 * 8231 / 32768 + 1000 * 8231 / 32768);
  assert_int_equal(mixer_remove(&m, 4), 0);
  assert_int_equal(mixer_remove(&m, 1), 0);
  assert_int_equal(mixer_remove(&m, 1), -ENOENT);

  /* An alarm at volume 0: at 1 */

  m.volume[USE_ALARM] = 0;
  assert_int_equal(mixer_add(&m, &a, USE_ALARM, false, 5), 0);
  mixer_fill(&m, out, 1, ended);
  assert_int_equal(out[0], 10000 * 1463 / 32768);
}

/****************************************************************************
 * Name: test_mixer_limits
 *
 * Description:
 *   A looping sound goes on until removed; a fifth voice is refused; the
 *   high-pass takes a constant away.
 *
 ****************************************************************************/

static void test_mixer_limits(void **state)
{
  struct mixer_s m;
  struct mixer_sound_s s;
  struct mixer_sound_s p;
  int16_t samples[4];
  int16_t poison[8];
  int16_t out[400 * MIXER_CHANNELS];
  uint32_t ended[MIXER_VOICES];
  int i;

  mixer_init(&m, 0);
  make_sound(&s, samples, 4, MIXER_RATE, 1000);
  for (i = 0; i < MIXER_VOICES; i++)
    {
      assert_int_equal(mixer_add(&m, &s, USE_MEDIA, true, 10 + i), 0);
    }

  assert_int_equal(mixer_add(&m, &s, USE_MEDIA, true, 20), -ENOSPC);
  assert_int_equal(mixer_fill(&m, out, 100, ended), 0);
  for (i = 0; i < MIXER_VOICES; i++)
    {
      assert_int_equal(mixer_remove(&m, 10 + i), 0);
    }

  /* A looping sound of one frame at three times the output's rate, poison
   * past it: each output frame passes its end three times over, and only
   * its one sample is ever read
   */

  poison[0] = 1000;
  for (i = 1; i < 8; i++)
    {
      poison[i] = -32768;
    }

  p.samples  = poison;
  p.frames   = 1;
  p.rate     = MIXER_RATE * 3;
  p.channels = 1;
  assert_int_equal(mixer_add(&m, &p, USE_MEDIA, true, 40), 0);
  assert_int_equal(mixer_fill(&m, out, 100, ended), 0);
  for (i = 0; i < 100 * MIXER_CHANNELS; i++)
    {
      assert_int_equal(out[i], 1000 * 8231 / 32768);
    }

  assert_int_equal(mixer_remove(&m, 40), 0);

  /* A constant through the high-pass at 300 Hz fades out */

  mixer_init(&m, 300);
  m.volume[USE_MEDIA] = 10;
  assert_int_equal(mixer_add(&m, &s, USE_MEDIA, true, 30), 0);
  mixer_fill(&m, out, 400, ended);
  assert_true(out[0] > 900);
  assert_true(abs(out[399 * MIXER_CHANNELS]) < 20);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_wav_formats),
    cmocka_unit_test(test_mixer_basic),
    cmocka_unit_test(test_mixer_uses),
    cmocka_unit_test(test_mixer_limits),
  };

  return cmocka_run_group_tests_name("mixer", tests, NULL, NULL);
}
