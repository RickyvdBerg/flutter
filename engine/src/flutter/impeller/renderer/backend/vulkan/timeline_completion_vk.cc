// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/timeline_completion_vk.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <utility>

#include "flutter/fml/cpu_affinity.h"
#include "flutter/fml/logging.h"
#include "flutter/fml/thread.h"
#include "flutter/fml/trace_event.h"
#include "impeller/base/validation.h"

namespace impeller {

TimelineCompletionVK::TimelineCompletionVK(
    std::weak_ptr<DeviceHolderVK> device_holder)
    : device_holder_(std::move(device_holder)) {
  auto holder = device_holder_.lock();
  if (!holder) {
    return;
  }

  vk::StructureChain<vk::SemaphoreCreateInfo, vk::SemaphoreTypeCreateInfo>
      semaphore_chain;
  auto& type_info = semaphore_chain.get<vk::SemaphoreTypeCreateInfo>();
  type_info.setSemaphoreType(vk::SemaphoreType::eTimeline);
  type_info.setInitialValue(0u);

  auto [result, semaphore] =
      holder->GetDevice().createSemaphoreUnique(semaphore_chain.get());
  if (result != vk::Result::eSuccess) {
    VALIDATION_LOG << "Could not create Vulkan timeline semaphore: "
                   << vk::to_string(result);
    return;
  }

  timeline_ = std::move(semaphore);
  is_valid_ = true;
  waiter_thread_ = std::make_unique<std::thread>([this]() { Main(); });
}

TimelineCompletionVK::~TimelineCompletionVK() {
  uint64_t highest_pending_value = 0u;
  {
    std::scoped_lock lock(mutex_);
    if (!pending_.empty()) {
      highest_pending_value = pending_.rbegin()->first;
    }
  }
  if (highest_pending_value != 0u) {
    WaitFor(highest_pending_value);
  }
  Terminate();
}

bool TimelineCompletionVK::IsValid() const {
  std::scoped_lock lock(mutex_);
  return is_valid_ && !!timeline_ && !terminate_;
}

uint64_t TimelineCompletionVK::ReserveSubmitValue() {
  return next_value_.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

vk::Semaphore TimelineCompletionVK::GetSemaphore() const {
  return timeline_.get();
}

bool TimelineCompletionVK::AddCompletion(uint64_t value,
                                         CompletionCallback callback) {
  if (!IsValid() || !callback || value == 0u) {
    return false;
  }

  bool ready = false;
  {
    std::scoped_lock lock(mutex_);
    if (terminate_) {
      return false;
    }
    ready = value <= completed_value_.load(std::memory_order_acquire);
    if (!ready) {
      pending_[value].emplace_back(std::move(callback));
    }
  }

  if (ready) {
    callback(CommandBuffer::Status::kCompleted);
  } else {
    cv_.notify_one();
  }
  return true;
}

bool TimelineCompletionVK::WaitFor(uint64_t value) {
  if (!IsValid()) {
    return false;
  }
  if (value <= completed_value_.load(std::memory_order_acquire)) {
    return true;
  }
  auto holder = device_holder_.lock();
  if (!holder) {
    return false;
  }
  if (!WaitForValue(holder->GetDevice(), value)) {
    return false;
  }
  const auto completed = QueryCompletedValue(holder->GetDevice());
  completed_value_.store(completed, std::memory_order_release);
  DrainReady(completed, CommandBuffer::Status::kCompleted);
  return value <= completed_value_.load(std::memory_order_acquire);
}

uint64_t TimelineCompletionVK::GetCompletedValue() const {
  return completed_value_.load(std::memory_order_acquire);
}

void TimelineCompletionVK::Terminate() {
  {
    std::scoped_lock lock(mutex_);
    terminate_ = true;
  }
  cv_.notify_all();
  if (waiter_thread_ && waiter_thread_->joinable()) {
    waiter_thread_->join();
  }
}

void TimelineCompletionVK::Main() {
  fml::Thread::SetCurrentThreadName(
      fml::Thread::ThreadConfig{"IplrVkTimeline"});
  fml::RequestAffinity(fml::CpuAffinity::kEfficiency);

  while (true) {
    uint64_t target = 0u;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&]() { return terminate_ || !pending_.empty(); });
      if (pending_.empty()) {
        if (terminate_) {
          break;
        }
        continue;
      }
      target = pending_.begin()->first;
    }

    auto holder = device_holder_.lock();
    if (!holder) {
      DrainAll(CommandBuffer::Status::kError);
      break;
    }

    if (!WaitForValue(holder->GetDevice(), target)) {
      DrainAll(CommandBuffer::Status::kError);
      break;
    }

    const auto completed = QueryCompletedValue(holder->GetDevice());
    completed_value_.store(completed, std::memory_order_release);
    DrainReady(completed, CommandBuffer::Status::kCompleted);
  }

  auto holder = device_holder_.lock();
  if (holder) {
    const auto completed = QueryCompletedValue(holder->GetDevice());
    completed_value_.store(completed, std::memory_order_release);
    DrainReady(completed, CommandBuffer::Status::kCompleted);
  }
  DrainAll(CommandBuffer::Status::kError);
}

bool TimelineCompletionVK::WaitForValue(const vk::Device& device,
                                        uint64_t value) {
  if (value <= completed_value_.load(std::memory_order_acquire)) {
    return true;
  }

  std::array<vk::Semaphore, 1> semaphores = {timeline_.get()};
  std::array<uint64_t, 1> values = {value};

  vk::SemaphoreWaitInfo wait_info;
  wait_info.setSemaphores(semaphores);
  wait_info.setValues(values);

  TRACE_EVENT0("impeller", "TimelineCompletionVK::WaitForValue");
  const auto result =
      device.waitSemaphoresKHR(wait_info, std::numeric_limits<uint64_t>::max());
  if (result != vk::Result::eSuccess) {
    VALIDATION_LOG << "Vulkan timeline semaphore wait failed: "
                   << vk::to_string(result);
    return false;
  }
  return true;
}

uint64_t TimelineCompletionVK::QueryCompletedValue(const vk::Device& device) {
  auto result = device.getSemaphoreCounterValueKHR(timeline_.get());
  if (result.result != vk::Result::eSuccess) {
    VALIDATION_LOG << "Could not query Vulkan timeline semaphore value: "
                   << vk::to_string(result.result);
    return completed_value_.load(std::memory_order_acquire);
  }
  return result.value;
}

void TimelineCompletionVK::DrainReady(uint64_t value,
                                      CommandBuffer::Status status) {
  std::vector<CompletionCallback> ready;
  {
    std::scoped_lock lock(mutex_);
    auto end = pending_.upper_bound(value);
    for (auto it = pending_.begin(); it != end; ++it) {
      std::move(it->second.begin(), it->second.end(),
                std::back_inserter(ready));
    }
    pending_.erase(pending_.begin(), end);
  }

  for (auto& callback : ready) {
    callback(status);
  }
}

void TimelineCompletionVK::DrainAll(CommandBuffer::Status status) {
  std::vector<CompletionCallback> callbacks;
  {
    std::scoped_lock lock(mutex_);
    for (auto& [value, pending] : pending_) {
      std::move(pending.begin(), pending.end(), std::back_inserter(callbacks));
    }
    pending_.clear();
  }

  for (auto& callback : callbacks) {
    callback(status);
  }
}

}  // namespace impeller
