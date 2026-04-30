// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/swapchain/ahb/external_semaphore_vk.h"

#include "impeller/base/validation.h"
#include "impeller/renderer/backend/vulkan/capabilities_vk.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "vulkan/vulkan_handles.hpp"

namespace impeller {

ExternalSemaphoreVK::ExternalSemaphoreVK(
    const std::shared_ptr<Context>& context) {
  if (!context) {
    return;
  }
  const auto& context_vk = ContextVK::Cast(*context);
  const auto& caps = CapabilitiesVK::Cast(*context->GetCapabilities());
  if (!caps.SupportsExternalSemaphoreExtensions()) {
    return;
  }

  vk::PhysicalDeviceExternalSemaphoreInfo external_info;
  external_info.handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd;
  const auto external_properties =
      context_vk.GetPhysicalDevice().getExternalSemaphoreProperties(
          external_info);
  if (!(external_properties.externalSemaphoreFeatures &
        vk::ExternalSemaphoreFeatureFlagBits::eExportable)) {
    return;
  }

  vk::StructureChain<vk::SemaphoreCreateInfo, vk::ExportSemaphoreCreateInfoKHR>
      info;

  info.get<vk::ExportSemaphoreCreateInfoKHR>().handleTypes =
      vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd;

  auto [result, semaphore] =
      context_vk.GetDevice().createSemaphoreUnique(info.get());
  if (result != vk::Result::eSuccess) {
    VALIDATION_LOG << "Could not create external fence: "
                   << vk::to_string(result);
    return;
  }

  context_vk.SetDebugName(semaphore.get(), "ExternalSemaphoreSyncFD");

  semaphore_ = MakeSharedVK(std::move(semaphore));
}

ExternalSemaphoreVK::~ExternalSemaphoreVK() = default;

bool ExternalSemaphoreVK::IsValid() const {
  return !!semaphore_;
}

fml::UniqueFD ExternalSemaphoreVK::CreateFD() const {
  if (!IsValid()) {
    return {};
  }
  vk::SemaphoreGetFdInfoKHR info;
  info.semaphore = semaphore_->Get();
  info.handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd;
  auto [result, fd] =
      semaphore_->GetUniqueWrapper().getOwner().getSemaphoreFdKHR(info);
  if (result != vk::Result::eSuccess) {
    VALIDATION_LOG << "Could not export external fence FD: "
                   << vk::to_string(result);
    return {};
  }
  return fml::UniqueFD{fd};
}

const vk::Semaphore& ExternalSemaphoreVK::GetHandle() const {
  return semaphore_->Get();
}

const SharedHandleVK<vk::Semaphore>& ExternalSemaphoreVK::GetSharedHandle()
    const {
  return semaphore_;
}

}  // namespace impeller
