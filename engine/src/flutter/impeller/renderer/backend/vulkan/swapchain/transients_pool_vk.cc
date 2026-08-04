// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/swapchain/transients_pool_vk.h"

#include <cstdlib>
#include <limits>
#include <utility>

#include "flutter/fml/logging.h"

namespace impeller {

size_t TransientsPoolVK::ResolveByteBudgetFromEnv(size_t default_bytes) {
  const char* override_value = std::getenv(kBudgetEnvVar);
  if (override_value == nullptr || override_value[0] == '\0') {
    return default_bytes;
  }
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(override_value, &end, 10);
  constexpr size_t kBytesPerMiB = 1024ull * 1024ull;
  if (end == override_value || *end != '\0' || parsed == 0u ||
      parsed > std::numeric_limits<size_t>::max() / kBytesPerMiB) {
    FML_LOG(WARNING) << kBudgetEnvVar
                     << " set to invalid value, using default budget: "
                     << override_value;
    return default_bytes;
  }
  return static_cast<size_t>(parsed) * kBytesPerMiB;
}

TransientsPoolVK::TransientsPoolVK(std::weak_ptr<Context> context,
                                   PixelFormat depth_stencil_format,
                                   bool supports_memoryless_textures,
                                   TransientsPoolLimitsVK limits)
    : context_(std::move(context)),
      depth_stencil_format_(depth_stencil_format),
      supports_memoryless_textures_(supports_memoryless_textures),
      max_entries_(limits.max_entries),
      max_bytes_(limits.allow_environment_override
                     ? ResolveByteBudgetFromEnv(limits.max_bytes)
                     : limits.max_bytes) {}

TransientsPoolVK::~TransientsPoolVK() {
  Reset();
}

void TransientsPoolVK::Reset() {
  std::scoped_lock lock(mutex_);
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

  // Hit: promote an idle entry to MRU and return it. Entries with additional
  // wrapper owners or cached texture refs may still be referenced by pending
  // render targets or in-flight GPU work, so they cannot be handed out again
  // for the same key.
  for (auto it = lru_.begin(); it != lru_.end(); ++it) {
    if (it->key == key && it->transients.use_count() == 1u &&
        it->transients->IsIdle()) {
      lru_.splice(lru_.begin(), lru_, it);
      return lru_.front().transients;
    }
  }

  // Miss, or all matching entries are leased: construct a fresh entry. Bind
  // the transients to the same context we hold weakly so its lifetime cannot
  // outlast the owning ContextVK.
  const auto footprint = ComputeFootprint(desc, enable_msaa);
  if (!footprint.has_value() || !ReserveFor(*footprint)) {
    return nullptr;
  }
  auto transients =
      std::make_shared<SwapchainTransientsVK>(context_, desc, enable_msaa);
  lru_.push_front(Entry{
      .key = key,
      .transients = transients,
      .byte_footprint = *footprint,
  });
  total_bytes_ += *footprint;

  return transients;
}

std::optional<size_t> TransientsPoolVK::ComputeFootprint(
    const TextureDescriptor& desc,
    bool enable_msaa) const {
  if (supports_memoryless_textures_) {
    // Lazily-allocated attachments do not occupy persistent VRAM. They
    // still count against the entry-count cap so the cache cannot grow
    // unbounded with a churning set of distinct view sizes.
    return 0u;
  }
  if (desc.size.width <= 0 || desc.size.height <= 0) {
    return std::nullopt;
  }
  const size_t width = static_cast<size_t>(desc.size.width);
  const size_t height = static_cast<size_t>(desc.size.height);
  if (width > std::numeric_limits<size_t>::max() / height) {
    return std::nullopt;
  }
  const size_t pixels = width * height;
  const size_t color_samples = enable_msaa
                                   ? static_cast<size_t>(SampleCount::kCount4)
                                   : static_cast<size_t>(SampleCount::kCount1);
  const size_t depth_samples = enable_msaa
                                   ? static_cast<size_t>(SampleCount::kCount4)
                                   : static_cast<size_t>(SampleCount::kCount1);
  // MSAA-only color allocation: if MSAA is disabled the resolve target is
  // the swapchain image itself (owned externally), so the pool holds no
  // color attachment in that case.
  const auto attachment_bytes = [pixels](
                                    size_t bytes_per_pixel,
                                    size_t samples) -> std::optional<size_t> {
    if (bytes_per_pixel == 0u || samples == 0u ||
        pixels > std::numeric_limits<size_t>::max() / bytes_per_pixel) {
      return std::nullopt;
    }
    const size_t single_sample_bytes = pixels * bytes_per_pixel;
    if (single_sample_bytes > std::numeric_limits<size_t>::max() / samples) {
      return std::nullopt;
    }
    return single_sample_bytes * samples;
  };
  const auto color_bytes =
      enable_msaa ? attachment_bytes(BytesPerPixelForPixelFormat(desc.format),
                                     color_samples)
                  : std::optional<size_t>{0u};
  const auto depth_bytes = attachment_bytes(
      BytesPerPixelForPixelFormat(depth_stencil_format_), depth_samples);
  if (!color_bytes.has_value() || !depth_bytes.has_value() ||
      *color_bytes > std::numeric_limits<size_t>::max() - *depth_bytes) {
    return std::nullopt;
  }
  return *color_bytes + *depth_bytes;
}

bool TransientsPoolVK::ReserveFor(size_t byte_footprint) {
  if (max_entries_ == 0u || byte_footprint > max_bytes_) {
    return false;
  }
  while (lru_.size() >= max_entries_ ||
         total_bytes_ > max_bytes_ - byte_footprint) {
    auto candidate = lru_.end();
    for (auto it = lru_.end(); it != lru_.begin();) {
      --it;
      if (EntryIsIdle(*it)) {
        candidate = it;
        break;
      }
    }
    if (candidate == lru_.end()) {
      return false;
    }
    total_bytes_ -= candidate->byte_footprint;
    lru_.erase(candidate);
  }
  return true;
}

bool TransientsPoolVK::EntryIsIdle(const Entry& entry) const {
  return entry.transients.use_count() == 1u && entry.transients->IsIdle();
}

ResourceCacheTrimResult TransientsPoolVK::TrimIdle() {
  std::scoped_lock lock(mutex_);
  ResourceCacheTrimResult result{.before = GetUsageLocked()};
  for (auto it = lru_.begin(); it != lru_.end();) {
    if (!EntryIsIdle(*it)) {
      ++it;
      continue;
    }
    total_bytes_ -= it->byte_footprint;
    it = lru_.erase(it);
  }
  result.after = GetUsageLocked();
  return result;
}

ResourceCacheUsage TransientsPoolVK::GetUsage() const {
  std::scoped_lock lock(mutex_);
  return GetUsageLocked();
}

ResourceCacheUsage TransientsPoolVK::GetUsageLocked() const {
  return {.entries = lru_.size(), .bytes = total_bytes_};
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
