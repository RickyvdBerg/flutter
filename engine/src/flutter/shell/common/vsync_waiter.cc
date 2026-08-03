// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/vsync_waiter.h"

#include "flow/frame_timings.h"
#include "flutter/fml/task_runner.h"
#include "flutter/fml/trace_event.h"
#include "fml/logging.h"
#include "fml/message_loop_task_queues.h"
#include "fml/task_queue_id.h"
#include "fml/time/time_point.h"

namespace flutter {

static constexpr const char* kVsyncFlowName = "VsyncFlow";

static constexpr const char* kVsyncTraceName = "VsyncProcessCallback";

VsyncWaiter::VsyncWaiter(const TaskRunners& task_runners)
    : task_runners_(task_runners) {}

VsyncWaiter::~VsyncWaiter() = default;

// Per-display variant: requests a vsync for a specific display.
void VsyncWaiter::AsyncWaitForVsync(
    DisplayId display_id,
    const Callback& callback,
    const CancellationCallback& cancellation_callback) {
  if (!callback) {
    return;
  }

  const auto display_id_str = std::to_string(display_id);
  TRACE_EVENT1("flutter", "AsyncWaitForVsync", "display_id",
               display_id_str.c_str());

  {
    std::scoped_lock lock(callback_mutex_);
    if (display_id == kDefaultDisplayId) {
      // Use the legacy callback_ member for the default display so that
      // the existing FireCallback() path (used by all current platform
      // vsync waiters) continues to work without changes.
      if (callback_) {
        TRACE_EVENT_INSTANT0("flutter", "MultipleCallsToVsyncInFrameInterval");
        return;
      }
      callback_ = callback;
      cancellation_callback_ = cancellation_callback;
      if (!secondary_callbacks_.empty()) {
        return;
      }
    } else {
      if (display_callbacks_.count(display_id) > 0) {
        TRACE_EVENT_INSTANT0("flutter", "MultipleCallsToVsyncInFrameInterval");
        return;
      }
      display_callbacks_[display_id] = callback;
      display_cancellation_callbacks_[display_id] = cancellation_callback;
    }
  }
  AwaitVSync(display_id);
}

// Legacy single-display entry point.
void VsyncWaiter::AsyncWaitForVsync(const Callback& callback) {
  AsyncWaitForVsync(kDefaultDisplayId, callback);
}

// Default per-display AwaitVSync delegates to the parameterless version.
// Subclasses that support per-display vsync should override this.
void VsyncWaiter::AwaitVSync(DisplayId display_id) {
  AwaitVSync();
}

void VsyncWaiter::ScheduleSecondaryCallback(uintptr_t id,
                                            const fml::closure& callback) {
  FML_DCHECK(task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread());

  if (!callback) {
    return;
  }

  TRACE_EVENT0("flutter", "ScheduleSecondaryCallback");

  {
    std::scoped_lock lock(callback_mutex_);
    bool secondary_callbacks_originally_empty = secondary_callbacks_.empty();
    auto [_, inserted] = secondary_callbacks_.emplace(id, callback);
    if (!inserted) {
      // Multiple schedules must result in a single callback per frame interval.
      TRACE_EVENT_INSTANT0("flutter",
                           "MultipleCallsToSecondaryVsyncInFrameInterval");
      return;
    }
    if (callback_) {
      // Return directly as `AwaitVSync` is already called by
      // `AsyncWaitForVsync`.
      return;
    }
    if (!secondary_callbacks_originally_empty) {
      // Return directly as `AwaitVSync` is already called by
      // `ScheduleSecondaryCallback`.
      return;
    }
  }
  AwaitVSyncForSecondaryCallback();
}

void VsyncWaiter::FireCallback(fml::TimePoint frame_start_time,
                               fml::TimePoint frame_target_time,
                               bool pause_secondary_tasks,
                               std::optional<uint64_t> frame_opportunity_id,
                               std::set<int64_t> frame_opportunity_target_ids) {
  FML_DCHECK(fml::TimePoint::Now() >= frame_start_time);

  Callback callback;
  std::vector<fml::closure> secondary_callbacks;

  {
    std::scoped_lock lock(callback_mutex_);
    callback = std::move(callback_);
    cancellation_callback_ = nullptr;
    for (auto& pair : secondary_callbacks_) {
      secondary_callbacks.push_back(std::move(pair.second));
    }
    secondary_callbacks_.clear();
  }

  if (!callback && secondary_callbacks.empty()) {
    // This means that the vsync waiter implementation fired a callback for a
    // request we did not make. This is a paranoid check but we still want to
    // make sure we catch misbehaving vsync implementations.
    TRACE_EVENT_INSTANT0("flutter", "MismatchedFrameCallback");
    return;
  }

  if (callback) {
    const uint64_t flow_identifier = fml::tracing::TraceNonce();
    if (pause_secondary_tasks) {
      PauseDartEventLoopTasks();
    }

    // The base trace ensures that flows have a root to begin from if one does
    // not exist. The trace viewer will ignore traces that have no base event
    // trace. While all our message loops insert a base trace trace
    // (MessageLoop::RunExpiredTasks), embedders may not.
    TRACE_EVENT0_WITH_FLOW_IDS("flutter", "VsyncFireCallback",
                               /*flow_id_count=*/1,
                               /*flow_ids=*/&flow_identifier);

    TRACE_FLOW_BEGIN("flutter", kVsyncFlowName, flow_identifier);

    fml::TaskQueueId ui_task_queue_id =
        task_runners_.GetUITaskRunner()->GetTaskQueueId();
    task_runners_.GetUITaskRunner()->PostTask(
        [ui_task_queue_id, callback, flow_identifier, frame_start_time,
         frame_target_time, pause_secondary_tasks, frame_opportunity_id,
         frame_opportunity_target_ids =
             std::move(frame_opportunity_target_ids)]() mutable {
          FML_TRACE_EVENT_WITH_FLOW_IDS(
              "flutter", kVsyncTraceName, /*flow_id_count=*/1,
              /*flow_ids=*/&flow_identifier, "StartTime", frame_start_time,
              "TargetTime", frame_target_time);
          std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder =
              std::make_unique<FrameTimingsRecorder>();
          if (frame_opportunity_id.has_value()) {
            frame_timings_recorder->SetFrameOpportunity(
                frame_opportunity_id.value(), kDefaultDisplayId,
                std::move(frame_opportunity_target_ids));
          }
          frame_timings_recorder->RecordVsync(frame_start_time,
                                              frame_target_time);
          callback(std::move(frame_timings_recorder));
          TRACE_FLOW_END("flutter", kVsyncFlowName, flow_identifier);
          if (pause_secondary_tasks) {
            ResumeDartEventLoopTasks(ui_task_queue_id);
          }
        });
  }

  for (auto& secondary_callback : secondary_callbacks) {
    task_runners_.GetUITaskRunner()->PostTask(secondary_callback);
  }
}

void VsyncWaiter::FireCallback(DisplayId display_id,
                               fml::TimePoint frame_start_time,
                               fml::TimePoint frame_target_time,
                               bool pause_secondary_tasks,
                               std::optional<uint64_t> frame_opportunity_id,
                               std::set<int64_t> frame_opportunity_target_ids) {
  if (display_id == kDefaultDisplayId) {
    // Delegate to the legacy path which handles both callback_ and
    // secondary_callbacks_.
    FireCallback(frame_start_time, frame_target_time, pause_secondary_tasks,
                 frame_opportunity_id, std::move(frame_opportunity_target_ids));
    return;
  }

  FML_DCHECK(fml::TimePoint::Now() >= frame_start_time);

  Callback callback;
  {
    std::scoped_lock lock(callback_mutex_);
    auto it = display_callbacks_.find(display_id);
    if (it == display_callbacks_.end()) {
      const auto mismatched_display_id_str = std::to_string(display_id);
      TRACE_EVENT_INSTANT1("flutter", "MismatchedDisplayFrameCallback",
                           "display_id", mismatched_display_id_str.c_str());
      return;
    }
    callback = std::move(it->second);
    display_callbacks_.erase(it);
    display_cancellation_callbacks_.erase(display_id);
  }

  if (!callback) {
    return;
  }

  const uint64_t flow_identifier = fml::tracing::TraceNonce();
  if (pause_secondary_tasks) {
    PauseDartEventLoopTasks();
  }

  const auto fire_display_id_str = std::to_string(display_id);
  TRACE_EVENT1_WITH_FLOW_IDS("flutter", "VsyncFireCallback",
                             /*flow_id_count=*/1,
                             /*flow_ids=*/&flow_identifier, "display_id",
                             fire_display_id_str.c_str());

  TRACE_FLOW_BEGIN("flutter", kVsyncFlowName, flow_identifier);

  fml::TaskQueueId ui_task_queue_id =
      task_runners_.GetUITaskRunner()->GetTaskQueueId();
  task_runners_.GetUITaskRunner()->PostTask(
      [ui_task_queue_id, callback, flow_identifier, frame_start_time,
       frame_target_time, pause_secondary_tasks, frame_opportunity_id,
       display_id,
       frame_opportunity_target_ids =
           std::move(frame_opportunity_target_ids)]() mutable {
        FML_TRACE_EVENT_WITH_FLOW_IDS(
            "flutter", kVsyncTraceName, /*flow_id_count=*/1,
            /*flow_ids=*/&flow_identifier, "StartTime", frame_start_time,
            "TargetTime", frame_target_time);
        std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder =
            std::make_unique<FrameTimingsRecorder>();
        if (frame_opportunity_id.has_value()) {
          frame_timings_recorder->SetFrameOpportunity(
              frame_opportunity_id.value(), display_id,
              std::move(frame_opportunity_target_ids));
        }
        frame_timings_recorder->RecordVsync(frame_start_time,
                                            frame_target_time);
        callback(std::move(frame_timings_recorder));
        TRACE_FLOW_END("flutter", kVsyncFlowName, flow_identifier);
        if (pause_secondary_tasks) {
          ResumeDartEventLoopTasks(ui_task_queue_id);
        }
      });
}

bool VsyncWaiter::CancelCallback(DisplayId display_id,
                                 CancellationReason reason,
                                 fml::closure completion) {
  CancellationCallback cancellation_callback;
  {
    std::scoped_lock lock(callback_mutex_);
    if (display_id == kDefaultDisplayId) {
      if (!callback_ && secondary_callbacks_.empty()) {
        return false;
      }
      callback_ = nullptr;
      cancellation_callback = std::move(cancellation_callback_);
      secondary_callbacks_.clear();
    } else {
      auto callback_it = display_callbacks_.find(display_id);
      if (callback_it == display_callbacks_.end()) {
        return false;
      }
      display_callbacks_.erase(callback_it);
      auto cancellation_it = display_cancellation_callbacks_.find(display_id);
      if (cancellation_it != display_cancellation_callbacks_.end()) {
        cancellation_callback = std::move(cancellation_it->second);
        display_cancellation_callbacks_.erase(cancellation_it);
      }
    }
  }

  task_runners_.GetUITaskRunner()->PostTask(
      [cancellation_callback = std::move(cancellation_callback), reason,
       completion = std::move(completion)]() mutable {
        if (cancellation_callback) {
          cancellation_callback(reason);
        }
        if (completion) {
          completion();
        }
      });
  return true;
}

void VsyncWaiter::PauseDartEventLoopTasks() {
  auto ui_task_queue_id = task_runners_.GetUITaskRunner()->GetTaskQueueId();
  auto task_queues = fml::MessageLoopTaskQueues::GetInstance();
  if (ui_task_queue_id.is_valid()) {
    task_queues->PauseSecondarySource(ui_task_queue_id);
  }
}

void VsyncWaiter::ResumeDartEventLoopTasks(fml::TaskQueueId ui_task_queue_id) {
  auto task_queues = fml::MessageLoopTaskQueues::GetInstance();
  if (ui_task_queue_id.is_valid()) {
    task_queues->ResumeSecondarySource(ui_task_queue_id);
  }
}

}  // namespace flutter
