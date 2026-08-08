// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/compositor_context.h"

#include <optional>
#include <utility>
#include "flutter/display_list/geometry/dl_region.h"
#include "flutter/flow/layers/layer_tree.h"

namespace flutter {

void FrameDamage::ComputeDamage(flutter::LayerTree& layer_tree,
                                bool has_raster_cache,
                                bool impeller_enabled,
                                TextureRegistry* texture_registry) {
  ignore_damage_ = false;
  if (!layer_tree.root_layer()) {
    damage_.reset();
    raster_damage_.reset();
    return;
  }
  PaintRegionMap empty_paint_region_map;
  DiffContext context(layer_tree.frame_size(), layer_tree.paint_region_map(),
                      prev_layer_tree_ ? prev_layer_tree_->paint_region_map()
                                       : empty_paint_region_map,
                      has_raster_cache, impeller_enabled, texture_registry);
  context.PushCullRect(DlRect::MakeSize(layer_tree.frame_size()));
  {
    DiffContext::AutoSubtreeRestore subtree(&context);
    const Layer* prev_root_layer = nullptr;
    if (!prev_layer_tree_ ||
        prev_layer_tree_->frame_size() != layer_tree.frame_size()) {
      // If there is no previous layer tree assume the entire frame must be
      // repainted.
      context.MarkSubtreeDirty(DlRect::MakeSize(layer_tree.frame_size()));
    } else {
      prev_root_layer = prev_layer_tree_->root_layer();
    }
    layer_tree.root_layer()->Diff(&context, prev_root_layer);
  }

  damage_ = context.ComputeDamage(
      additional_damage_, horizontal_clip_alignment_, vertical_clip_alignment_);
  raster_damage_ = damage_->buffer_damage.isEmpty()
                       ? DlRegion()
                       : DlRegion(damage_->buffer_damage.bounds());
}

CompositorContext::CompositorContext()
    : texture_registry_(std::make_shared<TextureRegistry>()),
      raster_time_(fixed_refresh_rate_updater_),
      ui_time_(fixed_refresh_rate_updater_) {}

CompositorContext::CompositorContext(Stopwatch::RefreshRateUpdater& updater)
    : texture_registry_(std::make_shared<TextureRegistry>()),
      raster_time_(updater),
      ui_time_(updater) {}

CompositorContext::~CompositorContext() = default;

void CompositorContext::BeginFrame(ScopedFrame& frame,
                                   bool enable_instrumentation) {
  if (enable_instrumentation) {
    raster_time_.Start();
  }
}

void CompositorContext::EndFrame(ScopedFrame& frame,
                                 bool enable_instrumentation) {
  if (enable_instrumentation) {
    raster_time_.Stop();
  }
}

std::unique_ptr<CompositorContext::ScopedFrame> CompositorContext::AcquireFrame(
    GrDirectContext* gr_context,
    DlCanvas* canvas,
    ExternalViewEmbedder* view_embedder,
    const DlMatrix& root_surface_transformation,
    bool instrumentation_enabled,
    bool surface_supports_readback,
    fml::RefPtr<fml::RasterThreadMerger>
        raster_thread_merger,  // NOLINT(performance-unnecessary-value-param)
    impeller::AiksContext* aiks_context) {
  return std::make_unique<ScopedFrame>(
      *this, gr_context, canvas, view_embedder, root_surface_transformation,
      instrumentation_enabled, surface_supports_readback, raster_thread_merger,
      aiks_context);
}

CompositorContext::ScopedFrame::ScopedFrame(
    CompositorContext& context,
    GrDirectContext* gr_context,
    DlCanvas* canvas,
    ExternalViewEmbedder* view_embedder,
    const DlMatrix& root_surface_transformation,
    bool instrumentation_enabled,
    bool surface_supports_readback,
    fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger,
    impeller::AiksContext* aiks_context)
    : context_(context),
      gr_context_(gr_context),
      canvas_(canvas),
      aiks_context_(aiks_context),
      view_embedder_(view_embedder),
      root_surface_transformation_(root_surface_transformation),
      instrumentation_enabled_(instrumentation_enabled),
      surface_supports_readback_(surface_supports_readback),
      raster_thread_merger_(std::move(raster_thread_merger)) {
  context_.BeginFrame(*this, instrumentation_enabled_);
}

CompositorContext::ScopedFrame::~ScopedFrame() {
  context_.EndFrame(*this, instrumentation_enabled_);
}

RasterStatus CompositorContext::ScopedFrame::Raster(
    flutter::LayerTree& layer_tree,
    bool ignore_raster_cache,
    FrameDamage* frame_damage,
    bool reject_invalid_compositor_materials) {
  TRACE_EVENT0("flutter", "CompositorContext::ScopedFrame::Raster");

  std::optional<DlRegion> raster_damage;
  DlIRect clip_bounds = DlIRect::MakeLTRB(0, 0, 0, 0);
  if (frame_damage) {
    frame_damage->ComputeDamage(layer_tree, !ignore_raster_cache, !gr_context_,
                                context_.texture_registry().get());
    raster_damage = frame_damage->GetBufferDamage();

    // Exact no-change is a terminal result, not a request for a full repaint.
    // Test it before the partial-repaint cost heuristic: an empty clip has no
    // area to optimize, but resetting its damage would erase that distinction.
    const auto frame_damage_region = frame_damage->GetFrameDamage();
    if (frame_damage_region.has_value() && raster_damage.has_value() &&
        frame_damage_region->isEmpty() && raster_damage->isEmpty()) {
      return RasterStatus::kSuccess;
    }

    if (raster_damage.has_value() && !raster_damage->isEmpty()) {
      clip_bounds = raster_damage->bounds();
    }

    // A frame containing a readback -- a backdrop filter -- cannot be a
    // partial repaint under Impeller, however small its damage is. Impeller
    // rasters such a frame into an offscreen and copies that offscreen, whole,
    // over the target at the end of the pass, which replaces every pixel the
    // target preserved outside the damage region.
    //
    // This has to be decided here, before the cull rect below narrows Preroll
    // to the damage. Once the tree has been culled there is no way back: the
    // frame would then clear the whole target and repaint only the part of the
    // scene that intersected the damage.
    if (aiks_context_ &&
        (frame_damage->HasReadback() ||
         !ShouldPerformPartialRepaint(clip_bounds, layer_tree.frame_size()))) {
      raster_damage.reset();
      clip_bounds = DlIRect::MakeLTRB(0, 0, 0, 0);
      frame_damage->Reset();
    }
  }

  // Compute bounding rect for Preroll cull rect.
  DlRect cull_rect = kGiantRect;
  if (raster_damage.has_value() && !raster_damage->isEmpty()) {
    cull_rect = DlRect::Make(clip_bounds);
  }

  bool root_needs_readback =
      layer_tree.Preroll(*this, ignore_raster_cache, cull_rect);
  if (reject_invalid_compositor_materials &&
      layer_tree.avio_compositor_materials_invalid()) {
    return RasterStatus::kInvalidCompositorMaterials;
  }
  bool needs_save_layer = root_needs_readback && !surface_supports_readback();
  PostPrerollResult post_preroll_result = PostPrerollResult::kSuccess;
  if (view_embedder_ && raster_thread_merger_) {
    post_preroll_result =
        view_embedder_->PostPrerollAction(raster_thread_merger_);
  }

  if (post_preroll_result == PostPrerollResult::kResubmitFrame) {
    return RasterStatus::kResubmit;
  }
  if (post_preroll_result == PostPrerollResult::kSkipAndRetryFrame) {
    return RasterStatus::kSkipAndRetry;
  }

  if (aiks_context_) {
    PaintLayerTreeImpeller(layer_tree, clip_bounds, ignore_raster_cache);
  } else {
    // Skia path: use bounding rect (preserve existing behavior).
    std::optional<DlRect> skia_clip =
        raster_damage.has_value() && !raster_damage->isEmpty()
            ? std::make_optional(cull_rect)
            : std::nullopt;
    PaintLayerTreeSkia(layer_tree, skia_clip, needs_save_layer,
                       ignore_raster_cache);
  }
  return RasterStatus::kSuccess;
}

void CompositorContext::ScopedFrame::PaintLayerTreeSkia(
    flutter::LayerTree& layer_tree,
    std::optional<DlRect> clip_rect,
    bool needs_save_layer,
    bool ignore_raster_cache) {
  DlAutoCanvasRestore restore(canvas(), clip_rect.has_value());

  if (canvas()) {
    if (clip_rect) {
      canvas()->ClipRect(*clip_rect);
    }

    if (needs_save_layer) {
      TRACE_EVENT0("flutter", "Canvas::saveLayer");
      DlRect bounds = DlRect::MakeSize(layer_tree.frame_size());
      DlPaint paint;
      paint.setBlendMode(DlBlendMode::kSrc);
      canvas()->SaveLayer(bounds, &paint);
    }
    canvas()->Clear(DlColor::kTransparent());
  }

  // The canvas()->Restore() is taken care of by the DlAutoCanvasRestore
  layer_tree.Paint(*this, ignore_raster_cache);
}

void CompositorContext::ScopedFrame::PaintLayerTreeImpeller(
    flutter::LayerTree& layer_tree,
    DlIRect clip_bounds,
    bool ignore_raster_cache) {
  if (clip_bounds.IsEmpty()) {
    // Full repaint - no clip.
    layer_tree.Paint(*this, ignore_raster_cache);
    return;
  }

  DlAutoCanvasRestore restore(canvas(), true);
  if (canvas()) {
    canvas()->ClipRect(DlRect::Make(clip_bounds));
  }
  layer_tree.Paint(*this, ignore_raster_cache);
}

/// @brief The max ratio of pixel width or height to size that is dirty which
///        results in a partial repaint.
///
///        Performing a partial repaint has a small overhead - Impeller needs to
///        allocate a fairly large resolve texture for the root pass instead of
///        using the drawable texture, and a final blit must be performed. At a
///        minimum, if the damage rect is the entire buffer, we must not perform
///        a partial repaint. Beyond that, we could only experimentally
///        determine what this value should be. From looking at the Flutter
///        Gallery, we noticed that there are occassionally small partial
///        repaints which shave off trivial numbers of pixels.
constexpr float kImpellerRepaintRatio = 0.7f;

bool CompositorContext::ShouldPerformPartialRepaint(DlIRect bounds,
                                                    DlISize layer_tree_size) {
  if (bounds.IsEmpty()) {
    return false;
  }
  if (bounds.GetWidth() >= layer_tree_size.width &&
      bounds.GetHeight() >= layer_tree_size.height) {
    return false;
  }
  auto rx = static_cast<float>(bounds.GetWidth()) / layer_tree_size.width;
  auto ry = static_cast<float>(bounds.GetHeight()) / layer_tree_size.height;
  return rx <= kImpellerRepaintRatio || ry <= kImpellerRepaintRatio;
}

void CompositorContext::OnGrContextCreated() {
  texture_registry_->OnGrContextCreated();
#if !SLIMPELLER
  raster_cache_.Clear();
#endif  //  !SLIMPELLER
}

void CompositorContext::OnGrContextDestroyed() {
  texture_registry_->OnGrContextDestroyed();
#if !SLIMPELLER
  raster_cache_.Clear();
#endif  //  !SLIMPELLER
}

}  // namespace flutter
