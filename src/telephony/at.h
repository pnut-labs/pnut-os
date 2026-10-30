/****************************************************************************
 * pnut-os/src/telephony/at.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_TELEPHONY_AT_H
#define __PNUT_OS_TELEPHONY_AT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pnut/compiler.h>
#include <pnut/loop.h>
#include <pnut/module.h>
#include <pnut/timer.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AT_LINE_MAX     600         /* The longest line kept whole */
#define AT_REPLY_MAX    1024        /* A command's lines, kept */
#define AT_COMMAND_MAX  256         /* A command, without its CR */
#define AT_PREFIX_MAX   16
#define AT_QUEUE        8           /* Commands waiting, the one sent
                                     * among them */
#define AT_TIMEOUT      5000        /* A command's, by default, in ms */

/* A command's flags */

#define AT_SECRET       0x01        /* Never logged, and wiped once done:
                                     * a PIN */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Called on the loop with a command's result: zero (OK); -EIO for an
 * error, whose line ends reply; -ETIMEDOUT; or the result at_cancel()
 * gave.  reply holds the command's lines, each ended by '\n', valid while
 * the handler runs.
 */

typedef CODE void (*at_done_t)(FAR void *arg, int result,
                               FAR const char *reply);

struct at_ops_s
{
  /* The bytes to the modem, all or none: zero (OK); -EAGAIN when the
   * channel has no room, and at_room() is called once it has; another
   * negated errno value
   */

  CODE int (*send)(FAR void *arg, FAR const char *buf, size_t len);

  /* A line the modem sent unasked */

  CODE void (*urc)(FAR void *arg, FAR const char *line);
};

struct at_command_s
{
  char command[AT_COMMAND_MAX + 2];   /* With its CR */
  char prefix[AT_PREFIX_MAX];
  uint32_t timeout;
  uint8_t flags;
  at_done_t done;
  FAR void *arg;
};

/* The modem's AT engine (RFC 0030): commands queued and sent one at a
 * time, their answers read as they come, with what the modem sends unasked
 * sorted out from between them.  It runs on the module's loop, and logs
 * what goes each way at the module's debug level.
 */

struct at_s
{
  FAR const struct at_ops_s *ops;
  FAR void *arg;
  FAR struct pnut_module_s *module;
  FAR struct pnut_loop_s *loop;

  struct at_command_s queue[AT_QUEUE];
  uint8_t head;
  uint8_t count;
  bool sent;                      /* The first in the queue is out */
  uint8_t cancelling;             /* In at_cancel(), nested perhaps:
                                   * nothing is sent */
  FAR struct pnut_timer_s *timer; /* Its timeout */

  size_t rlen;
  char reply[AT_REPLY_MAX];
  size_t llen;
  bool long_line;                 /* The line is past AT_LINE_MAX */
  char line[AT_LINE_MAX];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: at_init
 ****************************************************************************/

void at_init(FAR struct at_s *at, FAR struct pnut_module_s *module,
             FAR const struct at_ops_s *ops, FAR void *arg);

/****************************************************************************
 * Name: at_command
 *
 * Description:
 *   Queue a command; done is called with its result.
 *
 * Input Parameters:
 *   at      - The engine.
 *   command - The command, without its CR.
 *   prefix  - The lines of the answer to keep: those it begins, which may
 *             look like lines sent unasked ("+CPIN:"); NULL for any line
 *             that cannot be one.
 *   timeout - In ms; zero for AT_TIMEOUT.
 *   flags   - AT_SECRET, or zero.
 *   done    - Called with the result.
 *   arg     - Passed to done.
 *
 * Returned Value:
 *   Zero (OK); -EBUSY when AT_QUEUE commands wait; -E2BIG for a command
 *   or prefix too long.
 *
 ****************************************************************************/

int at_command(FAR struct at_s *at, FAR const char *command,
               FAR const char *prefix, uint32_t timeout, uint8_t flags,
               at_done_t done, FAR void *arg);

/****************************************************************************
 * Name: at_input
 *
 * Description:
 *   The modem's bytes, from the UART or the multiplexer's AT channel.
 *
 ****************************************************************************/

void at_input(FAR struct at_s *at, FAR const char *buf, size_t len);

/****************************************************************************
 * Name: at_room
 *
 * Description:
 *   The channel takes bytes again: send what waits.
 *
 ****************************************************************************/

void at_room(FAR struct at_s *at);

/****************************************************************************
 * Name: at_cancel
 *
 * Description:
 *   End every command with result: the modem went away, or its channel
 *   changed.  Their handlers may queue new ones.  A line half read is
 *   dropped.
 *
 ****************************************************************************/

void at_cancel(FAR struct at_s *at, int result);

/****************************************************************************
 * Name: at_idle
 *
 * Description:
 *   Whether no command waits.
 *
 ****************************************************************************/

bool at_idle(FAR const struct at_s *at);

#endif /* __PNUT_OS_TELEPHONY_AT_H */
