// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/resource_manager_vk.h"

#include <atomic>

#include "flutter/fml/cpu_affinity.h"
#include "flutter/fml/thread.h"
#include "flutter/fml/trace_event.h"
#include "fml/logging.h"

namespace impeller {

namespace {

std::atomic<uint64_t> g_reclaim_enqueued_total = 0u;
std::atomic<uint64_t> g_reclaim_drained_total = 0u;
std::atomic<uint64_t> g_reclaim_batches_total = 0u;
std::atomic<uint64_t> g_reclaim_queue_high_water = 0u;

constexpr uint64_t kReclaimLogEveryBatches = 256u;

void UpdateMax(std::atomic<uint64_t>& target, uint64_t value) {
  uint64_t observed = target.load(std::memory_order_relaxed);
  while (value > observed &&
         !target.compare_exchange_weak(observed, value,
                                       std::memory_order_relaxed)) {
  }
}

}  // namespace

std::shared_ptr<ResourceManagerVK> ResourceManagerVK::Create() {
  // It will be tempting to refactor this to create the waiter thread in the
  // static method instead of the constructor. However, that causes the
  // destructor never to be called, and the thread never terminates!
  //
  // See https://github.com/flutter/flutter/issues/134482.
  return std::shared_ptr<ResourceManagerVK>(new ResourceManagerVK());
}

ResourceManagerVK::ResourceManagerVK() : waiter_([&]() { Start(); }) {}

ResourceManagerVK::~ResourceManagerVK() {
  FML_DCHECK(waiter_.get_id() != std::this_thread::get_id())
      << "The ResourceManager being destructed on its own spawned thread is a "
      << "sign that ContextVK was not properly destroyed. A usual fix for this "
      << "is to ensure that ContextVK is shutdown (i.e. context->Shutdown()) "
         "before the ResourceManager is destroyed (i.e. at the end of a test).";
  Terminate();
  waiter_.join();
}

void ResourceManagerVK::Start() {
  // It's possible for Start() to be called when terminating:
  // { ResourceManagerVK::Create(); }
  //
  // ... so no FML_DCHECK here.

  fml::Thread::SetCurrentThreadName(fml::Thread::ThreadConfig{"IplrVkResMgr"});
  // While this code calls destructors it doesn't need to be particularly fast
  // with them, as long as it doesn't interrupt raster thread.
  fml::RequestAffinity(fml::CpuAffinity::kEfficiency);

  bool should_exit = false;
  while (!should_exit) {
    std::unique_lock lock(reclaimables_mutex_);

    // Wait until there are reclaimable resource or if the manager should be
    // torn down.
    reclaimables_cv_.wait(
        lock, [&]() { return !reclaimables_.empty() || should_exit_; });

    // Don't reclaim resources when the lock is being held as this may gate
    // further reclaimables from being registered.
    Reclaimables resources_to_collect;
    std::swap(resources_to_collect, reclaimables_);

    // We can't read the ivar outside the lock. Read it here instead.
    should_exit = should_exit_;

    // We know what to collect. Unlock before doing anything else.
    lock.unlock();

    // Claim all resources while tracing.
    {
      TRACE_EVENT0("Impeller", "ReclaimResources");
      if (!resources_to_collect.empty()) {
        const auto drained_count = static_cast<uint64_t>(resources_to_collect.size());
        const auto drained_total =
            g_reclaim_drained_total.fetch_add(drained_count, std::memory_order_relaxed) +
            drained_count;
        const auto batches_total =
            g_reclaim_batches_total.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if ((batches_total % kReclaimLogEveryBatches) == 0u) {
          FML_LOG(INFO) << "Impeller Vulkan reclaim diagnostics: batches="
                        << batches_total << " drained_total=" << drained_total
                        << " enqueued_total="
                        << g_reclaim_enqueued_total.load(std::memory_order_relaxed)
                        << " queue_high_water="
                        << g_reclaim_queue_high_water.load(std::memory_order_relaxed);
        }
      }
      resources_to_collect.clear();  // Redundant because of scope but here so
                                     // we can add a trace around it.
    }
  }
}

void ResourceManagerVK::Reclaim(std::unique_ptr<ResourceVK> resource) {
  if (!resource) {
    return;
  }
  {
    std::scoped_lock lock(reclaimables_mutex_);
    reclaimables_.emplace_back(std::move(resource));
    UpdateMax(g_reclaim_queue_high_water,
              static_cast<uint64_t>(reclaimables_.size()));
  }
  g_reclaim_enqueued_total.fetch_add(1u, std::memory_order_relaxed);
  reclaimables_cv_.notify_one();
}

void ResourceManagerVK::Terminate() {
  // The thread should not be terminated more than once.
  FML_DCHECK(!should_exit_);

  {
    std::scoped_lock lock(reclaimables_mutex_);
    should_exit_ = true;
  }
  reclaimables_cv_.notify_one();
}

}  // namespace impeller
