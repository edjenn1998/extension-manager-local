/* Copyright 2026 Extension Manager Local contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "exm-local-install.h"
#include "local/exm-local-zip.h"
#include <adwaita.h>
#include <glib/gi18n.h>

typedef struct
{
    GtkWindow *parent;
    ExmManager *manager;
    GApplication *application;
    ExmLocalZip *zip;
    gchar *filename;
    gboolean replace;
} LocalInstall;

static void
finish (LocalInstall *data)
{
    GAction *quit = g_action_map_lookup_action (G_ACTION_MAP (data->application), "quit");
    if (quit)
        g_simple_action_set_enabled (G_SIMPLE_ACTION (quit), TRUE);
    gtk_widget_action_set_enabled (GTK_WIDGET (data->parent), "win.install-zip", TRUE);
    g_object_set_data (G_OBJECT (data->parent), "local-install-busy", NULL);
    exm_local_zip_free (data->zip);
    g_free (data->filename);
    g_object_unref (data->manager);
    g_object_unref (data->parent);
    g_application_release (data->application);
    g_object_unref (data->application);
    g_free (data);
}

static void
message (LocalInstall *data, const gchar *heading, const gchar *body)
{
    if (!gtk_widget_get_visible (GTK_WIDGET (data->parent)))
        return;
    AdwDialog *dialog = adw_alert_dialog_new (heading, body);
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "close", _("_Close"));
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "close");
    adw_dialog_present (dialog, GTK_WIDGET (data->parent));
}

static void
install_worker (GTask *task, gpointer source G_GNUC_UNUSED,
                gpointer task_data, GCancellable *cancellable G_GNUC_UNUSED)
{
    LocalInstall *data = task_data;
    GError *error = NULL;
    if (exm_local_zip_install (data->zip, data->replace, &error))
        g_task_return_boolean (task, TRUE);
    else
        g_task_return_error (task, error);
}

static void
installed (GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer user_data)
{
    LocalInstall *data = user_data;
    g_autoptr (GError) error = NULL;
    if (!g_task_propagate_boolean (G_TASK (result), &error))
        message (data, _("Installation Failed"), error->message);
    else
    {
        exm_manager_refresh_local (data->manager, data->zip->uuid);
        if (gtk_widget_get_visible (GTK_WIDGET (data->parent)))
            gtk_widget_activate_action (GTK_WIDGET (data->parent), "win.show-page", "s", "installed");
        message (data, _("Installed — Login Required"),
                 _("GNOME’s installer confirmed that the extension files were installed. "
                   "Log out and log back in to load the new code. On Wayland, GNOME Shell cannot be restarted in place.\n\n"
                   "Existing extension settings and enabled/disabled preferences were not changed. "
                   "A new extension can be enabled in the Installed tab after logging back in. "
                   "Until then, any previous version may still be running."));
    }
    finish (data);
}

static void
confirmed (GObject *source, GAsyncResult *result, gpointer user_data)
{
    LocalInstall *data = user_data;
    const gchar *response = adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);
    if (!g_str_equal (response, "install"))
    {
        finish (data);
        return;
    }
    /* Closing the window is safe (the application is held), but explicit
     * GApplication quit would otherwise terminate a running installation. */
    GAction *quit = g_action_map_lookup_action (G_ACTION_MAP (data->application), "quit");
    if (quit)
        g_simple_action_set_enabled (G_SIMPLE_ACTION (quit), FALSE);
    g_autoptr (GTask) task = g_task_new (NULL, NULL, installed, data);
    g_task_set_task_data (task, data, NULL);
    g_task_run_in_thread (task, install_worker);
}

static void
inspect_worker (GTask *task, gpointer source G_GNUC_UNUSED,
                gpointer task_data, GCancellable *cancellable G_GNUC_UNUSED)
{
    LocalInstall *data = task_data;
    GError *error = NULL;
    ExmLocalZip *zip = exm_local_zip_inspect (data->filename, &error);
    if (zip)
        g_task_return_pointer (task, zip, (GDestroyNotify) exm_local_zip_free);
    else
        g_task_return_error (task, error);
}

static void
inspected (GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer user_data)
{
    LocalInstall *data = user_data;
    g_autoptr (GError) error = NULL;
    data->zip = g_task_propagate_pointer (G_TASK (result), &error);
    if (!data->zip)
    {
        message (data, _("Invalid Extension ZIP"), error->message);
        finish (data);
        return;
    }
    if (!gtk_widget_get_visible (GTK_WIDGET (data->parent)))
    {
        finish (data);
        return;
    }
    ExmLocalZip *zip = data->zip;
    g_autoptr (ExmExtension) existing = exm_manager_get_by_uuid (data->manager, zip->uuid);
    data->replace = existing != NULL || exm_local_zip_is_installed (zip);
    g_autofree gchar *shell_version = NULL;
    g_object_get (data->manager, "shell-version", &shell_version, NULL);
    g_autofree gchar *versions = g_strjoinv (", ", zip->shell_versions);
    g_autofree gchar *old_version = NULL;
    if (existing)
        g_object_get (existing, "version", &old_version, NULL);
    gboolean compatible = exm_local_zip_supports (zip, shell_version);
    g_autofree gchar *body = g_strdup_printf (
        _("%s\nUUID: %s\nZIP version: %s\nInstalled version: %s\nSupported GNOME versions: %s\nCurrent GNOME: %s\n\n%s%s\n\n"
          "Only install extensions from sources you trust. Extension code runs inside GNOME Shell.\n\n"
          "Log out and log back in after installation. Your existing extension settings and enabled/disabled preferences will not be changed."),
        zip->name, zip->uuid, zip->version ? zip->version : _("Not specified"),
        data->replace ? (old_version ? old_version : _("Not specified")) : _("Not installed"),
        versions, shell_version ? shell_version : _("Unknown"),
        data->replace ? _("Replace the installation with this UUID? A system-wide extension will be overridden for your user only.")
                      : _("Install this extension for your user?"),
        compatible ? "" : _("\n\nWARNING: This extension does not declare support for your GNOME version. GNOME may refuse to load it. Version checks will not be disabled."));
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (
        data->replace ? _("Replace Extension?") : _("Install Extension?"), body));
    adw_alert_dialog_set_prefer_wide_layout (dialog, TRUE);
    adw_alert_dialog_add_responses (dialog, "cancel", _("_Cancel"), "install",
                                    data->replace ? _("_Replace") : _("_Install"), NULL);
    adw_alert_dialog_set_response_appearance (dialog, "install",
                                              data->replace || !compatible ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (dialog, "cancel");
    adw_alert_dialog_set_close_response (dialog, "cancel");
    adw_alert_dialog_choose (dialog, GTK_WIDGET (data->parent), NULL, confirmed, data);
}

static void
inspect_file (LocalInstall *data, GFile *file)
{
    data->filename = g_file_get_path (file);
    if (!data->filename)
    {
        message (data, _("Cannot Open ZIP"), _("Select a file on the local filesystem."));
        finish (data);
        return;
    }
    g_autoptr (GTask) task = g_task_new (NULL, NULL, inspected, data);
    g_task_set_task_data (task, data, NULL);
    g_task_run_in_thread (task, inspect_worker);
}

static void
selected (GObject *source, GAsyncResult *result, gpointer user_data)
{
    LocalInstall *data = user_data;
    g_autoptr (GError) error = NULL;
    g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);
    if (!file)
    {
        if (!g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
            !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED))
            message (data, _("Cannot Open ZIP"), error->message);
        finish (data);
        return;
    }
    inspect_file (data, file);
}

static LocalInstall *
begin (GtkWindow *parent, ExmManager *manager)
{
    if (g_object_get_data (G_OBJECT (parent), "local-install-busy"))
        return NULL;
    g_object_set_data (G_OBJECT (parent), "local-install-busy", GINT_TO_POINTER (1));
    LocalInstall *data = g_new0 (LocalInstall, 1);
    data->parent = g_object_ref (parent);
    data->manager = g_object_ref (manager);
    data->application = g_object_ref (g_application_get_default ());
    g_application_hold (data->application);
    gtk_widget_action_set_enabled (GTK_WIDGET (parent), "win.install-zip", FALSE);
    gboolean supported = FALSE;
    g_object_get (manager, "is-supported", &supported, NULL);
    if (!supported)
    {
        message (data, _("GNOME Shell Unavailable"), _("Log in to a GNOME session before installing extensions."));
        finish (data);
        return NULL;
    }
    return data;
}

void
exm_local_install_open_file (GtkWindow *parent, ExmManager *manager, GFile *file)
{
    LocalInstall *data = begin (parent, manager);
    if (data)
        inspect_file (data, file);
}

void
exm_local_install_choose (GtkWindow *parent, ExmManager *manager)
{
    LocalInstall *data = begin (parent, manager);
    if (!data)
        return;
    g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, _("Install from ZIP"));
    g_autoptr (GtkFileFilter) filter = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter, _("ZIP extension bundles"));
    gtk_file_filter_add_pattern (filter, "*.zip");
    gtk_file_filter_add_pattern (filter, "*.ZIP");
    g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
    g_list_store_append (filters, filter);
    gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
    gtk_file_dialog_set_default_filter (dialog, filter);
    gtk_file_dialog_open (dialog, parent, NULL, selected, data);
}
