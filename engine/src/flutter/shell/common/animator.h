// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_ANIMATOR_H_
#define FLUTTER_SHELL_COMMON_ANIMATOR_H_

#include <cstddef>
#include <deque>
#include <optional>
#include <set>
#include <unordered_map>

#include "flutter/common/frame_opportunity.h"
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

    virtual void OnAnimatorDrawLastLayerTreesForDisplay(
        std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder,
        const std::set<int64_t>& view_ids) {
      OnAnimatorDrawLastLayerTrees(std::move(frame_timings_recorder));
    }

    /// Notifies the delegate that a scheduled frame for the given display
    /// completed without producing any layer trees (no Render() calls from
    /// Dart for this display's views).
    ///
    /// This lets embedders retire in-flight frame bookkeeping without waiting
    /// for a present_view callback that will never arrive.
    virtual void OnAnimatorEmptyFrameForDisplay(
        int64_t display_id,
        const std::set<int64_t>& view_ids) {}

    virtual bool OnAnimatorFrameOpportunityOutcome(
        FrameOpportunityId opportunity_id,
        int64_t display_id,
        int64_t target_id,
        FrameOpportunityOutcome outcome) {
      return false;
    }
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
  enum class ViewVisibility {
    kVisible,
    kObscured,
    kSuspended,
  };

  struct DisplayFrameState {
    int64_t display_id = VsyncWaiter::kDefaultDisplayId;
    double refresh_rate = 60.0;
    // All registered views remain active targets for exact opportunity
    // reconciliation. Only renderable views may create new frame demand.
    std::set<int64_t> view_ids;
    std::set<int64_t> renderable_view_ids;
    // Views requested for the next display frame. Full-display requests take
    // precedence over subset requests so global callers keep legacy behavior.
    std::set<int64_t> pending_frame_view_ids;
    bool pending_frame_all_views = false;
    // Views participating in the frame currently being built. This is normally
    // the full display set, but compositor-directed frames can narrow it to the
    // view ids that actually changed.
    std::set<int64_t> current_frame_view_ids;
    std::set<int64_t> rendered_views_this_frame;
    // Targets removed after demand was admitted but before the exact frame
    // opportunity reached its UI callback remain named until that opportunity
    // terminalizes them.
    std::set<int64_t> removed_target_ids;
    bool retired = false;
    bool frame_scheduled = false;
    bool frame_in_progress = false;
    // Latch the strongest mode for the next scheduled vsync. A full rebuild
    // dominates texture-only requests so scene mutations are never dropped.
    bool pending_regenerate_layer_trees = false;
    // Tracks whether the current display frame must rebuild layer trees.
    bool frame_regenerate_layer_trees = true;
    std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder;
    std::optional<FrameOpportunityId> current_opportunity_id;
    std::optional<FrameOpportunityId> last_delivered_opportunity_id;
    std::optional<FrameOpportunityId> cancelled_opportunity_id;
    std::optional<VsyncWaiter::CancellationReason> cancelled_opportunity_reason;
    // All displays share the engine's single raster pipeline. Display-scoped
    // scheduling stays independent, but downstream raster backpressure and
    // resubmission logic remain coherent because frame items drain through one
    // queue. The producer reservation is likewise pipeline-scoped and lives on
    // Animator; storing one reservation per display can reserve every slot
    // with empty frames and deadlock unrelated displays.
    std::shared_ptr<FramePipeline> pipeline;
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

  /// Changes whether a registered view may create future raster demand.
  /// Returns true only when this update transitions the engine from at least
  /// one renderable view to none, allowing the embedder to trim idle caches.
  bool SetViewVisibility(int64_t view_id, ViewVisibility visibility);

  /// Removes a view from all per-display tracking state.
  void RemoveView(int64_t view_id);

  /// Requests a frame for a specific display on its next vsync.
  void RequestFrameForDisplay(int64_t display_id,
                              bool regenerate_layer_trees = true);

  /// Requests a frame for a subset of views on a specific display.
  void RequestFrameForDisplayViews(int64_t display_id,
                                   const std::set<int64_t>& view_ids,
                                   bool regenerate_layer_trees = true);

  // Settles UI-side state for an opportunity cancelled after its scheduled
  // baton was returned. Registry terminality is owned outside Animator; this
  // method only prevents or abandons the matching frame turn.
  void CancelFrameOpportunity(int64_t display_id,
                              FrameOpportunityId opportunity_id,
                              VsyncWaiter::CancellationReason reason);

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
  void EndTraceFlowIds();

  // -----------------------------------------------------------------------
  // Per-Display Frame Lifecycle (Private)
  // -----------------------------------------------------------------------

  /// Callback invoked when a display's vsync fires.
  void OnDisplayVsync(int64_t display_id,
                      std::unique_ptr<FrameTimingsRecorder> recorder);

  /// Settles the scheduled half of a display frame when its embedder baton is
  /// cancelled before return. Transport cancellation preserves the latched
  /// demand for a later explicit request; authority/target teardown retires
  /// it with the epoch that owned the baton.
  void OnDisplayVsyncCancelled(
      int64_t display_id,
      VsyncWaiter::CancellationReason cancellation_reason);

  /// Begins a frame for the given display: acquires a pipeline slot,
  /// notifies the delegate to invoke Dart callbacks for this display's views.
  void BeginFrameForDisplay(DisplayFrameState& state);

  void RequestFrameForDisplayInternal(int64_t display_id,
                                      const std::set<int64_t>* view_ids,
                                      bool regenerate_layer_trees);

  void ScheduleDisplayVsync(DisplayFrameState& state);

  std::set<int64_t> CompleteFrameOpportunity(
      DisplayFrameState& state,
      const std::set<int64_t>& target_ids,
      FrameOpportunityOutcome outcome);

  /// Ends a frame for the given display: packages layer trees into a
  /// FrameItem and submits to the pipeline.
  void EndFrameForDisplay(DisplayFrameState& state);

  /// Returns the DisplayFrameState for the given display, or nullptr.
  DisplayFrameState* GetDisplayState(int64_t display_id);

  /// Returns the display ID for the given view.
  int64_t GetDisplayForView(int64_t view_id) const;

  bool HasRenderableViews() const;

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

  /// Per-display frame states. Empty when in single-display mode.
  std::unordered_map<int64_t, DisplayFrameState> display_states_;

  /// View-to-display mapping. Views not in this map are on the default display.
  std::unordered_map<int64_t, int64_t> view_to_display_;

  /// Fallback state used when no displays are registered.
  DisplayFrameState default_state_;

  fml::TaskRunnerAffineWeakPtrFactory<Animator> weak_factory_;

  friend class testing::ShellTest;

  FML_DISALLOW_COPY_AND_ASSIGN(Animator);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_ANIMATOR_H_
