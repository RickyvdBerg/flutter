// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "flutter/flow/layers/avio_window_preview_layer.h"
#include <algorithm>
#include <cmath>
namespace flutter {
AvioWindowPreviewLayer::AvioWindowPreviewLayer(uint64_t surface_id,
                                               DlRect rect,
                                               float corner_radius,
                                               bool replace_children)
    : surface_id_(surface_id),
      rect_(rect),
      corner_radius_(corner_radius),
      replace_children_(replace_children) {
  set_subtree_has_avio_window_preview(true);
}
void AvioWindowPreviewLayer::Diff(DiffContext* context,
                                  const Layer* old_layer) {
  DiffContext::AutoSubtreeRestore subtree(context);
  const auto* previous =
      old_layer ? old_layer->as_avio_window_preview_layer() : nullptr;
  if (!context->IsSubtreeDirty() &&
      (replace_children_ || !previous || previous->surface_id_ != surface_id_ ||
       previous->rect_ != rect_ || previous->corner_radius_ != corner_radius_ ||
       previous->replace_children_ != replace_children_)) {
    if (previous)
      context->MarkSubtreeDirty(context->GetOldLayerPaintRegion(previous));
    else
      context->MarkSubtreeDirty();
  }
  DiffChildren(context, previous);
  context->AddLayerBounds(rect_);
  context->SetLayerPaintRegion(this, context->CurrentSubtreeRegion());
}
void AvioWindowPreviewLayer::Preroll(PrerollContext* context) {
  admitted_ = false;
  ContainerLayer::Preroll(context);
  // The clear operation cannot absorb opacity or filters into its paint.
  context->renderable_state_flags = 0;
  set_paint_bounds(paint_bounds().Union(rect_));
  auto* previews = context->avio_window_previews;
  auto* invalid = context->avio_window_previews_invalid;
  if (!previews || !invalid)
    return;
  const auto& matrix = context->state_stack.matrix();
  const auto min_scale = matrix.GetMinScale2D();
  const auto max_scale = matrix.GetMaxScale2D();
  if (surface_id_ == 0 || rect_.IsEmpty() || !rect_.IsFinite() ||
      !std::isfinite(corner_radius_) || corner_radius_ < 0 ||
      !matrix.IsTranslationScaleOnly() || matrix.m[0] <= 0 ||
      matrix.m[5] <= 0 || !min_scale || !max_scale ||
      !std::isfinite(*max_scale) ||
      std::abs(*max_scale - *min_scale) > *max_scale * 0.0001f) {
    *invalid = true;
    return;
  }
  const auto rect = rect_.TransformAndClipBounds(matrix);
  const auto clip =
      rect.Intersection(context->state_stack.device_scene_cull_rect());
  const float opacity = context->state_stack.outstanding_opacity();
  if (!rect.IsFinite() || !std::isfinite(opacity) || opacity < 0 ||
      opacity > 1) {
    *invalid = true;
    return;
  }
  if (!clip || clip->IsEmpty() || opacity == 0)
    return;
  // Inline nodes keep their placeholder when the bounded frame set is full.
  // Explicit declarations have prepainted cutouts, so overflow must reject.
  if (previews->size() >= kMaxAvioWindowPreviewsPerFrame ||
      std::any_of(previews->begin(), previews->end(), [this](const auto& p) {
        return p.surface_id == surface_id_;
      })) {
    if (!replace_children_)
      *invalid = true;
    return;
  }
  admitted_ = true;
  previews->push_back(
      {surface_id_, rect, *clip, corner_radius_ * *max_scale, opacity});
}
void AvioWindowPreviewLayer::Paint(PaintContext& context) const {
  if (!replace_children_ || !admitted_) {
    PaintChildren(context);
    return;
  }
  auto restore = context.state_stack.applyState(rect_, 0);
  context.canvas->DrawRoundRect(
      DlRoundRect::MakeRectXY(rect_, corner_radius_, corner_radius_),
      DlPaint().setBlendMode(DlBlendMode::kClear));
}
}  // namespace flutter
