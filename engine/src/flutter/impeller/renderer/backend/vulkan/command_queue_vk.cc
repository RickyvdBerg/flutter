// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fml/status.h"

#include "impeller/renderer/backend/vulkan/command_queue_vk.h"

#include "impeller/base/validation.h"
#include "impeller/renderer/backend/vulkan/command_buffer_vk.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/backend/vulkan/fence_waiter_vk.h"
#include "impeller/renderer/backend/vulkan/tracked_objects_vk.h"
#include "impeller/renderer/command_buffer.h"

namespace impeller {

CommandQueueVK::CommandQueueVK(const std::weak_ptr<ContextVK>& context)
    : context_(context) {}

CommandQueueVK::~CommandQueueVK() = default;

fml::Status CommandQueueVK::Submit(
    const std::vector<std::shared_ptr<CommandBuffer>>& buffers,
    const CompletionCallback& completion_callback,
    bool block_on_schedule) {
  if (buffers.empty()) {
    return fml::Status(fml::StatusCode::kInvalidArgument,
                       "No command buffers provided.");
  }
  // Success or failure, you only get to submit once.
  fml::ScopedCleanupClosure reset([&]() {
    if (completion_callback) {
      completion_callback(CommandBuffer::Status::kError);
    }
  });

  std::vector<vk::CommandBuffer> vk_buffers;
  std::vector<std::shared_ptr<TrackedObjectsVK>> tracked_objects;
  vk_buffers.reserve(buffers.size());
  tracked_objects.reserve(buffers.size());
  for (const std::shared_ptr<CommandBuffer>& buffer : buffers) {
    CommandBufferVK& command_buffer = CommandBufferVK::Cast(*buffer);
    if (!command_buffer.EndCommandBuffer()) {
      return fml::Status(fml::StatusCode::kCancelled,
                         "Failed to end command buffer.");
    }
    vk_buffers.push_back(command_buffer.GetCommandBuffer());
    tracked_objects.push_back(std::move(command_buffer.tracked_objects_));
  }

  auto context = context_.lock();
  if (!context) {
    VALIDATION_LOG << "Device lost.";
    return fml::Status(fml::StatusCode::kCancelled, "Device lost.");
  }
  auto [fence_result, fence] = context->GetDevice().createFenceUnique({});
  if (fence_result != vk::Result::eSuccess) {
    VALIDATION_LOG << "Failed to create fence: " << vk::to_string(fence_result);
    return fml::Status(fml::StatusCode::kCancelled, "Failed to create fence.");
  }

  // Collect wait semaphores from all tracked objects (e.g. DMA-BUF
  // acquire fences imported as VkSemaphores).
  std::vector<vk::Semaphore> wait_semaphore_handles;
  std::vector<vk::PipelineStageFlags> wait_stage_masks;
  std::vector<WaitSemaphore> wait_semaphores_storage;
  for (auto& objs : tracked_objects) {
    for (auto& sem : objs->TakeWaitSemaphores()) {
      wait_semaphore_handles.push_back(*sem.semaphore);
      wait_stage_masks.push_back(sem.wait_stage);
      wait_semaphores_storage.push_back(std::move(sem));
    }
  }

  // FenceWaiterVK invokes this synchronously while the vectors above remain
  // alive. Queue submission and fence registration therefore form one
  // operation, while the owned semaphore wrappers remain retained below.
  auto submit_callback = [&context, &vk_buffers, &wait_semaphore_handles,
                          &wait_stage_masks](vk::Fence submit_fence) {
    vk::SubmitInfo submit_info;
    submit_info.setCommandBuffers(vk_buffers);
    if (!wait_semaphore_handles.empty()) {
      submit_info.setWaitSemaphores(wait_semaphore_handles);
      submit_info.setWaitDstStageMask(wait_stage_masks);
    }
    auto status =
        context->GetGraphicsQueue()->Submit(submit_info, submit_fence);
    if (status != vk::Result::eSuccess) {
      std::string submit_error =
          "Failed to submit command: " + vk::to_string(status);
      VALIDATION_LOG << submit_error;
      return fml::Status(fml::StatusCode::kCancelled, submit_error);
    }
    return fml::Status();
  };

  std::shared_ptr<GpuSubmissionTracker> tracker =
      context->GetMutableSubmissionTracker();
  uint64_t submission_id = tracker->RecordSubmission();

  // This will be called later when the command completes.
  auto fence_complete_callback =
      [completion_callback, tracker, submission_id,
       tracked_objects = std::move(tracked_objects),
       wait_semaphores_storage = std::move(wait_semaphores_storage)]() mutable {
        // Ensure tracked objects and semaphores are destructed before calling
        // any final callbacks.
        wait_semaphores_storage.clear();
        tracked_objects.clear();
        tracker->RecordCompletion(submission_id);
        if (completion_callback) {
          completion_callback(CommandBuffer::Status::kCompleted);
        }
      };

  auto fence_status = context->GetFenceWaiter()->AddFence(
      std::move(fence), submit_callback, std::move(fence_complete_callback));
  if (!fence_status.ok()) {
    tracker->RecordCompletion(submission_id);
    return fence_status;
  }
  reset.Release();
  return fml::Status();
}

}  // namespace impeller
