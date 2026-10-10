/****************************************************************************
 * pnut-os/src/system/programs.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "programs.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: programs_name
 *
 * Description:
 *   Whether a word is a good name.
 *
 ****************************************************************************/

static bool programs_name(FAR const char *word)
{
  size_t len = strlen(word);
  size_t i;

  if (len == 0 || len > PROGRAMS_NAME_MAX)
    {
      return false;
    }

  for (i = 0; i < len; i++)
    {
      char c = word[i];

      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
        {
          return false;
        }
    }

  return true;
}

/****************************************************************************
 * Name: programs_find
 ****************************************************************************/

static FAR const struct programs_entry_s *
programs_find(FAR const struct programs_s *programs,
              FAR const char *program)
{
  uint8_t i;

  for (i = 0; i < programs->nprograms; i++)
    {
      if (strcmp(programs->programs[i].name, program) == 0)
        {
          return &programs->programs[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: programs_line
 *
 * Description:
 *   Take one line, NUL-terminated in the table's text: its words become
 *   names in place.
 *
 ****************************************************************************/

static int programs_line(FAR struct programs_s *programs, FAR char *line)
{
  FAR struct programs_entry_s *entry = NULL;
  FAR char *word;
  FAR char *save;
  FAR char *hash;
  uint8_t i;

  hash = strchr(line, '#');
  if (hash != NULL)
    {
      *hash = '\0';
    }

  for (word = strtok_r(line, " \t\r", &save); word != NULL;
       word = strtok_r(NULL, " \t\r", &save))
    {
      if (!programs_name(word))
        {
          return -EINVAL;
        }

      if (entry == NULL)
        {
          if (programs_find(programs, word) != NULL)
            {
              return -EINVAL;
            }

          if (programs->nprograms == PROGRAMS_MAX)
            {
              return -E2BIG;
            }

          entry        = &programs->programs[programs->nprograms++];
          entry->name  = word;
          entry->first = programs->nservices;
          entry->count = 0;
          continue;
        }

      /* A service is one program's (RFC 0007) */

      for (i = 0; i < programs->nservices; i++)
        {
          if (strcmp(programs->services[i], word) == 0)
            {
              return -EINVAL;
            }
        }

      if (programs->nservices == PROGRAMS_SERVICES_MAX)
        {
          return -E2BIG;
        }

      programs->services[programs->nservices++] = word;
      entry->count++;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int programs_parse(FAR struct programs_s *programs, FAR const char *text,
                   size_t len, FAR int *linep)
{
  FAR char *line;
  FAR char *next;
  int number = 0;
  int ret = OK;

  memset(programs, 0, sizeof(*programs));

  if (len >= sizeof(programs->text))
    {
      return -E2BIG;
    }

  memcpy(programs->text, text, len);
  programs->text[len] = '\0';

  for (line = programs->text; line != NULL && ret == OK; line = next)
    {
      next = strchr(line, '\n');
      if (next != NULL)
        {
          *next++ = '\0';
        }

      number++;
      ret = programs_line(programs, line);
    }

  if (ret < 0)
    {
      if (linep != NULL)
        {
          *linep = number;
        }

      memset(programs, 0, sizeof(*programs));
    }

  return ret;
}

int programs_load(FAR struct programs_s *programs, FAR const char *path,
                  FAR int *linep)
{
  char text[PROGRAMS_TEXT_MAX];
  ssize_t n;
  size_t len = 0;
  int fd;

  memset(programs, 0, sizeof(*programs));

  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      return -errno;
    }

  /* One byte more than the table takes tells a text too long */

  while (len < sizeof(text))
    {
      n = read(fd, text + len, sizeof(text) - len);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n < 0)
        {
          n = -errno;
          close(fd);
          return n;
        }

      if (n == 0)
        {
          break;
        }

      len += n;
    }

  close(fd);
  return programs_parse(programs, text, len, linep);
}

bool programs_runs(FAR const struct programs_s *programs,
                   FAR const char *program, FAR const char *service)
{
  FAR const struct programs_entry_s *entry;
  uint8_t i;

  entry = programs_find(programs, program);
  if (entry == NULL)
    {
      return false;
    }

  for (i = 0; i < entry->count; i++)
    {
      if (strcmp(programs->services[entry->first + i], service) == 0)
        {
          return true;
        }
    }

  return false;
}
