// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/swapchain/transients_pool_vk.h"

#include <cstdlib>
#include <utility>

#include "flutter/fml/hash_combine.h"
#include "flutter/fml/logging.h"

namespace impeller {

size_t TransientsPoolVK::KeyHash::operator()(const Key& key) const noexcept {
  return fml::HashCombine(static_cast<size_t>(key.width),
                          static_cast<size_t>(key.height),
                          static_cast<size_t>(key.color_format),
                          static_cast<size_t>(key.enable_msaa));
}

size_t TransientsPoolVK::ResolveByteBudgetFromEnv(size_t default_bytes) {
  const char* override_value = std::getenv(kBudgetEnvVar);
  if (override_value == nullptr || override_value[0] == '\0') {
    return default_bytes;
  }
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(override_value, &end, 10);
  if (end == override_value || parsed == 0u) {
    FML_LOG(WARNING) << kBudgetEnvVar
                     << " set to invalid value, using default budget: "
                     << override_value;
    return default_bytes;
  }
  return static_cast<size_t>(parsed) * 1024ull * 1024ull;
}

TransientsPoolVK::TransientsPoolVK(std::weak_ptr<Context> context,
                                   PixelFormat depth_stencil_format,
                                   bool supports_memoryless_textures,
                                   size_t max_entries,
                                   size_t max_bytes)
    : context_(std::move(context)),
      depth_stencil_format_(depth_stencil_format),
      supports_memoryless_textures_(supports_memoryless_textures),
      max_entries_(max_entries),
      max_bytes_(ResolveByteBudgetFromEnv(max_bytes)) {}

TransientsPoolVK::~TransientsPoolVK() {
  Reset();
}

void TransientsPoolVK::Reset() {
  std::scoped_lock lock(mutex_);
  map_.clear();
  lru_.clear();
  total_bytes_ = 0;
}

std::shared_ptr<SwapchainTransientsVK> TransientsPoolVK::Acquire(
    const TextureDescriptor& desc,
    bool enable_msaa) {
  Key key{
      .width = static_cast<int>(desc.size.width),
      .height = static_cast<int>(desc.size.height),
      .color_format = desc.format,
      .enable_msaa = enable_msaa,
  };

  std::scoped_lock lock(mutex_);

  // Hit: promote to MRU and return.
  auto found = map_.find(key);
  if (found != map_.end()) {
    lru_.splice(lru_.begin(), lru_, found->second);
    return found->second->transients;
  }

  // Miss: construct a fresh entry. Bind the transients to the same context
  // we hold weakly so its lifetime cannot outlast the owning ContextVK.
  auto transients =
      std::make_shared<SwapchainTransientsVK>(context_, desc, enable_msaa);
  if (!transients) {
    return nullptr;
  }
  const size_t footprint = ComputeFootprint(desc, enable_msaa);
  lru_.push_front(Entry{
      .key = key,
      .transients = transients,
      .byte_footprint = footprint,
  });
  map_[key] = lru_.begin();
  total_bytes_ += footprint;

  EvictExcess();

  return transients;
}

size_t TransientsPoolVK::ComputeFootprint(const TextureDescriptor& desc,
                                          bool enable_msaa) const {
  if (supports_memoryless_textures_) {
    // Lazily-allocated attachments do not occupy persistent VRAM. They
    // still count against the entry-count cap so the cache cannot grow
    // unbounded with a churning set of distinct view sizes.
    return 0;
  }
  const size_t width = static_cast<size_t>(desc.size.width);
  const size_t height = static_cast<size_t>(desc.size.height);
  if (width == 0u || height == 0u) {
    return 0;
  }
  const size_t pixels = width * height;
  const size_t color_samples =
      enable_msaa ? static_cast<size_t>(SampleCount::kCount4)
                  : static_cast<size_t>(SampleCount::kCount1);
  const size_t depth_samples =
      enable_msaa ? static_cast<size_t>(SampleCount::kCount4)
                  : static_cast<size_t>(SampleCount::kCount1);
  // MSAA-only color allocation: if MSAA is disabled the resolve target is
  // the swapchain image itself (owned externally), so the pool holds no
  // color attachment in that case.
  const size_t color_bytes =
      enable_msaa
          ? pixels * BytesPerPixelForPixelFormat(desc.format) * color_samples
          : 0u;
  const size_t depth_bytes =
      pixels * BytesPerPixelForPixelFormat(depth_stencil_format_) *
      depth_samples;
  return color_bytes + depth_bytes;
}

void TransientsPoolVK::EvictExcess() {
  // Always preserve the front entry — the just-inserted MRU. Eviction
  // walks the tail until both constraints are within budget.
  while (lru_.size() > 1u &&
         (lru_.size() > max_entries_ || total_bytes_ > max_bytes_)) {
    auto& tail = lru_.back();
    total_bytes_ = (total_bytes_ >= tail.byte_footprint)
                       ? total_bytes_ - tail.byte_footprint
                       : 0u;
    map_.erase(tail.key);
    lru_.pop_back();
  }
}

size_t TransientsPoolVK::GetEntryCountForTesting() const {
  std::scoped_lock lock(mutex_);
  return lru_.size();
}

size_t TransientsPoolVK::GetByteFootprintForTesting() const {
  std::scoped_lock lock(mutex_);
  return total_bytes_;
}

}  // namespace impeller
