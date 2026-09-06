// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_render_target_impeller.h"

#include "flutter/fml/logging.h"
#include "flutter/impeller/display_list/aiks_context.h"
#include "flutter/impeller/renderer/render_target.h"
#include "impeller/renderer/context.h"

namespace flutter {

EmbedderRenderTargetImpeller::EmbedderRenderTargetImpeller(
    FlutterBackingStore backing_store,
    std::shared_ptr<impeller::AiksContext> aiks_context,
    std::unique_ptr<impeller::RenderTarget> impeller_target,
    fml::closure on_release,
    fml::closure framebuffer_destruction_callback,
    TakeRenderCompleteSyncFDCallback take_render_complete_sync_fd_callback)
    : EmbedderRenderTarget(backing_store, std::move(on_release)),
      aiks_context_(std::move(aiks_context)),
      impeller_target_(std::move(impeller_target)),
      target_size_(impeller_target_
                       ? DlISize(impeller_target_->GetRenderTargetSize())
                       : DlISize()),
      framebuffer_destruction_callback_(
          std::move(framebuffer_destruction_callback)),
      take_render_complete_sync_fd_callback_(
          std::move(take_render_complete_sync_fd_callback)) {
  FML_DCHECK(aiks_context_);
  FML_DCHECK(impeller_target_);
}

EmbedderRenderTargetImpeller::EmbedderRenderTargetImpeller(
    FlutterBackingStore backing_store,
    std::shared_ptr<impeller::AiksContext> aiks_context,
    DlISize target_size,
    RenderTargetFactory create_target,
    fml::closure on_release,
    fml::closure framebuffer_destruction_callback,
    TakeRenderCompleteSyncFDCallback take_render_complete_sync_fd_callback)
    : EmbedderRenderTarget(backing_store, std::move(on_release)),
      aiks_context_(std::move(aiks_context)),
      create_target_(std::move(create_target)),
      target_size_(target_size),
      framebuffer_destruction_callback_(
          std::move(framebuffer_destruction_callback)),
      take_render_complete_sync_fd_callback_(
          std::move(take_render_complete_sync_fd_callback)) {
  FML_DCHECK(aiks_context_);
  FML_DCHECK(create_target_);
}

EmbedderRenderTargetImpeller::~EmbedderRenderTargetImpeller() {
  create_target_ = {};
  impeller_target_.reset();
  if (framebuffer_destruction_callback_) {
    framebuffer_destruction_callback_();
  }
}

sk_sp<SkSurface> EmbedderRenderTargetImpeller::GetSkiaSurface() const {
  return nullptr;
}

impeller::RenderTarget* EmbedderRenderTargetImpeller::GetImpellerRenderTarget()
    const {
  if (create_target_) {
    // Consume once, including failure. The target belongs to one raster-thread
    // lease; retrying it could produce a second outcome for the same frame.
    auto create_target = std::move(create_target_);
    create_target_ = {};
    impeller_target_ = create_target();
  }
  return impeller_target_.get();
}

std::shared_ptr<impeller::AiksContext>
EmbedderRenderTargetImpeller::GetAiksContext() const {
  return aiks_context_;
}

DlISize EmbedderRenderTargetImpeller::GetRenderTargetSize() const {
  return target_size_;
}

bool EmbedderRenderTargetImpeller::RasterReplacesWholeTarget() const {
  return !impeller_target_ ||
         impeller_target_->GetColorAttachment(0u).resolve_texture != nullptr;
}

fml::UniqueFD EmbedderRenderTargetImpeller::TakeRenderCompleteSyncFD() {
  if (!take_render_complete_sync_fd_callback_) {
    return {};
  }
  auto sync_fd = take_render_complete_sync_fd_callback_();
  if (!sync_fd.is_valid()) {
    aiks_context_->GetContext()->GetIdleWaiter()->WaitIdle();
  }
  return sync_fd;
}

}  // namespace flutter
