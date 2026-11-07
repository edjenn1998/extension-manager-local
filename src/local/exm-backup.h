/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <json-glib/json-glib.h>
#include "exm-local-zip.h"

/* All filesystem/settings operations are blocking; GUI callers use GTask.
 * Imported paths never authorize writes. Settings destinations are rediscovered
 * from the currently installed extension, not taken from the manifest. */
typedef struct {
    JsonObject *manifest;
    GHashTable *bundles; /* UUID -> GBytes (validated again before installation) */
} ExmBackup;

ExmBackup *exm_backup_new (const gchar *shell_version);
void exm_backup_free (ExmBackup *backup);
gboolean exm_backup_uuid_valid (const gchar *uuid);
const gchar *exm_backup_string (JsonObject *object, const gchar *key);
gboolean exm_backup_directory_supports (const gchar *directory, const gchar *uuid,
                                        const gchar *shell, gboolean *known);
gboolean exm_backup_add (ExmBackup *backup, JsonObject *entry, const gchar *directory,
                         gboolean settings, gboolean full, GError **error);
gboolean exm_backup_write (ExmBackup *backup, GFile *file, gboolean full, GError **error);
ExmBackup *exm_backup_read (GFile *file, GError **error);
ExmLocalZip *exm_backup_inspect_bundle (ExmBackup *backup, const gchar *uuid, GError **error);
gchar *exm_backup_restore_settings (JsonObject *entry, const gchar *directory);
gchar *exm_backup_check_settings (JsonObject *entry, const gchar *directory);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ExmBackup, exm_backup_free)
