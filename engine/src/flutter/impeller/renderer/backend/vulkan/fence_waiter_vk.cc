// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/fence_waiter_vk.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <utility>

#include "flutter/fml/cpu_affinity.h"
#include "flutter/fml/logging.h"
#include "flutter/fml/thread.h"
#include "flutter/fml/trace_event.h"
#include "impeller/base/validation.h"

namespace impeller {

namespace {

std::atomic<uint64_t> g_wait_set_added_total = 0u;
std::atomic<uint64_t> g_wait_set_signaled_total = 0u;
std::atomic<uint64_t> g_wait_set_timeout_total = 0u;
std::atomic<uint64_t> g_wait_set_error_total = 0u;
std::atomic<uint64_t> g_wait_set_high_water = 0u;

constexpr uint64_t kFenceWaiterLogEverySignals = 1024u;

void UpdateMax(std::atomic<uint64_t>& target, uint64_t value) {
  uint64_t observed = target.load(std::memory_order_relaxed);
  while (value > observed &&
         !target.compare_exchange_weak(observed, value,
                                       std::memory_order_relaxed)) {
  }
}

}  // namespace

class WaitSetEntry {
 public:
  static std::shared_ptr<WaitSetEntry> Create(vk::UniqueFence p_fence,
                                              const fml::closure& p_callback) {
    return std::shared_ptr<WaitSetEntry>(
        new WaitSetEntry(std::move(p_fence), p_callback));
  }

  void UpdateSignalledStatus(const vk::Device& device) {
    if (is_signalled_) {
      return;
    }
    is_signalled_ = device.getFenceStatus(fence_.get()) == vk::Result::eSuccess;
  }

  const vk::Fence& GetFence() const { return fence_.get(); }

  bool IsSignalled() const { return is_signalled_; }

 private:
  vk::UniqueFence fence_;
  fml::ScopedCleanupClosure callback_;
  bool is_signalled_ = false;

  WaitSetEntry(vk::UniqueFence p_fence, const fml::closure& p_callback)
      : fence_(std::move(p_fence)),
        callback_(fml::ScopedCleanupClosure{p_callback}) {}

  WaitSetEntry(const WaitSetEntry&) = delete;

  WaitSetEntry(WaitSetEntry&&) = delete;

  WaitSetEntry& operator=(const WaitSetEntry&) = delete;

  WaitSetEntry& operator=(WaitSetEntry&&) = delete;
};

FenceWaiterVK::FenceWaiterVK(std::weak_ptr<DeviceHolderVK> device_holder)
    : device_holder_(std::move(device_holder)) {
  waiter_thread_ = std::make_unique<std::thread>([&]() { Main(); });
}

FenceWaiterVK::~FenceWaiterVK() {
  Terminate();
}

bool FenceWaiterVK::AddFence(vk::UniqueFence fence,
                             const fml::closure& callback) {
  if (!fence || !callback) {
    return false;
  }
  {
    // Maintain the invariant that terminate_ is accessed only under the lock.
    std::scoped_lock lock(wait_set_mutex_);
    if (terminate_) {
      return false;
    }
    wait_set_.emplace_back(WaitSetEntry::Create(std::move(fence), callback));
    UpdateMax(g_wait_set_high_water, static_cast<uint64_t>(wait_set_.size()));
  }
  g_wait_set_added_total.fetch_add(1u, std::memory_order_relaxed);
  wait_set_cv_.notify_one();
  return true;
}

static std::vector<vk::Fence> GetFencesForWaitSet(const WaitSet& set) {
  std::vector<vk::Fence> fences;
  for (const auto& entry : set) {
    if (!entry->IsSignalled()) {
      fences.emplace_back(entry->GetFence());
    }
  }
  return fences;
}

void FenceWaiterVK::Main() {
  fml::Thread::SetCurrentThreadName(
      fml::Thread::ThreadConfig{"IplrVkFenceWait"});
  // Since this thread mostly waits on fences, it doesn't need to be fast.
  fml::RequestAffinity(fml::CpuAffinity::kEfficiency);

  while (true) {
    // We'll read the terminate_ flag within the lock below.
    bool terminate = false;

    {
      std::unique_lock lock(wait_set_mutex_);

      // If there are no fences to wait on, wait on the condition variable.
      wait_set_cv_.wait(lock,
                        [&]() { return !wait_set_.empty() || terminate_; });

      // Still under the lock, check if the waiter has been terminated.
      terminate = terminate_;
    }

    if (terminate) {
      WaitUntilEmpty();
      break;
    }

    if (!Wait()) {
      break;
    }
  }
}

void FenceWaiterVK::WaitUntilEmpty() {
  // Note, there is no lock because once terminate_ is set to true, no other
  // fence can be added to the wait set. Just in case, here's a FML_DCHECK:
  FML_DCHECK(terminate_) << "Fence waiter must be terminated.";
  while (!wait_set_.empty() && Wait()) {
    // Intentionally empty.
  }
}

bool FenceWaiterVK::Wait() {
  // Snapshot the wait set and wait on the fences.
  WaitSet wait_set;
  {
    std::scoped_lock lock(wait_set_mutex_);
    wait_set = wait_set_;
  }

  using namespace std::literals::chrono_literals;

  // Check if the context had died in the meantime.
  auto device_holder = device_holder_.lock();
  if (!device_holder) {
    return false;
  }

  const auto& device = device_holder->GetDevice();
  // Wait for one or more fences to be signaled. Any additional fences added
  // to the waiter will be serviced in the next pass. If a fence that is going
  // to be signaled at an abnormally long deadline is the only one in the set,
  // a timeout will bail out the wait.
  auto fences = GetFencesForWaitSet(wait_set);
  if (fences.empty()) {
    return true;
  }

  auto result = device.waitForFences(
      /*fenceCount=*/fences.size(),
      /*pFences=*/fences.data(),
      /*waitAll=*/false,
      /*timeout=*/std::chrono::nanoseconds{100ms}.count());
  if (result == vk::Result::eTimeout) {
    g_wait_set_timeout_total.fetch_add(1u, std::memory_order_relaxed);
  } else if (result != vk::Result::eSuccess) {
    g_wait_set_error_total.fetch_add(1u, std::memory_order_relaxed);
    VALIDATION_LOG << "Fence waiter encountered an unexpected error. Tearing "
                      "down the waiter thread.";
    return false;
  }

  // One or more fences have been signaled. Find out which ones and update
  // their signaled statuses.
  {
    TRACE_EVENT0("impeller", "CheckFenceStatus");
    for (auto& entry : wait_set) {
      entry->UpdateSignalledStatus(device);
    }
    wait_set.clear();
  }

  // Quickly acquire the wait set lock and erase signaled entries. Make sure
  // the mutex is unlocked before calling the destructors of the erased
  // entries. These might touch allocators.
  WaitSet erased_entries;
  size_t pending_after_erase = 0u;
  {
    static constexpr auto is_signalled = [](const auto& entry) {
      return entry->IsSignalled();
    };
    std::scoped_lock lock(wait_set_mutex_);

    // TODO(matanlurey): Iterate the list 1x by copying is_signaled into erased.
    std::copy_if(wait_set_.begin(), wait_set_.end(),
                 std::back_inserter(erased_entries), is_signalled);
    wait_set_.erase(
        std::remove_if(wait_set_.begin(), wait_set_.end(), is_signalled),
        wait_set_.end());
    pending_after_erase = wait_set_.size();
  }

  {
    TRACE_EVENT0("impeller", "ClearSignaledFences");
    // Erase the erased entries which will invoke callbacks.
    if (!erased_entries.empty()) {
      const auto signaled_count = static_cast<uint64_t>(erased_entries.size());
      const auto signaled_total =
          g_wait_set_signaled_total.fetch_add(signaled_count, std::memory_order_relaxed) +
          signaled_count;
      if ((signaled_total % kFenceWaiterLogEverySignals) < signaled_count) {
        FML_LOG(INFO) << "Impeller Vulkan fence waiter diagnostics: added_total="
                      << g_wait_set_added_total.load(std::memory_order_relaxed)
                      << " signaled_total=" << signaled_total
                      << " timeout_total="
                      << g_wait_set_timeout_total.load(std::memory_order_relaxed)
                      << " error_total="
                      << g_wait_set_error_total.load(std::memory_order_relaxed)
                      << " high_water="
                      << g_wait_set_high_water.load(std::memory_order_relaxed)
                      << " pending=" << pending_after_erase;
      }
    }
    erased_entries.clear();  // Bit redundant because of scope but hey.
  }

  return true;
}

void FenceWaiterVK::Terminate() {
  {
    std::scoped_lock lock(wait_set_mutex_);
    if (terminate_) {
      return;
    }
    terminate_ = true;
  }
  wait_set_cv_.notify_one();
  waiter_thread_->join();
}

}  // namespace impeller
