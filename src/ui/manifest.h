/****************************************************************************
 * pnut-os/src/ui/manifest.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_UI_MANIFEST_H
#define __PNUT_OS_UI_MANIFEST_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <pnut/compiler.h>

#include "pnut/manifest.pb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The longest manifest read, and an app's id, without its NUL: NxInit's
 * longest service name (RFC 0037)
 */

#define MANIFEST_SIZE_MAX  1024
#define MANIFEST_ID_MAX    63

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* What the system UI takes from an app's manifest (RFC 0008, RFC 0037):
 * its icon and name, and the templates it may show, a bit for each
 */

struct manifest_s
{
  char icon[sizeof(((FAR pnut_app_manifest_t *)0)->icon)];
  char name[sizeof(((FAR pnut_app_manifest_t *)0)->name)];
  uint32_t templates;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Read <dir>/<id>.manifest, an encoded pnut.AppManifest.  -ENOENT when
 * there is none; -EINVAL for an id that is not one, a file over
 * MANIFEST_SIZE_MAX, or one that does not decode; -ENOMEM; another
 * negated errno value when it cannot be read.
 */

int manifest_load(FAR const char *dir, FAR const char *id,
                  FAR struct manifest_s *m);

/* Whether the app may show a template (a pnut_ui_template_t); none the
 * catalogue does not have
 */

bool manifest_allows(FAR const struct manifest_s *m, int tmpl);

/* Every template of the catalogue: an app from the developer's shell
 * without a manifest
 */

void manifest_any(FAR struct manifest_s *m);

/* An app's id: a reverse-DNS name of lowercase letters, digits, hyphens
 * and underscores, in labels parted by dots, at least two of them, up to
 * MANIFEST_ID_MAX bytes
 */

bool manifest_good_id(FAR const char *id);

#endif /* __PNUT_OS_UI_MANIFEST_H */
