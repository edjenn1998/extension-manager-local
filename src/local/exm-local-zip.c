/*
 * Local ZIP support for Extension Manager Local.
 * Copyright 2026 Extension Manager Local contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "exm-local-zip.h"

#include <archive.h>
#include <archive_entry.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <gio/gunixinputstream.h>

#define MAX_BUNDLE (128 * 1024 * 1024)
#define MAX_EXPANDED (256 * 1024 * 1024)
#define MAX_METADATA (1024 * 1024)
#define MAX_ENTRIES 10000

static gboolean
invalid (GError **error, const gchar *message)
{
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
    return FALSE;
}

/* Only used on our own private temporary directory, never on an extension or
 * user settings directory. lstat prevents following links during cleanup. */
static void
remove_private_tree (const gchar *path)
{
    GStatBuf st;
    if (g_lstat (path, &st) != 0)
        return;
    if (S_ISDIR (st.st_mode))
    {
        g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
        const gchar *name;
        while (dir && (name = g_dir_read_name (dir)))
        {
            g_autofree gchar *child = g_build_filename (path, name, NULL);
            remove_private_tree (child);
        }
        g_rmdir (path);
    }
    else
        g_unlink (path);
}

void
exm_local_zip_free (ExmLocalZip *zip)
{
    if (!zip)
        return;
    if (zip->directory)
        remove_private_tree (zip->directory);
    g_free (zip->directory);
    g_free (zip->bundle);
    g_free (zip->uuid);
    g_free (zip->name);
    g_free (zip->description);
    g_free (zip->version);
    g_free (zip->metadata_sha256);
    g_strfreev (zip->shell_versions);
    g_free (zip);
}

static gboolean
safe_path (const gchar *path)
{
    if (!path || !*path || strlen (path) > 4096 || !g_utf8_validate (path, -1, NULL) ||
        g_path_is_absolute (path) || strpbrk (path, "\\:"))
        return FALSE;
    for (const gchar *p = path; *p; p++)
        if ((guchar) *p < 32 || *p == 127)
            return FALSE;
    g_auto (GStrv) parts = g_strsplit (path, "/", -1);
    for (guint i = 0; parts[i]; i++)
        if (!*parts[i] || g_str_equal (parts[i], ".") || g_str_equal (parts[i], ".."))
            return FALSE;
    return TRUE;
}

static const gchar *
string_member (JsonObject *object, const gchar *key)
{
    JsonNode *node = json_object_get_member (object, key);
    return node && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_STRING
        ? json_node_get_string (node) : NULL;
}

static guint16
read16 (const guint8 *p)
{
    return (guint16) p[0] | ((guint16) p[1] << 8);
}

static guint32
read32 (const guint8 *p)
{
    return (guint32) read16 (p) | ((guint32) read16 (p + 2) << 16);
}

/* libarchive normalizes backslashes and some unusual Unix file types. Check
 * the original central directory and local names as well, before handing the
 * exact same archive to GNOME's libarchive-based installer. */
static gboolean
validate_zip_directory (const gchar *filename, GError **error)
{
    g_autoptr (GMappedFile) mapped = g_mapped_file_new (filename, FALSE, error);
    if (!mapped)
        return FALSE;
    gsize length = g_mapped_file_get_length (mapped);
    const guint8 *bytes = (const guint8 *) g_mapped_file_get_contents (mapped);
    if (length < 22)
        return invalid (error, "ZIP is truncated or has no central directory.");
    gssize end = length - 22;
    for (; end >= 0 && (gsize) end + 65557 >= length; end--)
        if (read32 (bytes + end) == 0x06054b50 &&
            (gsize) end + 22 + read16 (bytes + end + 20) == length)
            break;
    if (end < 0 || (gsize) end + 65557 < length)
        return invalid (error, "ZIP has no valid end-of-directory record.");
    guint count = read16 (bytes + end + 10);
    gsize offset = read32 (bytes + end + 16);
    gsize directory_size = read32 (bytes + end + 12);
    if (read16 (bytes + end + 4) || read16 (bytes + end + 6) ||
        read16 (bytes + end + 8) != count || count > MAX_ENTRIES ||
        offset > (gsize) end || directory_size != (gsize) end - offset)
        return invalid (error, "Multi-volume/ZIP64 archives, oversized directories and inconsistent ZIP records are not supported.");
    for (guint i = 0; i < count; i++)
    {
        if (offset + 46 > (gsize) end || read32 (bytes + offset) != 0x02014b50)
            return invalid (error, "Invalid ZIP central-directory entry.");
        const guint8 *entry = bytes + offset;
        guint name_length = read16 (entry + 28);
        gsize next = offset + 46 + name_length + read16 (entry + 30) + read16 (entry + 32);
        guint32 local = read32 (entry + 42);
        if (next > (gsize) end || !name_length ||
            memchr (entry + 46, '\0', name_length) || read16 (entry + 34) ||
            (read16 (entry + 8) & 0x41))
            return invalid (error, "Unsafe ZIP directory entry or encrypted archive.");
        g_autofree gchar *path = g_strndup ((const gchar *) entry + 46, name_length);
        if (g_str_has_suffix (path, "/"))
            path[name_length - 1] = '\0';
        guint mode = (read32 (entry + 38) >> 16) & S_IFMT;
        if (!safe_path (path) ||
            (entry[5] == 3 && mode && mode != S_IFREG && mode != S_IFDIR))
            return invalid (error, "Unsafe ZIP: raw path or Unix special-file attributes are not allowed.");
        if ((gsize) local + 30 > (gsize) end || read32 (bytes + local) != 0x04034b50 ||
            read16 (bytes + local + 26) != name_length ||
            (gsize) local + 30 + name_length > (gsize) end ||
            memcmp (bytes + local + 30, entry + 46, name_length) != 0)
            return invalid (error, "ZIP local and central entry names do not match.");
        offset = next;
    }
    return offset == (gsize) end ? TRUE : invalid (error, "Unexpected ZIP central-directory data.");
}

static gboolean
parse_metadata (ExmLocalZip *zip, const gchar *data, gsize length, GError **error)
{
    g_autoptr (JsonParser) parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, data, length, error))
        return FALSE;
    JsonNode *root = json_parser_get_root (parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT (root))
        return invalid (error, "metadata.json must contain a JSON object.");
    JsonObject *object = json_node_get_object (root);
    const gchar *uuid = string_member (object, "uuid");
    const gchar *name = string_member (object, "name");
    const gchar *description = string_member (object, "description");
    if (!uuid || !*uuid || strlen (uuid) > 200 || uuid[0] == '.' ||
        strspn (uuid, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@._-") != strlen (uuid))
        return invalid (error, "metadata.json has an invalid or unsafe extension UUID.");
    if (!name || !*name || !description || !*description)
        return invalid (error, "metadata.json requires nonempty name and description strings.");
    JsonNode *versions = json_object_get_member (object, "shell-version");
    if (!versions || !JSON_NODE_HOLDS_ARRAY (versions))
        return invalid (error, "metadata.json requires a shell-version array.");
    JsonArray *array = json_node_get_array (versions);
    guint count = json_array_get_length (array);
    if (count == 0 || count > 100)
        return invalid (error, "shell-version must contain between 1 and 100 versions.");
    zip->shell_versions = g_new0 (gchar *, count + 1);
    for (guint i = 0; i < count; i++)
    {
        JsonNode *node = json_array_get_element (array, i);
        if (!JSON_NODE_HOLDS_VALUE (node) || json_node_get_value_type (node) != G_TYPE_STRING ||
            !g_regex_match_simple ("^[0-9]+(\\.[0-9]+)*$", json_node_get_string (node), 0, 0))
            return invalid (error, "shell-version entries must be GNOME version strings, for example \"51\".");
        zip->shell_versions[i] = g_strdup (json_node_get_string (node));
    }
    JsonNode *version = json_object_get_member (object, "version");
    if (version)
    {
        if (!JSON_NODE_HOLDS_VALUE (version) || json_node_get_value_type (version) != G_TYPE_INT64 ||
            json_node_get_int (version) < 0)
            return invalid (error, "The optional version field must be a nonnegative integer.");
        zip->version = g_strdup_printf ("%" G_GINT64_FORMAT, json_node_get_int (version));
    }
    zip->uuid = g_strdup (uuid);
    zip->name = g_strdup (name);
    zip->description = g_strdup (description);
    return TRUE;
}

/* AppRun saves the original host environment. Never run a host GNOME tool
 * against the AppImage's libraries, modules or application-only schemas. */
static gchar **
host_environment (void)
{
    gchar **env = g_get_environ ();
    const gchar *names[] = { "PATH", "LD_LIBRARY_PATH", "GI_TYPELIB_PATH", "GIO_EXTRA_MODULES",
        "GIO_MODULE_DIR", "GSETTINGS_SCHEMA_DIR", "XDG_DATA_DIRS", "GTK_PATH",
        "GTK_EXE_PREFIX", "GTK_DATA_PREFIX", "GDK_PIXBUF_MODULE_FILE", "GDK_PIXBUF_MODULEDIR", NULL };
    if (g_getenv ("EXM_APPIMAGE"))
        for (guint i = 0; names[i]; i++)
        {
            g_autofree gchar *key = g_strconcat ("EXM_HOST_", names[i], NULL);
            const gchar *value = g_getenv (key);
            env = value ? g_environ_setenv (env, names[i], value, TRUE)
                        : g_environ_unsetenv (env, names[i]);
        }
    return env;
}

static gboolean
run_host (const gchar * const *argv, const gchar *cache_directory, GError **error)
{
    g_auto (GStrv) env = host_environment ();
    if (cache_directory)
        env = g_environ_setenv (env, "XDG_CACHE_HOME", cache_directory, TRUE);
    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                                       G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_set_environ (launcher, env);
    g_autoptr (GSubprocess) process = g_subprocess_launcher_spawnv (launcher, argv, error);
    if (!process)
        return FALSE;
    g_autofree gchar *out = NULL;
    g_autofree gchar *err = NULL;
    if (!g_subprocess_communicate_utf8 (process, NULL, NULL, &out, &err, error))
        return FALSE;
    if (!g_subprocess_get_successful (process))
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s failed: %s%s%s", argv[0],
                     err && *err ? err : "No diagnostic was returned.",
                     out && *out ? "\n" : "", out ? out : "");
        return FALSE;
    }
    return TRUE;
}

ExmLocalZip *
exm_local_zip_inspect (const gchar *filename, GError **error)
{
    g_autoptr (ExmLocalZip) zip = g_new0 (ExmLocalZip, 1);
    zip->directory = g_dir_make_tmp ("extension-manager-local-XXXXXX", error);
    if (!zip->directory)
        return NULL;
    zip->bundle = g_build_filename (zip->directory, "extension.zip", NULL);
    /* A bounded streaming copy avoids a memory spike and TOCTOU between
     * confirmation and installation. Reject non-regular inputs (FIFOs/devices). */
    int fd = g_open (filename, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);
    if (fd < 0)
    {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "Cannot open ZIP file: %s", g_strerror (errno));
        return NULL;
    }
    GStatBuf st;
    if (fstat (fd, &st) != 0 || !S_ISREG (st.st_mode) || st.st_size > MAX_BUNDLE)
    {
        close (fd);
        invalid (error, "Choose a regular local ZIP file no larger than 128 MiB.");
        return NULL;
    }
    g_autoptr (GInputStream) input = g_unix_input_stream_new (fd, TRUE);
    g_autoptr (GFile) destination = g_file_new_for_path (zip->bundle);
    g_autoptr (GFileOutputStream) output = g_file_create (destination, G_FILE_CREATE_PRIVATE, NULL, error);
    if (!output)
        return NULL;
    gchar buffer[65536];
    gssize size;
    gsize compressed = 0;
    while ((size = g_input_stream_read (G_INPUT_STREAM (input), buffer, sizeof buffer, NULL, error)) > 0)
    {
        compressed += size;
        if (compressed > MAX_BUNDLE)
        {
            invalid (error, "ZIP file exceeds the 128 MiB limit.");
            return NULL;
        }
        if (!g_output_stream_write_all (G_OUTPUT_STREAM (output), buffer, size, NULL, NULL, error))
            return NULL;
    }
    if (size < 0 || !g_output_stream_close (G_OUTPUT_STREAM (output), NULL, error))
        return NULL;
    if (!validate_zip_directory (zip->bundle, error))
        return NULL;

    struct archive *archive = archive_read_new ();
    archive_read_support_format_zip (archive);
    gboolean valid = FALSE;
    gboolean has_extension = FALSE;
    g_autoptr (GByteArray) metadata = g_byte_array_new ();
    g_autoptr (GHashTable) entries = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    guint count = 0;
    gsize expanded = 0;
    gboolean has_schemas = FALSE;
    struct archive_entry *entry;
    if (archive_read_open_filename (archive, zip->bundle, sizeof buffer) != ARCHIVE_OK)
        goto archive_error;
    int status;
    while ((status = archive_read_next_header (archive, &entry)) == ARCHIVE_OK)
    {
        g_autofree gchar *path = g_strdup (archive_entry_pathname (entry));
        mode_t type = archive_entry_filetype (entry);
        if (path && type == AE_IFDIR && g_str_has_suffix (path, "/"))
            path[strlen (path) - 1] = '\0';
        if (++count > MAX_ENTRIES || !safe_path (path) ||
            (type != AE_IFREG && type != AE_IFDIR) || archive_entry_symlink (entry) ||
            archive_entry_hardlink (entry) || archive_entry_is_encrypted (entry) ||
            g_hash_table_contains (entries, path))
        {
            invalid (error, "Unsafe ZIP: invalid/duplicate path, link, special file, encryption, or too many entries.");
            goto done;
        }
        g_hash_table_insert (entries, g_strdup (path), GINT_TO_POINTER (type));
        gboolean is_metadata = g_str_equal (path, "metadata.json") && type == AE_IFREG;
        has_extension |= g_str_equal (path, "extension.js") && type == AE_IFREG;
        g_autoptr (GByteArray) schema = g_byte_array_new ();
        gboolean is_schema = type == AE_IFREG && g_str_has_prefix (path, "schemas/");
        while ((size = archive_read_data (archive, buffer, sizeof buffer)) > 0)
        {
            expanded += size;
            if (expanded > MAX_EXPANDED || (is_metadata && metadata->len + size > MAX_METADATA))
            {
                invalid (error, "ZIP exceeds the expanded-size (256 MiB) or metadata-size (1 MiB) limit.");
                goto done;
            }
            if (is_metadata)
                g_byte_array_append (metadata, (guint8 *) buffer, size);
            if (is_schema)
                g_byte_array_append (schema, (guint8 *) buffer, size);
        }
        if (size < 0)
            goto archive_error;
        if (is_schema)
        {
            g_autofree gchar *target = g_build_filename (zip->directory, path, NULL);
            g_autofree gchar *parent = g_path_get_dirname (target);
            if (g_mkdir_with_parents (parent, 0700) != 0 ||
                !g_file_set_contents (target, (gchar *) schema->data, schema->len, error))
            {
                if (!error || !*error)
                    invalid (error, "Cannot stage extension schemas for validation.");
                goto done;
            }
            has_schemas = TRUE;
        }
    }
    if (status != ARCHIVE_EOF)
        goto archive_error;
    /* A regular file must never also be a directory ancestor. */
    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init (&iter, entries);
    while (g_hash_table_iter_next (&iter, &key, NULL))
    {
        g_autofree gchar *parent = g_path_get_dirname (key);
        while (!g_str_equal (parent, "."))
        {
            if (GPOINTER_TO_INT (g_hash_table_lookup (entries, parent)) == AE_IFREG)
            {
                invalid (error, "ZIP contains conflicting file and directory paths.");
                goto done;
            }
            gchar *next = g_path_get_dirname (parent);
            g_free (parent);
            parent = next;
        }
    }
    if (!has_extension || metadata->len == 0)
    {
        invalid (error, "The ZIP must contain metadata.json and extension.js directly at its root, not inside a wrapper folder.");
        goto done;
    }
    if (memchr (metadata->data, '\0', metadata->len))
    {
        invalid (error, "metadata.json must not contain NUL bytes.");
        goto done;
    }
    if (!parse_metadata (zip, (gchar *) metadata->data, metadata->len, error))
        goto done;
    zip->metadata_sha256 = g_compute_checksum_for_data (G_CHECKSUM_SHA256, metadata->data, metadata->len);
    if (has_schemas)
    {
        g_autofree gchar *schemas = g_build_filename (zip->directory, "schemas", NULL);
        const gchar *argv[] = { "/usr/bin/glib-compile-schemas", "--strict", "--dry-run", schemas, NULL };
        if (!run_host (argv, NULL, error))
            goto done;
    }
    valid = TRUE;
    goto done;
archive_error:
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Cannot read ZIP archive: %s",
                 archive_error_string (archive) ? archive_error_string (archive) : "invalid ZIP data");
done:
    archive_read_free (archive);
    return valid ? g_steal_pointer (&zip) : NULL;
}

gboolean
exm_local_zip_supports (ExmLocalZip *zip, const gchar *shell_version)
{
    if (!shell_version)
        return FALSE;
    g_auto (GStrv) current = g_strsplit (shell_version, ".", -1);
    for (guint i = 0; zip->shell_versions[i]; i++)
    {
        g_auto (GStrv) supported = g_strsplit (zip->shell_versions[i], ".", -1);
        if (g_str_equal (supported[0], current[0]) &&
            (g_ascii_strtoull (current[0], NULL, 10) >= 40 ||
             g_strcmp0 (supported[1], current[1]) == 0))
            return TRUE;
    }
    return FALSE;
}

gboolean
exm_local_zip_is_installed (ExmLocalZip *zip)
{
    g_autofree gchar *path = g_build_filename (g_get_user_data_dir (), "gnome-shell", "extensions", zip->uuid, NULL);
    return g_file_test (path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK);
}

gboolean
exm_local_zip_install (ExmLocalZip *zip, gboolean replace, GError **error)
{
    g_autofree gchar *base = g_build_filename (g_get_user_data_dir (), "gnome-shell", "extensions", NULL);
    g_autofree gchar *target = g_build_filename (base, zip->uuid, NULL);
    GStatBuf st;
    if (g_file_test (base, G_FILE_TEST_IS_SYMLINK) ||
        (g_lstat (target, &st) == 0 && !S_ISDIR (st.st_mode)))
        return invalid (error, "Refusing to install into a symlink or non-directory extension destination.");
    const gchar *argv[] = { "/usr/bin/gnome-extensions", "install", replace ? "--force" : zip->bundle,
                           replace ? zip->bundle : NULL, NULL };
    /* GNOME moves its extracted directory from XDG_CACHE_HOME into the data
     * directory. GFile cannot recursively move across filesystems. Give just
     * this child a private cache on the destination filesystem, including when
     * the user's normal cache is on tmpfs or another mount. */
    if (g_mkdir_with_parents (g_get_user_data_dir (), 0700) != 0)
    {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "Cannot create user data directory: %s", g_strerror (errno));
        return FALSE;
    }
    g_autofree gchar *cache = g_build_filename (g_get_user_data_dir (), ".extension-manager-local-XXXXXX", NULL);
    if (!g_mkdtemp_full (cache, 0700))
    {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "Cannot create installation cache: %s", g_strerror (errno));
        return FALSE;
    }
    gboolean installed = run_host (argv, cache, error);
    remove_private_tree (cache);
    if (!installed)
        return FALSE;
    /* Exit status zero is GNOME's installation confirmation. Also verify that
     * the installed metadata identifies the validated UUID. No enable,
     * disable, uninstall or dconf reset is performed during replacement. */
    g_autofree gchar *path = g_build_filename (g_get_user_data_dir (), "gnome-shell", "extensions",
                                              zip->uuid, "metadata.json", NULL);
    g_autofree gchar *installed_metadata = NULL;
    gsize length = 0;
    if (g_stat (path, &st) != 0 || st.st_size > MAX_METADATA)
        return invalid (error, "GNOME returned success, but the installed metadata is missing or oversized.");
    if (!g_file_get_contents (path, &installed_metadata, &length, error))
        return FALSE;
    g_autofree gchar *digest = g_compute_checksum_for_data (G_CHECKSUM_SHA256, (guint8 *) installed_metadata, length);
    if (!g_str_equal (digest, zip->metadata_sha256))
        return invalid (error, "GNOME returned success, but the installed metadata does not match the validated ZIP. Another process may have changed the installation.");
    return TRUE;
}
