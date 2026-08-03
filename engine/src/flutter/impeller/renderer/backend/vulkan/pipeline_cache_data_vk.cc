// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/pipeline_cache_data_vk.h"

#include <limits>

#include "flutter/fml/file.h"
#include "impeller/base/allocation.h"
#include "impeller/base/validation.h"

namespace impeller {

static constexpr const char* kPipelineCacheFileName =
    "flutter.impeller.vkcache";

bool PipelineCacheDataPersist(const fml::UniqueFD& cache_directory,
                              const VkPhysicalDeviceProperties& props,
                              const vk::UniquePipelineCache& cache,
                              size_t max_data_bytes) {
  if (!cache_directory.is_valid()) {
    return false;
  }
  size_t data_size = 0u;
  if (cache.getOwner().getPipelineCacheData(*cache, &data_size, nullptr) !=
      vk::Result::eSuccess) {
    VALIDATION_LOG << "Could not fetch pipeline cache size.";
    return false;
  }
  if (data_size == 0u) {
    return true;
  }
  if (data_size > max_data_bytes ||
      data_size >
          std::numeric_limits<size_t>::max() - sizeof(PipelineCacheHeaderVK)) {
    VALIDATION_LOG << "Pipeline cache data exceeds its configured budget.";
    return false;
  }
  const size_t allocated_data_bytes = data_size;
  auto allocation = std::make_shared<Allocation>();
  if (!allocation->Truncate(Bytes{sizeof(PipelineCacheHeaderVK) + data_size},
                            false)) {
    VALIDATION_LOG << "Could not allocate pipeline cache data staging buffer.";
    return false;
  }
  // Read the cache data and obtain the actual data size (which may be smaller
  // than the original query for the data size if rendering operations happened
  // after that call)
  vk::Result lookup_result = cache.getOwner().getPipelineCacheData(
      *cache, &data_size,
      allocation->GetBuffer() + sizeof(PipelineCacheHeaderVK));

  // Some drivers may return incomplete erroneously, but this is not an
  // error condition as some/all data was still written.
  if (lookup_result != vk::Result::eSuccess &&
      lookup_result != vk::Result::eIncomplete) {
    VALIDATION_LOG << "Could not copy pipeline cache data.";
    return false;
  }
  if (data_size > max_data_bytes || data_size > allocated_data_bytes) {
    VALIDATION_LOG << "Pipeline cache data grew beyond its configured budget.";
    return false;
  }

  const auto header = PipelineCacheHeaderVK{props, data_size};
  std::memcpy(allocation->GetBuffer(), &header, sizeof(header));

  fml::NonOwnedMapping allocation_mapping(
      allocation->GetBuffer(), sizeof(PipelineCacheHeaderVK) + data_size);
  if (!fml::WriteAtomically(cache_directory, kPipelineCacheFileName,
                            allocation_mapping)) {
    VALIDATION_LOG << "Could not write cache file to disk.";
    return false;
  }
  return true;
}

std::unique_ptr<fml::Mapping> PipelineCacheDataRetrieve(
    const fml::UniqueFD& cache_directory,
    const VkPhysicalDeviceProperties& props,
    size_t max_data_bytes) {
  if (!cache_directory.is_valid()) {
    return nullptr;
  }
  auto cache_file =
      fml::OpenFileReadOnly(cache_directory, kPipelineCacheFileName);
  const auto file_size = fml::GetRegularFileSize(cache_file);
  if (!file_size.has_value()) {
    VALIDATION_LOG << "Pipeline cache is not a regular file.";
    return nullptr;
  }
  if (max_data_bytes >
      std::numeric_limits<size_t>::max() - sizeof(PipelineCacheHeaderVK)) {
    VALIDATION_LOG << "Pipeline cache budget exceeds addressable memory.";
    return nullptr;
  }
  const size_t max_file_bytes = sizeof(PipelineCacheHeaderVK) + max_data_bytes;
  if (*file_size < sizeof(PipelineCacheHeaderVK)) {
    VALIDATION_LOG << "Pipeline cache data size is too small.";
    return nullptr;
  }
  if (*file_size > max_file_bytes) {
    VALIDATION_LOG << "Pipeline cache file exceeds its configured budget.";
    return nullptr;
  }
  std::shared_ptr<fml::FileMapping> on_disk_data =
      fml::FileMapping::CreateReadOnly(cache_file, "");
  if (!on_disk_data) {
    return nullptr;
  }
  if (on_disk_data->GetSize() != *file_size) {
    VALIDATION_LOG << "Pipeline cache size changed while it was opened.";
    return nullptr;
  }
  auto on_disk_header = PipelineCacheHeaderVK{};
  std::memcpy(&on_disk_header,             //
              on_disk_data->GetMapping(),  //
              sizeof(on_disk_header)       //
  );
  const auto current_header = PipelineCacheHeaderVK{props, 0u};
  if (!on_disk_header.IsCompatibleWith(current_header)) {
    FML_LOG(WARNING)
        << "Persisted pipeline cache is not compatible with current "
           "Vulkan context. Ignoring.";
    return nullptr;
  }
  // Zero sized data is known to cause issues.
  if (on_disk_header.data_size == 0u) {
    return nullptr;
  }
  const size_t available_data_bytes =
      *file_size - sizeof(PipelineCacheHeaderVK);
  if (on_disk_header.data_size > max_data_bytes ||
      on_disk_header.data_size > available_data_bytes) {
    VALIDATION_LOG << "Pipeline cache header declares an invalid data span.";
    return nullptr;
  }
  return std::make_unique<fml::NonOwnedMapping>(
      on_disk_data->GetMapping() + sizeof(on_disk_header),
      static_cast<size_t>(on_disk_header.data_size),
      [on_disk_data](auto, auto) {});
}

PipelineCacheHeaderVK::PipelineCacheHeaderVK() = default;

PipelineCacheHeaderVK::PipelineCacheHeaderVK(
    const VkPhysicalDeviceProperties& props,
    uint64_t p_data_size)
    : driver_version(props.driverVersion),
      vendor_id(props.vendorID),
      device_id(props.deviceID),
      data_size(p_data_size) {
  std::memcpy(uuid, props.pipelineCacheUUID, VK_UUID_SIZE);
}

bool PipelineCacheHeaderVK::IsCompatibleWith(
    const PipelineCacheHeaderVK& o) const {
  // Check for everything but the data size.
  return magic == o.magic &&                    //
         driver_version == o.driver_version &&  //
         vendor_id == o.vendor_id &&            //
         device_id == o.device_id &&            //
         abi == o.abi &&                        //
         std::memcmp(uuid, o.uuid, VK_UUID_SIZE) == 0;
}

}  // namespace impeller
