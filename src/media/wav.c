/****************************************************************************
 * pnut-os/src/media/wav.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* WAV files for Audio (RFC 0032): RIFF, a "fmt " chunk of PCM, a "data"
 * chunk; other chunks passed over.  The samples become 16 bits.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "wav.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WAV_MAX         (1024 * 1024)
#define WAV_PCM         1
#define WAV_EXTENSIBLE  0xfffe

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t wav_u32(FAR const uint8_t *p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static uint16_t wav_u16(FAR const uint8_t *p)
{
  return (uint16_t)(p[0] | p[1] << 8);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int wav_parse(FAR const uint8_t *data, size_t size,
              FAR struct mixer_sound_s *sound)
{
  FAR const uint8_t *fmt = NULL;
  FAR const uint8_t *pcm = NULL;
  uint32_t fmt_size = 0;
  uint32_t pcm_size = 0;
  uint32_t chunk;
  uint32_t i;
  uint16_t format;
  uint16_t channels;
  uint16_t bits;
  uint32_t rate;
  size_t off = 12;
  size_t bytes;
  uint32_t count;

  memset(sound, 0, sizeof(*sound));
  if (size < 12 || memcmp(data, "RIFF", 4) != 0 ||
      memcmp(data + 8, "WAVE", 4) != 0)
    {
      return -EINVAL;
    }

  /* The chunks, each padded to an even size */

  while (off + 8 <= size)
    {
      chunk = wav_u32(data + off + 4);
      if (chunk > size - off - 8)
        {
          chunk = (uint32_t)(size - off - 8);  /* A cut file: what is there */
        }

      if (memcmp(data + off, "fmt ", 4) == 0 && chunk >= 16)
        {
          fmt      = data + off + 8;
          fmt_size = chunk;
        }
      else if (memcmp(data + off, "data", 4) == 0)
        {
          pcm      = data + off + 8;
          pcm_size = chunk;
        }

      off += 8 + chunk + (chunk & 1);
    }

  if (fmt == NULL || pcm == NULL)
    {
      return -EINVAL;
    }

  format   = wav_u16(fmt);
  channels = wav_u16(fmt + 2);
  rate     = wav_u32(fmt + 4);
  bits     = wav_u16(fmt + 14);

  /* An extensible format is PCM if its sub-format's first two bytes say
   * so
   */

  if (format == WAV_EXTENSIBLE)
    {
      format = fmt_size >= 40 ? wav_u16(fmt + 24) : 0;
    }

  if (format != WAV_PCM ||
      (channels != 1 && channels != 2) || rate < 1000 || rate > 192000 ||
      (bits != 8 && bits != 16 && bits != 24))
    {
      return -EINVAL;
    }

  bytes = bits / 8;
  count = pcm_size / (uint32_t)(bytes * channels);
  if (count == 0)
    {
      return -EINVAL;
    }

  sound->samples = malloc((size_t)count * channels * sizeof(int16_t));
  if (sound->samples == NULL)
    {
      return -ENOMEM;
    }

  for (i = 0; i < count * channels; i++)
    {
      FAR const uint8_t *p = pcm + i * bytes;

      sound->samples[i] = bits == 8 ? (int16_t)((p[0] - 128) * 256) :
                          bits == 16 ? (int16_t)wav_u16(p) :
                          (int16_t)wav_u16(p + 1);
    }

  sound->frames   = count;
  sound->rate     = rate;
  sound->channels = (uint8_t)channels;
  return OK;
}

int wav_load(FAR const char *path, FAR struct mixer_sound_s *sound)
{
  FAR uint8_t *data;
  struct stat st;
  ssize_t n;
  size_t got = 0;
  int ret;
  int fd;

  memset(sound, 0, sizeof(*sound));
  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      return -errno;
    }

  if (fstat(fd, &st) < 0 || st.st_size <= 0 || st.st_size > WAV_MAX)
    {
      close(fd);
      return -EINVAL;
    }

  data = malloc(st.st_size);
  if (data == NULL)
    {
      close(fd);
      return -ENOMEM;
    }

  while (got < (size_t)st.st_size)
    {
      n = read(fd, data + got, st.st_size - got);
      if (n <= 0)
        {
          break;
        }

      got += n;
    }

  close(fd);
  ret = got == (size_t)st.st_size ? wav_parse(data, got, sound) : -EIO;
  free(data);
  return ret;
}

void wav_free(FAR struct mixer_sound_s *sound)
{
  free(sound->samples);
  memset(sound, 0, sizeof(*sound));
}
