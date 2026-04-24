// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_layers.h"

#include <algorithm>

#include "flutter/fml/logging.h"

namespace flutter {

namespace {

constexpr FlutterPlatformViewIdentifier kShellLayerBreakToUnderlay = -901001;
constexpr FlutterPlatformViewIdentifier kShellLayerBreakToOverlay = -901002;
constexpr FlutterPlatformViewIdentifier kShellLayerBreakToPerWindowChrome =
    -901003;
constexpr FlutterPlatformViewIdentifier
    kShellLayerBreakToPerWindowChromeExplicitBase = -9000000000000000000LL;
constexpr uint64_t kShellLayerBreakToPerWindowChromeExplicitMaxVisualIdentifier =
    899999999999999999ULL;

std::optional<uint64_t> DecodePerWindowChromeVisualIdentifier(
    FlutterPlatformViewIdentifier identifier) {
  if (identifier <= kShellLayerBreakToPerWindowChromeExplicitBase) {
    return std::nullopt;
  }
  constexpr FlutterPlatformViewIdentifier kPerWindowChromeExplicitMaxViewId =
      kShellLayerBreakToPerWindowChromeExplicitBase +
      static_cast<FlutterPlatformViewIdentifier>(
          kShellLayerBreakToPerWindowChromeExplicitMaxVisualIdentifier);
  if (identifier > kPerWindowChromeExplicitMaxViewId) {
    return std::nullopt;
  }
  return static_cast<uint64_t>(identifier -
                               kShellLayerBreakToPerWindowChromeExplicitBase);
}

void ApplyShellSourceRect(FlutterLayer* layer,
                          const FlutterShellSourceInfo* source) {
  if (layer == nullptr || source == nullptr) {
    return;
  }
  const auto width = source->source_rect.right - source->source_rect.left;
  const auto height = source->source_rect.bottom - source->source_rect.top;
  if (width <= 0.0 || height <= 0.0) {
    return;
  }
  layer->offset.x = source->source_rect.left;
  layer->offset.y = source->source_rect.top;
  layer->size.width = width;
  layer->size.height = height;
}

bool IsShellLayerBoundaryMarker(FlutterPlatformViewIdentifier identifier) {
  switch (identifier) {
    case kShellLayerBreakToUnderlay:
    case kShellLayerBreakToOverlay:
    case kShellLayerBreakToPerWindowChrome:
      return true;
    default:
      return DecodePerWindowChromeVisualIdentifier(identifier).has_value();
  }
}

}  // namespace

EmbedderLayers::EmbedderLayers(SkISize frame_size,
                               double device_pixel_ratio,
                               SkMatrix root_surface_transformation,
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

  const auto layer_bounds = SkRect::MakeWH(frame_size_.width(), frame_size_.height());

  const auto transformed_layer_bounds =
      root_surface_transformation_.mapRect(layer_bounds);

  layer.offset.x = transformed_layer_bounds.x();
  layer.offset.y = transformed_layer_bounds.y();
  layer.size.width = transformed_layer_bounds.width();
  layer.size.height = transformed_layer_bounds.height();

  auto paint_region_rects = std::make_unique<std::vector<FlutterRect>>();
  paint_region_rects->reserve(paint_region_vec.size());

  for (const auto& rect : paint_region_vec) {
    auto transformed_rect =
        root_surface_transformation_.mapRect(SkRect::Make(ToSkIRect(rect)));
    paint_region_rects->push_back(FlutterRect{
        transformed_rect.x(),
        transformed_rect.y(),
        transformed_rect.right(),
        transformed_rect.bottom(),
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
    auto frame_damage_rects = std::make_unique<std::vector<FlutterRect>>();
    const auto frame_damage_rect_list = frame_damage_->getRects(/*deband=*/true);
    frame_damage_rects->reserve(frame_damage_rect_list.size());
    for (const auto& rect : frame_damage_rect_list) {
      auto transformed_rect =
          root_surface_transformation_.mapRect(SkRect::Make(ToSkIRect(rect)));
      frame_damage_rects->push_back(FlutterRect{
          transformed_rect.x(),
          transformed_rect.y(),
          transformed_rect.right(),
          transformed_rect.bottom(),
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
  const auto layer_bounds =
      SkRect::MakeXYWH(params.finalBoundingRect().x(),                     //
                       params.finalBoundingRect().y(),                     //
                       params.sizePoints().width() * device_pixel_ratio_,  //
                       params.sizePoints().height() * device_pixel_ratio_  //
      );

  const auto transformed_layer_bounds =
      root_surface_transformation_.mapRect(layer_bounds);

  if (IsShellLayerBoundaryMarker(identifier)) {
    saw_shell_layer_boundary_ = true;
    return;
  }

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
          break;
      }
    }

    if (!mutations_array.empty()) {
      // If there are going to be any mutations, they must first take into
      // account the root surface transformation.
      if (!root_surface_transformation_.isIdentity()) {
        auto matrix = ToDlMatrix(root_surface_transformation_);
        mutations_array.push_back(
            mutations_referenced_.emplace_back(ConvertMutation(matrix)).get());
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

  layer.offset.x = transformed_layer_bounds.x();
  layer.offset.y = transformed_layer_bounds.y();
  layer.size.width = transformed_layer_bounds.width();
  layer.size.height = transformed_layer_bounds.height();

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
    const GetShellVisualsCallback* get_shell_visuals_callback,
    const GetShellSourcesCallback* get_shell_sources_callback,
    const PresentCallback& callback) {
  (void)retained_layers;
  auto presented_layers = presented_layers_;
  if (!saw_shell_layer_boundary_) {
    for (auto& layer : presented_layers) {
      if (layer.type == kFlutterLayerContentTypeBackingStore) {
        layer.shell_layer_role = kFlutterShellLayerRoleOverlay;
      }
    }
  }

  auto is_anonymous_per_window_chrome = [](const FlutterLayer& layer) {
    return layer.type == kFlutterLayerContentTypeBackingStore &&
           layer.shell_layer_role == kFlutterShellLayerRolePerWindowChrome &&
           layer.shell_visual_identifier == 0;
  };
  auto is_explicit_per_window_chrome = [](const FlutterLayer& layer) {
    return layer.type == kFlutterLayerContentTypeBackingStore &&
           layer.shell_layer_role == kFlutterShellLayerRolePerWindowChrome &&
           layer.shell_visual_identifier != 0;
  };
  auto is_anonymous_overlay = [](const FlutterLayer& layer) {
    return layer.type == kFlutterLayerContentTypeBackingStore &&
           layer.shell_layer_role == kFlutterShellLayerRoleOverlay &&
           layer.shell_visual_identifier == 0;
  };

  std::vector<FlutterShellSourceInfo> shell_sources;
  if (get_shell_sources_callback != nullptr && *get_shell_sources_callback) {
    shell_sources = (*get_shell_sources_callback)(view_id);
  }

  auto source_for_role = [&shell_sources](FlutterShellLayerRole role)
      -> const FlutterShellSourceInfo* {
    for (const auto& source : shell_sources) {
      const auto width = source.source_rect.right - source.source_rect.left;
      const auto height = source.source_rect.bottom - source.source_rect.top;
      if (source.shell_layer_role == role && width > 0.0 && height > 0.0) {
        return &source;
      }
    }
    return nullptr;
  };

  const auto* overlay_source =
      source_for_role(kFlutterShellLayerRoleOverlay);
  const auto* per_window_chrome_source =
      source_for_role(kFlutterShellLayerRolePerWindowChrome);

  for (auto& layer : presented_layers) {
    if (is_anonymous_overlay(layer) && overlay_source != nullptr) {
      ApplyShellSourceRect(&layer, overlay_source);
    }
    if (is_anonymous_per_window_chrome(layer) &&
        per_window_chrome_source != nullptr) {
      ApplyShellSourceRect(&layer, per_window_chrome_source);
    }
  }

  // Diagnostic: count anonymous per-window chrome layers BEFORE fan-out.
  {
    size_t anon_chrome_count = 0;
    for (const auto& layer : presented_layers) {
      if (is_anonymous_per_window_chrome(layer)) {
        anon_chrome_count++;
      }
    }
    static int64_t diag_last_view_id = -1;
    static size_t diag_last_anon_chrome = SIZE_MAX;
    if (anon_chrome_count != diag_last_anon_chrome || view_id != diag_last_view_id) {
      FML_LOG(IMPORTANT)
          << "EmbedderLayers fan-out input: view=" << view_id
          << " anon_chrome_layers=" << anon_chrome_count
          << " total_layers=" << presented_layers.size()
          << " saw_boundary=" << saw_shell_layer_boundary_;
      diag_last_view_id = view_id;
      diag_last_anon_chrome = anon_chrome_count;
    }
  }

  if (get_shell_visuals_callback != nullptr && *get_shell_visuals_callback) {
    const auto shell_visuals = (*get_shell_visuals_callback)(view_id);
    if (!shell_visuals.empty()) {
      std::vector<const FlutterShellVisualInfo*> overlay_visuals;
      std::vector<const FlutterShellVisualInfo*> per_window_chrome_visuals;
      overlay_visuals.reserve(shell_visuals.size());
      per_window_chrome_visuals.reserve(shell_visuals.size());
      for (const auto& visual : shell_visuals) {
        const auto width = visual.source_rect.right - visual.source_rect.left;
        const auto height = visual.source_rect.bottom - visual.source_rect.top;
        if (visual.shell_visual_identifier == 0 || width <= 0.0 || height <= 0.0) {
          continue;
        }
        if (visual.shell_layer_role == kFlutterShellLayerRoleOverlay) {
          overlay_visuals.push_back(&visual);
        } else if (visual.shell_layer_role == kFlutterShellLayerRolePerWindowChrome) {
          per_window_chrome_visuals.push_back(&visual);
        }
      }
      auto find_per_window_chrome_visual =
          [&per_window_chrome_visuals](uint64_t visual_identifier)
          -> const FlutterShellVisualInfo* {
        for (const auto* visual : per_window_chrome_visuals) {
          if (visual->shell_visual_identifier == visual_identifier) {
            return visual;
          }
        }
        return nullptr;
      };

      size_t next_overlay_visual_index = 0;
      size_t matched_overlay_visuals = 0;
      size_t overlay_layers_without_visual = 0;
      size_t matched_visuals = 0;
      size_t dropped_explicit_per_window_chrome_layers = 0;

      std::vector<FlutterLayer> resolved_layers;
      resolved_layers.reserve(presented_layers.size() +
                              per_window_chrome_visuals.size());
      std::optional<FlutterLayer> overlay_source_layer;
      std::vector<uint64_t> matched_explicit_per_window_chrome_ids;
      matched_explicit_per_window_chrome_ids.reserve(
          per_window_chrome_visuals.size());
      for (auto layer : presented_layers) {
        if (is_anonymous_overlay(layer)) {
          if (!overlay_source_layer.has_value()) {
            overlay_source_layer = layer;
          }
          if (next_overlay_visual_index < overlay_visuals.size()) {
            const auto* visual = overlay_visuals[next_overlay_visual_index++];
            const auto width = visual->source_rect.right - visual->source_rect.left;
            const auto height = visual->source_rect.bottom - visual->source_rect.top;
            layer.offset.x = visual->source_rect.left;
            layer.offset.y = visual->source_rect.top;
            layer.size.width = width;
            layer.size.height = height;
            layer.shell_visual_identifier = visual->shell_visual_identifier;
            layer.shell_visual_generation = visual->shell_visual_generation;
            layer.shell_chrome_model_serial = 0;
            matched_overlay_visuals++;
          } else {
            overlay_layers_without_visual++;
          }
          resolved_layers.push_back(layer);
          continue;
        }

        if (is_anonymous_per_window_chrome(layer)) {
          continue;
        }

        if (is_explicit_per_window_chrome(layer)) {
          const auto* visual =
              find_per_window_chrome_visual(layer.shell_visual_identifier);
          if (visual == nullptr) {
            dropped_explicit_per_window_chrome_layers++;
            continue;
          }
          const auto width = visual->source_rect.right - visual->source_rect.left;
          const auto height = visual->source_rect.bottom - visual->source_rect.top;
          if (width <= 0.0 || height <= 0.0) {
            dropped_explicit_per_window_chrome_layers++;
            continue;
          }
          layer.offset.x = visual->source_rect.left;
          layer.offset.y = visual->source_rect.top;
          layer.size.width = width;
          layer.size.height = height;
          layer.shell_visual_identifier = visual->shell_visual_identifier;
          layer.shell_visual_generation = visual->shell_visual_generation;
          layer.shell_chrome_model_serial = visual->shell_chrome_model_serial;
          resolved_layers.push_back(layer);
          matched_explicit_per_window_chrome_ids.push_back(
              layer.shell_visual_identifier);
          matched_visuals++;
          continue;
        }

        if (!is_anonymous_per_window_chrome(layer)) {
          resolved_layers.push_back(layer);
        }
      }

      const size_t unused_overlay_visuals =
          overlay_visuals.size() - next_overlay_visual_index;
      size_t unused_visuals = 0;
      const bool has_overlay_source = overlay_source != nullptr;
      std::vector<const FlutterShellVisualInfo*> unmatched_per_window_chrome_visuals;
      unmatched_per_window_chrome_visuals.reserve(per_window_chrome_visuals.size());
      for (const auto* visual : per_window_chrome_visuals) {
        if (std::find(matched_explicit_per_window_chrome_ids.begin(),
                      matched_explicit_per_window_chrome_ids.end(),
                      visual->shell_visual_identifier) !=
            matched_explicit_per_window_chrome_ids.end()) {
          continue;
        }
        unmatched_per_window_chrome_visuals.push_back(visual);
      }
      if (!unmatched_per_window_chrome_visuals.empty()) {
        unused_visuals = unmatched_per_window_chrome_visuals.size();
      }

      if (dropped_explicit_per_window_chrome_layers > 0) {
        FML_LOG(WARNING)
            << "Dropped explicit per-window chrome layers without an active "
               "visual descriptor for view "
            << view_id
            << " (dropped=" << dropped_explicit_per_window_chrome_layers
            << ", chrome_visuals=" << per_window_chrome_visuals.size() << ")";
      }
      if (unused_visuals > 0) {
        FML_LOG(IMPORTANT)
            << "Per-window chrome layer/visual count mismatch for view " << view_id
            << " (matched=" << matched_visuals
            << ", fanout_fallback=disabled"
            << ", unused_visuals=" << unused_visuals
            << ", input_layers=" << presented_layers.size()
            << ", saw_boundary=" << saw_shell_layer_boundary_ << ")";
      }
      if (!overlay_visuals.empty() && !has_overlay_source) {
        FML_LOG(WARNING)
            << "Overlay visuals were provided for view " << view_id
            << " but no explicit overlay source channel was available";
      } else if (!overlay_visuals.empty() && matched_overlay_visuals == 0) {
        FML_LOG(WARNING)
            << "Overlay visuals were provided for view " << view_id
            << " but no anonymous overlay layers were available to annotate";
      } else if (overlay_layers_without_visual > 0 || unused_overlay_visuals > 0) {
        FML_LOG(WARNING)
            << "Overlay layer/visual count mismatch for view " << view_id
            << " (matched=" << matched_overlay_visuals
            << ", unmatched_overlay_layers=" << overlay_layers_without_visual
            << ", unused_overlay_visuals=" << unused_overlay_visuals << ")";
      }

      presented_layers = std::move(resolved_layers);
    } else {
      presented_layers.erase(
          std::remove_if(
              presented_layers.begin(), presented_layers.end(),
              [](const FlutterLayer& layer) {
                return layer.type == kFlutterLayerContentTypeBackingStore &&
                       layer.shell_layer_role ==
                           kFlutterShellLayerRolePerWindowChrome;
              }),
          presented_layers.end());
    }
  }

  // Diagnostic: count layers by role in the final presented set.
  {
    size_t bs_total = 0, bs_background = 0, bs_underlay = 0, bs_overlay = 0;
    size_t bs_per_window_chrome = 0, bs_embedded = 0, bs_unknown = 0;
    size_t platform_view_total = 0;
    for (const auto& layer : presented_layers) {
      if (layer.type == kFlutterLayerContentTypeBackingStore) {
        bs_total++;
        switch (layer.shell_layer_role) {
          case kFlutterShellLayerRoleBackground: bs_background++; break;
          case kFlutterShellLayerRoleUnderlay: bs_underlay++; break;
          case kFlutterShellLayerRoleOverlay: bs_overlay++; break;
          case kFlutterShellLayerRolePerWindowChrome: bs_per_window_chrome++; break;
          case kFlutterShellLayerRoleEmbeddedContent: bs_embedded++; break;
          default: bs_unknown++; break;
        }
      } else if (layer.type == kFlutterLayerContentTypePlatformView) {
        platform_view_total++;
      }
    }
    // Build a comma-separated list of chrome visual identifiers.
    std::string chrome_ids;
    for (const auto& layer : presented_layers) {
      if (layer.type == kFlutterLayerContentTypeBackingStore &&
          layer.shell_layer_role == kFlutterShellLayerRolePerWindowChrome &&
          layer.shell_visual_identifier != 0) {
        if (!chrome_ids.empty()) chrome_ids += ",";
        chrome_ids += std::to_string(layer.shell_visual_identifier);
      }
    }
    static int64_t last_view_id = -1;
    static size_t last_chrome_count = SIZE_MAX;
    static std::string last_chrome_ids;
    if (bs_per_window_chrome != last_chrome_count ||
        chrome_ids != last_chrome_ids ||
        view_id != last_view_id) {
      FML_LOG(IMPORTANT)
          << "EmbedderLayers::InvokePresentCallback view=" << view_id
          << " saw_boundary=" << saw_shell_layer_boundary_
          << " bs_total=" << bs_total
          << " bg=" << bs_background
          << " underlay=" << bs_underlay
          << " overlay=" << bs_overlay
          << " chrome=" << bs_per_window_chrome
          << " embedded=" << bs_embedded
          << " pv=" << platform_view_total
          << " chrome_ids=[" << chrome_ids << "]";
      last_view_id = view_id;
      last_chrome_count = bs_per_window_chrome;
      last_chrome_ids = chrome_ids;
    }
  }

  retained_layers_dependency_.reset();
  presented_layers_ = std::move(presented_layers);

  std::vector<const FlutterLayer*> presented_layers_pointers;
  presented_layers_pointers.reserve(presented_layers_.size());
  for (const auto& layer : presented_layers_) {
    presented_layers_pointers.push_back(&layer);
  }
  callback(view_id, presented_layers_pointers);
}

}  // namespace flutter
