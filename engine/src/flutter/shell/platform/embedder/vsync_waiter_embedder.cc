// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/vsync_waiter_embedder.h"

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

// |VsyncWaiter|
void VsyncWaiterEmbedder::AwaitVSync() {
  auto* weak_waiter = new std::weak_ptr<VsyncWaiter>(shared_from_this());
  intptr_t baton = reinterpret_cast<intptr_t>(weak_waiter);
  if (vsync_for_display_callback_) {
    vsync_for_display_callback_(baton, kDefaultDisplayId);
  } else {
    vsync_callback_(baton);
  }
}

// |VsyncWaiter| Per-display override.
void VsyncWaiterEmbedder::AwaitVSync(DisplayId display_id) {
  auto* weak_waiter = new std::weak_ptr<VsyncWaiter>(shared_from_this());
  intptr_t baton = reinterpret_cast<intptr_t>(weak_waiter);
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

  // If the time here is in the future, the contract for `FlutterEngineOnVsync`
  // says that the engine will only process the frame when the time becomes
  // current.
  task_runners.GetUITaskRunner()->PostTaskForTime(
      [frame_start_time, frame_target_time, baton]() {
        std::weak_ptr<VsyncWaiter>* weak_waiter =
            reinterpret_cast<std::weak_ptr<VsyncWaiter>*>(baton);
        auto vsync_waiter = weak_waiter->lock();
        delete weak_waiter;
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

  task_runners.GetUITaskRunner()->PostTaskForTime(
      [frame_start_time, frame_target_time, baton, display_id]() {
        std::weak_ptr<VsyncWaiter>* weak_waiter =
            reinterpret_cast<std::weak_ptr<VsyncWaiter>*>(baton);
        auto vsync_waiter = weak_waiter->lock();
        delete weak_waiter;
        if (vsync_waiter) {
          vsync_waiter->FireCallback(display_id, frame_start_time,
                                     frame_target_time);
        }
      },
      frame_start_time);

  return true;
}

}  // namespace flutter
