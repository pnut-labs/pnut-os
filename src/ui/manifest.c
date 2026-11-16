/****************************************************************************
 * pnut-os/src/ui/manifest.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

/* Apps' manifests, as far as the system UI reads them (RFC 0037).  Until
 * Packages exists (RFC 0009), a built-in app's is a file of its own,
 * <dir>/<id>.manifest, read whole: it is small.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <pb_decode.h>

#include "manifest.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The catalogue's templates: the bits of struct manifest_s's templates
 * that can be set, LIST to BUSY at level 1
 */

#define MANIFEST_CATALOGUE \
  (((1u << (_PNUT_UI_TEMPLATE_MAX + 1)) - 1) & \
   ~(1u << PNUT_UI_TEMPLATE_UI_TEMPLATE_NONE))

/****************************************************************************
 * Public Functions
 ****************************************************************************/

bool manifest_good_id(FAR const char *id)
{
  size_t label = 0;
  int dots = 0;
  size_t i;

  for (i = 0; id[i] != '\0'; i++)
    {
      char c = id[i];

      if (i >= MANIFEST_ID_MAX)
        {
          return false;
        }

      if (c == '.')
        {
          if (label == 0)
            {
              return false;
            }

          label = 0;
          dots++;
        }
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_')
        {
          label++;
        }
      else
        {
          return false;
        }
    }

  return dots > 0 && label > 0;
}

int manifest_load(FAR const char *dir, FAR const char *id,
                  FAR struct manifest_s *m)
{
  static uint8_t buf[MANIFEST_SIZE_MAX + 1];
  pnut_app_manifest_t in;
  pb_istream_t stream;
  char path[128];
  ssize_t n;
  size_t len = 0;
  int ret = 0;
  int fd;
  pb_size_t i;

  memset(m, 0, sizeof(*m));
  if (!manifest_good_id(id))
    {
      return -EINVAL;
    }

  n = snprintf(path, sizeof(path), "%s/%s.manifest", dir, id);
  if (n < 0 || n >= (ssize_t)sizeof(path))
    {
      return -ENAMETOOLONG;
    }

  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      return -errno;
    }

  /* One byte more than the most taken, to tell a file too long */

  while (len < sizeof(buf))
    {
      n = read(fd, buf + len, sizeof(buf) - len);
      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          ret = -errno;
          break;
        }

      if (n == 0)
        {
          break;
        }

      len += n;
    }

  close(fd);
  if (ret < 0)
    {
      return ret;
    }

  if (len > MANIFEST_SIZE_MAX)
    {
      return -EINVAL;
    }

  memset(&in, 0, sizeof(in));
  stream = pb_istream_from_buffer(buf, len);
  if (!pb_decode(&stream, PNUT_APP_MANIFEST_FIELDS, &in))
    {
      return -EINVAL;
    }

  strlcpy(m->icon, in.icon, sizeof(m->icon));
  strlcpy(m->name, in.name, sizeof(m->name));
  for (i = 0; i < in.templates_count; i++)
    {
      int t = (int)in.templates[i];

      if (t >= 0 && t < 32)
        {
          m->templates |= (1u << t) & MANIFEST_CATALOGUE;
        }
    }

  return 0;
}

bool manifest_allows(FAR const struct manifest_s *m, int tmpl)
{
  return tmpl >= 0 && tmpl < 32 &&
         (m->templates & MANIFEST_CATALOGUE & (1u << tmpl)) != 0;
}

void manifest_any(FAR struct manifest_s *m)
{
  m->templates = MANIFEST_CATALOGUE;
}
