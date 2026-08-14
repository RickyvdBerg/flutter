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

  void FireDisplay(DisplayId display_id,
                   std::optional<uint64_t> opportunity_id = std::nullopt,
                   std::set<int64_t> opportunity_target_ids = {}) {
    const auto now = fml::TimePoint::Now();
    FireCallback(display_id, now, now, /*pause_secondary_tasks=*/true,
                 opportunity_id, std::move(opportunity_target_ids));
  }

  size_t request_count() const { return request_count_; }

  // |VsyncWaiter|
  bool SupportsPerDisplayVsync() const override { return true; }

 protected:
  void AwaitVSync() override { request_count_++; }
  void AwaitVSync(DisplayId) override { request_count_++; }

 private:
  size_t request_count_ = 0;
};

/// A waiter with no per-display support, matching every stock platform waiter
/// (the timer fallback, Android, and an embedder that supplied only the
/// legacy vsync callback).
class GlobalOnlyVsyncWaiter final : public VsyncWaiter {
 public:
  explicit GlobalOnlyVsyncWaiter(const TaskRunners& task_runners)
      : VsyncWaiter(task_runners) {}

  void Fire() {
    const auto now = fml::TimePoint::Now();
    FireCallback(now, now, /*pause_secondary_tasks=*/true);
  }

  size_t request_count() const { return request_count_; }

 protected:
  void AwaitVSync() override { request_count_++; }

 private:
  size_t request_count_ = 0;
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

TEST_F(ShellTest, EmptyFramesOnMultipleDisplaysShareOnePipelineReservation) {
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

TEST_F(ShellTest, RemovedTargetTerminatesExactFrameOpportunity) {
  class ExactDisplayDelegate final : public FakeAnimatorDelegate {
   public:
    MOCK_METHOD(void,
                OnAnimatorEmptyFrameForDisplay,
                (int64_t display_id, const std::set<int64_t>& view_ids),
                (override));
    MOCK_METHOD(bool,
                OnAnimatorFrameOpportunityOutcome,
                (FrameOpportunityId opportunity_id,
                 int64_t display_id,
                 int64_t target_id,
                 FrameOpportunityOutcome outcome),
                (override));
  } delegate;

  TaskRunners task_runners = {"test", CreateNewThread(), CreateNewThread(),
                              CreateNewThread(), CreateNewThread()};
  std::shared_ptr<Animator> animator;
  ManualDisplayVsyncWaiter* waiter = nullptr;
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto owned_waiter =
        std::make_unique<ManualDisplayVsyncWaiter>(task_runners);
    waiter = owned_waiter.get();
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(owned_waiter));
    animator->AddDisplay(7, 60.0);
    animator->SetViewDisplay(101, 7);
    animator->RemoveView(101);
  });

  EXPECT_CALL(delegate,
              OnAnimatorFrameOpportunityOutcome(
                  55, 7, 101, FrameOpportunityOutcome::kTargetRemoved))
      .WillOnce(::testing::Return(true));
  EXPECT_CALL(delegate, OnAnimatorEmptyFrameForDisplay(7, ::testing::_));
  PostTaskSync(task_runners.GetUITaskRunner(),
               [waiter] { waiter->FireDisplay(7, 55, {101}); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

TEST_F(ShellTest, HiddenViewSettlesAdmittedWorkAndRearmsOnlyOnResume) {
  class VisibilityDelegate final : public FakeAnimatorDelegate {
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
    MOCK_METHOD(bool,
                OnAnimatorFrameOpportunityOutcome,
                (FrameOpportunityId opportunity_id,
                 int64_t display_id,
                 int64_t target_id,
                 FrameOpportunityOutcome outcome),
                (override));
  } delegate;

  TaskRunners task_runners = {"test", CreateNewThread(), CreateNewThread(),
                              CreateNewThread(), CreateNewThread()};
  std::shared_ptr<Animator> animator;
  ManualDisplayVsyncWaiter* waiter = nullptr;
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto owned_waiter =
        std::make_unique<ManualDisplayVsyncWaiter>(task_runners);
    waiter = owned_waiter.get();
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(owned_waiter));
    animator->AddDisplay(21, 60.0);
    animator->SetViewDisplay(121, 21);
    EXPECT_TRUE(
        animator->SetViewVisibility(121, Animator::ViewVisibility::kObscured));
    EXPECT_FALSE(
        animator->SetViewVisibility(121, Animator::ViewVisibility::kSuspended));
  });
  ASSERT_EQ(waiter->request_count(), 1u);

  EXPECT_CALL(delegate, OnAnimatorBeginFrameForDisplay).Times(0);
  EXPECT_CALL(delegate,
              OnAnimatorFrameOpportunityOutcome(
                  901, 21, 121, FrameOpportunityOutcome::kNoVisualChange))
      .WillOnce(::testing::Return(true));
  EXPECT_CALL(delegate,
              OnAnimatorEmptyFrameForDisplay(21, std::set<int64_t>({121})));
  PostTaskSync(task_runners.GetUITaskRunner(),
               [waiter] { waiter->FireDisplay(21, 901, {121}); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  ::testing::Mock::VerifyAndClearExpectations(&delegate);

  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    EXPECT_FALSE(animator->RequestFrameForDisplayViews(21, {121}));
  });
  EXPECT_EQ(waiter->request_count(), 1u);

  EXPECT_CALL(delegate,
              OnAnimatorBeginFrameForDisplay(::testing::_, ::testing::_, 21,
                                             std::set<int64_t>({121})));
  EXPECT_CALL(delegate,
              OnAnimatorFrameOpportunityOutcome(
                  902, 21, 121, FrameOpportunityOutcome::kNoVisualChange))
      .WillOnce(::testing::Return(true));
  EXPECT_CALL(delegate,
              OnAnimatorEmptyFrameForDisplay(21, std::set<int64_t>({121})));
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    EXPECT_FALSE(
        animator->SetViewVisibility(121, Animator::ViewVisibility::kVisible));
  });
  EXPECT_EQ(waiter->request_count(), 2u);
  PostTaskSync(task_runners.GetUITaskRunner(),
               [waiter] { waiter->FireDisplay(21, 902, {121}); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

TEST_F(ShellTest, RemovingUnrequestedTargetDoesNotInventAnOutcome) {
  class ExactDisplayDelegate final : public FakeAnimatorDelegate {
   public:
    MOCK_METHOD(bool,
                OnAnimatorFrameOpportunityOutcome,
                (FrameOpportunityId opportunity_id,
                 int64_t display_id,
                 int64_t target_id,
                 FrameOpportunityOutcome outcome),
                (override));
  } delegate;

  TaskRunners task_runners = {"test", CreateNewThread(), CreateNewThread(),
                              CreateNewThread(), CreateNewThread()};
  std::shared_ptr<Animator> animator;
  ManualDisplayVsyncWaiter* waiter = nullptr;
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto owned_waiter =
        std::make_unique<ManualDisplayVsyncWaiter>(task_runners);
    waiter = owned_waiter.get();
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(owned_waiter));
    animator->AddDisplay(14, 60.0);
    animator->SetViewDisplay(114, 14);
    animator->SetViewDisplay(214, 14);
  });

  // Drain the all-target request created while assigning the two views.
  EXPECT_CALL(delegate, OnAnimatorBeginFrame(::testing::_, ::testing::_))
      .Times(2);
  PostTaskSync(task_runners.GetUITaskRunner(),
               [waiter] { waiter->FireDisplay(14); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});

  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    EXPECT_TRUE(animator->RequestFrameForDisplayViews(14, {114}));
    animator->RemoveView(214);
  });
  EXPECT_CALL(delegate,
              OnAnimatorFrameOpportunityOutcome(
                  88, 14, 114, FrameOpportunityOutcome::kNoVisualChange))
      .WillOnce(::testing::Return(true));
  EXPECT_CALL(delegate,
              OnAnimatorFrameOpportunityOutcome(88, 14, 214, ::testing::_))
      .Times(0);
  PostTaskSync(task_runners.GetUITaskRunner(),
               [waiter] { waiter->FireDisplay(14, 88, {114}); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

TEST_F(ShellTest, ExactOpportunityReconcilesEveryAdmittedTarget) {
  class ExactDisplayDelegate final : public FakeAnimatorDelegate {
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
    MOCK_METHOD(bool,
                OnAnimatorFrameOpportunityOutcome,
                (FrameOpportunityId opportunity_id,
                 int64_t display_id,
                 int64_t target_id,
                 FrameOpportunityOutcome outcome),
                (override));
  } delegate;

  TaskRunners task_runners = {"test", CreateNewThread(), CreateNewThread(),
                              CreateNewThread(), CreateNewThread()};
  std::shared_ptr<Animator> animator;
  ManualDisplayVsyncWaiter* waiter = nullptr;
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto owned_waiter =
        std::make_unique<ManualDisplayVsyncWaiter>(task_runners);
    waiter = owned_waiter.get();
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(owned_waiter));
    animator->AddDisplay(14, 60.0);
    animator->SetViewDisplay(114, 14);
    animator->SetViewDisplay(214, 14);
    animator->SetViewDisplay(314, 14);
  });

  EXPECT_CALL(delegate, OnAnimatorBeginFrameForDisplay(
                            ::testing::_, ::testing::_, 14,
                            std::set<int64_t>({114, 214, 314})));
  EXPECT_CALL(delegate, OnAnimatorEmptyFrameForDisplay(
                            14, std::set<int64_t>({114, 214, 314})));
  PostTaskSync(task_runners.GetUITaskRunner(),
               [waiter] { waiter->FireDisplay(14); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  ::testing::Mock::VerifyAndClearExpectations(&delegate);

  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    EXPECT_TRUE(animator->RequestFrameForDisplayViews(14, {114, 214}));
  });
  EXPECT_CALL(delegate,
              OnAnimatorBeginFrameForDisplay(::testing::_, ::testing::_, 14,
                                             std::set<int64_t>({114})));
  EXPECT_CALL(delegate,
              OnAnimatorFrameOpportunityOutcome(
                  600, 14, 114, FrameOpportunityOutcome::kNoVisualChange))
      .WillOnce(::testing::Return(true));
  EXPECT_CALL(delegate,
              OnAnimatorFrameOpportunityOutcome(
                  600, 14, 314, FrameOpportunityOutcome::kNoVisualChange))
      .WillOnce(::testing::Return(true));
  EXPECT_CALL(delegate,
              OnAnimatorFrameOpportunityOutcome(
                  600, 14, 414, FrameOpportunityOutcome::kTargetRemoved))
      .WillOnce(::testing::Return(true));
  EXPECT_CALL(delegate,
              OnAnimatorFrameOpportunityOutcome(600, 14, 214, ::testing::_))
      .Times(0);
  EXPECT_CALL(delegate, OnAnimatorEmptyFrameForDisplay(
                            14, std::set<int64_t>({314, 414})));
  EXPECT_CALL(delegate,
              OnAnimatorEmptyFrameForDisplay(14, std::set<int64_t>({114})));
  PostTaskSync(task_runners.GetUITaskRunner(),
               [waiter] { waiter->FireDisplay(14, 600, {114, 314, 414}); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

TEST_F(ShellTest, DisplayRemovalTerminatesOnlyPendingTargets) {
  class ExactDisplayDelegate final : public FakeAnimatorDelegate {
   public:
    MOCK_METHOD(bool,
                OnAnimatorFrameOpportunityOutcome,
                (FrameOpportunityId opportunity_id,
                 int64_t display_id,
                 int64_t target_id,
                 FrameOpportunityOutcome outcome),
                (override));
  } delegate;

  TaskRunners task_runners = {"test", CreateNewThread(), CreateNewThread(),
                              CreateNewThread(), CreateNewThread()};
  std::shared_ptr<Animator> animator;
  ManualDisplayVsyncWaiter* waiter = nullptr;
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto owned_waiter =
        std::make_unique<ManualDisplayVsyncWaiter>(task_runners);
    waiter = owned_waiter.get();
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(owned_waiter));
    animator->AddDisplay(15, 60.0);
    animator->SetViewDisplay(115, 15);
    animator->SetViewDisplay(215, 15);
  });

  EXPECT_CALL(delegate, OnAnimatorBeginFrame(::testing::_, ::testing::_));
  PostTaskSync(task_runners.GetUITaskRunner(),
               [waiter] { waiter->FireDisplay(15); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    EXPECT_TRUE(animator->RequestFrameForDisplayViews(15, {115}));
    animator->RemoveDisplay(15);
  });

  EXPECT_CALL(delegate,
              OnAnimatorFrameOpportunityOutcome(
                  99, 15, 115, FrameOpportunityOutcome::kTargetRemoved))
      .WillOnce(::testing::Return(true));
  EXPECT_CALL(delegate,
              OnAnimatorFrameOpportunityOutcome(99, 15, 215, ::testing::_))
      .Times(0);
  PostTaskSync(task_runners.GetUITaskRunner(),
               [waiter] { waiter->FireDisplay(15, 99, {115}); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

TEST_F(ShellTest, RetainedDisplayFrameReachesRasterAtBuildEnd) {
  class RetainedDisplayDelegate final : public FakeAnimatorDelegate {
   public:
    Animator* animator = nullptr;
    bool retained_frame_seen = false;

    void OnAnimatorBeginFrameForDisplay(fml::TimePoint,
                                        uint64_t,
                                        int64_t,
                                        const std::set<int64_t>&) override {
      auto layer_tree = std::make_unique<LayerTree>(nullptr, DlISize(600, 800));
      animator->Render(116, std::move(layer_tree), 1.0);
    }

    void OnAnimatorDrawLastLayerTreesForDisplay(
        std::unique_ptr<FrameTimingsRecorder> recorder,
        const std::set<int64_t>& view_ids) override {
      recorder->AssertInState(FrameTimingsRecorder::State::kBuildEnd);
      EXPECT_EQ(view_ids, std::set<int64_t>({116}));
      retained_frame_seen = true;
    }
  } delegate;

  TaskRunners task_runners = {"test", CreateNewThread(), CreateNewThread(),
                              CreateNewThread(), CreateNewThread()};
  std::shared_ptr<Animator> animator;
  ManualDisplayVsyncWaiter* waiter = nullptr;
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto owned_waiter =
        std::make_unique<ManualDisplayVsyncWaiter>(task_runners);
    waiter = owned_waiter.get();
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(owned_waiter));
    delegate.animator = animator.get();
    animator->AddDisplay(16, 60.0);
    animator->SetViewDisplay(116, 16);
  });

  EXPECT_CALL(delegate, OnAnimatorUpdateLatestFrameTargetTime(::testing::_));
  EXPECT_CALL(delegate, OnAnimatorDraw(::testing::_));
  PostTaskSync(task_runners.GetUITaskRunner(),
               [waiter] { waiter->FireDisplay(16); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    EXPECT_TRUE(animator->RequestFrameForDisplayViews(
        16, {116}, /*regenerate_layer_trees=*/false));
  });
  PostTaskSync(task_runners.GetUITaskRunner(),
               [waiter] { waiter->FireDisplay(16, 100, {116}); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});

  EXPECT_TRUE(delegate.retained_frame_seen);
  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

TEST_F(ShellTest, PerDisplayTraceCleanupDoesNotMintAPlatformBaton) {
  FakeAnimatorDelegate delegate;
  TaskRunners task_runners = {"test", CreateNewThread(), CreateNewThread(),
                              CreateNewThread(), CreateNewThread()};
  std::shared_ptr<Animator> animator;
  ManualDisplayVsyncWaiter* waiter = nullptr;
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto owned_waiter =
        std::make_unique<ManualDisplayVsyncWaiter>(task_runners);
    waiter = owned_waiter.get();
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(owned_waiter));
    animator->AddDisplay(17, 60.0);
  });

  EXPECT_EQ(waiter->request_count(), 0u);
  PostTaskSync(task_runners.GetUITaskRunner(),
               [&] { animator->EnqueueTraceFlowId(170); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  EXPECT_EQ(waiter->request_count(), 0u);
  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

TEST_F(ShellTest, CancelledReturnedOpportunityCannotRunItsFrameTurn) {
  class CancellationDelegate final : public FakeAnimatorDelegate {
   public:
    MOCK_METHOD(void,
                OnAnimatorBeginFrameForDisplay,
                (fml::TimePoint frame_target_time,
                 uint64_t frame_number,
                 int64_t display_id,
                 const std::set<int64_t>& view_ids),
                (override));
  } delegate;

  TaskRunners task_runners = {"test", CreateNewThread(), CreateNewThread(),
                              CreateNewThread(), CreateNewThread()};
  std::shared_ptr<Animator> animator;
  ManualDisplayVsyncWaiter* waiter = nullptr;
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto owned_waiter =
        std::make_unique<ManualDisplayVsyncWaiter>(task_runners);
    waiter = owned_waiter.get();
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(owned_waiter));
    animator->AddDisplay(18, 60.0);
    animator->SetViewDisplay(118, 18);
    animator->CancelFrameOpportunity(
        18, 500, VsyncWaiter::CancellationReason::kHostTerminated);
  });

  EXPECT_CALL(delegate, OnAnimatorBeginFrameForDisplay(
                            ::testing::_, ::testing::_, 18, ::testing::_))
      .Times(0);
  PostTaskSync(task_runners.GetUITaskRunner(),
               [waiter] { waiter->FireDisplay(18, 500, {118}); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  ::testing::Mock::VerifyAndClearExpectations(&delegate);

  EXPECT_CALL(delegate, OnAnimatorBeginFrameForDisplay(
                            ::testing::_, ::testing::_, 18, ::testing::_));
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    EXPECT_TRUE(animator->RequestFrameForDisplayViews(18, {118}));
  });
  PostTaskSync(task_runners.GetUITaskRunner(),
               [waiter] { waiter->FireDisplay(18, 501, {118}); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

TEST_F(ShellTest, EmptyTargetTerminatesAsNoVisualChange) {
  class ExactDisplayDelegate final : public FakeAnimatorDelegate {
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
    MOCK_METHOD(bool,
                OnAnimatorFrameOpportunityOutcome,
                (FrameOpportunityId opportunity_id,
                 int64_t display_id,
                 int64_t target_id,
                 FrameOpportunityOutcome outcome),
                (override));
  } delegate;

  TaskRunners task_runners = {"test", CreateNewThread(), CreateNewThread(),
                              CreateNewThread(), CreateNewThread()};
  std::shared_ptr<Animator> animator;
  ManualDisplayVsyncWaiter* waiter = nullptr;
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto owned_waiter =
        std::make_unique<ManualDisplayVsyncWaiter>(task_runners);
    waiter = owned_waiter.get();
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(owned_waiter));
    animator->AddDisplay(9, 60.0);
    animator->SetViewDisplay(109, 9);
  });

  EXPECT_CALL(delegate, OnAnimatorBeginFrameForDisplay(
                            ::testing::_, ::testing::_, 9, ::testing::_));
  EXPECT_CALL(delegate,
              OnAnimatorFrameOpportunityOutcome(
                  77, 9, 109, FrameOpportunityOutcome::kNoVisualChange))
      .WillOnce(::testing::Return(true));
  EXPECT_CALL(delegate, OnAnimatorEmptyFrameForDisplay(9, ::testing::_));
  PostTaskSync(task_runners.GetUITaskRunner(),
               [waiter] { waiter->FireDisplay(9, 77, {109}); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

TEST_F(ShellTest, PipelineBackpressureTerminatesThenRearmsDemand) {
  class ExactDisplayDelegate final : public FakeAnimatorDelegate {
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
    MOCK_METHOD(bool,
                OnAnimatorFrameOpportunityOutcome,
                (FrameOpportunityId opportunity_id,
                 int64_t display_id,
                 int64_t target_id,
                 FrameOpportunityOutcome outcome),
                (override));
  } delegate;

  TaskRunners task_runners = {"test", CreateNewThread(), CreateNewThread(),
                              CreateNewThread(), CreateNewThread()};
  std::shared_ptr<Animator> animator;
  ManualDisplayVsyncWaiter* waiter = nullptr;
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto owned_waiter =
        std::make_unique<ManualDisplayVsyncWaiter>(task_runners);
    waiter = owned_waiter.get();
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(owned_waiter));
    animator->AddDisplay(12, 60.0);
    animator->SetViewDisplay(112, 12);
  });

  EXPECT_CALL(delegate, OnAnimatorBeginFrameForDisplay(
                            ::testing::_, ::testing::_, 12, ::testing::_))
      .Times(2)
      .WillRepeatedly(
          [&](fml::TimePoint, uint64_t, int64_t, const std::set<int64_t>&) {
            auto layer_tree =
                std::make_unique<LayerTree>(nullptr, DlISize(600, 800));
            animator->Render(112, std::move(layer_tree), 1.0);
          });
  EXPECT_CALL(delegate, OnAnimatorUpdateLatestFrameTargetTime(::testing::_))
      .Times(2);
  EXPECT_CALL(delegate, OnAnimatorDraw(::testing::_)).Times(2);
  EXPECT_CALL(delegate,
              OnAnimatorFrameOpportunityOutcome(
                  3, 12, 112, FrameOpportunityOutcome::kBackpressured))
      .WillOnce(::testing::Return(true));
  EXPECT_CALL(delegate, OnAnimatorEmptyFrameForDisplay(12, ::testing::_));

  for (uint64_t opportunity_id = 1; opportunity_id <= 3; opportunity_id++) {
    if (opportunity_id > 1) {
      PostTaskSync(task_runners.GetUITaskRunner(), [&] {
        EXPECT_TRUE(animator->RequestFrameForDisplayViews(12, {112}));
      });
    }
    PostTaskSync(task_runners.GetUITaskRunner(), [waiter, opportunity_id] {
      waiter->FireDisplay(12, opportunity_id, {112});
    });
    PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  }

  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

TEST_F(ShellTest, DisplayRegistrationAloneDoesNotEnterPerDisplayMode) {
  FakeAnimatorDelegate delegate;
  TaskRunners task_runners = {"test", CreateNewThread(), CreateNewThread(),
                              CreateNewThread(), CreateNewThread()};
  std::shared_ptr<Animator> animator;
  GlobalOnlyVsyncWaiter* waiter = nullptr;
  bool per_display_mode = true;
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto owned_waiter = std::make_unique<GlobalOnlyVsyncWaiter>(task_runners);
    waiter = owned_waiter.get();
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(owned_waiter));
    // Stock `FlutterEngineNotifyDisplayUpdate` reaches the animator as bare
    // display registration. It describes topology only.
    animator->AddDisplay(31, 60.0);
    per_display_mode = animator->IsPerDisplayMode();
  });
  EXPECT_FALSE(per_display_mode);

  // An embedder that never opted in keeps the global frame clock, so a
  // scheduleFrame still arms a baton and still builds a frame.
  EXPECT_CALL(delegate, OnAnimatorBeginFrame(::testing::_, ::testing::_));
  PostTaskSync(task_runners.GetUITaskRunner(),
               [&] { animator->RequestFrame(); });
  PostTaskSync(task_runners.GetUITaskRunner(),
               [&] { EXPECT_EQ(waiter->request_count(), 1u); });
  PostTaskSync(task_runners.GetUITaskRunner(), [waiter] { waiter->Fire(); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});

  // Homing a view on a display is the other opt-in signal.
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    animator->SetViewDisplay(131, 31);
    per_display_mode = animator->IsPerDisplayMode();
  });
  EXPECT_TRUE(per_display_mode);
  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

TEST_F(ShellTest, InitialViewDisplayRegistrationIsOneShotAndDemandFree) {
  FakeAnimatorDelegate delegate;
  TaskRunners task_runners = {"test", CreateNewThread(), CreateNewThread(),
                              CreateNewThread(), CreateNewThread()};
  std::shared_ptr<Animator> animator;
  ManualDisplayVsyncWaiter* waiter = nullptr;
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto owned_waiter =
        std::make_unique<ManualDisplayVsyncWaiter>(task_runners);
    waiter = owned_waiter.get();
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(owned_waiter));
    animator->AddDisplay(41, 60.0);
    animator->AddDisplay(42, 60.0);

    EXPECT_TRUE(animator->RegisterInitialViewDisplay(141, 41));
    EXPECT_EQ(waiter->request_count(), 0u);

    EXPECT_FALSE(animator->RegisterInitialViewDisplay(141, 42));
    EXPECT_EQ(waiter->request_count(), 0u);

    EXPECT_TRUE(animator->RequestFrameForDisplayViews(41, {141}));
    EXPECT_EQ(waiter->request_count(), 1u);
  });
  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

TEST_F(ShellTest, ScopedFrameRequestReportsWhetherItWasRetained) {
  FakeAnimatorDelegate delegate;
  TaskRunners task_runners = {"test", CreateNewThread(), CreateNewThread(),
                              CreateNewThread(), CreateNewThread()};
  std::shared_ptr<Animator> animator;
  ManualDisplayVsyncWaiter* waiter = nullptr;
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto owned_waiter =
        std::make_unique<ManualDisplayVsyncWaiter>(task_runners);
    waiter = owned_waiter.get();
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(owned_waiter));
    animator->AddDisplay(51, 60.0);
    EXPECT_TRUE(animator->RegisterInitialViewDisplay(151, 51));

    EXPECT_TRUE(animator->RequestFrameForDisplayViews(51, {151}));
    EXPECT_EQ(waiter->request_count(), 1u);

    EXPECT_FALSE(animator->RequestFrameForDisplayViews(51, {251}));
    EXPECT_FALSE(animator->RequestFrameForDisplayViews(99, {151}));
  });

  PostTaskSync(task_runners.GetUITaskRunner(),
               [waiter] { waiter->FireDisplay(51); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});

  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    EXPECT_TRUE(
        animator->SetViewVisibility(151, Animator::ViewVisibility::kObscured));
    EXPECT_FALSE(animator->RequestFrameForDisplayViews(51, {151}));
  });
  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

TEST_F(ShellTest, RegisteredDisplaysWithoutHomedViewsStillScheduleFrames) {
  FakeAnimatorDelegate delegate;
  TaskRunners task_runners = {"test", CreateNewThread(), CreateNewThread(),
                              CreateNewThread(), CreateNewThread()};
  std::shared_ptr<Animator> animator;
  ManualDisplayVsyncWaiter* waiter = nullptr;
  bool per_display_mode = false;
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto owned_waiter =
        std::make_unique<ManualDisplayVsyncWaiter>(task_runners);
    waiter = owned_waiter.get();
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(owned_waiter));
    animator->AddDisplay(32, 60.0);
    per_display_mode = animator->IsPerDisplayMode();
  });
  // The waiter can service display-scoped batons, so per-display mode is on
  // even though no view is homed yet.
  EXPECT_TRUE(per_display_mode);

  // No display owns a view, so the unscoped request must fall through to the
  // global path instead of being dropped by the per-display loop.
  EXPECT_CALL(delegate, OnAnimatorBeginFrame(::testing::_, ::testing::_));
  PostTaskSync(task_runners.GetUITaskRunner(),
               [&] { animator->RequestFrame(); });
  PostTaskSync(task_runners.GetUITaskRunner(),
               [&] { EXPECT_EQ(waiter->request_count(), 1u); });
  PostTaskSync(task_runners.GetUITaskRunner(), [waiter] {
    waiter->FireDisplay(VsyncWaiter::kDefaultDisplayId);
  });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

TEST_F(ShellTest, UnhomedViewsKeepTheGlobalFrameClockInPerDisplayMode) {
  class MixedDelegate final : public FakeAnimatorDelegate {
   public:
    MOCK_METHOD(void,
                OnAnimatorBeginFrameForDisplay,
                (fml::TimePoint frame_target_time,
                 uint64_t frame_number,
                 int64_t display_id,
                 const std::set<int64_t>& view_ids),
                (override));
  } delegate;

  TaskRunners task_runners = {"test", CreateNewThread(), CreateNewThread(),
                              CreateNewThread(), CreateNewThread()};
  std::shared_ptr<Animator> animator;
  ManualDisplayVsyncWaiter* waiter = nullptr;
  PostTaskSync(task_runners.GetUITaskRunner(), [&] {
    auto owned_waiter =
        std::make_unique<ManualDisplayVsyncWaiter>(task_runners);
    waiter = owned_waiter.get();
    animator = std::make_unique<Animator>(delegate, task_runners,
                                          std::move(owned_waiter));
    animator->AddDisplay(33, 60.0);
    animator->SetViewDisplay(133, 33);
    // Display 99 is not registered, so this view parks in the default state.
    animator->SetViewDisplay(233, 99);
  });

  EXPECT_CALL(delegate, OnAnimatorBeginFrameForDisplay(
                            ::testing::_, ::testing::_, 33, ::testing::_))
      .Times(::testing::AtLeast(1));
  EXPECT_CALL(delegate, OnAnimatorBeginFrame(::testing::_, ::testing::_));

  PostTaskSync(task_runners.GetUITaskRunner(),
               [&] { animator->RequestFrame(); });
  PostTaskSync(task_runners.GetUITaskRunner(),
               [waiter] { waiter->FireDisplay(33); });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  PostTaskSync(task_runners.GetUITaskRunner(), [waiter] {
    waiter->FireDisplay(VsyncWaiter::kDefaultDisplayId);
  });
  PostTaskSync(task_runners.GetUITaskRunner(), [] {});
  PostTaskSync(task_runners.GetUITaskRunner(), [&] { animator.reset(); });
}

}  // namespace testing
}  // namespace flutter

// NOLINTEND(clang-analyzer-core.StackAddressEscape)
