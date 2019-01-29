/* GStreamer Editing Services
 *
 * Copyright (C) 2012 Thibault Saunier <thibault.saunier@collabora.com>
 * Copyright (C) 2012 Volodymyr Rudyi <vladimir.rudoy@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */
/**
 * SECTION: gesuriclipasset
 * @title: GESUriClipAsset
 * @short_description: A GESAsset subclass specialized in GESUriClip extraction
 *
 * The #GESUriClipAsset is a special #GESAsset that lets you handle
 * the media file to use inside the GStreamer Editing Services. It has APIs that
 * let you get information about the medias. Also, the tags found in the media file are
 * set as Metadata of the Asset.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <gst/pbutils/pbutils.h>
#include "ges.h"
#include "ges-internal.h"
#include "ges-track-element-asset.h"

#define DEFAULT_DISCOVERY_TIMEOUT (60 * GST_SECOND)

static GHashTable *parent_newparent_table = NULL;

static GstDiscoverer *discoverer = NULL;

static void
initable_iface_init (GInitableIface * initable_iface)
{
  /*  We can not iniate synchronously */
  initable_iface->init = NULL;
}

/* TODO: We should monitor files here, and add some way of reporting changes
 * to user
 */
enum
{
  PROP_0,
  PROP_DURATION,
  PROP_LAST
};
static GParamSpec *properties[PROP_LAST];

static void discoverer_discovered_cb (GstDiscoverer * discoverer,
    GstDiscovererInfo * info, GError * err, gpointer user_data);

struct _GESUriClipAssetPrivate
{
  GstDiscovererInfo *info;
  GstClockTime duration;
  gboolean is_image;

  GList *asset_trackfilesources;
};

typedef struct
{
  GMainLoop *ml;
  GESAsset *asset;
  GError *error;
} RequestSyncData;

struct _GESUriSourceAssetPrivate
{
  GstDiscovererStreamInfo *sinfo;
  GESUriClipAsset *parent_asset;

  const gchar *uri;
};

G_DEFINE_TYPE_WITH_CODE (GESUriClipAsset, ges_uri_clip_asset,
    GES_TYPE_CLIP_ASSET, G_ADD_PRIVATE (GESUriClipAsset)
    G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init));

static void
ges_uri_clip_asset_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GESUriClipAssetPrivate *priv = GES_URI_CLIP_ASSET (object)->priv;

  switch (property_id) {
    case PROP_DURATION:
      g_value_set_uint64 (value, priv->duration);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
ges_uri_clip_asset_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GESUriClipAssetPrivate *priv = GES_URI_CLIP_ASSET (object)->priv;

  switch (property_id) {
    case PROP_DURATION:
      priv->duration = g_value_get_uint64 (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static GESAssetLoadingReturn
_start_loading (GESAsset * asset, GError ** error)
{
  gboolean ret;
  const gchar *uri;
  GESUriClipAssetClass *class = GES_URI_CLIP_ASSET_GET_CLASS (asset);

  GST_DEBUG ("Started loading %p", asset);

  uri = ges_asset_get_id (asset);

  ret = gst_discoverer_discover_uri_async (class->discoverer, uri);
  if (ret)
    return GES_ASSET_LOADING_ASYNC;

  return GES_ASSET_LOADING_ERROR;
}

static gboolean
_request_id_update (GESAsset * self, gchar ** proposed_new_id, GError * error)
{
  if (error->domain == GST_RESOURCE_ERROR &&
      (error->code == GST_RESOURCE_ERROR_NOT_FOUND ||
          error->code == GST_RESOURCE_ERROR_OPEN_READ)) {
    const gchar *uri = ges_asset_get_id (self);
    GFile *parent, *file = g_file_new_for_uri (uri);

    /* Check if we have the new parent in cache */
    parent = g_file_get_parent (file);
    if (parent) {
      GFile *new_parent = g_hash_table_lookup (parent_newparent_table, parent);

      if (new_parent) {
        gchar *basename = g_file_get_basename (file);
        GFile *new_file = g_file_get_child (new_parent, basename);

        /* FIXME Handle the GCancellable */
        if (g_file_query_exists (new_file, NULL)) {
          *proposed_new_id = g_file_get_uri (new_file);
          GST_DEBUG_OBJECT (self, "Proposing path: %s as proxy",
              *proposed_new_id);
        }

        gst_object_unref (new_file);
        g_free (basename);
      }
      gst_object_unref (parent);
    }

    gst_object_unref (file);

    return TRUE;
  }

  return FALSE;
}

static void
_asset_proxied (GESAsset * self, const gchar * new_uri)
{
  const gchar *uri = ges_asset_get_id (self);
  GFile *parent, *new_parent, *new_file = g_file_new_for_uri (new_uri),
      *file = g_file_new_for_uri (uri);

  parent = g_file_get_parent (file);
  new_parent = g_file_get_parent (new_file);
  g_hash_table_insert (parent_newparent_table, parent, new_parent);
  gst_object_unref (file);
  gst_object_unref (new_file);
}

static void
ges_uri_clip_asset_dispose (GObject * object)
{
  GESUriClipAsset *self = GES_URI_CLIP_ASSET (object);
  GESUriClipAssetPrivate *prif = self->priv;

  if (prif->asset_trackfilesources) {
    g_list_free_full (prif->asset_trackfilesources,
        (GDestroyNotify) gst_object_unref);
    prif->asset_trackfilesources = NULL;
  }

  gst_clear_object (&prif->info);

  G_OBJECT_CLASS (ges_uri_clip_asset_parent_class)->dispose (object);
}

static void
ges_uri_clip_asset_class_init (GESUriClipAssetClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = ges_uri_clip_asset_get_property;
  object_class->set_property = ges_uri_clip_asset_set_property;
  object_class->dispose = ges_uri_clip_asset_dispose;

  GES_ASSET_CLASS (klass)->start_loading = _start_loading;
  GES_ASSET_CLASS (klass)->request_id_update = _request_id_update;
  GES_ASSET_CLASS (klass)->inform_proxy = _asset_proxied;

  klass->discovered = discoverer_discovered_cb;


  /**
   * GESUriClipAsset:duration:
   *
   * The duration (in nanoseconds) of the media file
   */
  properties[PROP_DURATION] =
      g_param_spec_uint64 ("duration", "Duration", "The duration to use", 0,
      G_MAXUINT64, GST_CLOCK_TIME_NONE, G_PARAM_READWRITE);
  g_object_class_install_property (object_class, PROP_DURATION,
      properties[PROP_DURATION]);

  _ges_uri_asset_ensure_setup (klass);
}

static void
ges_uri_clip_asset_init (GESUriClipAsset * self)
{
  GESUriClipAssetPrivate *priv;

  priv = self->priv = ges_uri_clip_asset_get_instance_private (self);

  priv->info = NULL;
  priv->duration = GST_CLOCK_TIME_NONE;
  priv->is_image = FALSE;
}

static void
_create_uri_source_asset (GESUriClipAsset * asset,
    GstDiscovererStreamInfo * sinfo, GESTrackType type)
{
  GESAsset *tck_filesource_asset;
  GESUriSourceAssetPrivate *priv_tckasset;
  GESUriClipAssetPrivate *priv = asset->priv;
  gchar *stream_id =
      g_strdup (gst_discoverer_stream_info_get_stream_id (sinfo));

  if (stream_id == NULL) {
    GST_WARNING ("No stream ID found, using the pointer instead");

    stream_id = g_strdup_printf ("%i", GPOINTER_TO_INT (sinfo));
  }

  if (type == GES_TRACK_TYPE_VIDEO)
    tck_filesource_asset = ges_asset_request (GES_TYPE_VIDEO_URI_SOURCE,
        stream_id, NULL);
  else
    tck_filesource_asset = ges_asset_request (GES_TYPE_AUDIO_URI_SOURCE,
        stream_id, NULL);
  g_free (stream_id);

  priv_tckasset = GES_URI_SOURCE_ASSET (tck_filesource_asset)->priv;
  priv_tckasset->uri = ges_asset_get_id (GES_ASSET (asset));
  priv_tckasset->sinfo = gst_object_ref (sinfo);
  priv_tckasset->parent_asset = asset;
  ges_track_element_asset_set_track_type (GES_TRACK_ELEMENT_ASSET
      (tck_filesource_asset), type);

  priv->asset_trackfilesources = g_list_append (priv->asset_trackfilesources,
      tck_filesource_asset);
}

static void
ges_uri_clip_asset_set_info (GESUriClipAsset * self, GstDiscovererInfo * info)
{
  GList *tmp, *stream_list;

  GESTrackType supportedformats = GES_TRACK_TYPE_UNKNOWN;
  GESUriClipAssetPrivate *priv = GES_URI_CLIP_ASSET (self)->priv;

  /* Extract infos from the GstDiscovererInfo */
  stream_list = gst_discoverer_info_get_stream_list (info);
  for (tmp = stream_list; tmp; tmp = tmp->next) {
    GESTrackType type = GES_TRACK_TYPE_UNKNOWN;
    GstDiscovererStreamInfo *sinf = (GstDiscovererStreamInfo *) tmp->data;

    if (GST_IS_DISCOVERER_AUDIO_INFO (sinf)) {
      if (supportedformats == GES_TRACK_TYPE_UNKNOWN)
        supportedformats = GES_TRACK_TYPE_AUDIO;
      else
        supportedformats |= GES_TRACK_TYPE_AUDIO;

      type = GES_TRACK_TYPE_AUDIO;
    } else if (GST_IS_DISCOVERER_VIDEO_INFO (sinf)) {
      if (supportedformats == GES_TRACK_TYPE_UNKNOWN)
        supportedformats = GES_TRACK_TYPE_VIDEO;
      else
        supportedformats |= GES_TRACK_TYPE_VIDEO;
      if (gst_discoverer_video_info_is_image ((GstDiscovererVideoInfo *)
              sinf))
        priv->is_image = TRUE;
      type = GES_TRACK_TYPE_VIDEO;
    }

    GST_DEBUG_OBJECT (self, "Creating GESUriSourceAsset for stream: %s",
        gst_discoverer_stream_info_get_stream_id (sinf));
    _create_uri_source_asset (self, sinf, type);
  }
  ges_clip_asset_set_supported_formats (GES_CLIP_ASSET
      (self), supportedformats);

  if (stream_list)
    gst_discoverer_stream_info_list_free (stream_list);

  if (priv->is_image == FALSE)
    priv->duration = gst_discoverer_info_get_duration (info);
  /* else we keep #GST_CLOCK_TIME_NONE */

  priv->info = gst_object_ref (info);
}

static void
_set_meta_file_size (const gchar * uri, GESUriClipAsset * asset)
{
  GError *error = NULL;
  GFileInfo *file_info = NULL;
  guint64 file_size;
  GFile *gfile = NULL;

  GESMetaContainer *container = GES_META_CONTAINER (asset);

  gfile = g_file_new_for_uri (uri);
  file_info = g_file_query_info (gfile, "standard::size",
      G_FILE_QUERY_INFO_NONE, NULL, &error);
  if (!error) {
    file_size = g_file_info_get_attribute_uint64 (file_info, "standard::size");
    ges_meta_container_register_meta_uint64 (container, GES_META_READ_WRITE,
        "file-size", file_size);
  } else {
    g_error_free (error);
  }
  if (gfile)
    g_object_unref (gfile);
  if (file_info)
    g_object_unref (file_info);
}

static void
_set_meta_foreach (const GstTagList * tags, const gchar * tag,
    GESMetaContainer * container)
{
  GValue value = { 0 };

  if (gst_tag_list_copy_value (&value, tags, tag)) {
    ges_meta_container_set_meta (container, tag, &value);
    g_value_unset (&value);
  } else {
    GST_INFO ("Could not set metadata: %s", tag);
  }
}

static void
discoverer_discovered_cb (GstDiscoverer * discoverer,
    GstDiscovererInfo * info, GError * err, gpointer user_data)
{
  GError *error = NULL;
  const GstTagList *tags;

  const gchar *uri = gst_discoverer_info_get_uri (info);
  GESUriClipAsset *mfs =
      GES_URI_CLIP_ASSET (ges_asset_cache_lookup (GES_TYPE_URI_CLIP, uri));

  tags = gst_discoverer_info_get_tags (info);
  if (tags)
    gst_tag_list_foreach (tags, (GstTagForeachFunc) _set_meta_foreach, mfs);

  _set_meta_file_size (uri, mfs);

  if (gst_discoverer_info_get_result (info) == GST_DISCOVERER_OK) {
    ges_uri_clip_asset_set_info (mfs, info);
  } else {
    if (err) {
      error = g_error_copy (err);
    } else {
      error = g_error_new (GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_FAILED,
          "Stream %s discovering failed (error code: %d)",
          uri, gst_discoverer_info_get_result (info));
    }
  }

  ges_asset_cache_set_loaded (GES_TYPE_URI_CLIP, uri, error);

  if (error)
    g_error_free (error);
}

static void
asset_ready_cb (GESAsset * source, GAsyncResult * res, RequestSyncData * data)
{
  data->asset = ges_asset_request_finish (res, &data->error);

  if (data->error) {
    gchar *possible_uri = ges_uri_asset_try_update_id (data->error, source);

    if (possible_uri) {
      g_clear_error (&data->error);
      ges_asset_request_async (GES_TYPE_URI_CLIP, possible_uri, NULL,
          (GAsyncReadyCallback) asset_ready_cb, data);
      g_free (possible_uri);

      return;
    }
  }
  g_main_loop_quit (data->ml);
}

/* API implementation */
/**
 * ges_uri_clip_asset_get_info:
 * @self: Target asset
 *
 * Gets #GstDiscovererInfo about the file
 *
 * Returns: (transfer none): #GstDiscovererInfo of specified asset
 */
GstDiscovererInfo *
ges_uri_clip_asset_get_info (const GESUriClipAsset * self)
{
  g_return_val_if_fail (GES_IS_URI_CLIP_ASSET (self), NULL);

  return self->priv->info;
}

/**
 * ges_uri_clip_asset_get_duration:
 * @self: a #GESUriClipAsset
 *
 * Gets duration of the file represented by @self
 *
 * Returns: The duration of @self
 */
GstClockTime
ges_uri_clip_asset_get_duration (GESUriClipAsset * self)
{
  g_return_val_if_fail (GES_IS_URI_CLIP_ASSET (self), GST_CLOCK_TIME_NONE);

  return self->priv->duration;
}

/**
 * ges_uri_clip_asset_is_image:
 * @self: a #indent: Standard input:311: Error:Unexpected end of file
GESUriClipAsset
 *
 * Gets Whether the file represented by @self is an image or not
 *
 * Returns: Whether the file represented by @self is an image or not
 */
gboolean
ges_uri_clip_asset_is_image (GESUriClipAsset * self)
{
  g_return_val_if_fail (GES_IS_URI_CLIP_ASSET (self), FALSE);

  return self->priv->is_image;
}

/**
 * ges_uri_clip_asset_new:
 * @uri: The URI of the file for which to create a #GESUriClipAsset
 * @cancellable: optional %GCancellable object, %NULL to ignore.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the initialization is finished
 * @user_data: The user data to pass when @callback is called
 *
 * Creates a #GESUriClipAsset for @uri
 *
 * Example of request of a GESUriClipAsset:
 * |[
 * // The request callback
 * static void
 * filesource_asset_loaded_cb (GESAsset * source, GAsyncResult * res, gpointer user_data)
 * {
 *   GError *error = NULL;
 *   GESUriClipAsset *filesource_asset;
 *
 *   filesource_asset = ges_uri_clip_asset_finish (res, &error);
 *   if (filesource_asset) {
 *    g_print ("The file: %s is usable as a FileSource, it is%s an image and lasts %" GST_TIME_FORMAT,
 *        ges_asset_get_id (GES_ASSET (filesource_asset))
 *        ges_uri_clip_asset_is_image (filesource_asset) ? "" : " not",
 *        GST_TIME_ARGS (ges_uri_clip_asset_get_duration (filesource_asset));
 *   } else {
 *    g_print ("The file: %s is *not* usable as a FileSource because: %s",
 *        ges_asset_get_id (source), error->message);
 *   }
 *
 *   gst_object_unref (mfs);
 * }
 *
 * // The request:
 * ges_uri_clip_asset_new (uri, (GAsyncReadyCallback) filesource_asset_loaded_cb, user_data);
 * ]|
 */
void
ges_uri_clip_asset_new (const gchar * uri, GCancellable * cancellable,
    GAsyncReadyCallback callback, gpointer user_data)
{
  ges_asset_request_async (GES_TYPE_URI_CLIP, uri, cancellable,
      callback, user_data);
}

/**
 * ges_uri_clip_asset_finish:
 * @res: The #GAsyncResult from which to get the newly created #GESUriClipAsset
 * @error: An error to be set in case something wrong happens or %NULL
 *
 * Finalize the request of an async #GESUriClipAsset
 *
 * Returns: (transfer full): The #GESUriClipAsset previously requested
 */
GESUriClipAsset *
ges_uri_clip_asset_finish (GAsyncResult * res, GError ** error)
{
  GESAsset *asset;

  g_return_val_if_fail (G_IS_ASYNC_RESULT (res), NULL);

  asset = ges_asset_request_finish (res, error);
  if (asset != NULL) {
    return GES_URI_CLIP_ASSET (asset);
  }

  return NULL;
}

/**
 * ges_uri_clip_asset_request_sync:
 * @uri: The URI of the file for which to create a #GESUriClipAsset.
 * You can also use multi file uris for #GESMultiFileSource.
 * @error: An error to be set in case something wrong happens or %NULL
 *
 * Creates a #GESUriClipAsset for @uri syncronously. You should avoid
 * to use it in application, and rather create #GESUriClipAsset asynchronously
 *
 * Returns: (transfer full): A reference to the requested asset or %NULL if
 * an error happened
 */
GESUriClipAsset *
ges_uri_clip_asset_request_sync (const gchar * uri, GError ** error)
{
  GError *lerror = NULL;
  GESUriClipAsset *asset;
  RequestSyncData data = { 0, };

  asset = GES_URI_CLIP_ASSET (ges_asset_request (GES_TYPE_URI_CLIP, uri,
          &lerror));

  if (asset)
    return asset;

  data.ml = g_main_loop_new (NULL, TRUE);
  ges_asset_request_async (GES_TYPE_URI_CLIP, uri, NULL,
      (GAsyncReadyCallback) asset_ready_cb, &data);
  g_main_loop_run (data.ml);
  g_main_loop_unref (data.ml);

  if (data.error) {
    GST_ERROR ("Got an error requesting asset: %s", data.error->message);
    if (error != NULL)
      g_propagate_error (error, data.error);

    return NULL;
  }

  return GES_URI_CLIP_ASSET (data.asset);
}

/**
 * ges_uri_clip_asset_class_set_timeout:
 * @klass: The #GESUriClipAssetClass on which to set the discoverer timeout
 * @timeout: The timeout to set
 *
 * Sets the timeout of #GESUriClipAsset loading
 */
void
ges_uri_clip_asset_class_set_timeout (GESUriClipAssetClass * klass,
    GstClockTime timeout)
{
  g_return_if_fail (GES_IS_URI_CLIP_ASSET_CLASS (klass));

  g_object_set (klass->discoverer, "timeout", timeout, NULL);
}

/**
 * ges_uri_clip_asset_get_stream_assets:
 * @self: A #GESUriClipAsset
 *
 * Get the GESUriSourceAsset @self containes
 *
 * Returns: (transfer none) (element-type GESUriSourceAsset): a
 * #GList of #GESUriSourceAsset
 */
const GList *
ges_uri_clip_asset_get_stream_assets (GESUriClipAsset * self)
{
  g_return_val_if_fail (GES_IS_URI_CLIP_ASSET (self), FALSE);

  return self->priv->asset_trackfilesources;
}

/*****************************************************************
 *            GESUriSourceAsset implementation             *
 *****************************************************************/
/**
 * SECTION: gesurisourceasset
 * @title: GESUriClipAsset
 * @short_description: A GESAsset subclass specialized in GESUriSource extraction
 *
 * NOTE: You should never request such a #GESAsset as they will be created automatically
 * by #GESUriClipAsset-s.
 */

G_DEFINE_TYPE_WITH_PRIVATE (GESUriSourceAsset, ges_uri_source_asset,
    GES_TYPE_TRACK_ELEMENT_ASSET);

static GESExtractable *
_extract (GESAsset * asset, GError ** error)
{
  gchar *uri = NULL;
  GESTrackElement *trackelement;
  GESUriSourceAssetPrivate *priv = GES_URI_SOURCE_ASSET (asset)->priv;

  if (GST_IS_DISCOVERER_STREAM_INFO (priv->sinfo) == FALSE) {
    GST_WARNING_OBJECT (asset, "Can not extract as no strean info set");

    return NULL;
  }

  if (priv->uri == NULL) {
    GST_WARNING_OBJECT (asset, "Can not extract as no uri set");

    return NULL;
  }

  uri = g_strdup (priv->uri);

  if (g_str_has_prefix (priv->uri, GES_MULTI_FILE_URI_PREFIX)) {
    trackelement = GES_TRACK_ELEMENT (ges_multi_file_source_new (uri));
  } else if (GST_IS_DISCOVERER_VIDEO_INFO (priv->sinfo)
      && gst_discoverer_video_info_is_image ((GstDiscovererVideoInfo *)
          priv->sinfo))
    trackelement = GES_TRACK_ELEMENT (ges_image_source_new (uri));
  else if (GST_IS_DISCOVERER_VIDEO_INFO (priv->sinfo))
    trackelement = GES_TRACK_ELEMENT (ges_video_uri_source_new (uri));
  else
    trackelement = GES_TRACK_ELEMENT (ges_audio_uri_source_new (uri));

  ges_track_element_set_track_type (trackelement,
      ges_track_element_asset_get_track_type (GES_TRACK_ELEMENT_ASSET (asset)));

  g_free (uri);

  return GES_EXTRACTABLE (trackelement);
}

static void
ges_uri_source_asset_dispose (GObject * object)
{
  GESUriSourceAsset *self = GES_URI_SOURCE_ASSET (object);
  GESUriSourceAssetPrivate *priv = self->priv;

  gst_clear_object (&priv->sinfo);

  G_OBJECT_CLASS (ges_uri_source_asset_parent_class)->dispose (object);
}

static void
ges_uri_source_asset_class_init (GESUriSourceAssetClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = ges_uri_source_asset_dispose;

  GES_ASSET_CLASS (klass)->extract = _extract;
}

static void
ges_uri_source_asset_init (GESUriSourceAsset * self)
{
  GESUriSourceAssetPrivate *priv;

  priv = self->priv = ges_uri_source_asset_get_instance_private (self);

  priv->sinfo = NULL;
  priv->parent_asset = NULL;
  priv->uri = NULL;
}

/**
 * ges_uri_source_asset_get_stream_info:
 * @asset: A #GESUriClipAsset
 *
 * Get the #GstDiscovererStreamInfo user by @asset
 *
 * Returns: (transfer none): a #GESUriClipAsset
 */
GstDiscovererStreamInfo *
ges_uri_source_asset_get_stream_info (GESUriSourceAsset * asset)
{
  g_return_val_if_fail (GES_IS_URI_SOURCE_ASSET (asset), NULL);

  return asset->priv->sinfo;
}

const gchar *
ges_uri_source_asset_get_stream_uri (GESUriSourceAsset * asset)
{
  g_return_val_if_fail (GES_IS_URI_SOURCE_ASSET (asset), NULL);

  return asset->priv->uri;
}

/**
 * ges_uri_source_asset_get_filesource_asset:
 * @asset: A #GESUriClipAsset
 *
 * Get the #GESUriClipAsset @self is contained in
 *
 * Returns: a #GESUriClipAsset
 */
const GESUriClipAsset *
ges_uri_source_asset_get_filesource_asset (GESUriSourceAsset * asset)
{
  g_return_val_if_fail (GES_IS_URI_SOURCE_ASSET (asset), NULL);

  return asset->priv->parent_asset;
}

void
_ges_uri_asset_cleanup (void)
{
  g_clear_object (&discoverer);
  if (parent_newparent_table) {
    g_hash_table_destroy (parent_newparent_table);
    parent_newparent_table = NULL;
  }
}

gboolean
_ges_uri_asset_ensure_setup (gpointer uriasset_class)
{
  GESUriClipAssetClass *klass;
  GError *err;
  GstClockTime timeout;
  const gchar *timeout_str;

  g_return_val_if_fail (GES_IS_URI_CLIP_ASSET_CLASS (uriasset_class), FALSE);

  klass = GES_URI_CLIP_ASSET_CLASS (uriasset_class);

  timeout = DEFAULT_DISCOVERY_TIMEOUT;
  errno = 0;
  timeout_str = g_getenv ("GES_DISCOVERY_TIMEOUT");
  if (timeout_str)
    timeout = g_ascii_strtod (timeout_str, NULL) * GST_SECOND;
  else
    errno = 10;

  if (errno)
    timeout = DEFAULT_DISCOVERY_TIMEOUT;

  if (!discoverer) {
    discoverer = gst_discoverer_new (timeout, &err);
    if (!discoverer) {
      GST_ERROR ("Could not create discoverer: %s", err->message);
      g_error_free (err);
      return FALSE;
    }
  }

  /* The class structure keeps weak pointers on the discoverers so they
   * can be properly cleaned up in _ges_uri_asset_cleanup(). */
  if (!klass->discoverer) {
    klass->discoverer = klass->sync_discoverer = discoverer;
    g_object_add_weak_pointer (G_OBJECT (discoverer),
        (gpointer *) & klass->discoverer);
    g_object_add_weak_pointer (G_OBJECT (discoverer),
        (gpointer *) & klass->sync_discoverer);

    g_signal_connect (klass->discoverer, "discovered",
        G_CALLBACK (klass->discovered), NULL);
  }

  /* We just start the discoverer and let it live */
  gst_discoverer_start (klass->discoverer);
  if (parent_newparent_table == NULL) {
    parent_newparent_table = g_hash_table_new_full (g_file_hash,
        (GEqualFunc) g_file_equal, g_object_unref, g_object_unref);
  }

  return TRUE;
}
