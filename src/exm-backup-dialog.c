/* Copyright 2026 Extension Manager Local contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "exm-backup-dialog.h"
#include "local/exm-backup.h"
#include "web/exm-data-provider.h"
#include <glib/gi18n.h>
#include <glib/gstdio.h>

typedef struct {
    JsonObject *entry;
    gchar *directory;
    gchar *settings_summary;
    GtkCheckButton *check;
    AdwActionRow *widget;
    gboolean compatible;
    gboolean known;
    gboolean installed;
    gboolean selected;
} BackupRow;

typedef struct {
    GtkWindow *window;
    GApplication *application;
    ExmManager *manager;
    ExmDataProvider *provider;
    ExmBackup *backup;
    GPtrArray *rows;
    gchar *shell;
    GtkBox *content;
    GtkWidget *options;
    AdwPreferencesGroup *list;
    GtkCheckButton *settings, *states, *extensions;
    GtkDropDown *mode;
    GtkButton *start, *cancel;
    GtkProgressBar *progress;
    GtkLabel *report;
    GString *results;
    GFile *file;
    gboolean importing, busy, stop, full, do_settings, do_states, do_extensions;
    guint current;
    guint pulse;
} BackupDialog;

static void next_restore (BackupDialog *data);

static void
row_free (BackupRow *row)
{
    json_object_unref (row->entry);
    g_free (row->directory);
    g_free (row->settings_summary);
    g_free (row);
}

static void
dialog_free (BackupDialog *data)
{
    if (data->pulse) g_source_remove (data->pulse);
    g_object_unref (data->manager);
    g_object_unref (data->provider);
    exm_backup_free (data->backup);
    g_ptr_array_unref (data->rows);
    g_clear_object (&data->file);
    g_free (data->shell);
    g_string_free (data->results, TRUE);
    g_application_release (data->application);
    g_object_unref (data->application);
    g_free (data);
}

static gboolean
pulse_progress (gpointer user_data)
{
    BackupDialog *data = user_data;
    gtk_progress_bar_pulse (data->progress);
    return G_SOURCE_CONTINUE;
}

static void
set_busy (BackupDialog *data, gboolean busy)
{
    if (data->busy == busy) return;
    data->busy = busy;
    if (busy && !data->importing) data->pulse = g_timeout_add (150, pulse_progress, data);
    else if (!busy && data->pulse) { g_source_remove (data->pulse); data->pulse = 0; }
    gtk_widget_set_sensitive (data->options, !busy);
    gtk_widget_set_sensitive (GTK_WIDGET (data->list), !busy);
    gtk_widget_set_sensitive (GTK_WIDGET (data->start), !busy);
    gtk_widget_set_visible (GTK_WIDGET (data->cancel), busy);
    GApplication *app = g_application_get_default ();
    GAction *quit = g_action_map_lookup_action (G_ACTION_MAP (app), "quit");
    guint count = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (app), "backup-operations"));
    count = busy ? count + 1 : count - 1;
    g_object_set_data (G_OBJECT (app), "backup-operations", GUINT_TO_POINTER (count));
    if (quit) g_simple_action_set_enabled (G_SIMPLE_ACTION (quit), count == 0);
    if (busy) g_application_hold (app);
    else g_application_release (app);
}

static void
append_report (BackupDialog *data, const gchar *text)
{
    g_string_append_printf (data->results, "%s\n", text);
    gtk_label_set_text (data->report, data->results->str);
}

static gboolean
close_requested (GtkWindow *window G_GNUC_UNUSED, BackupDialog *data)
{
    if (data->busy) {
        data->stop = TRUE;
        gtk_button_set_label (data->cancel, _("Stopping after current operation…"));
        return TRUE;
    }
    return FALSE;
}

static void
cancel_clicked (GtkButton *button G_GNUC_UNUSED, BackupDialog *data)
{
    data->stop = TRUE;
    gtk_button_set_label (data->cancel, _("Stopping after current operation…"));
}

static BackupRow *
add_row (BackupDialog *data, JsonObject *entry, const gchar *directory)
{
    BackupRow *row = g_new0 (BackupRow, 1);
    row->entry = json_object_ref (entry);
    row->directory = g_strdup (directory);
    row->check = GTK_CHECK_BUTTON (gtk_check_button_new ());
    gtk_check_button_set_active (row->check, TRUE);
    row->widget = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row->widget), FALSE);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row->widget), exm_backup_string (entry, "name"));
    adw_action_row_set_subtitle (row->widget, exm_backup_string (entry, "uuid"));
    adw_action_row_add_prefix (row->widget, GTK_WIDGET (row->check));
    adw_action_row_set_activatable_widget (row->widget, GTK_WIDGET (row->check));
    adw_preferences_group_add (data->list, GTK_WIDGET (row->widget));
    g_ptr_array_add (data->rows, row);
    return row;
}

static void
select_all (GtkButton *button, BackupDialog *data)
{
    gboolean active = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "select"));
    for (guint i = 0; i < data->rows->len; i++)
        gtk_check_button_set_active (((BackupRow *) g_ptr_array_index (data->rows, i))->check, active);
}

static void
preview_row (BackupDialog *data G_GNUC_UNUSED, BackupRow *row)
{
    JsonArray *settings = json_object_has_member (row->entry, "settings") ? json_object_get_array_member (row->entry, "settings") : NULL;
    g_autofree gchar *subtitle = g_strdup_printf (_("%s\n%s · %s\n%s"),
        exm_backup_string (row->entry, "uuid"), row->installed ? _("Installed (kept, not replaced)") : _("Missing"),
        row->known ? (row->compatible ? _("Compatible") : _("Incompatible")) : _("Compatibility unknown — installation skipped"),
        settings && json_array_get_length (settings) ?
            (row->installed ? (row->settings_summary ? row->settings_summary : _("Checking current schemas…")) : _("Settings available; requires successful installation and current schemas")) :
            _("No supported settings available"));
    adw_action_row_set_subtitle (row->widget, subtitle);
}

static void check_next (BackupDialog *data);

static void
compatibility_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
    BackupDialog *data = user_data;
    BackupRow *row = g_ptr_array_index (data->rows, data->current);
    g_autoptr (GError) error = NULL;
    g_autoptr (ExmSearchResult) extension = exm_data_provider_get_finish (EXM_DATA_PROVIDER (source), result, &error);
    row->known = extension != NULL;
    row->compatible = extension && exm_search_result_supports_shell_version (extension, data->shell);
    preview_row (data, row);
    data->current++;
    check_next (data);
    g_object_unref (data->window);
}

static void
check_next (BackupDialog *data)
{
    while (!data->stop && data->current < data->rows->len) {
        BackupRow *row = g_ptr_array_index (data->rows, data->current);
        gtk_progress_bar_set_fraction (data->progress, (double) data->current / MAX (1, data->rows->len));
        if (row->installed || json_object_has_member (row->entry, "_compatible")) {
            if (!row->installed) {
                row->known = TRUE;
                row->compatible = json_object_get_boolean_member (row->entry, "_compatible");
            }
            preview_row (data, row);
            data->current++;
        } else {
            exm_data_provider_get_async (data->provider, exm_backup_string (row->entry, "uuid"), NULL,
                compatibility_done, data);
            g_object_ref (data->window);
            return;
        }
    }
    gtk_progress_bar_set_fraction (data->progress, 1);
    append_report (data, _("Review the selection. Existing extension files are never replaced. Settings restoration requires confirmation and may skip incompatible keys. Only install backup code you trust."));
    set_busy (data, FALSE);
}

static void
read_worker (GTask *task, gpointer source G_GNUC_UNUSED, gpointer task_data, GCancellable *cancel G_GNUC_UNUSED)
{
    BackupDialog *data = task_data;
    GError *error = NULL;
    ExmBackup *backup = exm_backup_read (data->file, &error);
    if (!backup) { g_task_return_error (task, error); return; }
    JsonArray *entries = json_object_get_array_member (backup->manifest, "extensions");
    for (guint i = 0; i < json_array_get_length (entries); i++) {
        JsonObject *entry = json_array_get_object_element (entries, i);
        /* Do not accept internal preview annotations from untrusted input. */
        json_object_remove_member (entry, "_compatible");
        if (g_hash_table_contains (backup->bundles, exm_backup_string (entry, "uuid"))) {
            g_autoptr (ExmLocalZip) zip = exm_backup_inspect_bundle (backup, exm_backup_string (entry, "uuid"), &error);
            if (!zip) { exm_backup_free (backup); g_task_return_error (task, error); return; }
            json_object_set_boolean_member (entry, "_compatible", exm_local_zip_supports (zip, data->shell));
        }
    }
    g_task_return_pointer (task, backup, (GDestroyNotify) exm_backup_free);
}

static void
preview_worker (GTask *task, gpointer source G_GNUC_UNUSED, gpointer task_data, GCancellable *cancel G_GNUC_UNUSED)
{
    BackupDialog *data = task_data;
    for (guint i = 0; i < data->rows->len; i++) {
        BackupRow *row = g_ptr_array_index (data->rows, i);
        if (!row->installed) {
            g_autofree gchar *directory = g_build_filename (g_get_user_data_dir (), "gnome-shell", "extensions",
                exm_backup_string (row->entry, "uuid"), NULL);
            GStatBuf st;
            if (g_lstat (directory, &st) == 0) {
                row->installed = TRUE;
                row->directory = g_steal_pointer (&directory);
            }
        }
        if (row->installed) row->compatible = exm_backup_directory_supports (row->directory,
            exm_backup_string (row->entry, "uuid"), data->shell, &row->known);
        if (row->installed) {
            row->settings_summary = exm_backup_check_settings (row->entry, row->directory);
            gchar *newline = strchr (row->settings_summary, '\n');
            if (newline) *newline = '\0';
        }
    }
    g_task_return_boolean (task, TRUE);
}

static void
preview_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer user_data)
{
    g_task_propagate_boolean (G_TASK (result), NULL);
    check_next (user_data);
}

static void
read_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer user_data)
{
    BackupDialog *data = user_data;
    g_autoptr (GError) error = NULL;
    data->backup = g_task_propagate_pointer (G_TASK (result), &error);
    if (!data->backup) {
        append_report (data, error->message);
        set_busy (data, FALSE);
        gtk_widget_set_sensitive (GTK_WIDGET (data->start), FALSE);
        return;
    }
    JsonArray *entries = json_object_get_array_member (data->backup->manifest, "extensions");
    for (guint i = 0; i < json_array_get_length (entries); i++) {
        JsonObject *entry = json_array_get_object_element (entries, i);
        const gchar *uuid = exm_backup_string (entry, "uuid");
        g_autoptr (ExmExtension) installed = exm_manager_get_by_uuid (data->manager, uuid);
        g_autofree gchar *path = NULL;
        if (installed) g_object_get (installed, "path", &path, NULL);
        BackupRow *row = add_row (data, entry, path);
        row->installed = installed != NULL;
        preview_row (data, row);
    }
    data->current = 0;
    g_autoptr (GTask) task = g_task_new (data->window, NULL, preview_done, data);
    g_task_set_task_data (task, data, NULL);
    g_task_run_in_thread (task, preview_worker);
}

static void
export_worker (GTask *task, gpointer source G_GNUC_UNUSED, gpointer task_data, GCancellable *cancel G_GNUC_UNUSED)
{
    BackupDialog *data = task_data;
    g_autoptr (ExmBackup) backup = exm_backup_new (data->shell);
    GString *report = g_string_new (NULL);
    guint exported = 0;
    for (guint i = 0; i < data->rows->len; i++) {
        BackupRow *row = g_ptr_array_index (data->rows, i);
        if (!row->selected) continue;
        g_autoptr (GError) error = NULL;
        if (!data->do_states) json_object_remove_member (row->entry, "enabled");
        if (!exm_backup_add (backup, row->entry, row->directory, data->do_settings, data->full, &error))
            g_string_append_printf (report, "%s: FAILED: %s\n", exm_backup_string (row->entry, "name"), error->message);
        else {
            exported++;
            JsonArray *settings = json_object_get_array_member (row->entry, "settings");
            g_string_append_printf (report, "%s: backed up; %u schema records, %" G_GINT64_FORMAT " filtered/unsupported keys or paths. External configuration not included.\n",
                exm_backup_string (row->entry, "name"), json_array_get_length (settings), json_object_get_int_member (row->entry, "settings-skipped"));
        }
    }
    g_autoptr (GError) error = NULL;
    if (!exported || !exm_backup_write (backup, data->file, data->full, &error)) {
        g_string_append_printf (report, "Backup NOT saved: %s", error ? error->message : "no extensions exported");
    } else g_string_append_printf (report, "Saved %u extensions. Treat this backup as potentially sensitive. Store it securely.\n", exported);
    g_task_return_pointer (task, g_string_free (report, FALSE), g_free);
}

static void
export_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer user_data)
{
    BackupDialog *data = user_data;
    g_autofree gchar *report = g_task_propagate_pointer (G_TASK (result), NULL);
    append_report (data, report);
    gtk_progress_bar_set_fraction (data->progress, 1);
    set_busy (data, FALSE);
    /* A new export starts from a fresh discovery snapshot. */
    gtk_widget_set_sensitive (GTK_WIDGET (data->start), FALSE);
}

static void
file_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
    BackupDialog *data = user_data;
    g_autoptr (GError) error = NULL;
    data->file = data->importing ? gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error) :
        gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);
    if (!data->file) {
        if (!g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
            !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED)) append_report (data, error->message);
        set_busy (data, FALSE);
        if (data->importing) gtk_widget_set_sensitive (GTK_WIDGET (data->start), FALSE);
    } else {
        g_autoptr (GTask) task = g_task_new (data->window, NULL, data->importing ? read_done : export_done, data);
        g_task_set_task_data (task, data, NULL);
        g_task_run_in_thread (task, data->importing ? read_worker : export_worker);
    }
    g_object_unref (data->window);
}

static void
choose_file (BackupDialog *data)
{
    data->stop = FALSE;
    set_busy (data, TRUE);
    g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, data->importing ? _("Import Extensions") : _("Export Extensions"));
    if (data->importing) gtk_file_dialog_open (dialog, data->window, NULL, file_chosen, data);
    else {
        gtk_file_dialog_set_initial_name (dialog, data->full ? "extensions-backup.zip" : "extensions-backup.json");
        gtk_file_dialog_save (dialog, data->window, NULL, file_chosen, data);
    }
    g_object_ref (data->window);
}

static void
restore_worker (GTask *task, gpointer source G_GNUC_UNUSED, gpointer task_data, GCancellable *cancel G_GNUC_UNUSED)
{
    BackupDialog *data = task_data;
    BackupRow *row = g_ptr_array_index (data->rows, data->current);
    const gchar *uuid = exm_backup_string (row->entry, "uuid");
    GString *report = g_string_new (NULL);
    gboolean installed = row->installed;
    if (!installed && data->do_extensions && g_hash_table_contains (data->backup->bundles, uuid)) {
        g_autoptr (GError) error = NULL;
        g_autoptr (ExmLocalZip) zip = exm_backup_inspect_bundle (data->backup, uuid, &error);
        installed = zip && exm_local_zip_supports (zip, data->shell) && exm_local_zip_install (zip, FALSE, &error);
        if (!installed) g_string_append_printf (report, "Installation failed: %s\n", error ? error->message : "incompatible GNOME version");
        else g_string_append (report, "Bundled extension installed. Log out and back in to load its code.\n");
    }
    if (installed && !row->directory)
        row->directory = g_build_filename (g_get_user_data_dir (), "gnome-shell", "extensions", uuid, NULL);
    if (installed) {
        gboolean known = FALSE;
        if (!exm_backup_directory_supports (row->directory, uuid, data->shell, &known) || !known) {
            installed = FALSE;
            g_string_append (report, "Settings and state skipped: current installed metadata is missing, changed or incompatible.\n");
        }
    }
    if (data->do_settings) {
        if (installed && row->directory) {
            g_autofree gchar *settings = exm_backup_restore_settings (row->entry, row->directory);
            g_string_append_printf (report, "%s\n", settings);
        } else g_string_append (report, "Settings skipped: extension installation unavailable.\n");
    }
    row->installed = installed;
    g_task_return_pointer (task, g_string_free (report, FALSE), g_free);
}

static void
finish_row (BackupDialog *data)
{
    data->current++;
    next_restore (data);
}

static void
state_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
    BackupDialog *data = user_data;
    g_autoptr (GError) error = NULL;
    if (exm_manager_set_enabled_finish (EXM_MANAGER (source), result, &error)) append_report (data, _("Enabled/disabled state restored."));
    else {
        g_autofree gchar *text = g_strdup_printf (_("State not restored: %s. Log out/in, then import again with only states selected."), error->message);
        append_report (data, text);
    }
    finish_row (data);
    g_object_unref (data->window);
}

static void
restore_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer user_data)
{
    BackupDialog *data = user_data;
    BackupRow *row = g_ptr_array_index (data->rows, data->current);
    g_autofree gchar *report = g_task_propagate_pointer (G_TASK (result), NULL);
    append_report (data, report);
    if (row->installed) exm_manager_refresh_local (data->manager, exm_backup_string (row->entry, "uuid"));
    if (row->installed && data->do_states && json_object_has_member (row->entry, "enabled")) {
        g_object_ref (data->window);
        exm_manager_set_enabled_async (data->manager, exm_backup_string (row->entry, "uuid"),
            json_object_get_boolean_member (row->entry, "enabled"), NULL, state_done, data);
    } else {
        if (data->do_states) append_report (data, _("State skipped: no saved state or extension not installed."));
        finish_row (data);
    }
}

static void
online_installed (GObject *source, GAsyncResult *result, gpointer user_data)
{
    BackupDialog *data = user_data;
    BackupRow *row = g_ptr_array_index (data->rows, data->current);
    g_autoptr (GError) error = NULL;
    row->installed = exm_manager_install_finish (source, result, &error);
    append_report (data, row->installed ? _("Online extension installed.") : (error ? error->message : _("Installation cancelled or failed.")));
    if (row->installed) {
        g_autoptr (ExmExtension) extension = exm_manager_get_by_uuid (data->manager, exm_backup_string (row->entry, "uuid"));
        if (extension) g_object_get (extension, "path", &row->directory, NULL);
    }
    g_autoptr (GTask) task = g_task_new (data->window, NULL, restore_done, data);
    g_task_set_task_data (task, data, NULL);
    g_task_run_in_thread (task, restore_worker);
    g_object_unref (data->window);
}

static void
next_restore (BackupDialog *data)
{
    while (!data->stop && data->current < data->rows->len) {
        BackupRow *row = g_ptr_array_index (data->rows, data->current);
        gtk_progress_bar_set_fraction (data->progress, (double) data->current / MAX (1, data->rows->len));
        if (!row->selected) { data->current++; continue; }
        append_report (data, exm_backup_string (row->entry, "name"));
        if (!row->known || !row->compatible) {
            append_report (data, _("Skipped: incompatible or compatibility could not be verified."));
            data->current++; continue;
        }
        if (!row->installed && data->do_extensions && !g_hash_table_contains (data->backup->bundles, exm_backup_string (row->entry, "uuid"))) {
            g_object_ref (data->window);
            exm_manager_install_async (data->manager, exm_backup_string (row->entry, "uuid"), NULL, online_installed, data);
        } else {
            if (row->installed && data->do_extensions) append_report (data, _("Existing installation kept; files were not overwritten."));
            g_autoptr (GTask) task = g_task_new (data->window, NULL, restore_done, data);
            g_task_set_task_data (task, data, NULL);
            g_task_run_in_thread (task, restore_worker);
        }
        return;
    }
    append_report (data, data->stop ? _("Stopped. Completed operations were retained; remaining extensions were skipped.") : _("Restore complete. Review failures and skipped settings above. Log out and back in before using newly installed code. GNOME’s global extensions switch is not changed."));
    gtk_progress_bar_set_fraction (data->progress, 1);
    set_busy (data, FALSE);
    gtk_widget_set_sensitive (GTK_WIDGET (data->start), FALSE);
}

static void
confirmed (GObject *source, GAsyncResult *result, gpointer user_data)
{
    BackupDialog *data = user_data;
    const gchar *response = adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);
    if (g_str_equal (response, "restore")) {
        data->current = 0;
        data->stop = FALSE;
        next_restore (data);
    } else set_busy (data, FALSE);
    g_object_unref (data->window);
}

static void
start_clicked (GtkButton *button G_GNUC_UNUSED, BackupDialog *data)
{
    guint selected = 0;
    for (guint i = 0; i < data->rows->len; i++) {
        BackupRow *row = g_ptr_array_index (data->rows, i);
        row->selected = gtk_check_button_get_active (row->check);
        selected += row->selected;
    }
    if (!selected) { append_report (data, _("Select at least one extension.")); return; }
    data->do_settings = gtk_check_button_get_active (data->settings);
    data->do_states = gtk_check_button_get_active (data->states);
    data->do_extensions = !data->importing || gtk_check_button_get_active (data->extensions);
    if (data->importing && !data->do_settings && !data->do_states && !data->do_extensions) {
        append_report (data, _("Choose at least one restore option."));
        return;
    }
    if (!data->importing) {
        data->full = gtk_drop_down_get_selected (data->mode) == 1;
        choose_file (data);
        return;
    }
    set_busy (data, TRUE);
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (_("Restore Selected Extensions?"),
        data->do_settings ? _("This will overwrite supported settings of selected extensions, including existing preferences. Unsupported settings are skipped. Existing extension files will be kept. Only proceed if you trust the backup and its bundled code.") :
        _("Restore the selected extensions and requested states? Existing extension files will be kept. Only proceed if you trust the backup and its bundled code.")));
    adw_alert_dialog_add_responses (dialog, "cancel", _("Cancel"), "restore", _("Restore"), NULL);
    adw_alert_dialog_set_response_appearance (dialog, "restore", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response (dialog, "cancel");
    adw_alert_dialog_set_close_response (dialog, "cancel");
    g_object_ref (data->window);
    adw_alert_dialog_choose (dialog, GTK_WIDGET (data->window), NULL, confirmed, data);
}

static GtkCheckButton *
option (GtkBox *box, const gchar *label)
{
    GtkCheckButton *check = GTK_CHECK_BUTTON (gtk_check_button_new_with_label (label));
    gtk_check_button_set_active (check, TRUE);
    gtk_box_append (box, GTK_WIDGET (check));
    return check;
}

void
exm_backup_dialog_present (GtkWindow *parent, ExmManager *manager, gboolean importing)
{
    BackupDialog *data = g_new0 (BackupDialog, 1);
    data->application = g_object_ref (g_application_get_default ());
    g_application_hold (data->application);
    data->manager = g_object_ref (manager);
    data->provider = exm_data_provider_new ();
    data->rows = g_ptr_array_new_with_free_func ((GDestroyNotify) row_free);
    data->results = g_string_new (NULL);
    data->importing = importing;
    g_object_get (manager, "shell-version", &data->shell, NULL);
    data->window = GTK_WINDOW (adw_window_new ());
    gtk_window_set_title (data->window, _("Backup and Restore"));
    gtk_window_set_default_size (data->window, 700, 720);
    gtk_window_set_transient_for (data->window, parent);
    gtk_window_set_modal (data->window, TRUE);
    g_object_set_data_full (G_OBJECT (data->window), "backup-dialog", data, (GDestroyNotify) dialog_free);
    g_signal_connect (data->window, "close-request", G_CALLBACK (close_requested), data);
    AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
    AdwHeaderBar *header = ADW_HEADER_BAR (adw_header_bar_new ());
    adw_header_bar_set_title_widget (header, adw_window_title_new (importing ? _("Import Extensions") : _("Export Extensions"), _("Backup and Restore")));
    adw_toolbar_view_add_top_bar (toolbar, GTK_WIDGET (header));
    GtkWidget *scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    data->content = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 16));
    gtk_widget_set_margin_start (GTK_WIDGET (data->content), 24);
    gtk_widget_set_margin_end (GTK_WIDGET (data->content), 24);
    gtk_widget_set_margin_top (GTK_WIDGET (data->content), 16);
    gtk_widget_set_margin_bottom (GTK_WIDGET (data->content), 24);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), GTK_WIDGET (data->content));
    adw_toolbar_view_set_content (toolbar, scroll);
    adw_window_set_content (ADW_WINDOW (data->window), GTK_WIDGET (toolbar));
    GtkBox *options = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 10));
    data->options = GTK_WIDGET (options);
    gtk_box_append (data->content, data->options);
    if (importing) data->extensions = option (options, _("Restore extensions"));
    else {
        const gchar *modes[] = { _("List Backup (JSON)"), _("Full Backup (ZIP with extension code)"), NULL };
        data->mode = GTK_DROP_DOWN (gtk_drop_down_new_from_strings (modes));
        gtk_widget_set_tooltip_text (GTK_WIDGET (data->mode), _("Full Backup includes user-installed code, never system files. Code may contain embedded sensitive data; inspect custom extensions before exporting."));
        gtk_box_append (options, GTK_WIDGET (data->mode));
    }
    data->settings = option (options, importing ? _("Restore extension settings") : _("Include extension settings"));
    data->states = option (options, importing ? _("Restore enabled/disabled states") : _("Include enabled/disabled states"));
    GtkWidget *warning = gtk_label_new (_("Unknown configuration files, arbitrary text and potentially sensitive settings are not included. Full backups include extension code/assets: inspect custom code for embedded credentials. Treat backups as potentially sensitive and keep them secure."));
    gtk_label_set_wrap (GTK_LABEL (warning), TRUE);
    gtk_box_append (options, warning);
    GtkBox *selection = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12));
    for (guint i = 0; i < 2; i++) {
        GtkWidget *button = gtk_button_new_with_label (i ? _("Deselect All") : _("Select All"));
        g_object_set_data (G_OBJECT (button), "select", GINT_TO_POINTER (!i));
        g_signal_connect (button, "clicked", G_CALLBACK (select_all), data);
        gtk_box_append (selection, button);
    }
    gtk_box_append (options, GTK_WIDGET (selection));
    data->list = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (data->list, _("Extensions"));
    gtk_box_append (data->content, GTK_WIDGET (data->list));
    data->progress = GTK_PROGRESS_BAR (gtk_progress_bar_new ());
    gtk_box_append (data->content, GTK_WIDGET (data->progress));
    data->start = GTK_BUTTON (gtk_button_new_with_label (importing ? _("Restore Selected") : _("Export")));
    gtk_widget_add_css_class (GTK_WIDGET (data->start), "suggested-action");
    g_signal_connect (data->start, "clicked", G_CALLBACK (start_clicked), data);
    gtk_box_append (data->content, GTK_WIDGET (data->start));
    data->cancel = GTK_BUTTON (gtk_button_new_with_label (_("Stop after current operation")));
    g_signal_connect (data->cancel, "clicked", G_CALLBACK (cancel_clicked), data);
    gtk_widget_set_visible (GTK_WIDGET (data->cancel), FALSE);
    gtk_box_append (data->content, GTK_WIDGET (data->cancel));
    data->report = GTK_LABEL (gtk_label_new (""));
    gtk_label_set_wrap (data->report, TRUE);
    gtk_label_set_selectable (data->report, TRUE);
    gtk_label_set_xalign (data->report, 0);
    gtk_box_append (data->content, GTK_WIDGET (data->report));
    if (!importing) {
        g_autoptr (GListModel) extensions = NULL;
        g_object_get (manager, "extensions", &extensions, NULL);
        for (guint i = 0; extensions && i < g_list_model_get_n_items (extensions); i++) {
            g_autoptr (ExmExtension) extension = g_list_model_get_item (extensions, i);
            g_autofree gchar *uuid = NULL, *name = NULL, *path = NULL, *version = NULL;
            gboolean enabled = FALSE, user = FALSE, local = FALSE;
            g_object_get (extension, "uuid", &uuid, "name", &name, "path", &path, "version", &version, "enabled", &enabled, "is-user", &user, "local-pending", &local, NULL);
            if (!exm_backup_uuid_valid (uuid)) continue;
            g_autoptr (JsonObject) entry = json_object_new ();
            json_object_set_string_member (entry, "uuid", uuid);
            json_object_set_string_member (entry, "name", name ? name : uuid);
            if (version) json_object_set_string_member (entry, "extension-version", version);
            json_object_set_boolean_member (entry, "enabled", enabled);
            /* GNOME does not persist a reliable local-vs-EGO provenance flag. */
            json_object_set_string_member (entry, "source", user ? (local ? "local" : "user-unknown") : "system");
            add_row (data, entry, path);
        }
        if (!data->rows->len) append_report (data, _("No extensions discovered. Wait for GNOME Shell, then reopen this screen."));
    }
    gtk_window_present (data->window);
    if (importing) choose_file (data);
}
