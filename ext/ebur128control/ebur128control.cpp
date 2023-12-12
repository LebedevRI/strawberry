/*
 * Strawberry Music Player
 * Copyright 2023 Roman Lebedev
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <glib-object.h>
#include <glib.h>
#include <glibconfig.h>
#include <gobject/gobject.h>
#include <gst/audio/audio.h>
#include <gst/audio/gstaudiofilter.h>
#include <gst/base/gstbasetransform.h>
#include <gst/gstbuffer.h>
#include <gst/gstcaps.h>
#include <gst/gstelement.h>
#include <gst/gstinfo.h>
#include <gst/gstmemory.h>
#include <gst/gstobject.h>
#include <gst/gstpad.h>
#include <gst/gstplugin.h>
#include <gst/gstpluginfeature.h>
#include <gst/gstversion.h>
#include <cstring>

namespace {

G_DECLARE_FINAL_TYPE(EBUR128Control, ebur128control, STRAWBERRY, EBUR128CONTROL,
                     GstAudioFilter)

#define EBUR128CONTROL STRAWBERRY_EBUR128CONTROL

/**
 * EBUR128Control:
 *
 * Opaque data structure.
 */
struct _EBUR128Control {
  GstAudioFilter element;

  struct Properties {
    gdouble volume;
  } properties;

  struct Params {
    void (*process)(EBUR128Control *, gpointer, guint);

    gdouble volume;

    gboolean negotiated;
  } committed_params;
};

#define GST_CAT_DEFAULT ebur128control_debug

GST_DEBUG_CATEGORY_STATIC(GST_CAT_DEFAULT);

template <typename T>
void process(EBUR128Control *self, gpointer bytes, guint n_bytes) {

  T vol = self->committed_params.volume;
  auto *data = static_cast<T *>(bytes);
  for (guint i = 0, num_samples = n_bytes / sizeof(T); i != num_samples; i++) {
    *data++ *= vol;
  }

}

constexpr double mute_volume = 0.0;
constexpr double neutral_volume = 1.0;

GstFlowReturn transform_ip(GstBaseTransform *base, GstBuffer *outbuf) {

  EBUR128Control *self = EBUR128CONTROL(base);

  if (G_UNLIKELY(!self->committed_params.negotiated)) {
    GST_ELEMENT_ERROR(self, CORE, NEGOTIATION, ("No format was negotiated"),
                      (nullptr));
    return GST_FLOW_NOT_NEGOTIATED;
  }

  // Don't process data with GAP.
  if (GST_BUFFER_FLAG_IS_SET(outbuf, GST_BUFFER_FLAG_GAP))
    return GST_FLOW_OK;

  GstMapInfo map;
  gst_buffer_map(outbuf, &map, GST_MAP_READWRITE);

  if (self->committed_params.volume == mute_volume) {
    memset(map.data, 0, map.size);
    GST_BUFFER_FLAG_SET(outbuf, GST_BUFFER_FLAG_GAP);
  } else if (self->committed_params.volume != neutral_volume) {
    self->committed_params.process(self, map.data, map.size);
  }

  gst_buffer_unmap(outbuf, &map);

  return GST_FLOW_OK;

}

void commit_params(EBUR128Control *self, const GstAudioInfo *info,
                   EBUR128Control::Properties properties) {

  self->committed_params.process = nullptr;
  self->committed_params.volume = mute_volume;
  self->committed_params.negotiated = false;

  GST_DEBUG_OBJECT(self, "configure volume %f", properties.volume);

  self->committed_params.volume = properties.volume;

  bool passthrough;
  switch (GST_AUDIO_INFO_FORMAT(info)) {
  case GST_AUDIO_FORMAT_F32:
  case GST_AUDIO_FORMAT_F64:
    passthrough = (self->committed_params.volume == neutral_volume);
    break;
  default:
    return;
  }

  GST_DEBUG_OBJECT(self, "set passthrough %d", passthrough);

  gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), passthrough);

  switch (GST_AUDIO_INFO_FORMAT(info)) {
  case GST_AUDIO_FORMAT_F32:
    self->committed_params.process = process<gfloat>;
    break;
  case GST_AUDIO_FORMAT_F64:
    self->committed_params.process = process<gdouble>;
    break;
  default:
    return;
  }

  self->committed_params.negotiated = true;

}

// Returns a snapshot (a copy) of currently-set properties.
EBUR128Control::Properties copy_properties(EBUR128Control *self) {

  GST_OBJECT_LOCK(self);
  EBUR128Control::Properties properties = self->properties;
  GST_OBJECT_UNLOCK(self);

  return properties;

}

gboolean setup(GstAudioFilter *filter, const GstAudioInfo *info) {

  EBUR128Control *self = EBUR128CONTROL(filter);

  EBUR128Control::Properties properties = copy_properties(self);
  commit_params(self, info, properties);

  if (!self->committed_params.negotiated) {
    GST_ELEMENT_ERROR(self, CORE, NEGOTIATION, ("Invalid incoming format"),
                      (nullptr));
  }

  return self->committed_params.negotiated;

}

enum class Properties : guint {
  Volume = 1,
};

void set_property(GObject *object, guint prop_id, const GValue *value,
                  GParamSpec *pspec) {

  EBUR128Control *self = EBUR128CONTROL(object);

  switch (static_cast<Properties>(prop_id)) {
  case Properties::Volume:
    GST_OBJECT_LOCK(self);
    self->properties.volume = g_value_get_double(value);
    GST_OBJECT_UNLOCK(self);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }

}

void get_property(GObject *object, guint prop_id, GValue *value,
                  GParamSpec *pspec) {

  EBUR128Control *self = EBUR128CONTROL(object);

  switch (static_cast<Properties>(prop_id)) {
  case Properties::Volume:
    GST_OBJECT_LOCK(self);
    g_value_set_double(value, self->properties.volume);
    GST_OBJECT_UNLOCK(self);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }

}

void ebur128control_init(EBUR128Control *self) {

  self->properties.volume = neutral_volume;

  self->committed_params.process = nullptr;
  self->committed_params.volume = mute_volume;
  self->committed_params.negotiated = false;

  gst_base_transform_set_gap_aware(GST_BASE_TRANSFORM(self), true);

}

void ebur128control_class_init(EBUR128ControlClass *klass) {

  auto *element_class = reinterpret_cast<GstElementClass *>(klass);
  gst_element_class_set_static_metadata(
      element_class, "EBUR128Control", "Filter/Effect/Audio",
      "Control EBU R 128 loudness characteristics of audio/raw streams",
      "Roman Lebedev <lebedev.ri@gmail.com>");

  auto *gobject_class = reinterpret_cast<GObjectClass *>(klass);

  gobject_class->set_property = set_property;
  gobject_class->get_property = get_property;

  g_object_class_install_property(
      gobject_class, static_cast<guint>(Properties::Volume),
      g_param_spec_double("volume", "Volume", "volume factor, 1.0=100%", 0.0,
                          G_MAXDOUBLE, mute_volume,
                          static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                   G_PARAM_STATIC_STRINGS)));

  auto *filter_class = reinterpret_cast<GstAudioFilterClass *>(klass);

  GstStaticCaps static_caps =
      GST_STATIC_CAPS("audio/x-raw,"
                      "format = (string) { F32LE, F64LE }");

  GstCaps *caps = gst_static_caps_get(&static_caps);
  gst_audio_filter_class_add_pad_templates(filter_class, caps);
  gst_caps_unref(caps);

  filter_class->setup = GST_DEBUG_FUNCPTR(setup);

  auto *trans_class = reinterpret_cast<GstBaseTransformClass *>(klass);
  trans_class->transform_ip = GST_DEBUG_FUNCPTR(transform_ip);
  trans_class->transform_ip_on_passthrough = false;

}

G_DEFINE_TYPE(EBUR128Control, ebur128control, GST_TYPE_AUDIO_FILTER);

// The rest is public API definition.

GST_ELEMENT_REGISTER_DEFINE(ebur128control, "strawberry-ebur128control",
                            GST_RANK_NONE, ebur128control_get_type());

gboolean plugin_init(GstPlugin *plugin) {

  GST_DEBUG_CATEGORY_INIT(GST_CAT_DEFAULT, "strawberry-ebur128control", 0,
                          "EBU R 128 Loudness Control");

  return GST_ELEMENT_REGISTER(ebur128control, plugin);

}

#define PACKAGE "strawberry"

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR,
                  strawberry_ebur128control,
                  "plugin for controlling audio loudness (EBU R 128)",
                  plugin_init, "0.1", "GPL", "strawberry-ebur128control",
                  "https://www.strawberrymusicplayer.org");

} // namespace
