// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/animator.h"

#include <algorithm>
#include <iterator>

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

bool LatchDisplayFrameRequest(Animator::DisplayFrameState& state,
                              const std::set<int64_t>* view_ids,
                              bool regenerate_layer_trees) {
  if (view_ids == nullptr) {
    if (state.renderable_view_ids.empty()) {
      return false;
    }
    state.pending_frame_all_views = true;
    state.pending_frame_view_ids.clear();
    state.pending_regenerate_layer_trees =
        state.pending_regenerate_layer_trees || regenerate_layer_trees;
    return true;
  }

  bool request_retained = false;
  for (int64_t view_id : *view_ids) {
    if (state.renderable_view_ids.find(view_id) !=
        state.renderable_view_ids.end()) {
      request_retained = true;
      if (!state.pending_frame_all_views) {
        state.pending_frame_view_ids.insert(view_id);
      }
    }
  }

  if (request_retained) {
    state.pending_regenerate_layer_trees =
        state.pending_regenerate_layer_trees || regenerate_layer_trees;
  }
  return request_retained;
}

void ResolveDisplayFrameViewIds(Animator::DisplayFrameState& state) {
  state.current_frame_view_ids.clear();
  if (state.pending_frame_all_views) {
    state.current_frame_view_ids = state.renderable_view_ids;
  } else {
    for (int64_t view_id : state.pending_frame_view_ids) {
      if (state.renderable_view_ids.find(view_id) !=
          state.renderable_view_ids.end()) {
        state.current_frame_view_ids.insert(view_id);
      }
    }
  }
  state.pending_frame_all_views = false;
  state.pending_frame_view_ids.clear();
}

struct ReconciledFrameTargets {
  std::set<int64_t> render;
  std::set<int64_t> no_visual_change;
  std::set<int64_t> removed;
};

ReconciledFrameTargets ReconcileFrameTargets(
    const std::set<int64_t>& admitted,
    const std::set<int64_t>& requested,
    const std::set<int64_t>& active,
    const std::set<int64_t>& removed_since_request) {
  ReconciledFrameTargets result;
  for (int64_t target_id : admitted) {
    const bool was_removed =
        removed_since_request.find(target_id) != removed_since_request.end();
    const bool is_active = active.find(target_id) != active.end();
    if (was_removed || !is_active) {
      result.removed.insert(target_id);
    } else if (requested.find(target_id) != requested.end()) {
      result.render.insert(target_id);
    } else {
      result.no_visual_change.insert(target_id);
    }
  }
  return result;
}

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
      per_display_opt_in_(waiter_->SupportsPerDisplayVsync()),
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

  EndTraceFlowIds();

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
  dart_frame_deadline_ =
      frame_timings_recorder_->GetRenderDeadlineTime().ToEpochDelta();
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

  // Always keep the latest layer tree for each view.
  //
  // In per-display mode, the framework may still route through legacy global
  // draw callbacks, which can emit multiple Render calls for the same view
  // before that display's frame drains. Replacing here prevents stale layer
  // trees from being replayed and keeps scanout deterministic.
  layer_trees_tasks_[view_id] = std::make_unique<LayerTreeTask>(
      view_id, std::move(layer_tree), device_pixel_ratio);

  // Per-display frame completion tracking. When all views for a display
  // have rendered, end that display's frame so it can be rasterized
  // independently.
  if (IsPerDisplayMode()) {
    int64_t display_id = GetDisplayForView(view_id);
    DisplayFrameState* state = GetDisplayState(display_id);
    if (state) {
      if (state->frame_in_progress) {
        if (state->current_frame_view_ids.find(view_id) !=
            state->current_frame_view_ids.end()) {
          state->rendered_views_this_frame.insert(view_id);
          if (state->rendered_views_this_frame ==
              state->current_frame_view_ids) {
            EndFrameForDisplay(*state);
          }
        }
      } else if (!state->frame_scheduled) {
        // A view can render while another display owns the active frame
        // callback (for example, legacy global draw fallback). Ensure that the
        // target display still receives a vsync callback to consume the cached
        // layer tree.
        RequestFrameForDisplay(display_id, false);
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
  DrawLastLayerTreesForDisplay(std::move(frame_timings_recorder));
}

void Animator::DrawLastLayerTreesForDisplay(
    std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder) {
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
    // PlatformDispatcher.scheduleFrame() carries no view identity. In
    // per-display mode the framework uses RequestFrameForDisplayViews() when
    // it can prove the dirty view set; the unscoped path must preserve global
    // semantics instead of inheriting the view subset currently being drawn.
    // Otherwise a ticker/Riverpod callback that cannot be attributed before
    // scheduling can get pinned to a titlebar/chrome subset and starve dock,
    // spotlight, menu, or overlay views until some unrelated event widens the
    // request.
    bool display_owns_a_view = false;
    for (auto& [display_id, state] : display_states_) {
      if (!state.view_ids.empty()) {
        display_owns_a_view = true;
        RequestFrameForDisplay(display_id, regenerate_layer_trees);
      }
    }

    // Views the display states cannot speak for still need a frame clock, so
    // the request falls through to the global path rather than being dropped.
    // Two cases reach here: views parked in `default_state_`, which keeps
    // serving them in per-display mode (mirroring GetDisplayState and
    // HasRenderableViews), and an embedder that registered displays while
    // every view remains unhomed. Note the condition is view ownership, not
    // whether a display accepted the request: a display whose views are all
    // non-renderable declined on purpose, and a global frame must not
    // resurrect that suppressed demand.
    if (display_owns_a_view && default_state_.view_ids.empty()) {
      return;
    }
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

  // Displays schedule frames independently, but rasterization still drains on
  // one thread and the downstream pipeline pressure logic assumes a shared
  // queue. Reuse the engine-wide pipeline for every display instead of
  // creating per-display queues that can diverge and strand frame pressure.
  FML_DCHECK(layer_tree_pipeline_);
  if (state.pipeline.get() != layer_tree_pipeline_.get()) {
    state.pipeline = layer_tree_pipeline_;
  }

  // Move any views that were assigned to this display before it was
  // registered (e.g. the implicit view whose SetViewDisplay arrives
  // before the display list is propagated from the platform thread).
  auto it = default_state_.view_ids.begin();
  while (it != default_state_.view_ids.end()) {
    auto mapping = view_to_display_.find(*it);
    if (mapping != view_to_display_.end() && mapping->second == display_id) {
      state.view_ids.insert(*it);
      if (default_state_.renderable_view_ids.erase(*it) > 0) {
        state.renderable_view_ids.insert(*it);
      }
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

  DisplayFrameState& state = it->second;
  FML_DCHECK(!state.frame_in_progress)
      << "Display removal cannot interrupt a synchronous UI frame";

  // Preserve only the targets named by the pending frame request until its
  // baton is either returned or exactly cancelled. Naming every view on the
  // display here would invent outcomes for targets the host never admitted.
  state.retired = true;
  if (state.pending_frame_all_views) {
    state.removed_target_ids.insert(state.view_ids.begin(),
                                    state.view_ids.end());
  } else {
    state.removed_target_ids.insert(state.pending_frame_view_ids.begin(),
                                    state.pending_frame_view_ids.end());
  }

  // Move views from the removed display to the default display.
  for (int64_t view_id : state.view_ids) {
    view_to_display_.erase(view_id);
    default_state_.view_ids.insert(view_id);
    if (state.renderable_view_ids.count(view_id) > 0) {
      default_state_.renderable_view_ids.insert(view_id);
    }
  }
  state.view_ids.clear();
  state.renderable_view_ids.clear();
  state.pending_frame_view_ids.clear();
  state.current_frame_view_ids.clear();

  if (!state.frame_scheduled) {
    display_states_.erase(it);
  }
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
  // Homing a view on a display is the embedder asserting that it drives that
  // display's frames, so it opts into display-scoped scheduling even when the
  // waiter has no per-display callback.
  per_display_opt_in_ = true;
  if (BindViewToDisplay(view_id, display_id)) {
    RequestFrameForDisplay(display_id);
  }
}

bool Animator::RegisterInitialViewDisplay(int64_t view_id, int64_t display_id) {
  TRACE_EVENT2_INT("flutter", "Animator::RegisterInitialViewDisplay", "view_id",
                   view_id, "display_id", display_id);
  per_display_opt_in_ = true;
  if (view_to_display_.find(view_id) != view_to_display_.end() ||
      default_state_.view_ids.count(view_id) > 0) {
    return false;
  }
  BindViewToDisplay(view_id, display_id);
  return true;
}

bool Animator::BindViewToDisplay(int64_t view_id, int64_t display_id) {
  // Preserve render relevance while moving the view between displays. A new
  // view is visible by default until the embedder supplies an explicit state.
  bool is_renderable = true;
  bool was_registered = default_state_.view_ids.count(view_id) > 0;
  if (was_registered) {
    is_renderable = default_state_.renderable_view_ids.count(view_id) > 0;
  }

  // Remove from previous display if mapped.
  auto prev_it = view_to_display_.find(view_id);
  if (prev_it != view_to_display_.end()) {
    int64_t prev_display = prev_it->second;
    auto state_it = display_states_.find(prev_display);
    if (state_it != display_states_.end()) {
      DisplayFrameState& prev_state = state_it->second;
      was_registered = prev_state.view_ids.count(view_id) > 0;
      if (was_registered) {
        is_renderable = prev_state.renderable_view_ids.count(view_id) > 0;
      }
      if (prev_state.frame_in_progress &&
          prev_state.current_frame_view_ids.count(view_id) > 0) {
        CompleteFrameOpportunity(prev_state, {view_id},
                                 FrameOpportunityOutcome::kTargetRemoved);
        delegate_.OnAnimatorEmptyFrameForDisplay(prev_state.display_id,
                                                 {view_id});
      } else if (prev_state.frame_scheduled &&
                 (prev_state.pending_frame_all_views ||
                  prev_state.pending_frame_view_ids.count(view_id) > 0)) {
        prev_state.removed_target_ids.insert(view_id);
      }
      prev_state.view_ids.erase(view_id);
      prev_state.renderable_view_ids.erase(view_id);
      prev_state.pending_frame_view_ids.erase(view_id);
      prev_state.current_frame_view_ids.erase(view_id);
      prev_state.rendered_views_this_frame.erase(view_id);
      if (prev_state.frame_in_progress &&
          prev_state.rendered_views_this_frame ==
              prev_state.current_frame_view_ids) {
        EndFrameForDisplay(prev_state);
      }
    }
  }
  default_state_.view_ids.erase(view_id);
  default_state_.renderable_view_ids.erase(view_id);

  // Add to new display.
  auto state_it = display_states_.find(display_id);
  if (state_it != display_states_.end()) {
    state_it->second.view_ids.insert(view_id);
    if (!was_registered || is_renderable) {
      state_it->second.renderable_view_ids.insert(view_id);
    }
    view_to_display_[view_id] = display_id;

    return !was_registered || is_renderable;
  } else {
    // Display not registered yet; record the intended display so that
    // AddDisplay can move this view when the display arrives.
    view_to_display_[view_id] = display_id;
    default_state_.view_ids.insert(view_id);
    if (!was_registered || is_renderable) {
      default_state_.renderable_view_ids.insert(view_id);
    }
    return false;
  }
}

bool Animator::SetViewVisibility(int64_t view_id, ViewVisibility visibility) {
  DisplayFrameState* state = &default_state_;
  auto mapping_it = view_to_display_.find(view_id);
  if (mapping_it != view_to_display_.end()) {
    auto state_it = display_states_.find(mapping_it->second);
    if (state_it != display_states_.end()) {
      state = &state_it->second;
    }
  }
  if (state->view_ids.count(view_id) == 0) {
    return false;
  }

  const bool had_renderable_views = HasRenderableViews();
  const bool should_render = visibility == ViewVisibility::kVisible;
  const bool was_renderable = state->renderable_view_ids.count(view_id) > 0;
  if (should_render == was_renderable) {
    return false;
  }

  if (should_render) {
    state->renderable_view_ids.insert(view_id);
    if (state != &default_state_) {
      static_cast<void>(
          RequestFrameForDisplayViews(state->display_id, {view_id}));
    }
  } else {
    // Keep existing pending/current target sets intact. If the platform has
    // already admitted an exact opportunity, OnDisplayVsync reconciles this
    // still-active but no-longer-requested target as NoVisualChange.
    state->renderable_view_ids.erase(view_id);
  }
  return had_renderable_views && !HasRenderableViews();
}

void Animator::RemoveView(int64_t view_id) {
  // Drop any queued render payload for the removed view immediately.
  layer_trees_tasks_.erase(view_id);

  default_state_.view_ids.erase(view_id);
  default_state_.renderable_view_ids.erase(view_id);
  default_state_.pending_frame_view_ids.erase(view_id);
  default_state_.current_frame_view_ids.erase(view_id);
  default_state_.rendered_views_this_frame.erase(view_id);

  auto mapping_it = view_to_display_.find(view_id);
  if (mapping_it == view_to_display_.end()) {
    return;
  }

  int64_t display_id = mapping_it->second;
  view_to_display_.erase(mapping_it);

  auto state_it = display_states_.find(display_id);
  if (state_it == display_states_.end()) {
    return;
  }

  DisplayFrameState& state = state_it->second;
  if (state.frame_in_progress &&
      state.current_frame_view_ids.count(view_id) > 0) {
    CompleteFrameOpportunity(state, {view_id},
                             FrameOpportunityOutcome::kTargetRemoved);
    delegate_.OnAnimatorEmptyFrameForDisplay(state.display_id, {view_id});
  } else if (state.frame_scheduled &&
             (state.pending_frame_all_views ||
              state.pending_frame_view_ids.count(view_id) > 0)) {
    state.removed_target_ids.insert(view_id);
  }
  state.view_ids.erase(view_id);
  state.renderable_view_ids.erase(view_id);
  state.pending_frame_view_ids.erase(view_id);
  state.current_frame_view_ids.erase(view_id);
  state.rendered_views_this_frame.erase(view_id);

  // If this removal unblocks an in-progress frame waiting on the removed view,
  // end the frame now so it does not stall.
  if (state.frame_in_progress &&
      state.rendered_views_this_frame == state.current_frame_view_ids) {
    EndFrameForDisplay(state);
  }
}

void Animator::RequestFrameForDisplay(int64_t display_id,
                                      bool regenerate_layer_trees) {
  static_cast<void>(RequestFrameForDisplayInternal(display_id, nullptr,
                                                   regenerate_layer_trees));
}

bool Animator::RequestFrameForDisplayViews(int64_t display_id,
                                           const std::set<int64_t>& view_ids,
                                           bool regenerate_layer_trees) {
  return RequestFrameForDisplayInternal(display_id, &view_ids,
                                        regenerate_layer_trees);
}

void Animator::CancelFrameOpportunity(int64_t display_id,
                                      FrameOpportunityId opportunity_id,
                                      VsyncWaiter::CancellationReason reason) {
  DisplayFrameState* state = GetDisplayState(display_id);
  if (!state) {
    return;
  }

  if (state->current_opportunity_id == opportunity_id) {
    FML_DCHECK(!state->frame_in_progress)
        << "A UI task cannot interleave with a synchronous frame build";
    state->current_frame_view_ids.clear();
    state->rendered_views_this_frame.clear();
    state->frame_timings_recorder = nullptr;
    state->current_opportunity_id = std::nullopt;
    state->frame_regenerate_layer_trees = true;
    return;
  }

  if (state->last_delivered_opportunity_id == opportunity_id) {
    // The frame already crossed the UI boundary. The engine-local registry
    // suppresses any later raster/root-target completion for this id.
    return;
  }

  if (state->frame_scheduled) {
    // Return won the waiter race, but its UI callback has not run yet.
    state->cancelled_opportunity_id = opportunity_id;
    state->cancelled_opportunity_reason = reason;
  }
}

bool Animator::RequestFrameForDisplayInternal(int64_t display_id,
                                              const std::set<int64_t>* view_ids,
                                              bool regenerate_layer_trees) {
  // Only proceed if the display has been explicitly registered.
  if (display_states_.find(display_id) == display_states_.end()) {
    // A view-scoped request against an unknown display can never produce a
    // frame. Complete it through the legacy empty-frame compatibility path.
    if (view_ids != nullptr && !view_ids->empty()) {
      delegate_.OnAnimatorEmptyFrameForDisplay(display_id, *view_ids);
    }
    return false;
  }
  DisplayFrameState* state = GetDisplayState(display_id);
  if (!state) {
    return false;
  }

  // Requested views that are not homed on this display are filtered by
  // LatchDisplayFrameRequest. Report them through the empty-frame terminal
  // path so callers can re-resolve their current display.
  if (view_ids != nullptr) {
    std::set<int64_t> unhomed_view_ids;
    for (int64_t view_id : *view_ids) {
      if (state->view_ids.find(view_id) == state->view_ids.end()) {
        unhomed_view_ids.insert(view_id);
      }
    }
    if (!unhomed_view_ids.empty()) {
      delegate_.OnAnimatorEmptyFrameForDisplay(display_id, unhomed_view_ids);
    }
  }

  if (!LatchDisplayFrameRequest(*state, view_ids, regenerate_layer_trees)) {
    return false;
  }

  if (state->frame_scheduled) {
    return true;
  }

  // Latch frame requests that arrive while this display is currently building
  // a frame. We cannot schedule an additional vsync callback yet, but we also
  // must not drop the request (animations call scheduleFrame() during build).
  if (state->frame_in_progress) {
    state->frame_scheduled = true;
    return true;
  }

  ScheduleDisplayVsync(*state);
  return true;
}

void Animator::ScheduleDisplayVsync(DisplayFrameState& state) {
  state.frame_scheduled = true;
  const auto display_id = state.display_id;
  const auto display_id_str = std::to_string(display_id);
  TRACE_EVENT1("flutter", "Animator::RequestFrameForDisplay", "display_id",
               display_id_str.c_str());

  waiter_->AsyncWaitForVsync(
      display_id,
      [self = weak_factory_.GetWeakPtr(),
       display_id](std::unique_ptr<FrameTimingsRecorder> recorder) {
        if (self) {
          self->OnDisplayVsync(display_id, std::move(recorder));
        }
      },
      [self = weak_factory_.GetWeakPtr(),
       display_id](VsyncWaiter::CancellationReason reason) {
        if (self) {
          self->OnDisplayVsyncCancelled(display_id, reason);
        }
      });
}

bool Animator::IsPerDisplayMode() const {
  return per_display_opt_in_ && !display_states_.empty();
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
  const auto frame_opportunity =
      state->frame_timings_recorder->GetFrameOpportunity();
  state->current_opportunity_id =
      frame_opportunity.has_value()
          ? std::optional<FrameOpportunityId>(frame_opportunity->id)
          : std::nullopt;

  if (state->cancelled_opportunity_id.has_value()) {
    const bool cancelled_current =
        state->current_opportunity_id == state->cancelled_opportunity_id;
    const bool preserve_demand =
        state->cancelled_opportunity_reason ==
        VsyncWaiter::CancellationReason::kTransportLost;
    state->cancelled_opportunity_id = std::nullopt;
    state->cancelled_opportunity_reason = std::nullopt;
    if (cancelled_current) {
      state->last_delivered_opportunity_id = state->current_opportunity_id;
      if (!preserve_demand) {
        state->pending_frame_view_ids.clear();
        state->pending_frame_all_views = false;
        state->pending_regenerate_layer_trees = false;
        state->removed_target_ids.clear();
      }
      state->frame_timings_recorder = nullptr;
      state->current_opportunity_id = std::nullopt;
      if (state->retired) {
        display_states_.erase(display_id);
      }
      return;
    }
  }
  state->last_delivered_opportunity_id = state->current_opportunity_id;

  if (state->retired) {
    const std::set<int64_t>& retired_targets =
        frame_opportunity.has_value() ? frame_opportunity->target_ids
                                      : state->removed_target_ids;
    CompleteFrameOpportunity(*state, retired_targets,
                             FrameOpportunityOutcome::kTargetRemoved);
    if (!retired_targets.empty()) {
      delegate_.OnAnimatorEmptyFrameForDisplay(display_id, retired_targets);
    }
    display_states_.erase(display_id);
    return;
  }

  state->frame_regenerate_layer_trees = state->pending_regenerate_layer_trees;
  state->pending_regenerate_layer_trees = false;
  std::set<int64_t> requested_view_ids;
  if (state->pending_frame_all_views) {
    requested_view_ids = state->renderable_view_ids;
  } else {
    std::set_intersection(
        state->pending_frame_view_ids.begin(),
        state->pending_frame_view_ids.end(), state->renderable_view_ids.begin(),
        state->renderable_view_ids.end(),
        std::inserter(requested_view_ids, requested_view_ids.end()));
  }

  if (frame_opportunity.has_value()) {
    FML_DCHECK(!frame_opportunity->target_ids.empty());
    const ReconciledFrameTargets targets =
        ReconcileFrameTargets(frame_opportunity->target_ids, requested_view_ids,
                              state->view_ids, state->removed_target_ids);
    state->current_frame_view_ids = targets.render;
    state->pending_frame_all_views = false;
    state->pending_frame_view_ids.clear();
    state->removed_target_ids.clear();

    CompleteFrameOpportunity(*state, targets.removed,
                             FrameOpportunityOutcome::kTargetRemoved);
    CompleteFrameOpportunity(*state, targets.no_visual_change,
                             FrameOpportunityOutcome::kNoVisualChange);

    std::set<int64_t> terminal_only_targets = targets.removed;
    terminal_only_targets.insert(targets.no_visual_change.begin(),
                                 targets.no_visual_change.end());
    if (!terminal_only_targets.empty()) {
      delegate_.OnAnimatorEmptyFrameForDisplay(display_id,
                                               terminal_only_targets);
    }
  } else {
    if (!state->removed_target_ids.empty()) {
      delegate_.OnAnimatorEmptyFrameForDisplay(display_id,
                                               state->removed_target_ids);
      state->removed_target_ids.clear();
    }
    ResolveDisplayFrameViewIds(*state);
  }

  if (state->current_frame_view_ids.empty()) {
    state->frame_regenerate_layer_trees = true;
    if (!frame_opportunity.has_value() && !requested_view_ids.empty()) {
      CompleteFrameOpportunity(*state, requested_view_ids,
                               FrameOpportunityOutcome::kTargetRemoved);
      delegate_.OnAnimatorEmptyFrameForDisplay(state->display_id,
                                               requested_view_ids);
    }
    state->frame_timings_recorder = nullptr;
    state->current_opportunity_id = std::nullopt;
    return;
  }

  BeginFrameForDisplay(*state);
}

void Animator::OnDisplayVsyncCancelled(
    int64_t display_id,
    VsyncWaiter::CancellationReason cancellation_reason) {
  const auto display_id_str = std::to_string(display_id);
  TRACE_EVENT1("flutter", "Animator::OnDisplayVsyncCancelled", "display_id",
               display_id_str.c_str());

  DisplayFrameState* state = GetDisplayState(display_id);
  if (!state) {
    return;
  }

  FML_DCHECK(state->frame_scheduled);
  FML_DCHECK(!state->frame_in_progress);
  state->frame_scheduled = false;
  state->cancelled_opportunity_id = std::nullopt;
  state->cancelled_opportunity_reason = std::nullopt;

  if (cancellation_reason == VsyncWaiter::CancellationReason::kTransportLost) {
    // Demand remains latched but intentionally unarmed. A new transport epoch
    // or a later explicit framework request is the only event allowed to ask
    // for another platform baton.
    return;
  }

  state->pending_frame_view_ids.clear();
  state->pending_frame_all_views = false;
  state->pending_regenerate_layer_trees = false;
  state->removed_target_ids.clear();
  if (state->retired) {
    display_states_.erase(display_id);
  }
}

std::set<int64_t> Animator::CompleteFrameOpportunity(
    DisplayFrameState& state,
    const std::set<int64_t>& target_ids,
    FrameOpportunityOutcome outcome) {
  if (!state.current_opportunity_id.has_value()) {
    return target_ids;
  }
  std::set<int64_t> completed_target_ids;
  for (int64_t target_id : target_ids) {
    if (delegate_.OnAnimatorFrameOpportunityOutcome(
            state.current_opportunity_id.value(), state.display_id, target_id,
            outcome)) {
      completed_target_ids.insert(target_id);
    }
  }
  return completed_target_ids;
}

void Animator::BeginFrameForDisplay(DisplayFrameState& state) {
  const auto display_id_str = std::to_string(state.display_id);
  TRACE_EVENT1("flutter", "Animator::BeginFrameForDisplay", "display_id",
               display_id_str.c_str());

  FML_DCHECK(!state.frame_in_progress);
  EndTraceFlowIds();
  if (!state.frame_regenerate_layer_trees) {
    if (!has_rendered_ || pending_configure_serial_ != 0) {
      state.frame_regenerate_layer_trees = true;
    } else {
      if (!state.frame_timings_recorder) {
        state.frame_timings_recorder = std::make_unique<FrameTimingsRecorder>();
        const fml::TimePoint now = fml::TimePoint::Now();
        state.frame_timings_recorder->RecordVsync(now, now);
      }
      const fml::TimePoint build_time = fml::TimePoint::Now();
      state.frame_timings_recorder->RecordBuildStart(build_time);
      state.frame_timings_recorder->RecordBuildEnd(build_time);
      delegate_.OnAnimatorDrawLastLayerTreesForDisplay(
          std::move(state.frame_timings_recorder),
          state.current_frame_view_ids);
      state.frame_timings_recorder = nullptr;
      state.current_opportunity_id = std::nullopt;
      state.frame_regenerate_layer_trees = true;
      // Rasterization of the retained trees carries the opportunity id into
      // the exact root-target callback. It is real produced work, not an empty
      // frame. EO3 narrows the retained-tree selection to this target set.
      state.current_frame_view_ids.clear();
      return;
    }
  }

  // Every display drains through the same raster pipeline, so the producer
  // reservation must also be shared. An empty frame intentionally preserves
  // the reservation for the next frame, matching the legacy animator path.
  // Keeping that reservation per display lets two empty displays consume both
  // slots of the shared depth-two pipeline without producing anything.
  FML_DCHECK(state.pipeline);
  FML_DCHECK(state.pipeline.get() == layer_tree_pipeline_.get());
  if (!producer_continuation_) {
    producer_continuation_ = state.pipeline->Produce();
    if (!producer_continuation_) {
      TRACE_EVENT0("flutter", "PipelineFull");
      const auto retry_targets =
          CompleteFrameOpportunity(state, state.current_frame_view_ids,
                                   FrameOpportunityOutcome::kBackpressured);
      delegate_.OnAnimatorEmptyFrameForDisplay(state.display_id,
                                               state.current_frame_view_ids);
      state.current_frame_view_ids.clear();
      state.frame_timings_recorder = nullptr;
      state.current_opportunity_id = std::nullopt;
      // Retry demand is a separate edge from terminal backpressure.
      if (!retry_targets.empty()) {
        static_cast<void>(
            RequestFrameForDisplayViews(state.display_id, retry_targets));
      }
      return;
    }
  }

  state.frame_in_progress = true;
  state.rendered_views_this_frame.clear();
  has_rendered_ = true;

  // If layer trees for this display were already produced by a prior
  // callback (for example, legacy global draw fallback rendering all views),
  // mark them as rendered so this display can consume the newest cached task
  // even when no new Render() arrives in this specific begin-frame callback.
  for (int64_t view_id : state.current_frame_view_ids) {
    if (layer_trees_tasks_.find(view_id) != layer_trees_tasks_.end()) {
      state.rendered_views_this_frame.insert(view_id);
    }
  }

  if (!state.frame_timings_recorder) {
    state.frame_timings_recorder = std::make_unique<FrameTimingsRecorder>();
    const fml::TimePoint now = fml::TimePoint::Now();
    state.frame_timings_recorder->RecordVsync(now, now);
  }
  state.frame_timings_recorder->RecordBuildStart(fml::TimePoint::Now());

  const fml::TimePoint frame_target_time =
      state.frame_timings_recorder->GetVsyncTargetTime();
  uint64_t frame_number = state.frame_timings_recorder->GetFrameNumber();

  // Notify delegate with display-scoped view set.
  delegate_.OnAnimatorBeginFrameForDisplay(frame_target_time, frame_number,
                                           state.display_id,
                                           state.current_frame_view_ids);

  // End the frame if it wasn't already completed by Render() callbacks.
  if (state.frame_in_progress) {
    EndFrameForDisplay(state);
  }
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

    PipelineProduceResult result = producer_continuation_.Complete(
        std::make_unique<FrameItem>(std::move(display_layer_trees),
                                    std::move(state.frame_timings_recorder)));

    if (!result.success) {
      FML_DLOG(INFO) << "Failed to commit per-display frame to pipeline";
      CompleteFrameOpportunity(state, state.current_frame_view_ids,
                               FrameOpportunityOutcome::kRasterFailed);
    } else {
      delegate_.OnAnimatorDraw(state.pipeline);
      // A view can be part of this frame's request set yet produce no layer
      // tree. Its sibling render jobs continue; this target terminalizes
      // independently as no visual change.
      std::set<int64_t> unrendered_view_ids;
      for (int64_t view_id : state.current_frame_view_ids) {
        if (state.rendered_views_this_frame.count(view_id) == 0) {
          unrendered_view_ids.insert(view_id);
        }
      }
      if (!unrendered_view_ids.empty()) {
        CompleteFrameOpportunity(state, unrendered_view_ids,
                                 FrameOpportunityOutcome::kNoVisualChange);
        delegate_.OnAnimatorEmptyFrameForDisplay(state.display_id,
                                                 unrendered_view_ids);
      }
    }
  } else {
    CompleteFrameOpportunity(state, state.current_frame_view_ids,
                             FrameOpportunityOutcome::kNoVisualChange);
    delegate_.OnAnimatorEmptyFrameForDisplay(state.display_id,
                                             state.current_frame_view_ids);
  }

  // Reset frame state.
  const bool request_next_frame = state.frame_scheduled;
  state.frame_in_progress = false;
  state.rendered_views_this_frame.clear();
  state.current_frame_view_ids.clear();
  state.frame_timings_recorder = nullptr;
  state.current_opportunity_id = std::nullopt;
  state.frame_regenerate_layer_trees = true;

  // Only request the next frame if a new request was latched while this
  // display frame was in progress.
  if (request_next_frame) {
    state.frame_scheduled = false;
    ScheduleDisplayVsync(state);
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

bool Animator::HasRenderableViews() const {
  if (!default_state_.renderable_view_ids.empty()) {
    return true;
  }
  return std::any_of(display_states_.begin(), display_states_.end(),
                     [](const auto& entry) {
                       return !entry.second.renderable_view_ids.empty();
                     });
}

void Animator::ScheduleMaybeClearTraceFlowIds() {
  if (IsPerDisplayMode()) {
    // The embedder's exact per-display batons represent compositor frame
    // opportunities. Trace cleanup is observability, not frame demand, so it
    // must not mint a second, targetless platform baton. A real display frame
    // ends these flows in BeginFrameForDisplay; otherwise the next UI turn
    // does.
    task_runners_.GetUITaskRunner()->PostTask(
        [self = weak_factory_.GetWeakPtr()] {
          if (!self) {
            return;
          }
          const bool frame_pending =
              std::any_of(self->display_states_.begin(),
                          self->display_states_.end(), [](const auto& entry) {
                            return entry.second.frame_scheduled ||
                                   entry.second.frame_in_progress;
                          });
          if (!frame_pending) {
            self->EndTraceFlowIds();
          }
        });
    return;
  }

  waiter_->ScheduleSecondaryCallback(
      reinterpret_cast<uintptr_t>(this), [self = weak_factory_.GetWeakPtr()] {
        if (!self) {
          return;
        }
        if (!self->frame_scheduled_ && !self->trace_flow_ids_.empty()) {
          const size_t flow_id_count = self->trace_flow_ids_.size();
          std::unique_ptr<uint64_t[]> flow_ids =
              std::make_unique<uint64_t[]>(flow_id_count);
          for (size_t i = 0; i < flow_id_count; ++i) {
            flow_ids.get()[i] = self->trace_flow_ids_.at(i);
          }

          TRACE_EVENT0_WITH_FLOW_IDS(
              "flutter", "Animator::ScheduleMaybeClearTraceFlowIds - callback",
              flow_id_count, flow_ids.get());

          self->EndTraceFlowIds();
        }
      });
}

void Animator::EndTraceFlowIds() {
  while (!trace_flow_ids_.empty()) {
    const uint64_t flow_id = trace_flow_ids_.front();
    TRACE_FLOW_END("flutter", "PointerEvent", flow_id);
    trace_flow_ids_.pop_front();
  }
}

}  // namespace flutter
