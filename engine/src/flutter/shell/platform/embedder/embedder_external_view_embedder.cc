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
#include "third_party/skia/include/gpu/ganesh/GrDirectContext.h"

namespace flutter {

static const auto kRootViewIdentifier = EmbedderExternalView::ViewIdentifier{};

namespace {

constexpr int64_t kShellLayerBreakToUnderlay = -901001;
constexpr int64_t kShellLayerBreakToOverlay = -901002;
constexpr int64_t kShellLayerBreakToPerWindowChrome = -901003;
constexpr int64_t kShellLayerBreakToPerWindowChromeExplicitBase =
    -9000000000000000000LL;
constexpr uint64_t kShellLayerBreakToPerWindowChromeExplicitMaxVisualIdentifier =
    899999999999999999ULL;

std::optional<uint64_t> DecodePerWindowChromeVisualIdentifier(int64_t view_id) {
  if (view_id <= kShellLayerBreakToPerWindowChromeExplicitBase) {
    return std::nullopt;
  }
  constexpr int64_t kPerWindowChromeExplicitMaxViewId =
      kShellLayerBreakToPerWindowChromeExplicitBase +
      static_cast<int64_t>(
          kShellLayerBreakToPerWindowChromeExplicitMaxVisualIdentifier);
  if (view_id > kPerWindowChromeExplicitMaxViewId) {
    return std::nullopt;
  }
  return static_cast<uint64_t>(view_id -
                               kShellLayerBreakToPerWindowChromeExplicitBase);
}

bool IsShellLayerBoundaryMarker(int64_t view_id) {
  return view_id == kShellLayerBreakToUnderlay ||
         view_id == kShellLayerBreakToOverlay ||
         view_id == kShellLayerBreakToPerWindowChrome ||
         DecodePerWindowChromeVisualIdentifier(view_id).has_value();
}

FlutterBackingStoreRequestType RequestTypeForShellBoundaryMarker(
    int64_t view_id) {
  switch (view_id) {
    case kShellLayerBreakToPerWindowChrome:
      return kFlutterBackingStoreRequestTypePerWindowChrome;
    case kShellLayerBreakToUnderlay:
    case kShellLayerBreakToOverlay:
    default:
      return DecodePerWindowChromeVisualIdentifier(view_id).has_value()
                 ? kFlutterBackingStoreRequestTypePerWindowChrome
                 : kFlutterBackingStoreRequestTypeView;
  }
}

FlutterShellLayerRole ShellLayerRoleForBoundaryMarker(int64_t view_id) {
  switch (view_id) {
    case kShellLayerBreakToUnderlay:
      return kFlutterShellLayerRoleUnderlay;
    case kShellLayerBreakToOverlay:
      return kFlutterShellLayerRoleOverlay;
    case kShellLayerBreakToPerWindowChrome:
      return kFlutterShellLayerRolePerWindowChrome;
    default:
      return DecodePerWindowChromeVisualIdentifier(view_id).has_value()
                 ? kFlutterShellLayerRolePerWindowChrome
                 : kFlutterShellLayerRoleBackground;
  }
}

}  // namespace

EmbedderExternalViewEmbedder::EmbedderExternalViewEmbedder(
    bool avoid_backing_store_cache,
    const CreateRenderTargetCallback& create_render_target_callback,
    const PresentCallback& present_callback,
    const GetShellVisualsCallback& get_shell_visuals_callback,
    const GetShellSourcesCallback& get_shell_sources_callback)
    : avoid_backing_store_cache_(avoid_backing_store_cache),
      create_render_target_callback_(create_render_target_callback),
      present_callback_(present_callback),
      get_shell_visuals_callback_(get_shell_visuals_callback),
      get_shell_sources_callback_(get_shell_sources_callback) {
  FML_DCHECK(create_render_target_callback_);
  FML_DCHECK(present_callback_);
}

EmbedderExternalViewEmbedder::~EmbedderExternalViewEmbedder() = default;

void EmbedderExternalViewEmbedder::CollectView(int64_t view_id) {
  render_target_caches_.erase(view_id);
  retained_presented_layers_.erase(view_id);
}

void EmbedderExternalViewEmbedder::SetSurfaceTransformationCallback(
    SurfaceTransformationCallback surface_transformation_callback) {
  surface_transformation_callback_ = std::move(surface_transformation_callback);
}

SkMatrix EmbedderExternalViewEmbedder::GetSurfaceTransformation() const {
  if (!surface_transformation_callback_) {
    return SkMatrix{};
  }

  return surface_transformation_callback_();
}

void EmbedderExternalViewEmbedder::Reset() {
  pending_views_.clear();
  composition_order_.clear();
  has_shell_layer_boundary_markers_ = false;
}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::CancelFrame() {
  Reset();
}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::BeginFrame(
    GrDirectContext* context,
    const fml::RefPtr<fml::RasterThreadMerger>& raster_thread_merger) {}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::PrepareFlutterView(
    SkISize frame_size,
    double device_pixel_ratio) {
  Reset();

  pending_frame_size_ = frame_size;
  pending_device_pixel_ratio_ = device_pixel_ratio;
  pending_surface_transformation_ = GetSurfaceTransformation();

  pending_views_[kRootViewIdentifier] = std::make_unique<EmbedderExternalView>(
      pending_frame_size_, pending_surface_transformation_);
  composition_order_.push_back(kRootViewIdentifier);
}

bool EmbedderExternalViewEmbedder::SupportsMetadataFrameDamageForCurrentFrame()
    const {
  return !has_shell_layer_boundary_markers_;
}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::PrerollCompositeEmbeddedView(
    int64_t view_id,
    std::unique_ptr<EmbeddedViewParams> params) {
  if (IsShellLayerBoundaryMarker(view_id)) {
    has_shell_layer_boundary_markers_ = true;
  }
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
    const SkISize& backing_store_size,
    FlutterBackingStoreRequestType request_type,
    uint64_t shell_visual_identifier) {
  FlutterBackingStoreConfig config = {};

  config.struct_size = sizeof(config);

  config.size.width = backing_store_size.width();
  config.size.height = backing_store_size.height();
  config.view_id = view_id;
  config.shell_visual_identifier = shell_visual_identifier;
  config.request_type = request_type;

  return config;
}

namespace {

struct PlatformView {
  EmbedderExternalView::ViewIdentifier view_identifier;
  const EmbeddedViewParams* params;

  // The frame of the platform view, after clipping, in screen coordinates.
  SkRect clipped_frame;

  explicit PlatformView(const EmbedderExternalView* view) {
    FML_DCHECK(view->HasPlatformView());
    view_identifier = view->GetViewIdentifier();
    params = view->GetEmbeddedViewParams();

    DlRect clip = ToDlRect(view->GetEmbeddedViewParams()->finalBoundingRect());
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
          break;
      }
    }
    clipped_frame = ToSkRect(clip);
  }

  bool IsShellLayerBoundaryMarker() const {
    return view_identifier.platform_view_id.has_value() &&
           ::flutter::IsShellLayerBoundaryMarker(
               view_identifier.platform_view_id.value());
  }

  std::optional<uint64_t> shell_visual_identifier() const {
    if (!view_identifier.platform_view_id.has_value()) {
      return std::nullopt;
    }
    return DecodePerWindowChromeVisualIdentifier(
        view_identifier.platform_view_id.value());
  }

  bool IsExplicitPerWindowChromeBoundaryMarker() const {
    return shell_visual_identifier().has_value();
  }

  FlutterBackingStoreRequestType backing_store_request_type() const {
    if (!view_identifier.platform_view_id.has_value()) {
      return kFlutterBackingStoreRequestTypeView;
    }
    return RequestTypeForShellBoundaryMarker(
        view_identifier.platform_view_id.value());
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
  bool IntersectsPlatformView(const SkRect& rect) {
    for (auto& platform_view : platform_views_) {
      if (platform_view.clipped_frame.intersects(rect)) {
        return true;
      }
    }
    return false;
  }

  /// Returns whether the region intersects any of the platform views of this
  /// layer.
  bool IntersectsPlatformView(const DlRegion& region) {
    for (auto& platform_view : platform_views_) {
      if (region.intersects(platform_view.clipped_frame.roundOut())) {
        return true;
      }
    }
    return false;
  }

  /// Returns whether the rectangle intersects any of the Flutter contents of
  /// this layer.
  bool IntersectsFlutterContents(const SkRect& rect) {
    return flutter_contents_region_.intersects(rect.roundOut());
  }

  /// Returns whether the region intersects any of the Flutter contents of this
  /// layer.
  bool IntersectsFlutterContents(const DlRegion& region) {
    return flutter_contents_region_.intersects(region);
  }

  /// Adds a platform view to this layer.
  void AddPlatformView(const PlatformView& platform_view,
                       const SkMatrix& surface_transformation) {
    if (platform_view.IsShellLayerBoundaryMarker()) {
      request_type_ = platform_view.backing_store_request_type();
      shell_layer_role_ = ShellLayerRoleForBoundaryMarker(
          platform_view.view_identifier.platform_view_id.value());
      if (platform_view.IsExplicitPerWindowChromeBoundaryMarker()) {
        shell_visual_identifier_ =
            platform_view.shell_visual_identifier().value_or(0);
        render_target_bounds_ =
            surface_transformation.mapRect(platform_view.clipped_frame);
      }
    }
    platform_views_.push_back(platform_view);
  }

  /// Adds Flutter contents to this layer.
  void AddFlutterContents(EmbedderExternalView* contents,
                          const DlRegion& contents_region) {
    flutter_contents_.push_back(contents);
    flutter_contents_region_ =
        DlRegion::MakeUnion(flutter_contents_region_, contents_region);
  }

  bool IsExplicitPerWindowChromeLayer() const {
    return request_type_ == kFlutterBackingStoreRequestTypePerWindowChrome &&
           shell_visual_identifier_ != 0;
  }

  bool NeedsExplicitPerWindowChromeContents() const {
    return IsExplicitPerWindowChromeLayer() && flutter_contents_.empty() &&
           render_target_bounds_.has_value();
  }

  bool CanProvideContentsFor(const Layer& target) const {
    if (!has_flutter_contents() || !target.render_target_bounds_.has_value()) {
      return false;
    }
    return flutter_contents_region_.intersects(
        target.render_target_bounds_->roundOut());
  }

  void CopyFlutterContentsFrom(const Layer& source) {
    flutter_contents_ = source.flutter_contents_;
    flutter_contents_region_ = source.flutter_contents_region_;
  }

  bool has_flutter_contents() const { return !flutter_contents_.empty(); }

  void SetRenderTarget(std::unique_ptr<EmbedderRenderTarget> target) {
    FML_DCHECK(render_target_ == nullptr);
    FML_DCHECK(has_flutter_contents());
    render_target_ = std::move(target);
  }

  /// Renders this layer Flutter contents to the render target previously
  /// assigned with SetRenderTarget.
  void RenderFlutterContents(const SkRect& default_render_bounds) {
    FML_DCHECK(has_flutter_contents());
    if (render_target_) {
      const auto render_target_bounds =
          render_target_bounds_.value_or(default_render_bounds);
      bool clear_surface = true;
      for (auto c : flutter_contents_) {
        c->Render(*render_target_, render_target_bounds, clear_surface);
        clear_surface = false;
      }
    }
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
      const SkISize& frame_size) const {
    const auto render_target_bounds =
        render_target_bounds_.value_or(SkRect::MakeWH(frame_size.width(),
                                                      frame_size.height()));
    return EmbedderExternalView::RenderTargetDescriptor(
        SkISize::Make(static_cast<int>(std::ceil(render_target_bounds.width())),
                      static_cast<int>(std::ceil(render_target_bounds.height()))),
        request_type_,
        shell_visual_identifier_);
  }

  bool is_empty() const {
    return platform_views_.empty() && flutter_contents_.empty();
  }

 private:
  std::vector<PlatformView> platform_views_;
  std::vector<EmbedderExternalView*> flutter_contents_;
  DlRegion flutter_contents_region_;
  std::unique_ptr<EmbedderRenderTarget> render_target_;
  FlutterBackingStoreRequestType request_type_ =
      kFlutterBackingStoreRequestTypeView;
  FlutterShellLayerRole shell_layer_role_ = kFlutterShellLayerRoleBackground;
  uint64_t shell_visual_identifier_ = 0;
  std::optional<SkRect> render_target_bounds_;
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

  explicit LayerBuilder(SkISize frame_size,
                        SkMatrix surface_transformation)
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
    PopulateExplicitPerWindowChromeLayers();
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
    const auto full_frame_bounds =
        SkRect::MakeWH(frame_size_.width(), frame_size_.height());
    for (auto& layer : layers_) {
      if (layer.has_flutter_contents()) {
        layer.RenderFlutterContents(full_frame_bounds);
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
        layers.PushBackingStoreLayer(layer.render_target()->GetBackingStore(),
                                     layer.render_target()->TakeRenderCompleteSyncFD(),
                                     layer.coverage(), layer.shell_layer_role_,
                                     layer.shell_visual_identifier_);
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
  void PopulateExplicitPerWindowChromeLayers() {
    for (size_t index = 0; index < layers_.size(); index++) {
      auto& target = layers_[index];
      if (!target.NeedsExplicitPerWindowChromeContents()) {
        continue;
      }

      Layer* source = nullptr;
      for (size_t candidate_index = index; candidate_index > 0;
           candidate_index--) {
        auto& candidate = layers_[candidate_index - 1];
        if (candidate.CanProvideContentsFor(target)) {
          source = &candidate;
          break;
        }
      }
      if (source == nullptr) {
        for (size_t candidate_index = index + 1; candidate_index < layers_.size();
             candidate_index++) {
          auto& candidate = layers_[candidate_index];
          if (candidate.CanProvideContentsFor(target)) {
            source = &candidate;
            break;
          }
        }
      }
      if (source != nullptr) {
        target.CopyFlutterContentsFrom(*source);
      }
    }
  }

  void AddPlatformView(PlatformView view) {
    auto& layer = GetLayerForPlatformView(view);
    layer.AddPlatformView(view, surface_transformation_);
    if (view.IsExplicitPerWindowChromeBoundaryMarker()) {
      pending_explicit_per_window_chrome_layer_index_ =
          layers_.empty() ? std::nullopt
                          : std::optional<size_t>(layers_.size() - 1);
    }
  }

  void AddFlutterContents(EmbedderExternalView* contents) {
    FML_DCHECK(contents->HasEngineRenderedContents());

    DlRegion region = contents->GetDlRegion();
    if (pending_explicit_per_window_chrome_layer_index_.has_value()) {
      const size_t layer_index =
          pending_explicit_per_window_chrome_layer_index_.value();
      pending_explicit_per_window_chrome_layer_index_.reset();
      if (layer_index < layers_.size()) {
        layers_[layer_index].AddFlutterContents(contents, region);
        return;
      }
    }
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
    if (view.IsExplicitPerWindowChromeBoundaryMarker()) {
      if (layers_.empty() || !layers_.back().is_empty()) {
        layers_.emplace_back();
      }
      return layers_.back();
    }
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
  std::optional<size_t> pending_explicit_per_window_chrome_layer_index_;
  SkISize frame_size_;
  SkMatrix surface_transformation_;
};

};  // namespace

void EmbedderExternalViewEmbedder::SubmitFlutterView(
    int64_t flutter_view_id,
    GrDirectContext* context,
    const std::shared_ptr<impeller::AiksContext>& aiks_context,
    std::unique_ptr<SurfaceFrame> frame) {
  // The unordered_map render_target_cache creates a new entry if the view ID is
  // unrecognized.
  EmbedderRenderTargetCache& render_target_cache =
      render_target_caches_[flutter_view_id];
  SkRect _rect = SkRect::MakeIWH(pending_frame_size_.width(),
                                 pending_frame_size_.height());
  pending_surface_transformation_.mapRect(&_rect);

  LayerBuilder builder(SkISize::Make(_rect.width(), _rect.height()),
                       pending_surface_transformation_);

  for (auto view_id : composition_order_) {
    auto& view = pending_views_[view_id];
    builder.AddExternalView(view.get());
  }

  builder.PrepareBackingStore([&](
                                  const EmbedderExternalView::
                                      RenderTargetDescriptor& descriptor) {
    if (!avoid_backing_store_cache_) {
      std::unique_ptr<EmbedderRenderTarget> target =
          render_target_cache.GetRenderTarget(descriptor);
      if (target != nullptr) {
        return target;
      }
    }
    auto config = MakeBackingStoreConfig(flutter_view_id,
                                         descriptor.surface_size,
                                         descriptor.request_type,
                                         descriptor.shell_visual_identifier);
    return create_render_target_callback_(context, aiks_context, config);
  });

  // This is where unused render targets will be collected. Control may flow
  // to the embedder. Here, the embedder has the opportunity to trample on the
  // OpenGL context.
  //
  // For optimum performance, we should tell the render target cache to clear
  // its unused entries before allocating new ones. This collection step
  // before allocating new render targets ameliorates peak memory usage within
  // the frame. But, this causes an issue in a known internal embedder. To
  // work around this issue while that embedder migrates, collection of render
  // targets is deferred after the presentation.
  //
  // @warning: Embedder may trample on our OpenGL context here.
  auto deferred_cleanup_render_targets =
      render_target_cache.ClearAllRenderTargetsInCache();

#if !SLIMPELLER
  // The OpenGL context could have been trampled by the embedder at this point
  // as it attempted to collect old render targets and create new ones. Tell
  // Skia to not rely on existing bindings.
  if (context) {
    context->resetContext(kAll_GrBackendState);
  }
#endif  //  !SLIMPELLER

  builder.Render();

  // Dispose thread-local cached Vulkan resources (command pools, descriptor
  // pools) that would otherwise grow unbounded. The Impeller Vulkan backend
  // caches these in thread-local storage and they must be explicitly disposed
  // each frame to recycle VkCommandBuffers and VkDescriptorPools.
  // On non-Vulkan backends this is a no-op.
  if (aiks_context) {
    aiks_context->GetContext()->DisposeThreadLocalCachedResources();
  }

#if !SLIMPELLER
  // We are going to be transferring control back over to the embedder there
  // the context may be trampled upon again. Flush all operations to the
  // underlying rendering API.
  //
  // @warning: Embedder may trample on our OpenGL context here.
  if (context) {
    context->flushAndSubmit();
  }
#endif  //  !SLIMPELLER

  {
    const auto submit_info = frame->submit_info();
    auto presentation_time_optional = submit_info.presentation_time;
    uint64_t presentation_time =
        presentation_time_optional.has_value()
            ? presentation_time_optional->ToEpochDelta().ToNanoseconds()
            : 0;

    // Submit the scribbled layer to the embedder for presentation.
    //
    // @warning: Embedder may trample on our OpenGL context here.
    auto presented_layers = std::make_shared<EmbedderLayers>(
        pending_frame_size_, pending_device_pixel_ratio_,
        pending_surface_transformation_, presentation_time,
        submit_info.frame_damage);

    builder.PushLayers(*presented_layers);

    auto retained = retained_presented_layers_.find(flutter_view_id);
    auto retained_layers =
        retained == retained_presented_layers_.end()
            ? std::shared_ptr<const EmbedderLayers>()
            : std::static_pointer_cast<const EmbedderLayers>(retained->second);
    presented_layers->InvokePresentCallback(flutter_view_id,
                                            std::move(retained_layers),
                                            &get_shell_visuals_callback_,
                                            &get_shell_sources_callback_,
                                            present_callback_);
    retained_presented_layers_[flutter_view_id] = std::move(presented_layers);
  }

  // See why this is necessary in the comment where this collection in
  // realized.
  //
  // @warning: Embedder may trample on our OpenGL context here.
  deferred_cleanup_render_targets.clear();

  auto render_targets = builder.ClearAndCollectRenderTargets();
  for (auto& [descriptor, render_target] : render_targets) {
    if (!avoid_backing_store_cache_) {
      render_target_cache.CacheRenderTarget(descriptor,
                                            std::move(render_target));
    }
  }

  frame->Submit();
}

}  // namespace flutter
