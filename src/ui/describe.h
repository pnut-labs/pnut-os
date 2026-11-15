/****************************************************************************
 * pnut-os/src/ui/describe.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_DESCRIBE_H
#define __PNUT_OS_UI_DESCRIBE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <pnut/compiler.h>

#include "pnut/telephony.pb.h"
#include "pnut/wifi.pb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The header's meters' steps */

#define DESCRIBE_STEPS  5

/* A network's name as shown: 32 bytes, each perhaps \xNN */

#define DESCRIBE_NAME_MAX  (32 * 4 + 1)

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Wi-Fi's one-line state: "On · Home".  The length written. */

size_t describe_wifi(FAR const pnut_wifi_info_t *w, FAR char *buf,
                     size_t size);

/* The mobile network's: "LTE · data off" */

size_t describe_cell(FAR const pnut_telephony_info_t *t, FAR char *buf,
                     size_t size);

/* Why Wi-Fi did not do what was asked, from an answer's status and
 * error code (WifiError)
 */

FAR const char *describe_wifi_failure(int status, uint32_t code);

/* A network's strength in a word, from its dBm */

FAR const char *describe_strength(int32_t dbm);

/* The header's meters: steps of DESCRIBE_STEPS lit for Telephony's bars
 * (0 to 4) and the battery's level (percent)
 */

uint8_t describe_signal_steps(uint32_t bars);
uint8_t describe_battery_steps(uint32_t level);

/* A name as shown: its characters the fonts draw as they are; any other
 * (not UTF-8, a control, invisible, direction-changing, one the fonts
 * lack, the backslash) as its bytes, \xNN each, so that no name can look
 * like another
 */

size_t describe_name(FAR const char *name, FAR char *buf, size_t size);

/* "Tue, 22 Sep"; "14:07" */

size_t describe_date(FAR const struct tm *tm, FAR char *buf, size_t size);
size_t describe_clock(FAR const struct tm *tm, FAR char *buf, size_t size);

#endif /* __PNUT_OS_UI_DESCRIBE_H */
