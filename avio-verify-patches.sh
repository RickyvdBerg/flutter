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

echo "--- Damage tracking ---"
need "frame_damage in FlutterBackingStorePresentInfo" \
  $F/shell/platform/embedder/embedder.h 'frame_damage'
need "multi-rect damage coalescing" \
  $F/flow 'CoalesceDamageRects'
need "dmabuf per-commit damage rects ABI" \
  $F/shell/platform/embedder/embedder.h 'num_damage_rects'

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
need "typed scoped-request state gates exact-view input" \
  $W/lib/src/rendering/binding.dart 'markViewsAwaitingScopedFrame'
need "deferred pointer input is bounded per view" \
  $W/lib/src/rendering/binding.dart 'class _DeferredPointerEventQueue'
need "post-frame mouse updates remain view-scoped" \
  $W/lib/src/rendering/mouse_tracker.dart 'updateDevicesForViews'
need "widgets scheduler marks scoped requests before dispatch" \
  $W/lib/src/widgets/binding.dart 'markViewsAwaitingScopedFrame\(scoped.viewIds\)'
need "scoped-frame authority regression test" \
  $W/test/widgets/view_scoped_frame_scheduling_test.dart \
  'scoped frame defers dirty non-active view and its input'
need "deferred-hover coalescing regression test" \
  $W/test/widgets/view_scoped_frame_scheduling_test.dart \
  'deferred hover events coalesce'
need "view-removal input retirement regression test" \
  $W/test/widgets/view_scoped_frame_scheduling_test.dart \
  'removing a view retires its scoped wait and deferred input'
need "bounded deferred-input regression test" \
  $W/test/widgets/view_scoped_frame_scheduling_test.dart \
  'deferred pointer backlog is bounded'
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
need "embedder ownership ABI field" \
  $F/shell/platform/embedder/embedder.h \
  'has_external_queue_family_ownership'

echo "--- Impeller visual correctness fixes ---"
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
