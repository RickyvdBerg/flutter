#!/usr/bin/env bash
# Avio engine fork — must-survive patch inventory.
# Asserts every load-bearing fix / ABI extension is present in the working tree.
# Run from the fork root after any rebase or curation. Exit 0 = all preserved.
set -u
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
cd "$ROOT"
F=engine/src/flutter
W=packages/flutter
fail=0

need() { # $1=label $2=file $3=pattern
  if git grep -q -E "$3" -- "$2" 2>/dev/null || grep -qrE "$3" "$2" 2>/dev/null; then
    echo "OK   $1"
  else
    echo "MISS $1  [$3 not in $2]"; fail=1
  fi
}
absent() { # $1=label $2=pattern
  if grep -qrE "$2" $F/shell $F/impeller $F/lib/ui 2>/dev/null; then
    echo "LEAK $1  [$2 still present]"; fail=1
  else
    echo "OK   $1 (absent as intended)"
  fi
}
absent_in() { # $1=label $2=path $3=pattern
  if grep -qE "$3" "$2" 2>/dev/null; then
    echo "LEAK $1  [$3 still present in $2]"; fail=1
  else
    echo "OK   $1 (absent as intended)"
  fi
}

echo "--- RSS / lifecycle fixes ---"
need "RSS: DisposeThreadLocalCachedResources in EVE submit" \
  $F/shell/platform/embedder/embedder_external_view_embedder.cc 'DisposeThreadLocalCachedResources'
need "non-blocking PublishDmabufTexture entry point" \
  $F/shell/platform/embedder/embedder.cc 'FlutterEnginePublishDmabufTexture'

echo "--- Embedder compositor modes ---"
need "stock generic layer-present ABI" \
  $F/shell/platform/embedder/embedder.h 'FlutterLayersPresentCallback present_layers_callback'
need "stock generic per-view present ABI" \
  $F/shell/platform/embedder/embedder.h 'FlutterPresentViewCallback present_view_callback'
need "explicit root-target compositor mode" \
  $F/shell/platform/embedder/embedder.h 'kFlutterCompositorModeRootRenderTarget'
need "versioned Avio semantic capability query" \
  $F/shell/platform/embedder/embedder.h 'FlutterEngineGetAvioExtensionCapabilities'
need "typed root-target terminal results" \
  $F/shell/platform/embedder/embedder.h 'kFlutterPresentRenderTargetStatusUnsupportedPlatformView'
need "separate generic and root EVE submission paths" \
  $F/shell/platform/embedder/embedder_external_view_embedder.cc 'SubmitGenericFlutterView'
absent_in "Linux desktop embedder root-target override" \
  $F/shell/platform/linux/fl_engine.cc 'present_render_target_callback'
absent_in "Windows desktop embedder root-target override" \
  $F/shell/platform/windows/flutter_windows_engine.cc 'present_render_target_callback'
absent_in "macOS desktop embedder root-target override" \
  $F/shell/platform/darwin/macos/framework/Source/FlutterEngine.mm \
  'present_render_target_callback'

echo "--- Exact frame opportunities ---"
need "exact opportunity feature negotiation" \
  $F/shell/platform/embedder/embedder.h \
  'kFlutterAvioExtensionFeatureFrameOpportunityOutcomes'
need "engine-local opportunity conservation ledger" \
  $F/common/frame_opportunity.h 'class FrameOpportunityRegistry'
need "exact returned-opportunity cancellation ABI" \
  $F/shell/platform/embedder/embedder.h \
  'FlutterEngineCancelFrameOpportunity'
need "scheduled return names exact targets" \
  $F/shell/platform/embedder/embedder.h 'const FlutterViewId\* target_ids'
need "admitted targets travel with frame opportunity custody" \
  $F/common/frame_opportunity.h 'std::set<int64_t> target_ids'
need "Animator reconciles admitted targets at the UI boundary" \
  $F/shell/common/animator.cc 'ReconcileFrameTargets'
need "admitted-target reconciliation regression test" \
  $F/shell/common/animator_unittests.cc \
  'ExactOpportunityReconcilesEveryAdmittedTarget'
need "typed per-target terminal outcomes" \
  $F/shell/platform/embedder/embedder.h \
  'kFlutterFrameOpportunityOutcomeBackpressured'
absent_in "process-global vsync baton registry" \
  $F/shell/platform/embedder/vsync_waiter_embedder.cc \
  'PendingBatonRegistry|kMaxPendingBatons'
absent_in "Animator frame-path IMPORTANT/high-water logging" \
  $F/shell/common/animator.cc 'FML_LOG\(IMPORTANT\)|LogStateHighWatermarks'
absent_in "platform-configuration frame-path IMPORTANT/high-water logging" \
  $F/lib/ui/window/platform_configuration.cc \
  'FML_LOG\(IMPORTANT\)|LogStateHighWatermarks'
absent_in "signal/poll raster watchdog" \
  $F/shell/common/shell.cc \
  'raster_watchdog|RasterStallSigHandler|SIGUSR1|WATCHDOG:'

echo "--- Explicit sync chain ---"
need "render_complete_sync_fd ABI field" \
  $F/shell/platform/embedder/embedder.h 'render_complete_sync_fd'
need "ExternalSemaphoreVK SYNC_FD export" \
  $F/impeller/renderer/backend/vulkan 'ExternalSemaphoreVK'
need "per-texture signal semaphores" \
  $F/impeller/renderer/backend/vulkan 'CreateSignalSemaphores'

echo "--- DMA-BUF external textures ---"
need "FlutterDmabufDescriptor ABI" \
  $F/shell/platform/embedder/embedder.h 'FlutterDmabufDescriptor'
need "DmabufTextureSourceVK" \
  $F/impeller/renderer/backend/vulkan 'DmabufTextureSourceVK'
need "dmabuf acquire fence import (GPU-side wait)" \
  $F/impeller/renderer/backend/vulkan 'acquire_fence'
need "native texture notifications reach Dart" \
  $F/lib/ui/window/platform_configuration.cc 'NotifyTextureFrameAvailable'
need "framework texture invalidation is keyed by texture ID" \
  $W/lib/src/rendering/binding.dart \
  'Map<int, Set<VoidCallback>> _textureFrameCallbacks'
need "texture ownership follows attach and detach" \
  $W/lib/src/rendering/texture.dart \
  'registerTextureFrameAvailableCallback'
need "DMA-BUF texture publication notifies exact framework consumers" \
  $F/shell/platform/embedder/embedder_engine.cc \
  'engine->NotifyTextureFrameAvailable\(texture_id\)'
if sed -n '/bool EmbedderEngine::PublishDmabufTexture/,/^}/p' \
    "$F/shell/platform/embedder/embedder_engine.cc" | \
    grep -q 'engine->ScheduleFrame'; then
  echo "LEAK DMA-BUF texture publication schedules a global frame"
  fail=1
else
  echo "OK   DMA-BUF texture publication preserves compositor cadence"
fi

echo "--- Damage tracking ---"
need "frame_damage in FlutterBackingStorePresentInfo" \
  $F/shell/platform/embedder/embedder.h 'frame_damage'
need "logical and raster damage are separated" \
  $F/flow/compositor_context.h 'intentionally distinct from sparse logical frame damage'
need "dmabuf per-commit damage rects ABI" \
  $F/shell/platform/embedder/embedder.h 'num_damage_rects'
need "selected-target damage feature negotiation" \
  $F/shell/platform/embedder/embedder.h \
  'kFlutterAvioExtensionFeatureSelectedTargetDamage'
need "selected backing target carries exact content history" \
  $F/shell/platform/embedder/embedder.h \
  'FlutterBackingStoreContentState'
need "selected buffer damage is distinct from frame damage" \
  $F/shell/platform/embedder/embedder.h 'FlutterRegion\* buffer_damage'
need "root target is acquired before damage finalization" \
  $F/shell/common/rasterizer.cc 'AcquireRootRenderTarget'
need "Impeller first pass honors a preserved target load" \
  $F/impeller/entity/inline_pass_context.cc \
  'The first pass honors the render target'
absent_in "partial-raster scratch/copy compensation" \
  $F/shell/platform/embedder/embedder_external_view.cc \
  'Embedder Partial Repaint Copy|GetImpellerPartialRepaintTarget'
need "three-target selected-damage pixel regression" \
  $F/shell/platform/embedder/tests/embedder_vk_unittests.cc \
  'SelectedTargetDamageReacquiresAndRepaintsExactRetainedTarget'
need "transparent blur removal pixel regression" \
  $F/shell/platform/embedder/tests/embedder_vk_unittests.cc \
  'SelectedTargetDamageClearsRemovedBlurWithFullRepaintParity'
need "disjoint translucent replacement pixel regression" \
  $F/shell/platform/embedder/tests/embedder_vk_unittests.cc \
  'SelectedTargetDamageBoundsActualRasterForTranslucentGap'
need "full fallback clears preserved selected target" \
  $F/shell/platform/embedder/tests/embedder_vk_unittests.cc \
  'SelectedTargetDamageFullFallbackClearsPreservedTarget'

echo "--- Typed backing stores / chrome ---"
need "FlutterBackingStoreRequestType enum" \
  $F/shell/platform/embedder/embedder.h 'kFlutterBackingStoreRequestTypePerWindowChrome'
need "visual identifier plumbing" \
  $F/shell/platform/embedder 'visual_identifier'

echo "--- Frame scheduling ---"
need "empty-frame callback ABI" \
  $F/shell/platform/embedder/embedder.h 'on_empty_frame_callback'
need "animator empty-frame delegate" \
  $F/shell/common/animator.cc 'EmptyFrameForDisplay'
need "ScheduleFrame request-kind family" \
  $F/shell/platform/embedder/embedder.h 'FlutterEngineScheduleFrameForDisplayViewsWithRequestKind'
need "dart:ui scheduleFrameForDisplayViews" \
  $F/lib/ui/platform_dispatcher.dart 'scheduleFrameForDisplayViews'
need "configure_serial resize gating (metrics event)" \
  $F/shell/platform/embedder/embedder.h 'configure_serial'
need "animator pending-serial reuse gate" \
  $F/shell/common/animator.cc 'pending_configure_serial_'

echo "--- Framework scoped-frame authority ---"
need "scoped renderer consumes only active view ids" \
  $W/lib/src/rendering/binding.dart 'for \(final int viewId in activeViewIds\)'
need "residual render work gets a follow-up frame" \
  $W/lib/src/rendering/binding.dart '_scheduleResidualRenderWork'
need "View roots own independent build scopes" \
  $W/lib/src/widgets/view.dart 'final BuildScope _buildScope'
need "BuildOwner indexes view build scopes" \
  $W/lib/src/widgets/framework.dart 'registerViewBuildScope'
need "widgets build only admitted view scopes" \
  $W/lib/src/widgets/binding.dart 'owner\.buildViewScope\(viewId\)'
need "scoped frames can bootstrap structural View boundaries" \
  $W/lib/src/widgets/binding.dart 'activeViewIds == null \|\| _hasUnattributableDirtyBuild'
need "new View content waits in its owned build scope" \
  $W/lib/src/widgets/view.dart 'activeFrameViewIds == null'
need "dirty-view settlement occurs at the owning scope" \
  $W/lib/src/widgets/binding.dart '_dirtyBuildViewIds\.remove\(viewId\)'
need "unsettled widget scopes request another opportunity" \
  $W/lib/src/widgets/binding.dart 'schedulePendingWidgetBuilds\(\)'
need "retired views release build-scheduling custody" \
  $W/lib/src/widgets/binding.dart 'onViewBuildScopeRetired = _retireViewBuildScope'
need "test frames use the production build-scope operation" \
  packages/flutter_test/lib/src/binding.dart 'buildDirtyWidgetScopes\(\)'
need "test frames use the production residual-build operation" \
  packages/flutter_test/lib/src/binding.dart 'schedulePendingWidgetBuilds\(\)'
absent_in "deferred pointer queue below the build-scope invariant" \
  $W/lib/src/rendering/binding.dart '_DeferredPointerEventQueue'
absent_in "synthetic pointer terminal on scoped-frame delay" \
  $W/lib/src/rendering/binding.dart '_collapseToTerminalState'
need "post-frame mouse updates remain view-scoped" \
  $W/lib/src/rendering/mouse_tracker.dart 'updateDevicesForViews'
need "elements carry typed view ownership" \
  $W/lib/src/widgets/framework.dart 'sealed class BuildViewIdentity'
need "View boundaries seal one child identity" \
  $W/lib/src/widgets/view.dart 'BuildViewIdentity\.view'
need "dirty marks consume cached O(1) ownership" \
  $W/lib/src/widgets/framework.dart \
  'onElementDirtied\?\.call\(element\.buildViewIdentity\)'
need "view ownership follows GlobalKey reparenting" \
  $W/lib/src/widgets/framework.dart '_updateBuildViewIdentityRecursively'
absent_in "dirty-mark RenderView ancestor walk" \
  $W/lib/src/widgets/binding.dart \
  'findAncestorRenderObjectOfType<RenderView>'
need "scoped-frame authority regression test" \
  $W/test/widgets/view_scoped_frame_scheduling_test.dart \
  'scoped frame leaves inactive view coherent and its input live'
need "view BuildScope isolation regression test" \
  $W/test/widgets/view_test.dart \
  'view build scopes isolate dirty sibling trees'
need "nested View updates remain behind their own scope" \
  $W/test/widgets/view_test.dart \
  'parent updates cannot rebuild a nested view outside its scope'
need "residual widget demand schedules another scoped frame" \
  $W/test/widgets/view_scoped_build_scheduling_test.dart \
  'unprocessed view scope requests another frame opportunity'
need "first scoped frame bootstrap regression test" \
  $W/test/widgets/view_scoped_build_scheduling_test.dart \
  'the first scoped frame bootstraps only its admitted view'
need "structural pointer preservation regression test" \
  $W/test/widgets/view_scoped_frame_scheduling_test.dart \
  'scoped render demand never rewrites structural pointer events'
absent_in "cross-view frameRenderViews widening (forbidden, patch #22)" \
  $W/lib/src/rendering/binding.dart 'frameRenderViews'

echo "--- Vulkan lifecycle ---"
need "timeline-semaphore completion" \
  $F/impeller/renderer/backend/vulkan 'timeline_completion'
need "render batch precedes the timeline marker batch" \
  $F/impeller/renderer/backend/vulkan/command_queue_vk.cc \
  'ImpellerSubmitCompletionDependency'
need "timeline completion preserves upstream GPU submission tracking" \
  $F/impeller/renderer/backend/vulkan/command_queue_vk.cc \
  'GetMutableSubmissionTracker'
need "timeline callback retires the exact upstream submission id" \
  $F/impeller/renderer/backend/vulkan/command_queue_vk.cc \
  'RecordCompletion\(submission_id\)'
need "transients pool budget" \
  $F/impeller/renderer/backend/vulkan 'IMPELLER_VK_TRANSIENTS_BUDGET_MIB'
need "transient admission fails instead of unaccounting leased entries" \
  $F/impeller/renderer/backend/vulkan/swapchain/transients_pool_vk.cc \
  'if \(candidate == lru_\.end\(\)\)'
need "transient trim requires wrapper and GPU idleness" \
  $F/impeller/renderer/backend/vulkan/swapchain/transients_pool_vk.cc \
  'entry\.transients\.use_count\(\) == 1u && entry\.transients->IsIdle\(\)'
need "Slimpeller low-memory path trims idle Impeller caches" \
  $F/shell/common/rasterizer.cc 'TrimIdleResourceCaches'
need "explicit transient profiles bypass environment policy" \
  $F/impeller/renderer/backend/vulkan/swapchain/transients_pool_vk.h \
  'allow_environment_override'
need "resource lifecycle feature is negotiated" \
  $F/shell/platform/embedder/embedder.h \
  'kFlutterAvioExtensionFeatureResourceLifecycleConfig'
need "per-view visibility is negotiated" \
  $F/shell/platform/embedder/embedder.h \
  'kFlutterAvioExtensionFeatureViewVisibility'
need "hidden admitted targets terminalize without removal" \
  $F/shell/common/animator.cc \
  'renderable_view_ids'
need "visibility never masquerades as global memory pressure" \
  $F/shell/platform/embedder/embedder_engine.cc \
  'rasterizer->TrimIdleResourceCaches\(\)'
need "resource lifecycle config is paired with negotiation" \
  $F/shell/platform/embedder/embedder.cc \
  'A resource lifecycle configuration was supplied without'
need "resource cache directory is a duplicated capability" \
  $F/shell/platform/embedder/embedder.cc \
  'fml::Duplicate\(config->pipeline_cache_directory_fd\)'
need "resource profile disables environment override" \
  $F/shell/platform/embedder/embedder.cc \
  'allow_environment_override = false'
need "pipeline cache is bounded before mapping" \
  $F/impeller/renderer/backend/vulkan/pipeline_cache_data_vk.cc \
  'GetRegularFileSize'
need "pipeline cache payload fits the mapped remainder" \
  $F/impeller/renderer/backend/vulkan/pipeline_cache_data_vk.cc \
  'data_size > available_data_bytes'
need "malformed sparse pipeline cache regression" \
  $F/impeller/renderer/backend/vulkan/pipeline_cache_data_vk_unittests.cc \
  'RejectsOversizedSparseCacheBeforeMapping'
need "pipeline cache schema is explicit" \
  $F/impeller/renderer/backend/vulkan/pipeline_cache_data_vk.h \
  'kPipelineCacheSchemaVersionVK'
need "Impeller pipeline cache ABI is explicit" \
  $F/impeller/renderer/backend/vulkan/pipeline_cache_data_vk.h \
  'kImpellerPipelineCacheABIVersionVK'
need "read-only pipeline cache never persists" \
  $F/impeller/renderer/backend/vulkan/pipeline_cache_vk.cc \
  'cache_access_ != PipelineCacheAccessVK::kReadWrite'
need "pipeline cache uses profile byte ceiling" \
  $F/impeller/renderer/backend/vulkan/pipeline_cache_vk.cc \
  'max_data_bytes_'

echo "--- Cross-display pipeline conservation ---"
need "display frames reserve the shared raster pipeline" \
  $F/shell/common/animator.cc \
  'producer_continuation_ = state\.pipeline->Produce\(\)'
absent_in "per-display raster pipeline reservation" \
  $F/shell/common/animator.h \
  'ProducerContinuation producer_continuation;'

echo "--- External Vulkan image ownership ---"
need "typed external queue-family ownership" \
  $F/impeller/renderer/backend/vulkan/texture_source_vk.h \
  'struct ExternalImageOwnershipVK'
need "render-pass external image acquire" \
  $F/impeller/renderer/backend/vulkan/render_pass_vk.cc \
  'EncodeExternalImageAcquire'
need "render-pass external image release" \
  $F/impeller/renderer/backend/vulkan/render_pass_vk.cc \
  'EncodeExternalImageRelease'
need "external ownership covers the full texture descriptor" \
  $F/impeller/renderer/backend/vulkan/render_pass_vk.cc \
  'ToArrayLayerCount\(source\.GetTextureDescriptor\(\)\)'
need "embedder ownership ABI field" \
  $F/shell/platform/embedder/embedder.h \
  'has_external_queue_family_ownership'

echo "--- Impeller visual correctness fixes ---"
need "UberSDF two-pixel antialiasing ramp" \
  $F/impeller/entity/contents/uber_sdf_parameters.h \
  'kAntialiasPixels = 2\.0f'
need "UberSDF full-coverage inset tracks the wider ramp" \
  $F/impeller/entity/geometry/uber_sdf_geometry.cc \
  'kAdditionalInset'
need "UberSDF high-precision fragment arithmetic" \
  $F/impeller/entity/shaders/uber_sdf.frag \
  '^precision highp float;'
need "UberSDF thin-stroke coverage" \
  $F/impeller/entity/shaders/uber_sdf.frag \
  'strokeAlphaCoverage'
absent_in "DrawCircle UberSDF fast path with widened AA ramp" \
  $F/impeller/display_list/canvas.cc \
  'UberSDFParameters::MakeCircle'
need "degree normalization half-open range guard" \
  $F/impeller/geometry/scalar.h \
  'if \(deg >= 360\.0f\)'
need "tiny negative degree regression test" \
  $F/impeller/geometry/arc_unittests.cc \
  'TinyNegativeStartNormalizesBelowFullCircle'

echo "--- Removed paths must stay removed ---"
absent "RenderViewImmediate (forbidden, contract §8)" 'RenderViewImmediate'
absent "drain_tasks_now (forbidden support ABI)" 'drain_tasks_now|DrainTasksNow'
absent "RSS probe counters" 'g_reclaim_enqueued_total'

echo
[ $fail -eq 0 ] && echo "ALL PATCHES PRESERVED" || echo "FAILURES DETECTED"
exit $fail
