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

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <pnut/msg.h>

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

/* A UTF-8 sequence's length, if it is one: the shortest form of a code
 * point up to U+10FFFF, no surrogate; 0 if not
 */

static int describe_utf8(FAR const uint8_t *p)
{
  int len;
  int i;

  len = p[0] < 0x80 ? 1 : p[0] >= 0xc2 && p[0] <= 0xdf ? 2 :
        (p[0] & 0xf0) == 0xe0 ? 3 : p[0] >= 0xf0 && p[0] <= 0xf4 ? 4 : 0;
  for (i = 1; i < len; i++)
    {
      if ((p[i] & 0xc0) != 0x80)
        {
          return 0;
        }
    }

  if ((len == 3 && p[0] == 0xe0 && p[1] < 0xa0) ||   /* Overlong */
      (len == 3 && p[0] == 0xed && p[1] >= 0xa0) ||  /* Surrogate */
      (len == 4 && p[0] == 0xf0 && p[1] < 0x90) ||   /* Overlong */
      (len == 4 && p[0] == 0xf4 && p[1] >= 0x90))    /* Over U+10FFFF */
    {
      return 0;
    }

  return len;
}

static uint32_t describe_point(FAR const uint8_t *p, int len)
{
  uint32_t cp = len == 1 ? p[0] : len == 2 ? p[0] & 0x1f :
                len == 3 ? p[0] & 0x0f : p[0] & 0x07;
  int i;

  for (i = 1; i < len; i++)
    {
      cp = cp << 6 | (p[i] & 0x3f);
    }

  return cp;
}

/* Whether a name's character is drawn as it is: one the fonts have
 * (tools/mkfont.py's TEXT), visible (not U+00A0 nor U+00AD), and not the
 * backslash that \xNN starts with.  Anything else (controls, invisible
 * and direction-changing characters, letters the fonts lack and would draw
 * as nothing) could make one network's name look like another's.
 */

static bool describe_drawn(uint32_t cp)
{
  static const uint16_t more[] =
  {
    0x2013, 0x2014, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2026,
    0x2190, 0x2192
  };

  size_t i;

  if ((cp >= 0x20 && cp < 0x7f && cp != '\\') ||
      (cp > 0xa0 && cp <= 0x17f && cp != 0xad))
    {
      return true;
    }

  for (i = 0; i < sizeof(more) / sizeof(more[0]); i++)
    {
      if (cp == more[i])
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

size_t describe_wifi(FAR const pnut_wifi_info_t *w, FAR char *buf,
                     size_t size)
{
  char name[DESCRIBE_NAME_MAX];

  switch (w->state)
    {
      case PNUT_WIFI_INFO_STATE_ON:
        return describe_put(buf, size, UI_T_ON, NULL, NULL);

      case PNUT_WIFI_INFO_STATE_JOINING:
        describe_name(w->name, name, sizeof(name));
        return describe_put(buf, size, UI_T_JOINING,
                            name[0] != '\0' ? " " : NULL, name);

      case PNUT_WIFI_INFO_STATE_JOINED:
        describe_name(w->name, name, sizeof(name));
        return describe_put(buf, size, UI_T_ON, UI_DOT, name);

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

size_t describe_name(FAR const char *name, FAR char *buf, size_t size)
{
  FAR const uint8_t *p = (FAR const uint8_t *)name;
  size_t o = 0;
  int len;
  int i;

  while (*p != '\0')
    {
      len = describe_utf8(p);
      if (len > 0 && describe_drawn(describe_point(p, len)))
        {
          if (o + len + 1 > size)
            {
              break;
            }

          memcpy(buf + o, p, len);
          o += len;
          p += len;
          continue;
        }

      /* Not to be drawn as it is: its bytes, each as \xNN */

      len = len > 0 ? len : 1;
      if (o + 4 * len + 1 > size)
        {
          break;
        }

      for (i = 0; i < len; i++)
        {
          o += snprintf(buf + o, size - o, "\\x%02x", *p++);
        }
    }

  buf[o] = '\0';
  return o;
}

FAR const char *describe_wifi_failure(int status, uint32_t code)
{
  switch (code)
    {
      case PNUT_WIFI_ERROR_CODE_OFF:
        return UI_T_WIFI_OFF;

      case PNUT_WIFI_ERROR_CODE_NO_RADIO:
        return UI_T_WIFI_NO_RADIO;

      case PNUT_WIFI_ERROR_CODE_NOT_FOUND:
        return UI_T_WIFI_NOT_FOUND;

      case PNUT_WIFI_ERROR_CODE_REFUSED:
        return UI_T_WIFI_REFUSED;

      case PNUT_WIFI_ERROR_CODE_NO_ADDRESS:
        return UI_T_WIFI_NO_ADDRESS;

      case PNUT_WIFI_ERROR_CODE_CANCELLED:
        return UI_T_WIFI_CANCELLED;

      default:
        return status == PNUT_STATUS_TIMEOUT ||
               status == PNUT_STATUS_UNAVAILABLE ? UI_T_WIFI_NO_ANSWER :
                                                   UI_T_WIFI_FAILED;
    }
}
