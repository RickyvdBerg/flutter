// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_PUBLIC_FLUTTER_LINUX_FL_VIEW_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_PUBLIC_FLUTTER_LINUX_FL_VIEW_H_

#if !defined(__FLUTTER_LINUX_INSIDE__) && !defined(FLUTTER_LINUX_COMPILATION)
#error "Only <flutter_linux/flutter_linux.h> can be included directly."
#endif

#include <gmodule.h>
#include <gtk/gtk.h>

#include "fl_dart_project.h"
#include "fl_engine.h"

G_BEGIN_DECLS

G_MODULE_EXPORT
G_DECLARE_FINAL_TYPE(FlView, fl_view, FL, VIEW, GtkBox)

/// Recipe vocabulary for compositor-owned material attached to a Flutter
/// frame. This mirrors the Avio embedder extension without exposing the raw
/// embedder header through flutter_linux's public API.
typedef enum {
  FL_COMPOSITOR_MATERIAL_RECIPE_EXPLICIT = 0,
  FL_COMPOSITOR_MATERIAL_RECIPE_TIERED = 1,
} FlCompositorMaterialRecipe;

/// One immutable material node belonging to the exact frame being drawn.
typedef struct {
  guint64 id;
  gdouble left;
  gdouble top;
  gdouble right;
  gdouble bottom;
  FlCompositorMaterialRecipe recipe;
  guint32 tier;
  gboolean uses_default_corner;
  /// Uniform logical geometry scale applied to the material's corner shape.
  gfloat corner_scale;
  gdouble corner_radius;
  gdouble corner_exponent;
  guint32 corner_mask;
  gdouble blur_radius;
  gfloat tint_red;
  gfloat tint_green;
  gfloat tint_blue;
  gfloat tint_alpha;
  gfloat saturation;
  gfloat luminosity;
  gfloat noise_opacity;
  gint32 order;
  gfloat strength;
} FlCompositorMaterial;

/// Called on the GTK thread immediately before the exact Flutter frame is
/// drawn. The descriptor array is valid only for the callback.
typedef void (*FlViewCompositorMaterialsCallback)(
    FlView* view,
    const FlCompositorMaterial* materials,
    size_t materials_count,
    gboolean invalid,
    gpointer user_data);

/**
 * FlView:
 *
 * #FlView is a GTK widget that is capable of displaying a Flutter application.
 *
 * The following example shows how to set up a view in a GTK application:
 * |[<!-- language="C" -->
 *   FlDartProject *project = fl_dart_project_new ();
 *   FlView *view = fl_view_new (project);
 *   gtk_widget_show (GTK_WIDGET (view));
 *   gtk_container_add (GTK_CONTAINER (parent), view);
 *
 *   FlBinaryMessenger *messenger =
 *     fl_engine_get_binary_messenger (fl_view_get_engine (view));
 *   setup_channels_or_plugins (messenger);
 * ]|
 */

/**
 * fl_view_new:
 * @project: The project to show.
 *
 * Creates a widget to show a Flutter application.
 *
 * Returns: a new #FlView.
 */
FlView* fl_view_new(FlDartProject* project);

/**
 * fl_view_new_for_engine:
 * @engine: an #FlEngine.
 *
 * Creates a widget to show a window in a Flutter application.
 * The engine must be not be headless.
 *
 * Returns: a new #FlView.
 */
FlView* fl_view_new_for_engine(FlEngine* engine);

/**
 * fl_view_new_sized_to_content:
 * @engine: an #FlEngine.
 *
 * Creates a widget to show a window in a Flutter application.
 * The window will always display content in the size that Flutter provides.
 * The engine must be not be headless.
 *
 * Returns: a new #FlView.
 */
FlView* fl_view_new_sized_to_content(FlEngine* engine);

/**
 * fl_view_get_engine:
 * @view: an #FlView.
 *
 * Gets the engine being rendered in the view.
 *
 * Returns: an #FlEngine.
 */
FlEngine* fl_view_get_engine(FlView* view);

/**
 * fl_view_get_id:
 * @view: an #FlView.
 *
 * Gets the Flutter view ID used by this view.
 *
 * Returns: a view ID or -1 if now ID assigned.
 */
int64_t fl_view_get_id(FlView* view);

/**
 * fl_view_set_background_color:
 * @view: an #FlView.
 * @color: a background color.
 *
 * Set the background color for Flutter (defaults to black).
 */
void fl_view_set_background_color(FlView* view, const GdkRGBA* color);

/**
 * fl_view_set_compositor_materials_callback:
 * @view: an #FlView.
 * @callback: callback invoked immediately before drawing an exact frame.
 * @user_data: data supplied to @callback.
 * @destroy_notify: optional destroy function for @user_data.
 *
 * Installs the one consumer of retained external-compositor material metadata.
 * Replacing or disposing the callback invokes the previous destroy function.
 */
void fl_view_set_compositor_materials_callback(
    FlView* view,
    FlViewCompositorMaterialsCallback callback,
    gpointer user_data,
    GDestroyNotify destroy_notify);

G_END_DECLS

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_PUBLIC_FLUTTER_LINUX_FL_VIEW_H_
