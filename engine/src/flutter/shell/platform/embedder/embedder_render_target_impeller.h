// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_RENDER_TARGET_IMPELLER_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_RENDER_TARGET_IMPELLER_H_

#include "flutter/shell/platform/embedder/embedder_render_target.h"

namespace flutter {

class EmbedderRenderTargetImpeller final : public EmbedderRenderTarget {
 public:
  using RenderTargetFactory =
      std::function<std::unique_ptr<impeller::RenderTarget>()>;
  using TakeRenderCompleteSyncFDCallback = std::function<fml::UniqueFD()>;

  EmbedderRenderTargetImpeller(
      FlutterBackingStore backing_store,
      std::shared_ptr<impeller::AiksContext> aiks_context,
      std::unique_ptr<impeller::RenderTarget> impeller_target,
      fml::closure on_release,
      fml::closure framebuffer_destruction_callback,
      TakeRenderCompleteSyncFDCallback take_render_complete_sync_fd_callback =
          {});

  // Acquire backing-store identity now; construct raster resources only when
  // GetImpellerRenderTarget is called. Size/backend/damage queries stay cheap.
  // Deferred targets conservatively allow whole-target raster until realized.
  EmbedderRenderTargetImpeller(
      FlutterBackingStore backing_store,
      std::shared_ptr<impeller::AiksContext> aiks_context,
      DlISize target_size,
      RenderTargetFactory create_target,
      fml::closure on_release,
      fml::closure framebuffer_destruction_callback,
      TakeRenderCompleteSyncFDCallback take_render_complete_sync_fd_callback =
          {});

  // |EmbedderRenderTarget|
  ~EmbedderRenderTargetImpeller() override;

  // |EmbedderRenderTarget|
  sk_sp<SkSurface> GetSkiaSurface() const override;

  // |EmbedderRenderTarget|
  impeller::RenderTarget* GetImpellerRenderTarget() const override;

  // |EmbedderRenderTarget|
  std::shared_ptr<impeller::AiksContext> GetAiksContext() const override;

  // |EmbedderRenderTarget|
  DlISize GetRenderTargetSize() const override;

  // |EmbedderRenderTarget|
  fml::UniqueFD TakeRenderCompleteSyncFD() override;

  bool RasterReplacesWholeTarget() const override;

 private:
  std::shared_ptr<impeller::AiksContext> aiks_context_;
  mutable std::unique_ptr<impeller::RenderTarget> impeller_target_;
  mutable RenderTargetFactory create_target_;
  DlISize target_size_;
  fml::closure framebuffer_destruction_callback_;
  TakeRenderCompleteSyncFDCallback take_render_complete_sync_fd_callback_;

  FML_DISALLOW_COPY_AND_ASSIGN(EmbedderRenderTargetImpeller);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_RENDER_TARGET_IMPELLER_H_
