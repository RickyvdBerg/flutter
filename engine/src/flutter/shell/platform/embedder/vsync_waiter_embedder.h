// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_VSYNC_WAITER_EMBEDDER_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_VSYNC_WAITER_EMBEDDER_H_

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

  static bool OnEmbedderVsync(const flutter::TaskRunners& task_runners,
                              intptr_t baton,
                              fml::TimePoint frame_start_time,
                              fml::TimePoint frame_target_time);

  /// Per-display variant: fires the callback for a specific display.
  static bool OnEmbedderVsyncForDisplay(
      const flutter::TaskRunners& task_runners,
      intptr_t baton,
      DisplayId display_id,
      fml::TimePoint frame_start_time,
      fml::TimePoint frame_target_time);

 private:
  const VsyncCallback vsync_callback_;
  const VsyncForDisplayCallback vsync_for_display_callback_;

  // |VsyncWaiter|
  void AwaitVSync() override;

  // |VsyncWaiter| Per-display override.
  void AwaitVSync(DisplayId display_id) override;

  FML_DISALLOW_COPY_AND_ASSIGN(VsyncWaiterEmbedder);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_VSYNC_WAITER_EMBEDDER_H_
