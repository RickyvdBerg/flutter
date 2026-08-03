// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_VSYNC_WAITER_H_
#define FLUTTER_SHELL_COMMON_VSYNC_WAITER_H_

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "flutter/common/task_runners.h"
#include "flutter/flow/frame_timings.h"
#include "flutter/fml/time/time_point.h"

namespace flutter {

/// Abstract Base Class that represents a platform specific mechanism for
/// getting callbacks when a vsync event happens.
///
/// Per-display vsync support: The waiter maintains a map of pending callbacks
/// keyed by display ID. Each display can independently request and receive
/// vsync events. When no display ID is specified, `kDefaultDisplayId` is used,
/// preserving backward compatibility with single-display setups.
///
/// @see VsyncWaiterAndroid
/// @see VsyncWaiterEmbedder
class VsyncWaiter : public std::enable_shared_from_this<VsyncWaiter> {
 public:
  using DisplayId = int64_t;
  using Callback = std::function<void(std::unique_ptr<FrameTimingsRecorder>)>;

  enum class CancellationReason : uint32_t {
    kTransportLost,
    kTargetRemoved,
    kEpochReplaced,
    kHostTerminated,
  };
  using CancellationCallback = std::function<void(CancellationReason)>;

  /// Sentinel value for the default (or only) display. All views not
  /// explicitly assigned to a display are treated as being on this display.
  static constexpr DisplayId kDefaultDisplayId = 0;

  virtual ~VsyncWaiter();

  /// Requests a vsync callback for the specified display.
  ///
  /// When the platform fires a vsync for this display, the callback will be
  /// invoked on the UI thread with a `FrameTimingsRecorder` capturing the
  /// frame start and target times.
  ///
  /// Multiple displays can have pending callbacks simultaneously. A vsync
  /// event for one display does not consume callbacks for other displays.
  void AsyncWaitForVsync(
      DisplayId display_id,
      const Callback& callback,
      const CancellationCallback& cancellation_callback = nullptr);

  /// Requests a vsync callback for the default display.
  ///
  /// This is the legacy single-display entry point. Equivalent to calling
  /// `AsyncWaitForVsync(kDefaultDisplayId, callback)`.
  void AsyncWaitForVsync(const Callback& callback);

  /// Add a secondary callback for key |id| for the next vsync.
  ///
  /// See also |PointerDataDispatcher::ScheduleSecondaryVsyncCallback| and
  /// |Animator::ScheduleMaybeClearTraceFlowIds|.
  void ScheduleSecondaryCallback(uintptr_t id, const fml::closure& callback);

  // Embedder-driven waiters override these to consume their own pending
  // platform token. Keeping the operation on the waiter instance makes token
  // custody engine-local; generic platform waiters reject the operation.
  virtual bool ReturnVsync(DisplayId display_id,
                           intptr_t baton,
                           fml::TimePoint frame_start_time,
                           fml::TimePoint frame_target_time,
                           std::optional<uint64_t> frame_opportunity_id) {
    return false;
  }

  // Consumes the exact pending baton without firing the frame callback. The
  // completion runs only after the UI-side cancellation callback has settled
  // its animator state.
  virtual bool CancelVsync(DisplayId display_id,
                           intptr_t baton,
                           CancellationReason reason,
                           fml::closure completion) {
    return false;
  }

  // Cancels an opportunity whose baton was already returned but whose UI
  // callback has not yet been delivered. Once delivery wins, the embedder
  // settles the opportunity through Animator instead.
  virtual bool CancelFrameOpportunity(DisplayId display_id,
                                      FrameOpportunityId opportunity_id,
                                      CancellationReason reason,
                                      fml::closure completion) {
    return false;
  }

 protected:
  // On some backends, the |FireCallback| needs to be made from a static C
  // method.
  friend class VsyncWaiterAndroid;
  friend class VsyncWaiterEmbedder;

  const TaskRunners task_runners_;

  explicit VsyncWaiter(const TaskRunners& task_runners);

  // There are two distinct situations where VsyncWaiter wishes to awaken at
  // the next vsync. Although the functionality can be the same, the intent is
  // different, therefore it makes sense to have a method for each intent.

  // The intent of AwaitVSync() is that the Animator wishes to produce a frame.
  // The underlying implementation can choose to be aware of this intent when
  // it comes to implementing backpressure and other scheduling invariants.
  //
  // Implementations are meant to override this method and arm their vsync
  // latches when in response to this invocation. On vsync, they are meant to
  // invoke the |FireCallback| method once (and only once) with the appropriate
  // arguments. This method should not block the current thread.
  //
  // Per-display variant: Platforms that support per-display vsync should
  // override this to arm the latch for the specific display. The default
  // implementation delegates to the parameterless AwaitVSync().
  virtual void AwaitVSync(DisplayId display_id);
  virtual void AwaitVSync() = 0;

  // The intent of AwaitVSyncForSecondaryCallback() is simply to wake up at the
  // next vsync.
  //
  // Because there is no association with frame scheduling, underlying
  // implementations do not need to worry about maintaining invariants or
  // backpressure. The default implementation is to simply follow the same logic
  // as AwaitVSync().
  virtual void AwaitVSyncForSecondaryCallback() { AwaitVSync(); }

  // Schedules the callback on the UI task runner. Needs to be invoked as close
  // to the `frame_start_time` as possible.
  void FireCallback(
      fml::TimePoint frame_start_time,
      fml::TimePoint frame_target_time,
      bool pause_secondary_tasks = true,
      std::optional<uint64_t> frame_opportunity_id = std::nullopt);

  // Per-display variant: fires the pending callback for a specific display.
  // Only the callback for the given `display_id` is invoked; callbacks for
  // other displays remain pending. If no callback is pending for this
  // display, the call is a no-op.
  void FireCallback(
      DisplayId display_id,
      fml::TimePoint frame_start_time,
      fml::TimePoint frame_target_time,
      bool pause_secondary_tasks = true,
      std::optional<uint64_t> frame_opportunity_id = std::nullopt);

  bool CancelCallback(DisplayId display_id,
                      CancellationReason reason,
                      fml::closure completion);

 private:
  std::mutex callback_mutex_;
  Callback callback_;
  CancellationCallback cancellation_callback_;
  // Per-display pending callbacks. Each display can have at most one
  // pending callback at a time. The default display's callback is stored
  // in `callback_` for backward compatibility; per-display callbacks are
  // stored here.
  std::unordered_map<DisplayId, Callback> display_callbacks_;
  std::unordered_map<DisplayId, CancellationCallback>
      display_cancellation_callbacks_;
  std::unordered_map<uintptr_t, fml::closure> secondary_callbacks_;

  void PauseDartEventLoopTasks();
  static void ResumeDartEventLoopTasks(fml::TaskQueueId ui_task_queue_id);

  FML_DISALLOW_COPY_AND_ASSIGN(VsyncWaiter);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_VSYNC_WAITER_H_
