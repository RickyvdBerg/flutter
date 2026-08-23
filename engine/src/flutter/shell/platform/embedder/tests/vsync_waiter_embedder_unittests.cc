// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include "flutter/shell/platform/embedder/vsync_waiter_embedder.h"

#include <optional>
#include <string>
#include <vector>

#include "flutter/fml/message_loop.h"
#include "flutter/testing/testing.h"

namespace flutter {
namespace testing {

namespace {

TaskRunners CurrentThreadTaskRunners() {
  fml::MessageLoop::EnsureInitializedForCurrentThread();
  auto runner = fml::MessageLoop::GetCurrent().GetTaskRunner();
  return TaskRunners("vsync_waiter_embedder_test", runner, runner, runner,
                     runner);
}

void DrainCurrentMessageLoop() {
  // Returning a baton first schedules the exact-time task, which then posts
  // the engine callback. Keep those two custody transfers explicit in tests.
  fml::MessageLoop::GetCurrent().RunExpiredTasksNow();
  fml::MessageLoop::GetCurrent().RunExpiredTasksNow();
}

}  // namespace

TEST(VsyncWaiterEmbedderTest, BatonsAreOwnedByOneEngineInstance) {
  const auto task_runners = CurrentThreadTaskRunners();
  std::optional<intptr_t> first_baton;
  std::optional<intptr_t> second_baton;
  std::optional<FrameOpportunityContext> first_opportunity;
  std::optional<FrameOpportunityContext> second_opportunity;

  auto first = std::make_shared<VsyncWaiterEmbedder>(
      nullptr,
      [&](intptr_t baton, VsyncWaiter::DisplayId display_id) {
        EXPECT_EQ(display_id, 7);
        first_baton = baton;
      },
      task_runners);
  auto second = std::make_shared<VsyncWaiterEmbedder>(
      nullptr,
      [&](intptr_t baton, VsyncWaiter::DisplayId display_id) {
        EXPECT_EQ(display_id, 7);
        second_baton = baton;
      },
      task_runners);

  first->AsyncWaitForVsync(
      7, [&](std::unique_ptr<FrameTimingsRecorder> recorder) {
        first_opportunity = recorder->GetFrameOpportunity();
      });
  second->AsyncWaitForVsync(
      7, [&](std::unique_ptr<FrameTimingsRecorder> recorder) {
        second_opportunity = recorder->GetFrameOpportunity();
      });

  ASSERT_TRUE(first_baton.has_value());
  ASSERT_TRUE(second_baton.has_value());
  EXPECT_EQ(first_baton, second_baton);
  const auto now = fml::TimePoint::Now();
  const auto target = now + fml::TimeDelta::FromMilliseconds(1);
  EXPECT_TRUE(first->ReturnVsync(7, first_baton.value(), now, target, 101,
                                 {1001}, now));
  EXPECT_TRUE(second->ReturnVsync(7, second_baton.value(), now, target, 202,
                                  {2002}, now));
  DrainCurrentMessageLoop();

  ASSERT_TRUE(first_opportunity.has_value());
  EXPECT_EQ(first_opportunity->id, 101u);
  EXPECT_EQ(first_opportunity->display_id, 7);
  EXPECT_EQ(first_opportunity->target_ids, std::set<int64_t>({1001}));
  ASSERT_TRUE(second_opportunity.has_value());
  EXPECT_EQ(second_opportunity->id, 202u);
  EXPECT_EQ(second_opportunity->display_id, 7);
  EXPECT_EQ(second_opportunity->target_ids, std::set<int64_t>({2002}));
}

TEST(VsyncWaiterEmbedderTest, WrongIdentityDoesNotConsumeBaton) {
  const auto task_runners = CurrentThreadTaskRunners();
  std::optional<intptr_t> baton;
  size_t callback_count = 0;
  auto waiter = std::make_shared<VsyncWaiterEmbedder>(
      nullptr,
      [&](intptr_t next_baton, VsyncWaiter::DisplayId) { baton = next_baton; },
      task_runners);

  waiter->AsyncWaitForVsync(
      9, [&](std::unique_ptr<FrameTimingsRecorder>) { callback_count++; });
  ASSERT_TRUE(baton.has_value());
  const auto now = fml::TimePoint::Now();
  const auto target = now + fml::TimeDelta::FromMilliseconds(1);
  EXPECT_FALSE(
      waiter->ReturnVsync(8, baton.value(), now, target, 303, {}, now));
  EXPECT_FALSE(
      waiter->ReturnVsync(9, baton.value() + 1, now, target, 303, {}, now));
  EXPECT_FALSE(
      waiter->ReturnVsync(9, baton.value(), now, target, 303, {}, target));
  EXPECT_TRUE(waiter->ReturnVsync(9, baton.value(), now, target, 303, {}, now));
  EXPECT_FALSE(
      waiter->ReturnVsync(9, baton.value(), now, target, 303, {}, now));
  DrainCurrentMessageLoop();
  EXPECT_EQ(callback_count, 1u);
}

TEST(VsyncWaiterEmbedderTest, CancellationSettlesBeforeAcknowledgement) {
  const auto task_runners = CurrentThreadTaskRunners();
  std::optional<intptr_t> baton;
  std::vector<std::string> events;
  auto waiter = std::make_shared<VsyncWaiterEmbedder>(
      nullptr,
      [&](intptr_t next_baton, VsyncWaiter::DisplayId) { baton = next_baton; },
      task_runners);

  waiter->AsyncWaitForVsync(
      11,
      [&](std::unique_ptr<FrameTimingsRecorder>) { events.push_back("frame"); },
      [&](VsyncWaiter::CancellationReason reason) {
        EXPECT_EQ(reason, VsyncWaiter::CancellationReason::kEpochReplaced);
        events.push_back("animator");
      });
  ASSERT_TRUE(baton.has_value());
  EXPECT_TRUE(waiter->CancelVsync(
      11, baton.value(), VsyncWaiter::CancellationReason::kEpochReplaced,
      [&] { events.push_back("ack"); }));
  EXPECT_FALSE(waiter->CancelVsync(
      11, baton.value(), VsyncWaiter::CancellationReason::kEpochReplaced,
      nullptr));
  DrainCurrentMessageLoop();

  EXPECT_EQ(events, (std::vector<std::string>{"animator", "ack"}));
}

TEST(VsyncWaiterEmbedderTest, ReturnAndCancellationAreMutuallyExclusive) {
  const auto task_runners = CurrentThreadTaskRunners();
  std::optional<intptr_t> baton;
  size_t frame_count = 0;
  size_t cancellation_count = 0;
  auto waiter = std::make_shared<VsyncWaiterEmbedder>(
      nullptr,
      [&](intptr_t next_baton, VsyncWaiter::DisplayId) { baton = next_baton; },
      task_runners);

  waiter->AsyncWaitForVsync(
      12, [&](std::unique_ptr<FrameTimingsRecorder>) { frame_count++; },
      [&](VsyncWaiter::CancellationReason) { cancellation_count++; });
  ASSERT_TRUE(baton.has_value());
  const auto now = fml::TimePoint::Now();
  const auto target = now + fml::TimeDelta::FromMilliseconds(1);
  EXPECT_TRUE(
      waiter->ReturnVsync(12, baton.value(), now, target, 404, {}, now));
  EXPECT_FALSE(waiter->CancelVsync(
      12, baton.value(), VsyncWaiter::CancellationReason::kHostTerminated,
      nullptr));
  DrainCurrentMessageLoop();
  EXPECT_EQ(frame_count, 1u);
  EXPECT_EQ(cancellation_count, 0u);

  baton = std::nullopt;
  waiter->AsyncWaitForVsync(
      12, [&](std::unique_ptr<FrameTimingsRecorder>) { frame_count++; },
      [&](VsyncWaiter::CancellationReason) { cancellation_count++; });
  ASSERT_TRUE(baton.has_value());
  EXPECT_TRUE(waiter->CancelVsync(
      12, baton.value(), VsyncWaiter::CancellationReason::kHostTerminated,
      nullptr));
  EXPECT_FALSE(
      waiter->ReturnVsync(12, baton.value(), now, target, 405, {}, now));
  DrainCurrentMessageLoop();
  EXPECT_EQ(frame_count, 1u);
  EXPECT_EQ(cancellation_count, 1u);
}

TEST(VsyncWaiterEmbedderTest, ReturnedFutureOpportunityCanBeExactlyCancelled) {
  const auto task_runners = CurrentThreadTaskRunners();
  std::optional<intptr_t> baton;
  std::vector<std::string> events;
  auto waiter = std::make_shared<VsyncWaiterEmbedder>(
      nullptr,
      [&](intptr_t next_baton, VsyncWaiter::DisplayId) { baton = next_baton; },
      task_runners);

  waiter->AsyncWaitForVsync(
      14,
      [&](std::unique_ptr<FrameTimingsRecorder>) { events.push_back("frame"); },
      [&](VsyncWaiter::CancellationReason reason) {
        EXPECT_EQ(reason, VsyncWaiter::CancellationReason::kTargetRemoved);
        events.push_back("animator");
      });
  ASSERT_TRUE(baton.has_value());
  const auto now = fml::TimePoint::Now();
  EXPECT_TRUE(waiter->ReturnVsync(
      14, baton.value(), now + fml::TimeDelta::FromSeconds(1),
      now + fml::TimeDelta::FromSeconds(2), 406, {},
      now + fml::TimeDelta::FromMilliseconds(1500)));
  EXPECT_FALSE(waiter->CancelFrameOpportunity(
      14, 405, VsyncWaiter::CancellationReason::kTargetRemoved, nullptr));
  EXPECT_TRUE(waiter->CancelFrameOpportunity(
      14, 406, VsyncWaiter::CancellationReason::kTargetRemoved,
      [&] { events.push_back("ack"); }));
  EXPECT_FALSE(waiter->CancelFrameOpportunity(
      14, 406, VsyncWaiter::CancellationReason::kTargetRemoved, nullptr));
  DrainCurrentMessageLoop();

  EXPECT_EQ(events, (std::vector<std::string>{"animator", "ack"}));
}

TEST(VsyncWaiterEmbedderTest, RepeatedDemandCoalescesBeforeDelivery) {
  const auto task_runners = CurrentThreadTaskRunners();
  std::optional<intptr_t> baton;
  size_t request_count = 0;
  auto waiter = std::make_shared<VsyncWaiterEmbedder>(
      nullptr,
      [&](intptr_t next_baton, VsyncWaiter::DisplayId) {
        baton = next_baton;
        request_count++;
      },
      task_runners);

  waiter->AsyncWaitForVsync(13, [](std::unique_ptr<FrameTimingsRecorder>) {});
  waiter->AsyncWaitForVsync(13, [](std::unique_ptr<FrameTimingsRecorder>) {});
  EXPECT_EQ(request_count, 1u);
  ASSERT_TRUE(baton.has_value());
  EXPECT_TRUE(waiter->CancelVsync(
      13, baton.value(), VsyncWaiter::CancellationReason::kHostTerminated,
      nullptr));
  DrainCurrentMessageLoop();
}

}  // namespace testing
}  // namespace flutter
