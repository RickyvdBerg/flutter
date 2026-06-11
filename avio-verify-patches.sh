#!/usr/bin/env bash
# Avio engine fork — must-survive patch inventory.
# Asserts every load-bearing fix / ABI extension is present in the working tree.
# Run from the fork root after any rebase or curation. Exit 0 = all preserved.
set -u
cd /home/rvb/projects/flutter/engine-src-upstream-main
F=engine/src/flutter
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

echo "--- RSS / lifecycle fixes ---"
need "RSS: DisposeThreadLocalCachedResources in EVE submit" \
  $F/shell/platform/embedder/embedder_external_view_embedder.cc 'DisposeThreadLocalCachedResources'
need "non-blocking PublishDmabufTexture entry point" \
  $F/shell/platform/embedder/embedder.cc 'FlutterEnginePublishDmabufTexture'

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

echo "--- Vulkan lifecycle ---"
need "timeline-semaphore completion" \
  $F/impeller/renderer/backend/vulkan 'timeline_completion'
need "transients pool budget" \
  $F/impeller/renderer/backend/vulkan 'IMPELLER_VK_TRANSIENTS_BUDGET_MIB'

echo "--- Removed paths must stay removed ---"
absent "RenderViewImmediate (forbidden, contract §8)" 'RenderViewImmediate'
absent "drain_tasks_now (forbidden support ABI)" 'drain_tasks_now|DrainTasksNow'
absent "RSS probe counters" 'g_reclaim_enqueued_total'

echo
[ $fail -eq 0 ] && echo "ALL PATCHES PRESERVED" || echo "FAILURES DETECTED"
exit $fail
