// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fml/status.h"

#include <array>

#include "flutter/fml/make_copyable.h"
#include "impeller/renderer/backend/vulkan/command_queue_vk.h"

#include "impeller/base/validation.h"
#include "impeller/renderer/backend/vulkan/command_buffer_vk.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/backend/vulkan/swapchain/ahb/external_semaphore_vk.h"
#include "impeller/renderer/backend/vulkan/timeline_completion_vk.h"
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
  auto completion = context->GetTimelineCompletion();
  if (!completion || !completion->IsValid()) {
    VALIDATION_LOG << "Timeline completion tracker is not available.";
    return fml::Status(fml::StatusCode::kCancelled,
                       "Timeline completion tracker is not available.");
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

  std::vector<TrackedObjectsVK::PendingSignalSemaphoreVK>
      signal_semaphores_storage;
  std::vector<vk::Semaphore> signal_semaphore_handles;
  for (auto& objs : tracked_objects) {
    auto signals = objs->CreateSignalSemaphores(context);
    signal_semaphore_handles.reserve(signal_semaphore_handles.size() +
                                     signals.size());
    signal_semaphores_storage.reserve(signal_semaphores_storage.size() +
                                      signals.size());
    for (auto& signal : signals) {
      signal_semaphore_handles.push_back(signal.semaphore->GetHandle());
      signal_semaphores_storage.push_back(std::move(signal));
    }
  }

  vk::SemaphoreCreateInfo internal_dependency_info;
  auto [dependency_result, internal_dependency_semaphore] =
      context->GetDevice().createSemaphoreUnique(internal_dependency_info);
  if (dependency_result != vk::Result::eSuccess) {
    VALIDATION_LOG << "Could not create Vulkan submit dependency semaphore: "
                   << vk::to_string(dependency_result);
    return fml::Status(fml::StatusCode::kCancelled,
                       "Failed to create submit dependency semaphore.");
  }
  context->SetDebugName(internal_dependency_semaphore.get(),
                        "ImpellerSubmitCompletionDependency");

  vk::SubmitInfo render_submit_info;
  render_submit_info.setCommandBuffers(vk_buffers);
  if (!wait_semaphore_handles.empty()) {
    render_submit_info.setWaitSemaphores(wait_semaphore_handles);
    render_submit_info.setWaitDstStageMask(wait_stage_masks);
  }
  signal_semaphore_handles.push_back(internal_dependency_semaphore.get());
  render_submit_info.setSignalSemaphores(signal_semaphore_handles);

  std::array<vk::Semaphore, 1> completion_wait_semaphores = {
      internal_dependency_semaphore.get()};
  std::array<vk::PipelineStageFlags, 1> completion_wait_stage_masks = {
      vk::PipelineStageFlagBits::eTopOfPipe};
  std::array<vk::Semaphore, 1> completion_signal_semaphores = {
      completion->GetSemaphore()};
  std::array<uint64_t, 1> completion_wait_values = {0u};
  std::array<uint64_t, 1> completion_signal_values = {0u};
  vk::TimelineSemaphoreSubmitInfo completion_timeline_submit_info;
  completion_timeline_submit_info.setWaitSemaphoreValues(
      completion_wait_values);
  completion_timeline_submit_info.setSignalSemaphoreValues(
      completion_signal_values);

  vk::SubmitInfo completion_submit_info;
  completion_submit_info.setWaitSemaphores(completion_wait_semaphores);
  completion_submit_info.setWaitDstStageMask(completion_wait_stage_masks);
  completion_submit_info.setSignalSemaphores(completion_signal_semaphores);
  completion_submit_info.setPNext(&completion_timeline_submit_info);

  std::array<vk::SubmitInfo, 2> submit_infos = {render_submit_info,
                                                completion_submit_info};

  uint64_t completion_value = 0u;
  auto status = context->GetGraphicsQueue()->SubmitLocked(
      [&](const vk::Queue& queue) -> vk::Result {
        // Timeline values must reflect actual queue submission order. The
        // marker batch waits on a queue-local binary semaphore signaled by the
        // render batch, so CPU completion cannot run before render execution
        // reaches that signal.
        completion_value = completion->ReserveSubmitValue();
        completion_signal_values[0] = completion_value;
        return queue.submit(submit_infos, vk::Fence{});
      });
  if (status != vk::Result::eSuccess) {
    VALIDATION_LOG << "Failed to submit queue: " << vk::to_string(status);
    return fml::Status(fml::StatusCode::kCancelled, "Failed to submit queue: ");
  }

  for (const auto& signal : signal_semaphores_storage) {
    signal.texture->SetRenderCompleteSyncFD(signal.semaphore->CreateFD());
  }

  // Submit will proceed, call callback with true when it is done and do not
  // call when `reset` is collected.
  auto added_completion = completion->AddCompletion(
      completion_value,
      fml::MakeCopyable(
          [completion_callback,
           tracked_objects = std::move(tracked_objects),
           signal_semaphores_storage = std::move(signal_semaphores_storage),
           internal_dependency_semaphore =
               std::move(internal_dependency_semaphore),
           wait_semaphores_storage =
               std::move(wait_semaphores_storage)](
              CommandBuffer::Status status) mutable {
            // Ensure tracked objects and semaphores are destructed before
            // calling any final callbacks.
            signal_semaphores_storage.clear();
            internal_dependency_semaphore.reset();
            wait_semaphores_storage.clear();
            tracked_objects.clear();
            if (completion_callback) {
              completion_callback(status);
            }
          }));
  if (!added_completion) {
    completion->WaitFor(completion_value);
    return fml::Status(fml::StatusCode::kCancelled,
                       "Failed to add timeline completion.");
  }
  reset.Release();
  return fml::Status();
}

}  // namespace impeller
