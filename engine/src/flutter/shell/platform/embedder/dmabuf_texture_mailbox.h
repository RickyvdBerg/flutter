// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_DMABUF_TEXTURE_MAILBOX_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_DMABUF_TEXTURE_MAILBOX_H_

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "flutter/display_list/geometry/dl_geometry_types.h"
#include "flutter/fml/file.h"
#include "impeller/renderer/backend/vulkan/linux/dmabuf_texture_source_vk.h"

namespace flutter {

/// @brief      A mailbox-owned DMA-BUF descriptor with RAII fd ownership.
struct OwnedDmabufDescriptor {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t drm_format = 0;
  uint64_t drm_modifier = 0;
  uint32_t num_planes = 0;
  std::array<fml::UniqueFD, 4> plane_fds;
  std::array<uint32_t, 4> offsets = {};
  std::array<uint32_t, 4> strides = {};
  fml::UniqueFD acquire_fence_fd;
};

/// @brief      A single entry in the DMA-BUF texture mailbox.
struct DmabufMailboxEntry {
  // Populated after raster-side materialization.
  std::shared_ptr<impeller::DmabufTextureSourceVK> texture_source;

  // Populated by PublishDmabufTexture; consumed to create texture_source.
  std::optional<OwnedDmabufDescriptor> pending_descriptor;

  // Fired when an entry is dropped without being consumed, or chained onto
  // texture_source so it runs when the source is finally destroyed.
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
