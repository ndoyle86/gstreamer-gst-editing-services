/* GStreamer Editing Services
 * Copyright (C) 2009 Edward Hervey <edward.hervey@collabora.co.uk>
 *               2009 Nokia Corporation
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:gesvideourisource
 * @title: GESVideoUriSource
 * @short_description: outputs a single video stream from a given file
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>

#include <gst/pbutils/missing-plugins.h>
#include "ges-utils.h"
#include "ges-internal.h"
#include "ges-track-element.h"
#include "ges-video-uri-source.h"
#include "ges-uri-asset.h"
#include "ges-extractable.h"

struct _GESVideoUriSourcePrivate
{
  GstElement *decodebin;        /* Reference owned by parent class */
};

enum
{
  PROP_0,
  PROP_URI
};

static void
ges_video_uri_source_track_set_cb (GESVideoUriSource * self,
    GParamSpec * arg G_GNUC_UNUSED, gpointer nothing)
{
  GESTrack *track;
  const GstCaps *caps = NULL;

  if (!self->priv->decodebin)
    return;

  track = ges_track_element_get_track (GES_TRACK_ELEMENT (self));
  if (!track)
    return;

  caps = ges_track_get_caps (track);

  GST_INFO_OBJECT (self, "Setting caps to: %" GST_PTR_FORMAT, caps);
  g_object_set (self->priv->decodebin, "caps", caps, NULL);
}

/* GESSource VMethod */
static GstElement *
ges_video_uri_source_create_source (GESTrackElement * trksrc)
{
  GESVideoUriSource *self;
  GESTrack *track;
  GstElement *decodebin;
  const GstCaps *caps = NULL;

  self = (GESVideoUriSource *) trksrc;

  track = ges_track_element_get_track (trksrc);
  if (track)
    caps = ges_track_get_caps (track);

  decodebin = self->priv->decodebin = gst_element_factory_make ("uridecodebin",
      NULL);

  g_object_set (decodebin, "caps", caps,
      "expose-all-streams", FALSE, "uri", self->uri, NULL);

  return decodebin;
}

static gboolean
ges_video_uri_source_needs_converters (GESVideoSource * source)
{
  GESTrack *track = ges_track_element_get_track (GES_TRACK_ELEMENT (source));

  if (!track || ges_track_get_mixing (track)) {
    GESAsset *asset = ges_asset_request (GES_TYPE_URI_CLIP,
        GES_VIDEO_URI_SOURCE (source)->uri, NULL);
    gboolean is_nested = FALSE;

    g_assert (asset);

    g_object_get (asset, "is-nested-timeline", &is_nested, NULL);
    gst_object_unref (asset);

    return !is_nested;
  }


  return FALSE;
}

static GstDiscovererVideoInfo *
_get_video_stream_info (GESVideoUriSource * self)
{
  GstDiscovererStreamInfo *info;
  GESAsset *asset = ges_extractable_get_asset (GES_EXTRACTABLE (self));

  if (!asset) {
    GST_DEBUG_OBJECT (self, "No asset set yet");
    return NULL;
  }

  info = ges_uri_source_asset_get_stream_info (GES_URI_SOURCE_ASSET (asset));

  if (!GST_IS_DISCOVERER_VIDEO_INFO (info)) {
    GST_ERROR_OBJECT (self, "Doesn't have a video info (%" GST_PTR_FORMAT
        ")", info);
    return NULL;
  }

  return GST_DISCOVERER_VIDEO_INFO (info);
}


gboolean
ges_video_uri_source_get_natural_size (GESVideoSource * source, gint * width,
    gint * height)
{
  const GstTagList *tags = NULL;
  gchar *rotation_info = NULL;
  gint videoflip_method, rotate_angle;
  GstDiscovererVideoInfo *info =
      _get_video_stream_info (GES_VIDEO_URI_SOURCE (source));

  if (!info)
    return FALSE;

  *width = gst_discoverer_video_info_get_width (info);
  *height = gst_discoverer_video_info_get_height (info);
  if (!ges_timeline_element_lookup_child (GES_TIMELINE_ELEMENT (source),
          "GstVideoFlip::video-direction", NULL, NULL))
    goto done;

  ges_timeline_element_get_child_properties (GES_TIMELINE_ELEMENT (source),
      "GstVideoFlip::video-direction", &videoflip_method, NULL);

  /* Rotating 90 degrees, either way, rotate */
  if (videoflip_method == 1 || videoflip_method == 3)
    goto rotate;

  if (videoflip_method != 8)
    goto done;

  /* Rotation is automatic, we need to check if the media file is naturally
     rotated */
  tags =
      gst_discoverer_stream_info_get_tags (GST_DISCOVERER_STREAM_INFO (info));
  if (!tags)
    goto done;

  if (!gst_tag_list_get_string (tags, GST_TAG_IMAGE_ORIENTATION,
          &rotation_info))
    goto done;

  if (sscanf (rotation_info, "rotate-%d", &rotate_angle) == 1) {
    if (rotate_angle == 90 || rotate_angle == 270)
      goto rotate;
  }

done:
  g_free (rotation_info);
  return TRUE;

rotate:
  GST_INFO_OBJECT (source, "Stream is rotated, taking that into account");
  *width =
      gst_discoverer_video_info_get_height (GST_DISCOVERER_VIDEO_INFO (info));
  *height =
      gst_discoverer_video_info_get_width (GST_DISCOVERER_VIDEO_INFO (info));

  goto done;
}

/* Extractable interface implementation */

static gchar *
ges_extractable_check_id (GType type, const gchar * id, GError ** error)
{
  return g_strdup (id);
}

static void
ges_extractable_interface_init (GESExtractableInterface * iface)
{
  iface->asset_type = GES_TYPE_URI_SOURCE_ASSET;
  iface->check_id = ges_extractable_check_id;
}

G_DEFINE_TYPE_WITH_CODE (GESVideoUriSource, ges_video_uri_source,
    GES_TYPE_VIDEO_SOURCE, G_ADD_PRIVATE (GESVideoUriSource)
    G_IMPLEMENT_INTERFACE (GES_TYPE_EXTRACTABLE,
        ges_extractable_interface_init));

static gboolean
_find_positioner (GstElement * a, GstElement * b)
{
  return !g_strcmp0 (GST_OBJECT_NAME (gst_element_get_factory (a)),
      "framepositioner");
}

static void
post_missing_element_message (GstElement * element, const gchar * name)
{
  GstMessage *msg;

  msg = gst_missing_element_message_new (element, name);
  gst_element_post_message (element, msg);
}

/* GObject VMethods */
static gboolean
ges_video_uri_source_create_filters (GESVideoSource * source,
    GPtrArray * elements, gboolean needs_converters)
{
  GESAsset *asset = ges_extractable_get_asset (GES_EXTRACTABLE (source));
  GstDiscovererVideoInfo *info =
      GST_DISCOVERER_VIDEO_INFO (ges_uri_source_asset_get_stream_info
      (GES_URI_SOURCE_ASSET (asset)));

  g_assert (GES_IS_URI_SOURCE_ASSET (asset));
  if (!GES_VIDEO_SOURCE_CLASS (ges_video_uri_source_parent_class)
      ->ABI.abi.create_filters (source, elements, needs_converters))
    return FALSE;

  if (gst_discoverer_video_info_is_interlaced (info)) {
    const gchar *deinterlace_props[] = { "mode", "fields", "tff", NULL };
    GstElement *deinterlace =
        gst_element_factory_make ("deinterlace", "deinterlace");

    if (deinterlace == NULL) {
      post_missing_element_message (ges_track_element_get_nleobject
          (GES_TRACK_ELEMENT (source)), "deinterlace");

      GST_ELEMENT_WARNING (ges_track_element_get_nleobject (GES_TRACK_ELEMENT
              (source)), CORE, MISSING_PLUGIN,
          ("Missing element '%s' - check your GStreamer installation.",
              "deinterlace"), ("deinterlacing won't work"));
    } else {
      /* Right after the queue */
      g_ptr_array_insert (elements, 1, gst_element_factory_make ("videoconvert",
              NULL));
      g_ptr_array_insert (elements, 2, deinterlace);
      ges_track_element_add_children_props (GES_TRACK_ELEMENT (source),
          deinterlace, NULL, NULL, deinterlace_props);
    }

  }

  if (ges_uri_source_asset_is_image (GES_URI_SOURCE_ASSET (asset))) {
    guint i;

    g_ptr_array_find_with_equal_func (elements, NULL,
        (GEqualFunc) _find_positioner, &i);
    /* Adding the imagefreeze right before the positionner so positioning can happen
     * properly */
    g_ptr_array_insert (elements, i,
        gst_element_factory_make ("imagefreeze", NULL));
  }

  return TRUE;
}

static void
ges_video_uri_source_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GESVideoUriSource *urisource = GES_VIDEO_URI_SOURCE (object);

  switch (property_id) {
    case PROP_URI:
      g_value_set_string (value, urisource->uri);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
ges_video_uri_source_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GESVideoUriSource *urisource = GES_VIDEO_URI_SOURCE (object);

  switch (property_id) {
    case PROP_URI:
      if (urisource->uri) {
        GST_WARNING_OBJECT (object, "Uri already set to %s", urisource->uri);
        return;
      }
      urisource->uri = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
ges_video_uri_source_dispose (GObject * object)
{
  GESVideoUriSource *urisource = GES_VIDEO_URI_SOURCE (object);

  g_free (urisource->uri);

  G_OBJECT_CLASS (ges_video_uri_source_parent_class)->dispose (object);
}

static void
ges_video_uri_source_class_init (GESVideoUriSourceClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GESVideoSourceClass *source_class = GES_VIDEO_SOURCE_CLASS (klass);

  object_class->get_property = ges_video_uri_source_get_property;
  object_class->set_property = ges_video_uri_source_set_property;
  object_class->dispose = ges_video_uri_source_dispose;

  /**
   * GESVideoUriSource:uri:
   *
   * The location of the file/resource to use.
   */
  g_object_class_install_property (object_class, PROP_URI,
      g_param_spec_string ("uri", "URI", "uri of the resource",
          NULL, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  source_class->create_source = ges_video_uri_source_create_source;
  source_class->ABI.abi.needs_converters =
      ges_video_uri_source_needs_converters;
  source_class->ABI.abi.get_natural_size =
      ges_video_uri_source_get_natural_size;
  source_class->ABI.abi.create_filters = ges_video_uri_source_create_filters;
}

static void
ges_video_uri_source_init (GESVideoUriSource * self)
{
  self->priv = ges_video_uri_source_get_instance_private (self);

  g_signal_connect (self, "notify::track",
      G_CALLBACK (ges_video_uri_source_track_set_cb), NULL);
}

/**
 * ges_video_uri_source_new:
 * @uri: the URI the source should control
 *
 * Creates a new #GESVideoUriSource for the provided @uri.
 *
 * Returns: (transfer floating) (nullable): The newly created #GESVideoUriSource,
 * or %NULL if there was an error.
 */
GESVideoUriSource *
ges_video_uri_source_new (gchar * uri)
{
  return g_object_new (GES_TYPE_VIDEO_URI_SOURCE, "uri", uri, NULL);
}
