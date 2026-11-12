/****************************************************************************
 * pnut-os/src/media/main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The media program (RFC 0007): so far Haptics (RFC 0033).  NxInit starts
 * it once the system program is ready.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pnut/loop.h>
#include <pnut/module.h>

#include "haptics.h"
#ifdef CONFIG_PNUT_AUDIO
#  include "audio.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Board's service, in the system program: the motor, the speaker */

#define MEDIA_BOARD  "board"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pnut_loop_config_s config;
  struct haptics_config_s hconfig;
  FAR struct pnut_loop_s *loop;
  static struct haptics_s haptics;
#ifdef CONFIG_PNUT_AUDIO
  struct audio_config_s aconfig;
  static struct audio_s audio;
#endif
  int ret;

  pnut_loop_defaults(&config);
  config.name    = "media";
  config.levels  = true;
  config.workers = 0;
  config.jobs    = 0;

  ret = pnut_loop_create(&config, &loop);
  if (ret < 0)
    {
      fprintf(stderr, "pnut_media: no loop: %d\n", ret);
      return EXIT_FAILURE;
    }

  memset(&hconfig, 0, sizeof(hconfig));
  hconfig.ops   = &g_haptics_ff;
  hconfig.board = MEDIA_BOARD;
  if (CONFIG_PNUT_HAPTICS_DEVICE[0] != '\0')
    {
      hconfig.device = CONFIG_PNUT_HAPTICS_DEVICE;
    }

  haptics_init(&haptics, &hconfig);

  ret = pnut_module_add(loop, &haptics.module);

#ifdef CONFIG_PNUT_AUDIO
  if (ret >= 0)
    {
      memset(&aconfig, 0, sizeof(aconfig));
      aconfig.ops      = &g_audio_output;
      aconfig.board    = MEDIA_BOARD;
      aconfig.sounds   = CONFIG_PNUT_AUDIO_SOUNDS;
      aconfig.highpass = CONFIG_PNUT_AUDIO_HIGHPASS;
      if (CONFIG_PNUT_AUDIO_DEVICE[0] != '\0')
        {
          aconfig.device = CONFIG_PNUT_AUDIO_DEVICE;
        }

      audio_init(&audio, &aconfig);
      ret = pnut_module_add(loop, &audio.module);
    }
#endif

  if (ret >= 0)
    {
      ret = pnut_loop_run(loop);
    }

  pnut_loop_destroy(loop);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
