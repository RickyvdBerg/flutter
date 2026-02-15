// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_DMABUF_TEXTURE_MAILBOX_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_DMABUF_TEXTURE_MAILBOX_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "flutter/display_list/geometry/dl_geometry_types.h"
#include "impeller/renderer/backend/vulkan/linux/dmabuf_texture_source_vk.h"

namespace flutter {

/// @brief      A single entry in the DMA-BUF texture mailbox.
struct DmabufMailboxEntry {
  std::shared_ptr<impeller::DmabufTextureSourceVK> texture_source;
  std::function<void()> release_callback;
  std::vector<DlIRect> damage_rects;  // empty = full frame
};

/// @brief      Thread-safe single-slot-per-texture-id mailbox for DMA-BUF
///             textures.
///
///             The embedder pushes DMA-BUF descriptors via Store(). The
///             rasterizer pulls them via Consume() during rendering. Each
///             texture_id has at most one pending entry.
///
class DmabufTextureMailbox {
 public:
  DmabufTextureMailbox();
  ~DmabufTextureMailbox();

  /// @brief      Store a new entry for the given texture_id.
  ///
  ///             If a previous entry exists and was never consumed, its
  ///             release_callback is fired inline before being replaced.
  void Store(int64_t texture_id, DmabufMailboxEntry entry);

  /// @brief      Consume the pending entry for the given texture_id.
  ///
  ///             Returns the entry and clears the slot. Returns nullptr if
  ///             no entry was pending.
  std::unique_ptr<DmabufMailboxEntry> Consume(int64_t texture_id);

  /// @brief      Remove the entry for the given texture_id and fire its
  ///             release_callback if present.
  void Remove(int64_t texture_id);

  /// @brief      Peek at the damage rects for a texture without consuming.
  ///             Returns empty vector if no entry or no damage info.
  std::vector<DlIRect> PeekDamage(int64_t texture_id);

 private:
  std::mutex mutex_;
  std::unordered_map<int64_t, DmabufMailboxEntry> entries_;

  DmabufTextureMailbox(const DmabufTextureMailbox&) = delete;
  DmabufTextureMailbox& operator=(const DmabufTextureMailbox&) = delete;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_DMABUF_TEXTURE_MAILBOX_H_
