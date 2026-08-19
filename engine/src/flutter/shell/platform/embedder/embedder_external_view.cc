// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_external_view.h"

#include <cmath>

#include "flutter/display_list/dl_builder.h"
#include "flutter/fml/trace_event.h"
#include "flutter/shell/common/dl_op_spy.h"
#include "impeller/display_list/dl_dispatcher.h"  // nogncheck
#include "third_party/skia/include/gpu/ganesh/GrDirectContext.h"
#include "third_party/skia/include/gpu/ganesh/GrRecordingContext.h"

namespace flutter {

static DlISize TransformedSurfaceSize(const DlISize& size,
                                      const DlMatrix& transformation) {
  const auto source_rect = DlRect::MakeSize(size);
  const auto transformed_rect =
      source_rect.TransformAndClipBounds(transformation);
  return DlIRect::RoundOut(transformed_rect).GetSize();
}

EmbedderExternalView::EmbedderExternalView(
    const DlISize& frame_size,
    const DlMatrix& surface_transformation)
    : EmbedderExternalView(frame_size, surface_transformation, {}, nullptr) {}

EmbedderExternalView::EmbedderExternalView(
    const DlISize& frame_size,
    const DlMatrix& surface_transformation,
    ViewIdentifier view_identifier,
    std::unique_ptr<EmbeddedViewParams> params)
    : render_surface_size_(
          TransformedSurfaceSize(frame_size, surface_transformation)),
      surface_transformation_(surface_transformation),
      view_identifier_(view_identifier),
      embedded_view_params_(std::move(params)),
      slice_(std::make_unique<DisplayListEmbedderViewSlice>(
          DlRect::MakeSize(frame_size))) {}

EmbedderExternalView::~EmbedderExternalView() = default;

EmbedderExternalView::RenderTargetDescriptor
EmbedderExternalView::CreateRenderTargetDescriptor() const {
  return RenderTargetDescriptor(render_surface_size_);
}

DlCanvas* EmbedderExternalView::GetCanvas() {
  return slice_->canvas();
}

DlISize EmbedderExternalView::GetRenderSurfaceSize() const {
  return render_surface_size_;
}

bool EmbedderExternalView::IsRootView() const {
  return !HasPlatformView();
}

bool EmbedderExternalView::HasPlatformView() const {
  return view_identifier_.platform_view_id.has_value();
}

const DlRegion& EmbedderExternalView::GetDlRegion() const {
  return slice_->getRegion();
}

bool EmbedderExternalView::HasEngineRenderedContents() {
  if (has_engine_rendered_contents_.has_value()) {
    return has_engine_rendered_contents_.value();
  }
  TryEndRecording();
  DlOpSpy dl_op_spy;
  slice_->dispatch(dl_op_spy);
  has_engine_rendered_contents_ = dl_op_spy.did_draw() && !slice_->is_empty();
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  return has_engine_rendered_contents_.value();
}

EmbedderExternalView::ViewIdentifier EmbedderExternalView::GetViewIdentifier()
    const {
  return view_identifier_;
}

const EmbeddedViewParams* EmbedderExternalView::GetEmbeddedViewParams() const {
  return embedded_view_params_.get();
}

void EmbedderExternalView::Render(DlCanvas& dl_canvas, bool clear_surface) {
  TRACE_EVENT0("flutter", "EmbedderExternalView::Render");
  TryEndRecording();
  FML_DCHECK(HasEngineRenderedContents())
      << "Unnecessarily asked to render into a render target when there was "
         "nothing to render.";

  int restore_count = dl_canvas.GetSaveCount();
  dl_canvas.SetTransform(surface_transformation_);
  if (clear_surface) {
    dl_canvas.Clear(DlColor::kTransparent());
  }
  slice_->render_into(&dl_canvas);
  dl_canvas.RestoreToCount(restore_count);
}

// TODO(https://github.com/flutter/flutter/issues/151670): Implement this for
//  Impeller as well.
#if !SLIMPELLER
static void InvalidateApiState(SkSurface& skia_surface) {
  auto recording_context = skia_surface.recordingContext();

  // Should never happen.
  FML_DCHECK(recording_context) << "Recording context was null.";

  auto direct_context = recording_context->asDirectContext();
  if (direct_context == nullptr) {
    // Can happen when using software rendering.
    // Print an error but otherwise continue in that case.
    FML_LOG(ERROR) << "Embedder asked to invalidate cached graphics API state "
                      "but Flutter is not using a graphics API.";
  } else {
    direct_context->resetContext(kAll_GrBackendState);
  }
}
#endif

EmbedderExternalView::RenderResult EmbedderExternalView::Render(
    const EmbedderRenderTarget& render_target,
    const DlRect& render_target_bounds,
    const std::optional<DlRegion>& buffer_damage,
    bool clear_surface) {
  TRACE_EVENT0("flutter", "EmbedderExternalView::Render");
  TryEndRecording();
  // A selected retained target can require a clear-only render when the current
  // display list is empty but buffer damage removes pixels from the prior
  // frame. The damage clear below is the render work in that case.

  DlMatrix render_transform =
      DlMatrix::MakeTranslation(
          {-render_target_bounds.GetX(), -render_target_bounds.GetY()}) *
      surface_transformation_;

#ifdef IMPELLER_SUPPORTS_RENDERING
  auto* impeller_target = render_target.GetImpellerRenderTarget();
  if (impeller_target) {
    auto aiks_context = render_target.GetAiksContext();

    impeller::RenderTarget target = *impeller_target;
    auto color = target.GetColorAttachment(0u);
    color.clear_color = impeller::Color::BlackTransparent();
    // A multisampled pass rasters into its own attachment and resolves that
    // attachment over every pixel of the target. It can neither carry the
    // target's preserved contents into the frame nor honor a damage
    // rectangle, whatever the caller asked for.
    const bool replaces_whole_target = color.resolve_texture != nullptr;
    const bool honors_damage =
        buffer_damage.has_value() && !replaces_whole_target;
    if (!honors_damage) {
      color.load_action = impeller::LoadAction::kClear;
    }
    target.SetColorAttachment(color, 0u);

    auto dl_builder = DisplayListBuilder();
    std::vector<SkIRect> damage_rects;
    if (buffer_damage.has_value()) {
      const DlRect target_bounds =
          DlRect::MakeSize(target.GetRenderTargetSize());
      const DlPaint clear_paint =
          DlPaint(DlColor::kTransparent()).setBlendMode(DlBlendMode::kSrc);
      for (const DlIRect& rect : buffer_damage->getRects(/*deband=*/true)) {
        const DlRect target_rect = DlRect::Make(rect)
                                       .TransformAndClipBounds(render_transform)
                                       .IntersectionOrEmpty(target_bounds);
        if (target_rect.IsEmpty()) {
          continue;
        }
        const DlIRect rounded = DlIRect::RoundOut(target_rect);
        damage_rects.push_back(
            SkIRect::MakeLTRB(rounded.GetLeft(), rounded.GetTop(),
                              rounded.GetRight(), rounded.GetBottom()));
        if (clear_surface && honors_damage) {
          dl_builder.DrawRect(DlRect::Make(rounded), clear_paint);
        }
      }
      if (damage_rects.empty()) {
        // Every requested rectangle fell outside the target. No pass runs, so
        // the target still holds exactly its previous contents; saying
        // otherwise would let the caller record this frame as its history.
        return RenderResult::kNoVisualChange;
      }
    }
    dl_builder.SetTransform(render_transform);
    slice_->render_into(&dl_builder);
    auto display_list = dl_builder.Build();

    if (honors_damage) {
      return impeller::RenderToTarget(aiks_context->GetContentContext(), target,
                                      display_list, damage_rects,
                                      /*reset_host_buffer=*/true,
                                      /*is_onscreen=*/false)
                 ? RenderResult::kRenderedRequestedDamage
                 : RenderResult::kFailed;
    }
    return impeller::RenderToTarget(
               aiks_context->GetContentContext(), target, display_list,
               impeller::Rect::MakeSize(target.GetRenderTargetSize()),
               /*reset_host_buffer=*/true,
               /*is_onscreen=*/false)
               ? RenderResult::kRenderedFullTarget
               : RenderResult::kFailed;
  }
#endif  // IMPELLER_SUPPORTS_RENDERING

#if SLIMPELLER
  FML_LOG(FATAL) << "Impeller opt-out unavailable.";
  return RenderResult::kFailed;
#else   // SLIMPELLER
  auto skia_surface = render_target.GetSkiaSurface();
  if (!skia_surface) {
    return RenderResult::kFailed;
  }

  auto [ok, invalidate_api_state] = render_target.MaybeMakeCurrent();

  if (invalidate_api_state) {
    InvalidateApiState(*skia_surface);
  }
  if (!ok) {
    FML_LOG(ERROR) << "Could not make the surface current.";
    return RenderResult::kFailed;
  }

  // Clear the current render target (most likely EGLSurface) at the
  // end of this scope.
  fml::ScopedCleanupClosure clear_current_surface([&]() {
    auto [ok, invalidate_api_state] = render_target.MaybeClearCurrent();
    if (invalidate_api_state) {
      InvalidateApiState(*skia_surface);
    }
    if (!ok) {
      FML_LOG(ERROR) << "Could not clear the current surface.";
    }
  });

  FML_DCHECK(
      render_target.GetRenderTargetSize() ==
      DlISize(static_cast<int>(std::ceil(render_target_bounds.GetWidth())),
              static_cast<int>(std::ceil(render_target_bounds.GetHeight()))));

  auto canvas = skia_surface->getCanvas();
  if (!canvas) {
    return RenderResult::kFailed;
  }
  DlSkCanvasAdapter dl_canvas(canvas);
  int restore_count = dl_canvas.GetSaveCount();
  dl_canvas.SetTransform(render_transform);
  if (clear_surface) {
    dl_canvas.Clear(DlColor::kTransparent());
  }
  slice_->render_into(&dl_canvas);
  dl_canvas.RestoreToCount(restore_count);
  dl_canvas.Flush();
#endif  //  !SLIMPELLER

  // The Skia path has no partial-repaint mode; it always paints the whole
  // surface.
  return RenderResult::kRenderedFullTarget;
}

void EmbedderExternalView::TryEndRecording() const {
  if (slice_->recording_ended()) {
    return;
  }
  slice_->end_recording();
}

}  // namespace flutter
