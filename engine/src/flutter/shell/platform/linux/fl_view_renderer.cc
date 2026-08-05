// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_view_renderer.h"

#include <algorithm>

typedef struct {
  // Background color drawn behind the Flutter frame.
  GdkRGBA* background_color;

  // TRUE if have got the first frame to render.
  gboolean have_first_frame;

  // Exact-frame metadata is hard-bounded by the public engine contract. Keep
  // it inline so the first drawable frame does not allocate merely to retain
  // the sidecar until GTK's paired draw.
  FlCompositorMaterial
      compositor_materials[FLUTTER_AVIO_MAX_COMPOSITOR_MATERIALS];
  size_t compositor_materials_count;
  gboolean compositor_materials_invalid;
  FlViewRendererCompositorMaterialsCallback compositor_materials_callback;
  gpointer compositor_materials_user_data;
} FlViewRendererPrivate;

enum { SIGNAL_FIRST_FRAME, LAST_SIGNAL };

static guint fl_view_renderer_signals[LAST_SIGNAL];

G_DEFINE_TYPE_WITH_PRIVATE(FlViewRenderer,
                           fl_view_renderer,
                           GTK_TYPE_DRAWING_AREA)

// Default implementation for the abstract present_layers method. Subclasses
// must override this.
static void fl_view_renderer_present_layers_default(
    FlViewRenderer* self,
    const FlutterLayer** layers,
    size_t layers_count,
    const FlCompositorMaterial* materials,
    size_t materials_count,
    gboolean materials_invalid) {
  g_assert_not_reached();
}

static void fl_view_renderer_dispose(GObject* object) {
  FlViewRendererPrivate* priv = static_cast<FlViewRendererPrivate*>(
      fl_view_renderer_get_instance_private(FL_VIEW_RENDERER(object)));

  g_clear_pointer(&priv->background_color, gdk_rgba_free);
  G_OBJECT_CLASS(fl_view_renderer_parent_class)->dispose(object);
}

static void fl_view_renderer_class_init(FlViewRendererClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_view_renderer_dispose;

  klass->present_layers = fl_view_renderer_present_layers_default;

  fl_view_renderer_signals[SIGNAL_FIRST_FRAME] =
      g_signal_new("first-frame", fl_view_renderer_get_type(),
                   G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void fl_view_renderer_init(FlViewRenderer* self) {
  FlViewRendererPrivate* priv = static_cast<FlViewRendererPrivate*>(
      fl_view_renderer_get_instance_private(self));

  GdkRGBA default_background = {
      .red = 0.0, .green = 0.0, .blue = 0.0, .alpha = 1.0};
  priv->background_color = gdk_rgba_copy(&default_background);
}

void fl_view_renderer_set_background_color(FlViewRenderer* self,
                                           const GdkRGBA* color) {
  g_return_if_fail(FL_IS_VIEW_RENDERER(self));

  FlViewRendererPrivate* priv = static_cast<FlViewRendererPrivate*>(
      fl_view_renderer_get_instance_private(self));

  gdk_rgba_free(priv->background_color);
  priv->background_color = gdk_rgba_copy(color);

  // Redraw so the new background color is shown.
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

void fl_view_renderer_paint_background(FlViewRenderer* self, cairo_t* cr) {
  g_return_if_fail(FL_IS_VIEW_RENDERER(self));

  FlViewRendererPrivate* priv = static_cast<FlViewRendererPrivate*>(
      fl_view_renderer_get_instance_private(self));

  // Don't bother drawing if fully transparent - the widget above this will
  // already be drawn by GTK.
  if (priv->background_color->red == 0 && priv->background_color->green == 0 &&
      priv->background_color->blue == 0 && priv->background_color->alpha == 0) {
    return;
  }

  gdk_cairo_set_source_rgba(cr, priv->background_color);
  cairo_paint(cr);
}

void fl_view_renderer_present_layers(FlViewRenderer* self,
                                     const FlutterLayer** layers,
                                     size_t layers_count,
                                     const FlCompositorMaterial* materials,
                                     size_t materials_count,
                                     gboolean materials_invalid) {
  g_return_if_fail(FL_IS_VIEW_RENDERER(self));

  FlViewRendererClass* klass = FL_VIEW_RENDERER_GET_CLASS(self);
  klass->present_layers(self, layers, layers_count, materials, materials_count,
                        materials_invalid);
}

void fl_view_renderer_store_compositor_materials(
    FlViewRenderer* self,
    const FlCompositorMaterial* materials,
    size_t materials_count,
    gboolean invalid) {
  g_return_if_fail(FL_IS_VIEW_RENDERER(self));
  FlViewRendererPrivate* priv = static_cast<FlViewRendererPrivate*>(
      fl_view_renderer_get_instance_private(self));
  const bool malformed =
      materials_count > FLUTTER_AVIO_MAX_COMPOSITOR_MATERIALS ||
      (materials_count > 0u && materials == nullptr);
  priv->compositor_materials_count = 0u;
  if (!invalid && !malformed && materials_count > 0u) {
    std::copy_n(materials, materials_count, priv->compositor_materials);
    priv->compositor_materials_count = materials_count;
  }
  priv->compositor_materials_invalid = invalid || malformed;
}

void fl_view_renderer_set_compositor_materials_callback(
    FlViewRenderer* self,
    FlViewRendererCompositorMaterialsCallback callback,
    gpointer user_data) {
  g_return_if_fail(FL_IS_VIEW_RENDERER(self));
  FlViewRendererPrivate* priv = static_cast<FlViewRendererPrivate*>(
      fl_view_renderer_get_instance_private(self));
  priv->compositor_materials_callback = callback;
  priv->compositor_materials_user_data = user_data;
}

void fl_view_renderer_notify_compositor_materials(FlViewRenderer* self) {
  g_return_if_fail(FL_IS_VIEW_RENDERER(self));
  FlViewRendererPrivate* priv = static_cast<FlViewRendererPrivate*>(
      fl_view_renderer_get_instance_private(self));
  if (priv->compositor_materials_callback == nullptr) {
    return;
  }
  priv->compositor_materials_callback(
      priv->compositor_materials, priv->compositor_materials_count,
      priv->compositor_materials_invalid, priv->compositor_materials_user_data);
}

void fl_view_renderer_notify_frame(FlViewRenderer* self) {
  g_return_if_fail(FL_IS_VIEW_RENDERER(self));

  FlViewRendererPrivate* priv = static_cast<FlViewRendererPrivate*>(
      fl_view_renderer_get_instance_private(self));

  if (!priv->have_first_frame) {
    priv->have_first_frame = TRUE;
    g_signal_emit(self, fl_view_renderer_signals[SIGNAL_FIRST_FRAME], 0);
  }
}
