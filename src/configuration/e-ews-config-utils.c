/*
 * e-ews-config-utils.c
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the program; if not, see <http://www.gnu.org/licenses/>
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <unistd.h>
#include <glib/gi18n-lib.h>

#include <gtk/gtk.h>
#include <libedataserver/libedataserver.h>
#include <libedataserverui/libedataserverui.h>
#include <e-util/e-dialog-utils.h>
#include <e-util/e-util.h>
#include <mail/em-folder-tree.h>
#include <misc/e-book-source-config.h>
#include <misc/e-cal-source-config.h>
#include <shell/e-shell.h>
#include <shell/e-shell-sidebar.h>
#include <shell/e-shell-view.h>
#include <shell/e-shell-window.h>

#include "server/e-ews-connection.h"
#include "server/e-source-ews-folder.h"

#include "e-ews-edit-folder-permissions.h"

#include "camel/camel-ews-store.h"
#include "camel/camel-ews-store-summary.h"

#include "e-ews-config-utils.h"
#include "e-ews-search-user.h"

struct RunWithFeedbackData
{
	GtkWindow *parent;
	GtkWidget *dialog;
	GCancellable *cancellable;
	GObject *with_object;
	EEwsSetupFunc thread_func;
	EEwsSetupFunc idle_func;
	gpointer user_data;
	GDestroyNotify free_user_data;
	GError *error;
	gboolean run_modal;
};

static void
free_run_with_feedback_data (gpointer ptr)
{
	struct RunWithFeedbackData *rfd = ptr;

	if (!rfd)
		return;

	if (rfd->dialog)
		gtk_widget_destroy (rfd->dialog);

	g_object_unref (rfd->cancellable);
	g_object_unref (rfd->with_object);

	if (rfd->free_user_data)
		rfd->free_user_data (rfd->user_data);

	g_clear_error (&rfd->error);

	g_free (rfd);
}

static gboolean
run_with_feedback_idle (gpointer user_data)
{
	struct RunWithFeedbackData *rfd = user_data;
	gboolean was_cancelled = FALSE;

	g_return_val_if_fail (rfd != NULL, FALSE);

	if (!g_cancellable_is_cancelled (rfd->cancellable)) {
		if (rfd->idle_func && !rfd->error)
			rfd->idle_func (rfd->with_object, rfd->user_data, rfd->cancellable, &rfd->error);

		was_cancelled = g_cancellable_is_cancelled (rfd->cancellable);

		if (rfd->dialog) {
			gtk_widget_destroy (rfd->dialog);
			rfd->dialog = NULL;
		}
	} else {
		was_cancelled = TRUE;
	}

	if (!was_cancelled) {
		if (rfd->error)
			e_notice (rfd->parent, GTK_MESSAGE_ERROR, "%s", rfd->error->message);
	}

	free_run_with_feedback_data (rfd);

	return FALSE;
}

static gpointer
run_with_feedback_thread (gpointer user_data)
{
	struct RunWithFeedbackData *rfd = user_data;

	g_return_val_if_fail (rfd != NULL, NULL);
	g_return_val_if_fail (rfd->thread_func != NULL, NULL);

	if (!g_cancellable_is_cancelled (rfd->cancellable))
		rfd->thread_func (rfd->with_object, rfd->user_data, rfd->cancellable, &rfd->error);

	g_idle_add (run_with_feedback_idle, rfd);

	return NULL;
}

static void
run_with_feedback_response_cb (GtkWidget *dialog,
			       gint resonse_id,
			       struct RunWithFeedbackData *rfd)
{
	g_return_if_fail (rfd != NULL);

	rfd->dialog = NULL;

	g_cancellable_cancel (rfd->cancellable);

	gtk_widget_destroy (dialog);
}

static void
e_ews_config_utils_run_in_thread_with_feedback_general (GtkWindow *parent,
							GObject *with_object,
							const gchar *description,
							EEwsSetupFunc thread_func,
							EEwsSetupFunc idle_func,
							gpointer user_data,
							GDestroyNotify free_user_data,
							gboolean run_modal)
{
	GtkWidget *dialog, *label, *content, *spinner, *box;
	struct RunWithFeedbackData *rfd;

	g_return_if_fail (with_object != NULL);
	g_return_if_fail (description != NULL);
	g_return_if_fail (thread_func != NULL);

	dialog = gtk_dialog_new_with_buttons ("",
		parent,
		GTK_DIALOG_MODAL,
		GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
		NULL);

	box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);

	spinner = gtk_spinner_new ();
	gtk_spinner_start (GTK_SPINNER (spinner));
	gtk_box_pack_start (GTK_BOX (box), spinner, FALSE, FALSE, 0);

	label = gtk_label_new (description);
	gtk_box_pack_start (GTK_BOX (box), label, TRUE, TRUE, 0);

	gtk_widget_show_all (box);

	content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));

	gtk_container_add (GTK_CONTAINER (content), box);
	gtk_container_set_border_width (GTK_CONTAINER (content), 12);

	rfd = g_new0 (struct RunWithFeedbackData, 1);
	rfd->parent = parent;
	rfd->dialog = dialog;
	rfd->cancellable = g_cancellable_new ();
	rfd->with_object = g_object_ref (with_object);
	rfd->thread_func = thread_func;
	rfd->idle_func = idle_func;
	rfd->user_data = user_data;
	rfd->free_user_data = free_user_data;
	rfd->error = NULL;
	rfd->run_modal = run_modal;

	g_signal_connect (dialog, "response", G_CALLBACK (run_with_feedback_response_cb), rfd);

	if (run_modal) {
		GCancellable *cancellable;

		cancellable = g_object_ref (rfd->cancellable);

		g_return_if_fail (g_thread_create (run_with_feedback_thread, rfd, FALSE, NULL));

		gtk_dialog_run (GTK_DIALOG (dialog));

		g_cancellable_cancel (cancellable);
		g_object_unref (cancellable);
	} else {
		gtk_widget_show (dialog);

		g_return_if_fail (g_thread_create (run_with_feedback_thread, rfd, FALSE, NULL));
	}
}

void
e_ews_config_utils_run_in_thread_with_feedback (GtkWindow *parent,
						GObject *with_object,
						const gchar *description,
						EEwsSetupFunc thread_func,
						EEwsSetupFunc idle_func,
						gpointer user_data,
						GDestroyNotify free_user_data)
{
	e_ews_config_utils_run_in_thread_with_feedback_general (parent, with_object, description, thread_func, idle_func, user_data, free_user_data, FALSE);
}

void
e_ews_config_utils_run_in_thread_with_feedback_modal (GtkWindow *parent,
						      GObject *with_object,
						      const gchar *description,
						      EEwsSetupFunc thread_func,
						      EEwsSetupFunc idle_func,
						      gpointer user_data,
						      GDestroyNotify free_user_data)
{
	e_ews_config_utils_run_in_thread_with_feedback_general (parent, with_object, description, thread_func, idle_func, user_data, free_user_data, TRUE);
}

typedef struct _EEwsConfigUtilsAuthenticator EEwsConfigUtilsAuthenticator;
typedef struct _EEwsConfigUtilsAuthenticatorClass EEwsConfigUtilsAuthenticatorClass;

struct _EEwsConfigUtilsAuthenticator {
	GObject parent;

	ESourceRegistry *registry;
	CamelEwsSettings *ews_settings;
	EEwsConnection *conn;
};

struct _EEwsConfigUtilsAuthenticatorClass {
	GObjectClass parent_class;
};

static ESourceAuthenticationResult
ews_config_utils_authenticator_try_password_sync (ESourceAuthenticator *auth,
						  const GString *password,
						  GCancellable *cancellable,
						  GError **error)
{
	EEwsConfigUtilsAuthenticator *authenticator = (EEwsConfigUtilsAuthenticator *) auth;
	CamelNetworkSettings *network_settings;
	gchar *hosturl, *user;
	EwsFolderId *fid;
	GSList *ids = NULL, *folders = NULL;
	GError *local_error = NULL;

	network_settings = CAMEL_NETWORK_SETTINGS (authenticator->ews_settings);

	hosturl = camel_ews_settings_dup_hosturl (authenticator->ews_settings);
	user = camel_network_settings_dup_user (network_settings);

	authenticator->conn = e_ews_connection_new (
		hosturl, authenticator->ews_settings);
	e_ews_connection_set_password (authenticator->conn, password->str);

	g_free (hosturl);
	g_free (user);

	if (local_error) {
		g_warn_if_fail (!authenticator->conn);
		authenticator->conn = NULL;

		g_propagate_error (error, local_error);

		return E_SOURCE_AUTHENTICATION_ERROR;
	}

	g_warn_if_fail (authenticator->conn);

	/* test whether connection works with some simple operation */
	fid = g_new0 (EwsFolderId, 1);
	fid->id = g_strdup ("inbox");
	fid->is_distinguished_id = TRUE;
	ids = g_slist_append (ids, fid);

	e_ews_connection_get_folder_sync (
		authenticator->conn, EWS_PRIORITY_MEDIUM, "Default",
		NULL, ids, &folders, cancellable, &local_error);

	e_ews_folder_id_free (fid);
	g_slist_free (ids);
	g_slist_free_full (folders, g_object_unref);

	if (local_error) {
		g_object_unref (authenticator->conn);
		authenticator->conn = NULL;

		if (g_error_matches (
		    local_error, EWS_CONNECTION_ERROR,
		    EWS_CONNECTION_ERROR_AUTHENTICATION_FAILED)) {
			g_clear_error (&local_error);
			return E_SOURCE_AUTHENTICATION_REJECTED;
		}

		g_propagate_error (error, local_error);

		return E_SOURCE_AUTHENTICATION_ERROR;
	}

	return E_SOURCE_AUTHENTICATION_ACCEPTED;
}

#define E_TYPE_EWS_CONFIG_UTILS_AUTHENTICATOR (e_ews_config_utils_authenticator_get_type ())

GType e_ews_config_utils_authenticator_get_type (void) G_GNUC_CONST;

static void e_ews_config_utils_authenticator_authenticator_init (ESourceAuthenticatorInterface *interface);

G_DEFINE_TYPE_EXTENDED (EEwsConfigUtilsAuthenticator, e_ews_config_utils_authenticator, G_TYPE_OBJECT, 0,
	G_IMPLEMENT_INTERFACE (E_TYPE_SOURCE_AUTHENTICATOR, e_ews_config_utils_authenticator_authenticator_init))

static void
ews_config_utils_authenticator_finalize (GObject *object)
{
	EEwsConfigUtilsAuthenticator *authenticator = (EEwsConfigUtilsAuthenticator *) object;

	g_object_unref (authenticator->registry);
	g_object_unref (authenticator->ews_settings);
	if (authenticator->conn)
		g_object_unref (authenticator->conn);

	G_OBJECT_CLASS (e_ews_config_utils_authenticator_parent_class)->finalize (object);
}

static void
e_ews_config_utils_authenticator_class_init (EEwsConfigUtilsAuthenticatorClass *class)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS (class);
	object_class->finalize = ews_config_utils_authenticator_finalize;
}

static void
e_ews_config_utils_authenticator_authenticator_init (ESourceAuthenticatorInterface *interface)
{
	interface->try_password_sync = ews_config_utils_authenticator_try_password_sync;
}

static void
e_ews_config_utils_authenticator_init (EEwsConfigUtilsAuthenticator *authenticator)
{
}

EEwsConnection	*
e_ews_config_utils_open_connection_for (GtkWindow *parent,
					ESourceRegistry *registry,
					ESource *source,
					CamelEwsSettings *ews_settings,
					GCancellable *cancellable,
					GError **perror)
{
	EEwsConnection *conn = NULL;
	CamelNetworkSettings *network_settings;
	GError *local_error = NULL;

	g_return_val_if_fail (registry != NULL, NULL);
	g_return_val_if_fail (source != NULL, NULL);
	g_return_val_if_fail (ews_settings != NULL, NULL);

	network_settings = CAMEL_NETWORK_SETTINGS (ews_settings);

	/* use the one from mailer, if there, otherwise open new */
	conn = e_ews_connection_find (
		camel_ews_settings_get_hosturl (ews_settings),
		camel_network_settings_get_user (network_settings));
	if (conn)
		return conn;

	while (!conn && !g_cancellable_is_cancelled (cancellable) && !local_error) {
		EEwsConfigUtilsAuthenticator *authenticator = g_object_new (E_TYPE_EWS_CONFIG_UTILS_AUTHENTICATOR, NULL);

		authenticator->ews_settings = g_object_ref (ews_settings);
		authenticator->registry = g_object_ref (registry);

		e_source_registry_authenticate_sync (
			registry, source, E_SOURCE_AUTHENTICATOR (authenticator),
			cancellable, &local_error);

		if (authenticator->conn)
			conn = g_object_ref (authenticator->conn);

		g_object_unref (authenticator);
	}

	if (local_error)
		g_propagate_error (perror, local_error);

	return conn;
}

static gboolean
get_ews_store_from_folder_tree (EShellView *shell_view,
				gchar **pfolder_path,
				CamelStore **pstore)
{
	EShellSidebar *shell_sidebar;
	EMFolderTree *folder_tree;
	gchar *selected_path = NULL;
	CamelStore *selected_store = NULL;
	gboolean found = FALSE;

	/* Get hold of Folder Tree */
	shell_sidebar = e_shell_view_get_shell_sidebar (shell_view);
	g_object_get (shell_sidebar, "folder-tree", &folder_tree, NULL);
	if (em_folder_tree_get_selected (folder_tree, &selected_store, &selected_path) ||
	    em_folder_tree_store_root_selected (folder_tree, &selected_store)) {
		if (selected_store) {
			CamelProvider *provider = camel_service_get_provider (CAMEL_SERVICE (selected_store));

			if (provider && g_ascii_strcasecmp (provider->protocol, "ews") == 0) {
				found = TRUE;

				if (pstore)
					*pstore = g_object_ref (selected_store);

				if (pfolder_path)
					*pfolder_path = selected_path;
				else
					g_free (selected_path);

				selected_path = NULL;
			}

			g_object_unref (selected_store);
		}

		g_free (selected_path);
	}

	g_object_unref (folder_tree);

	return found;
}

/* static void
action_subscribe_foreign_folder_cb (GtkAction *action,
				    EShellView *shell_view)
{
	GtkWindow *parent;
	EShellBackend *backend;
	CamelSession *session = NULL;
	CamelStore *store = NULL;

	if (!get_ews_store_from_folder_tree (shell_view, NULL, &store))
		return;

	parent = GTK_WINDOW (e_shell_view_get_shell_window (shell_view));
	backend = e_shell_view_get_shell_backend (shell_view);
	g_object_get (G_OBJECT (backend), "session", &session, NULL);

	e_ews_subscribe_foreign_folder (parent, session, store);

	g_object_unref (session);
	g_object_unref (store);
	g_free (profile);
} */

static void
action_folder_permissions_mail_cb (GtkAction *action,
				   EShellView *shell_view)
{
	gchar *folder_path = NULL;
	EShellWindow *shell_window;
	GtkWindow *parent;
	CamelStore *store = NULL;
	CamelEwsStore *ews_store;
	CamelNetworkSettings *network_settings;
	gchar *str_folder_id;

	if (!get_ews_store_from_folder_tree (shell_view, &folder_path, &store))
		return;

	ews_store = CAMEL_EWS_STORE (store);
	g_return_if_fail (ews_store != NULL);
	g_return_if_fail (folder_path != NULL);

	network_settings = CAMEL_NETWORK_SETTINGS (camel_service_get_settings (CAMEL_SERVICE (store)));
	g_return_if_fail (network_settings != NULL);

	shell_window = e_shell_view_get_shell_window (shell_view);
	parent = GTK_WINDOW (shell_window);

	str_folder_id = camel_ews_store_summary_get_folder_id_from_name (ews_store->summary, folder_path);
	if (!str_folder_id) {
		e_notice (parent, GTK_MESSAGE_ERROR, _("Cannot edit permissions of folder '%s', choose other folder."), folder_path);
	} else {
		ESourceRegistry *registry = e_shell_get_registry (e_shell_window_get_shell (shell_window));
		ESource *source;
		EwsFolderId *folder_id;
		gchar *str_change_key;

		source = e_source_registry_ref_source (registry, camel_service_get_uid (CAMEL_SERVICE (store)));
		g_return_if_fail (source != NULL);

		str_change_key = camel_ews_store_summary_get_change_key (ews_store->summary, str_folder_id, NULL);

		folder_id = e_ews_folder_id_new (str_folder_id, str_change_key, FALSE);

		e_ews_edit_folder_permissions (parent,
			registry,
			source,
			CAMEL_EWS_SETTINGS (network_settings),
			camel_service_get_display_name (CAMEL_SERVICE (store)),
			folder_path,
			folder_id,
			EWS_FOLDER_TYPE_MAILBOX);

		g_object_unref (source);
		g_free (str_folder_id);
		g_free (str_change_key);
		e_ews_folder_id_free (folder_id);
	}

	g_object_unref (store);
	g_free (folder_path);
}

static void
ews_ui_enable_actions (GtkActionGroup *action_group,
		       const GtkActionEntry *entries,
		       guint n_entries,
		       gboolean can_show,
		       gboolean is_online)
{
	gint ii;

	g_return_if_fail (action_group != NULL);
	g_return_if_fail (entries != NULL);

	for (ii = 0; ii < n_entries; ii++) {
		GtkAction *action;

		action = gtk_action_group_get_action (action_group, entries[ii].name);
		if (!action)
			continue;

		gtk_action_set_visible (action, can_show);
		if (can_show)
			gtk_action_set_sensitive (action, is_online);
	}
}

/*static GtkActionEntry mail_account_context_entries[] = {

	{ "mail-ews-subscribe-foreign-folder",
	  NULL,
	  N_("Subscribe to folder of other user..."),
	  NULL,
	  NULL,  / * XXX Add a tooltip! * /
	  G_CALLBACK (action_subscribe_foreign_folder_cb) }
};*/

static GtkActionEntry mail_folder_context_entries[] = {
	{ "mail-ews-folder-permissions",
	  "folder-new",
	  N_("Permissions..."),
	  NULL,
	  N_("Edit EWS folder permissions"),
	  G_CALLBACK (action_folder_permissions_mail_cb) }
};

static const gchar *ews_ui_mail_def =
	"<popup name=\"mail-folder-popup\">\n"
	"  <placeholder name=\"mail-folder-popup-actions\">\n"
	/*"    <menuitem action=\"mail-ews-subscribe-foreign-folder\"/>\n"*/
	"    <menuitem action=\"mail-ews-folder-permissions\"/>\n"
	"  </placeholder>\n"
	"</popup>\n";

static void
ews_ui_update_actions_mail_cb (EShellView *shell_view,
			       GtkActionEntry *entries)
{
	EShellWindow *shell_window;
	GtkActionGroup *action_group;
	GtkUIManager *ui_manager;
	EShellSidebar *shell_sidebar;
	EMFolderTree *folder_tree;
	CamelStore *selected_store = NULL;
	gchar *selected_path = NULL;
	gboolean account_node = FALSE, folder_node = FALSE;
	gboolean online = FALSE;

	shell_sidebar = e_shell_view_get_shell_sidebar (shell_view);
	g_object_get (shell_sidebar, "folder-tree", &folder_tree, NULL);
	if (em_folder_tree_get_selected (folder_tree, &selected_store, &selected_path) ||
	    em_folder_tree_store_root_selected (folder_tree, &selected_store)) {
		if (selected_store) {
			CamelProvider *provider = camel_service_get_provider (CAMEL_SERVICE (selected_store));

			if (provider && g_ascii_strcasecmp (provider->protocol, "ews") == 0) {
				account_node = !selected_path || !*selected_path;
				folder_node = !account_node;
			}

			g_object_unref (selected_store);
		}
	}
	g_object_unref (folder_tree);

	g_free (selected_path);

	shell_window = e_shell_view_get_shell_window (shell_view);
	ui_manager = e_shell_window_get_ui_manager (shell_window);
	action_group = e_lookup_action_group (ui_manager, "mail");

	if (account_node || folder_node) {
		EShellBackend *backend;
		CamelSession *session = NULL;

		backend = e_shell_view_get_shell_backend (shell_view);
		g_object_get (G_OBJECT (backend), "session", &session, NULL);

		online = session && camel_session_get_online (session);

		if (session)
			g_object_unref (session);
	}

	/* ews_ui_enable_actions (action_group, mail_account_context_entries, G_N_ELEMENTS (mail_account_context_entries), account_node, online); */
	ews_ui_enable_actions (action_group, mail_folder_context_entries, G_N_ELEMENTS (mail_folder_context_entries), folder_node, online);
}

static void
ews_ui_init_mail (GtkUIManager *ui_manager,
                  EShellView *shell_view,
		  gchar **ui_definition)
{
	EShellWindow *shell_window;
	GtkActionGroup *action_group;

	g_return_if_fail (ui_definition != NULL);

	*ui_definition = g_strdup (ews_ui_mail_def);

	shell_window = e_shell_view_get_shell_window (shell_view);
	action_group = e_shell_window_get_action_group (shell_window, "mail");

	/* Add actions to the "mail" action group. */
	/*e_action_group_add_actions_localized (action_group, GETTEXT_PACKAGE,
		mail_account_context_entries, G_N_ELEMENTS (mail_account_context_entries), shell_view);*/
	e_action_group_add_actions_localized (action_group, GETTEXT_PACKAGE,
		mail_folder_context_entries, G_N_ELEMENTS (mail_folder_context_entries), shell_view);

	/* Decide whether we want this option to be visible or not */
	g_signal_connect (shell_view, "update-actions",
			  G_CALLBACK (ews_ui_update_actions_mail_cb),
			  shell_view);

	g_object_unref (action_group);
}

static gboolean
get_selected_ews_source (EShellView *shell_view,
			 ESource **selected_source,
			 ESourceRegistry **registry)
{
	ESource *source;
	EShellSidebar *shell_sidebar;
	ESourceSelector *selector = NULL;

	g_return_val_if_fail (shell_view != NULL, FALSE);

	shell_sidebar = e_shell_view_get_shell_sidebar (shell_view);
	g_return_val_if_fail (shell_sidebar != NULL, FALSE);

	g_object_get (shell_sidebar, "selector", &selector, NULL);
	g_return_val_if_fail (selector != NULL, FALSE);

	source = e_source_selector_ref_primary_selection (selector);
	if (source) {
		ESourceBackend *backend_ext = NULL;

		if (e_source_has_extension (source, E_SOURCE_EXTENSION_ADDRESS_BOOK))
			backend_ext = e_source_get_extension (source, E_SOURCE_EXTENSION_ADDRESS_BOOK);
		else if (e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR))
			backend_ext = e_source_get_extension (source, E_SOURCE_EXTENSION_CALENDAR);
		else if (e_source_has_extension (source, E_SOURCE_EXTENSION_MEMO_LIST))
			backend_ext = e_source_get_extension (source, E_SOURCE_EXTENSION_MEMO_LIST);
		else if (e_source_has_extension (source, E_SOURCE_EXTENSION_TASK_LIST))
			backend_ext = e_source_get_extension (source, E_SOURCE_EXTENSION_TASK_LIST);
		else if (e_source_has_extension (source, E_SOURCE_EXTENSION_MAIL_ACCOUNT))
			backend_ext = e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_ACCOUNT);

		if (!backend_ext ||
		    g_strcmp0 (e_source_backend_get_backend_name (backend_ext), "ews") != 0) {
			g_object_unref (source);
			source = NULL;
		}
	}

	if (source && registry)
		*registry = g_object_ref (e_source_selector_get_registry (selector));

	g_object_unref (selector);

	if (selected_source)
		*selected_source = source;
	else if (source)
		g_object_unref (source);

	return source != NULL;
}

/* how many menu entries are defined; all calendar/tasks/memos/contacts
   actions should have same count */
#define EWS_ESOURCE_NUM_ENTRIES 1

static void
update_ews_source_entries_cb (EShellView *shell_view,
			      GtkActionEntry *entries)
{
	GtkActionGroup *action_group;
	EShell *shell;
	EShellWindow *shell_window;
	const gchar *group;
	gboolean is_ews_source, is_online;

	g_return_if_fail (E_IS_SHELL_VIEW (shell_view));
	g_return_if_fail (entries != NULL);

	if (strstr (entries->name, "calendar"))
		group = "calendar";
	else if (strstr (entries->name, "tasks"))
		group = "tasks";
	else if (strstr (entries->name, "memos"))
		group = "memos";
	else if (strstr (entries->name, "contacts"))
		group = "contacts";
	else
		g_return_if_reached ();

	is_ews_source = get_selected_ews_source (shell_view, NULL, NULL);
	shell_window = e_shell_view_get_shell_window (shell_view);
	shell = e_shell_window_get_shell (shell_window);

	is_online = shell && e_shell_get_online (shell);
	action_group = e_shell_window_get_action_group (shell_window, group);

	ews_ui_enable_actions (action_group, entries, EWS_ESOURCE_NUM_ENTRIES, is_ews_source, is_online);
}

static void
setup_ews_source_actions (EShellView *shell_view,
			  GtkUIManager *ui_manager,
			  GtkActionEntry *entries,
			  guint n_entries)
{
	EShellWindow *shell_window;
	const gchar *group;

	g_return_if_fail (shell_view != NULL);
	g_return_if_fail (ui_manager != NULL);
	g_return_if_fail (entries != NULL);
	g_return_if_fail (n_entries > 0);
	g_return_if_fail (n_entries == EWS_ESOURCE_NUM_ENTRIES);

	if (strstr (entries->name, "calendar"))
		group = "calendar";
	else if (strstr (entries->name, "tasks"))
		group = "tasks";
	else if (strstr (entries->name, "memos"))
		group = "memos";
	else if (strstr (entries->name, "contacts"))
		group = "contacts";
	else
		g_return_if_reached ();

	shell_window = e_shell_view_get_shell_window (shell_view);

	e_action_group_add_actions_localized (
		e_shell_window_get_action_group (shell_window, group), GETTEXT_PACKAGE,
		entries, EWS_ESOURCE_NUM_ENTRIES, shell_view);

	g_signal_connect (shell_view, "update-actions", G_CALLBACK (update_ews_source_entries_cb), entries);
}

static void
action_folder_permissions_source_cb (GtkAction *action,
				     EShellView *shell_view)
{
	ESourceRegistry *registry = NULL;
	ESource *source = NULL, *parent_source;
	ESourceEwsFolder *folder_ext;
	ESourceCamel *extension;
	CamelSettings *settings;
	const gchar *extension_name;
	EwsFolderId *folder_id;
	EwsFolderType folder_type;

	g_return_if_fail (action != NULL);
	g_return_if_fail (shell_view != NULL);
	g_return_if_fail (get_selected_ews_source (shell_view, &source, &registry));
	g_return_if_fail (source != NULL);
	g_return_if_fail (e_source_has_extension (source, E_SOURCE_EXTENSION_EWS_FOLDER));
	g_return_if_fail (gtk_action_get_name (action) != NULL);

	folder_ext = e_source_get_extension (source, E_SOURCE_EXTENSION_EWS_FOLDER);
	folder_id = e_source_ews_folder_dup_folder_id (folder_ext);
	g_return_if_fail (folder_id != NULL);

	parent_source = e_source_registry_ref_source (registry, e_source_get_parent (source));

	extension_name = e_source_camel_get_extension_name ("ews");
	extension = e_source_get_extension (parent_source, extension_name);
	settings = e_source_camel_get_settings (extension);

	folder_type = EWS_FOLDER_TYPE_MAILBOX;
	if (strstr (gtk_action_get_name (action), "calendar") != NULL)
		folder_type = EWS_FOLDER_TYPE_CALENDAR;
	else if (strstr (gtk_action_get_name (action), "contacts") != NULL)
		folder_type = EWS_FOLDER_TYPE_CONTACTS;
	else if (strstr (gtk_action_get_name (action), "tasks") != NULL)
		folder_type = EWS_FOLDER_TYPE_TASKS;

	e_ews_edit_folder_permissions (NULL,
		registry,
		source,
		CAMEL_EWS_SETTINGS (settings),
		e_source_get_display_name (parent_source),
		e_source_get_display_name (source),
		folder_id,
		folder_type);

	g_object_unref (source);
	g_object_unref (parent_source);
	g_object_unref (registry);
	e_ews_folder_id_free (folder_id);
}

static GtkActionEntry calendar_context_entries[] = {

	{ "calendar-ews-folder-permissions",
	  "folder-new",
	  N_("Permissions..."),
	  NULL,
	  N_("Edit EWS calendar permissions"),
	  G_CALLBACK (action_folder_permissions_source_cb) }
};

static const gchar *ews_ui_cal_def =
	"<popup name=\"calendar-popup\">\n"
	"  <placeholder name=\"calendar-popup-actions\">\n"
	"    <menuitem action=\"calendar-ews-folder-permissions\"/>\n"
	"  </placeholder>\n"
	"</popup>\n";

static void
ews_ui_init_calendar (GtkUIManager *ui_manager,
		      EShellView *shell_view,
		      gchar **ui_definition)
{
	g_return_if_fail (ui_definition != NULL);

	*ui_definition = g_strdup (ews_ui_cal_def);

	setup_ews_source_actions (shell_view, ui_manager,
		calendar_context_entries, G_N_ELEMENTS (calendar_context_entries));
}

static GtkActionEntry tasks_context_entries[] = {

	{ "tasks-ews-folder-permissions",
	  "folder-new",
	  N_("Permissions..."),
	  NULL,
	  N_("Edit EWS tasks permissions"),
	  G_CALLBACK (action_folder_permissions_source_cb) }
};

static const gchar *ews_ui_task_def =
	"<popup name=\"task-list-popup\">\n"
	"  <placeholder name=\"task-list-popup-actions\">\n"
	"    <menuitem action=\"tasks-ews-folder-permissions\"/>\n"
	"  </placeholder>\n"
	"</popup>\n";

static void
ews_ui_init_tasks (GtkUIManager *ui_manager,
		   EShellView *shell_view,
		   gchar **ui_definition)
{
	g_return_if_fail (ui_definition != NULL);

	*ui_definition = g_strdup (ews_ui_task_def);

	setup_ews_source_actions (shell_view, ui_manager,
		tasks_context_entries, G_N_ELEMENTS (tasks_context_entries));
}

static GtkActionEntry memos_context_entries[] = {

	{ "memos-ews-folder-permissions",
	  "folder-new",
	  N_("Permissions..."),
	  NULL,
	  N_("Edit EWS memos permissions"),
	  G_CALLBACK (action_folder_permissions_source_cb) }
};

static const gchar *ews_ui_memo_def =
	"<popup name=\"memo-list-popup\">\n"
	"  <placeholder name=\"memo-list-popup-actions\">\n"
	"    <menuitem action=\"memos-ews-folder-permissions\"/>\n"
	"  </placeholder>\n"
	"</popup>\n";

static void
ews_ui_init_memos (GtkUIManager *ui_manager,
		   EShellView *shell_view,
		   gchar **ui_definition)
{
	g_return_if_fail (ui_definition != NULL);

	*ui_definition = g_strdup (ews_ui_memo_def);

	setup_ews_source_actions (shell_view, ui_manager,
		memos_context_entries, G_N_ELEMENTS (memos_context_entries));
}

static GtkActionEntry contacts_context_entries[] = {

	{ "contacts-ews-folder-permissions",
	  "folder-new",
	  N_("Permissions..."),
	  NULL,
	  N_("Edit EWS contacts permissions"),
	  G_CALLBACK (action_folder_permissions_source_cb) }
};

static const gchar *ews_ui_book_def =
	"<popup name=\"address-book-popup\">\n"
	"  <placeholder name=\"address-book-popup-actions\">\n"
	"    <menuitem action=\"contacts-ews-folder-permissions\"/>\n"
	"  </placeholder>\n"
	"</popup>\n";

static void
ews_ui_init_contacts (GtkUIManager *ui_manager,
		      EShellView *shell_view,
		      gchar **ui_definition)
{
	g_return_if_fail (ui_definition != NULL);

	*ui_definition = g_strdup (ews_ui_book_def);

	setup_ews_source_actions (shell_view, ui_manager,
		contacts_context_entries, G_N_ELEMENTS (contacts_context_entries));
}

void
e_ews_config_utils_init_ui (EShellView *shell_view,
			    const gchar *ui_manager_id,
			    gchar **ui_definition)
{
	EShellWindow *shell_window;
	GtkUIManager *ui_manager;

	g_return_if_fail (shell_view != NULL);
	g_return_if_fail (ui_manager_id != NULL);
	g_return_if_fail (ui_definition != NULL);

	shell_window = e_shell_view_get_shell_window (shell_view);
	ui_manager = e_shell_window_get_ui_manager (shell_window);

	if (g_strcmp0 (ui_manager_id, "org.gnome.evolution.mail") == 0)
		ews_ui_init_mail (ui_manager, shell_view, ui_definition);
	else if (g_strcmp0 (ui_manager_id, "org.gnome.evolution.calendars") == 0)
		ews_ui_init_calendar (ui_manager, shell_view, ui_definition);
	else if (g_strcmp0 (ui_manager_id, "org.gnome.evolution.tasks") == 0)
		ews_ui_init_tasks (ui_manager, shell_view, ui_definition);
	else if (g_strcmp0 (ui_manager_id, "org.gnome.evolution.memos") == 0)
		ews_ui_init_memos (ui_manager, shell_view, ui_definition);
	else if (g_strcmp0 (ui_manager_id, "org.gnome.evolution.contacts") == 0)
		ews_ui_init_contacts (ui_manager, shell_view, ui_definition);
}

gboolean
e_ews_config_utils_is_online (void)
{
	EShell *shell;

	shell = e_shell_get_default ();

	return shell && e_shell_get_online (shell);
}

GtkWindow *
e_ews_config_utils_get_widget_toplevel_window (GtkWidget *widget)
{
	if (!widget)
		return NULL;

	if (!GTK_IS_WINDOW (widget))
		widget = gtk_widget_get_toplevel (widget);

	if (GTK_IS_WINDOW (widget))
		return GTK_WINDOW (widget);

	return NULL;
}

static gpointer
ews_config_utils_unref_in_thread (gpointer user_data)
{
	g_object_unref (user_data);

	return NULL;
}

void
e_ews_config_utils_unref_in_thread (GObject *object)
{
	GThread *thread;

	g_return_if_fail (object != NULL);
	g_return_if_fail (G_IS_OBJECT (object));

	thread = g_thread_new (NULL, ews_config_utils_unref_in_thread, object);
	g_thread_unref (thread);
}