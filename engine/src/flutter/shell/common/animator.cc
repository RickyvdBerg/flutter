// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/animator.h"

#include "flutter/common/constants.h"
#include "flutter/flow/frame_timings.h"
#include "flutter/fml/time/time_point.h"
#include "flutter/fml/trace_event.h"
#include "third_party/dart/runtime/include/dart_tools_api.h"

namespace flutter {

namespace {

// Wait 51 milliseconds (which is 1 more milliseconds than 3 frames at 60hz)
// before notifying the engine that we are idle.  See comments in |BeginFrame|
// for further discussion on why this is necessary.
constexpr fml::TimeDelta kNotifyIdleTaskWaitTime =
    fml::TimeDelta::FromMilliseconds(51);

}  // namespace

Animator::Animator(Delegate& delegate,
                   const TaskRunners& task_runners,
                   std::unique_ptr<VsyncWaiter> waiter)
    : delegate_(delegate),
      task_runners_(task_runners),
      waiter_(std::move(waiter)),
#if SHELL_ENABLE_METAL
      layer_tree_pipeline_(std::make_shared<FramePipeline>(2)),
#else   // SHELL_ENABLE_METAL
      // TODO(dnfield): We should remove this logic and set the pipeline depth
      // back to 2 in this case. See
      // https://github.com/flutter/engine/pull/9132 for discussion.
      layer_tree_pipeline_(std::make_shared<FramePipeline>(
          task_runners.GetPlatformTaskRunner() ==
                  task_runners.GetRasterTaskRunner()
              ? 1
              : 2)),
#endif  // SHELL_ENABLE_METAL
      pending_frame_semaphore_(1),
      weak_factory_(this) {
}

Animator::~Animator() = default;

void Animator::ScheduleImmediateFrame(uint64_t configure_serial) {
  pending_configure_serial_ = configure_serial;

  if (!pending_frame_semaphore_.TryWait()) {
    // Keep immediate scheduling aligned with RequestFrame gating so resize
    // bursts do not inflate the pending-frame semaphore.
    RequestFrame();
    return;
  }

  // Create a FrameTimingsRecorder with synthetic vsync timestamps.
  auto recorder = std::make_unique<FrameTimingsRecorder>();
  auto now = fml::TimePoint::Now();
  recorder->RecordVsync(now, now + fml::TimeDelta::FromMilliseconds(16));
  recorder->SetConfigureSerial(configure_serial);

  // Bypass AwaitVSync — build immediately.
  // BeginFrame triggers Dart build synchronously (merged UI+Platform threads).
  // EndFrame packages layer trees into pipeline and posts to raster thread.
  BeginFrame(std::move(recorder),
             /*preserve_regenerate_layer_trees=*/frame_scheduled_);
  EndFrame();
}

void Animator::EnqueueTraceFlowId(uint64_t trace_flow_id) {
  fml::TaskRunner::RunNowOrPostTask(
      task_runners_.GetUITaskRunner(),
      [self = weak_factory_.GetWeakPtr(), trace_flow_id] {
        if (!self) {
          return;
        }
        self->trace_flow_ids_.push_back(trace_flow_id);
        self->ScheduleMaybeClearTraceFlowIds();
      });
}

void Animator::BeginFrame(
    std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder,
    bool preserve_regenerate_layer_trees) {
  TRACE_EVENT_ASYNC_END0("flutter", "Frame Request Pending",
                         frame_request_number_);
  // Clear layer trees rendered out of a frame. Only Animator::Render called
  // within a frame is used.
  layer_trees_tasks_.clear();

  frame_request_number_++;

  frame_timings_recorder_ = std::move(frame_timings_recorder);
  if (frame_timings_recorder_->GetConfigureSerial() == 0 &&
      pending_configure_serial_ != 0) {
    frame_timings_recorder_->SetConfigureSerial(pending_configure_serial_);
  }
  frame_timings_recorder_->RecordBuildStart(fml::TimePoint::Now());

  size_t flow_id_count = trace_flow_ids_.size();
  std::unique_ptr<uint64_t[]> flow_ids =
      std::make_unique<uint64_t[]>(flow_id_count);
  for (size_t i = 0; i < flow_id_count; ++i) {
    flow_ids.get()[i] = trace_flow_ids_.at(i);
  }

  TRACE_EVENT_WITH_FRAME_NUMBER(frame_timings_recorder_, "flutter",
                                "Animator::BeginFrame", flow_id_count,
                                flow_ids.get());

  while (!trace_flow_ids_.empty()) {
    uint64_t trace_flow_id = trace_flow_ids_.front();
    TRACE_FLOW_END("flutter", "PointerEvent", trace_flow_id);
    trace_flow_ids_.pop_front();
  }

  frame_scheduled_ = false;
  if (!preserve_regenerate_layer_trees) {
    regenerate_layer_trees_ = false;
  }
  pending_frame_semaphore_.Signal();

  if (!producer_continuation_) {
    // We may already have a valid pipeline continuation in case a previous
    // begin frame did not result in an Animator::Render. Simply reuse that
    // instead of asking the pipeline for a fresh continuation.
    producer_continuation_ = layer_tree_pipeline_->Produce();

    if (!producer_continuation_) {
      // If we still don't have valid continuation, the pipeline is currently
      // full because the consumer is being too slow. Try again at the next
      // frame interval.
      TRACE_EVENT0("flutter", "PipelineFull");
      RequestFrame();
      return;
    }
  }

  // We have acquired a valid continuation from the pipeline and are ready
  // to service potential frame.
  FML_DCHECK(producer_continuation_);
  const fml::TimePoint frame_target_time =
      frame_timings_recorder_->GetVsyncTargetTime();
  dart_frame_deadline_ = frame_target_time.ToEpochDelta();
  uint64_t frame_number = frame_timings_recorder_->GetFrameNumber();
  delegate_.OnAnimatorBeginFrame(frame_target_time, frame_number);
}

void Animator::EndFrame() {
  if (frame_timings_recorder_ == nullptr) {
    // `EndFrame` has been called in this frame. This happens if the engine has
    // called `OnAllViewsRendered` and then the end of the vsync task calls
    // `EndFrame` again.
    return;
  }
  if (!layer_trees_tasks_.empty()) {
    const uint64_t configure_serial =
        frame_timings_recorder_->GetConfigureSerial();
    // The build is completed in OnAnimatorBeginFrame.
    frame_timings_recorder_->RecordBuildEnd(fml::TimePoint::Now());

    delegate_.OnAnimatorUpdateLatestFrameTargetTime(
        frame_timings_recorder_->GetVsyncTargetTime());

    // Commit the pending continuation.
    std::vector<std::unique_ptr<LayerTreeTask>> layer_tree_task_list;
    layer_tree_task_list.reserve(layer_trees_tasks_.size());
    for (auto& [view_id, layer_tree_task] : layer_trees_tasks_) {
      layer_tree_task_list.push_back(std::move(layer_tree_task));
    }
    layer_trees_tasks_.clear();
    PipelineProduceResult result = producer_continuation_.Complete(
        std::make_unique<FrameItem>(std::move(layer_tree_task_list),
                                    std::move(frame_timings_recorder_)));

    if (!result.success) {
      FML_DLOG(INFO) << "Failed to commit to the pipeline";
    } else {
      if (configure_serial != 0 &&
          pending_configure_serial_ == configure_serial) {
        pending_configure_serial_ = 0;
      }
      if (!result.is_first_item) {
        // Do nothing. It has been successfully pushed to the pipeline but not
        // as the first item. Eventually the 'Rasterizer' will consume it, so
        // we don't need to notify the delegate.
      } else {
        delegate_.OnAnimatorDraw(layer_tree_pipeline_);
      }
    }
  }
  frame_timings_recorder_ = nullptr;

  if (!frame_scheduled_ && has_rendered_) {
    // Wait a tad more than 3 60hz frames before reporting a big idle period.
    // This is a heuristic that is meant to avoid giving false positives to the
    // VM when we are about to schedule a frame in the next vsync, the idea
    // being that if there have been three vsyncs with no frames it's a good
    // time to start doing GC work.
    task_runners_.GetUITaskRunner()->PostDelayedTask(
        [self = weak_factory_.GetWeakPtr()]() {
          if (!self) {
            return;
          }
          // If there's a frame scheduled, bail.
          // If there's no frame scheduled, but we're not yet past the last
          // vsync deadline, bail.
          if (!self->frame_scheduled_) {
            auto now =
                fml::TimeDelta::FromMicroseconds(Dart_TimelineGetMicros());
            if (now > self->dart_frame_deadline_) {
              TRACE_EVENT0("flutter", "BeginFrame idle callback");
              self->delegate_.OnAnimatorNotifyIdle(
                  now + fml::TimeDelta::FromMilliseconds(100));
            }
          }
        },
        kNotifyIdleTaskWaitTime);
  }
  FML_DCHECK(layer_trees_tasks_.empty());
  FML_DCHECK(frame_timings_recorder_ == nullptr);
}

void Animator::Render(int64_t view_id,
                      std::unique_ptr<flutter::LayerTree> layer_tree,
                      float device_pixel_ratio) {
  has_rendered_ = true;

  if (!frame_timings_recorder_) {
    // Framework can directly call render with a built scene. A major reason is
    // to render warm up frames.
    frame_timings_recorder_ = std::make_unique<FrameTimingsRecorder>();
    const fml::TimePoint placeholder_time = fml::TimePoint::Now();
    frame_timings_recorder_->RecordVsync(placeholder_time, placeholder_time);
    frame_timings_recorder_->RecordBuildStart(placeholder_time);
  }

  TRACE_EVENT_WITH_FRAME_NUMBER(frame_timings_recorder_, "flutter",
                                "Animator::Render", /*flow_id_count=*/0,
                                /*flow_ids=*/nullptr);

  // Only inserts if the view ID has not been rendered before, ignoring
  // duplicate Render calls.
  layer_trees_tasks_.try_emplace(
      view_id, std::make_unique<LayerTreeTask>(view_id, std::move(layer_tree),
                                               device_pixel_ratio));

  // Per-display frame completion tracking. When all views for a display
  // have rendered, end that display's frame so it can be rasterized
  // independently.
  if (IsPerDisplayMode()) {
    int64_t display_id = GetDisplayForView(view_id);
    DisplayFrameState* state = GetDisplayState(display_id);
    if (state && state->frame_in_progress) {
      state->rendered_views_this_frame.insert(view_id);
      if (state->rendered_views_this_frame == state->view_ids) {
        EndFrameForDisplay(*state);
      }
    }
  }
}

const std::weak_ptr<VsyncWaiter> Animator::GetVsyncWaiter() const {
  std::weak_ptr<VsyncWaiter> weak = waiter_;
  return weak;
}

bool Animator::CanReuseLastLayerTrees() {
  return pending_configure_serial_ == 0 && !regenerate_layer_trees_;
}

void Animator::DrawLastLayerTrees(
    std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder) {
  // This method is very cheap, but this makes it explicitly clear in trace
  // files.
  TRACE_EVENT0("flutter", "Animator::DrawLastLayerTrees");

  pending_frame_semaphore_.Signal();
  // In this case BeginFrame doesn't get called, we need to
  // adjust frame timings to update build start and end times,
  // given that the frame doesn't get built in this case, we
  // will use Now() for both start and end times as an indication.
  const auto now = fml::TimePoint::Now();
  frame_timings_recorder->RecordBuildStart(now);
  frame_timings_recorder->RecordBuildEnd(now);
  delegate_.OnAnimatorDrawLastLayerTrees(std::move(frame_timings_recorder));
}

void Animator::RequestFrame(bool regenerate_layer_trees) {
  if (IsPerDisplayMode()) {
    if (active_frame_display_id_ >= 0) {
      // Called during a per-display frame (e.g. from Dart's scheduleFrame()).
      // Only re-schedule the display whose frame is currently executing to
      // prevent cross-display coupling that would lock all displays to the
      // same frame rate.
      RequestFrameForDisplay(active_frame_display_id_);
    } else {
      // Called outside a frame (e.g. initial setup, user input, view added).
      // Fan out to all displays with views.
      for (auto& [display_id, state] : display_states_) {
        if (!state.view_ids.empty()) {
          RequestFrameForDisplay(display_id);
        }
      }
    }
    return;
  }

  if (regenerate_layer_trees && !regenerate_layer_trees_) {
    // This event will be closed by BeginFrame. BeginFrame will only be called
    // if regenerating the layer trees. If a frame has been requested to update
    // an external texture, this will be false and no BeginFrame call will
    // happen.
    TRACE_EVENT_ASYNC_BEGIN0("flutter", "Frame Request Pending",
                             frame_request_number_);
    regenerate_layer_trees_ = true;
  }

  if (!pending_frame_semaphore_.TryWait()) {
    // Multiple calls to Animator::RequestFrame will still result in a
    // single request to the VsyncWaiter.
    return;
  }

  // The AwaitVSync is going to call us back at the next VSync. However, we want
  // to be reasonably certain that the UI thread is not in the middle of a
  // particularly expensive callout. We post the AwaitVSync to run right after
  // an idle. This does NOT provide a guarantee that the UI thread has not
  // started an expensive operation right after posting this message however.
  // To support that, we need edge triggered wakes on VSync.

  task_runners_.GetUITaskRunner()->PostTask(
      [self = weak_factory_.GetWeakPtr()]() {
        if (!self) {
          return;
        }
        self->AwaitVSync();
      });
  frame_scheduled_ = true;
}

void Animator::AwaitVSync() {
  waiter_->AsyncWaitForVsync(
      [self = weak_factory_.GetWeakPtr()](
          std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder) {
        if (self) {
          if (self->CanReuseLastLayerTrees()) {
            self->DrawLastLayerTrees(std::move(frame_timings_recorder));
          } else {
            self->BeginFrame(std::move(frame_timings_recorder));
            self->EndFrame();
          }
        }
      });
  if (has_rendered_) {
    delegate_.OnAnimatorNotifyIdle(dart_frame_deadline_);
  }
}

void Animator::OnAllViewsRendered() {
  if (!layer_trees_tasks_.empty()) {
    EndFrame();
  }
}

void Animator::ScheduleSecondaryVsyncCallback(uintptr_t id,
                                              const fml::closure& callback) {
  waiter_->ScheduleSecondaryCallback(id, callback);
}

// ---------------------------------------------------------------------------
// Per-Display VSync API
// ---------------------------------------------------------------------------

void Animator::AddDisplay(int64_t display_id, double refresh_rate) {
  TRACE_EVENT2_INT("flutter", "Animator::AddDisplay", "display_id", display_id,
                   "refresh_rate", refresh_rate);
  auto& state = display_states_[display_id];
  state.display_id = display_id;
  state.refresh_rate = refresh_rate;

  // Each display gets its own pipeline so frames can be produced and
  // rasterized independently without serializing through a shared pipeline.
  if (!state.pipeline) {
    state.pipeline = std::make_shared<FramePipeline>(2);
  }

  // Move any views that were assigned to this display before it was
  // registered (e.g. the implicit view whose SetViewDisplay arrives
  // before the display list is propagated from the platform thread).
  auto it = default_state_.view_ids.begin();
  while (it != default_state_.view_ids.end()) {
    auto mapping = view_to_display_.find(*it);
    if (mapping != view_to_display_.end() && mapping->second == display_id) {
      state.view_ids.insert(*it);
      it = default_state_.view_ids.erase(it);
    } else {
      ++it;
    }
  }
}

void Animator::RemoveDisplay(int64_t display_id) {
  const auto display_id_str = std::to_string(display_id);
  TRACE_EVENT1("flutter", "Animator::RemoveDisplay", "display_id",
               display_id_str.c_str());

  auto it = display_states_.find(display_id);
  if (it == display_states_.end()) {
    return;
  }

  // Move views from the removed display to the default display.
  for (int64_t view_id : it->second.view_ids) {
    view_to_display_.erase(view_id);
    default_state_.view_ids.insert(view_id);
  }
  display_states_.erase(it);
}

void Animator::RemoveStaleDisplays(const std::set<int64_t>& active_ids) {
  std::vector<int64_t> to_remove;
  for (auto& [display_id, state] : display_states_) {
    if (active_ids.find(display_id) == active_ids.end()) {
      to_remove.push_back(display_id);
    }
  }
  for (int64_t id : to_remove) {
    RemoveDisplay(id);
  }
}

void Animator::SetViewDisplay(int64_t view_id, int64_t display_id) {
  TRACE_EVENT2_INT("flutter", "Animator::SetViewDisplay", "view_id", view_id,
                   "display_id", display_id);
  // Remove from previous display if mapped.
  auto prev_it = view_to_display_.find(view_id);
  if (prev_it != view_to_display_.end()) {
    int64_t prev_display = prev_it->second;
    auto state_it = display_states_.find(prev_display);
    if (state_it != display_states_.end()) {
      state_it->second.view_ids.erase(view_id);
    }
  }
  default_state_.view_ids.erase(view_id);

  // Add to new display.
  auto state_it = display_states_.find(display_id);
  if (state_it != display_states_.end()) {
    state_it->second.view_ids.insert(view_id);
    view_to_display_[view_id] = display_id;

    // Kick off the frame loop for the new display. If the display had no
    // views before, its loop will have stopped. This ensures the moved
    // view starts rendering immediately at the new display's refresh rate.
    RequestFrameForDisplay(display_id);
  } else {
    // Display not registered yet; record the intended display so that
    // AddDisplay can move this view when the display arrives.
    view_to_display_[view_id] = display_id;
    default_state_.view_ids.insert(view_id);
  }
}

void Animator::RequestFrameForDisplay(int64_t display_id) {
  // Only proceed if the display has been explicitly registered.
  if (display_states_.find(display_id) == display_states_.end()) {
    return;
  }
  DisplayFrameState* state = GetDisplayState(display_id);
  if (!state) {
    return;
  }

  if (state->frame_scheduled || state->frame_in_progress) {
    return;
  }

  state->frame_scheduled = true;
  const auto display_id_str = std::to_string(display_id);
  TRACE_EVENT1("flutter", "Animator::RequestFrameForDisplay", "display_id",
               display_id_str.c_str());

  waiter_->AsyncWaitForVsync(
      display_id, [self = weak_factory_.GetWeakPtr(),
                   display_id](std::unique_ptr<FrameTimingsRecorder> recorder) {
        if (self) {
          self->OnDisplayVsync(display_id, std::move(recorder));
        }
      });
}

bool Animator::IsPerDisplayMode() const {
  return !display_states_.empty();
}

void Animator::OnDisplayVsync(int64_t display_id,
                              std::unique_ptr<FrameTimingsRecorder> recorder) {
  const auto display_id_str = std::to_string(display_id);
  TRACE_EVENT1("flutter", "Animator::OnDisplayVsync", "display_id",
               display_id_str.c_str());

  DisplayFrameState* state = GetDisplayState(display_id);
  if (!state) {
    return;
  }

  state->frame_scheduled = false;
  state->frame_timings_recorder = std::move(recorder);

  BeginFrameForDisplay(*state);
}

void Animator::BeginFrameForDisplay(DisplayFrameState& state) {
  const auto display_id_str = std::to_string(state.display_id);
  TRACE_EVENT1("flutter", "Animator::BeginFrameForDisplay", "display_id",
               display_id_str.c_str());

  FML_DCHECK(!state.frame_in_progress);

  // Acquire a pipeline production slot from this display's own pipeline.
  FML_DCHECK(state.pipeline);
  if (!state.producer_continuation) {
    state.producer_continuation = state.pipeline->Produce();
    if (!state.producer_continuation) {
      TRACE_EVENT0("flutter", "PipelineFull");
      // Re-request so we try again on the next vsync.
      RequestFrameForDisplay(state.display_id);
      return;
    }
  }

  state.frame_in_progress = true;
  state.rendered_views_this_frame.clear();
  has_rendered_ = true;

  if (!state.frame_timings_recorder) {
    state.frame_timings_recorder = std::make_unique<FrameTimingsRecorder>();
    const fml::TimePoint now = fml::TimePoint::Now();
    state.frame_timings_recorder->RecordVsync(now, now);
  }
  state.frame_timings_recorder->RecordBuildStart(fml::TimePoint::Now());

  const fml::TimePoint frame_target_time =
      state.frame_timings_recorder->GetVsyncTargetTime();
  uint64_t frame_number = state.frame_timings_recorder->GetFrameNumber();

  // Track the active display so that RequestFrame() calls from Dart's
  // scheduleFrame() during this frame only re-schedule this display.
  active_frame_display_id_ = state.display_id;

  // Notify delegate with display-scoped view set.
  delegate_.OnAnimatorBeginFrameForDisplay(frame_target_time, frame_number,
                                           state.display_id, state.view_ids);

  // End the frame if it wasn't already completed by Render() callbacks.
  if (state.frame_in_progress) {
    EndFrameForDisplay(state);
  }

  active_frame_display_id_ = -1;
}

void Animator::EndFrameForDisplay(DisplayFrameState& state) {
  const auto display_id_str = std::to_string(state.display_id);
  TRACE_EVENT1("flutter", "Animator::EndFrameForDisplay", "display_id",
               display_id_str.c_str());

  FML_DCHECK(state.frame_in_progress);

  // Collect layer trees for this display's views.
  std::vector<std::unique_ptr<LayerTreeTask>> display_layer_trees;
  for (int64_t view_id : state.rendered_views_this_frame) {
    auto it = layer_trees_tasks_.find(view_id);
    if (it != layer_trees_tasks_.end()) {
      display_layer_trees.push_back(std::move(it->second));
      layer_trees_tasks_.erase(it);
    }
  }

  if (!display_layer_trees.empty()) {
    state.frame_timings_recorder->RecordBuildEnd(fml::TimePoint::Now());

    delegate_.OnAnimatorUpdateLatestFrameTargetTime(
        state.frame_timings_recorder->GetVsyncTargetTime());

    PipelineProduceResult result = state.producer_continuation.Complete(
        std::make_unique<FrameItem>(std::move(display_layer_trees),
                                    std::move(state.frame_timings_recorder)));

    if (!result.success) {
      FML_DLOG(INFO) << "Failed to commit per-display frame to pipeline";
    } else {
      delegate_.OnAnimatorDraw(state.pipeline);
    }
  }

  // Reset frame state.
  state.frame_in_progress = false;
  state.rendered_views_this_frame.clear();
  state.frame_timings_recorder = nullptr;

  // Immediately request the next vsync for this display so continuous
  // animations keep rendering at the display's refresh rate.
  if (!state.view_ids.empty()) {
    RequestFrameForDisplay(state.display_id);
  }
}

Animator::DisplayFrameState* Animator::GetDisplayState(int64_t display_id) {
  if (!IsPerDisplayMode()) {
    return &default_state_;
  }

  auto it = display_states_.find(display_id);
  if (it != display_states_.end()) {
    return &it->second;
  }

  if (display_id == VsyncWaiter::kDefaultDisplayId) {
    return &default_state_;
  }

  return nullptr;
}

int64_t Animator::GetDisplayForView(int64_t view_id) const {
  auto it = view_to_display_.find(view_id);
  return it != view_to_display_.end() ? it->second
                                      : VsyncWaiter::kDefaultDisplayId;
}

void Animator::ScheduleMaybeClearTraceFlowIds() {
  waiter_->ScheduleSecondaryCallback(
      reinterpret_cast<uintptr_t>(this), [self = weak_factory_.GetWeakPtr()] {
        if (!self) {
          return;
        }
        if (!self->frame_scheduled_ && !self->trace_flow_ids_.empty()) {
          size_t flow_id_count = self->trace_flow_ids_.size();
          std::unique_ptr<uint64_t[]> flow_ids =
              std::make_unique<uint64_t[]>(flow_id_count);
          for (size_t i = 0; i < flow_id_count; ++i) {
            flow_ids.get()[i] = self->trace_flow_ids_.at(i);
          }

          TRACE_EVENT0_WITH_FLOW_IDS(
              "flutter", "Animator::ScheduleMaybeClearTraceFlowIds - callback",
              flow_id_count, flow_ids.get());

          while (!self->trace_flow_ids_.empty()) {
            auto flow_id = self->trace_flow_ids_.front();
            TRACE_FLOW_END("flutter", "PointerEvent", flow_id);
            self->trace_flow_ids_.pop_front();
          }
        }
      });
}

}  // namespace flutter
