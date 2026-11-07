/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "local/exm-backup.h"
#include <stdio.h>

int main (int argc, char **argv)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GFile) file = NULL;
    g_autoptr (ExmBackup) backup = NULL;
    if (argc < 3) return 2;
    if (g_str_equal (argv[1], "supports") && argc == 5) {
        gboolean known = FALSE;
        gboolean compatible = exm_backup_directory_supports (argv[2], argv[3], argv[4], &known);
        printf ("%s %s\n", known ? "known" : "unknown", compatible ? "compatible" : "incompatible");
        return 0;
    }
    if (g_str_equal (argv[1], "export") && argc == 6) {
        g_autoptr (ExmBackup) backup = exm_backup_new ("51.1");
        g_autoptr (JsonObject) entry = json_object_new ();
        json_object_set_string_member (entry, "uuid", argv[3]);
        json_object_set_string_member (entry, "name", "Disposable backup test");
        json_object_set_string_member (entry, "source", g_str_equal (argv[5], "system") ? "system" : "user-unknown");
        json_object_set_boolean_member (entry, "enabled", FALSE);
        g_autoptr (GFile) file = g_file_new_for_path (argv[4]);
        gboolean full = !g_str_equal (argv[5], "list");
        if (!exm_backup_add (backup, entry, argv[2], TRUE, full, &error) ||
            !exm_backup_write (backup, file, full, &error)) goto failed;
        return 0;
    }
    file = g_file_new_for_path (argv[2]);
    backup = exm_backup_read (file, &error);
    if (!backup) goto failed;
    if (g_str_equal (argv[1], "read")) {
        printf ("%u extensions, %u bundles\n", json_array_get_length (json_object_get_array_member (backup->manifest, "extensions")), g_hash_table_size (backup->bundles));
    } else if ((g_str_equal (argv[1], "restore") || g_str_equal (argv[1], "check")) && argc == 4) {
        JsonArray *entries = json_object_get_array_member (backup->manifest, "extensions");
        for (guint i = 0; i < json_array_get_length (entries); i++) {
            g_autofree gchar *report = g_str_equal (argv[1], "check") ?
                exm_backup_check_settings (json_array_get_object_element (entries, i), argv[3]) :
                exm_backup_restore_settings (json_array_get_object_element (entries, i), argv[3]);
            puts (report);
        }
    } else if (g_str_equal (argv[1], "install") && argc == 4) {
        g_autoptr (ExmLocalZip) zip = exm_backup_inspect_bundle (backup, argv[3], &error);
        if (!zip || !exm_local_zip_install (zip, FALSE, &error)) goto failed;
    } else return 2;
    return 0;
failed:
    fprintf (stderr, "%s\n", error ? error->message : "operation failed");
    return 1;
}
