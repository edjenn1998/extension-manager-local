/* Copyright 2026 Extension Manager Local contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "exm-backup.h"
#include <archive.h>
#include <archive_entry.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#define MANIFEST_LIMIT (8 * 1024 * 1024)
#define ARCHIVE_LIMIT (256 * 1024 * 1024)
#define FILE_LIMIT (32 * 1024 * 1024)
#define ENTRY_LIMIT 10000

static gboolean
invalid (GError **error, const gchar *message)
{
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
    return FALSE;
}

const gchar *
exm_backup_string (JsonObject *object, const gchar *key)
{
    JsonNode *node = json_object_get_member (object, key);
    return node && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_STRING
        ? json_node_get_string (node) : NULL;
}

gboolean
exm_backup_uuid_valid (const gchar *uuid)
{
    return uuid && *uuid && strlen (uuid) <= 200 && uuid[0] != '.' &&
        strspn (uuid, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@._-") == strlen (uuid);
}

static JsonArray *
array_member (JsonObject *object, const gchar *key)
{
    JsonNode *node = json_object_get_member (object, key);
    return node && JSON_NODE_HOLDS_ARRAY (node) ? json_node_get_array (node) : NULL;
}

gboolean
exm_backup_directory_supports (const gchar *directory, const gchar *uuid,
                               const gchar *shell, gboolean *known)
{
    *known = FALSE;
    if (!directory) return FALSE;
    GStatBuf directory_stat;
    if (g_lstat (directory, &directory_stat) || !S_ISDIR (directory_stat.st_mode)) return FALSE;
    g_autofree gchar *path = g_build_filename (directory, "metadata.json", NULL);
    GStatBuf st;
    g_autoptr (JsonParser) parser = json_parser_new ();
    if (g_lstat (path, &st) || !S_ISREG (st.st_mode) || st.st_size > 1024 * 1024 ||
        !json_parser_load_from_file (parser, path, NULL)) return FALSE;
    JsonNode *root = json_parser_get_root (parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT (root)) return FALSE;
    JsonObject *metadata = json_node_get_object (root);
    JsonArray *versions = array_member (metadata, "shell-version");
    if (g_strcmp0 (exm_backup_string (metadata, "uuid"), uuid) || !versions ||
        !json_array_get_length (versions) || json_array_get_length (versions) > 128) return FALSE;
    g_auto (GStrv) strings = g_new0 (gchar *, json_array_get_length (versions) + 1);
    for (guint i = 0; i < json_array_get_length (versions); i++) {
        JsonNode *node = json_array_get_element (versions, i);
        if (json_node_get_value_type (node) != G_TYPE_STRING) return FALSE;
        strings[i] = g_strdup (json_node_get_string (node));
    }
    ExmLocalZip zip = { .shell_versions = strings };
    *known = shell && *shell;
    return exm_local_zip_supports (&zip, shell);
}

ExmBackup *
exm_backup_new (const gchar *shell_version)
{
    ExmBackup *backup = g_new0 (ExmBackup, 1);
    backup->manifest = json_object_new ();
    backup->bundles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_bytes_unref);
    json_object_set_string_member (backup->manifest, "format", "extension-manager-local");
    json_object_set_int_member (backup->manifest, "version", 1);
    json_object_set_string_member (backup->manifest, "shell-version", shell_version ? shell_version : "unknown");
    g_autoptr (GDateTime) now = g_date_time_new_now_utc ();
    g_autofree gchar *date = g_date_time_format_iso8601 (now);
    json_object_set_string_member (backup->manifest, "created", date);
    json_object_set_array_member (backup->manifest, "extensions", json_array_new ());
    return backup;
}

void
exm_backup_free (ExmBackup *backup)
{
    if (!backup) return;
    json_object_unref (backup->manifest);
    g_hash_table_unref (backup->bundles);
    g_free (backup);
}

static gboolean
settings_path_safe (const gchar *path)
{
    if (!path || !g_str_has_prefix (path, "/org/gnome/shell/extensions/") ||
        !g_str_has_suffix (path, "/") || strlen (path) > 512 || strstr (path, "//"))
        return FALSE;
    return strspn (path, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-/") == strlen (path);
}

/* Only an adjacent, literal schema_id/path pair in a Gio.Settings constructor
 * is recognized. Dynamic paths and helper abstractions are intentionally not
 * guessed. The same mapping must exist in the CURRENT extension on restore. */
static gchar *
relocatable_path (const gchar *directory, const gchar *id)
{
    const gchar *files[] = { "extension.js", "prefs.js", NULL };
    g_autofree gchar *escaped = g_regex_escape_string (id, -1);
    g_autofree gchar *pattern = g_strdup_printf (
        "new\\s+(?:Gio\\.)?Settings\\s*\\(\\s*\\{\\s*schema[_-]id\\s*:\\s*['\"]%s['\"]\\s*,\\s*path\\s*:\\s*['\"]([^'\"]+)['\"]\\s*[,}]", escaped);
    g_autoptr (GRegex) regex = g_regex_new (pattern, 0, 0, NULL);
    gchar *found = NULL;
    for (guint i = 0; files[i]; i++) {
        g_autofree gchar *path = g_build_filename (directory, files[i], NULL);
        g_autofree gchar *text = NULL;
        GStatBuf st;
        if (g_lstat (path, &st) || !S_ISREG (st.st_mode) || st.st_size > FILE_LIMIT ||
            !g_file_get_contents (path, &text, NULL, NULL)) continue;
        g_autoptr (GMatchInfo) match = NULL;
        g_regex_match (regex, text, 0, &match);
        while (g_match_info_matches (match)) {
            gchar *value = g_match_info_fetch (match, 1);
            if (!settings_path_safe (value) || (found && !g_str_equal (found, value))) {
                g_free (value); g_free (found); return NULL;
            }
            g_free (found); found = value;
            g_match_info_next (match, NULL);
        }
    }
    return found;
}

static GSettingsSchemaSource *
schema_source (const gchar *directory)
{
    if (!directory) return NULL;
    g_autofree gchar *path = g_build_filename (directory, "schemas", NULL);
    /* Never trust a compiled schema bundled in the BACKUP to select targets.
     * This source is opened only from the current installation. */
    return g_settings_schema_source_new_from_directory (path,
        g_settings_schema_source_get_default (), FALSE, NULL);
}

static gboolean
sensitive_name (const gchar *name)
{
    g_autofree gchar *lower = g_ascii_strdown (name, -1);
    const gchar *words[] = { "password", "passwd", "secret", "token", "credential", "api", "auth", "private", "cookie", "account", "email", "location", "history", "command", "script", NULL };
    for (guint i = 0; words[i]; i++) if (strstr (lower, words[i])) return TRUE;
    return FALSE;
}

static gboolean
sensitive_contents (const gchar *data, gsize size)
{
    /* Defense in depth, not a claim that arbitrary source can be proven free
     * of secrets. Ordinary external configuration is never included. */
    if (g_strstr_len (data, size, "-----BEGIN PRIVATE KEY-----") ||
        g_strstr_len (data, size, "-----BEGIN RSA PRIVATE KEY-----")) return TRUE;
    g_autoptr (GRegex) regex = g_regex_new (
        "(?i)(?:password|passwd|secret|(?:api|access|refresh)[_-]?(?:key|token)|auth[_-]?token)[\\\"']?\\s*[:=]\\s*[\\\"'][^\\\"'\\r\\n]{4,}[\\\"']|(?:gh[pousr]_[A-Za-z0-9]{20,}|AKIA[A-Z0-9]{16})",
        G_REGEX_RAW, 0, NULL);
    return g_regex_match_full (regex, data, size, 0, 0, NULL, NULL);
}

/* Arbitrary text/dictionaries/byte arrays can contain credentials even when
 * their key names look innocent. Only scalar preferences, enums and recognized
 * accelerator arrays are supported. This policy is also applied on import. */
static gboolean
value_safe (GSettingsSchemaKey *key, GVariant *value)
{
    if (sensitive_name (g_settings_schema_key_get_name (key))) return FALSE;
    const gchar *type = g_variant_get_type_string (value);
    if (strlen (type) == 1 && strchr ("bniuxqtd", type[0])) return TRUE;
    g_autoptr (GVariant) range = g_settings_schema_key_get_range (key);
    g_autoptr (GVariant) kind = g_variant_get_child_value (range, 0);
    if (g_str_equal (type, "s") && g_str_equal (g_variant_get_string (kind, NULL), "enum")) return TRUE;
    if (g_str_equal (type, "as") && (strstr (g_settings_schema_key_get_name (key), "shortcut") ||
        strstr (g_settings_schema_key_get_name (key), "keybinding") || strstr (g_settings_schema_key_get_name (key), "hotkey"))) {
        if (g_variant_n_children (value) > 32) return FALSE;
        for (gsize i = 0; i < g_variant_n_children (value); i++) {
            g_autoptr (GVariant) child = g_variant_get_child_value (value, i);
            const gchar *s = g_variant_get_string (child, NULL);
            if (strlen (s) > 128 || strspn (s, "<>abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-+") != strlen (s)) return FALSE;
        }
        return TRUE;
    }
    return FALSE;
}

static JsonArray *
export_settings (const gchar *directory, guint *skipped)
{
    JsonArray *result = json_array_new ();
    g_autoptr (GSettingsSchemaSource) source = schema_source (directory);
    if (!source) return result;
    g_auto (GStrv) fixed = NULL;
    g_auto (GStrv) relocatable = NULL;
    g_settings_schema_source_list_schemas (source, FALSE, &fixed, &relocatable);
    for (guint pass = 0; pass < 2; pass++) {
        gchar **ids = pass ? relocatable : fixed;
        for (guint i = 0; ids && ids[i]; i++) {
            g_autoptr (GSettingsSchema) schema = g_settings_schema_source_lookup (source, ids[i], FALSE);
            g_autofree gchar *dynamic = pass ? relocatable_path (directory, ids[i]) : NULL;
            const gchar *path = pass ? dynamic : g_settings_schema_get_path (schema);
            if (!settings_path_safe (path)) { (*skipped)++; continue; }
            g_autoptr (GSettings) settings = g_settings_new_full (schema, NULL, path);
            JsonObject *record = json_object_new ();
            json_object_set_string_member (record, "schema", ids[i]);
            json_object_set_string_member (record, "path", path);
            JsonObject *values = json_object_new ();
            json_object_set_object_member (record, "values", values);
            g_auto (GStrv) keys = g_settings_schema_list_keys (schema);
            for (guint j = 0; keys[j]; j++) {
                g_autoptr (GSettingsSchemaKey) key = g_settings_schema_get_key (schema, keys[j]);
                g_autoptr (GVariant) value = g_settings_get_value (settings, keys[j]);
                if (!value_safe (key, value)) { (*skipped)++; continue; }
                JsonObject *saved = json_object_new ();
                g_autofree gchar *printed = g_variant_print (value, TRUE);
                json_object_set_string_member (saved, "type", g_variant_get_type_string (value));
                json_object_set_string_member (saved, "value", printed);
                json_object_set_object_member (values, keys[j], saved);
            }
            json_array_add_object_element (result, record);
        }
    }
    return result;
}

static gchar *
settings_report (JsonObject *entry, const gchar *directory, gboolean apply)
{
    guint restored = 0, skipped = 0;
    GString *details = g_string_new (NULL);
    JsonArray *records = array_member (entry, "settings");
    g_autoptr (GSettingsSchemaSource) source = schema_source (directory);
    for (guint i = 0; records && i < json_array_get_length (records); i++) {
        JsonObject *record = json_array_get_object_element (records, i);
        const gchar *id = exm_backup_string (record, "schema");
        const gchar *saved_path = exm_backup_string (record, "path");
        g_autoptr (GSettingsSchema) schema = source ? g_settings_schema_source_lookup (source, id, FALSE) : NULL;
        g_autofree gchar *dynamic = schema && !g_settings_schema_get_path (schema) ? relocatable_path (directory, id) : NULL;
        const gchar *path = schema ? g_settings_schema_get_path (schema) : NULL;
        if (!path) path = dynamic;
        if (!schema || !settings_path_safe (path) || g_strcmp0 (path, saved_path)) {
            skipped++;
            g_string_append_printf (details, "\n  %s: schema/path unavailable or changed", id);
            continue;
        }
        g_autoptr (GSettings) settings = g_settings_new_full (schema, NULL, path);
        JsonObject *values = json_object_get_object_member (record, "values");
        g_autoptr (GList) keys = json_object_get_members (values);
        for (GList *l = keys; l; l = l->next) {
            const gchar *name = l->data;
            JsonObject *saved = json_object_get_object_member (values, name);
            const gchar *type = exm_backup_string (saved, "type");
            const gchar *text = exm_backup_string (saved, "value");
            g_autoptr (GSettingsSchemaKey) key = g_settings_schema_has_key (schema, name) ? g_settings_schema_get_key (schema, name) : NULL;
            g_autoptr (GVariant) value = key && g_str_equal (type, g_variant_type_peek_string (g_settings_schema_key_get_value_type (key)))
                ? g_variant_parse (g_settings_schema_key_get_value_type (key), text, NULL, NULL, NULL) : NULL;
            if (value && value_safe (key, value) && g_settings_schema_key_range_check (key, value) &&
                g_settings_is_writable (settings, name) && (!apply || g_settings_set_value (settings, name, value))) restored++;
            else { skipped++; g_string_append_printf (details, "\n  %s/%s: missing, filtered, invalid, changed type/range or locked", id, name); }
        }
    }
    if (apply) g_settings_sync ();
    gchar *result = g_strdup_printf ("Settings: %u %s, %u skipped%s%s", restored, apply ? "restored" : "restorable", skipped,
        records && json_array_get_length (records) ? "" : " (no supported settings in backup)", details->str);
    g_string_free (details, TRUE);
    return result;
}

gchar *
exm_backup_restore_settings (JsonObject *entry, const gchar *directory)
{
    return settings_report (entry, directory, TRUE);
}

gchar *
exm_backup_check_settings (JsonObject *entry, const gchar *directory)
{
    return settings_report (entry, directory, FALSE);
}

static gboolean
archive_add (struct archive *writer, const gchar *name, const void *data, gsize length, GError **error)
{
    struct archive_entry *entry = archive_entry_new ();
    archive_entry_set_pathname (entry, name);
    archive_entry_set_filetype (entry, AE_IFREG);
    archive_entry_set_perm (entry, 0600);
    archive_entry_set_size (entry, length);
    gboolean ok = archive_write_header (writer, entry) == ARCHIVE_OK &&
        archive_write_data (writer, data, length) == (la_ssize_t) length;
    archive_entry_free (entry);
    return ok ? TRUE : invalid (error, "Could not write backup archive.");
}

/* No links, hidden files, caches, binaries or external configuration trees.
 * Unknown assets fail the extension's full backup rather than silently making
 * an incomplete runnable copy. Compiled schemas are reconstructed by GNOME. */
static gboolean
pack_tree (struct archive *writer, int directory_fd, const gchar *relative,
           guint *count, gsize *total, guint depth, GError **error)
{
    if (depth > 32) return invalid (error, "Extension directory nesting exceeds limit.");
    /* Traverse relative to pinned directory descriptors. A parent replaced with
     * a symlink during export must never redirect a read into unrelated files. */
    int scan_fd = dup (directory_fd);
    if (scan_fd < 0) return invalid (error, "Cannot open extension directory.");
    DIR *dir = fdopendir (scan_fd);
    if (!dir) { close (scan_fd); return invalid (error, "Cannot read extension directory."); }
    gboolean ok = TRUE;
    struct dirent *dent;
    const gchar *name;
    while (ok && (dent = readdir (dir))) {
        name = dent->d_name;
        if (name[0] == '.' || g_str_equal (name, "gschemas.compiled") ||
            g_str_equal (name, "__pycache__") || g_str_equal (name, "node_modules")) continue;
        if (sensitive_name (name)) { ok = invalid (error, "Potentially sensitive extension filename; full backup refused. Use List Backup."); break; }
        g_autofree gchar *rel = *relative ? g_build_filename (relative, name, NULL) : g_strdup (name);
        GStatBuf st;
        if (fstatat (directory_fd, name, &st, AT_SYMLINK_NOFOLLOW)) { ok = invalid (error, "Cannot inspect extension file."); break; }
        if (S_ISDIR (st.st_mode)) {
            int child_fd = openat (directory_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child_fd < 0) { ok = invalid (error, "Unsafe extension subdirectory."); break; }
            ok = pack_tree (writer, child_fd, rel, count, total, depth + 1, error);
            close (child_fd);
            continue;
        }
        if (!S_ISREG (st.st_mode) || st.st_size < 0 || st.st_size > FILE_LIMIT ||
            ++(*count) > ENTRY_LIMIT || *total + st.st_size > ARCHIVE_LIMIT / 2)
            { ok = invalid (error, "Extension has links, special files or excessive size."); break; }
        const gchar *dot = strrchr (name, '.');
        const gchar *allowed[] = { ".js", ".mjs", ".json", ".xml", ".css", ".ui", ".blp", ".svg", ".png", ".jpg", ".jpeg", ".webp", ".mo", ".po", ".txt", ".md", NULL };
        gboolean supported = g_str_equal (name, "LICENSE") || g_str_equal (name, "COPYING");
        for (guint i = 0; dot && allowed[i]; i++) supported |= g_str_equal (dot, allowed[i]);
        /* JSON settings/data files cannot be distinguished from credentials by
         * extension ownership alone. Only GNOME's required metadata is allowed. */
        if (dot && g_str_equal (dot, ".json") && !g_str_equal (rel, "metadata.json")) supported = FALSE;
        if (!supported) { ok = invalid (error, "Unsupported extension file type; full backup refused. Use List Backup."); break; }
        /* Open without following a symlink swapped after lstat. */
        int fd = openat (directory_fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) { ok = invalid (error, "Could not safely open extension file."); break; }
        GStatBuf current;
        ok = fstat (fd, &current) == 0 && S_ISREG (current.st_mode) && current.st_size == st.st_size;
        g_autofree gchar *data = ok ? g_malloc (st.st_size + 1) : NULL;
        gsize got = 0;
        while (ok && got < (gsize) st.st_size) {
            ssize_t n = read (fd, data + got, st.st_size - got);
            if (n <= 0) ok = FALSE; else got += n;
        }
        close (fd);
        if (!ok) { invalid (error, "Extension changed during backup."); break; }
        if (sensitive_contents (data, got)) { ok = invalid (error, "Potential credentials found in extension assets; full backup refused."); break; }
        *total += got;
        ok = archive_add (writer, rel, data, got, error);
    }
    closedir (dir);
    return ok;
}

static GBytes *
pack_extension (const gchar *directory, const gchar *uuid, GError **error)
{
    g_autofree gchar *temporary = NULL;
    int fd = g_file_open_tmp ("exm-backup-XXXXXX.zip", &temporary, error);
    if (fd < 0) return NULL;
    struct archive *writer = archive_write_new ();
    archive_write_set_format_zip (writer);
    guint count = 0;
    gsize total = 0;
    gboolean ok = archive_write_open_fd (writer, fd) == ARCHIVE_OK;
    if (!ok) invalid (error, "Cannot create extension archive.");
    int directory_fd = g_open (directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
    if (ok && directory_fd < 0) ok = invalid (error, "Unsafe or missing extension directory.");
    if (ok) ok = pack_tree (writer, directory_fd, "", &count, &total, 0, error);
    if (directory_fd >= 0) close (directory_fd);
    if (archive_write_close (writer) != ARCHIVE_OK && ok) ok = invalid (error, "Cannot finish extension archive.");
    archive_write_free (writer);
    close (fd);
    g_autoptr (ExmLocalZip) inspected = ok ? exm_local_zip_inspect (temporary, error) : NULL;
    gchar *data = NULL;
    gsize size = 0;
    if (inspected && !g_str_equal (inspected->uuid, uuid)) invalid (error, "Installed metadata UUID does not match extension.");
    else if (inspected) g_file_get_contents (inspected->bundle, &data, &size, error);
    g_unlink (temporary);
    return data ? g_bytes_new_take (data, size) : NULL;
}

gboolean
exm_backup_add (ExmBackup *backup, JsonObject *entry, const gchar *directory,
                gboolean settings, gboolean full, GError **error)
{
    const gchar *uuid = exm_backup_string (entry, "uuid");
    if (!exm_backup_uuid_valid (uuid)) return invalid (error, "Invalid extension UUID.");
    if (directory) {
        g_autofree gchar *metadata = g_build_filename (directory, "metadata.json", NULL);
        GStatBuf st;
        g_autoptr (JsonParser) parser = json_parser_new ();
        if (g_lstat (metadata, &st) == 0 && S_ISREG (st.st_mode) && st.st_size <= 1024 * 1024 &&
            json_parser_load_from_file (parser, metadata, NULL)) {
            JsonNode *root = json_parser_get_root (parser);
            if (root && JSON_NODE_HOLDS_OBJECT (root)) {
                JsonObject *object = json_node_get_object (root);
                if (g_strcmp0 (exm_backup_string (object, "uuid"), uuid) == 0) {
                    JsonArray *versions = array_member (object, "shell-version");
                    if (versions) json_object_set_array_member (entry, "shell-versions", json_array_ref (versions));
                    const gchar *url = exm_backup_string (object, "url");
                    if (url) json_object_set_string_member (entry, "url", url);
                }
            }
        }
    }
    guint skipped = 0;
    json_object_set_array_member (entry, "settings", settings ? export_settings (directory, &skipped) : json_array_new ());
    json_object_set_int_member (entry, "settings-skipped", skipped);
    json_object_set_string_member (entry, "settings-notice",
        "Only discovered extension schemas and safe preference types are included. Free text, dictionaries, unknown/dynamic paths, secrets and external configuration files are not supported. Configuration may still be sensitive.");
    if (full && directory && g_strcmp0 (exm_backup_string (entry, "source"), "system") != 0) {
        GBytes *bytes = pack_extension (directory, uuid, error);
        if (!bytes) return FALSE;
        gsize accumulated = g_bytes_get_size (bytes);
        GHashTableIter iter;
        gpointer existing;
        g_hash_table_iter_init (&iter, backup->bundles);
        while (g_hash_table_iter_next (&iter, NULL, &existing)) accumulated += g_bytes_get_size (existing);
        if (accumulated > ARCHIVE_LIMIT - MANIFEST_LIMIT - 1024 * 1024) {
            g_bytes_unref (bytes);
            return invalid (error, "Full backup payload limit reached; export this extension in a separate backup.");
        }
        g_hash_table_insert (backup->bundles, g_strdup (uuid), bytes);
        json_object_set_boolean_member (entry, "bundled", TRUE);
    }
    json_array_add_object_element (array_member (backup->manifest, "extensions"), json_object_ref (entry));
    return TRUE;
}

static gchar *
serialize (JsonObject *object, gsize *length)
{
    g_autoptr (JsonNode) node = json_node_new (JSON_NODE_OBJECT);
    json_node_set_object (node, object);
    g_autoptr (JsonGenerator) generator = json_generator_new ();
    json_generator_set_root (generator, node);
    json_generator_set_pretty (generator, TRUE);
    return json_generator_to_data (generator, length);
}

gboolean
exm_backup_write (ExmBackup *backup, GFile *file, gboolean full, GError **error)
{
    json_object_set_string_member (backup->manifest, "mode", full ? "full" : "list");
    gsize length;
    g_autofree gchar *json = serialize (backup->manifest, &length);
    if (length > MANIFEST_LIMIT) return invalid (error, "Manifest exceeds 8 MiB limit.");
    if (!full) return g_file_replace_contents (file, json, length, NULL, FALSE,
        G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, error);
    /* Bound memory and output independently of the per-extension limits. */
    gsize capacity = length + 1024 * 1024;
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init (&iter, backup->bundles);
    while (g_hash_table_iter_next (&iter, &key, &value)) capacity += g_bytes_get_size (value) + 1024;
    if (capacity > ARCHIVE_LIMIT) return invalid (error, "Full backup exceeds 256 MiB limit; export smaller groups.");
    g_autofree gchar *buffer = g_malloc (capacity);
    size_t used = 0;
    struct archive *writer = archive_write_new ();
    archive_write_set_format_zip (writer);
    gboolean ok = archive_write_open_memory (writer, buffer, capacity, &used) == ARCHIVE_OK;
    if (!ok) invalid (error, "Cannot create backup archive.");
    if (ok) ok = archive_add (writer, "manifest.json", json, length, error);
    g_hash_table_iter_init (&iter, backup->bundles);
    while (ok && g_hash_table_iter_next (&iter, &key, &value)) {
        g_autofree gchar *name = g_strdup_printf ("extensions/%s.zip", (gchar *) key);
        gsize size;
        const void *data = g_bytes_get_data (value, &size);
        ok = archive_add (writer, name, data, size, error);
    }
    if (archive_write_close (writer) != ARCHIVE_OK && ok) ok = invalid (error, "Cannot finish backup archive.");
    archive_write_free (writer);
    return ok && g_file_replace_contents (file, buffer, used, NULL, FALSE,
        G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, error);
}

static gboolean
validate_manifest (ExmBackup *backup, const gchar *data, gsize length, GError **error)
{
    g_autoptr (JsonParser) parser = json_parser_new ();
    if (length > MANIFEST_LIMIT) return invalid (error, "Manifest exceeds size limit.");
    if (!json_parser_load_from_data (parser, data, length, error)) return FALSE;
    JsonNode *root = json_parser_get_root (parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT (root)) return invalid (error, "Backup must be a JSON object.");
    JsonObject *object = json_node_get_object (root);
    JsonNode *version = json_object_get_member (object, "version");
    JsonArray *entries = array_member (object, "extensions");
    if (g_strcmp0 (exm_backup_string (object, "format"), "extension-manager-local") ||
        !version || json_node_get_value_type (version) != G_TYPE_INT64 || json_node_get_int (version) != 1 ||
        !exm_backup_string (object, "shell-version") || !entries || json_array_get_length (entries) > 512)
        return invalid (error, "Unsupported backup format/version or extension count.");
    g_autoptr (GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
    for (guint i = 0; i < json_array_get_length (entries); i++) {
        JsonNode *node = json_array_get_element (entries, i);
        if (!JSON_NODE_HOLDS_OBJECT (node)) return invalid (error, "Invalid extension record.");
        JsonObject *entry = json_node_get_object (node);
        const gchar *uuid = exm_backup_string (entry, "uuid");
        if (!exm_backup_uuid_valid (uuid) || !exm_backup_string (entry, "name") ||
            !g_hash_table_add (seen, (gpointer) uuid)) return invalid (error, "Invalid/duplicate extension identity.");
        const gchar *booleans[] = { "enabled", "bundled", NULL };
        for (guint b = 0; booleans[b]; b++) {
            JsonNode *flag = json_object_get_member (entry, booleans[b]);
            if (flag && json_node_get_value_type (flag) != G_TYPE_BOOLEAN) return invalid (error, "Invalid extension flag.");
        }
        JsonArray *records = array_member (entry, "settings");
        if ((json_object_has_member (entry, "settings") && !records) || (records && json_array_get_length (records) > 128)) return invalid (error, "Invalid settings records.");
        for (guint j = 0; records && j < json_array_get_length (records); j++) {
            JsonNode *rn = json_array_get_element (records, j);
            if (!JSON_NODE_HOLDS_OBJECT (rn)) return invalid (error, "Invalid schema record.");
            JsonObject *record = json_node_get_object (rn);
            JsonNode *vn = json_object_get_member (record, "values");
            if (!exm_backup_string (record, "schema") || !settings_path_safe (exm_backup_string (record, "path")) ||
                !vn || !JSON_NODE_HOLDS_OBJECT (vn)) return invalid (error, "Invalid schema/path/values.");
            JsonObject *values = json_node_get_object (vn);
            if (json_object_get_size (values) > 2048) return invalid (error, "Too many settings.");
            g_autoptr (GList) keys = json_object_get_members (values);
            for (GList *l = keys; l; l = l->next) {
                JsonNode *sn = json_object_get_member (values, l->data);
                if (!JSON_NODE_HOLDS_OBJECT (sn)) return invalid (error, "Invalid setting.");
                JsonObject *saved = json_node_get_object (sn);
                const gchar *type = exm_backup_string (saved, "type");
                const gchar *text = exm_backup_string (saved, "value");
                if (!type || strlen (type) > 32 || !g_variant_type_string_is_valid (type) || !text || strlen (text) > 65536)
                    return invalid (error, "Invalid setting type/value.");
            }
        }
    }
    json_object_unref (backup->manifest);
    backup->manifest = json_object_ref (object);
    return TRUE;
}

ExmBackup *
exm_backup_read (GFile *file, GError **error)
{
    /* Bounded streaming read: no untrusted size may trigger an unbounded allocation. */
    g_autoptr (GFileInputStream) stream = g_file_read (file, NULL, error);
    if (!stream) return NULL;
    g_autoptr (GByteArray) input = g_byte_array_new ();
    guint8 chunk[65536];
    gssize n;
    while ((n = g_input_stream_read (G_INPUT_STREAM (stream), chunk, sizeof chunk, NULL, error)) > 0) {
        if (input->len + n > ARCHIVE_LIMIT) { invalid (error, "Backup exceeds 256 MiB limit."); return NULL; }
        g_byte_array_append (input, chunk, n);
    }
    if (n < 0) return NULL;
    g_autoptr (ExmBackup) backup = exm_backup_new ("unknown");
    if (input->len >= 4 && !memcmp (input->data, "PK\003\004", 4)) {
        struct archive *reader = archive_read_new ();
        archive_read_support_format_zip (reader);
        gboolean ok = archive_read_open_memory (reader, input->data, input->len) == ARCHIVE_OK;
        gboolean manifest = FALSE;
        gsize total = 0;
        guint count = 0;
        struct archive_entry *entry;
        int status = ARCHIVE_FATAL;
        g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        while (ok && (status = archive_read_next_header (reader, &entry)) == ARCHIVE_OK) {
            const gchar *name = archive_entry_pathname (entry);
            g_autofree gchar *uuid = NULL;
            if (name && g_str_has_prefix (name, "extensions/") && g_str_has_suffix (name, ".zip"))
                uuid = g_strndup (name + 11, strlen (name) - 15);
            la_int64_t size = archive_entry_size (entry);
            if (!name || (!g_str_equal (name, "manifest.json") && !exm_backup_uuid_valid (uuid)) ||
                archive_entry_filetype (entry) != AE_IFREG || archive_entry_symlink (entry) || archive_entry_hardlink (entry) ||
                archive_entry_is_encrypted (entry) || size < 0 || size > ARCHIVE_LIMIT / 2 ||
                (!uuid && size > MANIFEST_LIMIT) || ++count > 513 || total + size > ARCHIVE_LIMIT || !g_hash_table_add (seen, g_strdup (name))) { ok = FALSE; break; }
            total += size;
            g_autofree gchar *contents = g_malloc (size + 1);
            gsize got = 0;
            while (got < (gsize) size) {
                la_ssize_t read_size = archive_read_data (reader, contents + got, size - got);
                if (read_size <= 0) break;
                got += read_size;
            }
            char extra;
            if (got != (gsize) size || archive_read_data (reader, &extra, 1) != 0) { ok = FALSE; break; }
            contents[size] = 0;
            if (uuid) g_hash_table_insert (backup->bundles, g_steal_pointer (&uuid), g_bytes_new_take (g_steal_pointer (&contents), size));
            else { manifest = TRUE; ok = validate_manifest (backup, contents, size, error); }
        }
        archive_read_free (reader);
        if (!ok || status != ARCHIVE_EOF || !manifest) {
            if (!error || !*error) invalid (error, "Corrupt/unsafe backup archive or missing manifest.");
            return NULL;
        }
        JsonArray *entries = array_member (backup->manifest, "extensions");
        guint expected = 0;
        for (guint i = 0; i < json_array_get_length (entries); i++) {
            JsonObject *e = json_array_get_object_element (entries, i);
            if (json_object_has_member (e, "bundled") && json_object_get_boolean_member (e, "bundled")) {
                const gchar *uuid = exm_backup_string (e, "uuid");
                expected++;
                g_autoptr (ExmLocalZip) zip = exm_backup_inspect_bundle (backup, uuid, error);
                if (!zip) return NULL;
            }
        }
        if (expected != g_hash_table_size (backup->bundles)) { invalid (error, "Unexpected bundled extension."); return NULL; }
    } else {
        if (!validate_manifest (backup, (const gchar *) input->data, input->len, error)) return NULL;
        JsonArray *entries = array_member (backup->manifest, "extensions");
        for (guint i = 0; i < json_array_get_length (entries); i++) {
            JsonObject *e = json_array_get_object_element (entries, i);
            if (json_object_has_member (e, "bundled") && json_object_get_boolean_member (e, "bundled")) {
                invalid (error, "List backup references missing bundled files."); return NULL;
            }
        }
    }
    return g_steal_pointer (&backup);
}

ExmLocalZip *
exm_backup_inspect_bundle (ExmBackup *backup, const gchar *uuid, GError **error)
{
    GBytes *bytes = g_hash_table_lookup (backup->bundles, uuid);
    if (!bytes) { invalid (error, "Bundled extension is missing."); return NULL; }
    g_autofree gchar *temporary = NULL;
    int fd = g_file_open_tmp ("exm-restore-XXXXXX.zip", &temporary, error);
    if (fd < 0) return NULL;
    gsize size;
    const gchar *data = g_bytes_get_data (bytes, &size);
    gsize written = 0;
    while (written < size) {
        ssize_t n = write (fd, data + written, size - written);
        if (n <= 0) break;
        written += n;
    }
    close (fd);
    ExmLocalZip *zip = written == size ? exm_local_zip_inspect (temporary, error) : NULL;
    if (written != size) invalid (error, "Cannot stage bundled extension.");
    g_unlink (temporary);
    if (zip && !g_str_equal (zip->uuid, uuid)) {
        exm_local_zip_free (zip); zip = NULL;
        invalid (error, "Bundle UUID differs from manifest.");
    }
    return zip;
}
