// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_ANIMATOR_H_
#define FLUTTER_SHELL_COMMON_ANIMATOR_H_

#include <cstddef>
#include <deque>
#include <set>
#include <unordered_map>

#include "flutter/common/task_runners.h"
#include "flutter/flow/frame_timings.h"
#include "flutter/fml/memory/ref_ptr.h"
#include "flutter/fml/memory/weak_ptr.h"
#include "flutter/fml/synchronization/semaphore.h"
#include "flutter/fml/time/time_point.h"
#include "flutter/shell/common/pipeline.h"
#include "flutter/shell/common/rasterizer.h"
#include "flutter/shell/common/vsync_waiter.h"

namespace flutter {

namespace testing {
class ShellTest;
}

/// Executor of animations.
///
/// In conjunction with the |VsyncWaiter| it allows callers (typically Dart
/// code) to schedule work that ends up generating a |LayerTree|.
class Animator final {
 public:
  class Delegate {
   public:
    virtual void OnAnimatorBeginFrame(fml::TimePoint frame_target_time,
                                      uint64_t frame_number) = 0;

    /// Per-display variant of OnAnimatorBeginFrame. The delegate should
    /// invoke PlatformDispatcher.onBeginFrame scoped to the given views.
    ///
    /// The default implementation calls the legacy OnAnimatorBeginFrame,
    /// ignoring display_id and view_ids.
    virtual void OnAnimatorBeginFrameForDisplay(
        fml::TimePoint frame_target_time,
        uint64_t frame_number,
        int64_t display_id,
        const std::set<int64_t>& view_ids) {
      OnAnimatorBeginFrame(frame_target_time, frame_number);
    }

    virtual void OnAnimatorNotifyIdle(fml::TimeDelta deadline) = 0;

    virtual void OnAnimatorUpdateLatestFrameTargetTime(
        fml::TimePoint frame_target_time) = 0;

    virtual void OnAnimatorDraw(std::shared_ptr<FramePipeline> pipeline) = 0;

    virtual void OnAnimatorDrawLastLayerTrees(
        std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder) = 0;
  };

  Animator(Delegate& delegate,
           const TaskRunners& task_runners,
           std::unique_ptr<VsyncWaiter> waiter);

  ~Animator();

  void RequestFrame(bool regenerate_layer_trees = true);

  /// Bypasses vsync wait and immediately begins a frame build.
  /// Used for synchronous resize to minimize latency.
  void ScheduleImmediateFrame(uint64_t configure_serial);

  //--------------------------------------------------------------------------
  /// @brief    Tells the Animator that all views that should render for this
  ///           frame have been rendered.
  ///
  ///           In regular frames, since all `Render` calls must take place
  ///           during a vsync task, the Animator knows that all views have
  ///           been rendered at the end of the vsync task, therefore calling
  ///           this method is not needed.
  ///
  ///           However, the engine might decide to start it a bit earlier, for
  ///           example, if the engine decides that no more views can be
  ///           rendered, so that the rasterization can start a bit earlier.
  ///
  ///           This method is also useful in warm-up frames. In a warm up
  ///           frame, `Animator::Render` is called out of vsync tasks, and
  ///           Animator requires an explicit end-of-frame call to know when to
  ///           send the layer trees to the pipeline.
  ///
  ///           For more about warm up frames, see
  ///           `PlatformDispatcher.scheduleWarmUpFrame`.
  ///
  void OnAllViewsRendered();

  // -----------------------------------------------------------------------
  // Per-Display VSync API
  // -----------------------------------------------------------------------

  /// Per-display frame scheduling state.
  ///
  /// Each registered display maintains its own independent frame lifecycle:
  /// request -> vsync -> begin frame -> render views -> end frame -> rasterize.
  struct DisplayFrameState {
    int64_t display_id = VsyncWaiter::kDefaultDisplayId;
    double refresh_rate = 60.0;
    std::set<int64_t> view_ids;
    std::set<int64_t> rendered_views_this_frame;
    bool frame_scheduled = false;
    bool frame_in_progress = false;
    // Latch the strongest mode for the next scheduled vsync. A full rebuild
    // dominates texture-only requests so scene mutations are never dropped.
    bool pending_regenerate_layer_trees = false;
    // Tracks whether the current display frame must rebuild layer trees.
    bool frame_regenerate_layer_trees = true;
    std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder;
    FramePipeline::ProducerContinuation producer_continuation;
    // All displays share the engine's single raster pipeline. Display-scoped
    // scheduling stays independent, but downstream raster backpressure and
    // resubmission logic remain coherent because frame items drain through one
    // queue.
    std::shared_ptr<FramePipeline> pipeline;

    // Frame-flow stall diagnostics.
    uint64_t total_frames = 0;
    uint64_t empty_frames = 0;
    uint64_t consecutive_empty_frames = 0;
    uint64_t pipeline_full_count = 0;
  };

  /// Registers a display with the given refresh rate.
  void AddDisplay(int64_t display_id, double refresh_rate);

  /// Removes a display. Views on it move to the default display.
  void RemoveDisplay(int64_t display_id);

  /// Removes displays not in the given set. Views on removed displays
  /// move to the default display.
  void RemoveStaleDisplays(const std::set<int64_t>& active_ids);

  /// Assigns a view to a display for per-display vsync rendering.
  void SetViewDisplay(int64_t view_id, int64_t display_id);

  /// Removes a view from all per-display tracking state.
  void RemoveView(int64_t view_id);

  /// Requests a frame for a specific display on its next vsync.
  void RequestFrameForDisplay(int64_t display_id,
                              bool regenerate_layer_trees = true);

  /// Returns true if per-display mode is active.
  bool IsPerDisplayMode() const;

  //--------------------------------------------------------------------------
  /// @brief    Tells the Animator that this frame needs to render another view.
  ///
  ///           This method must be called during a vsync callback, or
  ///           technically, between Animator::BeginFrame and Animator::EndFrame
  ///           (both private methods). Otherwise, this call will be ignored.
  ///
  void Render(int64_t view_id,
              std::unique_ptr<flutter::LayerTree> layer_tree,
              float device_pixel_ratio);

  const std::weak_ptr<VsyncWaiter> GetVsyncWaiter() const;

  //--------------------------------------------------------------------------
  /// @brief    Schedule a secondary callback to be executed right after the
  ///           main `VsyncWaiter::AsyncWaitForVsync` callback (which is added
  ///           by `Animator::RequestFrame`).
  ///
  ///           Like the callback in `AsyncWaitForVsync`, this callback is
  ///           only scheduled to be called once, and it's supposed to be
  ///           called in the UI thread. If there is no AsyncWaitForVsync
  ///           callback (`Animator::RequestFrame` is not called), this
  ///           secondary callback will still be executed at vsync.
  ///
  ///           This callback is used to provide the vsync signal needed by
  ///           `SmoothPointerDataDispatcher`, and for our own flow events.
  ///
  /// @see      `PointerDataDispatcher::ScheduleSecondaryVsyncCallback`.
  void ScheduleSecondaryVsyncCallback(uintptr_t id,
                                      const fml::closure& callback);

  // Enqueue |trace_flow_id| into |trace_flow_ids_|.  The flow event will be
  // ended at either the next frame, or the next vsync interval with no active
  // rendering.
  void EnqueueTraceFlowId(uint64_t trace_flow_id);

 private:
  // Animator's work during a vsync is split into two methods, BeginFrame and
  // EndFrame. The two methods should be called synchronously back-to-back to
  // avoid being interrupted by a regular vsync. The reason to split them is to
  // allow ShellTest::PumpOneFrame to insert a Render in between.

  void BeginFrame(std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder,
                  bool preserve_regenerate_layer_trees = false);
  void EndFrame();

  bool CanReuseLastLayerTrees();

  void DrawLastLayerTrees(
      std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder);
  void DrawLastLayerTreesForDisplay(
      std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder);

  void AwaitVSync();

  // Clear |trace_flow_ids_| if |frame_scheduled_| is false.
  void ScheduleMaybeClearTraceFlowIds();

  // -----------------------------------------------------------------------
  // Per-Display Frame Lifecycle (Private)
  // -----------------------------------------------------------------------

  /// Callback invoked when a display's vsync fires.
  void OnDisplayVsync(int64_t display_id,
                      std::unique_ptr<FrameTimingsRecorder> recorder);

  /// Begins a frame for the given display: acquires a pipeline slot,
  /// notifies the delegate to invoke Dart callbacks for this display's views.
  void BeginFrameForDisplay(DisplayFrameState& state);

  /// Ends a frame for the given display: packages layer trees into a
  /// FrameItem and submits to the pipeline.
  void EndFrameForDisplay(DisplayFrameState& state);

  /// Returns the DisplayFrameState for the given display, or nullptr.
  DisplayFrameState* GetDisplayState(int64_t display_id);

  /// Returns the display ID for the given view.
  int64_t GetDisplayForView(int64_t view_id) const;

  size_t TotalTrackedDisplayViews() const;
  void LogStateHighWatermarks(const char* reason);

  Delegate& delegate_;
  TaskRunners task_runners_;
  std::shared_ptr<VsyncWaiter> waiter_;

  std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder_;
  std::unordered_map<int64_t, std::unique_ptr<LayerTreeTask>>
      layer_trees_tasks_;
  uint64_t frame_request_number_ = 1;
  fml::TimeDelta dart_frame_deadline_;
  std::shared_ptr<FramePipeline> layer_tree_pipeline_;
  fml::Semaphore pending_frame_semaphore_;
  FramePipeline::ProducerContinuation producer_continuation_;
  bool regenerate_layer_trees_ = false;
  bool frame_scheduled_ = false;
  std::deque<uint64_t> trace_flow_ids_;
  bool has_rendered_ = false;
  // Non-zero while a synchronous resize frame is pending. This serial is
  // stamped onto subsequent frame recorders until a frame is successfully
  // committed to the pipeline.
  uint64_t pending_configure_serial_ = 0;

  // -----------------------------------------------------------------------
  // Per-Display State
  // -----------------------------------------------------------------------

  /// The display ID of the frame currently being processed, or -1 if no
  /// per-display frame is active. Used to scope RequestFrame() calls made
  /// during a display's frame (e.g. from Dart's scheduleFrame()) to only
  /// re-schedule the active display, preventing cross-display coupling.
  int64_t active_frame_display_id_ = -1;

  /// Per-display frame states. Empty when in single-display mode.
  std::unordered_map<int64_t, DisplayFrameState> display_states_;

  /// View-to-display mapping. Views not in this map are on the default display.
  std::unordered_map<int64_t, int64_t> view_to_display_;

  /// Fallback state used when no displays are registered.
  DisplayFrameState default_state_;

  // Total Render() calls across all views (frame-flow stall diagnostic).
  uint64_t render_calls_total_ = 0;
  size_t layer_trees_tasks_high_water_ = 0;
  size_t display_states_high_water_ = 0;
  size_t view_to_display_high_water_ = 0;
  size_t default_view_ids_high_water_ = 0;
  size_t display_view_ids_high_water_ = 0;

  fml::TaskRunnerAffineWeakPtrFactory<Animator> weak_factory_;

  friend class testing::ShellTest;

  FML_DISALLOW_COPY_AND_ASSIGN(Animator);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_ANIMATOR_H_
