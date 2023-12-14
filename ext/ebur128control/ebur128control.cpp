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

#include <cmath>
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
#include <tuple>

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
    gdouble integrated_loudness_lufs;
    gdouble target_level_lufs;
  } properties;

  struct XFormedProperties {
    gdouble volume;

    gboolean passthrough;
  } xformed_properties;

  struct Params {
    XFormedProperties p;

    void (*process)(EBUR128Control *, gpointer, guint);

    gboolean negotiated;
  } committed_params;
};

bool operator==(const EBUR128Control::XFormedProperties &lhs,
                const EBUR128Control::XFormedProperties &rhs) {

  return std::tie(lhs.perform_loudness_normalization, lhs.volume,
                  lhs.passthrough) ==
         std::tie(rhs.perform_loudness_normalization, rhs.volume,
                  rhs.passthrough);

}

bool operator!=(const EBUR128Control::XFormedProperties &lhs,
                const EBUR128Control::XFormedProperties &rhs) {

  return !(lhs == rhs);

}

#define GST_CAT_DEFAULT ebur128control_debug

GST_DEBUG_CATEGORY_STATIC(GST_CAT_DEFAULT);

template <typename T>
void process(EBUR128Control *self, gpointer bytes, guint n_bytes) {

  T vol = self->committed_params.p.volume;
  auto *data = static_cast<T *>(bytes);
  for (guint i = 0, num_samples = n_bytes / sizeof(T); i != num_samples; i++) {
    *data++ *= vol;
  }

}

constexpr double mute_volume = 0.0;
constexpr double neutral_volume = 1.0;

GstFlowReturn transform_ip(GstBaseTransform *base, GstBuffer *outbuf) {

  EBUR128Control *self = EBUR128CONTROL(base);

  g_assert(!gst_base_transform_is_passthrough(base));

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

  if (self->committed_params.p.volume == mute_volume) {
    memset(map.data, 0, map.size);
    GST_BUFFER_FLAG_SET(outbuf, GST_BUFFER_FLAG_GAP);
  } else if (self->committed_params.p.volume != neutral_volume) {
    self->committed_params.process(self, map.data, map.size);
  }

  gst_buffer_unmap(outbuf, &map);

  return GST_FLOW_OK;

}

void commit_params(EBUR128Control *self, const GstAudioInfo *info) {

  self->committed_params.process = nullptr;
  self->committed_params.negotiated = false;

  GST_DEBUG_OBJECT(self, "configure integrated loudness %f lufs",
                   self->properties.integrated_loudness_lufs);
  GST_DEBUG_OBJECT(self, "configure target level %f lufs",
                   self->properties.target_level_lufs);

  self->committed_params.p = self->xformed_properties;

  GST_DEBUG_OBJECT(self, "configure volume %f",
                   self->committed_params.p.volume);
  GST_DEBUG_OBJECT(self, "set passthrough %d",
                   self->committed_params.p.passthrough);

  gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self),
                                     self->committed_params.p.passthrough);

  switch (GST_AUDIO_INFO_FORMAT(info)) {
  case GST_AUDIO_FORMAT_F32:
    self->committed_params.process = process<gfloat>;
    break;
  case GST_AUDIO_FORMAT_F64:
    self->committed_params.process = process<gdouble>;
    break;
  default:
    g_assert(self->committed_params.p.passthrough);
    break;
  }

  self->committed_params.negotiated = true;

}

void before_transform (GstBaseTransform * base, GstBuffer * buffer) {

  (void)buffer;

  EBUR128Control *self = EBUR128CONTROL(base);

  if(self->committed_params.p != self->xformed_properties)
    commit_params(self, GST_AUDIO_FILTER_INFO (self));

}

gboolean setup(GstAudioFilter *filter, const GstAudioInfo *info) {

  EBUR128Control *self = EBUR128CONTROL(filter);

  commit_params(self, info);

  if (!self->committed_params.negotiated) {
    GST_ELEMENT_ERROR(self, CORE, NEGOTIATION, ("Invalid incoming format"),
                      (nullptr));
  }

  return self->committed_params.negotiated;

}

GstCaps *template_caps(bool passthrough) {

  static GstStaticCaps passthrough_caps = GST_STATIC_CAPS("audio/x-raw");

  static GstStaticCaps process_caps =
      GST_STATIC_CAPS("audio/x-raw,"
                      "format = (string) { F32LE, F64LE }");

  return gst_static_caps_get(passthrough ? &passthrough_caps : &process_caps);

}

GstCaps *sink_getcaps(EBUR128Control *self, GstPad *srcpad,
                      GstCaps *filter) {

  GstCaps *sink_caps;
  GstCaps *sink_template_caps =
      template_caps(/*passthrough=*/self->xformed_properties.passthrough);

  if (GstCaps *downstream_caps = gst_pad_get_allowed_caps(srcpad)) {
    sink_caps = gst_caps_intersect_full(sink_template_caps, downstream_caps,
                                        GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(downstream_caps);
  } else {
    sink_caps = gst_caps_ref(sink_template_caps);
  }
  gst_caps_unref(sink_template_caps);

  if (filter) {
    GstCaps *tmp =
        gst_caps_intersect_full(sink_caps, filter, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(sink_caps);
    sink_caps = tmp;
  }

  return sink_caps;

}

gboolean sink_query_caps(GstPad *pad, GstObject *parent, GstQuery *query) {

  (void)pad;

  EBUR128Control *self = EBUR128CONTROL(parent);

  g_assert (GST_QUERY_TYPE(query) == GST_QUERY_CAPS);

  GstCaps *filter;
  gst_query_parse_caps(query, &filter);

  GstCaps *caps = sink_getcaps(self, GST_BASE_TRANSFORM(self)->srcpad, filter);
  gst_query_set_caps_result(query, caps);
  gst_caps_unref(caps);

  return true;

}

gboolean sink_query(GstPad *pad, GstObject *parent, GstQuery *query) {

  switch (GST_QUERY_TYPE(query)) {
  case GST_QUERY_CAPS:
    return sink_query_caps(pad, parent, query);
  default:
    return gst_pad_query_default(pad, parent, query);
  }

}

EBUR128Control::XFormedProperties
xform_properties(EBUR128Control::Properties properties) {

  EBUR128Control::XFormedProperties p;

  p.volume = mute_volume;
  p.passthrough = false;

  auto computeGain_dB = [](double source_dB, double target_dB) {
    // Let's suppose the `source_dB` is -12 dB, while `target_dB` is -23 dB.
    // In that case, we'd need to apply -11 dB of gain, which is computed as:
    //   -12 dB + x dB = -23 dB --> x dB = -23 dB - (-12 dB)
    return target_dB - source_dB;
  };

  auto dB_to_mult = [](const double gain_dB) {
    return std::pow(10., gain_dB / 20.);
  };

  double loudness_normalizing_gain_db = computeGain_dB(
      properties.integrated_loudness_lufs, properties.target_level_lufs);

  p.volume = dB_to_mult(loudness_normalizing_gain_db);

  p.passthrough = (p.volume == neutral_volume);

  return p;

}

enum class Properties : guint {
  IntegratedLoudness = 1,
  TargetLevel,
};

void set_property(GObject *object, guint prop_id, const GValue *value,
                  GParamSpec *pspec) {

  EBUR128Control *self = EBUR128CONTROL(object);

  GST_OBJECT_LOCK(self);

  switch (static_cast<Properties>(prop_id)) {
  case Properties::IntegratedLoudness:
    self->properties.integrated_loudness_lufs = g_value_get_double(value);
    break;
  case Properties::TargetLevel:
    self->properties.target_level_lufs = g_value_get_double(value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    GST_OBJECT_UNLOCK(self);
    return;
  }

  auto new_xformed_properties = xform_properties(self->properties);

  bool should_reconfigure_sink = new_xformed_properties.passthrough !=
                                 self->xformed_properties.passthrough;

  self->xformed_properties = new_xformed_properties;

  GST_OBJECT_UNLOCK(self);

  if (should_reconfigure_sink)
    gst_base_transform_reconfigure_sink(GST_BASE_TRANSFORM(self));

}

void get_property(GObject *object, guint prop_id, GValue *value,
                  GParamSpec *pspec) {

  EBUR128Control *self = EBUR128CONTROL(object);

  switch (static_cast<Properties>(prop_id)) {
  case Properties::IntegratedLoudness:
    GST_OBJECT_LOCK(self);
    g_value_set_double(value, self->properties.integrated_loudness_lufs);
    GST_OBJECT_UNLOCK(self);
    break;
  case Properties::TargetLevel:
    GST_OBJECT_LOCK(self);
    g_value_set_double(value, self->properties.target_level_lufs);
    GST_OBJECT_UNLOCK(self);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }

}

void ebur128control_init(EBUR128Control *self) {

  self->properties.integrated_loudness_lufs = -23.0;
  self->properties.target_level_lufs = -23.0;

  self->xformed_properties.volume = neutral_volume;
  self->xformed_properties.passthrough = true;

  self->committed_params.p = self->xformed_properties;

  self->committed_params.process = nullptr;
  self->committed_params.negotiated = false;

  gst_base_transform_set_gap_aware(GST_BASE_TRANSFORM(self), true);

  gst_pad_set_query_function (GST_BASE_TRANSFORM(self)->sinkpad, sink_query);

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
      gobject_class, static_cast<guint>(Properties::IntegratedLoudness),
      g_param_spec_double("integrated_loudness_lufs", "integrated loudness",
                          "EBU R 128 Integrated Loudness [LUFS]", -G_MAXDOUBLE,
                          G_MAXDOUBLE, -23.0,
                          static_cast<GParamFlags>(G_PARAM_READWRITE)));

  g_object_class_install_property(
      gobject_class, static_cast<guint>(Properties::TargetLevel),
      g_param_spec_double("target_level_lufs", "target level",
                          "EBU R 128 Target Level [LUFS]", -G_MAXDOUBLE,
                          G_MAXDOUBLE, -23.0,
                          static_cast<GParamFlags>(G_PARAM_READWRITE)));

  auto *filter_class = reinterpret_cast<GstAudioFilterClass *>(klass);

  GstCaps *caps = template_caps(/*passthrough=*/true);
  gst_audio_filter_class_add_pad_templates(filter_class, caps);
  gst_caps_unref(caps);

  filter_class->setup = GST_DEBUG_FUNCPTR(setup);

  auto *trans_class = reinterpret_cast<GstBaseTransformClass *>(klass);
  trans_class->before_transform = GST_DEBUG_FUNCPTR (before_transform);
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
