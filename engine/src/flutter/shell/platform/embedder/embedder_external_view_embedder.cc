// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_external_view_embedder.h"

#include <cassert>
#include <cmath>
#include <utility>

#include "flutter/common/constants.h"
#include "flutter/shell/platform/embedder/embedder_layers.h"
#include "flutter/shell/platform/embedder/embedder_render_target.h"
#include "flutter/shell/platform/embedder/embedder_struct_macros.h"
#include "third_party/skia/include/core/SkRegion.h"
#include "third_party/skia/include/gpu/ganesh/GrDirectContext.h"

#ifdef IMPELLER_SUPPORTS_RENDERING
#include "impeller/display_list/dl_dispatcher.h"  // nogncheck
#endif                                            // IMPELLER_SUPPORTS_RENDERING

namespace flutter {

static const auto kRootViewIdentifier = EmbedderExternalView::ViewIdentifier{};

EmbedderExternalViewEmbedder::EmbedderExternalViewEmbedder(
    FlutterCompositorMode compositor_mode,
    bool selected_target_damage,
    bool avoid_backing_store_cache,
    const CreateRenderTargetCallback& create_render_target_callback,
    const PresentCallback& present_callback,
    const PresentRenderTargetCallback& present_render_target_callback)
    : compositor_mode_(compositor_mode),
      selected_target_damage_(selected_target_damage),
      avoid_backing_store_cache_(avoid_backing_store_cache),
      create_render_target_callback_(create_render_target_callback),
      present_callback_(present_callback),
      present_render_target_callback_(present_render_target_callback) {
  FML_DCHECK(create_render_target_callback_);
  switch (compositor_mode_) {
    case kFlutterCompositorModeGeneric:
      FML_DCHECK(present_callback_);
      FML_DCHECK(!present_render_target_callback_);
      break;
    case kFlutterCompositorModeRootRenderTarget:
      FML_DCHECK(!present_callback_);
      FML_DCHECK(present_render_target_callback_);
      break;
  }
}

EmbedderExternalViewEmbedder::~EmbedderExternalViewEmbedder() = default;

void EmbedderExternalViewEmbedder::CollectView(int64_t view_id) {
  render_target_caches_.erase(view_id);
  root_paint_regions_.erase(view_id);
}

void EmbedderExternalViewEmbedder::SetSurfaceTransformationCallback(
    SurfaceTransformationCallback surface_transformation_callback) {
  surface_transformation_callback_ = std::move(surface_transformation_callback);
}

DlMatrix EmbedderExternalViewEmbedder::GetSurfaceTransformation() const {
  if (!surface_transformation_callback_) {
    return DlMatrix{};
  }

  return surface_transformation_callback_();
}

void EmbedderExternalViewEmbedder::ResetPendingRootRenderTarget() {
  pending_root_render_target_.reset();
  pending_root_deferred_cleanup_render_targets_.clear();
  pending_root_descriptor_ = std::nullopt;
  pending_root_view_id_ = std::nullopt;
}

void EmbedderExternalViewEmbedder::Reset() {
  ResetPendingRootRenderTarget();
  pending_views_.clear();
  composition_order_.clear();
}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::CancelFrame() {
  Reset();
}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::BeginFrame(
    GrDirectContext* context,
    const fml::RefPtr<fml::RasterThreadMerger>& raster_thread_merger) {
  pending_frame_opportunity_ = std::nullopt;
}

void EmbedderExternalViewEmbedder::SetFrameOpportunity(
    std::optional<FrameOpportunityContext> frame_opportunity) {
  pending_frame_opportunity_ = frame_opportunity;
}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::PrepareFlutterView(
    DlISize frame_size,
    double device_pixel_ratio) {
  Reset();

  pending_frame_size_ = frame_size;
  pending_device_pixel_ratio_ = device_pixel_ratio;
  pending_surface_transformation_ = GetSurfaceTransformation();

  pending_views_[kRootViewIdentifier] = std::make_unique<EmbedderExternalView>(
      pending_frame_size_, pending_surface_transformation_);
  composition_order_.push_back(kRootViewIdentifier);
}

static FlutterBackingStoreConfig MakeBackingStoreConfig(
    int64_t view_id,
    const DlISize& backing_store_size,
    FlutterBackingStoreRequestType request_type,
    uint64_t shell_visual_identifier);

namespace {

constexpr size_t kMaxSelectedTargetDamageRects = 4096u;

SurfaceFrame::FramebufferInfo ReadSelectedTargetFramebufferInfo(
    const EmbedderRenderTarget& render_target,
    const DlMatrix& surface_transformation,
    const DlISize& frame_size) {
  SurfaceFrame::FramebufferInfo info;
  info.supports_readback = true;
  info.supports_partial_repaint = true;
  info.existing_damage = DlRegion(DlIRect::MakeSize(frame_size));

  const FlutterBackingStore* backing_store = render_target.GetBackingStore();
  const auto* content_state =
      SAFE_ACCESS(backing_store, content_state, nullptr);
  if (content_state == nullptr ||
      !STRUCT_HAS_MEMBER(content_state, existing_damage)) {
    return info;
  }

  info.target_identifier = content_state->target_identifier;
  info.content_epoch = content_state->content_epoch;
  info.preserved_contents = content_state->preserved_contents;
  if (!content_state->preserved_contents ||
      content_state->existing_damage == nullptr ||
      render_target.GetImpellerRenderTarget() == nullptr ||
      !surface_transformation.IsInvertible()) {
    return info;
  }

  const FlutterRegion* region = content_state->existing_damage;
  if (!STRUCT_HAS_MEMBER(region, rects) ||
      region->rects_count > kMaxSelectedTargetDamageRects ||
      (region->rects_count != 0 && region->rects == nullptr)) {
    return info;
  }

  const DlMatrix target_to_view = surface_transformation.Invert();
  const DlRect view_bounds = DlRect::MakeSize(frame_size);
  std::vector<DlIRect> damage_rects;
  damage_rects.reserve(region->rects_count);
  for (size_t index = 0; index < region->rects_count; index++) {
    const FlutterRect& rect = region->rects[index];
    if (!std::isfinite(rect.left) || !std::isfinite(rect.top) ||
        !std::isfinite(rect.right) || !std::isfinite(rect.bottom) ||
        rect.right <= rect.left || rect.bottom <= rect.top) {
      return info;
    }
    const DlRect target_rect =
        DlRect::MakeLTRB(rect.left, rect.top, rect.right, rect.bottom);
    const DlRect view_rect = target_rect.TransformAndClipBounds(target_to_view)
                                 .IntersectionOrEmpty(view_bounds);
    if (!view_rect.IsEmpty()) {
      damage_rects.push_back(DlIRect::RoundOut(view_rect));
    }
  }

  info.existing_damage = DlRegion(damage_rects);
  return info;
}

}  // namespace

std::optional<SurfaceFrame::FramebufferInfo>
EmbedderExternalViewEmbedder::AcquireRootRenderTarget(
    int64_t flutter_view_id,
    GrDirectContext* context,
    const std::shared_ptr<impeller::AiksContext>& aiks_context) {
  if (compositor_mode_ != kFlutterCompositorModeRootRenderTarget ||
      !selected_target_damage_) {
    return std::nullopt;
  }

  auto root_found = pending_views_.find(kRootViewIdentifier);
  if (root_found == pending_views_.end()) {
    return std::nullopt;
  }

  FML_DCHECK(!pending_root_view_id_.has_value());
  pending_root_view_id_ = flutter_view_id;

  const auto descriptor = root_found->second->CreateRenderTargetDescriptor();
  pending_root_descriptor_ = descriptor;
  EmbedderRenderTargetCache& render_target_cache =
      render_target_caches_[flutter_view_id];
  auto config = MakeBackingStoreConfig(flutter_view_id, descriptor.surface_size,
                                       descriptor.request_type,
                                       descriptor.shell_visual_identifier);
  pending_root_render_target_ =
      create_render_target_callback_(context, aiks_context, config);
  pending_root_deferred_cleanup_render_targets_ =
      render_target_cache.ClearAllRenderTargetsInCache();

  if (pending_root_render_target_ == nullptr) {
    SurfaceFrame::FramebufferInfo unavailable_info;
    unavailable_info.supports_readback = true;
    unavailable_info.supports_partial_repaint = true;
    unavailable_info.existing_damage =
        DlRegion(DlIRect::MakeSize(pending_frame_size_));
    return unavailable_info;
  }
  return ReadSelectedTargetFramebufferInfo(*pending_root_render_target_,
                                           pending_surface_transformation_,
                                           pending_frame_size_);
}

bool EmbedderExternalViewEmbedder::SupportsMetadataFrameDamageForCurrentFrame()
    const {
  return true;
}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::PrerollCompositeEmbeddedView(
    int64_t view_id,
    std::unique_ptr<EmbeddedViewParams> params) {
  auto vid = EmbedderExternalView::ViewIdentifier(view_id);
  FML_DCHECK(pending_views_.count(vid) == 0);

  pending_views_[vid] = std::make_unique<EmbedderExternalView>(
      pending_frame_size_,              // frame size
      pending_surface_transformation_,  // surface xformation
      vid,                              // view identifier
      std::move(params)                 // embedded view params
  );
  composition_order_.push_back(vid);
}

// |ExternalViewEmbedder|
DlCanvas* EmbedderExternalViewEmbedder::GetRootCanvas() {
  auto found = pending_views_.find(kRootViewIdentifier);
  if (found == pending_views_.end()) {
    FML_DLOG(WARNING)
        << "No root canvas could be found. This is extremely unlikely and "
           "indicates that the external view embedder did not receive the "
           "notification to begin the frame.";
    return nullptr;
  }
  return found->second->GetCanvas();
}

// |ExternalViewEmbedder|
DlCanvas* EmbedderExternalViewEmbedder::CompositeEmbeddedView(int64_t view_id) {
  auto vid = EmbedderExternalView::ViewIdentifier(view_id);
  auto found = pending_views_.find(vid);
  if (found == pending_views_.end()) {
    FML_DCHECK(false) << "Attempted to composite a view that was not "
                         "pre-rolled.";
    return nullptr;
  }
  return found->second->GetCanvas();
}

static FlutterBackingStoreConfig MakeBackingStoreConfig(
    int64_t view_id,
    const DlISize& backing_store_size,
    FlutterBackingStoreRequestType request_type,
    uint64_t shell_visual_identifier) {
  FlutterBackingStoreConfig config = {};

  config.struct_size = sizeof(config);

  config.size.width = backing_store_size.width;
  config.size.height = backing_store_size.height;
  config.view_id = view_id;
  config.shell_visual_identifier = shell_visual_identifier;
  config.request_type = request_type;

  return config;
}

static FlutterRect ToFlutterRect(const DlIRect& rect,
                                 const DlMatrix& transformation) {
  const auto transformed_rect =
      DlRect::Make(rect).TransformAndClipBounds(transformation);
  return FlutterRect{
      transformed_rect.GetLeft(),
      transformed_rect.GetTop(),
      transformed_rect.GetRight(),
      transformed_rect.GetBottom(),
  };
}

static std::vector<FlutterRect> ToFlutterRects(
    const std::vector<DlIRect>& rects,
    const DlMatrix& transformation) {
  std::vector<FlutterRect> flutter_rects;
  flutter_rects.reserve(rects.size());
  for (const auto& rect : rects) {
    flutter_rects.push_back(ToFlutterRect(rect, transformation));
  }
  return flutter_rects;
}

std::vector<FlutterAvioCompositorMaterial>
ConvertAvioCompositorMaterialsToEmbedderCoordinates(
    const std::vector<AvioCompositorMaterial>& materials,
    const DlMatrix& surface_transformation,
    double device_pixel_ratio) {
  const double logical_scale =
      device_pixel_ratio > 0.0 ? 1.0 / device_pixel_ratio : 1.0;
  const auto surface_scale =
      surface_transformation.GetMaxScale2D().value_or(0.0f);
  std::vector<FlutterAvioCompositorMaterial> result;
  result.reserve(materials.size());
  for (const auto& material : materials) {
    const auto rect =
        material.rect.TransformAndClipBounds(surface_transformation);
    result.push_back(FlutterAvioCompositorMaterial{
        .struct_size = sizeof(FlutterAvioCompositorMaterial),
        .id = material.id,
        .rect =
            FlutterRect{
                rect.GetLeft() * logical_scale,
                rect.GetTop() * logical_scale,
                rect.GetRight() * logical_scale,
                rect.GetBottom() * logical_scale,
            },
        .recipe =
            static_cast<FlutterAvioCompositorMaterialRecipe>(material.recipe),
        .tier = material.tier,
        .uses_default_corner = material.uses_default_corner,
        .corner_scale = static_cast<float>(material.corner_scale *
                                           surface_scale * logical_scale),
        .corner_radius = material.corner_radius,
        .corner_exponent = material.corner_exponent,
        .corner_mask = material.corner_mask,
        .blur_radius = material.blur_radius,
        .tint_red = material.tint_red,
        .tint_green = material.tint_green,
        .tint_blue = material.tint_blue,
        .tint_alpha = material.tint_alpha,
        .saturation = material.saturation,
        .luminosity = material.luminosity,
        .noise_opacity = material.noise_opacity,
        .order = material.order,
        .strength = material.strength,
    });
  }
  return result;
}

static_assert(kMaxAvioCompositorMaterialsPerFrame ==
              FLUTTER_AVIO_MAX_COMPOSITOR_MATERIALS);

static SkRegion ToSkRegion(const DlRegion& region) {
  SkRegion result;
  for (const DlIRect& rect : region.getRects(/*deband=*/true)) {
    result.op(SkIRect::MakeLTRB(rect.GetLeft(), rect.GetTop(), rect.GetRight(),
                                rect.GetBottom()),
              SkRegion::kUnion_Op);
  }
  return result;
}

static DlRegion ToDlRegion(const SkRegion& region) {
  std::vector<DlIRect> rects;
  for (SkRegion::Iterator iterator(region); !iterator.done(); iterator.next()) {
    const SkIRect& rect = iterator.rect();
    rects.push_back(DlIRect::MakeLTRB(rect.left(), rect.top(), rect.right(),
                                      rect.bottom()));
  }
  return DlRegion(rects);
}

static DlRegion UpdateRetainedPaintCoverage(
    const std::optional<DlRegion>& previous,
    const DlRegion& current_recording,
    const std::optional<DlRegion>& buffer_damage) {
  if (!previous.has_value() || !buffer_damage.has_value()) {
    return current_recording;
  }

  SkRegion retained = ToSkRegion(previous.value());
  const SkRegion replaced = ToSkRegion(buffer_damage.value());
  retained.op(replaced, SkRegion::kDifference_Op);
  retained.op(ToSkRegion(current_recording), SkRegion::kUnion_Op);
  return ToDlRegion(retained);
}

namespace {

struct PlatformView {
  EmbedderExternalView::ViewIdentifier view_identifier;
  const EmbeddedViewParams* params;

  // The frame of the platform view, after clipping, in screen coordinates.
  DlRect clipped_frame;

  explicit PlatformView(const EmbedderExternalView* view) {
    FML_DCHECK(view->HasPlatformView());
    view_identifier = view->GetViewIdentifier();
    params = view->GetEmbeddedViewParams();

    DlRect clip = view->GetEmbeddedViewParams()->finalBoundingRect();
    DlMatrix matrix;
    for (auto i = params->mutatorsStack().Begin();
         i != params->mutatorsStack().End(); ++i) {
      const auto& m = *i;
      switch (m->GetType()) {
        case MutatorType::kClipRect: {
          auto rect = m->GetRect().TransformAndClipBounds(matrix);
          clip = clip.IntersectionOrEmpty(rect);
          break;
        }
        case MutatorType::kClipRRect: {
          auto rect = m->GetRRect().GetBounds().TransformAndClipBounds(matrix);
          clip = clip.IntersectionOrEmpty(rect);
          break;
        }
        case MutatorType::kClipRSE: {
          auto rect = m->GetRSE().GetBounds().TransformAndClipBounds(matrix);
          clip = clip.IntersectionOrEmpty(rect);
          break;
        }
        case MutatorType::kClipPath: {
          auto rect = m->GetPath().GetBounds().TransformAndClipBounds(matrix);
          clip = clip.IntersectionOrEmpty(rect);
          break;
        }
        case MutatorType::kTransform: {
          matrix = matrix * m->GetMatrix();
          break;
        }
        case MutatorType::kOpacity:
        case MutatorType::kBackdropFilter:
        case MutatorType::kBackdropClipRect:
        case MutatorType::kBackdropClipRRect:
        case MutatorType::kBackdropClipRSuperellipse:
        case MutatorType::kBackdropClipPath:
          break;
      }
    }
    clipped_frame = clip;
  }
};

/// Each layer will result in a single physical surface that contains Flutter
/// contents. It may contain multiple platform views and the slices
/// that would be otherwise rendered between these platform views will be
/// collapsed into this layer, as long as they do not intersect any of the
/// platform views.
/// In Z order the Flutter contents of Layer is above the platform views.
class Layer {
 public:
  /// Returns whether the rectangle intersects any of the platform views of
  /// this layer.
  bool IntersectsPlatformView(const DlRect& rect) {
    for (auto& platform_view : platform_views_) {
      if (platform_view.clipped_frame.IntersectsWithRect(rect)) {
        return true;
      }
    }
    return false;
  }

  /// Returns whether the region intersects any of the platform views of this
  /// layer.
  bool IntersectsPlatformView(const DlRegion& region) {
    for (auto& platform_view : platform_views_) {
      auto clipped_frame = DlIRect::RoundOut(platform_view.clipped_frame);
      if (region.intersects(clipped_frame)) {
        return true;
      }
    }
    return false;
  }

  /// Returns whether the rectangle intersects any of the Flutter contents of
  /// this layer.
  bool IntersectsFlutterContents(const DlRect& rect) {
    return flutter_contents_region_.intersects(DlIRect::RoundOut(rect));
  }

  /// Returns whether the region intersects any of the Flutter contents of this
  /// layer.
  bool IntersectsFlutterContents(const DlRegion& region) {
    return flutter_contents_region_.intersects(region);
  }

  /// Adds a platform view to this layer.
  void AddPlatformView(const PlatformView& platform_view) {
    platform_views_.push_back(platform_view);
  }

  /// Adds Flutter contents to this layer.
  void AddFlutterContents(EmbedderExternalView* contents,
                          const DlRegion& contents_region) {
    flutter_contents_.push_back(contents);
    flutter_contents_region_ =
        DlRegion::MakeUnion(flutter_contents_region_, contents_region);
  }

  bool has_flutter_contents() const { return !flutter_contents_.empty(); }

  void SetRenderTarget(std::unique_ptr<EmbedderRenderTarget> target) {
    FML_DCHECK(render_target_ == nullptr);
    FML_DCHECK(has_flutter_contents());
    render_target_ = std::move(target);
  }

  /// Renders this layer Flutter contents to the render target previously
  /// assigned with SetRenderTarget.
  void RenderFlutterContents(bool frame_boundary) {
    FML_DCHECK(has_flutter_contents());
    if (!render_target_) {
      return;
    }

#ifdef IMPELLER_SUPPORTS_RENDERING
    if (render_target_->GetImpellerRenderTarget()) {
      RenderFlutterContentsImpeller(frame_boundary);
      return;
    }
#endif  // IMPELLER_SUPPORTS_RENDERING

#if SLIMPELLER
    FML_LOG(FATAL) << "Impeller opt-out unavailable.";
#else   // SLIMPELLER
    RenderFlutterContentsSkia();
#endif  // SLIMPELLER
  }

  /// Returns platform views for this layer. In Z-order the platform views are
  /// positioned *below* this layer's Flutter contents.
  const std::vector<PlatformView>& platform_views() const {
    return platform_views_;
  }

  EmbedderRenderTarget* render_target() { return render_target_.get(); }

  std::vector<DlIRect> coverage() {
    return flutter_contents_region_.getRects();
  }

  EmbedderExternalView::RenderTargetDescriptor CreateRenderTargetDescriptor(
      const DlISize& frame_size) const {
    return EmbedderExternalView::RenderTargetDescriptor(
        frame_size, kFlutterBackingStoreRequestTypeView, 0);
  }

  bool is_empty() const {
    return platform_views_.empty() && flutter_contents_.empty();
  }

 private:
#if !SLIMPELLER
  // TODO(https://github.com/flutter/flutter/issues/151670): Implement this
  // for Impeller as well.
  static void InvalidateApiState(SkSurface& skia_surface) {
    auto recording_context = skia_surface.recordingContext();

    // Should never happen.
    FML_DCHECK(recording_context) << "Recording context was null.";

    auto direct_context = recording_context->asDirectContext();
    if (direct_context == nullptr) {
      // Can happen when using software rendering.
      // Print an error but otherwise continue in that case.
      FML_LOG(ERROR) << "Embedder asked to invalidate cached graphics API "
                        "state but Flutter is not using a graphics API.";
    } else {
      direct_context->resetContext(kAll_GrBackendState);
    }
  }

  void RenderFlutterContentsSkia() {
    auto skia_surface = render_target_->GetSkiaSurface();
    if (!skia_surface) {
      return;
    }

    auto [ok, invalidate_api_state] = render_target_->MaybeMakeCurrent();

    if (invalidate_api_state) {
      InvalidateApiState(*skia_surface);
    }
    if (!ok) {
      FML_LOG(ERROR) << "Could not make the surface current.";
      return;
    }

    // Clear the current render target (most likely EGLSurface) at the
    // end of this scope.
    fml::ScopedCleanupClosure clear_current_surface([&]() {
      auto [ok, invalidate_api_state] = render_target_->MaybeClearCurrent();
      if (invalidate_api_state) {
        InvalidateApiState(*skia_surface);
      }
      if (!ok) {
        FML_LOG(ERROR) << "Could not clear the current surface.";
      }
    });

    auto canvas = skia_surface->getCanvas();
    if (!canvas) {
      return;
    }

    DlSkCanvasAdapter dl_canvas(canvas);
    bool clear_surface = true;
    for (auto c : flutter_contents_) {
      FML_DCHECK(render_target_->GetRenderTargetSize() ==
                 c->GetRenderSurfaceSize());
      c->Render(dl_canvas, clear_surface);
      clear_surface = false;
    }
    dl_canvas.Flush();
  }
#endif  //  !SLIMPELLER

#ifdef IMPELLER_SUPPORTS_RENDERING
  void RenderFlutterContentsImpeller(bool frame_boundary) {
    auto dl_builder = DisplayListBuilder();
    bool clear_surface = true;
    for (auto c : flutter_contents_) {
      FML_DCHECK(render_target_->GetRenderTargetSize() ==
                 c->GetRenderSurfaceSize());
      c->Render(dl_builder, clear_surface);
      clear_surface = false;
    }
    auto display_list = dl_builder.Build();

    auto* impeller_target = render_target_->GetImpellerRenderTarget();
    auto aiks_context = render_target_->GetAiksContext();
    auto cull_rect =
        impeller::Rect::MakeSize(impeller_target->GetRenderTargetSize());

    impeller::RenderToTarget(aiks_context->GetContentContext(),     //
                             *impeller_target,                      //
                             display_list,                          //
                             cull_rect,                             //
                             /*reset_host_buffer=*/frame_boundary,  //
                             /*is_onscreen=*/false                  //
    );
  }
#endif  // IMPELLER_SUPPORTS_RENDERING

  std::vector<PlatformView> platform_views_;
  std::vector<EmbedderExternalView*> flutter_contents_;
  DlRegion flutter_contents_region_;
  std::unique_ptr<EmbedderRenderTarget> render_target_;
  friend class LayerBuilder;
};

/// A layout builder is responsible for building an optimized list of Layers
/// from a list of `EmbedderExternalView`s. Single EmbedderExternalView contains
/// at most one platform view and at most one layer of Flutter contents
/// ('slice'). LayerBuilder is responsible for producing as few Layers from the
/// list of EmbedderExternalViews as possible while maintaining identical visual
/// result.
///
/// Implements https://flutter.dev/go/optimized-platform-view-layers
class LayerBuilder {
 public:
  using RenderTargetProvider =
      std::function<std::unique_ptr<EmbedderRenderTarget>(
          const EmbedderExternalView::RenderTargetDescriptor& descriptor)>;

  explicit LayerBuilder(DlISize frame_size, DlMatrix surface_transformation)
      : frame_size_(frame_size),
        surface_transformation_(surface_transformation) {
    layers_.push_back(Layer());
  }

  /// Adds the platform view and/or flutter contents from the
  /// EmbedderExternalView instance.
  ///
  /// This will try to add the content and platform view to an existing layer
  /// if possible. If not, a new layer will be created.
  void AddExternalView(EmbedderExternalView* view) {
    if (view->HasPlatformView()) {
      PlatformView platform_view(view);
      AddPlatformView(platform_view);
    }
    if (view->HasEngineRenderedContents()) {
      AddFlutterContents(view);
    }
  }

  /// Prepares the render targets for all layers that have Flutter contents.
  void PrepareBackingStore(const RenderTargetProvider& target_provider) {
    for (auto& layer : layers_) {
      if (layer.has_flutter_contents()) {
        layer.SetRenderTarget(
            target_provider(layer.CreateRenderTargetDescriptor(frame_size_)));
      }
    }
  }

  /// Renders all layers with Flutter contents to their respective render
  /// targets.
  void Render() {
    // Find the last layer that has Flutter contents.  The frame boundary flag
    // will be set for this layer.
    auto last_flutter_layer_rev_iter =
        std::find_if(layers_.rbegin(), layers_.rend(),
                     [](const Layer& l) { return l.has_flutter_contents(); });
    if (last_flutter_layer_rev_iter == layers_.rend()) {
      return;
    }
    auto last_flutter_layer_iter = last_flutter_layer_rev_iter.base() - 1;

    for (auto iter = layers_.begin(); iter != layers_.end(); iter++) {
      bool frame_boundary = iter == last_flutter_layer_iter;
      if (iter->has_flutter_contents()) {
        iter->RenderFlutterContents(frame_boundary);
      }
    }
  }

  /// Populates EmbedderLayers from layer builder's layers.
  void PushLayers(EmbedderLayers& layers) {
    for (auto& layer : layers_) {
      for (auto& view : layer.platform_views()) {
        auto platform_view_id = view.view_identifier.platform_view_id;
        if (platform_view_id.has_value()) {
          layers.PushPlatformViewLayer(platform_view_id.value(), *view.params);
        }
      }
      if (layer.render_target() != nullptr) {
        layers.PushBackingStoreLayer(
            layer.render_target()->GetBackingStore(),
            layer.render_target()->TakeRenderCompleteSyncFD(), layer.coverage(),
            kFlutterShellLayerRoleEmbeddedContent, 0);
      }
    }
  }

  /// Removes the render targets from layers and returns them for collection.
  std::vector<std::pair<EmbedderExternalView::RenderTargetDescriptor,
                        std::unique_ptr<EmbedderRenderTarget>>>
  ClearAndCollectRenderTargets() {
    std::vector<std::pair<EmbedderExternalView::RenderTargetDescriptor,
                          std::unique_ptr<EmbedderRenderTarget>>>
        result;
    for (auto& layer : layers_) {
      if (layer.render_target() != nullptr) {
        result.emplace_back(layer.CreateRenderTargetDescriptor(frame_size_),
                            std::move(layer.render_target_));
      }
    }
    layers_.clear();
    return result;
  }

 private:
  void AddPlatformView(PlatformView view) {
    auto& layer = GetLayerForPlatformView(view);
    layer.AddPlatformView(view);
  }

  void AddFlutterContents(EmbedderExternalView* contents) {
    FML_DCHECK(contents->HasEngineRenderedContents());

    DlRegion region = contents->GetDlRegion();
    GetLayerForFlutterContentsRegion(region).AddFlutterContents(contents,
                                                                region);
  }

  /// Returns the deepest layer to which the platform view can be added. That
  /// would be (whichever comes first):
  /// - First layer from back that has platform view that intersects with this
  ///   view
  /// - Very last layer from back that has surface that doesn't intersect with
  ///   this. That is because layer content renders on top of the platform view.
  Layer& GetLayerForPlatformView(PlatformView view) {
    for (auto iter = layers_.rbegin(); iter != layers_.rend(); ++iter) {
      // This layer has surface that intersects with this view. That means we
      // went one too far and need the layer before this.
      if (iter->IntersectsFlutterContents(view.clipped_frame)) {
        if (iter == layers_.rbegin()) {
          layers_.emplace_back();
          return layers_.back();
        } else {
          --iter;
          return *iter;
        }
      }
      if (iter->IntersectsPlatformView(view.clipped_frame)) {
        return *iter;
      }
    }
    return layers_.front();
  }

  /// Finds layer to which the Flutter content can be added. That would
  /// be first layer from back that has any intersection with this region.
  Layer& GetLayerForFlutterContentsRegion(const DlRegion& region) {
    for (auto iter = layers_.rbegin(); iter != layers_.rend(); ++iter) {
      if (iter->IntersectsPlatformView(region) ||
          iter->IntersectsFlutterContents(region)) {
        return *iter;
      }
    }
    return layers_.front();
  }

  std::vector<Layer> layers_;
  DlISize frame_size_;
  DlMatrix surface_transformation_;
};

};  // namespace

void EmbedderExternalViewEmbedder::SubmitFlutterView(
    int64_t flutter_view_id,
    GrDirectContext* context,
    const std::shared_ptr<impeller::AiksContext>& aiks_context,
    std::unique_ptr<SurfaceFrame> frame) {
  switch (compositor_mode_) {
    case kFlutterCompositorModeGeneric:
      SubmitGenericFlutterView(flutter_view_id, context, aiks_context,
                               std::move(frame));
      return;
    case kFlutterCompositorModeRootRenderTarget:
      SubmitRootRenderTarget(flutter_view_id, context, aiks_context,
                             std::move(frame));
      return;
  }
}

void EmbedderExternalViewEmbedder::SubmitGenericFlutterView(
    int64_t flutter_view_id,
    GrDirectContext* context,
    const std::shared_ptr<impeller::AiksContext>& aiks_context,
    std::unique_ptr<SurfaceFrame> frame) {
  EmbedderRenderTargetCache& render_target_cache =
      render_target_caches_[flutter_view_id];
  const DlRect transformed_bounds =
      DlRect::MakeSize(pending_frame_size_)
          .TransformAndClipBounds(pending_surface_transformation_);
  LayerBuilder builder(DlIRect::RoundOut(transformed_bounds).GetSize(),
                       pending_surface_transformation_);

  for (const auto& view_id : composition_order_) {
    builder.AddExternalView(pending_views_.at(view_id).get());
  }

  builder.PrepareBackingStore(
      [&](const EmbedderExternalView::RenderTargetDescriptor& descriptor) {
        if (!avoid_backing_store_cache_) {
          auto cached = render_target_cache.GetRenderTarget(descriptor);
          if (cached != nullptr) {
            return cached;
          }
        }
        auto config = MakeBackingStoreConfig(
            flutter_view_id, descriptor.surface_size, descriptor.request_type,
            descriptor.shell_visual_identifier);
        return create_render_target_callback_(context, aiks_context, config);
      });

  auto deferred_cleanup_render_targets =
      render_target_cache.ClearAllRenderTargetsInCache();

#if !SLIMPELLER
  if (context) {
    context->resetContext(kAll_GrBackendState);
  }
#endif  // !SLIMPELLER

  builder.Render();

  if (aiks_context) {
    aiks_context->GetContext()->DisposeThreadLocalCachedResources();
  }

#if !SLIMPELLER
  if (context) {
    context->flushAndSubmit();
  }
#endif  // !SLIMPELLER

  const auto& submit_info = frame->submit_info();
  const uint64_t presentation_time =
      submit_info.presentation_time.has_value()
          ? submit_info.presentation_time->ToEpochDelta().ToNanoseconds()
          : 0;
  EmbedderLayers presented_layers(
      pending_frame_size_, pending_device_pixel_ratio_,
      pending_surface_transformation_, presentation_time);
  builder.PushLayers(presented_layers);
  const auto compositor_materials =
      ConvertAvioCompositorMaterialsToEmbedderCoordinates(
          submit_info.avio_compositor_materials,
          pending_surface_transformation_, pending_device_pixel_ratio_);
  presented_layers.InvokePresentCallback(
      flutter_view_id, nullptr, compositor_materials,
      submit_info.avio_compositor_materials_invalid, present_callback_);

  deferred_cleanup_render_targets.clear();
  for (auto& [descriptor, render_target] :
       builder.ClearAndCollectRenderTargets()) {
    if (!avoid_backing_store_cache_) {
      render_target_cache.CacheRenderTarget(descriptor,
                                            std::move(render_target));
    }
  }

  frame->Submit();
}

void EmbedderExternalViewEmbedder::SubmitRootRenderTarget(
    int64_t flutter_view_id,
    GrDirectContext* context,
    const std::shared_ptr<impeller::AiksContext>& aiks_context,
    std::unique_ptr<SurfaceFrame> frame) {
  // The unordered_map render_target_cache creates a new entry if the view ID is
  // unrecognized.
  EmbedderRenderTargetCache& render_target_cache =
      render_target_caches_[flutter_view_id];
  auto root_found = pending_views_.find(kRootViewIdentifier);
  if (root_found == pending_views_.end()) {
    FML_LOG(ERROR) << "Explicit render-target presentation requires a root "
                      "Flutter view.";
    CompleteRootRenderTarget(
        flutter_view_id,
        kFlutterPresentRenderTargetStatusInternalInvariantViolation);
    ResetPendingRootRenderTarget();
    frame->Submit();
    return;
  }

  for (const auto& view_id : composition_order_) {
    if (view_id.platform_view_id.has_value()) {
      FML_LOG(ERROR)
          << "Explicit render-target presentation does not support embedded "
             "platform views.";
      CompleteRootRenderTarget(
          flutter_view_id,
          kFlutterPresentRenderTargetStatusUnsupportedPlatformView,
          pending_root_render_target_
              ? pending_root_render_target_->GetBackingStore()
              : nullptr);
      ResetPendingRootRenderTarget();
      frame->Submit();
      return;
    }
  }

  const auto& submit_info = frame->submit_info();
  const auto compositor_materials =
      ConvertAvioCompositorMaterialsToEmbedderCoordinates(
          submit_info.avio_compositor_materials,
          pending_surface_transformation_, pending_device_pixel_ratio_);
  if (submit_info.avio_compositor_materials_invalid) {
    CompleteRootRenderTarget(
        flutter_view_id,
        kFlutterPresentRenderTargetStatusInvalidCompositorMaterials,
        pending_root_render_target_
            ? pending_root_render_target_->GetBackingStore()
            : nullptr,
        nullptr, &compositor_materials, true);
    ResetPendingRootRenderTarget();
    frame->Submit();
    return;
  }

  auto& root_view = root_found->second;
  const bool has_previous_root_frame =
      root_paint_regions_.find(flutter_view_id) != root_paint_regions_.end();
  if ((!selected_target_damage_ || !has_previous_root_frame) &&
      !root_view->HasEngineRenderedContents() && compositor_materials.empty()) {
    CompleteRootRenderTarget(
        flutter_view_id, kFlutterPresentRenderTargetStatusNoVisualChange,
        pending_root_render_target_
            ? pending_root_render_target_->GetBackingStore()
            : nullptr);
    ResetPendingRootRenderTarget();
    frame->Submit();
    return;
  }

  auto descriptor = root_view->CreateRenderTargetDescriptor();
  std::unique_ptr<EmbedderRenderTarget> render_target;
  std::set<std::unique_ptr<EmbedderRenderTarget>>
      deferred_cleanup_render_targets;
  if (selected_target_damage_) {
    if (pending_root_view_id_ != flutter_view_id ||
        !pending_root_descriptor_.has_value()) {
      CompleteRootRenderTarget(
          flutter_view_id,
          kFlutterPresentRenderTargetStatusInternalInvariantViolation);
      ResetPendingRootRenderTarget();
      frame->Submit();
      return;
    }
    descriptor = pending_root_descriptor_.value();
    render_target = std::move(pending_root_render_target_);
    deferred_cleanup_render_targets =
        std::move(pending_root_deferred_cleanup_render_targets_);
    pending_root_descriptor_ = std::nullopt;
    pending_root_view_id_ = std::nullopt;
  } else {
    if (!avoid_backing_store_cache_) {
      render_target = render_target_cache.GetRenderTarget(descriptor);
    }
    if (render_target == nullptr) {
      auto config = MakeBackingStoreConfig(
          flutter_view_id, descriptor.surface_size, descriptor.request_type,
          descriptor.shell_visual_identifier);
      render_target =
          create_render_target_callback_(context, aiks_context, config);
    }
    deferred_cleanup_render_targets =
        render_target_cache.ClearAllRenderTargetsInCache();
  }
  if (render_target == nullptr) {
    FML_LOG(ERROR) << "Could not acquire an embedder render target for view "
                   << flutter_view_id;
    CompleteRootRenderTarget(
        flutter_view_id,
        kFlutterPresentRenderTargetStatusRenderTargetUnavailable);
    frame->Submit();
    return;
  }

#if !SLIMPELLER
  if (context) {
    context->resetContext(kAll_GrBackendState);
  }
#endif  // !SLIMPELLER

  if (selected_target_damage_ && submit_info.buffer_damage.has_value() &&
      submit_info.buffer_damage->isEmpty()) {
    CompleteRootRenderTarget(flutter_view_id,
                             kFlutterPresentRenderTargetStatusNoVisualChange,
                             render_target->GetBackingStore());
    deferred_cleanup_render_targets.clear();
    frame->Submit();
    return;
  }

  const auto render_bounds = DlRect::MakeSize(descriptor.surface_size);
  if (!root_view->Render(*render_target, render_bounds,
                         submit_info.buffer_damage)) {
    FML_LOG(ERROR) << "Could not render Flutter contents into explicit "
                      "render target for view "
                   << flutter_view_id;
    deferred_cleanup_render_targets.clear();
    CompleteRootRenderTarget(
        flutter_view_id, kFlutterPresentRenderTargetStatusRasterFailed,
        selected_target_damage_ ? render_target->GetBackingStore() : nullptr);
    frame->Submit();
    return;
  }

  std::optional<DlRegion> previous_paint_region;
  auto previous_paint = root_paint_regions_.find(flutter_view_id);
  if (previous_paint != root_paint_regions_.end()) {
    previous_paint_region = previous_paint->second;
  }
  DlRegion retained_paint_region = UpdateRetainedPaintCoverage(
      previous_paint_region, root_view->GetDlRegion(),
      submit_info.buffer_damage);
  root_paint_regions_[flutter_view_id] = retained_paint_region;

  if (aiks_context) {
    aiks_context->GetContext()->DisposeThreadLocalCachedResources();
  }

#if !SLIMPELLER
  if (context) {
    context->flushAndSubmit();
  }
#endif  // !SLIMPELLER

  auto paint_region_rects = ToFlutterRects(retained_paint_region.getRects(),
                                           pending_surface_transformation_);
  FlutterRegion paint_region = {
      .struct_size = sizeof(FlutterRegion),
      .rects_count = paint_region_rects.size(),
      .rects = paint_region_rects.data(),
  };

  auto frame_damage_rects =
      submit_info.frame_damage.has_value()
          ? ToFlutterRects(submit_info.frame_damage->getRects(
                               /*deband=*/true),
                           pending_surface_transformation_)
          : std::vector<FlutterRect>{};
  FlutterRegion frame_damage_region = {
      .struct_size = sizeof(FlutterRegion),
      .rects_count = frame_damage_rects.size(),
      .rects = frame_damage_rects.data(),
  };

  auto buffer_damage_rects =
      submit_info.buffer_damage.has_value()
          ? ToFlutterRects(submit_info.buffer_damage->getRects(
                               /*deband=*/true),
                           pending_surface_transformation_)
          : std::vector<FlutterRect>{};
  FlutterRegion buffer_damage_region = {
      .struct_size = sizeof(FlutterRegion),
      .rects_count = buffer_damage_rects.size(),
      .rects = buffer_damage_rects.data(),
  };

  auto render_complete_sync_fd = render_target->TakeRenderCompleteSyncFD();
  FlutterBackingStorePresentInfo present_info = {
      .struct_size = sizeof(FlutterBackingStorePresentInfo),
      .paint_region = &paint_region,
      .frame_damage =
          submit_info.frame_damage.has_value() ? &frame_damage_region : nullptr,
      .render_complete_sync_fd = -1,
      .buffer_damage = submit_info.buffer_damage.has_value()
                           ? &buffer_damage_region
                           : nullptr,
  };
#if !FML_OS_WIN
  if (render_complete_sync_fd.is_valid()) {
    present_info.render_complete_sync_fd = render_complete_sync_fd.get();
  }
#else
  (void)render_complete_sync_fd;
#endif

  if (!CompleteRootRenderTarget(flutter_view_id,
                                kFlutterPresentRenderTargetStatusPresented,
                                render_target->GetBackingStore(), &present_info,
                                &compositor_materials)) {
    FML_LOG(ERROR) << "Could not present explicit render target for view "
                   << flutter_view_id;
  }

  deferred_cleanup_render_targets.clear();
  if (!selected_target_damage_ && !avoid_backing_store_cache_) {
    render_target_cache.CacheRenderTarget(descriptor, std::move(render_target));
  }

  frame->Submit();
}

bool EmbedderExternalViewEmbedder::CompleteRootRenderTarget(
    int64_t flutter_view_id,
    FlutterPresentRenderTargetStatus status,
    const FlutterBackingStore* backing_store,
    const FlutterBackingStorePresentInfo* backing_store_present_info,
    const std::vector<FlutterAvioCompositorMaterial>* compositor_materials,
    bool compositor_materials_invalid) const {
  static const std::vector<FlutterAvioCompositorMaterial> kNoMaterials;
  return present_render_target_callback_(
      flutter_view_id,
      pending_frame_opportunity_.has_value() ? pending_frame_opportunity_->id
                                             : 0,
      pending_frame_opportunity_.has_value()
          ? static_cast<FlutterEngineDisplayId>(
                pending_frame_opportunity_->display_id)
          : 0,
      status, backing_store, backing_store_present_info,
      compositor_materials ? *compositor_materials : kNoMaterials,
      compositor_materials_invalid);
}

}  // namespace flutter
