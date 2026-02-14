// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/diff_context.h"

#include "flutter/flow/layers/layer.h"
#include "flutter/flow/raster_cache_util.h"

namespace flutter {

DiffContext::DiffContext(DlISize frame_size,
                         PaintRegionMap& this_frame_paint_region_map,
                         const PaintRegionMap& last_frame_paint_region_map,
                         bool has_raster_cache,
                         bool impeller_enabled)
    : rects_(std::make_shared<std::vector<DlRect>>()),
      frame_size_(frame_size),
      this_frame_paint_region_map_(this_frame_paint_region_map),
      last_frame_paint_region_map_(last_frame_paint_region_map),
      has_raster_cache_(has_raster_cache),
      impeller_enabled_(impeller_enabled) {}

void DiffContext::BeginSubtree() {
  state_stack_.push_back(state_);

  bool had_integral_transform = state_.integral_transform;
  state_.rect_index = rects_->size();
  state_.has_filter_bounds_adjustment = false;
  state_.has_texture = false;
  state_.integral_transform = false;

  if (had_integral_transform) {
    MakeTransformIntegral(state_.matrix_clip);
  }
}

void DiffContext::EndSubtree() {
  FML_DCHECK(!state_stack_.empty());
  if (state_.has_filter_bounds_adjustment) {
    filter_bounds_adjustment_stack_.pop_back();
  }
  state_ = state_stack_.back();
  state_stack_.pop_back();
}

DiffContext::State::State() : matrix_clip(kGiantRect, DlMatrix()) {}

void DiffContext::PushTransform(const DlMatrix& transform) {
  state_.matrix_clip.transform(transform);
}

void DiffContext::MakeTransformIntegral(
    DisplayListMatrixClipState& matrix_clip) {
  // TODO(knopp): This is duplicated from LayerStack. Maybe should be part of
  // clip tracker?
  DlMatrix integral;
  if (RasterCacheUtil::ComputeIntegralTransCTM(matrix_clip.matrix(),
                                               &integral)) {
    matrix_clip.setTransform(integral);
  }
}

void DiffContext::PushFilterBoundsAdjustment(
    const FilterBoundsAdjustment& filter) {
  FML_DCHECK(state_.has_filter_bounds_adjustment == false);
  state_.has_filter_bounds_adjustment = true;
  filter_bounds_adjustment_stack_.push_back(filter);
}

DlRect DiffContext::ApplyFilterBoundsAdjustment(DlRect rect) const {
  // Apply filter bounds adjustment in reverse order
  for (auto i = filter_bounds_adjustment_stack_.rbegin();
       i != filter_bounds_adjustment_stack_.rend(); ++i) {
    rect = (*i)(rect);
  }
  return rect;
}

void DiffContext::AlignRect(DlIRect& rect,
                            int horizontal_alignment,
                            int vertical_alignment) const {
  auto top = rect.GetTop();
  auto left = rect.GetLeft();
  auto right = rect.GetRight();
  auto bottom = rect.GetBottom();
  if (top % vertical_alignment != 0) {
    top -= top % vertical_alignment;
  }
  if (left % horizontal_alignment != 0) {
    left -= left % horizontal_alignment;
  }
  if (right % horizontal_alignment != 0) {
    right += horizontal_alignment - right % horizontal_alignment;
  }
  if (bottom % vertical_alignment != 0) {
    bottom += vertical_alignment - bottom % vertical_alignment;
  }
  right = std::min(right, frame_size_.width);
  bottom = std::min(bottom, frame_size_.height);
  rect = DlIRect::MakeLTRB(left, top, right, bottom);
}

Damage DiffContext::ComputeDamage(const DlRegion& additional_damage,
                                  int horizontal_clip_alignment,
                                  int vertical_clip_alignment) const {
  DlRegion buffer_damage = DlRegion::MakeUnion(additional_damage, damage_);
  DlRegion frame_damage = damage_;

  for (const auto& r : readbacks_) {
    DlRegion paint_rgn(r.paint_rect);
    DlRegion readback_rgn(r.readback_rect);
    // Changes either in readback or paint rect require repainting both readback
    // and paint rect.
    if (frame_damage.intersects(r.paint_rect) ||
        frame_damage.intersects(r.readback_rect)) {
      auto combined = DlRegion::MakeUnion(paint_rgn, readback_rgn);
      frame_damage = DlRegion::MakeUnion(frame_damage, combined);
      buffer_damage = DlRegion::MakeUnion(buffer_damage, combined);
    }
  }

  DlIRect frame_clip = DlIRect::MakeSize(frame_size_);
  DlRegion clip_region(frame_clip);

  Damage res;
  res.frame_damage = DlRegion::MakeIntersection(frame_damage, clip_region);
  res.buffer_damage = DlRegion::MakeIntersection(buffer_damage, clip_region);

  if (horizontal_clip_alignment > 1 || vertical_clip_alignment > 1) {
    auto align = [&](const DlRegion& rgn) -> DlRegion {
      auto rects = rgn.getRects(true);
      for (auto& rect : rects) {
        AlignRect(rect, horizontal_clip_alignment, vertical_clip_alignment);
      }
      return DlRegion(rects);
    };
    res.frame_damage = align(res.frame_damage);
    res.buffer_damage = align(res.buffer_damage);
  }
  return res;
}

DlRect DiffContext::MapRect(const DlRect& rect) {
  DlRect mapped_rect(rect);
  state_.matrix_clip.mapRect(&mapped_rect);
  return mapped_rect;
}

bool DiffContext::PushCullRect(const DlRect& clip) {
  state_.matrix_clip.clipRect(clip, DlClipOp::kIntersect, false);
  return !state_.matrix_clip.is_cull_rect_empty();
}

const DlMatrix& DiffContext::GetMatrix() const {
  return state_.matrix_clip.matrix();
}

DlRect DiffContext::GetCullRect() const {
  return state_.matrix_clip.GetLocalCullCoverage();
}

void DiffContext::MarkSubtreeDirty(const PaintRegion& previous_paint_region) {
  FML_DCHECK(!IsSubtreeDirty());
  if (previous_paint_region.is_valid()) {
    AddDamage(previous_paint_region);
  }
  state_.dirty = true;
}

void DiffContext::MarkSubtreeDirty(const DlRect& previous_paint_region) {
  FML_DCHECK(!IsSubtreeDirty());
  AddDamage(previous_paint_region);
  state_.dirty = true;
}

void DiffContext::AddLayerBounds(const DlRect& rect) {
  // During painting we cull based on non-overriden transform and then
  // override the transform right before paint. Do the same thing here to get
  // identical paint rect.
  auto transformed_rect = ApplyFilterBoundsAdjustment(MapRect(rect));
  if (transformed_rect.IntersectsWithRect(
          state_.matrix_clip.GetDeviceCullCoverage())) {
    if (state_.integral_transform) {
      DisplayListMatrixClipState temp_state = state_.matrix_clip;
      MakeTransformIntegral(temp_state);
      temp_state.mapRect(rect, &transformed_rect);
      transformed_rect = ApplyFilterBoundsAdjustment(transformed_rect);
    }
    rects_->push_back(transformed_rect);
    if (IsSubtreeDirty()) {
      AddDamage(transformed_rect);
    }
  }
}

void DiffContext::MarkSubtreeHasTextureLayer() {
  // Set the has_texture flag on current state and all parent states. That
  // way we'll know that we can't skip diff for retained layers because
  // they contain a TextureLayer.
  for (auto& state : state_stack_) {
    state.has_texture = true;
  }
  state_.has_texture = true;
}

void DiffContext::AddExistingPaintRegion(const PaintRegion& region) {
  // Adding paint region for retained layer implies that current subtree is not
  // dirty, so we know, for example, that the inherited transforms must match
  FML_DCHECK(!IsSubtreeDirty());
  if (region.is_valid()) {
    rects_->insert(rects_->end(), region.begin(), region.end());
  }
}

void DiffContext::AddReadbackRegion(const DlIRect& paint_rect,
                                    const DlIRect& readback_rect) {
  Readback readback;
  readback.paint_rect = paint_rect;
  readback.readback_rect = readback_rect;
  readback.position = rects_->size();
  // Push empty rect as a placeholder for position in current subtree
  rects_->push_back(DlRect());
  readbacks_.push_back(readback);
}

PaintRegion DiffContext::CurrentSubtreeRegion() const {
  bool has_readback = std::any_of(
      readbacks_.begin(), readbacks_.end(),
      [&](const Readback& r) { return r.position >= state_.rect_index; });
  return PaintRegion(rects_, state_.rect_index, rects_->size(), has_readback,
                     state_.has_texture);
}

void DiffContext::AddDamage(const PaintRegion& damage) {
  FML_DCHECK(damage.is_valid());
  std::vector<DlIRect> irects;
  irects.reserve(std::distance(damage.begin(), damage.end()));
  for (const auto& r : damage) {
    irects.push_back(DlIRect::RoundOut(r));
  }
  damage_ = DlRegion::MakeUnion(damage_, DlRegion(irects));
}

void DiffContext::AddDamage(const DlRect& rect) {
  damage_ = DlRegion::MakeUnion(damage_, DlRegion(DlIRect::RoundOut(rect)));
}

void DiffContext::SetLayerPaintRegion(const Layer* layer,
                                      const PaintRegion& region) {
  this_frame_paint_region_map_[layer->unique_id()] = region;
}

PaintRegion DiffContext::GetOldLayerPaintRegion(const Layer* layer) const {
  auto i = last_frame_paint_region_map_.find(layer->unique_id());
  if (i != last_frame_paint_region_map_.end()) {
    return i->second;
  } else {
    // This is valid when Layer::PreservePaintRegion is called for retained
    // layer with zero sized parent clip (these layers are not diffed)
    return PaintRegion();
  }
}

void DiffContext::Statistics::LogStatistics() {
#if !FLUTTER_RELEASE
  FML_TRACE_COUNTER("flutter", "DiffContext", reinterpret_cast<int64_t>(this),
                    "NewPictures", new_pictures_, "PicturesTooComplexToCompare",
                    pictures_too_complex_to_compare_, "DeepComparePictures",
                    deep_compare_pictures_, "SameInstancePictures",
                    same_instance_pictures_,
                    "DifferentInstanceButEqualPictures",
                    different_instance_but_equal_pictures_);
#endif  // !FLUTTER_RELEASE
}

}  // namespace flutter
