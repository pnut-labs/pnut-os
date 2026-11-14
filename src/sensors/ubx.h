/****************************************************************************
 * pnut-os/src/sensors/ubx.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_SENSORS_UBX_H
#define __PNUT_OS_SENSORS_UBX_H

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

/* u-blox's UBX protocol, as the u-blox M10 SPG 5.10 interface description
 * (UBX-21035062) has it: a frame is 0xb5 0x62, a class, an id, a little
 * endian length, the payload, and a Fletcher checksum over all but the
 * sync bytes.
 */

#define UBX_SYNC1             0xb5
#define UBX_SYNC2             0x62
#define UBX_OVERHEAD          8
#define UBX_PAYLOAD_MAX       256     /* Longer frames are errors */

#define UBX_CLASS_NAV         0x01
#define UBX_CLASS_ACK         0x05
#define UBX_CLASS_CFG         0x06
#define UBX_CLASS_MGA         0x13

#define UBX_NAV_PVT           0x07    /* 92 bytes */
#define UBX_ACK_NAK           0x00
#define UBX_ACK_ACK           0x01
#define UBX_CFG_VALSET        0x8a
#define UBX_CFG_VALGET        0x8b
#define UBX_MGA_INI           0x40
#define UBX_MGA_ACK           0x60
#define UBX_MGA_DBD           0x80

/* Configuration keys; a key's size is in its bits 28 to 30 */

#define UBX_KEY_ANA_USE_ANA          0x10230001  /* L: AssistNow Autonomous */
#define UBX_KEY_MSGOUT_NAV_PVT_UART1 0x20910007  /* U1: rate, epochs */
#define UBX_KEY_MSGOUT_GGA_UART1     0x209100bb
#define UBX_KEY_MSGOUT_GLL_UART1     0x209100ca
#define UBX_KEY_MSGOUT_GSA_UART1     0x209100c0
#define UBX_KEY_MSGOUT_GSV_UART1     0x209100c5
#define UBX_KEY_MSGOUT_RMC_UART1     0x209100ac
#define UBX_KEY_MSGOUT_VTG_UART1     0x209100b1
#define UBX_KEY_NAVSPG_ACKAIDING     0x10110025  /* L: MGA-ACK for aiding */
#define UBX_KEY_PM_OPERATEMODE       0x20d00001  /* E1: 0 full, 1 PSMOO,
                                                  * 2 PSMCT */
#define UBX_KEY_RATE_MEAS            0x30210001  /* U2: ms */
#define UBX_KEY_UART1INPROT_UBX      0x10730001  /* L */
#define UBX_KEY_UART1OUTPROT_UBX     0x10740001  /* L */
#define UBX_KEY_UART1OUTPROT_NMEA    0x10740002  /* L */

/* NAV-PVT's flags */

#define UBX_PVT_VALID_DATE    0x01
#define UBX_PVT_VALID_TIME    0x02
#define UBX_PVT_RESOLVED      0x04
#define UBX_PVT_FIX_OK        0x01
#define UBX_PVT_CONFIRMED     0xc0    /* flags2: date and time confirmed */
#define UBX_PVT_INVALID_LLH   0x0001

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A frame received, its checksum right */

struct ubx_frame_s
{
  uint8_t cls;
  uint8_t id;
  uint16_t len;
  uint8_t payload[UBX_PAYLOAD_MAX];
};

/* The receive side: bytes in, frames out.  Anything that is not UBX (NMEA
 * sentences, noise) is skipped; a frame with a wrong checksum, or longer
 * than any read here (a length from noise, often), is an error, counted,
 * and the bytes after its length are searched for the next frame.
 */

struct ubx_parser_s
{
  uint8_t state;
  uint8_t ck_a;
  uint8_t ck_b;
  uint16_t pos;
  uint16_t len;
  uint32_t errors;                /* Checksums wrong, lengths too long */
  struct ubx_frame_s frame;
};

/* A configuration item to set: its key and value, sized by the key */

struct ubx_item_s
{
  uint32_t key;
  uint32_t value;
};

/* NAV-PVT: a navigation solution */

struct ubx_pvt_s
{
  uint32_t itow;                  /* GPS time of week, ms */
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t min;
  uint8_t sec;
  uint8_t valid;                  /* UBX_PVT_VALID_* */
  uint32_t tacc_ns;
  int32_t nano;
  uint8_t fix;                    /* 0 none, 2 2D, 3 3D, 5 time only */
  uint8_t flags;                  /* UBX_PVT_FIX_OK */
  uint8_t flags2;                 /* UBX_PVT_CONFIRMED */
  uint8_t numsv;
  int32_t lon;                    /* 1e-7 degrees */
  int32_t lat;
  int32_t height_mm;              /* Above the ellipsoid */
  int32_t hmsl_mm;                /* Above mean sea level */
  uint32_t hacc_mm;
  uint32_t vacc_mm;
  int32_t gspeed_mms;             /* Ground speed, mm/s */
  int32_t head_e5;                /* Heading of motion, 1e-5 degrees */
  uint32_t sacc_mms;
  uint32_t headacc_e5;
  uint16_t pdop;                  /* 0.01 */
  uint16_t flags3;                /* UBX_PVT_INVALID_LLH */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* The checksum over a frame's class, id, length and payload */

void ubx_checksum(FAR const uint8_t *data, size_t len, FAR uint8_t *ck_a,
                  FAR uint8_t *ck_b);

/* A frame into buf: its length, or 0 if it does not fit */

size_t ubx_build(FAR uint8_t *buf, size_t size, uint8_t cls, uint8_t id,
                 FAR const uint8_t *payload, uint16_t len);

/* CFG-VALSET of items into the RAM layer; 0 if they do not fit */

size_t ubx_valset(FAR uint8_t *buf, size_t size,
                  FAR const struct ubx_item_s *items, size_t n);

/* CFG-VALGET of keys from the RAM layer */

size_t ubx_valget(FAR uint8_t *buf, size_t size, FAR const uint32_t *keys,
                  size_t n);

/* MGA-INI-TIME_UTC: the time, on receipt, with its accuracy */

size_t ubx_ini_time(FAR uint8_t *buf, size_t size, int64_t utc,
                    uint32_t ns, uint32_t acc_ms);

/* MGA-INI-POS_LLH: a position (1e-7 degrees, cm) with its accuracy */

size_t ubx_ini_pos(FAR uint8_t *buf, size_t size, int32_t lat,
                   int32_t lon, int32_t alt_cm, uint32_t acc_cm);

/* The receive side */

void ubx_parser_init(FAR struct ubx_parser_s *parser);

/* A byte in: true when it completes a frame, then in parser->frame */

bool ubx_parse(FAR struct ubx_parser_s *parser, uint8_t byte);

/* A frame's meaning: false if it is not one of the kind */

bool ubx_nav_pvt(FAR const struct ubx_frame_s *frame,
                 FAR struct ubx_pvt_s *pvt);

/* ACK-ACK or ACK-NAK: the class and id answered, and which */

bool ubx_ack(FAR const struct ubx_frame_s *frame, FAR uint8_t *cls,
             FAR uint8_t *id, FAR bool *ack);

/* MGA-ACK: accepted or not, why, the message's id, and its payload's
 * first four bytes (a database dump's count)
 */

bool ubx_mga_ack(FAR const struct ubx_frame_s *frame, FAR bool *accepted,
                 FAR uint8_t *info, FAR uint8_t *msgid,
                 FAR uint32_t *start);

/* A solution's UTC as seconds since 1970, if its date and time are valid
 * and resolved
 */

bool ubx_pvt_utc(FAR const struct ubx_pvt_s *pvt, FAR int64_t *utc);

#endif /* __PNUT_OS_SENSORS_UBX_H */
