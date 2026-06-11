// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_TIMELINE_COMPLETION_VK_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_TIMELINE_COMPLETION_VK_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "impeller/renderer/backend/vulkan/device_holder_vk.h"
#include "impeller/renderer/command_buffer.h"

namespace impeller {

// Tracks GPU completion for a single Vulkan queue using a persistent timeline
// semaphore. Each queue submit signals a monotonically increasing value and
// callbacks are dispatched when the device reports that value complete.
class TimelineCompletionVK {
 public:
  using CompletionCallback = std::function<void(CommandBuffer::Status)>;

  explicit TimelineCompletionVK(std::weak_ptr<DeviceHolderVK> device_holder);

  ~TimelineCompletionVK();

  bool IsValid() const;

  uint64_t ReserveSubmitValue();

  vk::Semaphore GetSemaphore() const;

  bool AddCompletion(uint64_t value, CompletionCallback callback);

  bool WaitFor(uint64_t value);

  uint64_t GetCompletedValue() const;

  void Terminate();

 private:
  std::weak_ptr<DeviceHolderVK> device_holder_;
  vk::UniqueSemaphore timeline_;
  std::unique_ptr<std::thread> waiter_thread_;
  std::atomic_uint64_t next_value_ = 0u;
  std::atomic_uint64_t completed_value_ = 0u;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::map<uint64_t, std::vector<CompletionCallback>> pending_;
  bool terminate_ = false;
  bool is_valid_ = false;

  void Main();

  bool WaitForValue(const vk::Device& device, uint64_t value);

  uint64_t QueryCompletedValue(const vk::Device& device);

  void DrainReady(uint64_t value, CommandBuffer::Status status);

  void DrainAll(CommandBuffer::Status status);

  TimelineCompletionVK(const TimelineCompletionVK&) = delete;

  TimelineCompletionVK& operator=(const TimelineCompletionVK&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_TIMELINE_COMPLETION_VK_H_
