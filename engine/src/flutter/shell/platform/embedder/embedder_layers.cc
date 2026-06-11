// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_layers.h"

#include <algorithm>

#include "flutter/fml/logging.h"

namespace flutter {

EmbedderLayers::EmbedderLayers(DlISize frame_size,
                               double device_pixel_ratio,
                               DlMatrix root_surface_transformation,
                               uint64_t presentation_time,
                               std::optional<DlRegion> frame_damage)
    : frame_size_(frame_size),
      device_pixel_ratio_(device_pixel_ratio),
      root_surface_transformation_(root_surface_transformation),
      presentation_time_(presentation_time),
      frame_damage_(std::move(frame_damage)) {}

EmbedderLayers::~EmbedderLayers() = default;

void EmbedderLayers::PushBackingStoreLayer(
    const FlutterBackingStore* store,
    fml::UniqueFD render_complete_sync_fd,
    const std::vector<DlIRect>& paint_region_vec,
    FlutterShellLayerRole shell_layer_role,
    uint64_t shell_visual_identifier) {
  FlutterLayer layer = {};

  layer.struct_size = sizeof(FlutterLayer);
  layer.type = kFlutterLayerContentTypeBackingStore;
  layer.backing_store = store;

  const auto layer_bounds = DlRect::MakeSize(frame_size_);

  const auto transformed_layer_bounds =
      layer_bounds.TransformAndClipBounds(root_surface_transformation_);

  layer.offset.x = transformed_layer_bounds.GetX();
  layer.offset.y = transformed_layer_bounds.GetY();
  layer.size.width = transformed_layer_bounds.GetWidth();
  layer.size.height = transformed_layer_bounds.GetHeight();

  auto paint_region_rects = std::make_unique<std::vector<FlutterRect>>();
  paint_region_rects->reserve(paint_region_vec.size());

  for (const auto& rect : paint_region_vec) {
    auto transformed_rect =
        DlRect::Make(rect).TransformAndClipBounds(root_surface_transformation_);
    paint_region_rects->push_back(FlutterRect{
        .left = transformed_rect.GetLeft(),
        .top = transformed_rect.GetTop(),
        .right = transformed_rect.GetRight(),
        .bottom = transformed_rect.GetBottom(),
    });
  }

  auto paint_region = std::make_unique<FlutterRegion>();
  paint_region->struct_size = sizeof(FlutterRegion);
  paint_region->rects = paint_region_rects->data();
  paint_region->rects_count = paint_region_rects->size();
  rects_referenced_.push_back(std::move(paint_region_rects));

  auto present_info = std::make_unique<FlutterBackingStorePresentInfo>();
  present_info->struct_size = sizeof(FlutterBackingStorePresentInfo);
  present_info->paint_region = paint_region.get();

  if (frame_damage_.has_value()) {
    const auto target_local_frame_damage =
        DlRegion::MakeIntersection(*frame_damage_, DlRegion(paint_region_vec));
    auto frame_damage_rects = std::make_unique<std::vector<FlutterRect>>();
    const auto frame_damage_rect_list =
        target_local_frame_damage.getRects(/*deband=*/true);
    frame_damage_rects->reserve(frame_damage_rect_list.size());
    for (const auto& rect : frame_damage_rect_list) {
      auto transformed_rect =
          DlRect::Make(rect).TransformAndClipBounds(root_surface_transformation_);
      frame_damage_rects->push_back(FlutterRect{
          .left = transformed_rect.GetLeft(),
          .top = transformed_rect.GetTop(),
          .right = transformed_rect.GetRight(),
          .bottom = transformed_rect.GetBottom(),
      });
    }

    auto frame_damage_region = std::make_unique<FlutterRegion>();
    frame_damage_region->struct_size = sizeof(FlutterRegion);
    frame_damage_region->rects = frame_damage_rects->data();
    frame_damage_region->rects_count = frame_damage_rects->size();
    present_info->frame_damage = frame_damage_region.get();
    rects_referenced_.push_back(std::move(frame_damage_rects));
    regions_referenced_.push_back(std::move(frame_damage_region));
  } else {
    present_info->frame_damage = nullptr;
  }

  present_info->render_complete_sync_fd = -1;
#if !FML_OS_WIN
  if (render_complete_sync_fd.is_valid()) {
    present_info->render_complete_sync_fd = render_complete_sync_fd.get();
    render_complete_sync_fds_.push_back(std::move(render_complete_sync_fd));
  }
#else
  (void)render_complete_sync_fd;
#endif

  regions_referenced_.push_back(std::move(paint_region));
  layer.backing_store_present_info = present_info.get();
  layer.presentation_time = presentation_time_;
  layer.shell_layer_role = shell_layer_role;
  layer.shell_visual_identifier = shell_visual_identifier;
  layer.shell_visual_generation = 0;
  layer.shell_chrome_model_serial = 0;

  present_info_referenced_.push_back(std::move(present_info));
  presented_layers_.push_back(layer);
}

static std::unique_ptr<FlutterPlatformViewMutation> ConvertMutation(
    double opacity) {
  FlutterPlatformViewMutation mutation = {};
  mutation.type = kFlutterPlatformViewMutationTypeOpacity;
  mutation.opacity = opacity;
  return std::make_unique<FlutterPlatformViewMutation>(mutation);
}

static std::unique_ptr<FlutterPlatformViewMutation> ConvertMutation(
    const DlRect& rect) {
  FlutterPlatformViewMutation mutation = {};
  mutation.type = kFlutterPlatformViewMutationTypeClipRect;
  mutation.clip_rect.left = rect.GetLeft();
  mutation.clip_rect.top = rect.GetTop();
  mutation.clip_rect.right = rect.GetRight();
  mutation.clip_rect.bottom = rect.GetBottom();
  return std::make_unique<FlutterPlatformViewMutation>(mutation);
}

static FlutterSize ConvertSize(const DlSize& vector) {
  FlutterSize size = {};
  size.width = vector.width;
  size.height = vector.height;
  return size;
}

static std::unique_ptr<FlutterPlatformViewMutation> ConvertMutation(
    const DlRoundRect& rrect) {
  FlutterPlatformViewMutation mutation = {};
  mutation.type = kFlutterPlatformViewMutationTypeClipRoundedRect;
  const auto& rect = rrect.GetBounds();
  mutation.clip_rounded_rect.rect.left = rect.GetLeft();
  mutation.clip_rounded_rect.rect.top = rect.GetTop();
  mutation.clip_rounded_rect.rect.right = rect.GetRight();
  mutation.clip_rounded_rect.rect.bottom = rect.GetBottom();
  const auto& radii = rrect.GetRadii();
  mutation.clip_rounded_rect.upper_left_corner_radius =
      ConvertSize(radii.top_left);
  mutation.clip_rounded_rect.upper_right_corner_radius =
      ConvertSize(radii.top_right);
  mutation.clip_rounded_rect.lower_right_corner_radius =
      ConvertSize(radii.bottom_right);
  mutation.clip_rounded_rect.lower_left_corner_radius =
      ConvertSize(radii.bottom_left);
  return std::make_unique<FlutterPlatformViewMutation>(mutation);
}

static std::unique_ptr<FlutterPlatformViewMutation> ConvertMutation(
    const DlMatrix& matrix) {
  FlutterPlatformViewMutation mutation = {};
  mutation.type = kFlutterPlatformViewMutationTypeTransformation;
  mutation.transformation.scaleX = matrix.m[0];
  mutation.transformation.skewX = matrix.m[4];
  mutation.transformation.transX = matrix.m[12];
  mutation.transformation.skewY = matrix.m[1];
  mutation.transformation.scaleY = matrix.m[5];
  mutation.transformation.transY = matrix.m[13];
  mutation.transformation.pers0 = matrix.m[3];
  mutation.transformation.pers1 = matrix.m[7];
  mutation.transformation.pers2 = matrix.m[15];
  return std::make_unique<FlutterPlatformViewMutation>(mutation);
}

void EmbedderLayers::PushPlatformViewLayer(
    FlutterPlatformViewIdentifier identifier,
    const EmbeddedViewParams& params) {
  {
    FlutterPlatformView view = {};
    view.struct_size = sizeof(FlutterPlatformView);
    view.identifier = identifier;

    const auto& mutators = params.mutatorsStack();

    std::vector<const FlutterPlatformViewMutation*> mutations_array;

    for (auto i = mutators.Bottom(); i != mutators.Top(); ++i) {
      const auto& mutator = *i;
      switch (mutator->GetType()) {
        case MutatorType::kClipRect: {
          mutations_array.push_back(
              mutations_referenced_
                  .emplace_back(ConvertMutation(mutator->GetRect()))
                  .get());
        } break;
        case MutatorType::kClipRRect: {
          mutations_array.push_back(
              mutations_referenced_
                  .emplace_back(ConvertMutation(mutator->GetRRect()))
                  .get());
        } break;
        case MutatorType::kClipRSE: {
          mutations_array.push_back(
              mutations_referenced_
                  .emplace_back(ConvertMutation(mutator->GetRSEApproximation()))
                  .get());
        } break;
        case MutatorType::kClipPath: {
          // Unsupported mutation.
        } break;
        case MutatorType::kTransform: {
          const auto& matrix = mutator->GetMatrix();
          if (!matrix.IsIdentity()) {
            mutations_array.push_back(
                mutations_referenced_.emplace_back(ConvertMutation(matrix))
                    .get());
          }
        } break;
        case MutatorType::kOpacity: {
          const double opacity =
              std::clamp(mutator->GetAlphaFloat(), 0.0f, 1.0f);
          if (opacity < 1.0) {
            mutations_array.push_back(
                mutations_referenced_.emplace_back(ConvertMutation(opacity))
                    .get());
          }
        } break;
        case MutatorType::kBackdropFilter:
        case MutatorType::kBackdropClipRect:
        case MutatorType::kBackdropClipRRect:
        case MutatorType::kBackdropClipRSuperellipse:
        case MutatorType::kBackdropClipPath:
          break;
      }
    }

    if (!mutations_array.empty()) {
      // If there are going to be any mutations, they must first take into
      // account the root surface transformation.
      if (!root_surface_transformation_.IsIdentity()) {
        mutations_array.push_back(
            mutations_referenced_
                .emplace_back(ConvertMutation(root_surface_transformation_))
                .get());
      }

      auto mutations =
          std::make_unique<std::vector<const FlutterPlatformViewMutation*>>(
              mutations_array.rbegin(), mutations_array.rend());
      mutations_arrays_referenced_.emplace_back(std::move(mutations));

      view.mutations_count = mutations_array.size();
      view.mutations = mutations_arrays_referenced_.back().get()->data();
    }

    platform_views_referenced_.emplace_back(
        std::make_unique<FlutterPlatformView>(view));
  }

  FlutterLayer layer = {};

  layer.struct_size = sizeof(FlutterLayer);
  layer.type = kFlutterLayerContentTypePlatformView;
  layer.platform_view = platform_views_referenced_.back().get();

  const auto layer_bounds =
      DlRect::MakeXYWH(params.finalBoundingRect().GetX(),                //
                       params.finalBoundingRect().GetY(),                //
                       params.sizePoints().width * device_pixel_ratio_,  //
                       params.sizePoints().height * device_pixel_ratio_  //
      );

  const auto transformed_layer_bounds =
      layer_bounds.TransformAndClipBounds(root_surface_transformation_);

  layer.offset.x = transformed_layer_bounds.GetX();
  layer.offset.y = transformed_layer_bounds.GetY();
  layer.size.width = transformed_layer_bounds.GetWidth();
  layer.size.height = transformed_layer_bounds.GetHeight();

  layer.presentation_time = presentation_time_;
  layer.shell_layer_role = kFlutterShellLayerRoleEmbeddedContent;
  layer.shell_visual_identifier = 0;
  layer.shell_visual_generation = 0;
  layer.shell_chrome_model_serial = 0;

  presented_layers_.push_back(layer);
}

void EmbedderLayers::InvokePresentCallback(
    FlutterViewId view_id,
    std::shared_ptr<const EmbedderLayers> retained_layers,
    const PresentCallback& callback) {
  (void)retained_layers;
  auto presented_layers = presented_layers_;

  retained_layers_dependency_.reset();
  presented_layers_ = std::move(presented_layers);

  std::vector<const FlutterLayer*> presented_layers_pointers;
  presented_layers_pointers.reserve(presented_layers_.size());
  for (const auto& layer : presented_layers_) {
    presented_layers_pointers.push_back(&layer);
  }
  callback(view_id, presented_layers_pointers);
}

bool EmbedderLayers::InvokePresentRenderTargetCallback(
    FlutterViewId view_id,
    const PresentRenderTargetCallback& callback) {
  const FlutterLayer* backing_store_layer = nullptr;
  for (const auto& layer : presented_layers_) {
    switch (layer.type) {
      case kFlutterLayerContentTypeBackingStore:
        if (backing_store_layer != nullptr) {
          FML_LOG(ERROR)
              << "Explicit render-target presentation requires exactly one "
                 "backing-store layer.";
          return false;
        }
        backing_store_layer = &layer;
        break;
      case kFlutterLayerContentTypePlatformView:
        FML_LOG(ERROR)
            << "Explicit render-target presentation does not support platform "
               "view layers.";
        return false;
    }
  }

  if (backing_store_layer == nullptr || backing_store_layer->backing_store == nullptr ||
      backing_store_layer->backing_store_present_info == nullptr) {
    FML_LOG(ERROR)
        << "Explicit render-target presentation requires a backing store with "
           "present metadata.";
    return false;
  }

  return callback(view_id, backing_store_layer->backing_store,
                  backing_store_layer->backing_store_present_info);
}

}  // namespace flutter
