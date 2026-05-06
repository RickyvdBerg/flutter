// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_SWAPCHAIN_TRANSIENTS_POOL_VK_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_SWAPCHAIN_TRANSIENTS_POOL_VK_H_

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "impeller/base/thread.h"
#include "impeller/core/formats.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/renderer/backend/vulkan/swapchain/swapchain_transients_vk.h"
#include "impeller/renderer/context.h"

namespace impeller {

//------------------------------------------------------------------------------
/// @brief      Context-scoped pool of `SwapchainTransientsVK`, keyed by
///             `(width, height, format, sample-count enabled)`.
///
///             Embedder backing-store flows that disable Flutter's render
///             target cache (e.g. when the host compositor owns the
///             presentable buffer ring) need the MSAA + depth/stencil
///             attachments to outlive a single `EmbedderRenderTarget` so
///             that they're not re-allocated on every frame. The
///             `SwapchainTransientsVK` instances internally cache those
///             textures, but they only persist while *something* keeps a
///             strong reference. This pool is that strong reference, owned
///             by `ContextVK` so its lifetime is bound to the graphics
///             context.
///
///             The pool is bounded by both an entry count and a byte budget:
///
///             - Entry cap protects against unbounded growth when many
///               distinct view sizes flicker through the cache (e.g.,
///               window resize storms).
///             - Byte budget protects GPU memory on desktop drivers that
///               cannot use lazily-allocated / memoryless attachments.
///               Memoryless allocations contribute zero to the budget,
///               since they consume no VRAM.
///
///             Entries are evicted in LRU order on insert until both
///             constraints are satisfied. The most-recently-acquired entry
///             is never evicted in the same call that produced it.
///
class TransientsPoolVK {
 public:
  /// Default cap on cached entries. Sized for a typical multi-display
  /// desktop session: 3 displays + a few per-window-chrome variants.
  static constexpr size_t kDefaultMaxEntries = 6;

  /// Default cap on resident GPU memory held by cached attachments. Tuned
  /// for desktop GPUs without lazily-allocated attachment support; on
  /// tilers, memoryless attachments contribute zero and the cap is
  /// effectively unused.
  static constexpr size_t kDefaultMaxBytes = 256ull * 1024ull * 1024ull;

  /// Environment variable that overrides the byte budget at construction.
  /// Value is parsed as MiB.
  static constexpr const char* kBudgetEnvVar =
      "IMPELLER_VK_TRANSIENTS_BUDGET_MIB";

  TransientsPoolVK(std::weak_ptr<Context> context,
                   PixelFormat depth_stencil_format,
                   bool supports_memoryless_textures,
                   size_t max_entries = kDefaultMaxEntries,
                   size_t max_bytes = kDefaultMaxBytes);

  ~TransientsPoolVK();

  TransientsPoolVK(const TransientsPoolVK&) = delete;
  TransientsPoolVK& operator=(const TransientsPoolVK&) = delete;

  /// @brief  Return the cached `SwapchainTransientsVK` for the given
  ///         color descriptor, constructing one on miss. The caller may
  ///         hold the returned shared_ptr beyond a single render; the
  ///         pool retains its own strong reference until eviction.
  std::shared_ptr<SwapchainTransientsVK> Acquire(const TextureDescriptor& desc,
                                                 bool enable_msaa);

  /// @brief  Drop all cached entries. Must be called before the owning
  ///         `ResourceManagerVK` and `TimelineCompletionVK` are destroyed so
  ///         that the textures' destructors complete cleanly.
  void Reset();

  /// @brief  Snapshot of cached entry count for diagnostics.
  size_t GetEntryCountForTesting() const;

  /// @brief  Snapshot of total accounted bytes for diagnostics.
  size_t GetByteFootprintForTesting() const;

 private:
  struct Key {
    int width = 0;
    int height = 0;
    PixelFormat color_format = PixelFormat::kUnknown;
    bool enable_msaa = false;

    bool operator==(const Key& other) const {
      return width == other.width && height == other.height &&
             color_format == other.color_format &&
             enable_msaa == other.enable_msaa;
    }
  };

  struct KeyHash {
    size_t operator()(const Key& key) const noexcept;
  };

  struct Entry {
    Key key;
    std::shared_ptr<SwapchainTransientsVK> transients;
    size_t byte_footprint = 0;
  };

  // Compute the worst-case device memory footprint of an entry. Returns 0
  // when memoryless attachments are supported, since those consume no
  // dedicated VRAM.
  size_t ComputeFootprint(const TextureDescriptor& desc,
                          bool enable_msaa) const;

  // Drop entries from the LRU tail while either limit is exceeded. Never
  // evicts the front entry; callers should insert at front before calling.
  void EvictExcess() IPLR_REQUIRES(mutex_);

  static size_t ResolveByteBudgetFromEnv(size_t default_bytes);

  std::weak_ptr<Context> context_;
  const PixelFormat depth_stencil_format_;
  const bool supports_memoryless_textures_;
  const size_t max_entries_;
  const size_t max_bytes_;

  mutable std::mutex mutex_;
  // LRU order: front = most recently accessed, back = candidate for eviction.
  std::list<Entry> lru_ IPLR_GUARDED_BY(mutex_);
  std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> map_
      IPLR_GUARDED_BY(mutex_);
  size_t total_bytes_ IPLR_GUARDED_BY(mutex_) = 0;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_SWAPCHAIN_TRANSIENTS_POOL_VK_H_
