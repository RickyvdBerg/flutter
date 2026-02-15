// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/gpu/gpu_surface_vulkan_impeller.h"

#include <memory>
#include <vector>

#include "flow/surface_frame.h"
#include "flutter/display_list/geometry/dl_geometry_types.h"
#include "flutter/display_list/geometry/dl_region.h"
#include "flutter/fml/make_copyable.h"
#include "fml/trace_event.h"
#include "impeller/core/formats.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/display_list/dl_dispatcher.h"
#include "impeller/renderer/backend/vulkan/command_buffer_vk.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/backend/vulkan/surface_context_vk.h"
#include "impeller/renderer/backend/vulkan/swapchain/surface_vk.h"
#include "impeller/renderer/render_target.h"
#include "impeller/renderer/surface.h"
#include "impeller/typographer/backends/skia/typographer_context_skia.h"

namespace flutter {

namespace {

constexpr size_t kMaxTrackedDamageImages = 16u;

std::vector<SkIRect> DamageRectsOrFull(
    const SurfaceFrame::SubmitInfo& submit_info,
    impeller::ISize target_size) {
  std::vector<SkIRect> damage_rects;
  if (submit_info.buffer_damage.has_value()) {
    for (const auto& rect :
         submit_info.buffer_damage->getRects(/*deband=*/true)) {
      damage_rects.push_back(ToSkIRect(rect));
    }
  }
  if (damage_rects.empty()) {
    damage_rects.push_back(SkIRect::MakeWH(target_size.width,
                                           target_size.height));
  }
  return damage_rects;
}

}  // namespace

class WrappedTextureSourceVK : public impeller::TextureSourceVK {
 public:
  explicit WrappedTextureSourceVK(impeller::vk::Image image,
                                  impeller::vk::UniqueImageView image_view,
                                  impeller::TextureDescriptor desc)
      : TextureSourceVK(desc),
        image_(image),
        image_view_(std::move(image_view)) {}

  ~WrappedTextureSourceVK() override = default;

 private:
  impeller::vk::Image GetImage() const override { return image_; }

  impeller::vk::ImageView GetImageView() const override {
    return image_view_.get();
  }

  impeller::vk::ImageView GetRenderTargetView() const override {
    return image_view_.get();
  }

  bool IsSwapchainImage() const override { return true; }

  impeller::vk::Image image_;
  impeller::vk::UniqueImageView image_view_;
};

GPUSurfaceVulkanImpeller::GPUSurfaceVulkanImpeller(
    GPUSurfaceVulkanDelegate* delegate,
    std::shared_ptr<impeller::Context> context,
    bool render_to_surface)
    : delegate_(delegate), render_to_surface_(render_to_surface) {
  if (!context || !context->IsValid()) {
    return;
  }

  auto aiks_context = std::make_shared<impeller::AiksContext>(
      context, impeller::TypographerContextSkia::Make());
  if (!aiks_context->IsValid()) {
    return;
  }

  impeller_context_ = std::move(context);
  aiks_context_ = std::move(aiks_context);
  is_valid_ = !!aiks_context_;
}

// |Surface|
GPUSurfaceVulkanImpeller::~GPUSurfaceVulkanImpeller() = default;

// |Surface|
bool GPUSurfaceVulkanImpeller::IsValid() {
  return is_valid_;
}

// |Surface|
std::unique_ptr<SurfaceFrame> GPUSurfaceVulkanImpeller::AcquireFrame(
    const DlISize& size) {
  if (!IsValid()) {
    FML_LOG(ERROR) << "Vulkan surface was invalid.";
    return nullptr;
  }

  if (size.IsEmpty()) {
    FML_LOG(ERROR) << "Vulkan surface was asked for an empty frame.";
    return nullptr;
  }

  if (!render_to_surface_) {
    return std::make_unique<SurfaceFrame>(
        nullptr, SurfaceFrame::FramebufferInfo(),
        [](const SurfaceFrame& surface_frame, DlCanvas* canvas) {
          return true;
        },
        [](const SurfaceFrame& surface_frame) { return true; }, size);
  }

  if (delegate_ == nullptr) {
    auto& context_vk = impeller::SurfaceContextVK::Cast(*impeller_context_);
    std::unique_ptr<impeller::Surface> surface =
        context_vk.AcquireNextSurface();

    if (!surface) {
      FML_LOG(ERROR) << "No surface available.";
      return nullptr;
    }

    impeller::RenderTarget render_target = surface->GetRenderTarget();
    SurfaceFrame::EncodeCallback encode_callback = [aiks_context =
                                                        aiks_context_,  //
                                                    render_target  //
    ](SurfaceFrame& surface_frame, DlCanvas* canvas) mutable -> bool {
      if (!aiks_context) {
        return false;
      }

      auto display_list = surface_frame.BuildDisplayList();
      if (!display_list) {
        FML_LOG(ERROR) << "Could not build display list for surface frame.";
        return false;
      }

      const auto damage_rects =
          DamageRectsOrFull(surface_frame.submit_info(),
                            render_target.GetRenderTargetSize());
      return impeller::RenderToTarget(
          aiks_context->GetContentContext(),                                //
          render_target,                                                    //
          display_list,                                                     //
          damage_rects,                                                     //
          /*reset_host_buffer=*/surface_frame.submit_info().frame_boundary  //
      );
    };

    return std::make_unique<SurfaceFrame>(
        nullptr,                          // surface
        SurfaceFrame::FramebufferInfo{},  // framebuffer info
        encode_callback,                  // encode callback
        fml::MakeCopyable([surface = std::move(surface)](const SurfaceFrame&) {
          return surface->Present();
        }),       // submit callback
        size,     // frame size
        nullptr,  // context result
        true      // display list fallback
    );
  } else {
    FlutterVulkanImage flutter_image = delegate_->AcquireImage(size);
    if (!flutter_image.image) {
      FML_LOG(ERROR) << "Invalid VkImage given by the embedder.";
      return nullptr;
    }
    impeller::vk::Format vk_format =
        static_cast<impeller::vk::Format>(flutter_image.format);
    std::optional<impeller::PixelFormat> format =
        impeller::VkFormatToImpellerFormat(vk_format);
    if (!format.has_value()) {
      FML_LOG(ERROR) << "Unsupported pixel format: "
                     << impeller::vk::to_string(vk_format);
      return nullptr;
    }

    impeller::ContextVK& context_vk =
        impeller::ContextVK::Cast(*impeller_context_);

    context_vk.DisposeThreadLocalCachedResources();

    impeller::vk::Image vk_image =
        impeller::vk::Image(reinterpret_cast<VkImage>(flutter_image.image));

    impeller::TextureDescriptor desc;
    desc.format = format.value();
    desc.size = impeller::ISize{size.width, size.height};
    desc.storage_mode = impeller::StorageMode::kDevicePrivate;
    desc.mip_count = 1;
    desc.compression_type = impeller::CompressionType::kLossless;
    desc.usage = impeller::TextureUsage::kRenderTarget;

    impeller::vk::ImageViewCreateInfo view_info = {};
    view_info.viewType = impeller::vk::ImageViewType::e2D;
    view_info.format = ToVKImageFormat(desc.format);
    view_info.subresourceRange.aspectMask =
        impeller::vk::ImageAspectFlagBits::eColor;
    view_info.subresourceRange.baseMipLevel = 0u;
    view_info.subresourceRange.baseArrayLayer = 0u;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    view_info.image = vk_image;

    auto [result, image_view] =
        context_vk.GetDevice().createImageViewUnique(view_info);
    if (result != impeller::vk::Result::eSuccess) {
      FML_LOG(ERROR) << "Failed to create image view for provided image: "
                     << impeller::vk::to_string(result);
      return nullptr;
    }

    impeller::ISize frame_size{size.width, size.height};
    if (transients_ == nullptr || transients_size_ != frame_size) {
      transients_ = std::make_shared<impeller::SwapchainTransientsVK>(
          impeller_context_, desc,
          /*enable_msaa=*/true);
      transients_size_ = frame_size;
    }

    auto wrapped_onscreen = std::make_shared<WrappedTextureSourceVK>(
        vk_image, std::move(image_view), desc);
    auto surface = impeller::SurfaceVK::WrapSwapchainImage(
        transients_, wrapped_onscreen, [&]() -> bool { return true; });
    impeller::RenderTarget render_target = surface->GetRenderTarget();
    uint64_t image_key = flutter_image.image;
    if (!disable_partial_repaint_ && damage_ &&
        damage_->find(image_key) == damage_->end() &&
        damage_->size() >= kMaxTrackedDamageImages) {
      damage_->clear();
    }

    SurfaceFrame::EncodeCallback encode_callback =
        fml::MakeCopyable([aiks_context = aiks_context_,  //
                           damage = damage_,
                           disable_partial_repaint = disable_partial_repaint_,
                           render_target,
                           image_key   //
    ](SurfaceFrame& surface_frame, DlCanvas* canvas) mutable -> bool {
      if (!aiks_context) {
        return false;
      }

      auto display_list = surface_frame.BuildDisplayList();
      if (!display_list) {
        FML_LOG(ERROR) << "Could not build display list for surface frame.";
        return false;
      }

      if (!disable_partial_repaint && damage) {
        for (auto& entry : *damage) {
          if (entry.first != image_key) {
            // Accumulate damage for other framebuffers.
            if (surface_frame.submit_info().frame_damage) {
              auto bounds = surface_frame.submit_info().frame_damage->bounds();
              entry.second.join(ToSkIRect(bounds));
            }
          }
        }
        // Reset accumulated damage for current framebuffer.
        (*damage)[image_key] = SkIRect::MakeEmpty();
      }

      const auto damage_rects =
          DamageRectsOrFull(surface_frame.submit_info(),
                            render_target.GetRenderTargetSize());

      return impeller::RenderToTarget(
          aiks_context->GetContentContext(),                                //
          render_target,                                                    //
          display_list,                                                     //
          damage_rects,                                                     //
          /*reset_host_buffer=*/surface_frame.submit_info().frame_boundary  //
      );
    });

    SurfaceFrame::SubmitCallback submit_callback =
        [image = flutter_image, delegate = delegate_,
         impeller_context = impeller_context_,
         wrapped_onscreen](const SurfaceFrame&) -> bool {
      TRACE_EVENT0("flutter", "GPUSurfaceVulkan::PresentImage");

      {
        const auto& context = impeller::ContextVK::Cast(*impeller_context);

        //----------------------------------------------------------------------------
        /// Transition the image to color-attachment-optimal.
        ///
        auto cmd_buffer = context.CreateCommandBuffer();

        auto vk_final_cmd_buffer =
            impeller::CommandBufferVK::Cast(*cmd_buffer).GetCommandBuffer();
        {
          impeller::BarrierVK barrier;
          barrier.new_layout =
              impeller::vk::ImageLayout::eColorAttachmentOptimal;
          barrier.cmd_buffer = vk_final_cmd_buffer;
          barrier.src_access =
              impeller::vk::AccessFlagBits::eColorAttachmentWrite;
          barrier.src_stage =
              impeller::vk::PipelineStageFlagBits::eColorAttachmentOutput;
          barrier.dst_access = {};
          barrier.dst_stage =
              impeller::vk::PipelineStageFlagBits::eBottomOfPipe;

          if (!wrapped_onscreen->SetLayout(barrier).ok()) {
            return false;
          }
        }
        if (!context.GetCommandQueue()->Submit({cmd_buffer}).ok()) {
          return false;
        }
      }

      return delegate->PresentImage(reinterpret_cast<VkImage>(image.image),
                                    static_cast<VkFormat>(image.format));
    };

    SurfaceFrame::FramebufferInfo framebuffer_info{.supports_readback = true};

    if (!disable_partial_repaint_) {
      // Provide accumulated damage to rasterizer (area in current framebuffer
      // that lags behind front buffer).
      auto i = damage_->find(image_key);
      if (i != damage_->end()) {
        framebuffer_info.existing_damage = DlRegion(ToDlIRect(i->second));
      }
      framebuffer_info.supports_partial_repaint = true;
    }

    return std::make_unique<SurfaceFrame>(nullptr,           // surface
                                          framebuffer_info,  // framebuffer info
                                          encode_callback,   // encode callback
                                          submit_callback,
                                          size,     // frame size
                                          nullptr,  // context result
                                          true      // display list fallback
    );
  }
}

// |Surface|
DlMatrix GPUSurfaceVulkanImpeller::GetRootTransformation() const {
  // This backend does not currently support root surface transformations. Just
  // return identity.
  return {};
}

// |Surface|
GrDirectContext* GPUSurfaceVulkanImpeller::GetContext() {
  // Impeller != Skia.
  return nullptr;
}

// |Surface|
std::unique_ptr<GLContextResult>
GPUSurfaceVulkanImpeller::MakeRenderContextCurrent() {
  // This backend has no such concept.
  return std::make_unique<GLContextDefaultResult>(true);
}

// |Surface|
bool GPUSurfaceVulkanImpeller::EnableRasterCache() const {
  return false;
}

// |Surface|
std::shared_ptr<impeller::AiksContext>
GPUSurfaceVulkanImpeller::GetAiksContext() const {
  return aiks_context_;
}

}  // namespace flutter
