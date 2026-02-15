// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/linux/dmabuf_texture_source_vk.h"

#include <unistd.h>
#include <set>

#include "flutter/fml/file.h"
#include "impeller/renderer/backend/vulkan/allocator_vk.h"
#include "impeller/renderer/backend/vulkan/capabilities_vk.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/backend/vulkan/texture_source_vk.h"

namespace impeller {

namespace {

// DRM fourcc codes. Defined here to avoid a build dependency on drm_fourcc.h.
constexpr uint32_t kDrmFormatArgb8888 = 0x34325241;  // DRM_FORMAT_ARGB8888
constexpr uint32_t kDrmFormatXrgb8888 = 0x34325258;  // DRM_FORMAT_XRGB8888
constexpr uint32_t kDrmFormatAbgr8888 = 0x34324241;  // DRM_FORMAT_ABGR8888
constexpr uint32_t kDrmFormatXbgr8888 = 0x34324258;  // DRM_FORMAT_XBGR8888

vk::Format DrmFormatToVkFormat(uint32_t drm_format) {
  switch (drm_format) {
    case kDrmFormatArgb8888:
    case kDrmFormatXrgb8888:
      return vk::Format::eB8G8R8A8Unorm;
    case kDrmFormatAbgr8888:
    case kDrmFormatXbgr8888:
      return vk::Format::eR8G8B8A8Unorm;
    default:
      return vk::Format::eUndefined;
  }
}

PixelFormat DrmFormatToPixelFormat(uint32_t drm_format) {
  switch (drm_format) {
    case kDrmFormatArgb8888:
    case kDrmFormatXrgb8888:
      return PixelFormat::kB8G8R8A8UNormInt;
    case kDrmFormatAbgr8888:
    case kDrmFormatXbgr8888:
      return PixelFormat::kR8G8B8A8UNormInt;
    default:
      return PixelFormat::kR8G8B8A8UNormInt;
  }
}

TextureDescriptor ToTextureDescriptor(const DmabufDescriptor& desc) {
  TextureDescriptor texture_desc;
  texture_desc.storage_mode = StorageMode::kDevicePrivate;
  texture_desc.format = DrmFormatToPixelFormat(desc.drm_format);
  texture_desc.size = ISize{desc.width, desc.height};
  texture_desc.type = TextureType::kTexture2D;
  texture_desc.sample_count = SampleCount::kCount1;
  texture_desc.compression_type = CompressionType::kLossless;
  texture_desc.mip_count = 1u;
  return texture_desc;
}

}  // namespace

DmabufTextureSourceVK::DmabufTextureSourceVK(
    const std::shared_ptr<Context>& p_context,
    const DmabufDescriptor& desc)
    : TextureSourceVK(ToTextureDescriptor(desc)) {
  if (!p_context) {
    return;
  }

  if (desc.num_planes != 1) {
    VALIDATION_LOG << "Multi-plane DMA-BUF import is not yet supported "
                   << "(num_planes=" << desc.num_planes << ").";
    return;
  }
  if (desc.planes[0].fd < 0) {
    VALIDATION_LOG << "Invalid DMA-BUF plane fd: " << desc.planes[0].fd;
    return;
  }

  const auto& context = ContextVK::Cast(*p_context);
  const auto& device = context.GetDevice();
  const auto& physical_device = context.GetPhysicalDevice();

  const auto vk_format = DrmFormatToVkFormat(desc.drm_format);
  if (vk_format == vk::Format::eUndefined) {
    VALIDATION_LOG << "Unsupported DRM format: 0x" << std::hex
                   << desc.drm_format;
    return;
  }

  // Duplicate import FDs so partial failures never consume the caller's FDs.
  fml::UniqueFD plane_fd_for_import = fml::Duplicate(desc.planes[0].fd);
  if (!plane_fd_for_import.is_valid()) {
    VALIDATION_LOG << "Could not duplicate DMA-BUF plane fd for import.";
    return;
  }
  fml::UniqueFD acquire_fence_fd_for_import;
  if (desc.acquire_fence_fd >= 0) {
    acquire_fence_fd_for_import = fml::Duplicate(desc.acquire_fence_fd);
    if (!acquire_fence_fd_for_import.is_valid()) {
      VALIDATION_LOG << "Could not duplicate acquire fence fd for import.";
      return;
    }
  }

  // Build per-plane layout info for the DRM format modifier.
  vk::SubresourceLayout plane_layouts[4] = {};
  for (uint32_t i = 0; i < desc.num_planes; i++) {
    plane_layouts[i].offset = desc.planes[i].offset;
    plane_layouts[i].rowPitch = desc.planes[i].stride;
    plane_layouts[i].size = 0;  // Determined by driver.
    plane_layouts[i].arrayPitch = 0;
    plane_layouts[i].depthPitch = 0;
  }

  // Chain: ImageCreateInfo -> ExternalMemoryImageCreateInfo ->
  //        ImageDrmFormatModifierExplicitCreateInfoEXT
  vk::ImageDrmFormatModifierExplicitCreateInfoEXT drm_info;
  drm_info.drmFormatModifier = desc.drm_modifier;
  drm_info.drmFormatModifierPlaneCount = desc.num_planes;
  drm_info.pPlaneLayouts = plane_layouts;

  vk::ExternalMemoryImageCreateInfo external_mem_info;
  external_mem_info.handleTypes =
      vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
  external_mem_info.pNext = &drm_info;

  vk::ImageCreateInfo image_info;
  image_info.pNext = &external_mem_info;
  image_info.imageType = vk::ImageType::e2D;
  image_info.format = vk_format;
  image_info.extent.width = desc.width;
  image_info.extent.height = desc.height;
  image_info.extent.depth = 1;
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = vk::SampleCountFlagBits::e1;
  image_info.tiling = vk::ImageTiling::eDrmFormatModifierEXT;
  image_info.usage = vk::ImageUsageFlagBits::eSampled;
  image_info.sharingMode = vk::SharingMode::eExclusive;
  image_info.initialLayout = vk::ImageLayout::eUndefined;

  auto image_result = device.createImageUnique(image_info);
  if (image_result.result != vk::Result::eSuccess) {
    VALIDATION_LOG << "Could not create image for DMA-BUF import: "
                   << vk::to_string(image_result.result);
    return;
  }
  auto image = std::move(image_result.value);

  // Query memory requirements.
  auto mem_reqs = device.getImageMemoryRequirements(image.get());

  // Find a suitable memory type.
  vk::PhysicalDeviceMemoryProperties memory_properties;
  physical_device.getMemoryProperties(&memory_properties);
  int32_t memory_type_index = AllocatorVK::FindMemoryTypeIndex(
      mem_reqs.memoryTypeBits, memory_properties);
  if (memory_type_index < 0) {
    VALIDATION_LOG << "Could not find memory type for DMA-BUF import.";
    return;
  }

  // Import the DMA-BUF fd as device memory.
  // Chain: MemoryAllocateInfo -> MemoryDedicatedAllocateInfo ->
  //        ImportMemoryFdInfoKHR
  vk::ImportMemoryFdInfoKHR import_fd_info;
  import_fd_info.handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
  int imported_plane_fd = plane_fd_for_import.release();
  import_fd_info.fd = imported_plane_fd;

  vk::MemoryDedicatedAllocateInfo dedicated_info;
  dedicated_info.image = image.get();
  dedicated_info.pNext = &import_fd_info;

  vk::MemoryAllocateInfo mem_alloc_info;
  mem_alloc_info.pNext = &dedicated_info;
  mem_alloc_info.allocationSize = mem_reqs.size;
  mem_alloc_info.memoryTypeIndex = memory_type_index;

  auto device_memory_result = device.allocateMemoryUnique(mem_alloc_info);
  if (device_memory_result.result != vk::Result::eSuccess) {
    if (imported_plane_fd >= 0) {
      close(imported_plane_fd);
    }
    VALIDATION_LOG << "Could not allocate device memory for DMA-BUF import: "
                   << vk::to_string(device_memory_result.result);
    return;
  }
  imported_plane_fd = -1;
  auto device_memory = std::move(device_memory_result.value);

  // Bind the image to the imported memory.
  if (auto result = device.bindImageMemory(image.get(), device_memory.get(), 0);
      result != vk::Result::eSuccess) {
    VALIDATION_LOG << "Could not bind DMA-BUF device memory to image: "
                   << vk::to_string(result);
    return;
  }

  // Create image view.
  vk::ImageViewCreateInfo view_info;
  view_info.image = image.get();
  view_info.viewType = vk::ImageViewType::e2D;
  view_info.format = vk_format;
  view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  view_info.subresourceRange.baseMipLevel = 0;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.baseArrayLayer = 0;
  view_info.subresourceRange.layerCount = 1;

  auto image_view_result = device.createImageViewUnique(view_info);
  if (image_view_result.result != vk::Result::eSuccess) {
    VALIDATION_LOG << "Could not create image view for DMA-BUF import: "
                   << vk::to_string(image_view_result.result);
    return;
  }

  vk::UniqueSemaphore acquire_semaphore;

  // Import acquire fence as VkSemaphore for GPU-side synchronization. A fence
  // was explicitly provided by the producer, so failing to import it means we
  // cannot safely sample the texture.
  if (desc.acquire_fence_fd >= 0) {
    const auto& caps = CapabilitiesVK::Cast(*context.GetCapabilities());
    if (!caps.HasExtension(
            OptionalLinuxDeviceExtensionVK::kKHRExternalSemaphoreFd) ||
        !caps.HasExtension(
            OptionalLinuxDeviceExtensionVK::kKHRExternalSemaphore)) {
      VALIDATION_LOG << "Cannot import DMA-BUF acquire fence: "
                        "VK_KHR_external_semaphore_fd not available.";
      return;
    }

    auto sem_result = device.createSemaphoreUnique({});
    if (sem_result.result != vk::Result::eSuccess) {
      VALIDATION_LOG << "Could not create semaphore for acquire fence import: "
                     << vk::to_string(sem_result.result);
      return;
    }

    int imported_acquire_fd = acquire_fence_fd_for_import.release();
    vk::ImportSemaphoreFdInfoKHR import_info;
    import_info.semaphore = *sem_result.value;
    import_info.fd = imported_acquire_fd;
    import_info.handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd;
    import_info.flags = vk::SemaphoreImportFlagBitsKHR::eTemporary;
    auto import_result = device.importSemaphoreFdKHR(import_info);
    if (import_result != vk::Result::eSuccess) {
      if (imported_acquire_fd >= 0) {
        close(imported_acquire_fd);
      }
      VALIDATION_LOG << "Could not import acquire fence semaphore: "
                     << vk::to_string(import_result);
      return;
    }
    acquire_semaphore = std::move(sem_result.value);
  }

  // Success — transfer resources into members.
  device_memory_ = std::move(device_memory);
  image_ = std::move(image);
  image_view_ = std::move(image_view_result.value);
  acquire_semaphore_ = std::move(acquire_semaphore);

#ifdef IMPELLER_DEBUG
  context.SetDebugName(device_memory_.get(), "DmaBuf Device Memory");
  context.SetDebugName(image_.get(), "DmaBuf Image");
  context.SetDebugName(image_view_.get(), "DmaBuf ImageView");
#endif  // IMPELLER_DEBUG

  // The import consumed duplicate FDs. Close the original caller-provided FDs
  // now that ownership transfer has succeeded.
  std::set<int> transferred_fds;
  transferred_fds.insert(desc.planes[0].fd);
  if (desc.acquire_fence_fd >= 0) {
    transferred_fds.insert(desc.acquire_fence_fd);
  }
  for (int fd : transferred_fds) {
    close(fd);
  }

  // DRM format modifier images have a layout determined by the modifier.
  // Mark as shader-read-only so SetLayout() has the correct old layout.
  SetLayoutWithoutEncoding(vk::ImageLayout::eShaderReadOnlyOptimal);

  is_valid_ = true;
}

// |TextureSourceVK|
DmabufTextureSourceVK::~DmabufTextureSourceVK() {
  std::function<void()> callback;
  {
    std::scoped_lock lock(release_callback_mutex_);
    callback = std::move(release_callback_);
  }
  if (callback) {
    callback();
  }
}

bool DmabufTextureSourceVK::IsValid() const {
  return is_valid_;
}

// |TextureSourceVK|
vk::Image DmabufTextureSourceVK::GetImage() const {
  return image_.get();
}

// |TextureSourceVK|
vk::ImageView DmabufTextureSourceVK::GetImageView() const {
  return image_view_.get();
}

// |TextureSourceVK|
vk::ImageView DmabufTextureSourceVK::GetRenderTargetView() const {
  return image_view_.get();
}

// |TextureSourceVK|
bool DmabufTextureSourceVK::IsSwapchainImage() const {
  return false;
}

// |TextureSourceVK|
std::optional<WaitSemaphore> DmabufTextureSourceVK::ConsumeAcquireSemaphore()
    const {
  if (!acquire_semaphore_) {
    return std::nullopt;
  }
  WaitSemaphore result;
  result.semaphore = std::move(acquire_semaphore_);
  result.wait_stage = vk::PipelineStageFlagBits::eFragmentShader |
                      vk::PipelineStageFlagBits::eComputeShader;
  return result;
}

void DmabufTextureSourceVK::SetReleaseCallback(
    std::function<void()> release_callback) {
  if (!release_callback) {
    return;
  }
  std::scoped_lock lock(release_callback_mutex_);
  if (!release_callback_) {
    release_callback_ = std::move(release_callback);
    return;
  }

  auto existing = std::move(release_callback_);
  release_callback_ = [first = std::move(existing),
                       second = std::move(release_callback)]() mutable {
    first();
    second();
  };
}

}  // namespace impeller
