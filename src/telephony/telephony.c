/****************************************************************************
 * pnut-os/src/telephony/telephony.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pnut/log.h>
#include <pnut/loop.h>
#include <pnut/msg.h>
#include <pnut/topic.h>

#include "parse.h"
#include "telephony.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The A7682E's times (SIMCom's hardware design, and the previous system's
 * findings), in ms
 */

#define TEL_BOOT_MS       20000     /* To its first answer after the key */
#define TEL_OFF_MS        10000     /* To stop answering after AT+CPOF */
#define TEL_SETTLE_MS     2000      /* Its UART stops before it is down */
#define TEL_KEY_ON_MS     100       /* The key pressed to start it */
#define TEL_KEY_OFF_MS    3000      /* and to stop it */
#define TEL_CPOF_MS       3000      /* For AT+CPOF's answer */
#define TEL_CLOSE_MS      200       /* Out of the multiplexer, to plain AT */
#define TEL_OPEN_MS       2000      /* For a channel's answer */
#define TEL_READY_MS      300       /* Open channels refuse at first */
#define TEL_SWITCH_MS     100       /* Into the multiplexer, before SABM */
#define TEL_RESTART_MS    5000      /* Before a restart, doubling */

#define TEL_RAIL_MS       100       /* The rail on, before the key */
#define TEL_PROBE_MS      1000      /* For an answer, at start */
#define TEL_POLL_MS       1000      /* Between questions while it boots */
#define TEL_FADE_MS       500       /* Between questions while it stops */
#define TEL_SIM_MS        2000      /* Before asking a busy SIM again */
#define TEL_SETTINGS_MS   3000      /* For Settings' values, at start */
#define TEL_TRIAL_MS      120000    /* For GSM to find a network */
#define TEL_HOUR_MS       3600000   /* Between readings of its clock */
#define TEL_FRESH_MS      30000     /* A state that old is read again */
#define TEL_PIN_MS        15000     /* For the SIM to take a PIN */
#define TEL_COMMAND_MS    10000     /* A command passed through */
#define TEL_COMMAND_MAX   180000    /* and the longest it may take */

#define TEL_SILENT_MAX    2         /* Unanswered in a row: it stopped */
#define TEL_OPEN_TRIES    3
#define TEL_SIM_TRIES     10

/* AT+CNMP's modes */

#define TEL_CNMP_AUTO     2
#define TEL_CNMP_LTE      38

/* The system UI's program (RFC 0007) */

#define TEL_UI            "ui"

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum telephony_step_e
{
  STEP_WAITING = 0,               /* For the description and settings */
  STEP_OFF,
  STEP_FAILED,                    /* Left off until turned on again */
  STEP_PROBE,                     /* Is a modem left on answering? */
  STEP_RAIL,                      /* The rail just on */
  STEP_KEY,                       /* The key pressed to start it */
  STEP_BOOT,                      /* Waiting for its first answer */
  STEP_CMUX,                      /* AT+CMUX sent */
  STEP_OPEN,                      /* The channels opening */
  STEP_READY,                     /* Open: a moment before they answer */
  STEP_SETUP,                     /* Its settings, again */
  STEP_ON,
  STEP_SHUTDOWN,                  /* AT+CPOF sent */
  STEP_KEY_OFF,                   /* The key held to stop it */
  STEP_FADING,                    /* Asked until it stops answering */
  STEP_SETTLE,                    /* It stopped: before the rail goes */
};

/* What follows a power-down */

enum telephony_after_e
{
  AFTER_OFF = 0,
  AFTER_FAILED,
  AFTER_RESTART,
};

/* The settings set again at each start (RFC 0030), in order; then the
 * bands and the mode, airplane mode, and the SIM
 */

enum telephony_setup_e
{
  SETUP_BANDS = 0,
  SETUP_MODE,
  SETUP_AIRPLANE,
  SETUP_SIM,
  SETUP_DONE,
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void tel_power_down(FAR struct telephony_s *tel, uint8_t after);
static void tel_power_up(FAR struct telephony_s *tel);
static void tel_setup_next(FAR struct telephony_s *tel);
static void tel_refresh(FAR struct telephony_s *tel);
static void tel_publish(FAR struct telephony_s *tel);
static void tel_flush(FAR struct telephony_s *tel);
static void tel_status(FAR struct pnut_telephony_server_s *server,
                       FAR const struct pnut_request_s *req,
                       FAR const pnut_telephony_status_request_t *in,
                       FAR void *arg);
static void tel_enter_pin(FAR struct pnut_telephony_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_telephony_pin_request_t *in,
                          FAR void *arg);
static void tel_enter_puk(FAR struct pnut_telephony_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_telephony_puk_request_t *in,
                          FAR void *arg);
static void tel_command(FAR struct pnut_telephony_server_s *server,
                        FAR const struct pnut_request_s *req,
                        FAR const pnut_telephony_command_request_t *in,
                        FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR const char *const g_tel_setup[] =
{
  "ATE0",                         /* No echo */
  "AT+CMEE=2",                    /* Errors as text */
  "AT+CMGF=0",                    /* SMS as PDUs */
  "AT+CNMI=2,1,0,0,0",            /* SMS stored, and announced */
  "AT+CREG=2",                    /* Registration reported as it changes */
  "AT+CEREG=2",
  "AT+COPS=3,0",                  /* The operator by its name */
  "AT+CEMODE=2",                  /* Combined attach: SMS without IMS */
  "AT+CTZU=1",                    /* The clock from the network's time */
  "AT+CTZR=2",                    /* and the network's time reported */
  "AT+CFGRI=1",                   /* The ring indicator for SMS too */
  "AT+CSCLK=0",                   /* The modem's own sleep off */
};

#define TEL_SETUP_BASIC  (sizeof(g_tel_setup) / sizeof(g_tel_setup[0]))

static const struct pnut_telephony_handlers_s g_tel_handlers =
{
  .status    = tel_status,
  .enter_pin = tel_enter_pin,
  .enter_puk = tel_enter_puk,
  .command   = tel_command,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tel_now
 *
 * Description:
 *   Milliseconds, monotonic: the clock network_time's readings name.
 *
 ****************************************************************************/

static uint64_t tel_now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/****************************************************************************
 * Name: tel_config
 *
 * Description:
 *   A time of the config's, or the modem's own.
 *
 ****************************************************************************/

static uint32_t tel_config(uint32_t value, uint32_t fallback)
{
  return value > 0 ? value : fallback;
}

/****************************************************************************
 * Name: tel_line
 *
 * Description:
 *   Set one of the modem's lines; one the device lacks is left.
 *
 ****************************************************************************/

static void tel_line(FAR struct telephony_s *tel, int line, bool high)
{
  int ret;

  if (tel->line[line] < 0)
    {
      return;
    }

  ret = tel->config.set_line(tel->config.arg, tel->line[line], high);
  if (ret < 0)
    {
      pnut_log(&tel->module, PNUT_LOG_ERROR, "Cannot set the modem's %s "
               "line: %d", line == TELEPHONY_POWER ? "power" : "key", ret);
    }
}

/****************************************************************************
 * Name: tel_rail
 *
 * Description:
 *   Whether the modem's rail is on.  Without a line for it, the modem is
 *   always supplied.
 *
 ****************************************************************************/

static bool tel_rail(FAR struct telephony_s *tel)
{
  bool high = true;

  if (tel->line[TELEPHONY_POWER] >= 0 &&
      tel->config.get_line(tel->config.arg, tel->line[TELEPHONY_POWER],
                           &high) < 0)
    {
      high = false;
    }

  return high;
}

/****************************************************************************
 * Name: tel_timer
 *
 * Description:
 *   The step's timer, in place of the one running.
 *
 ****************************************************************************/

static void tel_timer(FAR struct telephony_s *tel, uint32_t ms,
                      pnut_timer_handler_t handler)
{
  int ret;

  if (tel->timer != NULL)
    {
      pnut_timer_cancel(tel->module.loop, tel->timer);
      tel->timer = NULL;
    }

  ret = pnut_timer_start(tel->module.loop, ms, 0, handler, tel,
                         &tel->timer);
  if (ret < 0)
    {
      tel->timer = NULL;
      pnut_log(&tel->module, PNUT_LOG_ERROR, "No timer: %d", ret);
    }
}

/****************************************************************************
 * Name: tel_untime
 ****************************************************************************/

static void tel_untime(FAR struct telephony_s *tel)
{
  if (tel->timer != NULL)
    {
      pnut_timer_cancel(tel->module.loop, tel->timer);
      tel->timer = NULL;
    }
}

/****************************************************************************
 * Name: tel_step
 ****************************************************************************/

static void tel_step(FAR struct telephony_s *tel, uint8_t step)
{
  tel->step  = step;
  tel->since = tel_now();
  tel_publish(tel);
}

/****************************************************************************
 * Name: tel_talking
 *
 * Description:
 *   Whether the modem is up, answering.
 *
 ****************************************************************************/

static bool tel_talking(FAR struct telephony_s *tel)
{
  return tel->step >= STEP_CMUX && tel->step <= STEP_ON;
}

/****************************************************************************
 * Name: tel_send
 *
 * Description:
 *   A command, logged if it cannot be queued.
 *
 ****************************************************************************/

static int tel_send(FAR struct telephony_s *tel, FAR const char *command,
                    FAR const char *prefix, uint32_t timeout,
                    at_done_t done)
{
  int ret;

  ret = at_command(&tel->at, command, prefix, timeout, 0, done, tel);
  if (ret < 0)
    {
      pnut_log(&tel->module, PNUT_LOG_WARNING, "Cannot send %s: %d",
               command, ret);
    }

  return ret;
}

/****************************************************************************
 * Name: tel_find
 *
 * Description:
 *   The line of an answer that the prefix begins, without its end, cut to
 *   size: NULL when the answer has none.
 *
 ****************************************************************************/

static FAR const char *tel_find(FAR const char *reply, FAR char *line,
                                size_t size, FAR const char *prefix)
{
  size_t plen = strlen(prefix);
  FAR const char *p = reply;
  size_t n;

  while (*p != '\0')
    {
      n = strcspn(p, "\n");
      if (n >= plen && strncmp(p, prefix, plen) == 0)
        {
          if (n >= size)
            {
              n = size - 1;
            }

          memcpy(line, p, n);
          line[n] = '\0';
          return line;
        }

      p += n;
      if (*p == '\n')
        {
          p++;
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: tel_publish
 *
 * Description:
 *   Announce the state, if it changed.
 *
 ****************************************************************************/

static void tel_publish(FAR struct telephony_s *tel)
{
  FAR pnut_telephony_info_t *s = &tel->state;
  int ret;

  switch (tel->step)
    {
      case STEP_WAITING:
      case STEP_OFF:
        s->modem = PNUT_TELEPHONY_INFO_MODEM_OFF;
        break;

      case STEP_FAILED:
        s->modem = PNUT_TELEPHONY_INFO_MODEM_FAILED;
        break;

      case STEP_ON:
        s->modem = PNUT_TELEPHONY_INFO_MODEM_ON;
        break;

      case STEP_SHUTDOWN:
      case STEP_KEY_OFF:
      case STEP_FADING:
      case STEP_SETTLE:
        s->modem = PNUT_TELEPHONY_INFO_MODEM_STOPPING;
        break;

      default:
        s->modem = PNUT_TELEPHONY_INFO_MODEM_STARTING;
        break;
    }

  if (s->modem != PNUT_TELEPHONY_INFO_MODEM_ON)
    {
      s->registration     = PNUT_TELEPHONY_INFO_REGISTRATION_NOT_REGISTERED;
      s->operator_name[0] = '\0';
      s->tech             = PNUT_TELEPHONY_INFO_TECH_NO_TECH;
      s->bars             = 0;
      s->dbm              = 0;
      if (s->modem != PNUT_TELEPHONY_INFO_MODEM_STARTING)
        {
          s->sim   = PNUT_TELEPHONY_INFO_SIM_SIM_UNKNOWN;
          s->tries = 0;
        }
    }

  if (s->modem == PNUT_TELEPHONY_INFO_MODEM_ON && tel->airplane)
    {
      s->reason = PNUT_TELEPHONY_INFO_REASON_AIRPLANE;
    }
  else if (s->reason == PNUT_TELEPHONY_INFO_REASON_AIRPLANE)
    {
      s->reason = PNUT_TELEPHONY_INFO_REASON_NO_REASON;
    }

  if (tel->states == NULL ||
      (tel->announced && memcmp(s, &tel->published, sizeof(*s)) == 0))
    {
      return;
    }

  ret = pnut_telephony_info_publish(tel->states, s);
  if (ret < 0)
    {
      pnut_log(&tel->module, PNUT_LOG_WARNING, "Cannot announce: %d", ret);
      return;
    }

  tel->published = *s;
  tel->announced = true;
}

/****************************************************************************
 * Name: tel_reason
 ****************************************************************************/

static void tel_reason(FAR struct telephony_s *tel, uint32_t reason)
{
  tel->state.reason = reason;
  tel_publish(tel);
}

/****************************************************************************
 * Name: tel_publish_time
 *
 * Description:
 *   A reading of the time, on network_time (RFC 0028).
 *
 ****************************************************************************/

static void tel_publish_time(FAR struct telephony_s *tel,
                             FAR const struct tel_time_s *t,
                             uint32_t uncertainty, uint64_t taken,
                             uint32_t kind)
{
  pnut_network_time_t msg;
  int ret;

  if (tel->times == NULL)
    {
      return;
    }

  memset(&msg, 0, sizeof(msg));
  msg.utc         = t->has_time ? t->utc : 0;
  msg.uncertainty = uncertainty;
  msg.has_offset  = t->has_offset;
  msg.offset      = t->offset;
  msg.dst         = t->dst;
  msg.taken       = taken;
  msg.kind        = kind;

  ret = pnut_network_time_publish(tel->times, &msg);
  if (ret < 0)
    {
      pnut_log(&tel->module, PNUT_LOG_WARNING,
               "Cannot announce the network's time: %d", ret);
    }
}

/****************************************************************************
 * Name: tel_uart_write
 *
 * Description:
 *   The multiplexer's bytes to the UART, not blocking.
 *
 ****************************************************************************/

static ssize_t tel_uart_write(FAR void *arg, FAR const uint8_t *buf,
                              size_t len)
{
  FAR struct telephony_s *tel = arg;
  ssize_t n;

  n = write(tel->uart, buf, len);
  if (n < 0)
    {
      return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ?
             0 : -errno;
    }

  return n;
}

/****************************************************************************
 * Name: tel_uart
 *
 * Description:
 *   The UART is readable, or takes more.
 *
 ****************************************************************************/

static void tel_uart(FAR struct pnut_loop_s *loop, int fd, uint32_t events,
                     FAR void *arg)
{
  FAR struct telephony_s *tel = arg;
  uint8_t buf[256];
  ssize_t n;

  if (events & EPOLLIN)
    {
      n = read(fd, buf, sizeof(buf));
      if (n > 0)
        {
          if (tel->mux.active)
            {
              mux_input(&tel->mux, buf, n);
            }
          else
            {
              at_input(&tel->at, (FAR const char *)buf, n);
            }
        }
    }

  if (events & EPOLLOUT)
    {
      tel_flush(tel);
    }
}

/****************************************************************************
 * Name: tel_flush
 *
 * Description:
 *   Write what waits for the UART; watch it for room while bytes wait.
 *
 ****************************************************************************/

static void tel_flush(FAR struct telephony_s *tel)
{
  int ret;
  bool more;

  if (tel->uart < 0)
    {
      return;
    }

  ret = mux_flush(&tel->mux);
  if (ret < 0 && ret != -EAGAIN)
    {
      pnut_log(&tel->module, PNUT_LOG_ERROR, "Cannot write the UART: %d",
               ret);
    }

  more = mux_waiting(&tel->mux);
  if (more != tel->writing)
    {
      tel->writing = more;
      pnut_loop_rewatch(tel->module.loop, tel->uart,
                        more ? EPOLLIN | EPOLLOUT : EPOLLIN);
    }
}

/****************************************************************************
 * Name: tel_at_send
 *
 * Description:
 *   The AT engine's bytes: on the multiplexer's AT channel, or plain.
 *
 ****************************************************************************/

static int tel_at_send(FAR void *arg, FAR const char *buf, size_t len)
{
  FAR struct telephony_s *tel = arg;
  int ret;

  ret = mux_send(&tel->mux, MUX_AT, buf, len);
  if (ret >= 0)
    {
      tel_flush(tel);
    }

  return ret;
}

/****************************************************************************
 * Name: tel_answered
 *
 * Description:
 *   A command's result, for the count of commands unanswered: a modem that
 *   misses two in a row has stopped (RFC 0030).
 *
 ****************************************************************************/

static void tel_restarted(FAR struct telephony_s *tel, FAR const char *why);

static bool tel_answered(FAR struct telephony_s *tel, int result)
{
  if (result != -ETIMEDOUT)
    {
      tel->silent = 0;
      return true;
    }

  if (tel->step == STEP_ON || tel->step == STEP_SETUP)
    {
      if (++tel->silent >= TEL_SILENT_MAX)
        {
          tel->silent = 0;
          tel_restarted(tel, "stopped answering");
        }
    }

  return false;
}

/****************************************************************************
 * Name: tel_setup_answered
 *
 * Description:
 *   A setting's answer: false when the settings are over (a cancel, or a
 *   modem that stopped answering and is started again).  One command left
 *   unanswered is one that failed, and the settings go on.
 *
 ****************************************************************************/

static bool tel_setup_answered(FAR struct telephony_s *tel, int result)
{
  if (tel->step != STEP_SETUP || result == -ECANCELED)
    {
      return false;
    }

  tel_answered(tel, result);
  return tel->step == STEP_SETUP;
}

/****************************************************************************
 * Name: tel_restart_counts
 *
 * Description:
 *   Count a restart of the modem's: whether it may start again, three
 *   times in an hour at most.
 *
 ****************************************************************************/

static bool tel_restart_counts(FAR struct telephony_s *tel)
{
  uint64_t now = tel_now();
  uint8_t i;
  uint8_t n = 0;

  for (i = 0; i < tel->nrestarts; i++)
    {
      if (now - tel->restarts[i] < TEL_HOUR_MS)
        {
          tel->restarts[n++] = tel->restarts[i];
        }
    }

  tel->nrestarts = n;
  if (n >= TELEPHONY_RESTARTS)
    {
      return false;
    }

  tel->restarts[tel->nrestarts++] = now;
  return true;
}

/****************************************************************************
 * Name: tel_give_up
 *
 * Description:
 *   The modem keeps stopping: off, until it is turned on again; GSM, if it
 *   was allowed, off too.
 *
 ****************************************************************************/

static void tel_give_up(FAR struct telephony_s *tel)
{
  if (tel->gsm && tel->settings_open)
    {
      pnut_log(&tel->module, PNUT_LOG_WARNING,
               "The modem kept stopping with GSM allowed: GSM off");
      tel->gsm = false;
      pnut_telephony_settings_set_gsm(&tel->settings, false, NULL, NULL);
      tel->state.reason = PNUT_TELEPHONY_INFO_REASON_GSM_TURNED_OFF;
    }
  else
    {
      tel->state.reason = PNUT_TELEPHONY_INFO_REASON_RESTARTS;
    }

  pnut_log(&tel->module, PNUT_LOG_ERROR,
           "The modem stopped %d times in an hour: left off",
           TELEPHONY_RESTARTS);
  tel_power_down(tel, AFTER_FAILED);
}

/****************************************************************************
 * Name: tel_rebooted
 *
 * Description:
 *   The modem restarted by itself (an unframed "RDY"), or left the
 *   multiplexer: in plain AT now, without its settings.  Into the
 *   multiplexer again, its settings set again, its state read again (RFC
 *   0030); three times an hour at most.
 *
 ****************************************************************************/

static void tel_boot_poll(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg);

static void tel_rebooted(FAR struct telephony_s *tel, FAR const char *why,
                         bool close)
{
  pnut_log(&tel->module, PNUT_LOG_WARNING, "The modem %s", why);

  /* Still in the multiplexer: closed down, to plain AT */

  if (close && tel->mux.active)
    {
      mux_close(&tel->mux);
      tel_flush(tel);
    }

  if (!tel_restart_counts(tel))
    {
      mux_reset(&tel->mux);         /* AT+CPOF in plain AT */
      tel_give_up(tel);
      return;
    }

  tel_untime(tel);
  mux_reset(&tel->mux);
  at_cancel(&tel->at, -ECANCELED);
  tel->silent = 0;
  tel->nitz   = false;
  tel->cereg  = PNUT_TELEPHONY_INFO_REGISTRATION_NOT_REGISTERED;
  tel->creg   = PNUT_TELEPHONY_INFO_REGISTRATION_NOT_REGISTERED;
  tel_step(tel, STEP_BOOT);
  tel_timer(tel, TEL_POLL_MS, tel_boot_poll);
}

/****************************************************************************
 * Name: tel_restarted
 *
 * Description:
 *   The modem stopped answering, or restarted by itself: started again in
 *   order, three times an hour at most.
 *
 ****************************************************************************/

static void tel_restarted(FAR struct telephony_s *tel, FAR const char *why)
{
  pnut_log(&tel->module, PNUT_LOG_WARNING, "The modem %s", why);
  if (!tel_restart_counts(tel))
    {
      tel_give_up(tel);
      return;
    }

  tel_power_down(tel, AFTER_RESTART);
}

/****************************************************************************
 * Name: tel_boot_poll
 *
 * Description:
 *   While the modem boots: asked every second until it answers, for the
 *   boot time at most.
 *
 ****************************************************************************/

static void tel_cmux(FAR struct telephony_s *tel);

static void tel_boot_answer(FAR void *arg, int result,
                            FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;

  if (tel->step != STEP_BOOT && tel->step != STEP_PROBE)
    {
      return;
    }

  if (result == OK || result == -EIO)
    {
      /* An answer, OK or not: it is up */

      pnut_log(&tel->module, PNUT_LOG_INFO, "The modem answers%s",
               tel->step == STEP_PROBE ? ", left on" : "");
      if (tel->step == STEP_PROBE && !tel->enabled)
        {
          tel_step(tel, STEP_CMUX);       /* Talking: shut it down */
          tel_power_down(tel, AFTER_OFF);
          return;
        }

      tel_cmux(tel);
    }
}

static void tel_boot_poll(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;

  tel->timer = NULL;
  if (tel->step != STEP_BOOT)
    {
      return;
    }

  if (tel_now() - tel->since > tel_config(tel->config.boot, TEL_BOOT_MS))
    {
      pnut_log(&tel->module, PNUT_LOG_ERROR,
               "The modem did not answer after power-up");
      tel->state.reason = PNUT_TELEPHONY_INFO_REASON_NO_ANSWER;
      tel->down_key     = true;
      tel_power_down(tel, AFTER_FAILED);
      return;
    }

  tel_send(tel, "AT", NULL, TEL_POLL_MS - 100, tel_boot_answer);
  tel_timer(tel, TEL_POLL_MS, tel_boot_poll);
}

/****************************************************************************
 * Name: tel_key_released
 *
 * Description:
 *   The key was pressed to start the modem: now it boots.
 *
 ****************************************************************************/

static void tel_key_released(FAR struct pnut_loop_s *loop,
                             FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;

  tel->timer = NULL;
  tel_line(tel, TELEPHONY_KEY, false);
  tel_step(tel, STEP_BOOT);
  tel_timer(tel, TEL_POLL_MS, tel_boot_poll);
}

/****************************************************************************
 * Name: tel_rail_up
 ****************************************************************************/

static void tel_rail_up(FAR struct pnut_loop_s *loop,
                        FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;

  tel->timer = NULL;
  tel_line(tel, TELEPHONY_KEY, true);
  tel_step(tel, STEP_KEY);
  tel_timer(tel, tel_config(tel->config.key_on, TEL_KEY_ON_MS),
            tel_key_released);
}

/****************************************************************************
 * Name: tel_power_up
 *
 * Description:
 *   The rail on, then the key, then waiting for the modem to answer.
 *
 ****************************************************************************/

static void tel_power_up(FAR struct telephony_s *tel)
{
  pnut_log(&tel->module, PNUT_LOG_INFO, "Powering the modem up");
  mux_reset(&tel->mux);
  at_cancel(&tel->at, -ECANCELED);
  tel->silent   = 0;
  tel->nitz     = false;
  tel->down_key = false;
  tel->cereg    = PNUT_TELEPHONY_INFO_REGISTRATION_NOT_REGISTERED;
  tel->creg     = PNUT_TELEPHONY_INFO_REGISTRATION_NOT_REGISTERED;
  if (tel->state.reason != PNUT_TELEPHONY_INFO_REASON_GSM_TURNED_OFF)
    {
      tel->state.reason = PNUT_TELEPHONY_INFO_REASON_NO_REASON;
    }

  tel->up_at = tel_now();
  tel_line(tel, TELEPHONY_POWER, true);
  tel_step(tel, STEP_RAIL);
  tel_timer(tel, TEL_RAIL_MS, tel_rail_up);
}

/****************************************************************************
 * Name: tel_probe
 *
 * Description:
 *   At start, with the rail on: a modem left running may still be in the
 *   multiplexer of a telephony program that stopped.  Closed blindly, it
 *   is asked whether it answers.
 *
 ****************************************************************************/

static void tel_probe_answer(FAR void *arg, int result,
                             FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;

  if (tel->step != STEP_PROBE || result == -ECANCELED)
    {
      return;
    }

  if (result == OK || result == -EIO)
    {
      tel_boot_answer(tel, result, reply);
      return;
    }

  if (++tel->tries < 2)
    {
      tel_send(tel, "AT", NULL, TEL_PROBE_MS, tel_probe_answer);
      return;
    }

  /* Silent: off, with its rail on.  Started with the key, or the rail
   * cut: a modem that does not answer is not running
   */

  if (tel->enabled)
    {
      tel_line(tel, TELEPHONY_KEY, true);
      tel_step(tel, STEP_KEY);
      tel_timer(tel, tel_config(tel->config.key_on, TEL_KEY_ON_MS),
                tel_key_released);
    }
  else
    {
      tel_line(tel, TELEPHONY_POWER, false);
      tel->state.reason = PNUT_TELEPHONY_INFO_REASON_DISABLED;
      tel_step(tel, STEP_OFF);
    }
}

static void tel_probe_kicked(FAR struct pnut_loop_s *loop,
                             FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;

  tel->timer = NULL;
  tel->tries = 0;
  tel_send(tel, "AT", NULL, TEL_PROBE_MS, tel_probe_answer);
}

static void tel_probe(FAR struct telephony_s *tel)
{
  tel->up_at = tel_now();
  tel_step(tel, STEP_PROBE);
  mux_reset(&tel->mux);
  mux_kick(&tel->mux);
  tel_flush(tel);
  tel_timer(tel, TEL_READY_MS, tel_probe_kicked);
}

/****************************************************************************
 * Name: tel_settled
 *
 * Description:
 *   The modem stopped answering a while ago: the rail goes, and what
 *   follows the power-down.
 *
 ****************************************************************************/

static void tel_restart_later(FAR struct pnut_loop_s *loop,
                              FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;

  tel->timer = NULL;
  if (tel->step == STEP_OFF && tel->enabled && !tel->finishing)
    {
      tel_power_up(tel);
    }
}

static void tel_settled(FAR struct pnut_loop_s *loop,
                        FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;
  uint8_t after = tel->after;

  tel->timer = NULL;
  tel_line(tel, TELEPHONY_POWER, false);
  mux_reset(&tel->mux);
  at_cancel(&tel->at, -ECANCELED);
  pnut_log(&tel->module, PNUT_LOG_INFO, "The modem is off");

  if (tel->finishing)
    {
      tel_step(tel, STEP_OFF);
      pnut_module_finished(&tel->module);
      return;
    }

  tel_step(tel, after == AFTER_FAILED ? STEP_FAILED : STEP_OFF);

  /* Started again after a wait that grows with each restart of the hour
   * (RFC 0030)
   */

  if (after == AFTER_RESTART && tel->enabled)
    {
      tel_timer(tel, TEL_RESTART_MS << (tel->nrestarts > 0 ?
                                        tel->nrestarts - 1 : 0),
                tel_restart_later);
    }
}

/****************************************************************************
 * Name: tel_fade
 *
 * Description:
 *   Shut down: asked until it stops answering.  One that still answers
 *   after the off time is stopped with the key, once; then the rail goes
 *   anyway.
 *
 ****************************************************************************/

static void tel_fade_poll(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg);

static void tel_key_off_released(FAR struct pnut_loop_s *loop,
                                 FAR struct pnut_timer_s *timer,
                                 FAR void *arg)
{
  FAR struct telephony_s *tel = arg;

  tel->timer = NULL;
  tel_line(tel, TELEPHONY_KEY, false);
  tel_step(tel, STEP_FADING);
  tel_timer(tel, TEL_FADE_MS, tel_fade_poll);
}

static void tel_key_off(FAR struct telephony_s *tel)
{
  tel->down_key = true;
  tel_line(tel, TELEPHONY_KEY, true);
  tel_step(tel, STEP_KEY_OFF);
  tel_timer(tel, tel_config(tel->config.key_off, TEL_KEY_OFF_MS),
            tel_key_off_released);
}

static void tel_fade_answer(FAR void *arg, int result,
                            FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;

  if (tel->step != STEP_FADING || result == -ECANCELED)
    {
      return;
    }

  /* Unanswered: it has stopped.  Not asked at all (the channel gone):
   * not known, so the key, once, before the rail
   */

  if (result == -ETIMEDOUT || (result != OK && result != -EIO &&
                               tel->down_key))
    {
      tel_step(tel, STEP_SETTLE);
      tel_timer(tel, tel_config(tel->config.settle, TEL_SETTLE_MS),
                tel_settled);
      return;
    }

  if (result != OK && result != -EIO)
    {
      tel_key_off(tel);
      return;
    }

  if (tel_now() - tel->since < tel_config(tel->config.off, TEL_OFF_MS))
    {
      tel_timer(tel, TEL_FADE_MS, tel_fade_poll);
    }
  else if (!tel->down_key)
    {
      pnut_log(&tel->module, PNUT_LOG_WARNING,
               "The modem still answers: the power key");
      tel_key_off(tel);
    }
  else
    {
      pnut_log(&tel->module, PNUT_LOG_ERROR,
               "The modem still answers: its rail cut");
      tel_step(tel, STEP_SETTLE);
      tel_timer(tel, 1, tel_settled);
    }
}

static void tel_fade_poll(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;

  tel->timer = NULL;
  if (tel->step == STEP_FADING)
    {
      tel_send(tel, "AT", NULL, TEL_FADE_MS, tel_fade_answer);
    }
}

/****************************************************************************
 * Name: tel_shut
 *
 * Description:
 *   AT+CPOF's answer, or none: the modem goes down.
 *
 ****************************************************************************/

static void tel_shut(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;

  if (tel->step != STEP_SHUTDOWN || result == -ECANCELED)
    {
      return;
    }

  /* Answered, OK or not: it goes down.  Unanswered, or not sent at all:
   * the key
   */

  if (result != OK && result != -EIO)
    {
      tel_key_off(tel);
      return;
    }

  tel_step(tel, STEP_FADING);
  tel_timer(tel, TEL_FADE_MS, tel_fade_poll);
}

/****************************************************************************
 * Name: tel_shut_later
 *
 * Description:
 *   Out of the multiplexer a moment ago: AT+CPOF, in plain AT.
 *
 ****************************************************************************/

static void tel_shut_later(FAR struct pnut_loop_s *loop,
                           FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;

  tel->timer = NULL;
  if (tel->step == STEP_SHUTDOWN &&
      tel_send(tel, "AT+CPOF", NULL, TEL_CPOF_MS, tel_shut) < 0)
    {
      tel_key_off(tel);
    }
}

/****************************************************************************
 * Name: tel_power_down
 *
 * Description:
 *   Power the modem down, always in order: AT+CPOF, or else the key held;
 *   then waiting until it stops answering; only then the rail (SIMCom:
 *   cutting a running modem's supply can damage its flash).
 *
 ****************************************************************************/

static void tel_power_down(FAR struct telephony_s *tel, uint8_t after)
{
  bool talking = tel_talking(tel);
  uint8_t step = tel->step;

  tel->after = after;
  if (tel->trial != NULL)
    {
      pnut_timer_cancel(tel->module.loop, tel->trial);
      tel->trial = NULL;
    }

  if (step >= STEP_SHUTDOWN)
    {
      return;                             /* On its way down already */
    }

  tel_untime(tel);
  at_cancel(&tel->at, -ECANCELED);

  if (step == STEP_WAITING || step == STEP_OFF || step == STEP_FAILED)
    {
      if (tel_rail(tel) && step == STEP_WAITING)
        {
          tel_line(tel, TELEPHONY_POWER, false);
        }

      tel_step(tel, after == AFTER_FAILED ? STEP_FAILED : STEP_OFF);
      if (tel->finishing)
        {
          pnut_module_finished(&tel->module);
        }

      return;
    }

  pnut_log(&tel->module, PNUT_LOG_INFO, "Powering the modem down");

  if (step == STEP_RAIL)
    {
      /* Not started yet: the rail alone */

      tel_step(tel, STEP_SETTLE);
      tel_timer(tel, 1, tel_settled);
      return;
    }

  /* In a multiplexer whose AT channel is not open yet: closed down
   * first, so that AT+CPOF goes in plain AT
   */

  if (tel->mux.active && !(tel->mux.opened & (1 << MUX_AT)))
    {
      mux_close(&tel->mux);
      tel_flush(tel);
      if (talking && !tel->down_key)
        {
          /* A moment for the modem to leave it (the previous system's) */

          tel_step(tel, STEP_SHUTDOWN);
          tel_timer(tel, TEL_CLOSE_MS, tel_shut_later);
          return;
        }
    }

  if (talking && !tel->down_key)
    {
      tel_step(tel, STEP_SHUTDOWN);
      if (tel_send(tel, "AT+CPOF", NULL, TEL_CPOF_MS, tel_shut) < 0)
        {
          tel_key_off(tel);
        }

      return;
    }

  /* Booting, or silent: the key, held */

  tel_key_off(tel);
}

/****************************************************************************
 * Name: tel_open_next
 *
 * Description:
 *   Open the multiplexer's channels in turn: control, AT, data.
 *
 ****************************************************************************/

static void tel_open_timeout(FAR struct pnut_loop_s *loop,
                             FAR struct pnut_timer_s *timer, FAR void *arg);

static void tel_open_next(FAR struct telephony_s *tel, uint8_t dlci)
{
  int ret;

  ret = mux_open(&tel->mux, dlci);
  tel_flush(tel);
  if (ret < 0)
    {
      pnut_log(&tel->module, PNUT_LOG_ERROR,
               "Cannot open the multiplexer's channel %u: %d", dlci, ret);
    }

  tel_timer(tel, tel_config(tel->config.open, TEL_OPEN_MS),
            tel_open_timeout);
}

static uint8_t tel_unopened(FAR struct telephony_s *tel)
{
  uint8_t dlci;

  for (dlci = 0; dlci < MUX_DLCIS; dlci++)
    {
      if (!(tel->mux.opened & (1 << dlci)))
        {
          return dlci;
        }
    }

  return MUX_DLCIS;
}

static void tel_open_timeout(FAR struct pnut_loop_s *loop,
                             FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;

  tel->timer = NULL;
  if (tel->step != STEP_OPEN)
    {
      return;
    }

  if (++tel->tries < TEL_OPEN_TRIES)
    {
      tel_open_next(tel, tel_unopened(tel));
      return;
    }

  tel_restarted(tel, "did not open the multiplexer's channels");
}

/****************************************************************************
 * Name: tel_ready
 *
 * Description:
 *   The channels are open, and answer by now: the first command on a fresh
 *   channel is refused, so a plain AT goes first, twice.
 *
 ****************************************************************************/

static void tel_first(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;

  if (tel->step != STEP_READY || result == -ECANCELED)
    {
      return;
    }

  if (++tel->tries < 2)
    {
      tel_send(tel, "AT", NULL, 0, tel_first);
      return;
    }

  if (!tel_answered(tel, result))
    {
      tel_restarted(tel, "did not answer on the multiplexer");
      return;
    }

  tel_step(tel, STEP_SETUP);
  tel->setup = 0;
  tel_setup_next(tel);
}

static void tel_ready(FAR struct pnut_loop_s *loop,
                      FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;

  tel->timer = NULL;
  if (tel->step != STEP_READY)
    {
      return;
    }

  tel->tries = 0;
  tel_send(tel, "AT", NULL, 0, tel_first);
}

/****************************************************************************
 * Name: tel_cmux
 *
 * Description:
 *   The modem answers in plain AT: into the multiplexer (AT+CMUX), at once
 *   (RFC 0030).
 *
 ****************************************************************************/

static void tel_switched(FAR struct pnut_loop_s *loop,
                         FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;

  tel->timer = NULL;
  if (tel->step == STEP_OPEN)
    {
      tel_open_next(tel, MUX_CONTROL);
    }
}

static void tel_cmuxed(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;

  if (tel->step != STEP_CMUX || result == -ECANCELED)
    {
      return;
    }

  if (result < 0)
    {
      tel_restarted(tel, "refused the multiplexer");
      return;
    }

  /* The modem takes a moment to switch (the previous system's finding) */

  mux_start(&tel->mux);
  tel_step(tel, STEP_OPEN);
  tel->tries = 0;
  tel_timer(tel, TEL_SWITCH_MS, tel_switched);
}

static void tel_cmux(FAR struct telephony_s *tel)
{
  char cmd[32];

  tel_untime(tel);
  tel_step(tel, STEP_CMUX);
  snprintf(cmd, sizeof(cmd), "AT+CMUX=0,0,5,%d", MUX_N1);
  tel_send(tel, cmd, NULL, 3000, tel_cmuxed);
}

/****************************************************************************
 * Name: tel_mux_data
 ****************************************************************************/

static void tel_mux_data(FAR void *arg, uint8_t dlci,
                         FAR const uint8_t *buf, size_t len)
{
  FAR struct telephony_s *tel = arg;

  if (dlci == MUX_AT)
    {
      at_input(&tel->at, (FAR const char *)buf, len);
    }

  /* The data channel's, PPP's, come with mobile data (step 4) */
}

/****************************************************************************
 * Name: tel_mux_event
 ****************************************************************************/

static void tel_mux_event(FAR void *arg, uint8_t dlci, int event)
{
  FAR struct telephony_s *tel = arg;

  switch (event)
    {
      case MUX_OPENED:
        if (tel->step != STEP_OPEN)
          {
            break;
          }

        tel->tries = 0;
        if (tel_unopened(tel) < MUX_DLCIS)
          {
            tel_open_next(tel, tel_unopened(tel));
            break;
          }

        mux_signals(&tel->mux, MUX_AT, MUX_V24);
        mux_signals(&tel->mux, MUX_DATA, MUX_V24);
        tel_flush(tel);
        tel_step(tel, STEP_READY);
        tel_timer(tel, tel_config(tel->config.ready, TEL_READY_MS),
                  tel_ready);
        break;

      case MUX_CLOSED:

        /* A channel closed, or refused: the modem is still in the
         * multiplexer
         */

        if (tel->step >= STEP_OPEN && tel->step <= STEP_ON)
          {
            tel_rebooted(tel, "closed a channel", true);
          }
        break;

      case MUX_ENDED:
      case MUX_RESTARTED:

        /* In plain AT now.  An "RDY" before the settings may be the end of
         * its start's banner, late: the channels' answers tell then
         */

        if ((tel->step >= STEP_SETUP && tel->step <= STEP_ON) ||
            (event == MUX_ENDED && tel->step >= STEP_OPEN &&
             tel->step <= STEP_ON))
          {
            tel_rebooted(tel, event == MUX_RESTARTED ?
                         "restarted by itself" :
                         "closed the multiplexer", false);
          }
        else if (tel->step >= STEP_SHUTDOWN)
          {
            mux_reset(&tel->mux);         /* Asked in plain AT now */
          }
        break;

      case MUX_ROOM:
        at_room(&tel->at);
        break;
    }
}

/****************************************************************************
 * Name: tel_setup_done
 *
 * Description:
 *   One of the settings is set, or not: the next.
 *
 ****************************************************************************/

static void tel_setup_done(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;

  if (!tel_setup_answered(tel, result))
    {
      return;
    }

  if (result < 0)
    {
      pnut_log(&tel->module, PNUT_LOG_WARNING, "%s failed: %s",
               tel->setup > 0 && tel->setup <= TEL_SETUP_BASIC ?
               g_tel_setup[tel->setup - 1] : "A setting", reply);
    }

  tel_setup_next(tel);
}

/****************************************************************************
 * Name: tel_bands_read
 *
 * Description:
 *   The modem's bands: set to the description's where they differ.  The
 *   modem keeps them across power cycles, so they are set only then.
 *
 ****************************************************************************/

static void tel_bands_read(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;
  char line[128];
  FAR const char *p;
  FAR char *end;
  uint64_t gsm = 0;
  uint64_t lte = 0;
  char cmd[64];

  if (!tel_setup_answered(tel, result))
    {
      return;
    }

  /* Not read: left as the modem has them */

  p = tel_find(reply, line, sizeof(line), "+CNBP:");
  if (result != OK || p == NULL)
    {
      tel_setup_next(tel);
      return;
    }

  p   = tel_args(p);
  gsm = strtoull(p, &end, 16);
  lte = *end == ',' ? strtoull(end + 1, NULL, 16) : 0;

  if (gsm == tel->gsm_bands && lte == tel->lte_bands)
    {
      tel_setup_next(tel);
      return;
    }

  snprintf(cmd, sizeof(cmd), "AT+CNBP=0x%016" PRIX64 ",0x%016" PRIX64,
           tel->gsm_bands, tel->lte_bands);
  pnut_log(&tel->module, PNUT_LOG_INFO, "The modem's bands set: %s", cmd);
  tel_send(tel, cmd, NULL, 0, tel_setup_done);
}

/****************************************************************************
 * Name: tel_mode_read
 *
 * Description:
 *   The modem's mode: LTE only, or automatic where GSM is allowed.
 *
 ****************************************************************************/

static void tel_mode_read(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;
  char line[128];
  FAR const char *p;
  int want = tel->gsm ? TEL_CNMP_AUTO : TEL_CNMP_LTE;
  char cmd[16];

  if (!tel_setup_answered(tel, result))
    {
      return;
    }

  p = tel_find(reply, line, sizeof(line), "+CNMP:");
  if (result == OK && p != NULL && atoi(tel_args(p)) == want)
    {
      tel_setup_next(tel);
      return;
    }

  snprintf(cmd, sizeof(cmd), "AT+CNMP=%d", want);
  pnut_log(&tel->module, PNUT_LOG_INFO, "The modem's mode set: %s", cmd);
  tel_send(tel, cmd, NULL, 0, tel_setup_done);
}

/****************************************************************************
 * Name: tel_air
 *
 * Description:
 *   Airplane mode, as Board announces it: off the air with AT+CFUN=4, the
 *   SIM still reachable; back with AT+CFUN=1.
 *
 ****************************************************************************/

static void tel_air_done(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;

  if (tel->step == STEP_SETUP)
    {
      if (tel_setup_answered(tel, result))
        {
          if (result < 0)
            {
              pnut_log(&tel->module, PNUT_LOG_WARNING,
                       "Airplane mode not applied: %s", reply);
            }

          tel_setup_next(tel);
        }

      return;
    }

  if (result == -ECANCELED || !tel_answered(tel, result))
    {
      return;
    }

  if (result < 0)
    {
      pnut_log(&tel->module, PNUT_LOG_WARNING,
               "Airplane mode not applied: %s", reply);
    }

  tel_refresh(tel);
}

static void tel_air_read(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;
  char line[128];
  FAR const char *p;
  int want = tel->airplane ? 4 : 1;

  if (!tel_setup_answered(tel, result))
    {
      return;
    }

  tel->air_set = tel->airplane;
  p = tel_find(reply, line, sizeof(line), "+CFUN:");
  if (result == OK && p != NULL && atoi(tel_args(p)) == want)
    {
      tel_setup_next(tel);
      return;
    }

  tel_send(tel, want == 4 ? "AT+CFUN=4" : "AT+CFUN=1", NULL, 10000,
           tel_air_done);
}

static void tel_air(FAR struct telephony_s *tel)
{
  tel_publish(tel);
  if (tel->step == STEP_ON)
    {
      tel->air_set = tel->airplane;
      pnut_log(&tel->module, PNUT_LOG_INFO, "Airplane mode %s",
               tel->airplane ? "on: the modem off the air" : "off");
      tel_send(tel, tel->airplane ? "AT+CFUN=4" : "AT+CFUN=1", NULL, 10000,
               tel_air_done);
    }
}

/****************************************************************************
 * Name: tel_sim
 *
 * Description:
 *   The SIM's state, as read or reported.
 *
 ****************************************************************************/

static void tel_tries_read(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;
  char line[128];
  FAR const char *p;
  uint32_t pin;
  uint32_t puk;

  if (result == -ECANCELED || !tel_answered(tel, result))
    {
      return;
    }

  p = tel_find(reply, line, sizeof(line), "+SPIC:");
  if (result == OK && p != NULL && tel_tries(p, &pin, &puk) == OK)
    {
      tel->state.tries =
        tel->state.sim == PNUT_TELEPHONY_INFO_SIM_NEEDS_PUK ? puk : pin;
      tel_publish(tel);
    }
}

static void tel_sim_state(FAR struct telephony_s *tel, int sim)
{
  int was = tel->state.sim;

  if (sim < 0 || (sim == was && sim != PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN))
    {
      return;
    }

  tel->state.sim = sim;
  if (sim == PNUT_TELEPHONY_INFO_SIM_READY)
    {
      tel->state.tries = 0;
    }

  tel_publish(tel);

  switch (sim)
    {
      case PNUT_TELEPHONY_INFO_SIM_READY:
        if (was != sim)
          {
            pnut_log(&tel->module, PNUT_LOG_INFO, "The SIM is ready");
            tel_refresh(tel);
          }
        break;

      case PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN:
      case PNUT_TELEPHONY_INFO_SIM_NEEDS_PUK:
        if (was != sim)
          {
            pnut_log(&tel->module, PNUT_LOG_INFO, "The SIM needs its %s",
                     sim == PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN ?
                     "PIN" : "PUK");
          }

        tel_send(tel, "AT+SPIC", "+SPIC:", 0, tel_tries_read);
        break;

      case PNUT_TELEPHONY_INFO_SIM_ABSENT:

        /* With no SIM the modem can do nothing useful here, at 35-60 mA:
         * off.  A SIM put in is read at the modem's next start.
         */

        pnut_log(&tel->module, PNUT_LOG_INFO, "No SIM: the modem goes off");
        tel->state.reason = PNUT_TELEPHONY_INFO_REASON_NO_SIM;
        tel_power_down(tel, AFTER_OFF);
        break;

      case PNUT_TELEPHONY_INFO_SIM_BLOCKED:
      case PNUT_TELEPHONY_INFO_SIM_SIM_FAILED:
        pnut_log(&tel->module, PNUT_LOG_WARNING, "The SIM %s",
                 sim == PNUT_TELEPHONY_INFO_SIM_BLOCKED ? "is blocked" :
                 "failed");
        break;
    }
}

static void tel_sim_retry(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg);

static void tel_sim_read(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;
  char line[128];
  FAR const char *p;
  int sim = -EINVAL;

  if (tel->step == STEP_SETUP)
    {
      if (!tel_setup_answered(tel, result))
        {
          return;
        }
    }
  else if (result == -ECANCELED || !tel_answered(tel, result))
    {
      return;
    }

  /* The answer, or an error that tells what is wrong with the SIM; a busy
   * SIM, still reading its phone book, or no answer, is asked again
   */

  p = tel_find(reply, line, sizeof(line), "+CPIN:");
  if (p == NULL)
    {
      p = tel_find(reply, line, sizeof(line), "+CME ERROR:");
    }

  if (p != NULL)
    {
      sim = tel_sim(p);
    }

  if (tel->step == STEP_SETUP)
    {
      if (sim < 0 && ++tel->tries < TEL_SIM_TRIES)
        {
          tel_timer(tel, TEL_SIM_MS, tel_sim_retry);
          return;
        }

      tel_untime(tel);
      tel_step(tel, STEP_ON);
      pnut_log(&tel->module, PNUT_LOG_INFO, "The modem is on, %" PRIu64
               " ms after its start", tel_now() - tel->up_at);

      /* Airplane mode changed while the settings were given */

      if (tel->air_set != tel->airplane)
        {
          tel_air(tel);
        }
    }

  if (tel->step == STEP_ON)
    {
      tel_sim_state(tel, sim);
    }
}

static void tel_sim_retry(FAR struct pnut_loop_s *loop,
                          FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;

  tel->timer = NULL;
  if (tel->step == STEP_SETUP)
    {
      tel_send(tel, "AT+CPIN?", "+CPIN:", 0, tel_sim_read);
    }
}

/****************************************************************************
 * Name: tel_trial_over
 *
 * Description:
 *   Two minutes since GSM was allowed: registered nowhere, back to LTE
 *   only (RFC 0030).
 *
 ****************************************************************************/

static void tel_mode_set(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;

  if (result != -ECANCELED && tel_answered(tel, result) && result < 0)
    {
      pnut_log(&tel->module, PNUT_LOG_WARNING, "The mode not set: %s",
               reply);
    }
}

static void tel_trial_over(FAR struct pnut_loop_s *loop,
                           FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;
  uint32_t reg = tel->state.registration;

  tel->trial = NULL;
  if (!tel->gsm || tel->step != STEP_ON ||
      reg == PNUT_TELEPHONY_INFO_REGISTRATION_HOME ||
      reg == PNUT_TELEPHONY_INFO_REGISTRATION_ROAMING)
    {
      return;
    }

  pnut_log(&tel->module, PNUT_LOG_WARNING,
           "No network two minutes after GSM was allowed: LTE only again");
  tel->gsm = false;
  if (tel->settings_open)
    {
      pnut_telephony_settings_set_gsm(&tel->settings, false, NULL, NULL);
    }

  tel_reason(tel, PNUT_TELEPHONY_INFO_REASON_GSM_TURNED_OFF);
  tel_send(tel, "AT+CNMP=38", NULL, 0, tel_mode_set);
}

static void tel_trial(FAR struct telephony_s *tel)
{
  if (tel->trial != NULL)
    {
      pnut_timer_cancel(tel->module.loop, tel->trial);
      tel->trial = NULL;
    }

  if (tel->gsm && pnut_timer_start(tel->module.loop, TEL_TRIAL_MS, 0,
                                   tel_trial_over, tel, &tel->trial) < 0)
    {
      tel->trial = NULL;
    }
}

/****************************************************************************
 * Name: tel_setup_next
 *
 * Description:
 *   The next of the settings the modem is given at each start.
 *
 ****************************************************************************/

static void tel_setup_next(FAR struct telephony_s *tel)
{
  uint8_t i = tel->setup++;

  if (i < TEL_SETUP_BASIC)
    {
      tel_send(tel, g_tel_setup[i], NULL, 0, tel_setup_done);
      return;
    }

  switch (i - TEL_SETUP_BASIC)
    {
      case SETUP_BANDS:
        if (tel->lte_bands != 0)
          {
            tel_send(tel, "AT+CNBP?", "+CNBP:", 0, tel_bands_read);
            return;
          }

        /* Without the description's bands, the modem keeps its own */

        tel_setup_next(tel);
        return;

      case SETUP_MODE:
        tel_send(tel, "AT+CNMP?", "+CNMP:", 0, tel_mode_read);
        return;

      case SETUP_AIRPLANE:
        tel_send(tel, "AT+CFUN?", "+CFUN:", 0, tel_air_read);
        return;

      case SETUP_SIM:
        tel->tries = 0;
        tel_send(tel, "AT+CPIN?", "+CPIN:", 0, tel_sim_read);
        tel_trial(tel);
        return;

      default:
        return;
    }
}

/****************************************************************************
 * Name: tel_refresh
 *
 * Description:
 *   Read the signal, the registration and the operator again: when the
 *   registration changes, when asked, and with the hourly reading.
 *
 ****************************************************************************/

static void tel_refresh_step(FAR struct telephony_s *tel, uint8_t which);

static void tel_registered(FAR struct telephony_s *tel)
{
  FAR pnut_telephony_info_t *s = &tel->state;
  int reg;

  /* LTE first, then GSM; registered anywhere wins */

  if (tel->cereg == PNUT_TELEPHONY_INFO_REGISTRATION_HOME ||
      tel->cereg == PNUT_TELEPHONY_INFO_REGISTRATION_ROAMING)
    {
      reg = tel->cereg;
    }
  else if (tel->creg == PNUT_TELEPHONY_INFO_REGISTRATION_HOME ||
           tel->creg == PNUT_TELEPHONY_INFO_REGISTRATION_ROAMING)
    {
      reg = tel->creg;
    }
  else if (tel->cereg == PNUT_TELEPHONY_INFO_REGISTRATION_SEARCHING ||
           tel->creg == PNUT_TELEPHONY_INFO_REGISTRATION_SEARCHING)
    {
      reg = PNUT_TELEPHONY_INFO_REGISTRATION_SEARCHING;
    }
  else if (tel->cereg == PNUT_TELEPHONY_INFO_REGISTRATION_DENIED ||
           tel->creg == PNUT_TELEPHONY_INFO_REGISTRATION_DENIED)
    {
      reg = PNUT_TELEPHONY_INFO_REGISTRATION_DENIED;
    }
  else
    {
      reg = PNUT_TELEPHONY_INFO_REGISTRATION_NOT_REGISTERED;
    }

  if ((uint32_t)reg != s->registration)
    {
      pnut_log(&tel->module, PNUT_LOG_INFO, "Registration: %s",
               reg == PNUT_TELEPHONY_INFO_REGISTRATION_HOME ? "home" :
               reg == PNUT_TELEPHONY_INFO_REGISTRATION_ROAMING ?
               "roaming" :
               reg == PNUT_TELEPHONY_INFO_REGISTRATION_SEARCHING ?
               "searching" :
               reg == PNUT_TELEPHONY_INFO_REGISTRATION_DENIED ?
               "denied" : "none");
    }

  s->registration = reg;
  if (reg != PNUT_TELEPHONY_INFO_REGISTRATION_HOME &&
      reg != PNUT_TELEPHONY_INFO_REGISTRATION_ROAMING)
    {
      s->operator_name[0] = '\0';
      s->tech             = PNUT_TELEPHONY_INFO_TECH_NO_TECH;
    }
}

static void tel_refresh_done(FAR void *arg, int result,
                             FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;
  char line[128];
  FAR pnut_telephony_info_t *s = &tel->state;
  FAR const char *p;
  int tech;
  int reg;

  if (result == -ECANCELED)
    {
      tel->refreshing = false;
      return;
    }

  if (!tel_answered(tel, result) || tel->step != STEP_ON)
    {
      tel->refreshing = false;
      return;
    }

  if (result == OK)
    {
      if ((p = tel_find(reply, line, sizeof(line), "+CSQ:")) != NULL)
        {
          tel_signal(p, &s->dbm, &s->bars);
          tel_refresh_step(tel, 1);
          return;
        }

      if ((p = tel_find(reply, line, sizeof(line), "+CEREG:")) != NULL)
        {
          reg = tel_registration(p, true);
          if (reg >= 0)
            {
              tel->cereg = reg;
            }

          tel_refresh_step(tel, 2);
          return;
        }

      if ((p = tel_find(reply, line, sizeof(line), "+CREG:")) != NULL)
        {
          reg = tel_registration(p, true);
          if (reg >= 0)
            {
              tel->creg = reg;
            }

          tel_registered(tel);
          tel_refresh_step(tel, 3);
          return;
        }

      if ((p = tel_find(reply, line, sizeof(line), "+COPS:")) != NULL)
        {
          tel_operator(p, s->operator_name, sizeof(s->operator_name),
                       &tech);
          s->tech = tech;
        }
    }

  /* The last, or one that failed: the state as read */

  tel->refreshing = false;
  tel_registered(tel);
  tel_publish(tel);
  if (tel->again)
    {
      tel->again = false;
      tel_refresh(tel);
    }
}

static void tel_refresh_step(FAR struct telephony_s *tel, uint8_t which)
{
  static FAR const char *const cmds[][2] =
  {
    { "AT+CSQ", "+CSQ:" },
    { "AT+CEREG?", "+CEREG:" },
    { "AT+CREG?", "+CREG:" },
    { "AT+COPS?", "+COPS:" },
  };

  if (tel_send(tel, cmds[which][0], cmds[which][1], 0,
               tel_refresh_done) < 0)
    {
      tel->refreshing = false;
    }
}

static void tel_refresh(FAR struct telephony_s *tel)
{
  if (tel->step != STEP_ON)
    {
      return;
    }

  if (tel->refreshing)
    {
      tel->again = true;
      return;
    }

  tel->refreshing = true;
  tel->refreshed  = tel_now();
  tel_refresh_step(tel, 0);
}

/****************************************************************************
 * Name: tel_clock_read
 *
 * Description:
 *   The modem's clock, read hourly while registered, once the network has
 *   sent its time this start (RFC 0030): marked as the modem's clock, its
 *   uncertainty a second and 100 ppm of its age since the network's.
 *
 ****************************************************************************/

static void tel_clock_read(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;
  char line[128];
  struct tel_time_s t;
  FAR const char *p;
  uint64_t now = tel_now();

  if (result == -ECANCELED || !tel_answered(tel, result) || result < 0)
    {
      return;
    }

  p = tel_find(reply, line, sizeof(line), "+CCLK:");
  if (p == NULL || tel_clock(p, &t) < 0)
    {
      pnut_log(&tel->module, PNUT_LOG_WARNING, "The modem's clock: %s",
               reply);
      return;
    }

  t.has_offset = false;           /* Its summer time is not known */
  tel_publish_time(tel, &t, 1000 + (uint32_t)((now - tel->nitz_at) / 10000),
                   now, PNUT_NETWORK_TIME_KIND_MODEM_CLOCK);
}

static void tel_hourly(FAR struct telephony_s *tel)
{
  uint64_t now = tel_now();
  uint32_t reg = tel->state.registration;

  if (tel->step != STEP_ON || !tel->nitz ||
      (reg != PNUT_TELEPHONY_INFO_REGISTRATION_HOME &&
       reg != PNUT_TELEPHONY_INFO_REGISTRATION_ROAMING) ||
      now - tel->clock_at < TEL_HOUR_MS)
    {
      return;
    }

  tel->clock_at = now;
  tel_send(tel, "AT+CCLK?", "+CCLK:", 0, tel_clock_read);
  tel_refresh(tel);
}

/****************************************************************************
 * Name: tel_urc
 *
 * Description:
 *   A line the modem sent unasked.
 *
 ****************************************************************************/

static void tel_urc(FAR void *arg, FAR const char *line)
{
  FAR struct telephony_s *tel = arg;
  struct tel_time_s t;
  uint64_t now = tel_now();
  int ret;

  if (strcmp(line, "RDY") == 0 || strncmp(line, "*ATREADY", 8) == 0)
    {
      /* A modem that restarts in plain AT, before the multiplexer: lost
       * its settings
       */

      if (tel->step >= STEP_SETUP && tel->step <= STEP_ON)
        {
          tel_rebooted(tel, "restarted by itself", false);
        }

      return;
    }

  if (tel->step != STEP_ON)
    {
      return;
    }

  if (strncmp(line, "+CPIN:", 6) == 0 || strncmp(line, "+SIMCARD:", 9) == 0)
    {
      tel_sim_state(tel, tel_sim(line));
    }
  else if (strncmp(line, "+CEREG:", 7) == 0 ||
           strncmp(line, "+CREG:", 6) == 0)
    {
      ret = tel_registration(line, false);
      if (ret >= 0)
        {
          if (line[2] == 'E')
            {
              tel->cereg = ret;
            }
          else
            {
              tel->creg = ret;
            }

          tel_registered(tel);
          tel_publish(tel);
          tel_refresh(tel);
        }
    }
  else if (strncmp(line, "+CSQ:", 5) == 0)
    {
      tel_signal(line, &tel->state.dbm, &tel->state.bars);
      tel_publish(tel);
    }
  else if (tel_network_time(line, &t) == OK)
    {
      /* The network's own message: stamped as it came (RFC 0030) */

      if (t.has_time)
        {
          tel->nitz     = true;
          tel->nitz_at  = now;
          tel->clock_at = now;
        }

      pnut_log(&tel->module, PNUT_LOG_INFO, "The network's time%s",
               t.has_time ? "" : ": its offset alone");
      tel_publish_time(tel, &t, 1000, now,
                       PNUT_NETWORK_TIME_KIND_MESSAGE);
    }

  tel_hourly(tel);
}

/****************************************************************************
 * Name: tel_begin
 *
 * Description:
 *   The description and the settings are in: the modem's device opened,
 *   and the modem brought to what the settings say.
 *
 ****************************************************************************/

static void tel_begin(FAR struct telephony_s *tel)
{
  FAR struct pnut_loop_s *loop = tel->module.loop;
  int i;
  int ret;

  if (tel->step != STEP_WAITING || !tel->described ||
      (!tel->settings_read && tel->wait != NULL))
    {
      return;
    }

  if (tel->wait != NULL)
    {
      pnut_timer_cancel(loop, tel->wait);
      tel->wait = NULL;
    }

  tel->enabled = tel->settings.values.enabled;
  tel->gsm     = tel->settings.values.gsm;

  if (!tel->modem)
    {
      pnut_log(&tel->module, PNUT_LOG_INFO,
               "The device's description has no modem");
      tel_reason(tel, PNUT_TELEPHONY_INFO_REASON_NO_MODEM);
      tel_step(tel, STEP_OFF);
      return;
    }

  tel->uart = tel->config.open_uart(tel->config.arg, tel->uart_path);
  if (tel->uart < 0)
    {
      pnut_log(&tel->module, PNUT_LOG_ERROR, "Cannot open %s: %d",
               tel->uart_path, tel->uart);
      tel->uart = -1;
      tel_reason(tel, PNUT_TELEPHONY_INFO_REASON_NO_MODEM);
      tel_step(tel, STEP_FAILED);
      return;
    }

  ret = pnut_loop_watch(loop, tel->uart, EPOLLIN, tel_uart, tel);
  if (ret < 0)
    {
      pnut_log(&tel->module, PNUT_LOG_ERROR, "Cannot watch %s: %d",
               tel->uart_path, ret);
      tel_step(tel, STEP_FAILED);
      return;
    }

  for (i = 0; i < TELEPHONY_LINES; i++)
    {
      if (tel->line_path[i][0] != '\0' &&
          (i == TELEPHONY_POWER || i == TELEPHONY_KEY))
        {
          tel->line[i] = tel->config.open_line(tel->config.arg,
                                               tel->line_path[i]);
          if (tel->line[i] < 0)
            {
              pnut_log(&tel->module, PNUT_LOG_ERROR, "Cannot open %s: %d",
                       tel->line_path[i], tel->line[i]);
              tel->line[i] = -1;
            }
        }
    }

  if (tel->line[TELEPHONY_KEY] < 0)
    {
      pnut_log(&tel->module, PNUT_LOG_ERROR,
               "No power key for the modem: left as it is");
      tel_step(tel, STEP_FAILED);
      return;
    }

  /* The board leaves the rail off at start, unless the modem was running
   * before a reset of the chip: then it may still be on
   */

  if (tel_rail(tel))
    {
      tel_probe(tel);
    }
  else if (tel->enabled)
    {
      tel_power_up(tel);
    }
  else
    {
      tel_reason(tel, PNUT_TELEPHONY_INFO_REASON_DISABLED);
      tel_step(tel, STEP_OFF);
    }
}

/****************************************************************************
 * Name: tel_settings_wait
 *
 * Description:
 *   Settings did not answer in time: the defaults, until it does.
 *
 ****************************************************************************/

static void tel_settings_wait(FAR struct pnut_loop_s *loop,
                              FAR struct pnut_timer_s *timer, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;

  tel->wait = NULL;
  if (!tel->settings_read)
    {
      pnut_log(&tel->module, PNUT_LOG_WARNING,
               "No settings yet: the defaults meanwhile");
    }

  tel_begin(tel);
}

/****************************************************************************
 * Name: tel_settings
 *
 * Description:
 *   The settings were read, or one changed: the modem follows them.
 *
 ****************************************************************************/

static void tel_settings(FAR struct pnut_settings_cache_s *cache, int index,
                         FAR void *arg)
{
  FAR struct telephony_s *tel = arg;
  bool enabled = tel->settings.values.enabled;
  bool gsm = tel->settings.values.gsm;

  if (!tel->settings_read)
    {
      tel->settings_read = true;
      if (tel->step == STEP_WAITING)
        {
          tel_begin(tel);
          return;
        }
    }

  if (tel->step == STEP_WAITING || tel->finishing)
    {
      return;
    }

  if (enabled != tel->enabled)
    {
      tel->enabled = enabled;
      pnut_log(&tel->module, PNUT_LOG_INFO, "Telephony %s",
               enabled ? "enabled" : "disabled");
      if (!enabled)
        {
          tel->state.reason = PNUT_TELEPHONY_INFO_REASON_DISABLED;
          tel_power_down(tel, AFTER_OFF);
        }
      else if (tel->step == STEP_OFF || tel->step == STEP_FAILED)
        {
          tel->nrestarts = 0;
          if (tel->uart < 0)
            {
              return;
            }

          tel_power_up(tel);
        }
      else if (tel->step >= STEP_SHUTDOWN)
        {
          tel->after = AFTER_RESTART;     /* Up again once it is down */
        }
    }

  if (gsm != tel->gsm)
    {
      tel->gsm = gsm;
      if (tel->step == STEP_ON)
        {
          pnut_log(&tel->module, PNUT_LOG_INFO, "GSM %s",
                   gsm ? "allowed" : "off: LTE only");
          tel_send(tel, gsm ? "AT+CNMP=2" : "AT+CNMP=38", NULL, 0,
                   tel_mode_set);
          if (gsm && tel->state.reason ==
              PNUT_TELEPHONY_INFO_REASON_GSM_TURNED_OFF)
            {
              tel_reason(tel, PNUT_TELEPHONY_INFO_REASON_NO_REASON);
            }

          tel_trial(tel);
        }
    }
}

/****************************************************************************
 * Name: tel_airplane
 ****************************************************************************/

static void tel_airplane(FAR struct pnut_airplane_mode_reader_s *reader,
                         FAR const pnut_airplane_mode_t *msg, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;

  if (msg->on == tel->airplane)
    {
      return;
    }

  tel->airplane = msg->on;
  tel_air(tel);
}

/****************************************************************************
 * Name: tel_described
 *
 * Description:
 *   Board's description: the modem's part, its UART, its lines and its
 *   bands.  Board is asked once: the description does not change.
 *
 ****************************************************************************/

static void tel_board(FAR struct pnut_board_client_s *client,
                      bool connected, FAR void *arg);

static void tel_described(FAR struct pnut_board_client_s *client,
                          int status,
                          FAR const pnut_board_description_t *out,
                          FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;
  FAR const pnut_board_part_t *part = NULL;
  FAR const pnut_board_modem_t *modem;
  int role;
  pb_size_t i;
  pb_size_t j;

  if (status == PNUT_STATUS_TIMEOUT ||
      (status == PNUT_STATUS_UNAVAILABLE && err == NULL))
    {
      pnut_log(&tel->module, PNUT_LOG_WARNING,
               "No answer from Board: status %d; asking again", status);
      if (status == PNUT_STATUS_TIMEOUT)
        {
          tel_board(client, true, tel);
        }

      return;
    }

  pnut_board_disconnect(&tel->board);
  tel->board_open = false;
  tel->described  = true;

  if (status == PNUT_STATUS_OK && out != NULL)
    {
      for (i = 0; i < out->parts_count; i++)
        {
          if (out->parts[i].kind == PNUT_BOARD_PART_KIND_MODEM &&
              out->parts[i].which_detail == PNUT_BOARD_PART_MODEM_TAG &&
              out->parts[i].device[0] != '\0')
            {
              part = &out->parts[i];
              break;
            }
        }
    }

  if (part == NULL)
    {
      tel->modem = false;
      tel_begin(tel);
      return;
    }

  tel->modem = true;
  strlcpy(tel->uart_path, part->device, sizeof(tel->uart_path));

  for (i = 0; i < out->lines_count; i++)
    {
      if (strcmp(out->lines[i].part, part->name) != 0)
        {
          continue;
        }

      role = out->lines[i].role;
      j = role == PNUT_BOARD_LINE_ROLE_POWER ? TELEPHONY_POWER :
          role == PNUT_BOARD_LINE_ROLE_POWER_KEY ? TELEPHONY_KEY :
          role == PNUT_BOARD_LINE_ROLE_RING ? TELEPHONY_RING :
          role == PNUT_BOARD_LINE_ROLE_DTR ? TELEPHONY_DTR :
          TELEPHONY_LINES;
      if (j < TELEPHONY_LINES)
        {
          strlcpy(tel->line_path[j], out->lines[i].device,
                  sizeof(tel->line_path[j]));
        }
    }

  /* AT+CNBP's masks: LTE band n is bit n - 1, as in the description;
   * GSM's bands at SIMCom's bits
   */

  modem = &part->detail.modem;
  tel->lte_bands = 0;
  for (i = 0; i < modem->lte_bands.size && i < 8; i++)
    {
      tel->lte_bands |= (uint64_t)modem->lte_bands.bytes[i] << (8 * i);
    }

  tel->gsm_bands = 0;
  for (i = 0; i < modem->gsm_bands_count; i++)
    {
      switch (modem->gsm_bands[i])
        {
          case PNUT_BOARD_MODEM_GSM_GSM_850:
            tel->gsm_bands |= 1ull << 19;
            break;

          case PNUT_BOARD_MODEM_GSM_GSM_900:
            tel->gsm_bands |= 1ull << 8;
            break;

          case PNUT_BOARD_MODEM_GSM_DCS_1800:
            tel->gsm_bands |= 1ull << 7;
            break;

          case PNUT_BOARD_MODEM_GSM_PCS_1900:
            tel->gsm_bands |= 1ull << 21;
            break;

          default:
            break;
        }
    }

  pnut_log(&tel->module, PNUT_LOG_INFO, "The modem: %s on %s",
           part->chip[0] != '\0' ? part->chip : part->name, tel->uart_path);
  tel_begin(tel);
}

static void tel_board(FAR struct pnut_board_client_s *client,
                      bool connected, FAR void *arg)
{
  FAR struct telephony_s *tel = arg;
  pnut_board_describe_request_t req;
  int ret;

  if (!connected)
    {
      return;
    }

  memset(&req, 0, sizeof(req));
  ret = pnut_board_describe(client, &req, 0, tel_described, tel);
  if (ret < 0)
    {
      pnut_log(&tel->module, PNUT_LOG_WARNING, "Cannot ask Board: %d",
               ret);
    }
}

/****************************************************************************
 * Name: tel_fail
 *
 * Description:
 *   Answer a request with an error.
 *
 ****************************************************************************/

static void tel_fail(FAR struct telephony_s *tel,
                     FAR const struct pnut_request_s *req, int status,
                     uint32_t code, FAR const char *text)
{
  pnut_telephony_fail(&tel->server, req, status, code, text);
}

/****************************************************************************
 * Name: tel_pin_done
 *
 * Description:
 *   The SIM took the PIN, or the PUK, or refused it.
 *
 ****************************************************************************/

static void tel_pin_done(FAR void *arg, int result, FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;
  pnut_telephony_reply_t out;

  if (!tel->pin_pending)
    {
      return;
    }

  tel->pin_pending = false;
  tel_answered(tel, result);

  if (result == OK)
    {
      memset(&out, 0, sizeof(out));
      pnut_telephony_enter_pin_reply(&tel->server, &tel->pin_req,
                                     PNUT_STATUS_OK, &out);
    }
  else if (result == -EIO)
    {
      tel_fail(tel, &tel->pin_req, PNUT_STATUS_INVALID,
               PNUT_TELEPHONY_ERROR_CODE_REFUSED, "the SIM refused it");
    }
  else
    {
      tel_fail(tel, &tel->pin_req, result == -ETIMEDOUT ?
               PNUT_STATUS_TIMEOUT : PNUT_STATUS_UNAVAILABLE,
               PNUT_TELEPHONY_ERROR_CODE_NO_MODEM, "no answer");
    }

  if (tel->step == STEP_ON && result != -ECANCELED)
    {
      tel_send(tel, "AT+CPIN?", "+CPIN:", 0, tel_sim_read);
    }
}

/****************************************************************************
 * Name: tel_good_code
 *
 * Description:
 *   A PIN or a PUK: digits only, 4 to 8 of them.
 *
 ****************************************************************************/

static bool tel_good_code(FAR const char *code)
{
  size_t n = strlen(code);
  size_t i;

  if (n < 4 || n > 8)
    {
      return false;
    }

  for (i = 0; i < n; i++)
    {
      if (!isdigit((unsigned char)code[i]))
        {
          return false;
        }
    }

  return true;
}

/****************************************************************************
 * Name: tel_do_pin
 *
 * Description:
 *   Hand the SIM its PIN, or its PUK with a new PIN.  Nothing logs them,
 *   and nothing keeps them (RFC 0026, RFC 0030).
 *
 ****************************************************************************/

static void tel_do_pin(FAR struct telephony_s *tel,
                       FAR const struct pnut_request_s *req,
                       FAR const char *pin, FAR const char *puk)
{
  uint32_t need = puk != NULL ? PNUT_TELEPHONY_INFO_SIM_NEEDS_PUK :
                  PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN;
  char cmd[40];
  int ret;

  if (tel->step != STEP_ON)
    {
      tel_fail(tel, req, PNUT_STATUS_UNAVAILABLE,
               PNUT_TELEPHONY_ERROR_CODE_NO_MODEM, "the modem is not on");
      return;
    }

  if (tel->state.sim != need)
    {
      tel_fail(tel, req, PNUT_STATUS_INVALID,
               PNUT_TELEPHONY_ERROR_CODE_NOT_NEEDED,
               puk != NULL ? "no PUK is asked for" : "no PIN is asked for");
      return;
    }

  if (!tel_good_code(pin) || (puk != NULL && !tel_good_code(puk)))
    {
      tel_fail(tel, req, PNUT_STATUS_INVALID,
               PNUT_TELEPHONY_ERROR_CODE_NONE, "4 to 8 digits");
      return;
    }

  if (tel->pin_pending)
    {
      tel_fail(tel, req, PNUT_STATUS_BUSY, PNUT_TELEPHONY_ERROR_CODE_BUSY,
               "a PIN is on its way");
      return;
    }

  if (puk != NULL)
    {
      snprintf(cmd, sizeof(cmd), "AT+CPIN=\"%s\",\"%s\"", puk, pin);
    }
  else
    {
      snprintf(cmd, sizeof(cmd), "AT+CPIN=\"%s\"", pin);
    }

  ret = at_command(&tel->at, cmd, NULL, TEL_PIN_MS, AT_SECRET, tel_pin_done,
                   tel);
  memset(cmd, 0, sizeof(cmd));
  if (ret < 0)
    {
      tel_fail(tel, req, PNUT_STATUS_BUSY, PNUT_TELEPHONY_ERROR_CODE_BUSY,
               "too many commands wait");
      return;
    }

  tel->pin_req     = *req;
  tel->pin_pending = true;
}

/****************************************************************************
 * Name: tel_prefix
 *
 * Description:
 *   The prefix of a command's answer: "+CSQ:" for AT+CSQ, so that its
 *   lines are kept even where they look like ones sent unasked.
 *
 ****************************************************************************/

static void tel_prefix(FAR const char *cmd, FAR char *prefix, size_t size)
{
  size_t n;

  prefix[0] = '\0';
  if ((cmd[2] != '+' && cmd[2] != '*' && cmd[2] != '^') || size < 3)
    {
      return;
    }

  n = strcspn(cmd + 2, "=?");
  if (n + 2 > size)
    {
      return;
    }

  memcpy(prefix, cmd + 2, n);
  prefix[n]     = ':';
  prefix[n + 1] = '\0';
}

/****************************************************************************
 * Name: tel_holds
 *
 * Description:
 *   Whether a command holds a word, in either case.
 *
 ****************************************************************************/

static bool tel_holds(FAR const char *cmd, FAR const char *word)
{
  size_t n = strlen(word);

  for (; *cmd != '\0'; cmd++)
    {
      if (strncasecmp(cmd, word, n) == 0)
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Name: tel_command_done
 ****************************************************************************/

static void tel_command_done(FAR void *arg, int result,
                             FAR const char *reply)
{
  FAR struct telephony_s *tel = arg;
  FAR pnut_telephony_command_reply_t *out;

  if (!tel->command_pending)
    {
      return;
    }

  tel->command_pending = false;
  tel_answered(tel, result);

  if (result != OK && result != -EIO)
    {
      tel_fail(tel, &tel->command_req, result == -ETIMEDOUT ?
               PNUT_STATUS_TIMEOUT : PNUT_STATUS_UNAVAILABLE,
               PNUT_TELEPHONY_ERROR_CODE_NO_MODEM, "no answer");
      return;
    }

  out = calloc(1, sizeof(*out));
  if (out == NULL)
    {
      tel_fail(tel, &tel->command_req, PNUT_STATUS_INTERNAL,
               PNUT_TELEPHONY_ERROR_CODE_NONE, "no memory");
      return;
    }

  out->ok = result == OK;
  strlcpy(out->reply, reply, sizeof(out->reply));
  pnut_telephony_command_reply(&tel->server, &tel->command_req,
                               PNUT_STATUS_OK, out);
  free(out);
}

/****************************************************************************
 * Name: tel_do_command
 *
 * Description:
 *   One AT command passed through, for diagnosis: the system interface.
 *
 ****************************************************************************/

static void tel_do_command(FAR struct telephony_s *tel,
                           FAR const struct pnut_request_s *req,
                           FAR const pnut_telephony_command_request_t *in)
{
  char cmd[AT_COMMAND_MAX + 3];
  char prefix[AT_PREFIX_MAX];
  uint32_t timeout;
  bool secret;
  size_t i;
  int ret;

  if (!tel_talking(tel) || tel->step < STEP_SETUP)
    {
      tel_fail(tel, req, PNUT_STATUS_UNAVAILABLE,
               PNUT_TELEPHONY_ERROR_CODE_NO_MODEM, "the modem is not on");
      return;
    }

  /* The "AT" may be left out; a line's end, or another control
   * character, may not be in
   */

  if (strncasecmp(in->command, "AT", 2) == 0)
    {
      strlcpy(cmd, in->command, sizeof(cmd));
    }
  else
    {
      snprintf(cmd, sizeof(cmd), "AT%s", in->command);
    }

  if (strlen(cmd) > AT_COMMAND_MAX)
    {
      tel_fail(tel, req, PNUT_STATUS_INVALID,
               PNUT_TELEPHONY_ERROR_CODE_NONE, "too long");
      return;
    }

  for (i = 0; cmd[i] != '\0'; i++)
    {
      if (iscntrl((unsigned char)cmd[i]))
        {
          tel_fail(tel, req, PNUT_STATUS_INVALID,
                   PNUT_TELEPHONY_ERROR_CODE_NONE, "a control character");
          return;
        }
    }

  if (tel->command_pending)
    {
      tel_fail(tel, req, PNUT_STATUS_BUSY, PNUT_TELEPHONY_ERROR_CODE_BUSY,
               "a command is on its way");
      return;
    }

  timeout = in->timeout > 0 ? in->timeout : TEL_COMMAND_MS;
  if (timeout > TEL_COMMAND_MAX)
    {
      timeout = TEL_COMMAND_MAX;
    }

  /* A command that carries a PIN is never logged */

  secret = tel_holds(cmd, "+CPIN=") || tel_holds(cmd, "+CPWD=") ||
           tel_holds(cmd, "+CLCK=");

  tel_prefix(cmd, prefix, sizeof(prefix));
  ret = at_command(&tel->at, cmd, prefix[0] != '\0' ? prefix : NULL,
                   timeout, secret ? AT_SECRET : 0, tel_command_done, tel);
  if (ret < 0)
    {
      tel_fail(tel, req, PNUT_STATUS_BUSY, PNUT_TELEPHONY_ERROR_CODE_BUSY,
               "too many commands wait");
      return;
    }

  pnut_log(&tel->module, PNUT_LOG_INFO, "A command passed through: %s",
           secret ? "(secret)" : cmd);
  memset(cmd, 0, sizeof(cmd));
  tel->command_req     = *req;
  tel->command_pending = true;
}

/****************************************************************************
 * Name: tel_who
 *
 * Description:
 *   Service states said who sent a request that only the system UI and
 *   the developer's shell may send.
 *
 ****************************************************************************/

static void tel_who(FAR struct pnut_services_client_s *client, int status,
                    FAR const pnut_services_who_reply_t *out,
                    FAR const pnut_error_t *err, FAR void *arg)
{
  FAR struct telephony_ask_s *ask = arg;
  FAR struct telephony_s *tel = ask->tel;

  ask->used = false;

  if (status != PNUT_STATUS_OK || out == NULL ||
      (strcmp(out->name, TEL_UI) != 0 && !out->shell))
    {
      tel_fail(tel, &ask->req, status == PNUT_STATUS_UNAVAILABLE ||
               status == PNUT_STATUS_TIMEOUT ? PNUT_STATUS_UNAVAILABLE :
               PNUT_STATUS_DENIED, PNUT_TELEPHONY_ERROR_CODE_DENIED,
               "for the system UI and the developer's shell");
    }
  else if (ask->method == PNUT_TELEPHONY_ENTER_PIN)
    {
      tel_do_pin(tel, &ask->req, ask->in.pin.pin, NULL);
    }
  else if (ask->method == PNUT_TELEPHONY_ENTER_PUK)
    {
      tel_do_pin(tel, &ask->req, ask->in.puk.pin, ask->in.puk.puk);
    }
  else
    {
      tel_do_command(tel, &ask->req, &ask->in.command);
    }

  memset(&ask->in, 0, sizeof(ask->in));
}

/****************************************************************************
 * Name: tel_ask
 *
 * Description:
 *   Ask Service states who sent a request, keeping it meanwhile.
 *
 ****************************************************************************/

static void tel_ask(FAR struct telephony_s *tel,
                    FAR const struct pnut_request_s *req, uint16_t method,
                    FAR const void *in, size_t size)
{
  FAR struct telephony_ask_s *ask = NULL;
  pnut_services_who_request_t who;
  int i;
  int ret;

  for (i = 0; i < TELEPHONY_ASKS; i++)
    {
      if (!tel->asks[i].used)
        {
          ask = &tel->asks[i];
          break;
        }
    }

  if (ask == NULL)
    {
      tel_fail(tel, req, PNUT_STATUS_BUSY, PNUT_TELEPHONY_ERROR_CODE_BUSY,
               "too many requests wait");
      return;
    }

  ask->tel    = tel;
  ask->req    = *req;
  ask->method = method;
  memcpy(&ask->in, in, size);

  memset(&who, 0, sizeof(who));
  who.pid = req->pid;
  ret = tel->services_open ?
        pnut_services_who(&tel->services, &who, 0, tel_who, ask) :
        -ENOTCONN;
  if (ret < 0)
    {
      memset(&ask->in, 0, sizeof(ask->in));
      tel_fail(tel, req, PNUT_STATUS_UNAVAILABLE,
               PNUT_TELEPHONY_ERROR_CODE_DENIED,
               "who calls cannot be told now");
      return;
    }

  ask->used = true;
}

/****************************************************************************
 * Name: tel_status, tel_enter_pin, tel_enter_puk, tel_command
 ****************************************************************************/

static void tel_status(FAR struct pnut_telephony_server_s *server,
                       FAR const struct pnut_request_s *req,
                       FAR const pnut_telephony_status_request_t *in,
                       FAR void *arg)
{
  FAR struct telephony_s *tel = arg;

  pnut_telephony_status_reply(server, req, PNUT_STATUS_OK, &tel->state);

  /* An old state is read again: the topic tells of what changed */

  if (tel->step == STEP_ON && tel_now() - tel->refreshed > TEL_FRESH_MS)
    {
      tel_refresh(tel);
      tel_hourly(tel);
    }
}

static void tel_enter_pin(FAR struct pnut_telephony_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_telephony_pin_request_t *in,
                          FAR void *arg)
{
  tel_ask(arg, req, PNUT_TELEPHONY_ENTER_PIN, in, sizeof(*in));
}

static void tel_enter_puk(FAR struct pnut_telephony_server_s *server,
                          FAR const struct pnut_request_s *req,
                          FAR const pnut_telephony_puk_request_t *in,
                          FAR void *arg)
{
  tel_ask(arg, req, PNUT_TELEPHONY_ENTER_PUK, in, sizeof(*in));
}

static void tel_command(FAR struct pnut_telephony_server_s *server,
                        FAR const struct pnut_request_s *req,
                        FAR const pnut_telephony_command_request_t *in,
                        FAR void *arg)
{
  tel_ask(arg, req, PNUT_TELEPHONY_COMMAND, in, sizeof(*in));
}

/****************************************************************************
 * Name: tel_start
 ****************************************************************************/

static const struct mux_ops_s g_tel_mux_ops =
{
  .write = tel_uart_write,
  .data  = tel_mux_data,
  .event = tel_mux_event,
};

static const struct at_ops_s g_tel_at_ops =
{
  .send = tel_at_send,
  .urc  = tel_urc,
};

static int tel_start(FAR struct pnut_module_s *module,
                     FAR struct pnut_loop_s *loop)
{
  FAR struct telephony_s *tel = (FAR struct telephony_s *)module;
  int ret;

  mux_init(&tel->mux, &g_tel_mux_ops, tel);
  at_init(&tel->at, module, &g_tel_at_ops, tel);

  ret = pnut_telephony_info_advertise(loop, &tel->states);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No telephony topic: %d", ret);
      tel->states = NULL;
    }

  ret = pnut_network_time_advertise(loop, &tel->times);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No network_time topic: %d", ret);
      tel->times = NULL;
    }

  tel_publish(tel);

  ret = pnut_telephony_serve(loop, TELEPHONY_SERVICE, 0, &g_tel_handlers,
                             tel, &tel->server);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "Cannot serve: %d", ret);
      return ret;
    }

  tel->serving = true;

  /* The settings hold their defaults until Settings' values are read:
   * the modem waits for them a moment
   */

  ret = pnut_telephony_settings_open(module, tel_settings, tel,
                                     &tel->settings);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No settings: %d", ret);
      tel->settings_read = true;
    }
  else
    {
      tel->settings_open = true;
      if (pnut_timer_start(loop, tel_config(tel->config.wait,
                                            TEL_SETTINGS_MS),
                           0, tel_settings_wait, tel, &tel->wait) < 0)
        {
          tel->wait = NULL;
        }
    }

  ret = pnut_airplane_mode_subscribe(loop, tel_airplane, tel,
                                     &tel->airplanes);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_WARNING, "No airplane_mode topic: %d",
               ret);
    }
  else
    {
      tel->airplanes_open = true;
    }

  if (tel->config.services != NULL)
    {
      ret = pnut_services_connect(loop, tel->config.services, NULL, NULL,
                                  &tel->services);
      if (ret < 0)
        {
          pnut_log(module, PNUT_LOG_WARNING,
                   "No Service states: %d; PINs refused", ret);
        }
      else
        {
          tel->services_open = true;
        }
    }

  ret = pnut_board_connect(loop, tel->config.board, tel_board, tel,
                           &tel->board);
  if (ret < 0)
    {
      pnut_log(module, PNUT_LOG_ERROR, "No Board: %d; no modem", ret);
    }
  else
    {
      tel->board_open = true;
    }

  pnut_module_ready(module);
  return OK;
}

/****************************************************************************
 * Name: tel_finish
 *
 * Description:
 *   Asked to stop: the modem powered down in order first (RFC 0030).
 *
 ****************************************************************************/

static int tel_finish(FAR struct pnut_module_s *module,
                      FAR struct pnut_loop_s *loop)
{
  FAR struct telephony_s *tel = (FAR struct telephony_s *)module;

  tel->finishing = true;

  if ((tel->step == STEP_WAITING || tel->step == STEP_OFF ||
       tel->step == STEP_FAILED) && !(tel->uart >= 0 && tel_rail(tel)))
    {
      return OK;
    }

  if (tel->step >= STEP_SHUTDOWN)
    {
      tel->after = AFTER_OFF;
      return -EINPROGRESS;
    }

  tel_power_down(tel, AFTER_OFF);
  return tel->step == STEP_OFF ? OK : -EINPROGRESS;
}

/****************************************************************************
 * Name: tel_stop
 ****************************************************************************/

static void tel_stop(FAR struct pnut_module_s *module,
                     FAR struct pnut_loop_s *loop)
{
  FAR struct telephony_s *tel = (FAR struct telephony_s *)module;
  int i;

  tel_untime(tel);
  at_cancel(&tel->at, -ECANCELED);

  if (tel->trial != NULL)
    {
      pnut_timer_cancel(loop, tel->trial);
      tel->trial = NULL;
    }

  if (tel->wait != NULL)
    {
      pnut_timer_cancel(loop, tel->wait);
      tel->wait = NULL;
    }

  if (tel->uart >= 0)
    {
      pnut_loop_unwatch(loop, tel->uart);
      close(tel->uart);
      tel->uart = -1;
    }

  for (i = 0; i < TELEPHONY_LINES; i++)
    {
      if (tel->line[i] >= 0)
        {
          close(tel->line[i]);
          tel->line[i] = -1;
        }
    }

  if (tel->board_open)
    {
      pnut_board_disconnect(&tel->board);
      tel->board_open = false;
    }

  if (tel->services_open)
    {
      pnut_services_disconnect(&tel->services);
      tel->services_open = false;
    }

  if (tel->airplanes_open)
    {
      pnut_airplane_mode_unsubscribe(&tel->airplanes);
      tel->airplanes_open = false;
    }

  if (tel->settings_open)
    {
      pnut_telephony_settings_close(&tel->settings);
      tel->settings_open = false;
    }

  if (tel->serving)
    {
      pnut_telephony_close(&tel->server);
      tel->serving = false;
    }

  pnut_topic_unadvertise(tel->times);
  tel->times = NULL;
  pnut_topic_unadvertise(tel->states);
  tel->states = NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void telephony_init(FAR struct telephony_s *tel,
                    FAR const struct telephony_config_s *config)
{
  int i;

  memset(tel, 0, sizeof(*tel));
  tel->config        = *config;
  tel->module.name   = "telephony";
  tel->module.start  = tel_start;
  tel->module.stop   = tel_stop;
  tel->module.finish = tel_finish;
  tel->uart          = -1;
  for (i = 0; i < TELEPHONY_LINES; i++)
    {
      tel->line[i] = -1;
    }
}
