/****************************************************************************
 * pnut-os/src/ui/text.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_TEXT_H
#define __PNUT_OS_UI_TEXT_H

/* The system UI's text, English, all in one place, each marked for gettext
 * (RFC 0017): N_() marks a string for translation and leaves it as it is.
 * "·" is U+00B7.
 */

#ifndef N_
#  define N_(s) s
#endif

/* The middle dot between parts of a line */

#define UI_DOT                N_(" \xc2\xb7 ")

/* Where a state is not known yet */

#define UI_T_UNKNOWN          N_("\xe2\x80\x94")

/* Wi-Fi's line */

#define UI_T_OFF              N_("Off")
#define UI_T_ON               N_("On")
#define UI_T_JOINING          N_("Joining")

/* The mobile network's line */

#define UI_T_MODEM_OFF        N_("Modem off")
#define UI_T_MODEM_FAILED     N_("Modem failed")
#define UI_T_NO_MODEM         N_("No modem")
#define UI_T_STARTING         N_("Starting")
#define UI_T_NO_SIM           N_("No SIM")
#define UI_T_SIM_LOCKED       N_("SIM locked")
#define UI_T_SIM_PUK          N_("SIM needs its PUK")
#define UI_T_SIM_BLOCKED      N_("SIM blocked")
#define UI_T_SIM_FAILED       N_("SIM failed")
#define UI_T_SEARCHING        N_("Searching")
#define UI_T_NO_NETWORK       N_("No network")
#define UI_T_REFUSED          N_("Network refused")
#define UI_T_LTE              N_("LTE")
#define UI_T_GSM              N_("GSM")
#define UI_T_NETWORK          N_("Network")
#define UI_T_DATA_ON          N_("data on")
#define UI_T_DATA_OFF         N_("data off")
#define UI_T_DATA_CONNECTING  N_("data connecting")
#define UI_T_DATA_FAILED      N_("data failed")
#define UI_T_DATA_STANDBY     N_("data on standby")

/* A network's strength */

#define UI_T_STRONG           N_("strong")
#define UI_T_FAIR             N_("fair")
#define UI_T_WEAK             N_("weak")

/* The date: Tue, 22 Sep */

#define UI_T_WEEKDAYS         N_("Sun Mon Tue Wed Thu Fri Sat")
#define UI_T_MONTHS           N_("Jan Feb Mar Apr May Jun Jul Aug Sep Oct " \
                                 "Nov Dec")

/* A folder's line: FOLDER · 4 APPS */

#define UI_T_FOLDER_APPS      N_("FOLDER \xc2\xb7 %u APPS")

/* Placeholders */

#define UI_T_NOT_AVAILABLE    N_("Not available yet")

/* Softkeys */

#define UI_T_OPEN             N_("Open")
#define UI_T_BACK             N_("Back")
#define UI_T_OPTIONS          N_("Options")
#define UI_T_MENU             N_("Menu")

/* The options' heading */

#define UI_T_OPTIONS_HEAD     N_("OPTIONS")

/* Settings (Proto-Settings) */

#define UI_T_SETTINGS         N_("Settings")
#define UI_T_WIFI             N_("Wi-Fi")
#define UI_T_CELLULAR         N_("Cellular")

/* Wi-Fi (Set-WiFi, Set-WiFi-Join) */

#define UI_T_CONNECTED        N_("Connected")
#define UI_T_NEARBY           N_("Nearby")
#define UI_T_SCANNING         N_("Nearby \xc2\xb7 scanning")
#define UI_T_SCANNED          N_("Nearby \xc2\xb7 scanned %02d:%02d")
#define UI_T_SCAN_FAILED      N_("Nearby \xc2\xb7 scan failed")
#define UI_T_NONE_NEARBY      N_("Nearby \xc2\xb7 none in reach")
#define UI_T_SECURED          N_("Secured")
#define UI_T_OPEN_NETWORK     N_("Open")
#define UI_T_SAVED            N_("saved")
#define UI_T_JOINING_DOTS     N_("Joining\xe2\x80\xa6")
#define UI_T_SCAN_AGAIN       N_("Scan again")
#define UI_T_LEAVE            N_("Leave")
#define UI_T_FORGET           N_("Forget")
#define UI_T_FORGET_YES       N_("Yes, forget it")
#define UI_T_KEEP             N_("No, keep it")
#define UI_T_SECURITY         N_("Security")
#define UI_T_WPA              N_("WPA2/WPA3-Personal")
#define UI_T_PASSWORD         N_("Password")
#define UI_T_SHOW_PASSWORD    N_("Show password")
#define UI_T_JOIN             N_("Join")
#define UI_T_PASSWORD_LENGTH  N_("The password: 8 to 63 characters")

/* Why Wi-Fi did not do what was asked (WifiError's codes) */

#define UI_T_WIFI_OFF         N_("Wi-Fi is off")
#define UI_T_WIFI_NO_RADIO    N_("The radio would not start")
#define UI_T_WIFI_NOT_FOUND   N_("Not in reach")
#define UI_T_WIFI_REFUSED     N_("Wrong password, or out of reach")
#define UI_T_WIFI_NO_ADDRESS  N_("Joined, but no address given")
#define UI_T_WIFI_CANCELLED   N_("Cancelled")
#define UI_T_WIFI_NO_ANSWER   N_("Wi-Fi did not answer")
#define UI_T_WIFI_FAILED      N_("Wi-Fi failed")

#endif /* __PNUT_OS_UI_TEXT_H */
