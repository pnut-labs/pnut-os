/****************************************************************************
 * pnut-os/src/radios/rules.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_RADIOS_RULES_H
#define __PNUT_OS_RADIOS_RULES_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pnut/compiler.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RADIO_BANDS_MAX   8       /* Sub-bands in a plan, at most */
#define RADIO_MINUTES     61      /* The hour counted, in minutes: the
                                   * current one only partly gone */
#define RADIO_DIPOLE_CDB  215     /* A dipole's gain over isotropic: ERP is
                                   * EIRP less 2.15 dB */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A sub-band of a band plan: its edges, its power limit (ERP, hundredths
 * of a dBm) and the share of each hour a device may send in it (tenths of
 * a percent)
 */

struct radio_band_s
{
  uint32_t lo_hz;
  uint32_t hi_hz;
  int16_t erp_cdbm;
  uint16_t duty_permille;
};

/* A region's band plan */

struct radio_plan_s
{
  FAR const char *name;
  uint32_t lo_hz;                 /* Its whole band */
  uint32_t hi_hz;
  FAR const struct radio_band_s *bands;
  uint8_t nbands;
};

/* A LoRa configuration, checked: the bandwidth as the radio uses it, and
 * the driver's code for it
 */

struct radio_lora_s
{
  uint32_t freq_hz;
  uint32_t bw_hz;                 /* 7812 for 7.8125 kHz, and so on */
  uint8_t bw_code;                /* The SX126x's */
  uint8_t sf;                     /* 5 to 12 */
  uint8_t cr;                     /* 5 to 8: 4/5 to 4/8 */
  uint8_t sync;                   /* One byte: 0x12, 0x34 */
  uint16_t preamble;              /* Symbols */
  int8_t power_dbm;
  bool crc;
  bool invert_iq;
  bool ldro;                      /* Low data rate optimisation: on when a
                                   * symbol lasts 16 ms or more */
};

/* A sub-band's air time over the last hour: what was sent in each minute
 * of the monotonic clock, rounded up to milliseconds
 */

struct radio_air_s
{
  uint32_t minute[RADIO_MINUTES];
  uint32_t ms[RADIO_MINUTES];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* A band plan by its name, or NULL */

FAR const struct radio_plan_s *radio_plan_find(FAR const char *name);

/* The plan whose band covers a radio's, or NULL */

FAR const struct radio_plan_s *radio_plan_for(uint32_t lo_hz,
                                              uint32_t hi_hz);

/* The sub-band of a plan that holds a channel whole (its edges included),
 * by its index, or -1
 */

int radio_band_of(FAR const struct radio_plan_s *plan, uint32_t freq_hz,
                  uint32_t bw_hz);

/* Whether a power at the chip, through an antenna of a gain (hundredths of
 * a dBi), keeps within a sub-band's limit
 */

bool radio_power_ok(FAR const struct radio_band_s *band, int power_dbm,
                    int gain_cdbi);

/* A LoRa bandwidth as asked (within 1 % of one the radio has): the
 * bandwidth used, and its code; false for none
 */

bool radio_bandwidth(uint32_t asked_hz, FAR uint32_t *bw_hz,
                     FAR uint8_t *code);

/* Whether a spreading factor and bandwidth need low data rate
 * optimisation
 */

bool radio_ldro(uint8_t sf, uint32_t bw_hz);

/* A packet's time on the air, in microseconds, rounded up (the SX1261/2
 * datasheet's formula, 6.1.4)
 */

uint32_t radio_airtime_us(FAR const struct radio_lora_s *lora, size_t len);

/* The air time sent in the hour up to a minute, in ms */

uint32_t radio_air_used(FAR const struct radio_air_s *air, uint32_t now);

/* Count a send of ms in a minute */

void radio_air_add(FAR struct radio_air_s *air, uint32_t now, uint32_t ms);

/* Minutes until a send of ms fits within a budget: 0 now, UINT32_MAX
 * never
 */

uint32_t radio_air_wait(FAR const struct radio_air_s *air, uint32_t now,
                        uint32_t budget_ms, uint32_t ms);

#endif /* __PNUT_OS_RADIOS_RULES_H */
