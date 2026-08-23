// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/tests/embedder_config_builder.h"

#include <atomic>
#include <mutex>

#include "flutter/common/constants.h"
#include "flutter/runtime/dart_vm.h"
#include "flutter/shell/platform/embedder/embedder.h"
#include "tests/embedder_test_context.h"
#include "third_party/skia/include/core/SkImage.h"

namespace flutter::testing {

struct ExactFramePump {
  void OnBaton(intptr_t baton, FlutterEngineDisplayId display_id) {
    FlutterEngine attached_engine = nullptr;
    {
      std::scoped_lock lock(mutex);
      attached_engine = engine;
      if (attached_engine == nullptr) {
        pending_batons.emplace_back(baton, display_id);
        return;
      }
    }
    ReturnBaton(attached_engine, baton, display_id);
  }

  void Attach(FlutterEngine attached_engine) {
    std::vector<std::pair<intptr_t, FlutterEngineDisplayId>> pending;
    {
      std::scoped_lock lock(mutex);
      engine = attached_engine;
      pending.swap(pending_batons);
    }
    for (const auto& [baton, display_id] : pending) {
      ReturnBaton(attached_engine, baton, display_id);
    }
  }

 private:
  void ReturnBaton(FlutterEngine attached_engine,
                   intptr_t baton,
                   FlutterEngineDisplayId display_id) {
    const FlutterViewId target = kFlutterImplicitViewId;
    const uint64_t now = FlutterEngineGetCurrentTime();
    const FlutterFrameOpportunityId opportunity_id =
        next_opportunity_id.fetch_add(1u);
    FML_CHECK(FlutterEngineOnVsyncForDisplayWithOpportunity(
                  attached_engine, baton, display_id, opportunity_id, &target,
                  1u, now, now + 15'000'000u, now + 16'666'667u) == kSuccess);
  }

  std::mutex mutex;
  FlutterEngine engine = nullptr;
  std::vector<std::pair<intptr_t, FlutterEngineDisplayId>> pending_batons;
  std::atomic<FlutterFrameOpportunityId> next_opportunity_id = 1u;
};

EmbedderConfigBuilder::EmbedderConfigBuilder(
    EmbedderTestContext& context,
    InitializationPreference preference)
    : context_(context) {
  project_args_.struct_size = sizeof(project_args_);
  project_args_.shutdown_dart_vm_when_done = true;
  project_args_.platform_message_callback =
      [](const FlutterPlatformMessage* message, void* context) {
        reinterpret_cast<EmbedderTestContext*>(context)
            ->PlatformMessageCallback(message);
      };

  custom_task_runners_.struct_size = sizeof(FlutterCustomTaskRunners);

  // The first argument is always the executable name. Don't make tests have to
  // do this manually.
  AddCommandLineArgument("embedder_unittest");

  if (preference != InitializationPreference::kNoInitialize) {
    SetAssetsPath();
    SetIsolateCreateCallbackHook();
    SetSemanticsCallbackHooks();
    SetLogMessageCallbackHook();
    SetLocalizationCallbackHooks();
    SetChannelUpdateCallbackHook();
    SetViewFocusChangeRequestHook();
    AddCommandLineArgument("--disable-vm-service");

    if (preference == InitializationPreference::kSnapshotsInitialize ||
        preference == InitializationPreference::kMultiAOTInitialize) {
      SetSnapshots();
    }
    if (preference == InitializationPreference::kAOTDataInitialize ||
        preference == InitializationPreference::kMultiAOTInitialize) {
      SetAOTDataElf();
    }
  }
}

EmbedderConfigBuilder::~EmbedderConfigBuilder() = default;

FlutterProjectArgs& EmbedderConfigBuilder::GetProjectArgs() {
  return project_args_;
}

void EmbedderConfigBuilder::SetAssetsPath() {
  project_args_.assets_path = context_.GetAssetsPath().c_str();
}

void EmbedderConfigBuilder::SetSnapshots() {
  if (auto mapping = context_.GetVMSnapshotData()) {
    project_args_.vm_snapshot_data = mapping->GetMapping();
    project_args_.vm_snapshot_data_size = mapping->GetSize();
  }

  if (auto mapping = context_.GetVMSnapshotInstructions()) {
    project_args_.vm_snapshot_instructions = mapping->GetMapping();
    project_args_.vm_snapshot_instructions_size = mapping->GetSize();
  }

  if (auto mapping = context_.GetIsolateSnapshotData()) {
    project_args_.isolate_snapshot_data = mapping->GetMapping();
    project_args_.isolate_snapshot_data_size = mapping->GetSize();
  }

  if (auto mapping = context_.GetIsolateSnapshotInstructions()) {
    project_args_.isolate_snapshot_instructions = mapping->GetMapping();
    project_args_.isolate_snapshot_instructions_size = mapping->GetSize();
  }
}

void EmbedderConfigBuilder::SetAOTDataElf() {
  project_args_.aot_data = context_.GetAOTData();
}

void EmbedderConfigBuilder::SetIsolateCreateCallbackHook() {
  project_args_.root_isolate_create_callback =
      EmbedderTestContext::GetIsolateCreateCallbackHook();
}

void EmbedderConfigBuilder::SetSemanticsCallbackHooks() {
  project_args_.update_semantics_callback2 =
      context_.GetUpdateSemanticsCallback2Hook();
  project_args_.update_semantics_callback =
      context_.GetUpdateSemanticsCallbackHook();
  project_args_.update_semantics_node_callback =
      context_.GetUpdateSemanticsNodeCallbackHook();
  project_args_.update_semantics_custom_action_callback =
      context_.GetUpdateSemanticsCustomActionCallbackHook();
}

void EmbedderConfigBuilder::SetLogMessageCallbackHook() {
  project_args_.log_message_callback =
      EmbedderTestContext::GetLogMessageCallbackHook();
}

void EmbedderConfigBuilder::SetChannelUpdateCallbackHook() {
  project_args_.channel_update_callback =
      context_.GetChannelUpdateCallbackHook();
}

void EmbedderConfigBuilder::SetViewFocusChangeRequestHook() {
  project_args_.view_focus_change_request_callback =
      context_.GetViewFocusChangeRequestCallbackHook();
}

void EmbedderConfigBuilder::SetLogTag(std::string tag) {
  log_tag_ = std::move(tag);
  project_args_.log_tag = log_tag_.c_str();
}

void EmbedderConfigBuilder::SetLocalizationCallbackHooks() {
  project_args_.compute_platform_resolved_locale_callback =
      EmbedderTestContext::GetComputePlatformResolvedLocaleCallbackHook();
}

void EmbedderConfigBuilder::SetExecutableName(std::string executable_name) {
  if (executable_name.empty()) {
    return;
  }
  command_line_arguments_[0] = std::move(executable_name);
}

void EmbedderConfigBuilder::SetDartEntrypoint(std::string entrypoint) {
  if (entrypoint.empty()) {
    return;
  }

  dart_entrypoint_ = std::move(entrypoint);
  project_args_.custom_dart_entrypoint = dart_entrypoint_.c_str();
}

void EmbedderConfigBuilder::AddCommandLineArgument(std::string arg) {
  if (arg.empty()) {
    return;
  }

  command_line_arguments_.emplace_back(std::move(arg));
}

void EmbedderConfigBuilder::AddDartEntrypointArgument(std::string arg) {
  if (arg.empty()) {
    return;
  }

  dart_entrypoint_arguments_.emplace_back(std::move(arg));
}

void EmbedderConfigBuilder::SetPlatformTaskRunner(
    const FlutterTaskRunnerDescription* runner) {
  if (runner == nullptr) {
    return;
  }
  custom_task_runners_.platform_task_runner = runner;
  project_args_.custom_task_runners = &custom_task_runners_;
}

void EmbedderConfigBuilder::SetUITaskRunner(
    const FlutterTaskRunnerDescription* runner) {
  if (runner == nullptr) {
    return;
  }
  custom_task_runners_.ui_task_runner = runner;
  project_args_.custom_task_runners = &custom_task_runners_;
}

void EmbedderConfigBuilder::SetupVsyncCallback() {
  project_args_.vsync_callback = [](void* user_data, intptr_t baton) {
    auto context = reinterpret_cast<EmbedderTestContext*>(user_data);
    context->RunVsyncCallback(baton);
  };
}

void EmbedderConfigBuilder::SetRenderTaskRunner(
    const FlutterTaskRunnerDescription* runner) {
  if (runner == nullptr) {
    return;
  }

  custom_task_runners_.render_task_runner = runner;
  project_args_.custom_task_runners = &custom_task_runners_;
}

void EmbedderConfigBuilder::SetPlatformMessageCallback(
    const std::function<void(const FlutterPlatformMessage*)>& callback) {
  context_.SetPlatformMessageCallback(callback);
}

void EmbedderConfigBuilder::SetViewFocusChangeRequestCallback(
    const std::function<void(const FlutterViewFocusChangeRequest*)>& callback) {
  context_.SetViewFocusChangeRequestCallback(callback);
}

void EmbedderConfigBuilder::SetCompositor(bool avoid_backing_store_cache,
                                          bool use_present_layers_callback) {
  context_.SetupCompositor();
  compositor_ = {};
  project_args_.avio_extension_request = nullptr;
  auto& compositor = context_.GetCompositor();
  compositor_.struct_size = sizeof(compositor_);
  compositor_.user_data = &compositor;
  compositor_.create_backing_store_callback =
      [](const FlutterBackingStoreConfig* config,  //
         FlutterBackingStore* backing_store_out,   //
         void* user_data                           //
      ) {
        return reinterpret_cast<EmbedderTestCompositor*>(user_data)
            ->CreateBackingStore(config, backing_store_out);
      };
  compositor_.collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store,  //
         void* user_data                            //
      ) {
        return reinterpret_cast<EmbedderTestCompositor*>(user_data)
            ->CollectBackingStore(backing_store);
      };
  if (use_present_layers_callback) {
    compositor_.present_layers_callback = [](const FlutterLayer** layers,
                                             size_t layers_count,
                                             void* user_data) {
      auto compositor = reinterpret_cast<EmbedderTestCompositor*>(user_data);
      return compositor->Present(kFlutterImplicitViewId, layers, layers_count);
    };
  } else {
    compositor_.present_view_callback = [](const FlutterPresentViewInfo* info) {
      auto compositor =
          reinterpret_cast<EmbedderTestCompositor*>(info->user_data);
      return compositor->Present(info->view_id, info->layers,
                                 info->layers_count);
    };
  }
  compositor_.avoid_backing_store_cache = avoid_backing_store_cache;
  compositor_.compositor_mode = kFlutterCompositorModeGeneric;
  project_args_.compositor = &compositor_;
}

void EmbedderConfigBuilder::SetRootRenderTargetCompositor(
    bool avoid_backing_store_cache,
    FlutterAvioExtensionFeatures required_features) {
  context_.SetupCompositor();
  compositor_ = {};
  auto& compositor = context_.GetCompositor();
  compositor_.struct_size = sizeof(compositor_);
  compositor_.user_data = &compositor;
  compositor_.acquire_render_target_callback =
      [](const FlutterRenderTargetAcquisitionInfo* info,
         FlutterBackingStore* backing_store_out) {
        return reinterpret_cast<EmbedderTestCompositor*>(info->user_data)
                       ->CreateBackingStore(info->config, backing_store_out)
                   ? kFlutterRenderTargetAcquisitionGranted
                   : kFlutterRenderTargetAcquisitionBackpressured;
      };
  compositor_.collect_backing_store_callback =
      [](const FlutterBackingStore* backing_store, void* user_data) {
        return reinterpret_cast<EmbedderTestCompositor*>(user_data)
            ->CollectBackingStore(backing_store);
      };
  compositor_.present_render_target_callback =
      [](const FlutterPresentRenderTargetInfo* info) {
        auto compositor =
            reinterpret_cast<EmbedderTestCompositor*>(info->user_data);
        if (!compositor->HandleRootRenderTargetResult(*info)) {
          return false;
        }
        if (info->status != kFlutterPresentRenderTargetStatusPresented) {
          return true;
        }
        FlutterLayer layer = {
            .struct_size = sizeof(FlutterLayer),
            .type = kFlutterLayerContentTypeBackingStore,
            .backing_store = info->backing_store,
            .offset = FlutterPoint{0.0, 0.0},
            .size = FlutterSize{0.0, 0.0},
            .backing_store_present_info =
                const_cast<FlutterBackingStorePresentInfo*>(
                    info->backing_store_present_info),
            .presentation_time = 0,
            .shell_layer_role = kFlutterShellLayerRoleUnknown,
            .shell_visual_identifier = 0,
            .shell_visual_generation = 0,
            .shell_chrome_model_serial = 0,
        };
        const FlutterLayer* layers[] = {&layer};
        return compositor->Present(info->target_id, layers, 1);
      };
  compositor_.avoid_backing_store_cache = avoid_backing_store_cache;
  compositor_.compositor_mode = kFlutterCompositorModeRootRenderTarget;
  required_features |= kFlutterAvioExtensionFeatureTypedRenderTargetAcquisition;
  const FlutterAvioExtensionFeatures exact_features =
      kFlutterAvioExtensionFeaturePerDisplayVsync |
      kFlutterAvioExtensionFeatureExplicitRenderCompletion |
      kFlutterAvioExtensionFeatureExactVsyncCancellation |
      kFlutterAvioExtensionFeatureFrameOpportunityOutcomes |
      kFlutterAvioExtensionFeatureRenderDeadline;
  if ((required_features & exact_features) == exact_features) {
    exact_frame_pump_ = std::make_shared<ExactFramePump>();
    context_.SetVsyncForDisplayCallback(
        [pump = exact_frame_pump_](intptr_t baton,
                                   FlutterEngineDisplayId display_id) {
          pump->OnBaton(baton, display_id);
        });
    project_args_.vsync_for_display_callback =
        [](void* user_data, intptr_t baton, FlutterEngineDisplayId display_id) {
          reinterpret_cast<EmbedderTestContext*>(user_data)
              ->RunVsyncForDisplayCallback(baton, display_id);
        };
    compositor_.frame_opportunity_outcome_callback =
        [](const FlutterFrameOpportunityOutcomeInfo*) {};
  }
  avio_extension_request_ = {
      .struct_size = sizeof(FlutterAvioExtensionRequest),
      .version = FLUTTER_AVIO_EXTENSION_VERSION,
      .required_features = required_features,
  };
  project_args_.avio_extension_request = &avio_extension_request_;
  project_args_.compositor = &compositor_;
}

FlutterCompositor& EmbedderConfigBuilder::GetCompositor() {
  return compositor_;
}

void EmbedderConfigBuilder::SetRenderTargetType(
    EmbedderTestBackingStoreProducer::RenderTargetType type,
    FlutterSoftwarePixelFormat software_pixfmt) {
  context_.GetCompositor().SetRenderTargetType(type, software_pixfmt);
}

UniqueEngine EmbedderConfigBuilder::LaunchEngine() const {
  return SetupEngine(true);
}

UniqueEngine EmbedderConfigBuilder::InitializeEngine() const {
  return SetupEngine(false);
}

UniqueEngine EmbedderConfigBuilder::SetupEngine(bool run) const {
  FlutterEngine engine = nullptr;
  FlutterProjectArgs project_args = project_args_;

  std::vector<const char*> args;
  args.reserve(command_line_arguments_.size());

  for (const auto& arg : command_line_arguments_) {
    args.push_back(arg.c_str());
  }

  if (!args.empty()) {
    project_args.command_line_argv = args.data();
    project_args.command_line_argc = args.size();
  } else {
    // Clear it out in case this is not the first engine launch from the
    // embedder config builder.
    project_args.command_line_argv = nullptr;
    project_args.command_line_argc = 0;
  }

  std::vector<const char*> dart_args;
  dart_args.reserve(dart_entrypoint_arguments_.size());

  for (const auto& arg : dart_entrypoint_arguments_) {
    dart_args.push_back(arg.c_str());
  }

  if (!dart_args.empty()) {
    project_args.dart_entrypoint_argv = dart_args.data();
    project_args.dart_entrypoint_argc = dart_args.size();
  } else {
    // Clear it out in case this is not the first engine launch from the
    // embedder config builder.
    project_args.dart_entrypoint_argv = nullptr;
    project_args.dart_entrypoint_argc = 0;
  }

  auto result = run ? FlutterEngineRun(FLUTTER_ENGINE_VERSION,
                                       &context_.GetRendererConfig(),
                                       &project_args, &context_, &engine)
                    : FlutterEngineInitialize(
                          FLUTTER_ENGINE_VERSION, &context_.GetRendererConfig(),
                          &project_args, &context_, &engine);

  if (result != kSuccess) {
    return {};
  }

  if (exact_frame_pump_) {
    exact_frame_pump_->Attach(engine);
  }

  return UniqueEngine{engine};
}

}  // namespace flutter::testing
