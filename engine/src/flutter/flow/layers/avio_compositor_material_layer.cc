// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/layers/avio_compositor_material_layer.h"

#include <algorithm>
#include <cmath>

namespace flutter {

AvioCompositorMaterialLayer::AvioCompositorMaterialLayer(
    AvioCompositorMaterial material)
    : material_(std::move(material)) {
  set_subtree_has_avio_compositor_material(true);
}

void AvioCompositorMaterialLayer::Diff(DiffContext* context,
                                       const Layer* old_layer) {
  DiffContext::AutoSubtreeRestore subtree(context);
  const auto* previous =
      old_layer ? old_layer->as_avio_compositor_material_layer() : nullptr;
  if (!context->IsSubtreeDirty() &&
      (previous == nullptr || previous->material_ != material_)) {
    if (previous != nullptr) {
      context->MarkSubtreeDirty(context->GetOldLayerPaintRegion(previous));
    } else {
      context->MarkSubtreeDirty();
    }
  }
  DiffChildren(context, previous);
  context->AddLayerBounds(material_.rect);
  context->SetLayerPaintRegion(this, context->CurrentSubtreeRegion());
}

void AvioCompositorMaterialLayer::Preroll(PrerollContext* context) {
  ContainerLayer::Preroll(context);
  set_paint_bounds(paint_bounds().Union(material_.rect));

  if (context->avio_compositor_materials == nullptr ||
      context->avio_compositor_materials_invalid == nullptr) {
    return;
  }

  if (!IsValidAvioCompositorMaterial(material_)) {
    *context->avio_compositor_materials_invalid = true;
    return;
  }

  const auto& matrix = context->state_stack.matrix();
  const auto full_bounds = material_.rect.TransformAndClipBounds(matrix);
  const auto transformed =
      full_bounds.Intersection(context->state_stack.device_scene_cull_rect());
  if (!transformed.has_value() || transformed->IsEmpty()) {
    return;
  }
  if (!context->state_stack.scene_clip_is_rectilinear()) {
    // The external compositor consumes axis-aligned rounded rectangles. A
    // path, rrect, or superellipse ancestor can cut an arbitrary shape from
    // this node, which cannot be represented by its material descriptor.
    *context->avio_compositor_materials_invalid = true;
    return;
  }
  const auto min_scale = matrix.GetMinScale2D();
  const auto max_scale = matrix.GetMaxScale2D();
  if (!matrix.IsTranslationScaleOnly() || !min_scale.has_value() ||
      !max_scale.has_value() || !std::isfinite(*min_scale) ||
      !std::isfinite(*max_scale) || *max_scale <= 0.0f || matrix.m[0] <= 0.0f ||
      matrix.m[5] <= 0.0f ||
      std::abs(*max_scale - *min_scale) > *max_scale * 0.0001f) {
    // The external compositor currently consumes axis-aligned rounded
    // rectangles. Publishing a bounding box for rotation, reflection,
    // perspective, or a non-uniform scale would silently describe a different
    // shape (and reflection would also require remapping corner identity).
    *context->avio_compositor_materials_invalid = true;
    return;
  }
  if (context->avio_compositor_materials->size() >=
      kMaxAvioCompositorMaterialsPerFrame) {
    *context->avio_compositor_materials_invalid = true;
    return;
  }
  if (std::any_of(context->avio_compositor_materials->begin(),
                  context->avio_compositor_materials->end(),
                  [id = material_.id](const auto& material) {
                    return material.id == id;
                  })) {
    // The id keys compositor cache identity. Two nodes cannot own it in the
    // same scene transaction; reject before either one's pixels are painted.
    *context->avio_compositor_materials_invalid = true;
    return;
  }

  auto material = material_;
  material.rect = transformed.value();
  material.corner_scale = *max_scale;
  // Clipping an original corner creates a square cut edge, not a new rounded
  // corner at the clipped boundary.
  if (material.rect.GetLeft() > full_bounds.GetLeft()) {
    material.corner_mask &= ~(0x01u | 0x08u);
  }
  if (material.rect.GetTop() > full_bounds.GetTop()) {
    material.corner_mask &= ~(0x01u | 0x02u);
  }
  if (material.rect.GetRight() < full_bounds.GetRight()) {
    material.corner_mask &= ~(0x02u | 0x04u);
  }
  if (material.rect.GetBottom() < full_bounds.GetBottom()) {
    material.corner_mask &= ~(0x04u | 0x08u);
  }
  material.strength =
      std::clamp(material.strength * context->state_stack.outstanding_opacity(),
                 0.0f, 1.0f);
  context->avio_compositor_materials->push_back(material);
}

void AvioCompositorMaterialLayer::Paint(PaintContext& context) const {
  PaintChildren(context);
}

}  // namespace flutter
