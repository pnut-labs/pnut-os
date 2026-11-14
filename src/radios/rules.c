/****************************************************************************
 * pnut-os/src/radios/rules.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* The region's rules (RFC 0034): the band plans, a LoRa packet's time on
 * the air, and the air time counted per sub-band over the last hour.
 * Everything here rounds towards the law: air time up, the power limits
 * down.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <string.h>

#include "rules.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct radio_bw_s
{
  uint32_t hz;                    /* Rounded down */
  uint8_t code;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* EU868: ERC Recommendation 70-03, annex 1, the bands for non-specific
 * short range devices LoRa uses.  25 mW is 13.98 dBm, 500 mW 26.99 dBm,
 * both ERP.
 */

static const struct radio_band_s g_eu868[] =
{
  { 863000000, 865000000, 1397, 1 },      /* h1.3: 25 mW, 0.1 % */
  { 865000000, 868000000, 1397, 10 },     /* h1.4: 25 mW, 1 % */
  { 868000000, 868600000, 1397, 10 },     /* h1.5: 25 mW, 1 % */
  { 868700000, 869200000, 1397, 1 },      /* h1.6: 25 mW, 0.1 % */
  { 869400000, 869650000, 2698, 100 },    /* h1.7: 500 mW, 10 % */
  { 869700000, 870000000, 1397, 10 },     /* h1.8: 25 mW, 1 % */
};

static const struct radio_plan_s g_plans[] =
{
  {
    "EU868", 863000000, 870000000, g_eu868,
    sizeof(g_eu868) / sizeof(g_eu868[0])
  },
};

#define RADIO_NPLANS (sizeof(g_plans) / sizeof(g_plans[0]))

/* The SX126x's LoRa bandwidths and their codes */

static const struct radio_bw_s g_bandwidths[] =
{
  { 7812, 0x00 },
  { 10416, 0x08 },
  { 15625, 0x01 },
  { 20833, 0x09 },
  { 31250, 0x02 },
  { 41666, 0x0a },
  { 62500, 0x03 },
  { 125000, 0x04 },
  { 250000, 0x05 },
  { 500000, 0x06 }
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR const struct radio_plan_s *radio_plan_find(FAR const char *name)
{
  size_t i;

  for (i = 0; i < RADIO_NPLANS; i++)
    {
      if (strcmp(g_plans[i].name, name) == 0)
        {
          return &g_plans[i];
        }
    }

  return NULL;
}

/* A radio built for one region's band: the plan whose band holds the
 * radio's whole.  A radio of many regions' bands matches none, and needs
 * the region set.
 */

FAR const struct radio_plan_s *radio_plan_for(uint32_t lo_hz,
                                              uint32_t hi_hz)
{
  size_t i;

  for (i = 0; i < RADIO_NPLANS; i++)
    {
      if (lo_hz < hi_hz && g_plans[i].lo_hz <= lo_hz &&
          hi_hz <= g_plans[i].hi_hz)
        {
          return &g_plans[i];
        }
    }

  return NULL;
}

int radio_band_of(FAR const struct radio_plan_s *plan, uint32_t freq_hz,
                  uint32_t bw_hz)
{
  uint32_t half = bw_hz / 2;
  int i;

  if (plan == NULL || freq_hz < half)
    {
      return -1;
    }

  for (i = 0; i < plan->nbands; i++)
    {
      if (plan->bands[i].lo_hz <= freq_hz - half &&
          (uint64_t)freq_hz + half <= plan->bands[i].hi_hz)
        {
          return i;
        }
    }

  return -1;
}

bool radio_power_ok(FAR const struct radio_band_s *band, int power_dbm,
                    int gain_cdbi)
{
  return power_dbm * 100 + gain_cdbi - RADIO_DIPOLE_CDB <= band->erp_cdbm;
}

bool radio_bandwidth(uint32_t asked_hz, FAR uint32_t *bw_hz,
                     FAR uint8_t *code)
{
  uint32_t diff;
  size_t i;

  for (i = 0; i < sizeof(g_bandwidths) / sizeof(g_bandwidths[0]); i++)
    {
      diff = asked_hz > g_bandwidths[i].hz ?
             asked_hz - g_bandwidths[i].hz : g_bandwidths[i].hz - asked_hz;
      if ((uint64_t)diff * 100 <= g_bandwidths[i].hz)
        {
          *bw_hz = g_bandwidths[i].hz;
          *code  = g_bandwidths[i].code;
          return true;
        }
    }

  return false;
}

bool radio_ldro(uint8_t sf, uint32_t bw_hz)
{
  return bw_hz > 0 && ((uint64_t)1000000 << sf) >= (uint64_t)16000 * bw_hz;
}

/* In quarter symbols, for the preamble's 4.25 or 6.25: an explicit header
 * always, as pnut-os sends.  SF5 and SF6 count no 8 bits more and a
 * longer preamble; low data rate optimisation packs 2 bits fewer a
 * symbol.
 */

uint32_t radio_airtime_us(FAR const struct radio_lora_s *lora, size_t len)
{
  bool low = lora->sf <= 6;
  uint64_t quarters;
  uint64_t us;
  int32_t bits;
  uint32_t div;
  uint32_t blocks;

  bits = 8 * (int32_t)len + (lora->crc ? 16 : 0) - 4 * lora->sf +
         (low ? 0 : 8) + 20;
  div  = 4 * (lora->ldro ? lora->sf - 2 : lora->sf);
  blocks = bits > 0 ? ((uint32_t)bits + div - 1) / div : 0;

  quarters = 4 * (uint64_t)lora->preamble + (low ? 25 : 17) +
             4 * (8 + (uint64_t)blocks * lora->cr);

  us = (quarters * ((uint64_t)1000000 << lora->sf) +
        4 * (uint64_t)lora->bw_hz - 1) / (4 * (uint64_t)lora->bw_hz);
  return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

uint32_t radio_air_used(FAR const struct radio_air_s *air, uint32_t now)
{
  uint32_t used = 0;
  int i;

  for (i = 0; i < RADIO_MINUTES; i++)
    {
      if (air->ms[i] > 0 && air->minute[i] <= now &&
          now - air->minute[i] < RADIO_MINUTES)
        {
          used += air->ms[i];
        }
    }

  return used;
}

void radio_air_add(FAR struct radio_air_s *air, uint32_t now, uint32_t ms)
{
  int i = now % RADIO_MINUTES;

  if (air->minute[i] != now)
    {
      air->minute[i] = now;
      air->ms[i]     = 0;
    }

  air->ms[i] += ms;
}

/* A minute's sends leave the count RADIO_MINUTES minutes on */

uint32_t radio_air_wait(FAR const struct radio_air_s *air, uint32_t now,
                        uint32_t budget_ms, uint32_t ms)
{
  uint32_t k;

  if (ms > budget_ms)
    {
      return UINT32_MAX;
    }

  for (k = 0; k <= RADIO_MINUTES; k++)
    {
      if (radio_air_used(air, now + k) + ms <= budget_ms)
        {
          return k;
        }
    }

  return RADIO_MINUTES;
}
