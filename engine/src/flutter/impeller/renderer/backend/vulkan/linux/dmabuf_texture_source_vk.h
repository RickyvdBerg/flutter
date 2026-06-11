// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_LINUX_DMABUF_TEXTURE_SOURCE_VK_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_LINUX_DMABUF_TEXTURE_SOURCE_VK_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "impeller/renderer/backend/vulkan/texture_source_vk.h"
#include "impeller/renderer/backend/vulkan/vk.h"

namespace impeller {

class ContextVK;
struct DmabufImportedImageResourceVK;

/// @brief      Describes a single plane of a DMA-BUF.
struct DmabufPlaneDescriptor {
  int fd = -1;
  uint32_t offset = 0;
  uint32_t stride = 0;
};

/// @brief      Damage rect for a DMA-BUF texture, in pixel coordinates.
struct DmabufDamageRect {
  int32_t left;
  int32_t top;
  int32_t right;
  int32_t bottom;
};

/// @brief      Describes a DMA-BUF to be imported as a Vulkan image.
struct DmabufDescriptor {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t drm_format = 0;
  uint64_t drm_modifier = 0;
  uint32_t num_planes = 0;
  DmabufPlaneDescriptor planes[4] = {};
  int acquire_fence_fd = -1;
  std::vector<DmabufDamageRect> damage_rects;  // empty = full frame
};

//------------------------------------------------------------------------------
/// @brief      A texture source that wraps a Linux DMA-BUF file descriptor,
///             importing it into Vulkan via VK_EXT_external_memory_dma_buf
///             and VK_EXT_image_drm_format_modifier.
///
///             On successful construction, Vulkan takes ownership of the
///             DMA-BUF file descriptors. On failure, the caller retains
///             ownership.
///
class DmabufTextureSourceVK final : public TextureSourceVK {
 public:
  DmabufTextureSourceVK(const std::shared_ptr<Context>& context,
                        const DmabufDescriptor& desc);

  // |TextureSourceVK|
  ~DmabufTextureSourceVK() override;

  // |TextureSourceVK|
  vk::Image GetImage() const override;

  // |TextureSourceVK|
  vk::ImageView GetImageView() const override;

  // |TextureSourceVK|
  vk::ImageView GetRenderTargetView(uint32_t mip_level,
                                    uint32_t array_layer) const override;

  // |TextureSourceVK|
  bool IsSwapchainImage() const override;

  // |TextureSourceVK|
  std::optional<WaitSemaphore> ConsumeAcquireSemaphore() const override;

  /// Sets a callback that is invoked when the texture source is destroyed.
  /// This is used by the embedder DMA-BUF API to signal that the engine is
  /// done reading the imported buffer.
  void SetReleaseCallback(std::function<void()> release_callback);

  bool IsValid() const;

 private:
  std::shared_ptr<DmabufImportedImageResourceVK> imported_resource_;
  mutable vk::UniqueSemaphore acquire_semaphore_ = {};
  mutable std::mutex release_callback_mutex_;
  std::function<void()> release_callback_;
  bool is_valid_ = false;

  DmabufTextureSourceVK(const DmabufTextureSourceVK&) = delete;

  DmabufTextureSourceVK& operator=(const DmabufTextureSourceVK&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_LINUX_DMABUF_TEXTURE_SOURCE_VK_H_
