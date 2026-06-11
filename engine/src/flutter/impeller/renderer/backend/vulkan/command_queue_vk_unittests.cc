// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>

#include "flutter/testing/testing.h"
#include "gtest/gtest.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/backend/vulkan/test/mock_vulkan.h"
#include "impeller/renderer/backend/vulkan/timeline_completion_vk.h"

namespace impeller {
namespace testing {

TEST(CommandQueueVKTest, QueueSubmit) {
  const auto context = MockVulkanContextBuilder().Build();
  auto buffer = context->CreateCommandBuffer();
  auto status = context->GetCommandQueue()->Submit({buffer});
  EXPECT_TRUE(status.ok());

  const auto called = GetMockVulkanFunctions(context->GetDevice());
  EXPECT_NE(std::find(called->begin(), called->end(), "vkQueueSubmit"),
            called->end());
}

TEST(CommandQueueVKTest, SubmitAfterTimelineCompletionTerminated) {
  const auto context = MockVulkanContextBuilder().Build();
  auto buffer = context->CreateCommandBuffer();
  context->GetTimelineCompletion()->Terminate();
  auto status = context->GetCommandQueue()->Submit({buffer});
  EXPECT_EQ(status.code(), fml::StatusCode::kCancelled);

  // The command buffer should not be submitted to the Vulkan queue if the
  // completion tracker has been terminated.
  const auto called = GetMockVulkanFunctions(context->GetDevice());
  EXPECT_EQ(std::find(called->begin(), called->end(), "vkQueueSubmit"),
            called->end());
}

}  // namespace testing
}  // namespace impeller
