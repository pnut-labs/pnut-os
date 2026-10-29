/****************************************************************************
 * pnut-os/src/system/power.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <pnut/log.h>
#include <pnut/loop.h>

#include "power.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void power_board(FAR struct pnut_board_client_s *client,
                        bool connected, FAR void *arg);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: power_read
 *
 * Description:
 *   One reading.  A voltage at or under the cutoff on battery power counts
 *   towards powering off; anything else starts the count again, so that a
 *   dip under a burst of current, a radio sending, does not.
 *
 ****************************************************************************/

static void power_read(FAR struct pnut_loop_s *loop,
                       FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct power_s *power = arg;
  bool external = false;
  int mv = 0;
  int ret;

  ret = power->config.measure(power->config.arg, &mv, &external);
  if (ret == -ENODEV)
    {
      pnut_log(&power->module, PNUT_LOG_WARNING,
               "The battery is gone: no more readings");
      pnut_timer_cancel(loop, power->timer);
      power->timer = NULL;
      return;
    }

  if (ret < 0)
    {
      if (!power->failing)
        {
          pnut_log(&power->module, PNUT_LOG_WARNING,
                   "Cannot read the battery: %d", ret);
          power->failing = true;
        }

      return;
    }

  power->failing = false;

  if (external || mv > power->config.cutoff)
    {
      power->low = 0;
      return;
    }

  power->low++;
  if (power->low < power->config.reads)
    {
      pnut_log(&power->module, PNUT_LOG_WARNING,
               "Battery at %d mV, at or under %d mV: %u of %u readings", mv,
               power->config.cutoff, power->low, power->config.reads);
      return;
    }

  pnut_log(&power->module, PNUT_LOG_ERROR,
           "Battery at %d mV, at or under %d mV: powering off", mv,
           power->config.cutoff);

  pnut_timer_cancel(loop, power->timer);
  power->timer = NULL;
  power->off   = true;
  pnut_loop_stop(loop, 0);
}

/****************************************************************************
 * Name: power_rate
 *
 * Description:
 *   Time announced the sleep clock's rate: hand it to the kernel, which
 *   corrects the time each light sleep moves the clock on by (RFC 0028).
 *
 ****************************************************************************/

static void power_rate(FAR struct pnut_sleep_rate_reader_s *reader,
                       FAR const pnut_sleep_rate_t *msg, FAR void *arg)
{
  FAR struct power_s *power = arg;
  int ret;

  ret = power->config.sleep_rate(power->config.arg, msg->ppb);
  if (ret < 0)
    {
      pnut_log(&power->module, PNUT_LOG_WARNING,
               "The kernel took no sleep clock's rate (%+" PRId32
               " ppb): %d", msg->ppb, ret);
      return;
    }

  pnut_log(&power->module, PNUT_LOG_INFO,
           "Light sleeps corrected by %+" PRId32 " ppb, %s", msg->ppb,
           msg->learnt ? "learnt" : "the device's");
}

/****************************************************************************
 * Name: power_watch
 *
 * Description:
 *   Start reading the battery, once the cutoff is known.
 *
 ****************************************************************************/

static void power_watch(FAR struct power_s *power,
                        FAR struct pnut_loop_s *loop)
{
  bool external;
  int mv;
  int ret;

  if (power->config.cutoff <= 0)
    {
      return;
    }

  ret = power->config.measure(power->config.arg, &mv, &external);
  if (ret == -ENODEV)
    {
      pnut_log(&power->module, PNUT_LOG_INFO,
               "No battery: no power-off at a low battery");
      return;
    }

  ret = pnut_timer_start(loop, power->config.interval,
                         power->config.interval, power_read, power,
                         &power->timer);
  if (ret < 0)
    {
      /* The program serves the others all the same */

      pnut_log(&power->module, PNUT_LOG_ERROR,
               "No timer for the battery: %d; no power-off at a low "
               "battery", ret);
      power->timer = NULL;
    }
}

/****************************************************************************
 * Name: power_described
 *
 * Description:
 *   Board's description came: the battery's power-off voltage is the
 *   cutoff.  Board is asked once: the description does not change while
 *   the device runs.
 *
 ****************************************************************************/

static void power_described(FAR struct pnut_board_client_s *client,
                            int status,
                            FAR const pnut_board_description_t *out,
                            FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct power_s *power = arg;
  FAR const pnut_board_part_t *part = NULL;
  pb_size_t i;

  /* No answer: the connection was lost, and the library connects again,
   * when Board is asked again; or Board did not answer in time
   */

  if (status == PNUT_STATUS_TIMEOUT ||
      (status == PNUT_STATUS_UNAVAILABLE && err == NULL))
    {
      pnut_log(&power->module, PNUT_LOG_WARNING,
               "No answer from Board: status %d; asking again", status);
      if (status == PNUT_STATUS_TIMEOUT)
        {
          power_board(client, true, power);
        }

      return;
    }

  pnut_board_disconnect(&power->board);
  power->board_open = false;

  if (status != PNUT_STATUS_OK || out == NULL)
    {
      /* A target without a description is told so; anything else is
       * unexpected
       */

      pnut_log(&power->module, status == PNUT_STATUS_UNAVAILABLE ?
               PNUT_LOG_INFO : PNUT_LOG_WARNING,
               "No description from Board: status %d; no power-off at a "
               "low battery", status);
      return;
    }

  for (i = 0; i < out->parts_count && part == NULL; i++)
    {
      if (out->parts[i].kind == PNUT_BOARD_PART_KIND_BATTERY &&
          out->parts[i].which_detail == PNUT_BOARD_PART_BATTERY_TAG)
        {
          part = &out->parts[i];
        }
    }

  if (part == NULL || part->detail.battery.empty_mv == 0)
    {
      pnut_log(&power->module, PNUT_LOG_INFO,
               "The description gives no power-off voltage for the "
               "battery: no power-off at a low battery");
      return;
    }

  power->config.cutoff = part->detail.battery.empty_mv;
  pnut_log(&power->module, PNUT_LOG_INFO,
           "Powers off at or under %d mV, the battery's",
           power->config.cutoff);
  power_watch(power, power->module.loop);
}

/****************************************************************************
 * Name: power_board
 ****************************************************************************/

static void power_board(FAR struct pnut_board_client_s *client,
                        bool connected, FAR void *arg)
{
  FAR struct power_s *power = arg;
  pnut_board_describe_request_t req;
  int ret;

  if (!connected)
    {
      return;
    }

  memset(&req, 0, sizeof(req));
  ret = pnut_board_describe(client, &req, 0, power_described, power);
  if (ret < 0)
    {
      pnut_log(&power->module, PNUT_LOG_WARNING,
               "Cannot ask Board: %d; no power-off at a low battery", ret);
    }
}

/****************************************************************************
 * Name: power_start
 ****************************************************************************/

static int power_start(FAR struct pnut_module_s *module,
                       FAR struct pnut_loop_s *loop)
{
  FAR struct power_s *power = (FAR struct power_s *)module;
  int ret;

  /* Nothing more to do here yet: Power serves nothing so far */

  pnut_module_ready(module);

  if (power->config.sleep_rate != NULL)
    {
      ret = pnut_sleep_rate_subscribe(loop, power_rate, power,
                                      &power->rates);
      if (ret < 0)
        {
          pnut_log(module, PNUT_LOG_WARNING,
                   "No sleep_rate topic: %d; light sleeps go uncorrected",
                   ret);
        }
      else
        {
          power->rates_open = true;
        }
    }

  if (power->config.measure == NULL)
    {
      return OK;
    }

  if (power->config.board == NULL)
    {
      power_watch(power, loop);
      return OK;
    }

  /* The cutoff is the battery's, from the description */

  power->config.cutoff = 0;
  ret = pnut_board_connect(loop, power->config.board, power_board, power,
                           &power->board);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING,
               "No Board: %d; no power-off at a low battery", ret);
      return OK;
    }

  power->board_open = true;
  return OK;
}

/****************************************************************************
 * Name: power_stop
 ****************************************************************************/

static void power_stop(FAR struct pnut_module_s *module,
                       FAR struct pnut_loop_s *loop)
{
  FAR struct power_s *power = (FAR struct power_s *)module;

  if (power->board_open)
    {
      pnut_board_disconnect(&power->board);
      power->board_open = false;
    }

  if (power->timer != NULL)
    {
      pnut_timer_cancel(loop, power->timer);
      power->timer = NULL;
    }

  if (power->rates_open)
    {
      pnut_sleep_rate_unsubscribe(&power->rates);
      power->rates_open = false;
    }

  power->low     = 0;
  power->failing = false;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: power_init
 ****************************************************************************/

void power_init(FAR struct power_s *power,
                FAR const struct power_config_s *config)
{
  memset(power, 0, sizeof(*power));
  power->config        = *config;
  power->module.name   = "power";
  power->module.start  = power_start;
  power->module.stop   = power_stop;

  if (power->config.reads == 0)
    {
      power->config.reads = 1;
    }
}

/****************************************************************************
 * Name: power_wants_off
 ****************************************************************************/

bool power_wants_off(FAR const struct power_s *power)
{
  return power->off;
}
