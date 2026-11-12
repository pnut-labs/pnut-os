/****************************************************************************
 * pnut-os/src/media/wav.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_MEDIA_WAV_H
#define __PNUT_OS_MEDIA_WAV_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>

#include "mixer.h"

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: wav_parse
 *
 * Description:
 *   Read a WAV file held in memory into a sound: PCM, mono or stereo, 8,
 *   16 or 24 bits, at any rate, its samples converted to 16 bits in a
 *   buffer of its own (free it with wav_free).
 *
 * Returned Value:
 *   OK; -EINVAL for what is not such a WAV file; -ENOMEM.
 *
 ****************************************************************************/

int wav_parse(FAR const uint8_t *data, size_t size,
              FAR struct mixer_sound_s *sound);

/****************************************************************************
 * Name: wav_load
 *
 * Description:
 *   wav_parse a file, read whole.  Up to 1 MB.
 *
 ****************************************************************************/

int wav_load(FAR const char *path, FAR struct mixer_sound_s *sound);

/****************************************************************************
 * Name: wav_free
 ****************************************************************************/

void wav_free(FAR struct mixer_sound_s *sound);

#endif /* __PNUT_OS_MEDIA_WAV_H */
