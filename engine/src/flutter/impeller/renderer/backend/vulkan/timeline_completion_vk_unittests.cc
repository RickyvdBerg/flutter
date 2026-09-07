// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/fml/synchronization/waitable_event.h"
#include "gtest/gtest.h"  // IWYU pragma: keep
#include "impeller/renderer/backend/vulkan/test/mock_vulkan.h"
#include "impeller/renderer/backend/vulkan/timeline_completion_vk.h"

#include <mutex>
#include <vector>

namespace impeller {
namespace testing {

TEST(TimelineCompletionVKTest, ExecutesCompletionCallback) {
  auto const context = MockVulkanContextBuilder().Build();
  auto const completion = context->GetTimelineCompletion();

  auto signal = fml::ManualResetWaitableEvent();
  const auto value = completion->ReserveSubmitValue();
  ASSERT_TRUE(
      completion->AddCompletion(value, [&signal](CommandBuffer::Status status) {
        EXPECT_EQ(status, CommandBuffer::Status::kCompleted);
        signal.Signal();
      }));

  vk::SemaphoreSignalInfo signal_info;
  signal_info.setSemaphore(completion->GetSemaphore());
  signal_info.setValue(value);
  ASSERT_EQ(context->GetDevice().signalSemaphoreKHR(signal_info),
            vk::Result::eSuccess);

  signal.Wait();
}

TEST(TimelineCompletionVKTest, ExecutesCallbacksInTimelineOrder) {
  auto const context = MockVulkanContextBuilder().Build();
  auto const completion = context->GetTimelineCompletion();

  auto first = fml::ManualResetWaitableEvent();
  auto second = fml::ManualResetWaitableEvent();
  std::vector<int> callbacks;
  std::mutex callbacks_mutex;

  const auto value1 = completion->ReserveSubmitValue();
  const auto value2 = completion->ReserveSubmitValue();
  ASSERT_TRUE(completion->AddCompletion(
      value2,
      [&callbacks, &callbacks_mutex, &second](CommandBuffer::Status status) {
        EXPECT_EQ(status, CommandBuffer::Status::kCompleted);
        std::scoped_lock lock(callbacks_mutex);
        callbacks.push_back(2);
        second.Signal();
      }));
  ASSERT_TRUE(completion->AddCompletion(
      value1,
      [&callbacks, &callbacks_mutex, &first](CommandBuffer::Status status) {
        EXPECT_EQ(status, CommandBuffer::Status::kCompleted);
        std::scoped_lock lock(callbacks_mutex);
        callbacks.push_back(1);
        first.Signal();
      }));

  vk::SemaphoreSignalInfo signal_info;
  signal_info.setSemaphore(completion->GetSemaphore());
  signal_info.setValue(value2);
  ASSERT_EQ(context->GetDevice().signalSemaphoreKHR(signal_info),
            vk::Result::eSuccess);

  first.Wait();
  second.Wait();
  std::scoped_lock lock(callbacks_mutex);
  ASSERT_EQ(callbacks.size(), 2u);
  EXPECT_EQ(callbacks[0], 1);
  EXPECT_EQ(callbacks[1], 2);
}

TEST(TimelineCompletionVKTest, ExecutesAlreadyCompletedValueImmediately) {
  auto const context = MockVulkanContextBuilder().Build();
  auto const completion = context->GetTimelineCompletion();

  const auto value = completion->ReserveSubmitValue();
  vk::SemaphoreSignalInfo signal_info;
  signal_info.setSemaphore(completion->GetSemaphore());
  signal_info.setValue(value);
  ASSERT_EQ(context->GetDevice().signalSemaphoreKHR(signal_info),
            vk::Result::eSuccess);
  ASSERT_TRUE(completion->WaitFor(value));

  bool called = false;
  ASSERT_TRUE(
      completion->AddCompletion(value, [&called](CommandBuffer::Status status) {
        EXPECT_EQ(status, CommandBuffer::Status::kCompleted);
        called = true;
      }));
  EXPECT_TRUE(called);
}

TEST(TimelineCompletionVKTest, WaitForAdvancesCompletedValue) {
  auto const context = MockVulkanContextBuilder().Build();
  auto const completion = context->GetTimelineCompletion();

  const auto value = completion->ReserveSubmitValue();
  vk::SemaphoreSignalInfo signal_info;
  signal_info.setSemaphore(completion->GetSemaphore());
  signal_info.setValue(value);
  ASSERT_EQ(context->GetDevice().signalSemaphoreKHR(signal_info),
            vk::Result::eSuccess);

  EXPECT_TRUE(completion->WaitFor(value));
  EXPECT_GE(completion->GetCompletedValue(), value);
}

}  // namespace testing
}  // namespace impeller
