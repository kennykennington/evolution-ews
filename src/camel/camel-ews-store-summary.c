#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include "camel-ews-store-summary.h"

#define S_LOCK(x) (g_static_rec_mutex_lock(&(x)->priv->s_lock))
#define S_UNLOCK(x) (g_static_rec_mutex_unlock(&(x)->priv->s_lock))

#define STORE_GROUP_NAME "##storepriv"

struct _CamelEwsStoreSummaryPrivate {
	GKeyFile *key_file;
	gboolean dirty;
	gchar *path;
	/* Note: We use the *same* strings in both of these hash tables, and
	   only id_fname_hash has g_free() hooked up as the destructor func.
	   So entries must always be removed from fname_id_hash *first*. */
	GHashTable *id_fname_hash;
	GHashTable *fname_id_hash;
	GStaticRecMutex s_lock;
};

G_DEFINE_TYPE (CamelEwsStoreSummary, camel_ews_store_summary, CAMEL_TYPE_OBJECT)

static void
ews_store_summary_finalize (GObject *object)
{
	CamelEwsStoreSummary *ews_summary = CAMEL_EWS_STORE_SUMMARY (object);
	CamelEwsStoreSummaryPrivate *priv = ews_summary->priv;

	g_key_file_free (priv->key_file);
	g_free (priv->path);
	g_hash_table_destroy (priv->fname_id_hash);
	g_hash_table_destroy (priv->id_fname_hash);
	g_static_rec_mutex_free (&priv->s_lock);

	g_free (priv);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (camel_ews_store_summary_parent_class)->finalize (object);
}

static void
camel_ews_store_summary_class_init (CamelEwsStoreSummaryClass *class)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS (class);
	object_class->finalize = ews_store_summary_finalize;
}

static void
camel_ews_store_summary_init (CamelEwsStoreSummary *ews_summary)
{
	CamelEwsStoreSummaryPrivate *priv;

	priv = g_new0 (CamelEwsStoreSummaryPrivate, 1);
	ews_summary->priv = priv;

	priv->key_file = g_key_file_new ();
	priv->dirty = FALSE;
	priv->fname_id_hash = g_hash_table_new (g_str_hash, g_str_equal);
	priv->id_fname_hash = g_hash_table_new_full (g_str_hash, g_str_equal,
						     (GDestroyNotify) g_free,
						     (GDestroyNotify) g_free);
	g_static_rec_mutex_init (&priv->s_lock);
}

static gchar *build_full_name (CamelEwsStoreSummary *ews_summary, const gchar *fid)
{
	gchar *pfid, *dname, *ret;
	gchar *pname = NULL;

	dname = camel_ews_store_summary_get_folder_name (ews_summary, fid, NULL);
	if (!dname)
		return NULL;

	pfid = camel_ews_store_summary_get_parent_folder_id (ews_summary, fid, NULL);
	if (pfid) {
		pname = build_full_name (ews_summary, pfid);
		g_free (pfid);
	}

	if (pname) {
		ret = g_strdup_printf ("%s/%s", pname, dname);
		g_free (pname);
		g_free (dname);
	} else
		ret = dname;

	return ret;
}

static void
load_id_fname_hash (CamelEwsStoreSummary *ews_summary)
{
	GSList *folders, *l;

	folders = camel_ews_store_summary_get_folders (ews_summary, NULL);

	for (l = folders; l != NULL; l = g_slist_next (l)) {
		gchar *id = l->data;
		gchar *fname;

		fname = build_full_name (ews_summary, id);

		g_hash_table_insert (ews_summary->priv->fname_id_hash, fname, id);
		g_hash_table_insert (ews_summary->priv->id_fname_hash, id, fname);
	}

	g_slist_free (folders);
}

CamelEwsStoreSummary *
camel_ews_store_summary_new (const gchar *path)
{
	CamelEwsStoreSummary *ews_summary;

	ews_summary = g_object_new (CAMEL_TYPE_EWS_STORE_SUMMARY, NULL);

	ews_summary->priv->path = g_strdup (path);

	return ews_summary;
}

gboolean
camel_ews_store_summary_load (CamelEwsStoreSummary *ews_summary,
			      GError **error)
{
	CamelEwsStoreSummaryPrivate *priv = ews_summary->priv;
	gboolean ret;

	S_LOCK(ews_summary);

	ret = g_key_file_load_from_file	(priv->key_file,
					 priv->path,
					 0, error);

	load_id_fname_hash (ews_summary);
	S_UNLOCK(ews_summary);

	return ret;
}

gboolean
camel_ews_store_summary_save (CamelEwsStoreSummary *ews_summary,
			      GError **error)
{
	CamelEwsStoreSummaryPrivate *priv = ews_summary->priv;
	gboolean ret = TRUE;
	GFile *file;
	gchar *contents = NULL;

	S_LOCK(ews_summary);

	if (!priv->dirty)
		goto exit;

	contents = g_key_file_to_data	(priv->key_file, NULL,
		       			 NULL);
	file = g_file_new_for_path	(priv->path);
	ret = g_file_replace_contents	(file, contents, strlen (contents),
					 NULL, FALSE, G_FILE_CREATE_PRIVATE,
					 NULL, NULL, error);
	priv->dirty = FALSE;

exit:
	S_UNLOCK(ews_summary);

	g_free (contents);
	return ret;
}

gboolean
camel_ews_store_summary_clear (CamelEwsStoreSummary *ews_summary)
{

	S_LOCK(ews_summary);

	g_key_file_free (ews_summary->priv->key_file);
	ews_summary->priv->key_file = g_key_file_new ();
	ews_summary->priv->dirty = TRUE;

	S_UNLOCK(ews_summary);

	return TRUE;
}

gboolean
camel_ews_store_summary_remove (CamelEwsStoreSummary *ews_summary)
{
	gint ret;

	S_LOCK(ews_summary);

	if (ews_summary->priv->key_file)
		camel_ews_store_summary_clear (ews_summary);

	ret = g_unlink (ews_summary->priv->path);

	S_UNLOCK(ews_summary);

	return (ret == 0);
}
/* Must be called with the summary lock held, and gets to keep
   both its string arguments */
static void ews_ss_hash_replace (CamelEwsStoreSummary *ews_summary,
				 gchar *folder_id,
				 gchar *full_name)
{
	const gchar *ofname;

	if (!full_name)
		full_name = build_full_name (ews_summary, folder_id);

	ofname = g_hash_table_lookup (ews_summary->priv->id_fname_hash,
				      folder_id);
	/* Remove the old fullname->id hash entry *iff* it's pointing
	   to this folder id. */
	if (ofname) {
		char *ofid = g_hash_table_lookup (ews_summary->priv->fname_id_hash,
						  ofname);
		if (!strcmp (folder_id, ofid))
			g_hash_table_remove (ews_summary->priv->fname_id_hash,
					     ofname);
	}
	g_hash_table_insert (ews_summary->priv->fname_id_hash, full_name, folder_id);

	/* Replace, not insert. The difference is that it frees the *old* folder_id
	   key, not the new one which we just inserted into fname_id_hash too. */
	g_hash_table_replace (ews_summary->priv->id_fname_hash, folder_id, full_name);
}

void
camel_ews_store_summary_set_folder_name (CamelEwsStoreSummary *ews_summary,
					 const gchar *folder_id,
					 const gchar *display_name)
{
	S_LOCK(ews_summary);

	g_key_file_set_string	(ews_summary->priv->key_file, folder_id,
				 "DisplayName", display_name);

	ews_ss_hash_replace (ews_summary, g_strdup (folder_id), NULL);
	ews_summary->priv->dirty = TRUE;

	S_UNLOCK(ews_summary);
}


void
camel_ews_store_summary_new_folder (CamelEwsStoreSummary *ews_summary,
				    const gchar *folder_id,
				    const gchar *parent_fid,
				    const gchar *change_key,
				    const gchar *display_name,
				    guint64 folder_type,
				    guint64 folder_flags,
				    guint64 total)
{
	S_LOCK(ews_summary);

	g_key_file_set_string (ews_summary->priv->key_file, folder_id,
			       "ParentFolderId", parent_fid);
	g_key_file_set_string (ews_summary->priv->key_file, folder_id,
			       "ChangeKey", change_key);
	g_key_file_set_string (ews_summary->priv->key_file, folder_id,
			       "DisplayName", display_name);
	g_key_file_set_uint64 (ews_summary->priv->key_file, folder_id,
			       "FolderType", folder_type);
	g_key_file_set_uint64 (ews_summary->priv->key_file, folder_id,
			       "Flags", folder_flags);
	g_key_file_set_uint64 (ews_summary->priv->key_file, folder_id,
			       "Total", total);

	ews_ss_hash_replace (ews_summary, g_strdup (folder_id), NULL);

	ews_summary->priv->dirty = TRUE;

	S_UNLOCK(ews_summary);
}


void
camel_ews_store_summary_set_parent_folder_id (CamelEwsStoreSummary *ews_summary,
					      const gchar *folder_id,
					      const gchar *parent_id)
{
	S_LOCK(ews_summary);

	g_key_file_set_string	(ews_summary->priv->key_file, folder_id,
				 "ParentFolderId", parent_id);
	ews_ss_hash_replace (ews_summary, g_strdup (folder_id), NULL);

	ews_summary->priv->dirty = TRUE;

	S_UNLOCK(ews_summary);
}

void
camel_ews_store_summary_set_change_key	(CamelEwsStoreSummary *ews_summary,
					 const gchar *folder_id,
					 const gchar *change_key)
{
	S_LOCK(ews_summary);

	g_key_file_set_string	(ews_summary->priv->key_file, folder_id,
				 "ChangeKey", change_key);
	ews_summary->priv->dirty = TRUE;

	S_UNLOCK(ews_summary);
}

void
camel_ews_store_summary_set_sync_state (CamelEwsStoreSummary *ews_summary,
					const gchar *folder_id,
					const gchar *sync_state)
{
	S_LOCK(ews_summary);

	g_key_file_set_string	(ews_summary->priv->key_file, folder_id,
				 "SyncState", sync_state);
	ews_summary->priv->dirty = TRUE;

	S_UNLOCK(ews_summary);
}

void
camel_ews_store_summary_set_folder_flags (CamelEwsStoreSummary *ews_summary,
					  const gchar *folder_id,
					  guint64 flags)
{
	S_LOCK(ews_summary);

	g_key_file_set_uint64	(ews_summary->priv->key_file, folder_id,
				 "Flags", flags);
	ews_summary->priv->dirty = TRUE;

	S_UNLOCK(ews_summary);
}

void
camel_ews_store_summary_set_folder_unread (CamelEwsStoreSummary *ews_summary,
					   const gchar *folder_id,
					   guint64 unread)
{
	S_LOCK(ews_summary);

	g_key_file_set_uint64	(ews_summary->priv->key_file, folder_id,
				 "UnRead", unread);
	ews_summary->priv->dirty = TRUE;

	S_UNLOCK(ews_summary);
}

void
camel_ews_store_summary_set_folder_total (CamelEwsStoreSummary *ews_summary,
					  const gchar *folder_id,
					  guint64 total)
{
	S_LOCK(ews_summary);

	g_key_file_set_uint64	(ews_summary->priv->key_file, folder_id,
				 "Total", total);
	ews_summary->priv->dirty = TRUE;

	S_UNLOCK(ews_summary);
}

void
camel_ews_store_summary_set_folder_type (CamelEwsStoreSummary *ews_summary,
					 const gchar *folder_id,
					 guint64 ews_folder_type)
{
	S_LOCK(ews_summary);

	g_key_file_set_uint64	(ews_summary->priv->key_file, folder_id,
				 "FolderType", ews_folder_type);
	ews_summary->priv->dirty = TRUE;

	S_UNLOCK(ews_summary);
}

void
camel_ews_store_summary_store_string_val (CamelEwsStoreSummary *ews_summary,
					  const gchar *key,
					  const gchar *value)
{
	S_LOCK(ews_summary);

	g_key_file_set_string	(ews_summary->priv->key_file, STORE_GROUP_NAME,
				 key, value);
	ews_summary->priv->dirty = TRUE;

	S_UNLOCK(ews_summary);
}

gchar *
camel_ews_store_summary_get_folder_name (CamelEwsStoreSummary *ews_summary,
					 const gchar *folder_id,
					 GError **error)
{
	gchar *ret;

	S_LOCK(ews_summary);

	ret = g_key_file_get_string	(ews_summary->priv->key_file, folder_id,
					 "DisplayName", error);

	S_UNLOCK(ews_summary);

	return ret;
}

gchar *
camel_ews_store_summary_get_folder_full_name (CamelEwsStoreSummary *ews_summary,
					      const gchar *folder_id,
					      GError **error)
{
	gchar *ret;

	S_LOCK(ews_summary);

	ret = g_hash_table_lookup (ews_summary->priv->id_fname_hash, folder_id);

	if (ret)
		ret = g_strdup (ret);

	S_UNLOCK(ews_summary);

	return ret;
}

gchar *
camel_ews_store_summary_get_parent_folder_id (CamelEwsStoreSummary *ews_summary,
					      const gchar *folder_id,
					      GError **error)
{
	gchar *ret;

	S_LOCK(ews_summary);

	ret = g_key_file_get_string	(ews_summary->priv->key_file, folder_id,
					 "ParentFolderId", error);

	S_UNLOCK(ews_summary);

	return ret;
}

gchar *
camel_ews_store_summary_get_change_key (CamelEwsStoreSummary *ews_summary,
					const gchar *folder_id,
					GError **error)
{
	gchar *ret;

	S_LOCK(ews_summary);

	ret = g_key_file_get_string	(ews_summary->priv->key_file, folder_id,
					 "ChangeKey", error);

	S_UNLOCK(ews_summary);

	return ret;
}

gchar *
camel_ews_store_summary_get_sync_state (CamelEwsStoreSummary *ews_summary,
					const gchar *folder_id,
					GError **error)
{
	gchar *ret;

	S_LOCK(ews_summary);

	ret = g_key_file_get_string	(ews_summary->priv->key_file, folder_id,
					 "SyncState", error);

	S_UNLOCK(ews_summary);

	return ret;
}

guint64
camel_ews_store_summary_get_folder_flags (CamelEwsStoreSummary *ews_summary,
					  const gchar *folder_id,
					  GError **error)
{
	guint64 ret;

	S_LOCK(ews_summary);

	ret = g_key_file_get_uint64	(ews_summary->priv->key_file, folder_id,
					 "Flags", error);

	S_UNLOCK(ews_summary);

	return ret;
}


guint64
camel_ews_store_summary_get_folder_unread (CamelEwsStoreSummary *ews_summary,
					   const gchar *folder_id,
					   GError **error)
{
	guint64 ret;

	S_LOCK(ews_summary);

	ret = g_key_file_get_uint64	(ews_summary->priv->key_file, folder_id,
					 "UnRead", error);

	S_UNLOCK(ews_summary);

	return ret;
}

guint64
camel_ews_store_summary_get_folder_total (CamelEwsStoreSummary *ews_summary,
					  const gchar *folder_id,
					  GError **error)
{
	guint64 ret;

	S_LOCK(ews_summary);

	ret = g_key_file_get_uint64	(ews_summary->priv->key_file, folder_id,
					 "Total", error);

	S_UNLOCK(ews_summary);

	return ret;
}

guint64
camel_ews_store_summary_get_folder_type (CamelEwsStoreSummary *ews_summary,
					 const gchar *folder_id,
					 GError **error)
{
	guint64 ret;

	S_LOCK(ews_summary);

	ret = g_key_file_get_uint64	(ews_summary->priv->key_file, folder_id,
					 "FolderType", error);

	S_UNLOCK(ews_summary);

	return ret;
}

gchar *
camel_ews_store_summary_get_string_val	(CamelEwsStoreSummary *ews_summary,
					 const gchar *key,
					 GError **error)
{
	gchar *ret;

	S_LOCK(ews_summary);

	ret = g_key_file_get_string	(ews_summary->priv->key_file, STORE_GROUP_NAME,
					 key, error);

	S_UNLOCK(ews_summary);

	return ret;
}

GSList *
camel_ews_store_summary_get_folders (CamelEwsStoreSummary *ews_summary,
				     const gchar *prefix)
{
	GSList *folders = NULL;
	gchar **groups = NULL;
	gsize length;
	int prefixlen = 0;
	gint i;

	if (prefix)
		prefixlen = strlen(prefix);

	S_LOCK(ews_summary);

	groups = g_key_file_get_groups (ews_summary->priv->key_file, &length);

	S_UNLOCK(ews_summary);

	for (i = 0; i < length; i++) {
		if (!g_ascii_strcasecmp (groups [i], STORE_GROUP_NAME))
			continue;
		if (prefix) {
			const gchar *fname = g_hash_table_lookup (ews_summary->priv->id_fname_hash, groups[i]);

			if (!fname || strncmp(fname, prefix, prefixlen) ||
			    (fname[prefixlen] && fname[prefixlen] != '/'))
				continue;
		}
		folders = g_slist_append (folders, g_strdup (groups [i]));
	}

	g_strfreev (groups);
	return folders;
}

gboolean
camel_ews_store_summary_remove_folder (CamelEwsStoreSummary *ews_summary,
				       const gchar *folder_id,
				       GError **error)
{
	gboolean ret = FALSE;
	gchar *full_name;

	S_LOCK(ews_summary);

	full_name = g_hash_table_lookup (ews_summary->priv->id_fname_hash, folder_id);
	if (!full_name)
		goto unlock;

	ret = g_key_file_remove_group (ews_summary->priv->key_file, folder_id,
				       error);

	g_hash_table_remove (ews_summary->priv->fname_id_hash, full_name);
	g_hash_table_remove (ews_summary->priv->id_fname_hash, folder_id);

	ews_summary->priv->dirty = TRUE;

 unlock:
	S_UNLOCK(ews_summary);

	return ret;
}

gchar *
camel_ews_store_summary_get_folder_id_from_name (CamelEwsStoreSummary *ews_summary,
						 const gchar *folder_name)
{
	gchar *folder_id;

	S_LOCK(ews_summary);

	folder_id = g_hash_table_lookup (ews_summary->priv->fname_id_hash, folder_name);
	if (folder_id)
		folder_id = g_strdup (folder_id);

	S_UNLOCK(ews_summary);

	return folder_id;
}

gboolean
camel_ews_store_summary_has_folder (CamelEwsStoreSummary *ews_summary, const gchar *folder_id)
{
	gboolean ret;

	S_LOCK(ews_summary);

	ret = g_key_file_has_group (ews_summary->priv->key_file, folder_id);

	S_UNLOCK(ews_summary);

	return ret;
}
