/****************************************************************************
 * pnut-os/tests/unit/test_radio_rules.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Radio's rules (RFC 0034): the band plan's sub-bands and power limits, a
 * LoRa packet's air time against Semtech's figures, and the air time
 * counted over the last hour.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "rules.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void lora(FAR struct radio_lora_s *l, uint8_t sf, uint32_t bw_hz,
                 uint16_t preamble)
{
  uint8_t code;

  memset(l, 0, sizeof(*l));
  assert_true(radio_bandwidth(bw_hz, &l->bw_hz, &code));
  l->bw_code  = code;
  l->sf       = sf;
  l->cr       = 5;
  l->preamble = preamble;
  l->crc      = true;
  l->ldro     = radio_ldro(sf, l->bw_hz);
}

/****************************************************************************
 * Name: test_rules_airtime
 *
 * Description:
 *   Semtech's calculator's figures: 10 bytes at SF7 and 125 kHz take
 *   41.216 ms, at SF12 (with low data rate optimisation) 991.232 ms; SF5
 *   counts its longer preamble and no 8 bits more; a time too long to
 *   count is the largest count.
 *
 ****************************************************************************/

static void test_rules_airtime(void **state)
{
  struct radio_lora_s l;

  lora(&l, 7, 125000, 8);
  assert_false(l.ldro);
  assert_int_equal(radio_airtime_us(&l, 10), 41216);

  lora(&l, 12, 125000, 8);
  assert_true(l.ldro);
  assert_int_equal(radio_airtime_us(&l, 10), 991232);

  /* 255 bytes, 16 symbols of preamble: low data rate optimisation's 2
   * bits fewer a symbol make 51 blocks of 5 symbols, not 43
   */

  lora(&l, 12, 125000, 16);
  assert_int_equal(radio_airtime_us(&l, 255), 9281536);

  lora(&l, 5, 125000, 8);
  assert_int_equal(radio_airtime_us(&l, 10), 12096);

  /* SF11 at 250 kHz, 50 bytes, 16 symbols of preamble: 78.25 symbols of
   * 8.192 ms
   */

  lora(&l, 11, 250000, 16);
  assert_false(l.ldro);
  assert_int_equal(radio_airtime_us(&l, 50), 641024);

  /* A longer packet never takes less */

  assert_true(radio_airtime_us(&l, 51) >= radio_airtime_us(&l, 50));

  /* The longest preamble at SF12 and 7.8 kHz: over 71 minutes, held at
   * the largest count rather than wrapped
   */

  lora(&l, 12, 7812, 65535);
  assert_int_equal(radio_airtime_us(&l, 10), UINT32_MAX);
}

/****************************************************************************
 * Name: test_rules_ldro
 *
 * Description:
 *   On from a symbol of 16 ms: SF11 and SF12 at 125 kHz, SF12 at 250,
 *   SF10 at 62.5.
 *
 ****************************************************************************/

static void test_rules_ldro(void **state)
{
  assert_true(radio_ldro(11, 125000));
  assert_true(radio_ldro(12, 250000));
  assert_true(radio_ldro(10, 62500));
  assert_false(radio_ldro(10, 125000));
  assert_false(radio_ldro(11, 250000));
  assert_true(radio_ldro(7, 7812));
}

/****************************************************************************
 * Name: test_rules_bandwidth
 ****************************************************************************/

static void test_rules_bandwidth(void **state)
{
  uint32_t hz;
  uint8_t code;

  assert_true(radio_bandwidth(250000, &hz, &code));
  assert_int_equal(hz, 250000);
  assert_int_equal(code, 0x05);
  assert_true(radio_bandwidth(7800, &hz, &code));
  assert_int_equal(hz, 7812);
  assert_int_equal(code, 0x00);
  assert_true(radio_bandwidth(41700, &hz, &code));
  assert_int_equal(code, 0x0a);
  assert_true(radio_bandwidth(62500, &hz, &code));
  assert_int_equal(code, 0x03);
  assert_false(radio_bandwidth(200000, &hz, &code));
  assert_false(radio_bandwidth(0, &hz, &code));
}

/****************************************************************************
 * Name: test_rules_mesh_edges
 *
 * Description:
 *   MeshCore and Meshtastic in Europe: 869.525 MHz at 250 kHz fills
 *   869.400 to 869.650 MHz exactly, the 10 % sub-band's edges.  It must
 *   fit; at 500 kHz it does not.
 *
 ****************************************************************************/

static void test_rules_mesh_edges(void **state)
{
  FAR const struct radio_plan_s *plan = radio_plan_find("EU868");

  assert_non_null(plan);
  assert_int_equal(radio_band_of(plan, 869525000, 250000), 4);
  assert_int_equal(plan->bands[4].duty_permille, 100);
  assert_int_equal(radio_band_of(plan, 869525000, 500000), -1);
  assert_int_equal(radio_band_of(plan, 869526000, 250000), -1);
}

/****************************************************************************
 * Name: test_rules_bands
 ****************************************************************************/

static void test_rules_bands(void **state)
{
  FAR const struct radio_plan_s *plan = radio_plan_find("EU868");

  assert_non_null(plan);
  assert_null(radio_plan_find("US915"));

  assert_int_equal(radio_band_of(plan, 868100000, 125000), 2);
  assert_int_equal(radio_band_of(plan, 863062500, 125000), 0);
  assert_int_equal(plan->bands[0].duty_permille, 1);
  assert_int_equal(radio_band_of(plan, 869937500, 125000), 5);

  /* Across two sub-bands, in the gap between two, outside the plan */

  assert_int_equal(radio_band_of(plan, 864900000, 250000), -1);
  assert_int_equal(radio_band_of(plan, 869300000, 125000), -1);
  assert_int_equal(radio_band_of(plan, 915000000, 125000), -1);
  assert_int_equal(radio_band_of(NULL, 868100000, 125000), -1);

  /* The plan for a radio's band: the T-Deck Max's SX1262 build; none for
   * a 915 MHz build or a radio of every band
   */

  assert_ptr_equal(radio_plan_for(863000000, 870000000), plan);
  assert_null(radio_plan_for(902000000, 928000000));
  assert_null(radio_plan_for(150000000, 960000000));
}

/****************************************************************************
 * Name: test_rules_power
 *
 * Description:
 *   At the antenna, ERP: 16 dBm through a 0 dBi antenna is 13.85 dBm, under
 *   25 mW; 17 dBm is over.  Through a 3 dBi whip, 13 dBm at most.  The
 *   10 % sub-band takes the chip's 22 dBm.
 *
 ****************************************************************************/

static void test_rules_power(void **state)
{
  FAR const struct radio_plan_s *plan = radio_plan_find("EU868");

  assert_true(radio_power_ok(&plan->bands[2], 16, 0));
  assert_false(radio_power_ok(&plan->bands[2], 17, 0));
  assert_true(radio_power_ok(&plan->bands[2], 13, 300));
  assert_false(radio_power_ok(&plan->bands[2], 14, 300));
  assert_true(radio_power_ok(&plan->bands[4], 22, 0));
  assert_true(radio_power_ok(&plan->bands[4], 22, 300));
}

/****************************************************************************
 * Name: test_rules_air
 *
 * Description:
 *   0.1 % of an hour is 3600 ms.  2000 ms in minute 100 and 1500 in minute
 *   130 leave room for 100 ms, not 200, until minute 100 leaves the count
 *   at minute 161.
 *
 ****************************************************************************/

static void test_rules_air(void **state)
{
  struct radio_air_s air;

  memset(&air, 0, sizeof(air));
  radio_air_add(&air, 100, 2000);
  radio_air_add(&air, 130, 1000);
  radio_air_add(&air, 130, 500);

  assert_int_equal(radio_air_used(&air, 130), 3500);
  assert_int_equal(radio_air_wait(&air, 130, 3600, 100), 0);
  assert_int_equal(radio_air_wait(&air, 130, 3600, 200), 31);
  assert_int_equal(radio_air_used(&air, 160), 3500);
  assert_int_equal(radio_air_used(&air, 161), 1500);
  assert_int_equal(radio_air_used(&air, 191), 0);
  assert_int_equal(radio_air_wait(&air, 130, 3600, 4000), UINT32_MAX);

  /* A minute's slot taken again an hour on forgets the old count */

  radio_air_add(&air, 100 + RADIO_MINUTES, 7);
  assert_int_equal(radio_air_used(&air, 100 + RADIO_MINUTES), 1500 + 7);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_rules_airtime),
    cmocka_unit_test(test_rules_ldro),
    cmocka_unit_test(test_rules_bandwidth),
    cmocka_unit_test(test_rules_mesh_edges),
    cmocka_unit_test(test_rules_bands),
    cmocka_unit_test(test_rules_power),
    cmocka_unit_test(test_rules_air),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
