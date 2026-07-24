// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include "flutter/shell/common/animator.h"

#include <functional>
#include <future>
#include <memory>

#include "flutter/shell/common/shell_test.h"
#include "flutter/shell/common/shell_test_platform_view.h"
#include "flutter/testing/post_task_sync.h"
#include "flutter/testing/testing.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

// CREATE_NATIVE_ENTRY is leaky by design
// NOLINTBEGIN(clang-analyzer-core.StackAddressEscape)

namespace flutter {
namespace testing {

constexpr int64_t kImplicitViewId = 0;

class FakeAnimatorDelegate : public Animator::Delegate {
 public:
  MOCK_METHOD(void,
              OnAnimatorBeginFrame,
              (fml::TimePoint frame_target_time, uint64_t frame_number),
              (override));

  void OnAnimatorNotifyIdle(fml::TimeDelta deadline) override {
    notify_idle_called_ = true;
  }

  MOCK_METHOD(void,
              OnAnimatorUpdateLatestFrameTargetTime,
              (fml::TimePoint frame_target_time),
              (override));

  MOCK_METHOD(void,
              OnAnimatorDraw,
              (std::shared_ptr<FramePipeline> pipeline),
              (override));

  void OnAnimatorDrawLastLayerTrees(
      std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder) override {}

  bool notify_idle_called_ = false;
};

class ManualDisplayVsyncWaiter final : public VsyncWaiter {
 public:
  explicit ManualDisplayVsyncWaiter(const TaskRunners& task_runners)
      : VsyncWaiter(task_runners) {}

  void FireDisplay(DisplayId display_id) {
    const auto now = fml::TimePoint::Now();
    FireCallback(display_id, now, now);
  }

 protected:
  void AwaitVSync() override {}
};

TEST_F(ShellTest, VSyncTargetTime) {
  // Add native callbacks to listen for window.onBeginFrame
  int64_t target_time;
  fml::AutoResetWaitableEvent on_target_time_latch;
  auto nativeOnBeginFrame = [&on_target_time_latch,
                             &target_time](Dart_NativeArguments args) {
    Dart_Handle exception = nullptr;
    target_time =
        tonic::DartConverter<int64_t>::FromArguments(args, 0, exception);
    on_target_time_latch.Signal();
  };
  AddNativeCallback("NativeOnBeginFrame",
                    CREATE_NATIVE_ENTRY(nativeOnBeginFrame));

  // Create all te prerequisites for a shell.
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  auto settings = CreateSettingsForFixture();

  std::unique_ptr<Shell> shell;

  TaskRunners task_runners = GetTaskRunnersForFixture();
  // this is not used as we are not using simulated events.
  const auto vsync_clock = std::make_shared<ShellTestVsyncClock>();
  CreateVsyncWaiter create_vsync_waiter = [&]() {
    return static_cast<std::unique_ptr<VsyncWaiter>>(
        std::make_unique<ConstantFiringVsyncWaiter>(task_runners));
  };

  // create a shell with a constant firing vsync waiter.
  auto platform_task = std::async(std::launch::async, [&]() {
    fml::MessageLoop::EnsureInitializedForCurrentThread();

    shell = Shell::Create(
        flutter::PlatformData(), task_runners, settings,
        [vsync_clock, &create_vsync_waiter](Shell& shell) {
          return ShellTestPlatformView::Create(
              ShellTestPlatformView::DefaultBackendType(), shell,
              shell.GetTaskRunners(), vsync_clock, create_vsync_waiter, nullptr,
              shell.GetIsGpuDisabledSyncSwitch());
        },
        [](Shell& shell) { return std::make_unique<Rasterizer>(shell); });
    ASSERT_TRUE(DartVMRef::IsInstanceRunning());

    auto configuration = RunConfiguration::InferFromSettings(settings);
    ASSERT_TRUE(configuration.IsValid());
    configuration.SetEntrypoint("onBeginFrameMain");

    RunEngine(shell.get(), std::move(configuration));
  });
  platform_task.wait();
  on_target_time_latch.Wait();
  const auto vsync_waiter_target_time =
      ConstantFiringVsyncWaiter::kFrameTargetTime;
  ASSERT_EQ(vsync_waiter_target_time.ToEpochDelta().ToMicroseconds(),
            target_time);

  // validate that the latest target time has also been updated.
  ASSERT_EQ(GetLatestFrameTargetTime(shell.get()), vsync_waiter_target_time);

  // teardown.
  DestroyShell(std::move(shell), task_runners);
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, AnimatorDoesNotNotifyIdleBeforeRender) {
  FakeAnimatorDelegate delegate;
  TaskRunners task_runners = {
      "test",
      CreateNewThread(),  // platform
      CreateNewThread(),  // raster
      CreateNewThread(),  // ui
      CreateNewThread()   // io
  };

  auto clock = std::make_shared<ShellTestVsyncClock>();
  fml::AutoResetWaitableEvent latch;
  std::shared_ptr<Animator> animator;

  auto flush_vsync_task = [&] {
    fml::AutoResetWaitableEvent ui_latch;
    task_runners.GetUITaskRunner()->PostTask([&] { ui_latch.Signal(); });
    do {
      clock->SimulateVSync();
    } while (ui_latch.WaitWithTimeout(fml::TimeDelta::FromMilliseconds(1)));
    latch.Signal();
  };

  // Create the animator on the UI task runner.
  task_runners.GetUITaskRunner()->PostTask([&] {
    auto vsync_waiter = static_cast<std::unique_ptr<VsyncWaiter>>(
        std::make_unique<ShellTestVsyncWaiter>(task_runners, clock));
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(vsync_waiter));
    latch.Signal();
  });
  latch.Wait();

  // Validate it has not notified idle and start it. This will request a frame.
  task_runners.GetUITaskRunner()->PostTask([&] {
    ASSERT_FALSE(delegate.notify_idle_called_);
    // Immediately request a frame saying it can reuse the last layer tree to
    // avoid more calls to BeginFrame by the animator.
    animator->RequestFrame(false);
    task_runners.GetPlatformTaskRunner()->PostTask(flush_vsync_task);
  });
  latch.Wait();
  ASSERT_FALSE(delegate.notify_idle_called_);

  fml::AutoResetWaitableEvent render_latch;
  // Validate it has not notified idle and try to render.
  task_runners.GetUITaskRunner()->PostDelayedTask(
      [&] {
        ASSERT_FALSE(delegate.notify_idle_called_);
        EXPECT_CALL(delegate, OnAnimatorBeginFrame).WillOnce([&] {
          auto layer_tree =
              std::make_unique<LayerTree>(nullptr, DlISize(600, 800));
          animator->Render(kImplicitViewId, std::move(layer_tree), 1.0);
          render_latch.Signal();
        });
        // Request a frame that builds a layer tree and renders a frame.
        // When the frame is rendered, render_latch will be signaled.
        animator->RequestFrame(true);
        task_runners.GetPlatformTaskRunner()->PostTask(flush_vsync_task);
      },
      // See kNotifyIdleTaskWaitTime in animator.cc.
      fml::TimeDelta::FromMilliseconds(60));
  latch.Wait();
  render_latch.Wait();

  // A frame has been rendered, and the next frame request will notify idle.
  // But at the moment there isn't another frame request, therefore it still
  // hasn't notified idle.
  task_runners.GetUITaskRunner()->PostTask([&] {
    ASSERT_FALSE(delegate.notify_idle_called_);
    // False to avoid getting cals to BeginFrame that will request more frames
    // before we are ready.
    animator->RequestFrame(false);
    task_runners.GetPlatformTaskRunner()->PostTask(flush_vsync_task);
  });
  latch.Wait();

  // Now it should notify idle. Make sure it is destroyed on the UI thread.
  ASSERT_TRUE(delegate.notify_idle_called_);

  task_runners.GetPlatformTaskRunner()->PostTask(flush_vsync_task);
  latch.Wait();

  task_runners.GetUITaskRunner()->PostTask([&] {
    animator.reset();
    latch.Signal();
  });
  latch.Wait();
}

TEST_F(ShellTest, AnimatorDoesNotNotifyDelegateIfPipelineIsNotEmpty) {
  FakeAnimatorDelegate delegate;
  TaskRunners task_runners = {
      "test",
      CreateNewThread(),  // platform
      CreateNewThread(),  // raster
      CreateNewThread(),  // ui
      CreateNewThread()   // io
  };

  auto clock = std::make_shared<ShellTestVsyncClock>();
  std::shared_ptr<Animator> animator;

  auto flush_vsync_task = [&] {
    fml::AutoResetWaitableEvent ui_latch;
    task_runners.GetUITaskRunner()->PostTask([&] { ui_latch.Signal(); });
    do {
      clock->SimulateVSync();
    } while (ui_latch.WaitWithTimeout(fml::TimeDelta::FromMilliseconds(1)));
  };

  // Create the animator on the UI task runner.
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto vsync_waiter = static_cast<std::unique_ptr<VsyncWaiter>>(
        std::make_unique<ShellTestVsyncWaiter>(task_runners, clock));
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(vsync_waiter));
  });

  fml::AutoResetWaitableEvent begin_frame_latch;
  // It must always be called when the method 'Animator::Render' is called,
  // regardless of whether the pipeline is empty or not.
  EXPECT_CALL(delegate, OnAnimatorUpdateLatestFrameTargetTime).Times(2);
  // It will only be called once even though we call the method
  // 'Animator::Render' twice. because it will only be called when the pipeline
  // is empty.
  EXPECT_CALL(delegate, OnAnimatorDraw).Times(1);

  for (int i = 0; i < 2; i++) {
    task_runners.GetUITaskRunner()->PostTask([&] {
      EXPECT_CALL(delegate, OnAnimatorBeginFrame).WillOnce([&] {
        auto layer_tree =
            std::make_unique<LayerTree>(nullptr, DlISize(600, 800));
        animator->Render(kImplicitViewId, std::move(layer_tree), 1.0);
        begin_frame_latch.Signal();
      });
      animator->RequestFrame();
      task_runners.GetPlatformTaskRunner()->PostTask(flush_vsync_task);
    });
    begin_frame_latch.Wait();
  }

  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

TEST_F(ShellTest,
       EmptyFramesOnMultipleDisplaysShareOnePipelineReservation) {
  class DisplayDelegate final : public FakeAnimatorDelegate {
   public:
    MOCK_METHOD(void,
                OnAnimatorBeginFrameForDisplay,
                (fml::TimePoint frame_target_time,
                 uint64_t frame_number,
                 int64_t display_id,
                 const std::set<int64_t>& view_ids),
                (override));
    MOCK_METHOD(void,
                OnAnimatorEmptyFrameForDisplay,
                (int64_t display_id, const std::set<int64_t>& view_ids),
                (override));
  } delegate;

  TaskRunners task_runners = {
      "test",
      CreateNewThread(),  // platform
      CreateNewThread(),  // raster
      CreateNewThread(),  // ui
      CreateNewThread()   // io
  };

  std::shared_ptr<Animator> animator;
  ManualDisplayVsyncWaiter* waiter = nullptr;
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto owned_waiter =
        std::make_unique<ManualDisplayVsyncWaiter>(task_runners);
    waiter = owned_waiter.get();
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(owned_waiter));
    for (int64_t display_id = 1; display_id <= 3; display_id++) {
      animator->AddDisplay(display_id, 60.0);
      animator->SetViewDisplay(100 + display_id, display_id);
    }
  });

  EXPECT_CALL(delegate, OnAnimatorEmptyFrameForDisplay(1, ::testing::_));
  EXPECT_CALL(delegate, OnAnimatorEmptyFrameForDisplay(2, ::testing::_));
  EXPECT_CALL(delegate, OnAnimatorUpdateLatestFrameTargetTime(::testing::_));
  EXPECT_CALL(delegate, OnAnimatorDraw(::testing::_)).Times(1);

  int begin_count = 0;
  EXPECT_CALL(delegate,
              OnAnimatorBeginFrameForDisplay(::testing::_, ::testing::_,
                                             ::testing::_, ::testing::_))
      .Times(3)
      .WillRepeatedly([&](fml::TimePoint, uint64_t, int64_t display_id,
                          const std::set<int64_t>&) {
        begin_count++;
        if (display_id == 3) {
          auto layer_tree =
              std::make_unique<LayerTree>(nullptr, DlISize(600, 800));
          animator->Render(103, std::move(layer_tree), 1.0);
        }
      });

  for (int64_t display_id = 1; display_id <= 3; display_id++) {
    PostTaskSync(task_runners.GetUITaskRunner(),
                 [waiter, display_id] { waiter->FireDisplay(display_id); });
    PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  }

  EXPECT_EQ(begin_count, 3);
  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

}  // namespace testing
}  // namespace flutter

// NOLINTEND(clang-analyzer-core.StackAddressEscape)
