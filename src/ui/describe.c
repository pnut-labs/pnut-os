/****************************************************************************
 * pnut-os/src/ui/describe.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* What the system UI says of the device's state, in a line or a word: the
 * text Settings shows under Wi-Fi and Cellular, the header's meters, the
 * date.  The words are text.h's.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include "describe.h"
#include "text.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* A network's strength: strong from -60 dBm, fair from -75 */

#define DESCRIBE_STRONG  (-60)
#define DESCRIBE_FAIR    (-75)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static size_t describe_put(FAR char *buf, size_t size, FAR const char *a,
                           FAR const char *b, FAR const char *c)
{
  int n = snprintf(buf, size, "%s%s%s", a, b != NULL ? b : "",
                   c != NULL ? c : "");

  return n < 0 ? 0 : (size_t)n < size ? (size_t)n : size - 1;
}

/* The nth of a text's space-separated words (a translation's may be of
 * any length, in any script)
 */

static void describe_word(FAR const char *words, int n, FAR char *out,
                          size_t size)
{
  size_t len;

  while (n-- > 0 && (words = strchr(words, ' ')) != NULL)
    {
      words++;
    }

  out[0] = '\0';
  if (words != NULL)
    {
      len = strcspn(words, " ");
      len = len < size - 1 ? len : size - 1;
      memcpy(out, words, len);
      out[len] = '\0';
    }
}

static FAR const char *describe_data(pnut_telephony_info_data_t data)
{
  switch (data)
    {
      case PNUT_TELEPHONY_INFO_DATA_DATA_ON:
        return UI_T_DATA_ON;

      case PNUT_TELEPHONY_INFO_DATA_CONNECTING:
        return UI_T_DATA_CONNECTING;

      case PNUT_TELEPHONY_INFO_DATA_DATA_FAILED:
        return UI_T_DATA_FAILED;

      case PNUT_TELEPHONY_INFO_DATA_DATA_STANDBY:
        return UI_T_DATA_STANDBY;

      default:
        return UI_T_DATA_OFF;
    }
}

static FAR const char *describe_sim(pnut_telephony_info_sim_t sim)
{
  switch (sim)
    {
      case PNUT_TELEPHONY_INFO_SIM_ABSENT:
        return UI_T_NO_SIM;

      case PNUT_TELEPHONY_INFO_SIM_NEEDS_PIN:
        return UI_T_SIM_LOCKED;

      case PNUT_TELEPHONY_INFO_SIM_NEEDS_PUK:
        return UI_T_SIM_PUK;

      case PNUT_TELEPHONY_INFO_SIM_BLOCKED:
        return UI_T_SIM_BLOCKED;

      case PNUT_TELEPHONY_INFO_SIM_SIM_FAILED:
        return UI_T_SIM_FAILED;

      case PNUT_TELEPHONY_INFO_SIM_SIM_UNKNOWN:
        return UI_T_STARTING;

      default:
        return NULL;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

size_t describe_wifi(FAR const pnut_wifi_info_t *w, FAR char *buf,
                     size_t size)
{
  switch (w->state)
    {
      case PNUT_WIFI_INFO_STATE_ON:
        return describe_put(buf, size, UI_T_ON, NULL, NULL);

      case PNUT_WIFI_INFO_STATE_JOINING:
        return describe_put(buf, size, UI_T_JOINING,
                            w->name[0] != '\0' ? " " : NULL, w->name);

      case PNUT_WIFI_INFO_STATE_JOINED:
        return describe_put(buf, size, UI_T_ON, UI_DOT, w->name);

      default:
        return describe_put(buf, size, UI_T_OFF, NULL, NULL);
    }
}

size_t describe_cell(FAR const pnut_telephony_info_t *t, FAR char *buf,
                     size_t size)
{
  FAR const char *line;
  FAR const char *tech;

  switch (t->modem)
    {
      case PNUT_TELEPHONY_INFO_MODEM_ON:
        break;

      case PNUT_TELEPHONY_INFO_MODEM_STARTING:
        return describe_put(buf, size, UI_T_STARTING, NULL, NULL);

      case PNUT_TELEPHONY_INFO_MODEM_FAILED:
        return describe_put(buf, size, UI_T_MODEM_FAILED, NULL, NULL);

      default:
        line = t->reason == PNUT_TELEPHONY_INFO_REASON_NO_MODEM ?
               UI_T_NO_MODEM :
               t->reason == PNUT_TELEPHONY_INFO_REASON_NO_SIM ?
               UI_T_NO_SIM : UI_T_MODEM_OFF;
        return describe_put(buf, size, line, NULL, NULL);
    }

  line = describe_sim(t->sim);
  if (line != NULL)
    {
      return describe_put(buf, size, line, NULL, NULL);
    }

  switch (t->registration)
    {
      case PNUT_TELEPHONY_INFO_REGISTRATION_HOME:
      case PNUT_TELEPHONY_INFO_REGISTRATION_ROAMING:
        break;

      case PNUT_TELEPHONY_INFO_REGISTRATION_SEARCHING:
        return describe_put(buf, size, UI_T_SEARCHING, NULL, NULL);

      case PNUT_TELEPHONY_INFO_REGISTRATION_DENIED:
        return describe_put(buf, size, UI_T_REFUSED, NULL, NULL);

      default:
        return describe_put(buf, size, UI_T_NO_NETWORK, NULL, NULL);
    }

  tech = t->tech == PNUT_TELEPHONY_INFO_TECH_LTE ? UI_T_LTE :
         t->tech == PNUT_TELEPHONY_INFO_TECH_GSM ? UI_T_GSM : UI_T_NETWORK;
  return describe_put(buf, size, tech, UI_DOT, describe_data(t->data));
}

FAR const char *describe_strength(int32_t dbm)
{
  return dbm >= DESCRIBE_STRONG ? UI_T_STRONG :
         dbm >= DESCRIBE_FAIR ? UI_T_FAIR : UI_T_WEAK;
}

uint8_t describe_signal_steps(uint32_t bars)
{
  return bars >= 4 ? DESCRIBE_STEPS : (uint8_t)((bars * 5 + 2) / 4);
}

uint8_t describe_battery_steps(uint32_t level)
{
  return level >= 100 ? DESCRIBE_STEPS : (uint8_t)((level + 19) / 20);
}

size_t describe_date(FAR const struct tm *tm, FAR char *buf, size_t size)
{
  char day[16];
  char month[16];
  int n;

  describe_word(UI_T_WEEKDAYS, tm->tm_wday, day, sizeof(day));
  describe_word(UI_T_MONTHS, tm->tm_mon, month, sizeof(month));
  n = snprintf(buf, size, "%s, %d %s", day, tm->tm_mday, month);
  return n < 0 ? 0 : (size_t)n < size ? (size_t)n : size - 1;
}

size_t describe_clock(FAR const struct tm *tm, FAR char *buf, size_t size)
{
  int n = snprintf(buf, size, "%02d:%02d", tm->tm_hour, tm->tm_min);

  return n < 0 ? 0 : (size_t)n < size ? (size_t)n : size - 1;
}
