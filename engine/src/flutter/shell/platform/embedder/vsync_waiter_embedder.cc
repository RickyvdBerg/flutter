// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/vsync_waiter_embedder.h"

#include <limits>

namespace flutter {

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

VsyncWaiterEmbedder::~VsyncWaiterEmbedder() = default;

intptr_t VsyncWaiterEmbedder::RegisterBaton(DisplayId display_id) {
  std::scoped_lock lock(baton_mutex_);
  FML_CHECK(batons_by_display_.find(display_id) == batons_by_display_.end())
      << "A display cannot own more than one embedder vsync baton";
  FML_CHECK(next_baton_ <=
            static_cast<uint64_t>(std::numeric_limits<intptr_t>::max()))
      << "Embedder vsync baton space exhausted";

  const intptr_t baton = static_cast<intptr_t>(next_baton_++);
  batons_by_display_.emplace(display_id, baton);
  displays_by_baton_.emplace(baton, display_id);
  return baton;
}

bool VsyncWaiterEmbedder::TakeBaton(DisplayId display_id, intptr_t baton) {
  if (baton <= 0) {
    return false;
  }
  std::scoped_lock lock(baton_mutex_);
  auto display_it = batons_by_display_.find(display_id);
  auto baton_it = displays_by_baton_.find(baton);
  if (display_it == batons_by_display_.end() ||
      baton_it == displays_by_baton_.end() || display_it->second != baton ||
      baton_it->second != display_id) {
    return false;
  }
  batons_by_display_.erase(display_it);
  displays_by_baton_.erase(baton_it);
  return true;
}

bool VsyncWaiterEmbedder::TakeBatonForOpportunity(
    DisplayId display_id,
    intptr_t baton,
    FrameOpportunityId opportunity_id) {
  if (baton <= 0 || opportunity_id == 0) {
    return false;
  }
  std::scoped_lock lock(baton_mutex_);
  auto display_it = batons_by_display_.find(display_id);
  auto baton_it = displays_by_baton_.find(baton);
  if (display_it == batons_by_display_.end() ||
      baton_it == displays_by_baton_.end() || display_it->second != baton ||
      baton_it->second != display_id ||
      returned_opportunities_by_display_.find(display_id) !=
          returned_opportunities_by_display_.end()) {
    return false;
  }
  batons_by_display_.erase(display_it);
  displays_by_baton_.erase(baton_it);
  returned_opportunities_by_display_.emplace(display_id, opportunity_id);
  return true;
}

bool VsyncWaiterEmbedder::TakeReturnedOpportunity(
    DisplayId display_id,
    FrameOpportunityId opportunity_id) {
  std::scoped_lock lock(baton_mutex_);
  auto opportunity = returned_opportunities_by_display_.find(display_id);
  if (opportunity == returned_opportunities_by_display_.end() ||
      opportunity->second != opportunity_id) {
    return false;
  }
  returned_opportunities_by_display_.erase(opportunity);
  return true;
}

// |VsyncWaiter|
void VsyncWaiterEmbedder::AwaitVSync() {
  intptr_t baton = RegisterBaton(kDefaultDisplayId);
  if (vsync_for_display_callback_) {
    vsync_for_display_callback_(baton, kDefaultDisplayId);
  } else {
    vsync_callback_(baton);
  }
}

// |VsyncWaiter| Per-display override.
void VsyncWaiterEmbedder::AwaitVSync(DisplayId display_id) {
  intptr_t baton = RegisterBaton(display_id);
  if (vsync_for_display_callback_) {
    vsync_for_display_callback_(baton, display_id);
  } else {
    // Fallback: the embedder does not support per-display vsync.
    // Use the legacy callback and ignore the display_id.
    vsync_callback_(baton);
  }
}

bool VsyncWaiterEmbedder::ReturnVsync(
    DisplayId display_id,
    intptr_t baton,
    fml::TimePoint frame_start_time,
    fml::TimePoint frame_target_time,
    std::optional<uint64_t> frame_opportunity_id) {
  const bool exact_opportunity = frame_opportunity_id.has_value();
  if (exact_opportunity ? !TakeBatonForOpportunity(display_id, baton,
                                                   frame_opportunity_id.value())
                        : !TakeBaton(display_id, baton)) {
    return false;
  }

  std::weak_ptr<VsyncWaiter> weak_waiter = shared_from_this();

  // If the time here is in the future, the contract for `FlutterEngineOnVsync`
  // says that the engine will only process the frame when the time becomes
  // current.
  task_runners_.GetUITaskRunner()->PostTaskForTime(
      [frame_start_time, frame_target_time,
       weak_waiter = std::move(weak_waiter), display_id,
       frame_opportunity_id]() {
        auto vsync_waiter = weak_waiter.lock();
        auto embedder_waiter =
            std::static_pointer_cast<VsyncWaiterEmbedder>(vsync_waiter);
        if (embedder_waiter &&
            (!frame_opportunity_id.has_value() ||
             embedder_waiter->TakeReturnedOpportunity(
                 display_id, frame_opportunity_id.value()))) {
          vsync_waiter->FireCallback(
              display_id, frame_start_time, frame_target_time,
              /*pause_secondary_tasks=*/true, frame_opportunity_id);
        }
      },
      frame_start_time);

  return true;
}

bool VsyncWaiterEmbedder::CancelVsync(DisplayId display_id,
                                      intptr_t baton,
                                      CancellationReason reason,
                                      fml::closure completion) {
  if (!TakeBaton(display_id, baton)) {
    return false;
  }
  return CancelCallback(display_id, reason, std::move(completion));
}

bool VsyncWaiterEmbedder::CancelFrameOpportunity(
    DisplayId display_id,
    FrameOpportunityId opportunity_id,
    CancellationReason reason,
    fml::closure completion) {
  if (!TakeReturnedOpportunity(display_id, opportunity_id)) {
    return false;
  }
  return CancelCallback(display_id, reason, std::move(completion));
}

}  // namespace flutter
