/****************************************************************************
 * pnut-os/src/telephony/parse.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_TELEPHONY_PARSE_H
#define __PNUT_OS_TELEPHONY_PARSE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pnut/compiler.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A reading of the time from the modem */

struct tel_time_s
{
  int64_t utc;                    /* Seconds since 1970, UTC */
  int32_t offset;                 /* Seconds east of UTC, summer time in */
  bool has_time;
  bool has_offset;
  bool dst;                       /* Summer time is in the offset */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: tel_args
 *
 * Description:
 *   A line's arguments: what follows "+XXX: ", or the whole line.
 *
 ****************************************************************************/

FAR const char *tel_args(FAR const char *line);

/****************************************************************************
 * Name: tel_field
 *
 * Description:
 *   Field n, from 0, of a line's arguments: separated by commas outside
 *   quotes, its quotes removed, cut to size.
 *
 * Returned Value:
 *   Whether the arguments have it.
 *
 ****************************************************************************/

bool tel_field(FAR const char *args, int n, FAR char *out, size_t size);

/****************************************************************************
 * Name: tel_utc
 *
 * Description:
 *   A date and time in UTC as seconds since 1970; a year under 100 is
 *   this century's.
 *
 * Returned Value:
 *   The seconds; -1 for a date or time out of range.
 *
 ****************************************************************************/

int64_t tel_utc(int year, int month, int day, int hour, int min, int sec);

/****************************************************************************
 * Name: tel_network_time
 *
 * Description:
 *   A report of the network's own time message: +CTZEU (UTC), +CTZE
 *   (local time), +CTZV (the offset alone), or *PSUTTZ.
 *
 * Returned Value:
 *   Zero (OK); -EINVAL for another line, or one that does not parse.
 *
 ****************************************************************************/

int tel_network_time(FAR const char *line, FAR struct tel_time_s *t);

/****************************************************************************
 * Name: tel_clock
 *
 * Description:
 *   The modem's clock, from +CCLK: "yy/MM/dd,hh:mm:ss+zz", its local time
 *   and offset in quarters of an hour.
 *
 * Returned Value:
 *   As tel_network_time().
 *
 ****************************************************************************/

int tel_clock(FAR const char *line, FAR struct tel_time_s *t);

/****************************************************************************
 * Name: tel_registration
 *
 * Description:
 *   The registration's state from +CREG or +CEREG: first as the modem
 *   reports it unasked, second in the answer to a question.
 *
 * Returned Value:
 *   A pnut.TelephonyInfo.Registration; -EINVAL.
 *
 ****************************************************************************/

int tel_registration(FAR const char *line, bool answer);

/****************************************************************************
 * Name: tel_signal
 *
 * Description:
 *   The signal from +CSQ, in dBm (0 when the modem does not know it) and
 *   in bars, 0 to 4.
 *
 * Returned Value:
 *   Zero (OK); -EINVAL.
 *
 ****************************************************************************/

int tel_signal(FAR const char *line, FAR int32_t *dbm, FAR uint32_t *bars);

/****************************************************************************
 * Name: tel_operator
 *
 * Description:
 *   The operator's name, and the technology as a
 *   pnut.TelephonyInfo.Tech, from +COPS.
 *
 * Returned Value:
 *   Zero (OK); -EINVAL.  A modem registered nowhere gives no name.
 *
 ****************************************************************************/

int tel_operator(FAR const char *line, FAR char *name, size_t size,
                 FAR int *tech);

/****************************************************************************
 * Name: tel_sim
 *
 * Description:
 *   The SIM's state, a pnut.TelephonyInfo.Sim, from +CPIN, or from the
 *   error a SIM's trouble gives.
 *
 * Returned Value:
 *   The state; -EINVAL for a line that tells nothing of the SIM (a busy
 *   SIM, or an error of another kind).
 *
 ****************************************************************************/

int tel_sim(FAR const char *line);

/****************************************************************************
 * Name: tel_tries
 *
 * Description:
 *   The tries left for the PIN and the PUK, from SIMCom's +SPIC.
 *
 * Returned Value:
 *   Zero (OK); -EINVAL.
 *
 ****************************************************************************/

int tel_tries(FAR const char *line, FAR uint32_t *pin, FAR uint32_t *puk);

/****************************************************************************
 * Name: tel_sms_slot, tel_sms_listed, tel_sms_length
 *
 * Description:
 *   SMS in the modem's memory, in PDU mode: the slot an SMS was stored in
 *   (+CMTI: "ME",<slot>), the slot of one listed (+CMGL: <slot>,<stat>,,
 *   <length>), and the length of one read (+CMGR: <stat>,,<length>), its
 *   TPDU's octets; the PDU follows on the next line.
 *
 * Returned Value:
 *   The slot, or the length; -EINVAL; for tel_sms_slot(), -ENODEV for an
 *   SMS stored elsewhere than the modem's memory ("SM", the SIM): the
 *   storage setting went, to be given again, and the memory listed.
 *
 ****************************************************************************/

int tel_sms_slot(FAR const char *line);
int tel_sms_listed(FAR const char *line);
int tel_sms_length(FAR const char *line);

#endif /* __PNUT_OS_TELEPHONY_PARSE_H */
