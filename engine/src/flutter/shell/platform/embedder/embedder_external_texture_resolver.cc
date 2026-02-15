// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_external_texture_resolver.h"

#include <memory>
#include <mutex>
#include <utility>

#ifdef __linux__
#include "flutter/fml/logging.h"
#include "flutter/shell/platform/embedder/dmabuf_texture_mailbox.h"
#include "impeller/display_list/aiks_context.h"
#include "impeller/display_list/dl_image_impeller.h"
#include "impeller/renderer/backend/vulkan/texture_vk.h"
#endif

namespace flutter {

#ifdef SHELL_ENABLE_GL
EmbedderExternalTextureResolver::EmbedderExternalTextureResolver(
    EmbedderExternalTextureGL::ExternalTextureCallback gl_callback)
    : gl_callback_(std::move(gl_callback)) {}
#endif

#ifdef SHELL_ENABLE_METAL
EmbedderExternalTextureResolver::EmbedderExternalTextureResolver(
    EmbedderExternalTextureMetal::ExternalTextureCallback metal_callback)
    : metal_callback_(std::move(metal_callback)) {}
#endif

#ifdef __linux__
// A Texture subclass that consumes DMA-BUF entries from a mailbox on each
// paint call, wrapping them as Impeller TextureVK objects.
class EmbedderExternalTextureDmabuf final : public Texture {
 public:
  EmbedderExternalTextureDmabuf(int64_t texture_id,
                                DmabufTextureMailbox* mailbox)
      : Texture(texture_id), mailbox_(mailbox) {}

  ~EmbedderExternalTextureDmabuf() override = default;

 private:
  DmabufTextureMailbox* mailbox_;
  mutable std::mutex state_mutex_;
  std::unique_ptr<DmabufMailboxEntry> current_entry_;
  sk_sp<DlImage> last_image_;

  // |flutter::Texture|
  void Paint(PaintContext& context,
             const DlRect& bounds,
             bool freeze,
             const DlImageSampling sampling) override {
    sk_sp<DlImage> image;
    {
      std::scoped_lock lock(state_mutex_);
      image = last_image_;
    }
    if (image == nullptr) {
      image = ResolveTexture(context);
    }

    DlCanvas* canvas = context.canvas;
    const DlPaint* paint = context.paint;

    if (image) {
      DlRect image_bounds = DlRect::Make(image->GetBounds());
      if (bounds != image_bounds) {
        canvas->DrawImageRect(image, image_bounds, bounds, sampling, paint);
      } else {
        canvas->DrawImage(image, bounds.GetOrigin(), sampling, paint);
      }
    }
  }

  sk_sp<DlImage> ResolveTexture(PaintContext& context) {
    auto entry = mailbox_->Consume(Id());
    if (!entry || !entry->texture_source || !entry->texture_source->IsValid()) {
      // No new frame; reuse the last image if we have one.
      std::scoped_lock lock(state_mutex_);
      return last_image_;
    }

    // Create a TextureVK from the DmabufTextureSourceVK.
    auto impeller_context =
        context.aiks_context ? context.aiks_context->GetContext() : nullptr;
    if (!impeller_context) {
      std::scoped_lock lock(state_mutex_);
      return last_image_;
    }

    if (entry->release_callback) {
      entry->texture_source->SetReleaseCallback(
          std::move(entry->release_callback));
    }

    auto texture = std::make_shared<impeller::TextureVK>(impeller_context,
                                                         entry->texture_source);
    auto image = impeller::DlImageImpeller::Make(std::move(texture));

    {
      std::scoped_lock lock(state_mutex_);
      current_entry_ = std::move(entry);
      last_image_ = image;
    }
    return image;
  }

  // |flutter::Texture|
  void MarkNewFrameAvailable() override {
    std::scoped_lock lock(state_mutex_);
    last_image_ = nullptr;
    SetNewFrameFlag();
  }

  // |flutter::Texture|
  DamageInfo GetPendingDamage() const override {
    auto rects = mailbox_->PeekDamage(Id());
    if (rects.empty()) {
      return {};  // No damage info — full repaint assumed.
    }
    return {.has_rects = true, .rects = std::move(rects)};
  }

  // |flutter::Texture|
  void OnGrContextCreated() override {}

  // |flutter::Texture|
  void OnGrContextDestroyed() override {}

  // |flutter::Texture|
  void OnTextureUnregistered() override {
    mailbox_->Remove(Id());
    std::scoped_lock lock(state_mutex_);
    current_entry_.reset();
    last_image_ = nullptr;
  }
};

void EmbedderExternalTextureResolver::SetDmabufMailbox(
    DmabufTextureMailbox* mailbox) {
  dmabuf_mailbox_ = mailbox;
}
#endif  // __linux__

std::unique_ptr<Texture>
EmbedderExternalTextureResolver::ResolveExternalTexture(int64_t texture_id) {
#ifdef SHELL_ENABLE_GL
  if (gl_callback_) {
    return std::make_unique<EmbedderExternalTextureGL>(texture_id,
                                                       gl_callback_);
  }
#endif

#ifdef SHELL_ENABLE_METAL
  if (metal_callback_) {
    return std::make_unique<EmbedderExternalTextureMetal>(texture_id,
                                                          metal_callback_);
  }
#endif

  // The DMA-BUF path is used as a fallback when no GL/Metal callback is
  // provided. This allows push-based DMA-BUF textures on Vulkan-only Linux
  // embedders while preserving GL external texture support when available.
#ifdef __linux__
  if (dmabuf_mailbox_) {
    return std::make_unique<EmbedderExternalTextureDmabuf>(texture_id,
                                                           dmabuf_mailbox_);
  }
#endif

  return nullptr;
}

bool EmbedderExternalTextureResolver::SupportsExternalTextures() {
#ifdef SHELL_ENABLE_GL
  if (gl_callback_) {
    return true;
  }
#endif

#ifdef SHELL_ENABLE_METAL
  if (metal_callback_) {
    return true;
  }
#endif

#ifdef __linux__
  if (dmabuf_mailbox_) {
    return true;
  }
#endif

  return false;
}

}  // namespace flutter
