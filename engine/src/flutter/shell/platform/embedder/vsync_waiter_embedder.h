// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_VSYNC_WAITER_EMBEDDER_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_VSYNC_WAITER_EMBEDDER_H_

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "flutter/fml/macros.h"
#include "flutter/shell/common/vsync_waiter.h"

namespace flutter {

class VsyncWaiterEmbedder final : public VsyncWaiter {
 public:
  using VsyncCallback = std::function<void(intptr_t)>;
  using VsyncForDisplayCallback = std::function<void(intptr_t, DisplayId)>;

  /// Constructs a VsyncWaiterEmbedder with optional per-display support.
  ///
  /// If `vsync_for_display_callback` is non-null, per-display vsync is
  /// enabled: AwaitVSync(display_id) calls the per-display callback with
  /// the display ID. Otherwise, falls back to the legacy single-display
  /// `vsync_callback`.
  VsyncWaiterEmbedder(const VsyncCallback& vsync_callback,
                      const VsyncForDisplayCallback& vsync_for_display_callback,
                      const flutter::TaskRunners& task_runners);

  /// Legacy constructor for backward compatibility.
  VsyncWaiterEmbedder(const VsyncCallback& callback,
                      const flutter::TaskRunners& task_runners);

  ~VsyncWaiterEmbedder() override;

  // |VsyncWaiter|
  bool ReturnVsync(DisplayId display_id,
                   intptr_t baton,
                   fml::TimePoint frame_start_time,
                   fml::TimePoint frame_target_time,
                   std::optional<uint64_t> frame_opportunity_id) override;

  // |VsyncWaiter|
  bool CancelVsync(DisplayId display_id,
                   intptr_t baton,
                   CancellationReason reason,
                   fml::closure completion) override;

  // |VsyncWaiter|
  bool CancelFrameOpportunity(DisplayId display_id,
                              FrameOpportunityId opportunity_id,
                              CancellationReason reason,
                              fml::closure completion) override;

 private:
  const VsyncCallback vsync_callback_;
  const VsyncForDisplayCallback vsync_for_display_callback_;

  intptr_t RegisterBaton(DisplayId display_id);
  bool TakeBaton(DisplayId display_id, intptr_t baton);
  bool TakeBatonForOpportunity(DisplayId display_id,
                               intptr_t baton,
                               FrameOpportunityId opportunity_id);
  bool TakeReturnedOpportunity(DisplayId display_id,
                               FrameOpportunityId opportunity_id);

  std::mutex baton_mutex_;
  uint64_t next_baton_ = 1;
  std::unordered_map<DisplayId, intptr_t> batons_by_display_;
  std::unordered_map<intptr_t, DisplayId> displays_by_baton_;
  std::unordered_map<DisplayId, FrameOpportunityId>
      returned_opportunities_by_display_;

  // |VsyncWaiter|
  void AwaitVSync() override;

  // |VsyncWaiter| Per-display override.
  void AwaitVSync(DisplayId display_id) override;

  FML_DISALLOW_COPY_AND_ASSIGN(VsyncWaiterEmbedder);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_VSYNC_WAITER_EMBEDDER_H_
