/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <gio/gio.h>

typedef struct
{
    gchar *directory;
    gchar *bundle;
    gchar *uuid;
    gchar *name;
    gchar *description;
    gchar *version;
    gchar *metadata_sha256;
    gchar **shell_versions;
} ExmLocalZip;

/* Blocking operations: callers in the GUI must use a worker thread. The bundle
 * is a private snapshot, not the user-controlled original selected file. */
ExmLocalZip *exm_local_zip_inspect (const gchar *filename, GError **error);
void exm_local_zip_free (ExmLocalZip *zip);
gboolean exm_local_zip_supports (ExmLocalZip *zip, const gchar *shell_version);
gboolean exm_local_zip_is_installed (ExmLocalZip *zip);
gboolean exm_local_zip_install (ExmLocalZip *zip, gboolean replace, GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ExmLocalZip, exm_local_zip_free)
