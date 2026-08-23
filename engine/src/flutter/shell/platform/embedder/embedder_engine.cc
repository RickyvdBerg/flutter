// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_engine.h"

#include <set>

#include "flutter/fml/make_copyable.h"
#include "flutter/shell/platform/embedder/vsync_waiter_embedder.h"

#ifdef __linux__
#include <unistd.h>

#include "flutter/fml/file.h"
#endif

namespace flutter {

struct ShellArgs {
  Settings settings;
  Shell::CreateCallback<PlatformView> on_create_platform_view;
  Shell::CreateCallback<Rasterizer> on_create_rasterizer;
  ShellArgs(const Settings& p_settings,
            Shell::CreateCallback<PlatformView> p_on_create_platform_view,
            Shell::CreateCallback<Rasterizer> p_on_create_rasterizer)
      : settings(p_settings),
        on_create_platform_view(std::move(p_on_create_platform_view)),
        on_create_rasterizer(std::move(p_on_create_rasterizer)) {}
};

#ifdef __linux__
namespace {

bool DuplicateDmabufDescriptorForMailbox(const impeller::DmabufDescriptor& desc,
                                         OwnedDmabufDescriptor* out) {
  if (!out || desc.num_planes == 0 || desc.num_planes > 4) {
    return false;
  }

  OwnedDmabufDescriptor owned_desc;
  owned_desc.width = desc.width;
  owned_desc.height = desc.height;
  owned_desc.drm_format = desc.drm_format;
  owned_desc.drm_modifier = desc.drm_modifier;
  owned_desc.num_planes = desc.num_planes;

  for (uint32_t i = 0; i < desc.num_planes; i++) {
    if (desc.planes[i].fd < 0) {
      return false;
    }
    auto duplicated_fd = fml::Duplicate(desc.planes[i].fd);
    if (!duplicated_fd.is_valid()) {
      return false;
    }
    owned_desc.plane_fds[i] = std::move(duplicated_fd);
    owned_desc.offsets[i] = desc.planes[i].offset;
    owned_desc.strides[i] = desc.planes[i].stride;
  }

  if (desc.acquire_fence_fd >= 0) {
    auto acquire_fence_fd = fml::Duplicate(desc.acquire_fence_fd);
    if (!acquire_fence_fd.is_valid()) {
      return false;
    }
    owned_desc.acquire_fence_fd = std::move(acquire_fence_fd);
  }

  *out = std::move(owned_desc);
  return true;
}

void CloseTransferredDmabufFds(const impeller::DmabufDescriptor& desc) {
  std::set<int> transferred_fds;
  for (uint32_t i = 0; i < desc.num_planes && i < 4; i++) {
    if (desc.planes[i].fd >= 0) {
      transferred_fds.insert(desc.planes[i].fd);
    }
  }
  if (desc.acquire_fence_fd >= 0) {
    transferred_fds.insert(desc.acquire_fence_fd);
  }
  for (int fd : transferred_fds) {
    close(fd);
  }
}

}  // namespace
#endif  // __linux__

EmbedderEngine::EmbedderEngine(
    std::unique_ptr<EmbedderThreadHost> thread_host,
    const flutter::TaskRunners& task_runners,
    const flutter::Settings& settings,
    RunConfiguration run_configuration,
    const Shell::CreateCallback<PlatformView>& on_create_platform_view,
    const Shell::CreateCallback<Rasterizer>& on_create_rasterizer,
    std::unique_ptr<EmbedderExternalTextureResolver> external_texture_resolver)
    : thread_host_(std::move(thread_host)),
      task_runners_(task_runners),
      run_configuration_(std::move(run_configuration)),
      shell_args_(std::make_unique<ShellArgs>(settings,
                                              on_create_platform_view,
                                              on_create_rasterizer)),
      external_texture_resolver_(std::move(external_texture_resolver)),
      avio_extension_features_(settings.avio_extension_features),
      frame_opportunity_registry_(settings.frame_opportunity_registry)
#ifdef __linux__
      ,
      dmabuf_mailbox_(std::make_unique<DmabufTextureMailbox>())
#endif
{
#ifdef __linux__
  if (external_texture_resolver_) {
    external_texture_resolver_->SetDmabufMailbox(dmabuf_mailbox_.get());
  }
#endif
}

EmbedderEngine::~EmbedderEngine() = default;

bool EmbedderEngine::LaunchShell() {
  if (!shell_args_) {
    FML_DLOG(ERROR) << "Invalid shell arguments.";
    return false;
  }

  if (shell_) {
    FML_DLOG(ERROR) << "Shell already initialized";
  }

  shell_ = Shell::Create(
      flutter::PlatformData(), task_runners_, shell_args_->settings,
      shell_args_->on_create_platform_view, shell_args_->on_create_rasterizer);

  // Reset the args no matter what. They will never be used to initialize a
  // shell again.
  shell_args_.reset();

  return IsValid();
}

bool EmbedderEngine::CollectShell() {
  shell_.reset();
  return IsValid();
}

void EmbedderEngine::CollectThreadHost() {
  if (!thread_host_) {
    return;
  }

  // Once the collected, EmbedderThreadHost::RunnerIsValid will return false for
  // all runners belonging to this thread host. This must be done with UI task
  // runner blocked to prevent possible raciness that could happen when
  // destroying the thread host in the middle of UI task runner execution. This
  // is not an issue for other runners, because raster task runner should not
  // have anything scheduled after engine shutdown and platform task runner is
  // where this method is called from.
  if (thread_host_->GetTaskRunners().GetUITaskRunner() &&
      !thread_host_->GetTaskRunners()
           .GetUITaskRunner()
           ->RunsTasksOnCurrentThread()) {
    fml::AutoResetWaitableEvent ui_thread_running;
    fml::AutoResetWaitableEvent ui_thread_block;
    fml::AutoResetWaitableEvent ui_thread_finished;

    thread_host_->GetTaskRunners().GetUITaskRunner()->PostTask([&] {
      ui_thread_running.Signal();
      ui_thread_block.Wait();
      ui_thread_finished.Signal();
    });

    // Wait until the task is running on the UI thread.
    ui_thread_running.Wait();
    thread_host_->InvalidateActiveRunners();
    ui_thread_block.Signal();

    // Needed to keep ui_thread_block in scope until the UI thread execution
    // finishes.
    ui_thread_finished.Wait();
  } else {
    thread_host_->InvalidateActiveRunners();
  }
  thread_host_.reset();
}

bool EmbedderEngine::RunRootIsolate() {
  if (!IsValid() || !run_configuration_.IsValid()) {
    return false;
  }
  shell_->RunEngine(std::move(run_configuration_));
  return true;
}

bool EmbedderEngine::IsValid() const {
  return static_cast<bool>(shell_);
}

const TaskRunners& EmbedderEngine::GetTaskRunners() const {
  return task_runners_;
}

bool EmbedderEngine::NotifyCreated() {
  if (!IsValid()) {
    return false;
  }

  shell_->GetPlatformView()->NotifyCreated();
  return true;
}

bool EmbedderEngine::NotifyDestroyed() {
  if (!IsValid()) {
    return false;
  }

  shell_->GetPlatformView()->NotifyDestroyed();

  return true;
}

bool EmbedderEngine::SetViewportMetrics(
    int64_t view_id,
    const flutter::ViewportMetrics& metrics) {
  if (!IsValid()) {
    return false;
  }

  auto platform_view = shell_->GetPlatformView();
  if (!platform_view) {
    return false;
  }
  platform_view->SetViewportMetrics(view_id, metrics);
  return true;
}

bool EmbedderEngine::DispatchPointerDataPacket(
    std::unique_ptr<flutter::PointerDataPacket> packet) {
  if (!IsValid() || !packet) {
    return false;
  }

  auto platform_view = shell_->GetPlatformView();
  if (!platform_view) {
    return false;
  }

  platform_view->DispatchPointerDataPacket(std::move(packet));
  return true;
}

bool EmbedderEngine::SendPlatformMessage(
    std::unique_ptr<PlatformMessage> message) {
  if (!IsValid() || !message) {
    return false;
  }

  auto platform_view = shell_->GetPlatformView();
  if (!platform_view) {
    return false;
  }

  platform_view->DispatchPlatformMessage(std::move(message));
  return true;
}

bool EmbedderEngine::RegisterTexture(int64_t texture) {
  if (!IsValid()) {
    return false;
  }
  shell_->GetPlatformView()->RegisterTexture(
      external_texture_resolver_->ResolveExternalTexture(texture));
  return true;
}

bool EmbedderEngine::UnregisterTexture(int64_t texture) {
  if (!IsValid()) {
    return false;
  }
  shell_->GetPlatformView()->UnregisterTexture(texture);
  return true;
}

bool EmbedderEngine::MarkTextureFrameAvailable(int64_t texture) {
  if (!IsValid()) {
    return false;
  }
  shell_->GetPlatformView()->MarkTextureFrameAvailable(texture);
  return true;
}

bool EmbedderEngine::SetSemanticsEnabled(bool enabled) {
  if (!IsValid()) {
    return false;
  }

  auto platform_view = shell_->GetPlatformView();
  if (!platform_view) {
    return false;
  }
  platform_view->SetSemanticsEnabled(enabled);
  return true;
}

bool EmbedderEngine::SetAccessibilityFeatures(int32_t flags) {
  if (!IsValid()) {
    return false;
  }
  auto platform_view = shell_->GetPlatformView();
  if (!platform_view) {
    return false;
  }
  platform_view->SetAccessibilityFeatures(flags);
  return true;
}

bool EmbedderEngine::DispatchSemanticsAction(int64_t view_id,
                                             int node_id,
                                             flutter::SemanticsAction action,
                                             fml::MallocMapping args) {
  if (!IsValid()) {
    return false;
  }
  auto platform_view = shell_->GetPlatformView();
  if (!platform_view) {
    return false;
  }
  platform_view->DispatchSemanticsAction(view_id, node_id, action,
                                         std::move(args));
  return true;
}

bool EmbedderEngine::OnVsyncEvent(intptr_t baton,
                                  fml::TimePoint frame_start_time,
                                  fml::TimePoint frame_target_time) {
  if (!IsValid() ||
      (avio_extension_features_ &
       kFlutterAvioExtensionFeatureFrameOpportunityOutcomes) != 0) {
    return false;
  }

  auto waiter = shell_->GetVsyncWaiter().lock();
  return waiter &&
         waiter->ReturnVsync(VsyncWaiter::kDefaultDisplayId, baton,
                             frame_start_time, frame_target_time, std::nullopt);
}

bool EmbedderEngine::OnVsyncEventForDisplay(intptr_t baton,
                                            int64_t display_id,
                                            fml::TimePoint frame_start_time,
                                            fml::TimePoint frame_target_time) {
  if (!IsValid() ||
      (avio_extension_features_ &
       kFlutterAvioExtensionFeatureFrameOpportunityOutcomes) != 0) {
    return false;
  }

  auto waiter = shell_->GetVsyncWaiter().lock();
  return waiter && waiter->ReturnVsync(display_id, baton, frame_start_time,
                                       frame_target_time, std::nullopt);
}

bool EmbedderEngine::OnVsyncEventForDisplayWithOpportunity(
    intptr_t baton,
    int64_t display_id,
    uint64_t frame_opportunity_id,
    const std::vector<int64_t>& target_ids,
    fml::TimePoint frame_start_time,
    fml::TimePoint render_deadline_time,
    fml::TimePoint frame_target_time) {
  if (!IsValid() || frame_opportunity_id == 0 ||
      frame_start_time > render_deadline_time ||
      render_deadline_time >= frame_target_time ||
      (avio_extension_features_ &
       kFlutterAvioExtensionFeatureFrameOpportunityOutcomes) == 0 ||
      (avio_extension_features_ & kFlutterAvioExtensionFeatureRenderDeadline) ==
          0 ||
      !frame_opportunity_registry_ ||
      !frame_opportunity_registry_->Open(frame_opportunity_id, display_id,
                                         target_ids)) {
    return false;
  }

  auto waiter = shell_->GetVsyncWaiter().lock();
  if (waiter && waiter->ReturnVsync(display_id, baton, frame_start_time,
                                    frame_target_time, frame_opportunity_id,
                                    {target_ids.begin(), target_ids.end()},
                                    render_deadline_time)) {
    return true;
  }
  frame_opportunity_registry_->Abandon(frame_opportunity_id, display_id);
  return false;
}

bool EmbedderEngine::CancelVsyncForDisplay(
    intptr_t baton,
    int64_t display_id,
    VsyncWaiter::CancellationReason reason,
    fml::closure completion) {
  if (!IsValid() || (avio_extension_features_ &
                     kFlutterAvioExtensionFeatureExactVsyncCancellation) == 0) {
    return false;
  }

  auto waiter = shell_->GetVsyncWaiter().lock();
  return waiter &&
         waiter->CancelVsync(display_id, baton, reason, std::move(completion));
}

bool EmbedderEngine::CancelFrameOpportunity(
    uint64_t frame_opportunity_id,
    int64_t display_id,
    VsyncWaiter::CancellationReason reason,
    fml::closure completion) {
  if (!IsValid() || !frame_opportunity_registry_ ||
      (avio_extension_features_ &
       kFlutterAvioExtensionFeatureFrameOpportunityOutcomes) == 0) {
    return false;
  }

  const FrameOpportunityOutcome outcome =
      reason == VsyncWaiter::CancellationReason::kTargetRemoved
          ? FrameOpportunityOutcome::kTargetRemoved
          : FrameOpportunityOutcome::kEpochStale;
  if (!frame_opportunity_registry_->Cancel(frame_opportunity_id, display_id,
                                           outcome)) {
    return false;
  }

  auto waiter = shell_->GetVsyncWaiter().lock();
  if (waiter && waiter->CancelFrameOpportunity(display_id, frame_opportunity_id,
                                               reason, completion)) {
    return true;
  }

  auto cancel_on_ui = [engine = shell_->GetEngine(), display_id,
                       frame_opportunity_id, reason,
                       completion = std::move(completion)]() mutable {
    if (engine) {
      engine->CancelFrameOpportunity(display_id, frame_opportunity_id, reason);
    }
    if (completion) {
      completion();
    }
  };
  fml::TaskRunner::RunNowOrPostTask(shell_->GetTaskRunners().GetUITaskRunner(),
                                    std::move(cancel_on_ui));
  return true;
}

bool EmbedderEngine::SetViewDisplay(int64_t view_id, int64_t display_id) {
  if (!IsValid()) {
    return false;
  }

  // Post to UI thread since Animator state must be accessed from UI thread.
  shell_->GetTaskRunners().GetUITaskRunner()->PostTask(
      [engine = shell_->GetEngine(), view_id, display_id]() {
        if (engine) {
          engine->SetViewDisplay(view_id, display_id);
        }
      });
  return true;
}

bool EmbedderEngine::SetViewVisibility(int64_t view_id,
                                       Animator::ViewVisibility visibility) {
  if (!IsValid() || (avio_extension_features_ &
                     kFlutterAvioExtensionFeatureViewVisibility) == 0) {
    return false;
  }

  const auto rasterizer = shell_->GetRasterizer();
  const auto raster_task_runner = task_runners_.GetRasterTaskRunner();
  shell_->GetTaskRunners().GetUITaskRunner()->PostTask(
      [engine = shell_->GetEngine(), rasterizer, raster_task_runner, view_id,
       visibility]() {
        if (!engine || !engine->SetViewVisibility(view_id, visibility)) {
          return;
        }
        raster_task_runner->PostTask([rasterizer]() {
          if (rasterizer) {
            rasterizer->TrimIdleResourceCaches();
          }
        });
      });
  return true;
}

bool EmbedderEngine::ReloadSystemFonts() {
  if (!IsValid()) {
    return false;
  }

  return shell_->ReloadSystemFonts();
}

bool EmbedderEngine::PostRenderThreadTask(const fml::closure& task) {
  if (!IsValid()) {
    return false;
  }

  shell_->GetTaskRunners().GetRasterTaskRunner()->PostTask(task);
  return true;
}

bool EmbedderEngine::RunTask(const FlutterTask* task) {
  // The shell doesn't need to be running or valid for access to the thread
  // host. This is why there is no `IsValid` check here. This allows embedders
  // to perform custom task runner interop before the shell is running.
  if (task == nullptr) {
    return false;
  }
  auto result = thread_host_->PostTask(reinterpret_cast<intptr_t>(task->runner),
                                       task->task);
  // If the UI and platform threads are separate, the microtask queue is
  // flushed through MessageLoopTaskQueues observer.
  // If the UI and platform threads are merged, the UI task runner has no
  // associated task queue, and microtasks need to be flushed manually
  // after running the task.
  if (result && shell_ && task_runners_.GetUITaskRunner() &&
      task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread() &&
      !task_runners_.GetUITaskRunner()->GetTaskQueueId().is_valid()) {
    shell_->FlushMicrotaskQueue();
  }

  return result;
}

bool EmbedderEngine::PostTaskOnEngineManagedNativeThreads(
    const std::function<void(FlutterNativeThreadType)>& closure) const {
  if (!IsValid() || closure == nullptr) {
    return false;
  }

  const auto trampoline = [closure](
                              FlutterNativeThreadType type,
                              const fml::RefPtr<fml::TaskRunner>& runner) {
    runner->PostTask([closure, type] { closure(type); });
  };

  // Post the task to all thread host threads.
  const auto& task_runners = shell_->GetTaskRunners();
  trampoline(kFlutterNativeThreadTypeRender,
             task_runners.GetRasterTaskRunner());
  trampoline(kFlutterNativeThreadTypeWorker, task_runners.GetIOTaskRunner());
  trampoline(kFlutterNativeThreadTypeUI, task_runners.GetUITaskRunner());
  trampoline(kFlutterNativeThreadTypePlatform,
             task_runners.GetPlatformTaskRunner());

  // Post the task to all worker threads.
  auto vm = shell_->GetDartVM();
  vm->GetConcurrentMessageLoop()->PostTaskToAllWorkers(
      [closure]() { closure(kFlutterNativeThreadTypeWorker); });

  return true;
}

bool EmbedderEngine::ScheduleFrame(bool regenerate_layer_trees) {
  if (!IsValid()) {
    return false;
  }

  auto ui_runner = shell_->GetTaskRunners().GetUITaskRunner();
  auto schedule_frame = [engine = shell_->GetEngine(),
                         regenerate_layer_trees]() {
    if (engine) {
      engine->ScheduleFrame(regenerate_layer_trees);
    }
  };
  if (ui_runner->RunsTasksOnCurrentThread()) {
    schedule_frame();
  } else {
    ui_runner->PostTask(std::move(schedule_frame));
  }
  return true;
}

bool EmbedderEngine::ScheduleFrameForDisplay(int64_t display_id,
                                             bool regenerate_layer_trees) {
  if (!IsValid()) {
    return false;
  }

  auto ui_runner = shell_->GetTaskRunners().GetUITaskRunner();
  auto schedule_frame = [engine = shell_->GetEngine(), display_id,
                         regenerate_layer_trees]() {
    if (engine) {
      engine->ScheduleFrameForDisplay(display_id, regenerate_layer_trees);
    }
  };
  if (ui_runner->RunsTasksOnCurrentThread()) {
    schedule_frame();
  } else {
    ui_runner->PostTask(std::move(schedule_frame));
  }
  return true;
}

bool EmbedderEngine::ScheduleFrameForDisplayViews(int64_t display_id,
                                                  std::set<int64_t> view_ids,
                                                  bool regenerate_layer_trees) {
  if (!IsValid() || view_ids.empty()) {
    return false;
  }

  auto ui_runner = shell_->GetTaskRunners().GetUITaskRunner();
  auto schedule_frame = [engine = shell_->GetEngine(), display_id,
                         view_ids = std::move(view_ids),
                         regenerate_layer_trees]() {
    if (engine) {
      static_cast<void>(engine->ScheduleFrameForDisplayViews(
          display_id, view_ids, regenerate_layer_trees));
    }
  };
  if (ui_runner->RunsTasksOnCurrentThread()) {
    schedule_frame();
  } else {
    ui_runner->PostTask(std::move(schedule_frame));
  }
  return true;
}

Shell& EmbedderEngine::GetShell() {
  FML_DCHECK(shell_);
  return *shell_.get();
}

#ifdef __linux__
bool EmbedderEngine::PublishDmabufTexture(
    int64_t texture_id,
    const impeller::DmabufDescriptor& desc,
    std::function<void()> release_callback) {
  if (!IsValid()) {
    return false;
  }

  // Get the Impeller context from the platform view.
  auto platform_view = shell_->GetPlatformView();
  if (!platform_view) {
    return false;
  }

  if (!platform_view->GetImpellerContext()) {
    return false;
  }

  OwnedDmabufDescriptor owned_desc;
  if (!DuplicateDmabufDescriptorForMailbox(desc, &owned_desc)) {
    return false;
  }

  DmabufMailboxEntry entry;
  entry.pending_descriptor = std::move(owned_desc);
  entry.release_callback = std::move(release_callback);
  for (const auto& r : desc.damage_rects) {
    entry.damage_rects.push_back(
        DlIRect::MakeLTRB(r.left, r.top, r.right, r.bottom));
  }

  dmabuf_mailbox_->Store(texture_id, std::move(entry));
  CloseTransferredDmabufFds(desc);

  // Mark the texture's dirty flag on the raster thread (non-blocking).
  //
  // The embedder is responsible for frame pacing (often per-display), so we do
  // not invoke PlatformView::MarkTextureFrameAvailable here because that helper
  // also schedules a global engine frame.
  //
  // Ordering guarantee: mark_texture is posted to the raster thread before any
  // subsequent ScheduleFrame-triggered pipeline task reaches the same thread.
  // FIFO task ordering on the raster thread ensures the dirty flag is set
  // before the frame that needs it renders.
  auto raster_runner = shell_->GetTaskRunners().GetRasterTaskRunner();
  auto mark_texture = [rasterizer = shell_->GetRasterizer(), texture_id]() {
    if (!rasterizer) {
      return;
    }
    auto registry = rasterizer->GetTextureRegistry();
    if (!registry) {
      return;
    }
    auto texture = registry->GetTexture(texture_id);
    if (!texture) {
      return;
    }
    texture->MarkNewFrameAvailable();
  };

  if (raster_runner->RunsTasksOnCurrentThread()) {
    mark_texture();
  } else {
    raster_runner->PostTask(std::move(mark_texture));
  }

  // The DMA-BUF path is paced by compositor-issued frame opportunities. Tell
  // Dart exactly which texture changed so its owning view becomes dirty, but
  // do not widen this into a global engine frame. The framework's dirty-view
  // scheduler will request work and the compositor remains cadence authority.
  auto ui_runner = shell_->GetTaskRunners().GetUITaskRunner();
  auto notify_framework = [engine = shell_->GetEngine(), texture_id]() {
    if (engine) {
      engine->NotifyTextureFrameAvailable(texture_id);
    }
  };
  if (ui_runner->RunsTasksOnCurrentThread()) {
    notify_framework();
  } else {
    ui_runner->PostTask(std::move(notify_framework));
  }
  return true;
}

DmabufTextureMailbox* EmbedderEngine::GetDmabufMailbox() const {
  return dmabuf_mailbox_.get();
}
#endif  // __linux__

}  // namespace flutter
