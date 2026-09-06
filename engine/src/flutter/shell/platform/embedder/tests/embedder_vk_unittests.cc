// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "embedder.h"
#include "embedder_engine.h"
#include "flutter/fml/file.h"
#include "flutter/fml/synchronization/count_down_latch.h"
#include "flutter/shell/platform/embedder/tests/embedder_config_builder.h"
#include "flutter/shell/platform/embedder/tests/embedder_test.h"
#include "flutter/shell/platform/embedder/tests/embedder_test_context_vulkan.h"
#include "flutter/shell/platform/embedder/tests/embedder_unittests_util.h"
#include "flutter/testing/testing.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkImageInfo.h"

// CREATE_NATIVE_ENTRY is leaky by design
// NOLINTBEGIN(clang-analyzer-core.StackAddressEscape)

namespace flutter {
namespace testing {

using EmbedderTest = testing::EmbedderTest;

constexpr FlutterAvioExtensionFeatures kExactSelectedTargetFeatures =
    kFlutterAvioExtensionFeaturePerDisplayVsync |
    kFlutterAvioExtensionFeatureRootRenderTarget |
    kFlutterAvioExtensionFeatureExplicitRenderCompletion |
    kFlutterAvioExtensionFeatureExactVsyncCancellation |
    kFlutterAvioExtensionFeatureFrameOpportunityOutcomes |
    kFlutterAvioExtensionFeatureRenderDeadline |
    kFlutterAvioExtensionFeatureSelectedTargetDamage;

////////////////////////////////////////////////////////////////////////////////
// Notice: Other Vulkan unit tests exist in embedder_gl_unittests.cc.
//         See https://github.com/flutter/flutter/issues/134322
////////////////////////////////////////////////////////////////////////////////

namespace {

struct VulkanProcInfo {
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
  PFN_vkQueueSubmit queue_submit_proc_addr = nullptr;
  bool did_call_queue_submit = false;
};

struct SelectedTargetTestContext {
  struct Target {
    FlutterBackingStore backing_store = {};
    FlutterBackingStoreContentState content_state = {};
    void (*destruction_callback)(void*) = nullptr;
    void* destruction_user_data = nullptr;
    bool created = false;
  };

  explicit SelectedTargetTestContext(EmbedderTestCompositor& compositor,
                                     size_t target_count = 1u)
      : compositor(compositor), target_count(target_count) {
    FML_CHECK(target_count > 0u && target_count <= targets.size());
    for (size_t index = 0; index < targets.size(); index++) {
      targets[index].content_state = {
          .struct_size = sizeof(FlutterBackingStoreContentState),
          .target_identifier = 7u + index,
          .content_epoch = 1u,
          .preserved_contents = false,
          .existing_damage = nullptr,
      };
    }
  }

  ~SelectedTargetTestContext() {
    for (const Target& target : targets) {
      if (target.destruction_callback != nullptr) {
        target.destruction_callback(target.destruction_user_data);
      }
    }
  }

  bool Create(const FlutterBackingStoreConfig* config,
              FlutterBackingStore* backing_store_out) {
    requested_sizes.emplace_back(config->size.width, config->size.height);
    Target& target = targets[create_count % target_count];
    if (!target.created) {
      target.backing_store.struct_size = sizeof(FlutterBackingStore);
      if (!compositor.CreateBackingStore(config, &target.backing_store)) {
        return false;
      }
      target.destruction_callback =
          target.backing_store.vulkan.destruction_callback;
      target.destruction_user_data = target.backing_store.vulkan.user_data;
      target.backing_store.vulkan.destruction_callback = [](void*) {};
      target.created = true;
    }
    target.backing_store.content_state = &target.content_state;
    *backing_store_out = target.backing_store;
    create_count++;
    return true;
  }

  bool Collect(const FlutterBackingStore* collected) {
    EXPECT_TRUE(
        std::any_of(targets.begin(), targets.end(), [&](const auto& target) {
          return target.created &&
                 target.backing_store.user_data == collected->user_data;
        }));
    collect_count++;
    return compositor.CollectBackingStore(collected);
  }

  bool Present(const FlutterPresentRenderTargetInfo& info) {
    present_count++;
    if (on_result && !on_result(info)) {
      return false;
    }
    if (info.status != kFlutterPresentRenderTargetStatusPresented) {
      return true;
    }

    FlutterLayer layer = {
        .struct_size = sizeof(FlutterLayer),
        .type = kFlutterLayerContentTypeBackingStore,
        .backing_store = info.backing_store,
        .offset = FlutterPoint{0.0, 0.0},
        .size = FlutterSize{0.0, 0.0},
        .backing_store_present_info =
            const_cast<FlutterBackingStorePresentInfo*>(
                info.backing_store_present_info),
        .presentation_time = 0,
        .shell_layer_role = kFlutterShellLayerRoleUnknown,
        .shell_visual_identifier = 0,
        .shell_visual_generation = 0,
        .shell_chrome_model_serial = 0,
    };
    const FlutterLayer* layers[] = {&layer};
    return compositor.Present(info.target_id, layers, 1);
  }

  void PreserveWithCatchUpDamage(size_t target = 0u) {
    FML_CHECK(target < target_count);
    targets[target].content_state.preserved_contents = true;
    targets[target].content_state.existing_damage = &catch_up_region;
  }

  void PreserveWithoutCatchUpDamage(size_t target = 0u) {
    FML_CHECK(target < target_count);
    targets[target].content_state.preserved_contents = true;
    targets[target].content_state.existing_damage = &empty_region;
  }

  void PreserveWithFullCatchUpDamage(size_t target = 0u) {
    FML_CHECK(target < target_count);
    targets[target].content_state.preserved_contents = true;
    targets[target].content_state.existing_damage = &full_region;
  }

  void Invalidate(size_t target = 0u) {
    FML_CHECK(target < target_count);
    targets[target].content_state.preserved_contents = false;
    targets[target].content_state.existing_damage = nullptr;
  }

  EmbedderTestCompositor& compositor;
  std::array<FlutterRect, 2> catch_up_rects = {
      FlutterRect{
          .left = 20.0,
          .top = 20.0,
          .right = 200.0,
          .bottom = 150.0,
      },
      FlutterRect{
          .left = 250.0,
          .top = 100.0,
          .right = 350.0,
          .bottom = 200.0,
      },
  };
  FlutterRegion catch_up_region = {
      .struct_size = sizeof(FlutterRegion),
      .rects_count = catch_up_rects.size(),
      .rects = catch_up_rects.data(),
  };
  FlutterRegion empty_region = {
      .struct_size = sizeof(FlutterRegion),
      .rects_count = 0,
      .rects = nullptr,
  };
  FlutterRect full_rect = {
      .left = 0.0,
      .top = 0.0,
      .right = 800.0,
      .bottom = 600.0,
  };
  FlutterRegion full_region = {
      .struct_size = sizeof(FlutterRegion),
      .rects_count = 1u,
      .rects = &full_rect,
  };
  std::array<Target, 3> targets;
  const size_t target_count;
  size_t create_count = 0;
  size_t collect_count = 0;
  size_t present_count = 0;
  std::vector<DlISize> requested_sizes;
  std::function<bool(const FlutterPresentRenderTargetInfo&)> on_result;
};

FlutterRenderTargetAcquisition AcquireSelectedTarget(
    const FlutterRenderTargetAcquisitionInfo* info,
    FlutterBackingStore* backing_store_out) {
  return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
                 ->Create(info->config, backing_store_out)
             ? kFlutterRenderTargetAcquisitionGranted
             : kFlutterRenderTargetAcquisitionBackpressured;
}

static_assert(std::is_trivially_destructible_v<VulkanProcInfo>);

VulkanProcInfo g_vulkan_proc_info;

VkResult QueueSubmit(VkQueue queue,
                     uint32_t submitCount,
                     const VkSubmitInfo* pSubmits,
                     VkFence fence) {
  FML_DCHECK(g_vulkan_proc_info.queue_submit_proc_addr != nullptr);
  g_vulkan_proc_info.did_call_queue_submit = true;
  return g_vulkan_proc_info.queue_submit_proc_addr(queue, submitCount, pSubmits,
                                                   fence);
}

template <size_t N>
int StrcmpFixed(const char* str1, const char (&str2)[N]) {
  return strncmp(str1, str2, N - 1);
}

PFN_vkVoidFunction GetDeviceProcAddr(VkDevice device, const char* pName) {
  FML_DCHECK(g_vulkan_proc_info.get_device_proc_addr != nullptr);
  if (StrcmpFixed(pName, "vkQueueSubmit") == 0) {
    g_vulkan_proc_info.queue_submit_proc_addr =
        reinterpret_cast<PFN_vkQueueSubmit>(
            g_vulkan_proc_info.get_device_proc_addr(device, pName));
    return reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit);
  }
  return g_vulkan_proc_info.get_device_proc_addr(device, pName);
}

PFN_vkVoidFunction GetInstanceProcAddr(VkInstance instance, const char* pName) {
  FML_DCHECK(g_vulkan_proc_info.get_instance_proc_addr != nullptr);
  if (StrcmpFixed(pName, "vkGetDeviceProcAddr") == 0) {
    g_vulkan_proc_info.get_device_proc_addr =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            g_vulkan_proc_info.get_instance_proc_addr(instance, pName));
    return reinterpret_cast<PFN_vkVoidFunction>(GetDeviceProcAddr);
  }
  return g_vulkan_proc_info.get_instance_proc_addr(instance, pName);
}

static_assert(
    std::is_same_v<decltype(&GetInstanceProcAddr), PFN_vkGetInstanceProcAddr>);
static_assert(
    std::is_same_v<decltype(&GetDeviceProcAddr), PFN_vkGetDeviceProcAddr>);
static_assert(std::is_same_v<decltype(&QueueSubmit), PFN_vkQueueSubmit>);
}  // namespace

TEST_F(EmbedderTest, CanGetVulkanEmbedderContext) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();
  EmbedderConfigBuilder builder(context);
}

TEST_F(EmbedderTest, CanSwapOutVulkanCalls) {
  fml::AutoResetWaitableEvent latch;

  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();
  context.AddIsolateCreateCallback([&latch]() { latch.Signal(); });
  context.SetVulkanInstanceProcAddressCallback(
      [](void* user_data, FlutterVulkanInstanceHandle instance,
         const char* name) -> void* {
        if (StrcmpFixed(name, "vkGetInstanceProcAddr") == 0) {
          g_vulkan_proc_info.get_instance_proc_addr =
              reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                  EmbedderTestContextVulkan::InstanceProcAddr(user_data,
                                                              instance, name));
          return reinterpret_cast<void*>(GetInstanceProcAddr);
        }
        return EmbedderTestContextVulkan::InstanceProcAddr(user_data, instance,
                                                           name);
      });

  EmbedderConfigBuilder builder(context);
  builder.SetSurface(DlISize(1024, 1024));
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Wait for the root isolate to launch.
  latch.Wait();
  engine.reset();
  EXPECT_TRUE(g_vulkan_proc_info.did_call_queue_submit);
}

////////////////////////////////////////////////////////////////////////////////
// Vulkan + Impeller compositor tests
////////////////////////////////////////////////////////////////////////////////

TEST_F(EmbedderTest, VulkanImpellerCompositorCanLaunchEngine) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetSurface(DlISize(800, 600));
  builder.SetCompositor();
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);
  builder.SetDartEntrypoint("render_impeller_test");

  // Verify the engine launches without hitting the "Unimplemented" error.
  fml::AutoResetWaitableEvent latch;
  context.AddIsolateCreateCallback([&latch]() { latch.Signal(); });

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  latch.Wait();
}

TEST_F(EmbedderTest, VulkanImpellerAcceptsExactResourceLifecycleConfig) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();
  fml::ScopedTemporaryDirectory cache_directory;

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetSurface(DlISize(64, 64));
  FlutterAvioExtensionRequest request = {
      .struct_size = sizeof(request),
      .version = FLUTTER_AVIO_EXTENSION_VERSION,
      .required_features = kFlutterAvioExtensionFeatureResourceLifecycleConfig,
  };
  FlutterAvioResourceLifecycleConfig resources = {
      .struct_size = sizeof(resources),
      .transient_max_entries = 3u,
      .transient_max_bytes = 16u * 1024u * 1024u,
      .pipeline_cache_policy = kFlutterAvioPipelineCacheReadOnly,
      .pipeline_cache_directory_fd = cache_directory.fd().get(),
      .pipeline_cache_max_bytes = 1024u * 1024u,
  };
  builder.GetProjectArgs().avio_extension_request = &request;
  builder.GetProjectArgs().avio_resource_lifecycle_config = &resources;

  auto engine = builder.InitializeEngine();
  ASSERT_TRUE(engine.is_valid());
  engine.reset();
  EXPECT_FALSE(
      fml::FileExists(cache_directory.fd(), "flutter.impeller.vkcache"));
}

TEST_F(EmbedderTest, VulkanImpellerCompositorPresentCallbackFires) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_impeller_test");
  builder.SetSurface(DlISize(800, 600));
  builder.SetCompositor();
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  fml::CountDownLatch latch(1);
  fml::CountDownLatch collect_latch(1);
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&collect_latch] { collect_latch.CountDown(); });
  context.GetCompositor().SetNextPresentCallback(
      [&](FlutterViewId view_id, const FlutterLayer** layers,
          size_t layers_count) {
        ASSERT_GT(layers_count, 0u);

        // The first layer should be a backing store rendered by Impeller.
        ASSERT_EQ(layers[0]->type, kFlutterLayerContentTypeBackingStore);
        ASSERT_EQ(layers[0]->backing_store->type,
                  kFlutterBackingStoreTypeVulkan);

        latch.CountDown();
      });

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);

  latch.Wait();
  engine.reset();
  collect_latch.Wait();
}

TEST_F(EmbedderTest, VulkanImpellerCompositorRendersScene) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_impeller_test");
  builder.SetSurface(DlISize(800, 600));
  builder.SetCompositor();
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  auto rendered_scene = context.GetNextSceneImage();
  fml::CountDownLatch collect_latch(1);
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&collect_latch] { collect_latch.CountDown(); });

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);

  // Verify that a scene was rendered (the future resolves with a non-null
  // image). Golden image comparison is deferred to a follow-up once the
  // reference images are captured with SwiftShader.
  auto scene_image = rendered_scene.get();
  ASSERT_TRUE(scene_image);
  engine.reset();
  collect_latch.Wait();
}

TEST_F(EmbedderTest, VulkanImpellerCompositorSkipsRootSurfaceAcquisition) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_impeller_test");
  builder.SetSurface(DlISize(800, 600));
  builder.SetCompositor();
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  // Verify that compositor present_view fires with valid layers, while
  // the root surface get_next_image/present_image callbacks are never called.
  fml::CountDownLatch latch(1);
  fml::CountDownLatch collect_latch(1);
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&collect_latch] { collect_latch.CountDown(); });
  context.GetCompositor().SetNextPresentCallback(
      [&](FlutterViewId view_id, const FlutterLayer** layers,
          size_t layers_count) {
        ASSERT_GT(layers_count, 0u);
        ASSERT_EQ(layers[0]->type, kFlutterLayerContentTypeBackingStore);
        ASSERT_EQ(layers[0]->backing_store->type,
                  kFlutterBackingStoreTypeVulkan);
        latch.CountDown();
      });

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);

  latch.Wait();

  // With compositor mode active, the root surface path should be skipped
  // entirely — no VkImage acquisition or presentation.
  EXPECT_EQ(context.GetNextImageCallCount(), 0u);
  EXPECT_EQ(context.GetSurfacePresentCount(), 0u);
  engine.reset();
  collect_latch.Wait();
}

TEST_F(EmbedderTest,
       SelectedTargetDamageReacquiresAndRepaintsExactRetainedTarget) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument("render_gradient_retained");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor(), 3u);
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  std::vector<uint64_t> target_identifiers;
  std::vector<int64_t> frame_damage_counts;
  std::vector<int64_t> buffer_damage_counts;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    statuses.push_back(info.status);
    const auto* content_state =
        info.backing_store ? info.backing_store->content_state : nullptr;
    target_identifiers.push_back(
        content_state ? content_state->target_identifier : UINT64_MAX);
    const auto* present_info = info.backing_store_present_info;
    frame_damage_counts.push_back(
        present_info && present_info->frame_damage
            ? static_cast<int64_t>(present_info->frame_damage->rects_count)
            : -1);
    buffer_damage_counts.push_back(
        present_info && present_info->buffer_damage
            ? static_cast<int64_t>(present_info->buffer_damage->rects_count)
            : -1);
    result_ready.Signal();
    return true;
  };

  auto full_repaint_scene = context.GetNextSceneImage();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  result_ready.Wait();
  target_collected.Wait();
  auto full_repaint_image = full_repaint_scene.get();
  ASSERT_TRUE(full_repaint_image);
  selected_target.PreserveWithCatchUpDamage(0u);

  auto second_target_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  result_ready.Wait();
  target_collected.Wait();
  auto second_target_image = second_target_scene.get();
  ASSERT_TRUE(second_target_image);
  EXPECT_TRUE(RasterImagesAreSame(full_repaint_image, second_target_image,
                                  /*allowable_different_pixels=*/0));
  selected_target.PreserveWithoutCatchUpDamage(1u);

  auto third_target_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  result_ready.Wait();
  target_collected.Wait();
  auto third_target_image = third_target_scene.get();
  ASSERT_TRUE(third_target_image);
  EXPECT_TRUE(RasterImagesAreSame(full_repaint_image, third_target_image,
                                  /*allowable_different_pixels=*/0));

  auto partial_repaint_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  result_ready.Wait();
  target_collected.Wait();
  auto partial_repaint_image = partial_repaint_scene.get();
  ASSERT_TRUE(partial_repaint_image);
  EXPECT_TRUE(RasterImagesAreSame(full_repaint_image, partial_repaint_image,
                                  /*allowable_different_pixels=*/0));

  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  result_ready.Wait();
  target_collected.Wait();

  EXPECT_EQ(statuses, (std::vector<FlutterPresentRenderTargetStatus>{
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusNoVisualChange,
                      }));
  EXPECT_EQ(target_identifiers, (std::vector<uint64_t>{7u, 8u, 9u, 7u, 8u}));
  EXPECT_EQ(frame_damage_counts, (std::vector<int64_t>{1, 0, 0, 0, -1}));
  // Logical frame damage above is per-frame and stays sparse. Buffer damage
  // is the rectangle the raster honored, and a multisampled root pass honors
  // none: it resolves over every pixel of whichever target it was handed,
  // including the preserved one reacquired on the fourth frame.
  EXPECT_EQ(buffer_damage_counts, (std::vector<int64_t>{-1, -1, -1, -1, -1}));
  EXPECT_EQ(selected_target.create_count, 5u);
  EXPECT_EQ(selected_target.collect_count, 5u);
  engine.reset();
}

TEST_F(EmbedderTest,
       SelectedTargetDamageClearsRemovedBlurWithFullRepaintParity) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument("render_partial_repaint_clear_and_blur");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor());
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  std::vector<int64_t> frame_damage_counts;
  std::vector<int64_t> buffer_damage_counts;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    statuses.push_back(info.status);
    const auto* present_info = info.backing_store_present_info;
    frame_damage_counts.push_back(
        present_info && present_info->frame_damage
            ? static_cast<int64_t>(present_info->frame_damage->rects_count)
            : -1);
    buffer_damage_counts.push_back(
        present_info && present_info->buffer_damage
            ? static_cast<int64_t>(present_info->buffer_damage->rects_count)
            : -1);
    result_ready.Signal();
    return true;
  };

  auto initial_scene = context.GetNextSceneImage();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto initial_image = initial_scene.get();
  ASSERT_TRUE(initial_image);

  selected_target.PreserveWithoutCatchUpDamage();
  auto partial_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto partial_image = partial_scene.get();
  ASSERT_TRUE(partial_image);

  selected_target.Invalidate();
  auto full_reference_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto full_reference_image = full_reference_scene.get();
  ASSERT_TRUE(full_reference_image);

  EXPECT_FALSE(RasterImagesAreSame(initial_image, partial_image,
                                   /*allowable_different_pixels=*/0));
  EXPECT_TRUE(RasterImagesAreSame(partial_image, full_reference_image,
                                  /*allowable_different_pixels=*/0));
  EXPECT_EQ(statuses, (std::vector<FlutterPresentRenderTargetStatus>{
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                      }));
  ASSERT_EQ(frame_damage_counts.size(), 3u);
  ASSERT_EQ(buffer_damage_counts.size(), 3u);
  // The blur's removal is real, sparse frame damage. No frame honors a
  // rectangle, because the multisampled root pass resolves over the whole
  // target whether or not the target preserved its contents.
  EXPECT_GT(frame_damage_counts[1], 0);
  EXPECT_EQ(buffer_damage_counts[1], -1);
  EXPECT_EQ(buffer_damage_counts[2], -1);
  EXPECT_EQ(selected_target.collect_count, 3u);
  engine.reset();
}

TEST_F(EmbedderTest,
       SelectedTargetDamageClearsFullyRemovedSceneBeforeNoVisualChange) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument("render_then_clear_to_empty");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor());
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  std::vector<int64_t> frame_damage_counts;
  std::vector<int64_t> buffer_damage_counts;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    statuses.push_back(info.status);
    const auto* present_info = info.backing_store_present_info;
    frame_damage_counts.push_back(
        present_info && present_info->frame_damage
            ? static_cast<int64_t>(present_info->frame_damage->rects_count)
            : -1);
    buffer_damage_counts.push_back(
        present_info && present_info->buffer_damage
            ? static_cast<int64_t>(present_info->buffer_damage->rects_count)
            : -1);
    result_ready.Signal();
    return true;
  };

  auto initial_scene = context.GetNextSceneImage();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto initial_image = initial_scene.get();
  ASSERT_TRUE(initial_image);

  selected_target.PreserveWithoutCatchUpDamage();
  auto cleared_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto cleared_image = cleared_scene.get();
  ASSERT_TRUE(cleared_image);

  selected_target.Invalidate();
  auto full_reference_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto full_reference_image = full_reference_scene.get();
  ASSERT_TRUE(full_reference_image);

  selected_target.PreserveWithoutCatchUpDamage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusNoVisualChange);

  EXPECT_FALSE(RasterImagesAreSame(initial_image, cleared_image,
                                   /*allowable_different_pixels=*/0));
  EXPECT_TRUE(RasterImagesAreSame(cleared_image, full_reference_image,
                                  /*allowable_different_pixels=*/0));
  EXPECT_EQ(statuses, (std::vector<FlutterPresentRenderTargetStatus>{
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusNoVisualChange,
                      }));
  ASSERT_EQ(frame_damage_counts.size(), 4u);
  ASSERT_EQ(buffer_damage_counts.size(), 4u);
  // Removing the scene is sparse frame damage, and the frame that removes it
  // still has to run: it is the pass that clears those pixels. It honors no
  // rectangle while doing so -- a multisampled root pass never does.
  EXPECT_GT(frame_damage_counts[1], 0);
  EXPECT_EQ(buffer_damage_counts[1], -1);
  EXPECT_EQ(buffer_damage_counts[2], -1);
  EXPECT_EQ(buffer_damage_counts[3], -1);
  EXPECT_EQ(selected_target.collect_count, 4u);
  engine.reset();
}

TEST_F(EmbedderTest,
       SelectedTargetDamageInitialEmptySceneDoesNotPublishUnknownTarget) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument(
      "empty_scene_posts_zero_layers_to_compositor");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor());
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  FlutterPresentRenderTargetStatus status =
      kFlutterPresentRenderTargetStatusInternalInvariantViolation;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    status = info.status;
    EXPECT_NE(info.backing_store, nullptr);
    EXPECT_EQ(info.backing_store_present_info, nullptr);
    result_ready.Signal();
    return true;
  };

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  EXPECT_EQ(status, kFlutterPresentRenderTargetStatusNoVisualChange);
  EXPECT_EQ(selected_target.create_count, 1u);
  EXPECT_EQ(selected_target.present_count, 1u);
  EXPECT_EQ(selected_target.collect_count, 1u);
  engine.reset();
}

TEST_F(EmbedderTest,
       SelectedTargetDamageKeepsSparseFrameDamageForTranslucentGap) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument(
      "render_disjoint_partial_repaint_with_translucent_gap");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor());
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  std::vector<int64_t> frame_damage_counts;
  std::vector<int64_t> buffer_damage_counts;
  std::vector<std::vector<FlutterRect>> frame_damage_rects;
  std::vector<std::vector<FlutterRect>> buffer_damage_rects;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    statuses.push_back(info.status);
    const auto* present_info = info.backing_store_present_info;
    const auto copy_region = [](const FlutterRegion* region) {
      if (region == nullptr || region->rects_count == 0u) {
        return std::vector<FlutterRect>{};
      }
      return std::vector<FlutterRect>(region->rects,
                                      region->rects + region->rects_count);
    };
    const FlutterRegion* frame_damage =
        present_info ? present_info->frame_damage : nullptr;
    const FlutterRegion* buffer_damage =
        present_info ? present_info->buffer_damage : nullptr;
    frame_damage_counts.push_back(
        frame_damage ? static_cast<int64_t>(frame_damage->rects_count) : -1);
    buffer_damage_counts.push_back(
        buffer_damage ? static_cast<int64_t>(buffer_damage->rects_count) : -1);
    frame_damage_rects.push_back(copy_region(frame_damage));
    buffer_damage_rects.push_back(copy_region(buffer_damage));
    result_ready.Signal();
    return true;
  };

  auto initial_scene = context.GetNextSceneImage();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto initial_image = initial_scene.get();
  ASSERT_TRUE(initial_image);

  selected_target.PreserveWithoutCatchUpDamage();
  auto partial_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto partial_image = partial_scene.get();
  ASSERT_TRUE(partial_image);

  selected_target.Invalidate();
  auto full_reference_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto full_reference_image = full_reference_scene.get();
  ASSERT_TRUE(full_reference_image);

  EXPECT_FALSE(RasterImagesAreSame(initial_image, partial_image,
                                   /*allowable_different_pixels=*/0));
  EXPECT_TRUE(RasterImagesAreSame(partial_image, full_reference_image,
                                  /*allowable_different_pixels=*/0));
  EXPECT_EQ(statuses, (std::vector<FlutterPresentRenderTargetStatus>{
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                      }));
  ASSERT_EQ(frame_damage_counts.size(), 3u);
  ASSERT_EQ(buffer_damage_counts.size(), 3u);
  // Logical frame damage stays sparse across the disjoint regions -- it is
  // what accumulates catch-up damage for the other buffers, and coalescing it
  // to a bounding box would sweep the translucent gap between them into every
  // other target's history.
  EXPECT_GT(frame_damage_counts[1], 1);
  ASSERT_GT(frame_damage_rects[1].size(), 1u);

  // No frame honors a damage rectangle: the root pass is multisampled, so it
  // resolves over every pixel of the target and reports no narrower update.
  EXPECT_EQ(buffer_damage_counts, (std::vector<int64_t>{-1, -1, -1}));
  EXPECT_TRUE(buffer_damage_rects[1].empty());

  // The gap between the two damaged regions is translucent, so a frame that
  // failed to repaint it -- or repainted it over stale contents -- would not
  // match the forced full repaint above.
  EXPECT_EQ(selected_target.collect_count, 3u);
  engine.reset();
}

TEST_F(EmbedderTest, SelectedTargetDamageFullFallbackClearsPreservedTarget) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument("render_partial_repaint_clear_and_blur");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor());
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  std::vector<int64_t> buffer_damage_counts;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    statuses.push_back(info.status);
    const auto* present_info = info.backing_store_present_info;
    buffer_damage_counts.push_back(
        present_info && present_info->buffer_damage
            ? static_cast<int64_t>(present_info->buffer_damage->rects_count)
            : -1);
    result_ready.Signal();
    return true;
  };

  auto initial_scene = context.GetNextSceneImage();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto initial_image = initial_scene.get();
  ASSERT_TRUE(initial_image);

  selected_target.PreserveWithFullCatchUpDamage();
  auto fallback_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto fallback_image = fallback_scene.get();
  ASSERT_TRUE(fallback_image);

  selected_target.Invalidate();
  auto full_reference_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto full_reference_image = full_reference_scene.get();
  ASSERT_TRUE(full_reference_image);

  EXPECT_FALSE(RasterImagesAreSame(initial_image, fallback_image,
                                   /*allowable_different_pixels=*/0));
  EXPECT_TRUE(RasterImagesAreSame(fallback_image, full_reference_image,
                                  /*allowable_different_pixels=*/0));
  EXPECT_EQ(statuses, (std::vector<FlutterPresentRenderTargetStatus>{
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                      }));
  EXPECT_EQ(buffer_damage_counts, (std::vector<int64_t>{-1, -1, -1}));
  EXPECT_EQ(selected_target.collect_count, 3u);
  engine.reset();
}

TEST_F(EmbedderTest, SelectedTargetDamageClearsRecycledSaveLayerTargets) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument(
      "render_partial_repaint_through_recycled_save_layers");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor());
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  std::vector<int64_t> buffer_damage_counts;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    statuses.push_back(info.status);
    const auto* present_info = info.backing_store_present_info;
    const FlutterRegion* buffer_damage =
        present_info ? present_info->buffer_damage : nullptr;
    buffer_damage_counts.push_back(
        buffer_damage ? static_cast<int64_t>(buffer_damage->rects_count) : -1);
    result_ready.Signal();
    return true;
  };

  auto initial_scene = context.GetNextSceneImage();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto initial_image = initial_scene.get();
  ASSERT_TRUE(initial_image);

  // The second frame retires the first frame's save layer offscreen into the
  // render target cache and asks for a same-sized one, which it only partly
  // paints.
  selected_target.PreserveWithoutCatchUpDamage();
  auto partial_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto partial_image = partial_scene.get();
  ASSERT_TRUE(partial_image);

  // Same scene, forced through the full-repaint path. Any pixel the partial
  // frame inherited from a recycled offscreen shows up as a difference here.
  selected_target.Invalidate();
  auto full_reference_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto full_reference_image = full_reference_scene.get();
  ASSERT_TRUE(full_reference_image);

  EXPECT_FALSE(RasterImagesAreSame(initial_image, partial_image,
                                   /*allowable_different_pixels=*/0));
  EXPECT_TRUE(RasterImagesAreSame(partial_image, full_reference_image,
                                  /*allowable_different_pixels=*/0));
  EXPECT_EQ(statuses, (std::vector<FlutterPresentRenderTargetStatus>{
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                      }));
  // Every frame resolves over its whole target, so none of them honors a
  // damage rectangle. The recycled save-layer target is the point of the
  // comparison above: it must not arrive carrying its previous tenant.
  ASSERT_EQ(buffer_damage_counts.size(), 3u);
  EXPECT_EQ(buffer_damage_counts, (std::vector<int64_t>{-1, -1, -1}));
  engine.reset();
}

TEST_F(EmbedderTest, SelectedTargetDamageRefusedWhenRootPassNeedsReadback) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument(
      "render_partial_repaint_with_root_backdrop_filter");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor());
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  std::vector<int64_t> buffer_damage_counts;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    statuses.push_back(info.status);
    const auto* present_info = info.backing_store_present_info;
    const FlutterRegion* buffer_damage =
        present_info ? present_info->buffer_damage : nullptr;
    buffer_damage_counts.push_back(
        buffer_damage ? static_cast<int64_t>(buffer_damage->rects_count) : -1);
    result_ready.Signal();
    return true;
  };

  auto initial_scene = context.GetNextSceneImage();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto initial_image = initial_scene.get();
  ASSERT_TRUE(initial_image);

  // The target is preserved and the frame damage is sparse, so the engine
  // would ordinarily raster only the damage. It must not: the root backdrop
  // filter makes Impeller copy a whole offscreen over this target.
  selected_target.PreserveWithoutCatchUpDamage();
  auto readback_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto readback_image = readback_scene.get();
  ASSERT_TRUE(readback_image);

  // Full catch-up damage rather than an invalidated target: the target stays
  // preserved, so only the damage differs between the two frames.
  selected_target.PreserveWithFullCatchUpDamage();
  auto full_reference_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto full_reference_image = full_reference_scene.get();
  ASSERT_TRUE(full_reference_image);

  EXPECT_FALSE(RasterImagesAreSame(initial_image, readback_image,
                                   /*allowable_different_pixels=*/0));
  EXPECT_TRUE(RasterImagesAreSame(readback_image, full_reference_image,
                                  /*allowable_different_pixels=*/0));
  EXPECT_EQ(statuses, (std::vector<FlutterPresentRenderTargetStatus>{
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                          kFlutterPresentRenderTargetStatusPresented,
                      }));
  // No buffer damage on any of the three frames: the readback frame published
  // a full repaint rather than a rectangle it did not honor.
  EXPECT_EQ(buffer_damage_counts, (std::vector<int64_t>{-1, -1, -1}));
  engine.reset();
}

namespace {

// Count pixels the raster covered partially. Impeller produces these only by
// rastering multisampled and resolving, so their presence is the observable
// consequence of a multisampled root pass and their absence is the observable
// consequence of a single-sample one.
size_t CountPartiallyCoveredPixels(const sk_sp<SkImage>& image) {
  if (!image) {
    return 0u;
  }
  const SkImageInfo info = SkImageInfo::MakeN32Premul(
      image->width(), image->height(), SkColorSpace::MakeSRGB());
  const size_t row_bytes = info.minRowBytes();
  std::vector<uint8_t> pixels(info.computeByteSize(row_bytes));
  if (!image->readPixels(info, pixels.data(), row_bytes, 0, 0)) {
    return 0u;
  }

  size_t partial = 0u;
  for (int y = 0; y < image->height(); y++) {
    const uint32_t* row =
        reinterpret_cast<const uint32_t*>(pixels.data() + y * row_bytes);
    for (int x = 0; x < image->width(); x++) {
      const uint32_t alpha = SkColorGetA(row[x]);
      if (alpha != 0u && alpha != 255u) {
        partial++;
      }
    }
  }
  return partial;
}

}  // namespace

// The defect this pins: the root pass used to drop to a single sample for any
// target whose contents the embedder had preserved, which is every target in a
// steady-state compositor session. Nothing downstream reported it -- the
// frames were correct, just unantialiased -- so the only way to catch a
// regression is to look at the pixels along an edge only multisampling can
// cover partially.
TEST_F(EmbedderTest, SelectedTargetDamageKeepsPreservedTargetMultisampled) {
  auto& context = GetEmbedderContext<EmbedderTestContextVulkan>();

  EmbedderConfigBuilder builder(context);
  builder.AddCommandLineArgument("--enable-impeller");
  builder.SetDartEntrypoint("render_selected_target_ready");
  builder.AddDartEntrypointArgument("render_clipped_diagonal_edge");
  fml::AutoResetWaitableEvent dart_ready;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&dart_ready](Dart_NativeArguments args) { dart_ready.Signal(); }));
  builder.SetSurface(DlISize(800, 600));
  builder.SetRootRenderTargetCompositor(
      /*avoid_backing_store_cache=*/false, kExactSelectedTargetFeatures);
  builder.SetRenderTargetType(
      EmbedderTestBackingStoreProducer::RenderTargetType::kVulkanImage);

  SelectedTargetTestContext selected_target(context.GetCompositor());
  builder.GetCompositor().user_data = &selected_target;
  builder.GetCompositor().acquire_render_target_callback =
      AcquireSelectedTarget;
  builder.GetCompositor().collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<SelectedTargetTestContext*>(user_data)->Collect(
            backing_store);
      };
  builder.GetCompositor().present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        return reinterpret_cast<SelectedTargetTestContext*>(info->user_data)
            ->Present(*info);
      };

  fml::AutoResetWaitableEvent result_ready;
  fml::AutoResetWaitableEvent target_collected;
  std::vector<FlutterPresentRenderTargetStatus> statuses;
  context.GetCompositor().AddOnCollectRenderTargetCallback(
      [&] { target_collected.Signal(); });
  selected_target.on_result = [&](const FlutterPresentRenderTargetInfo& info) {
    statuses.push_back(info.status);
    result_ready.Signal();
    return true;
  };

  auto unpreserved_scene = context.GetNextSceneImage();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Window metrics must follow Dart callback registration. Otherwise the
  // first frame can be delivered while the JIT entrypoint is still starting.
  ASSERT_FALSE(dart_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  event.pixel_ratio = 1.0;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto unpreserved_image = unpreserved_scene.get();
  ASSERT_TRUE(unpreserved_image);

  // Hand the same target back preserved. This is the state that used to select
  // a single-sample root pass; the scene is unchanged, so full catch-up damage
  // is what gives the frame something to raster.
  selected_target.PreserveWithFullCatchUpDamage();
  auto preserved_scene = context.GetNextSceneImage();
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_FALSE(result_ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  ASSERT_EQ(statuses.back(), kFlutterPresentRenderTargetStatusPresented);
  ASSERT_FALSE(
      target_collected.WaitWithTimeout(fml::TimeDelta::FromSeconds(5)));
  auto preserved_image = preserved_scene.get();
  ASSERT_TRUE(preserved_image);

  // The diagonal runs 580 pixels across and 430 down, so a multisampled edge
  // covers hundreds of pixels partially. A single-sample one covers none: the
  // threshold is far below what multisampling produces and far above the zero
  // a hard edge produces, so it does not need to track edge length exactly.
  const size_t unpreserved_partial =
      CountPartiallyCoveredPixels(unpreserved_image);
  const size_t preserved_partial = CountPartiallyCoveredPixels(preserved_image);
  EXPECT_GT(unpreserved_partial, 100u);
  EXPECT_GT(preserved_partial, 100u);

  // Same scene, same coverage: preserving a target changes nothing about how
  // its frame is rastered.
  EXPECT_TRUE(RasterImagesAreSame(unpreserved_image, preserved_image,
                                  /*allowable_different_pixels=*/0));
  engine.reset();
}

}  // namespace testing
}  // namespace flutter

// NOLINTEND(clang-analyzer-core.StackAddressEscape)
