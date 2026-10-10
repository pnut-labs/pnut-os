/****************************************************************************
 * pnut-os/src/system/programs.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_SYSTEM_PROGRAMS_H
#define __PNUT_OS_SYSTEM_PROGRAMS_H

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

/* The table's limits: its text, its programs, all their services
 * together, and the longest name, without its NUL
 */

#define PROGRAMS_TEXT_MAX      1024
#define PROGRAMS_MAX           16
#define PROGRAMS_SERVICES_MAX  64
#define PROGRAMS_NAME_MAX      31

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Which services each program runs (RFC 0007): a text file of lines
 *
 *   <program> <service>...
 *
 * with "#" starting a comment, and blank lines.  A program is a service
 * of NxInit's, named as there; its services are named as their settings'
 * owner (RFC 0025), and each is one program's.  Names are letters,
 * digits, '_', '-' and '.'.  The names point into the table's own copy of
 * the text.
 */

struct programs_entry_s
{
  FAR const char *name;
  uint8_t first;                  /* Its first service in services[] */
  uint8_t count;
};

struct programs_s
{
  char text[PROGRAMS_TEXT_MAX];
  struct programs_entry_s programs[PROGRAMS_MAX];
  FAR const char *services[PROGRAMS_SERVICES_MAX];
  uint8_t nprograms;
  uint8_t nservices;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: programs_parse
 *
 * Description:
 *   Read the table from text, len bytes, copied.  On failure the table is
 *   left empty.
 *
 * Returned Value:
 *   Zero (OK) on success; -E2BIG for a text, programs or services over the
 *   limits; -EINVAL for a bad name, or a program or a service named twice,
 *   with *linep (if not NULL) its line, from 1.
 *
 ****************************************************************************/

int programs_parse(FAR struct programs_s *programs, FAR const char *text,
                   size_t len, FAR int *linep);

/****************************************************************************
 * Name: programs_load
 *
 * Description:
 *   Read the table from a file.  On failure the table is left empty.
 *
 * Returned Value:
 *   As programs_parse(), or the negated errno of reading the file.
 *
 ****************************************************************************/

int programs_load(FAR struct programs_s *programs, FAR const char *path,
                  FAR int *linep);

/****************************************************************************
 * Name: programs_runs
 *
 * Description:
 *   Whether the program runs the service.
 *
 ****************************************************************************/

bool programs_runs(FAR const struct programs_s *programs,
                   FAR const char *program, FAR const char *service);

#endif /* __PNUT_OS_SYSTEM_PROGRAMS_H */
