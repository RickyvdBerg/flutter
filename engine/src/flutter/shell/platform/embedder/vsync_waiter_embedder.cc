// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/vsync_waiter_embedder.h"

#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace flutter {

namespace {

using DisplayId = VsyncWaiter::DisplayId;

constexpr size_t kMaxPendingBatons = 512;

struct BatonKey {
  uintptr_t waiter_owner;
  DisplayId display_id;

  bool operator==(const BatonKey& other) const {
    return waiter_owner == other.waiter_owner && display_id == other.display_id;
  }
};

struct BatonKeyHasher {
  size_t operator()(const BatonKey& key) const {
    const size_t owner_hash = std::hash<uintptr_t>{}(key.waiter_owner);
    const size_t display_hash = std::hash<DisplayId>{}(key.display_id);
    return owner_hash ^ (display_hash + 0x9e3779b9 + (owner_hash << 6) +
                         (owner_hash >> 2));
  }
};

struct PendingBaton {
  std::weak_ptr<VsyncWaiter> waiter;
  BatonKey key;
};

class PendingBatonRegistry {
 public:
  static PendingBatonRegistry& GetInstance() {
    static PendingBatonRegistry instance;
    return instance;
  }

  intptr_t Register(const std::weak_ptr<VsyncWaiter>& waiter,
                    uintptr_t owner,
                    DisplayId display_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    const BatonKey key{owner, display_id};

    auto latest_it = latest_by_key_.find(key);
    if (latest_it != latest_by_key_.end()) {
      pending_by_token_.erase(latest_it->second);
      latest_by_key_.erase(latest_it);
    }

    intptr_t token = next_token_++;
    if (token == 0) {
      token = next_token_++;
    }

    pending_by_token_[token] = PendingBaton{waiter, key};
    latest_by_key_[key] = token;
    insertion_order_.push_back(token);

    TrimLocked();
    return token;
  }

  std::optional<PendingBaton> Take(intptr_t token) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto pending_it = pending_by_token_.find(token);
    if (pending_it == pending_by_token_.end()) {
      return std::nullopt;
    }

    PendingBaton pending = std::move(pending_it->second);
    pending_by_token_.erase(pending_it);

    auto latest_it = latest_by_key_.find(pending.key);
    if (latest_it != latest_by_key_.end() && latest_it->second == token) {
      latest_by_key_.erase(latest_it);
    }

    return pending;
  }

  void RemoveOwner(uintptr_t owner) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto it = latest_by_key_.begin(); it != latest_by_key_.end();) {
      if (it->first.waiter_owner != owner) {
        ++it;
        continue;
      }
      pending_by_token_.erase(it->second);
      it = latest_by_key_.erase(it);
    }

    CompactOrderLocked();
  }

 private:
  void TrimLocked() {
    while (pending_by_token_.size() > kMaxPendingBatons &&
           !insertion_order_.empty()) {
      const intptr_t oldest_token = insertion_order_.front();
      insertion_order_.pop_front();

      auto pending_it = pending_by_token_.find(oldest_token);
      if (pending_it == pending_by_token_.end()) {
        continue;
      }

      auto latest_it = latest_by_key_.find(pending_it->second.key);
      if (latest_it != latest_by_key_.end() &&
          latest_it->second == oldest_token) {
        latest_by_key_.erase(latest_it);
      }
      pending_by_token_.erase(pending_it);
    }

    CompactOrderLocked();
  }

  void CompactOrderLocked() {
    if (insertion_order_.size() <= (kMaxPendingBatons * 4)) {
      return;
    }

    std::deque<intptr_t> compacted;
    for (intptr_t token : insertion_order_) {
      if (pending_by_token_.find(token) != pending_by_token_.end()) {
        compacted.push_back(token);
      }
    }
    insertion_order_.swap(compacted);
  }

  std::mutex mutex_;
  intptr_t next_token_ = 1;
  std::unordered_map<intptr_t, PendingBaton> pending_by_token_;
  std::unordered_map<BatonKey, intptr_t, BatonKeyHasher> latest_by_key_;
  std::deque<intptr_t> insertion_order_;
};

}  // namespace

VsyncWaiterEmbedder::VsyncWaiterEmbedder(
    const VsyncCallback& vsync_callback,
    const VsyncForDisplayCallback& vsync_for_display_callback,
    const flutter::TaskRunners& task_runners)
    : VsyncWaiter(task_runners),
      vsync_callback_(vsync_callback),
      vsync_for_display_callback_(vsync_for_display_callback) {
  // At least one callback must be provided.
  FML_DCHECK(vsync_callback_ || vsync_for_display_callback_);
}

VsyncWaiterEmbedder::VsyncWaiterEmbedder(
    const VsyncCallback& vsync_callback,
    const flutter::TaskRunners& task_runners)
    : VsyncWaiter(task_runners),
      vsync_callback_(vsync_callback),
      vsync_for_display_callback_(nullptr) {
  FML_DCHECK(vsync_callback_);
}

VsyncWaiterEmbedder::~VsyncWaiterEmbedder() {
  PendingBatonRegistry::GetInstance().RemoveOwner(
      reinterpret_cast<uintptr_t>(this));
}

// |VsyncWaiter|
void VsyncWaiterEmbedder::AwaitVSync() {
  intptr_t baton = PendingBatonRegistry::GetInstance().Register(
      shared_from_this(), reinterpret_cast<uintptr_t>(this), kDefaultDisplayId);
  if (vsync_for_display_callback_) {
    vsync_for_display_callback_(baton, kDefaultDisplayId);
  } else {
    vsync_callback_(baton);
  }
}

// |VsyncWaiter| Per-display override.
void VsyncWaiterEmbedder::AwaitVSync(DisplayId display_id) {
  intptr_t baton = PendingBatonRegistry::GetInstance().Register(
      shared_from_this(), reinterpret_cast<uintptr_t>(this), display_id);
  if (vsync_for_display_callback_) {
    vsync_for_display_callback_(baton, display_id);
  } else {
    // Fallback: the embedder does not support per-display vsync.
    // Use the legacy callback and ignore the display_id.
    vsync_callback_(baton);
  }
}

// static
bool VsyncWaiterEmbedder::OnEmbedderVsync(
    const flutter::TaskRunners& task_runners,
    intptr_t baton,
    fml::TimePoint frame_start_time,
    fml::TimePoint frame_target_time) {
  if (baton == 0) {
    return false;
  }

  std::optional<PendingBaton> pending =
      PendingBatonRegistry::GetInstance().Take(baton);
  if (!pending.has_value()) {
    return false;
  }

  std::weak_ptr<VsyncWaiter> weak_waiter = pending->waiter;

  // If the time here is in the future, the contract for `FlutterEngineOnVsync`
  // says that the engine will only process the frame when the time becomes
  // current.
  task_runners.GetUITaskRunner()->PostTaskForTime(
      [frame_start_time, frame_target_time,
       weak_waiter = std::move(weak_waiter)]() {
        auto vsync_waiter = weak_waiter.lock();
        if (vsync_waiter) {
          vsync_waiter->FireCallback(frame_start_time, frame_target_time);
        }
      },
      frame_start_time);

  return true;
}

// static
bool VsyncWaiterEmbedder::OnEmbedderVsyncForDisplay(
    const flutter::TaskRunners& task_runners,
    intptr_t baton,
    DisplayId display_id,
    fml::TimePoint frame_start_time,
    fml::TimePoint frame_target_time) {
  if (baton == 0) {
    return false;
  }

  std::optional<PendingBaton> pending =
      PendingBatonRegistry::GetInstance().Take(baton);
  if (!pending.has_value()) {
    return false;
  }

  std::weak_ptr<VsyncWaiter> weak_waiter = pending->waiter;

  task_runners.GetUITaskRunner()->PostTaskForTime(
      [frame_start_time, frame_target_time, display_id,
       weak_waiter = std::move(weak_waiter)]() {
        auto vsync_waiter = weak_waiter.lock();
        if (vsync_waiter) {
          vsync_waiter->FireCallback(display_id, frame_start_time,
                                     frame_target_time);
        }
      },
      frame_start_time);

  return true;
}

}  // namespace flutter
