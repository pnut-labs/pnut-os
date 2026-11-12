/****************************************************************************
 * pnut-os/src/media/haptics.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Haptics (RFC 0033): the vibration motor, named effects mapped to it, and
 * callers' own patterns.  One vibration plays at a time, by use: a higher
 * use stops a lower one, a lower one asked meanwhile is dropped.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <string.h>
#include <time.h>

#include <pnut/log.h>
#include <pnut/msg.h>

#include "haptics.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HAPTICS_SERVICE  "haptics"
#define HAPTICS_STRENGTHS 3       /* weak, medium, strong */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A named effect as steps, scaled by the strength, maybe repeating */

struct haptics_effect_s
{
  FAR const struct haptics_step_s *steps;
  uint8_t nsteps;
  bool repeat;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Steps: a time in ms, a level 0 to 100 */

static const struct haptics_step_s g_steps_tick[] =
{
  { 30, 100 }
};

static const struct haptics_step_s g_steps_click[] =
{
  { 50, 100 }
};

static const struct haptics_step_s g_steps_double[] =
{
  { 50, 100 },
  { 80, 0 },
  { 50, 100 }
};

static const struct haptics_step_s g_steps_notification[] =
{
  { 150, 100 },
  { 120, 0 },
  { 150, 100 }
};

static const struct haptics_step_s g_steps_ring[] =
{
  { 800, 100 },
  { 400, 0 },
  { 800, 100 },
  { 1600, 0 }
};

static const struct haptics_step_s g_steps_alarm[] =
{
  { 400, 100 },
  { 200, 0 },
  { 400, 100 },
  { 200, 0 },
  { 400, 100 },
  { 1000, 0 }
};

static const struct haptics_step_s g_steps_success[] =
{
  { 60, 100 },
  { 60, 0 },
  { 60, 100 }
};

static const struct haptics_step_s g_steps_error[] =
{
  { 300, 100 }
};

#define HAPTICS_EFFECT(steps, repeat) \
  { steps, sizeof(steps) / sizeof(steps[0]), repeat }

/* Every effect as steps: a motor with levels only, and the longer effects
 * on any motor
 */

static const struct haptics_effect_s g_haptics_steps[] =
{
  HAPTICS_EFFECT(g_steps_tick, false),
  HAPTICS_EFFECT(g_steps_click, false),
  HAPTICS_EFFECT(g_steps_double, false),
  HAPTICS_EFFECT(g_steps_notification, false),
  HAPTICS_EFFECT(g_steps_ring, true),
  HAPTICS_EFFECT(g_steps_alarm, true),
  HAPTICS_EFFECT(g_steps_success, false),
  HAPTICS_EFFECT(g_steps_error, false)
};

/* A DRV2605's library, its ERM effects as TI numbers them: the short
 * effects, weak, medium and strong, and how long they take; 0 for steps
 */

static const uint8_t g_drv2605_library[][HAPTICS_STRENGTHS] =
{
  { 26, 25, 24 },                 /* Tick: sharp tick 1 to 3 */
  { 19, 18, 17 },                 /* Click: strong click 1 to 3 */
  { 11, 10, 10 },                 /* Double click */
  { 0, 0, 0 },                    /* Notification */
  { 0, 0, 0 },                    /* Ring */
  { 0, 0, 0 },                    /* Alarm */
  { 29, 28, 27 },                 /* Success: short double click strong */
  { 49, 48, 47 },                 /* Error: buzz 1 to 3 */
};

static const uint16_t g_drv2605_ms[] =
{
  60, 80, 250, 0, 0, 0, 250, 300
};

/* The strengths' scale, for levels */

static const uint8_t g_haptics_scale[HAPTICS_STRENGTHS] =
{
  60, 80, 100
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t haptics_now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* The strength setting: 0 weak, 1 medium, 2 strong */

static int haptics_strength(FAR struct haptics_s *haptics)
{
  FAR const char *s = haptics->settings.values.strength;

  return strcmp(s, "weak") == 0 ? 0 : strcmp(s, "strong") == 0 ? 2 : 1;
}

/****************************************************************************
 * Name: haptics_publish
 ****************************************************************************/

static void haptics_publish(FAR struct haptics_s *haptics)
{
  pnut_haptics_info_t msg;

  if (haptics->topic == NULL)
    {
      return;
    }

  memset(&msg, 0, sizeof(msg));
  msg.playing = haptics->playing;
  if (haptics->playing)
    {
      msg.use     = haptics->use;
      msg.pattern = haptics->pattern;
      msg.effect  = haptics->effect;
    }

  pnut_haptics_info_publish(haptics->topic, &msg);
}

/****************************************************************************
 * Name: haptics_halt
 *
 * Description:
 *   Still the motor, and forget what played; not told.
 *
 ****************************************************************************/

static void haptics_halt(FAR struct haptics_s *haptics)
{
  if (haptics->timer != NULL)
    {
      pnut_timer_cancel(haptics->module.loop, haptics->timer);
      haptics->timer = NULL;
    }

  if (haptics->playing)
    {
      haptics->config.ops->stop(haptics->config.arg);
      haptics->playing = false;
    }
}

/****************************************************************************
 * Name: haptics_end
 ****************************************************************************/

static void haptics_end(FAR struct haptics_s *haptics)
{
  haptics_halt(haptics);
  haptics_publish(haptics);
}

/****************************************************************************
 * Name: haptics_step
 *
 * Description:
 *   Play the step due, and time the next.
 *
 ****************************************************************************/

static void haptics_tick(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg);

static int haptics_step(FAR struct haptics_s *haptics)
{
  FAR const struct haptics_step_s *step = &haptics->steps[haptics->step];
  uint8_t level;
  int ret;

  level = (uint8_t)(step->level *
                    g_haptics_scale[haptics_strength(haptics)] / 100);
  ret = haptics->config.ops->level(haptics->config.arg, level, step->ms);
  if (ret < 0)
    {
      return ret;
    }

  return pnut_timer_start(haptics->module.loop, step->ms, 0, haptics_tick,
                          haptics, &haptics->timer);
}

static void haptics_tick(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct haptics_s *haptics = arg;

  haptics->timer = NULL;
  if (haptics->nsteps == 0 || ++haptics->step >= haptics->nsteps)
    {
      if (haptics->nsteps == 0 || !haptics->repeat)
        {
          haptics_end(haptics);
          return;
        }

      haptics->step = 0;
    }

  if (haptics_step(haptics) < 0)
    {
      pnut_log(&haptics->module, PNUT_LOG_WARNING, "The motor failed");
      haptics_end(haptics);
    }
}

/****************************************************************************
 * Name: haptics_start
 *
 * Description:
 *   Play what is set up (steps, or a library sequence for ms), after
 *   stilling what played.
 *
 ****************************************************************************/

static int haptics_start(FAR struct haptics_s *haptics,
                         FAR const uint8_t *library, uint16_t ms)
{
  int ret;

  haptics->playing = true;
  haptics->step    = 0;

  if (library != NULL)
    {
      haptics->nsteps = 0;
      ret = haptics->config.ops->sequence(haptics->config.arg, library, 1);
      if (ret >= 0)
        {
          ret = pnut_timer_start(haptics->module.loop, ms, 0, haptics_tick,
                                 haptics, &haptics->timer);
        }
    }
  else
    {
      ret = haptics_step(haptics);
    }

  if (ret < 0)
    {
      pnut_log(&haptics->module, PNUT_LOG_WARNING, "The motor failed: %d",
               ret);
      haptics_halt(haptics);
    }

  haptics_publish(haptics);
  return ret;
}

/****************************************************************************
 * Name: haptics_admit
 *
 * Description:
 *   Whether a vibration of a use may play now; if not, the request is
 *   answered.  If it may, what plays is stilled.
 *
 ****************************************************************************/

static bool haptics_admit(FAR struct haptics_s *haptics,
                          FAR const struct pnut_request_s *req, int use)
{
  uint64_t now;

  if (use < PNUT_HAPTICS_USE_HAPTICS_USE_APP ||
      use > PNUT_HAPTICS_USE_HAPTICS_USE_ALARM)
    {
      pnut_haptics_fail(&haptics->server, req, PNUT_STATUS_INVALID,
                        PNUT_HAPTICS_ERROR_CODE_NONE, "no such use");
      return false;
    }

  if (!haptics->motor)
    {
      pnut_haptics_fail(&haptics->server, req, PNUT_STATUS_UNAVAILABLE,
                        PNUT_HAPTICS_ERROR_CODE_NO_MOTOR, "no motor");
      return false;
    }

  if (use == PNUT_HAPTICS_USE_HAPTICS_USE_INTERFACE &&
      !haptics->settings.values.interface)
    {
      pnut_haptics_fail(&haptics->server, req, PNUT_STATUS_UNAVAILABLE,
                        PNUT_HAPTICS_ERROR_CODE_OFF, "interface off");
      return false;
    }

  if (haptics->playing && use < haptics->use)
    {
      pnut_haptics_fail(&haptics->server, req, PNUT_STATUS_BUSY,
                        PNUT_HAPTICS_ERROR_CODE_BUSY, "a higher use plays");
      return false;
    }

  if (use == PNUT_HAPTICS_USE_HAPTICS_USE_APP)
    {
      now = haptics_now();
      if (haptics->app_at != 0 && now - haptics->app_at < HAPTICS_APP_MS)
        {
          pnut_haptics_fail(&haptics->server, req, PNUT_STATUS_BUSY,
                            PNUT_HAPTICS_ERROR_CODE_TOO_OFTEN,
                            "one a second");
          return false;
        }

      haptics->app_at = now;
    }

  haptics_halt(haptics);
  return true;
}

/****************************************************************************
 * Name: haptics_on_*
 ****************************************************************************/

static void haptics_on_effect(FAR struct pnut_haptics_server_s *server,
                              FAR const struct pnut_request_s *req,
                              FAR const pnut_haptics_effect_request_t *in,
                              FAR void *arg)
{
  FAR struct haptics_s *haptics = arg;
  FAR const struct haptics_effect_s *e;
  FAR const uint8_t *library = NULL;
  pnut_haptics_reply_t reply;

  if (in->effect < PNUT_HAPTICS_EFFECT_NAME_HAPTICS_EFFECT_TICK ||
      in->effect > PNUT_HAPTICS_EFFECT_NAME_HAPTICS_EFFECT_ERROR)
    {
      pnut_haptics_fail(server, req, PNUT_STATUS_INVALID,
                        PNUT_HAPTICS_ERROR_CODE_NONE, "no such effect");
      return;
    }

  if (!haptics_admit(haptics, req, in->use))
    {
      return;
    }

  e = &g_haptics_steps[in->effect];
  if (haptics->library && g_drv2605_library[in->effect][0] != 0)
    {
      library = &g_drv2605_library[in->effect][haptics_strength(haptics)];
    }

  haptics->use     = in->use;
  haptics->pattern = false;
  haptics->effect  = in->effect;
  haptics->repeat  = e->repeat;
  haptics->nsteps  = e->nsteps;
  memcpy(haptics->steps, e->steps, e->nsteps * sizeof(e->steps[0]));

  if (haptics_start(haptics, library, g_drv2605_ms[in->effect]) < 0)
    {
      if (in->use == PNUT_HAPTICS_USE_HAPTICS_USE_APP)
        {
          haptics->app_at = 0;    /* Nothing played */
        }

      pnut_haptics_fail(server, req, PNUT_STATUS_INTERNAL,
                        PNUT_HAPTICS_ERROR_CODE_NO_MOTOR, "motor failed");
      return;
    }

  memset(&reply, 0, sizeof(reply));
  pnut_haptics_effect_reply(server, req, PNUT_STATUS_OK, &reply);
}

static void haptics_on_pattern(FAR struct pnut_haptics_server_s *server,
                               FAR const struct pnut_request_s *req,
                               FAR const pnut_haptics_pattern_request_t *in,
                               FAR void *arg)
{
  FAR struct haptics_s *haptics = arg;
  pnut_haptics_reply_t reply;
  uint32_t total = 0;
  pb_size_t i;

  for (i = 0; i < in->steps_count; i++)
    {
      if (in->steps[i].strength > 100 || in->steps[i].ms == 0 ||
          in->steps[i].ms > HAPTICS_MAX_MS)
        {
          break;
        }

      total += in->steps[i].ms;
    }

  if (in->steps_count == 0 || i < in->steps_count ||
      in->steps_count > HAPTICS_STEPS || total > HAPTICS_MAX_MS)
    {
      pnut_haptics_fail(server, req, PNUT_STATUS_INVALID,
                        PNUT_HAPTICS_ERROR_CODE_TOO_LONG,
                        "1 to 16 steps, 5 s in all, strength to 100");
      return;
    }

  if (!haptics_admit(haptics, req, in->use))
    {
      return;
    }

  haptics->use     = in->use;
  haptics->pattern = true;
  haptics->effect  = 0;
  haptics->repeat  = false;
  haptics->nsteps  = (uint8_t)in->steps_count;
  for (i = 0; i < in->steps_count; i++)
    {
      haptics->steps[i].ms    = (uint16_t)in->steps[i].ms;
      haptics->steps[i].level = (uint8_t)in->steps[i].strength;
    }

  if (haptics_start(haptics, NULL, 0) < 0)
    {
      if (in->use == PNUT_HAPTICS_USE_HAPTICS_USE_APP)
        {
          haptics->app_at = 0;    /* Nothing played */
        }

      pnut_haptics_fail(server, req, PNUT_STATUS_INTERNAL,
                        PNUT_HAPTICS_ERROR_CODE_NO_MOTOR, "motor failed");
      return;
    }

  memset(&reply, 0, sizeof(reply));
  pnut_haptics_pattern_reply(server, req, PNUT_STATUS_OK, &reply);
}

static void haptics_on_stop(FAR struct pnut_haptics_server_s *server,
                            FAR const struct pnut_request_s *req,
                            FAR const pnut_haptics_stop_request_t *in,
                            FAR void *arg)
{
  FAR struct haptics_s *haptics = arg;
  pnut_haptics_reply_t reply;

  if (in->use < PNUT_HAPTICS_USE_HAPTICS_USE_APP ||
      in->use > PNUT_HAPTICS_USE_HAPTICS_USE_ALARM)
    {
      pnut_haptics_fail(server, req, PNUT_STATUS_INVALID,
                        PNUT_HAPTICS_ERROR_CODE_NONE, "no such use");
      return;
    }

  if (haptics->playing && haptics->use == in->use)
    {
      haptics_end(haptics);
    }

  memset(&reply, 0, sizeof(reply));
  pnut_haptics_stop_reply(server, req, PNUT_STATUS_OK, &reply);
}

static const struct pnut_haptics_handlers_s g_haptics_handlers =
{
  haptics_on_effect,
  haptics_on_pattern,
  haptics_on_stop
};

/****************************************************************************
 * Name: haptics_motor
 *
 * Description:
 *   The motor known: open it, and say Haptics is ready, with or without.
 *
 ****************************************************************************/

static void haptics_motor(FAR struct haptics_s *haptics,
                          FAR const char *path, FAR const char *chip)
{
  int ret;

  if (path == NULL || path[0] == '\0')
    {
      pnut_log(&haptics->module, PNUT_LOG_INFO, "No vibration motor");
      pnut_module_ready(&haptics->module);
      return;
    }

  strlcpy(haptics->path, path, sizeof(haptics->path));
  strlcpy(haptics->chip, chip != NULL ? chip : "", sizeof(haptics->chip));
  haptics->library = strncmp(haptics->chip, "DRV2605", 7) == 0;

  ret = haptics->config.ops->open(haptics->config.arg, haptics->path);
  if (ret < 0)
    {
      pnut_log(&haptics->module, PNUT_LOG_ERROR,
               "The motor %s does not open: %d", haptics->path, ret);
    }
  else
    {
      haptics->motor = true;
      pnut_log(&haptics->module, PNUT_LOG_INFO, "Motor: %s, %s",
               haptics->chip[0] != '\0' ? haptics->chip : "a motor",
               haptics->path);
    }

  pnut_module_ready(&haptics->module);
}

/****************************************************************************
 * Name: haptics_board, haptics_described
 *
 * Description:
 *   Board asked for its description, once: the first part that vibrates.
 *
 ****************************************************************************/

static void haptics_board(FAR struct pnut_board_client_s *client,
                          bool connected, FAR void *arg);

static void haptics_described(FAR struct pnut_board_client_s *client,
                              int status,
                              FAR const pnut_board_description_t *out,
                              FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct haptics_s *haptics = arg;
  FAR const pnut_board_part_t *part = NULL;
  pb_size_t i;

  if (status == PNUT_STATUS_TIMEOUT ||
      (status == PNUT_STATUS_UNAVAILABLE && err == NULL))
    {
      pnut_log(&haptics->module, PNUT_LOG_WARNING,
               "No answer from Board: status %d; asking again", status);
      if (status == PNUT_STATUS_TIMEOUT)
        {
          haptics_board(client, true, haptics);
        }

      return;
    }

  pnut_board_disconnect(&haptics->board);
  haptics->board_open = false;

  if (status == PNUT_STATUS_OK && out != NULL)
    {
      for (i = 0; i < out->parts_count; i++)
        {
          if (out->parts[i].kind == PNUT_BOARD_PART_KIND_VIBRATION &&
              out->parts[i].device[0] != '\0')
            {
              part = &out->parts[i];
              break;
            }
        }
    }

  haptics_motor(haptics, part != NULL ? part->device : NULL,
                part != NULL ? part->chip : NULL);
}

static void haptics_board(FAR struct pnut_board_client_s *client,
                          bool connected, FAR void *arg)
{
  FAR struct haptics_s *haptics = arg;
  pnut_board_describe_request_t req;
  int ret;

  if (!connected)
    {
      return;
    }

  memset(&req, 0, sizeof(req));
  ret = pnut_board_describe(client, &req, 0, haptics_described, haptics);
  if (ret < 0)
    {
      pnut_log(&haptics->module, PNUT_LOG_WARNING, "Cannot ask Board: %d",
               ret);
      pnut_board_disconnect(&haptics->board);
      haptics->board_open = false;
      haptics_motor(haptics, NULL, NULL);
    }
}

/****************************************************************************
 * Name: haptics_module_start
 ****************************************************************************/

static int haptics_module_start(FAR struct pnut_module_s *module,
                                FAR struct pnut_loop_s *loop)
{
  FAR struct haptics_s *haptics = (FAR struct haptics_s *)module;
  int ret;

  ret = pnut_haptics_settings_open(module, NULL, haptics,
                                   &haptics->settings);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No settings: %d", ret);
    }
  else
    {
      haptics->settings_open = true;
    }

  ret = pnut_haptics_info_advertise(loop, &haptics->topic);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No haptics topic: %d", ret);
      haptics->topic = NULL;
    }

  haptics_publish(haptics);

  ret = pnut_haptics_serve(loop, HAPTICS_SERVICE, 0, &g_haptics_handlers,
                           haptics, &haptics->server);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Cannot serve: %d", ret);
      return ret;
    }

  haptics->serving = true;

  if (haptics->config.device != NULL)
    {
      haptics_motor(haptics, haptics->config.device, haptics->config.chip);
      return OK;
    }

  ret = pnut_board_connect(loop, haptics->config.board, haptics_board,
                           haptics, &haptics->board);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No Board: %d; no motor", ret);
      haptics_motor(haptics, NULL, NULL);
      return OK;
    }

  haptics->board_open = true;
  return OK;
}

/****************************************************************************
 * Name: haptics_module_stop
 ****************************************************************************/

static void haptics_module_stop(FAR struct pnut_module_s *module,
                                FAR struct pnut_loop_s *loop)
{
  FAR struct haptics_s *haptics = (FAR struct haptics_s *)module;

  haptics_end(haptics);

  if (haptics->board_open)
    {
      pnut_board_disconnect(&haptics->board);
      haptics->board_open = false;
    }

  if (haptics->serving)
    {
      pnut_haptics_close(&haptics->server);
      haptics->serving = false;
    }

  if (haptics->settings_open)
    {
      pnut_haptics_settings_close(&haptics->settings);
      haptics->settings_open = false;
    }

  if (haptics->topic != NULL)
    {
      pnut_topic_unadvertise(haptics->topic);
      haptics->topic = NULL;
    }

  if (haptics->motor)
    {
      haptics->config.ops->close(haptics->config.arg);
      haptics->motor = false;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: haptics_init
 ****************************************************************************/

void haptics_init(FAR struct haptics_s *haptics,
                  FAR const struct haptics_config_s *config)
{
  memset(haptics, 0, sizeof(*haptics));
  haptics->config       = *config;
  haptics->module.name  = "haptics";
  haptics->module.start = haptics_module_start;
  haptics->module.stop  = haptics_module_stop;
}
