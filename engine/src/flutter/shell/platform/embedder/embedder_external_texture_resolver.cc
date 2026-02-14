// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_external_texture_resolver.h"

#include <memory>
#include <utility>

#ifdef __linux__
#include "flutter/fml/logging.h"
#include "flutter/shell/platform/embedder/dmabuf_texture_mailbox.h"
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

  ~EmbedderExternalTextureDmabuf() override {
    // Fire the release callback for any consumed entry still held.
    if (current_entry_ && current_entry_->release_callback) {
      current_entry_->release_callback();
    }
  }

 private:
  DmabufTextureMailbox* mailbox_;
  std::unique_ptr<DmabufMailboxEntry> current_entry_;
  sk_sp<DlImage> last_image_;

  // |flutter::Texture|
  void Paint(PaintContext& context,
             const DlRect& bounds,
             bool freeze,
             const DlImageSampling sampling) override {
    if (last_image_ == nullptr) {
      last_image_ = ResolveTexture(context);
    }

    DlCanvas* canvas = context.canvas;
    const DlPaint* paint = context.paint;

    if (last_image_) {
      DlRect image_bounds = DlRect::Make(last_image_->GetBounds());
      if (bounds != image_bounds) {
        canvas->DrawImageRect(last_image_, image_bounds, bounds, sampling,
                              paint);
      } else {
        canvas->DrawImage(last_image_, bounds.GetOrigin(), sampling, paint);
      }
    }
  }

  sk_sp<DlImage> ResolveTexture(PaintContext& context) {
    auto entry = mailbox_->Consume(Id());
    if (!entry || !entry->texture_source || !entry->texture_source->IsValid()) {
      // No new frame; reuse the last image if we have one.
      return last_image_;
    }

    // Release the previous entry's callback.
    if (current_entry_ && current_entry_->release_callback) {
      current_entry_->release_callback();
    }
    current_entry_ = std::move(entry);

    // Create a TextureVK from the DmabufTextureSourceVK.
    auto impeller_context = context.aiks_context
                                ? context.aiks_context->GetContext()
                                : nullptr;
    if (!impeller_context) {
      return nullptr;
    }

    auto texture = std::make_shared<impeller::TextureVK>(
        impeller_context, current_entry_->texture_source);
    return impeller::DlImageImpeller::Make(std::move(texture));
  }

  // |flutter::Texture|
  void MarkNewFrameAvailable() override { last_image_ = nullptr; }

  // |flutter::Texture|
  void OnGrContextCreated() override {}

  // |flutter::Texture|
  void OnGrContextDestroyed() override {}

  // |flutter::Texture|
  void OnTextureUnregistered() override {
    mailbox_->Remove(Id());
    // Release any held entry.
    if (current_entry_ && current_entry_->release_callback) {
      current_entry_->release_callback();
    }
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
