# Avio Engine Patch Stack

This fork of flutter/flutter carries Avio's engine + framework patches as a
curated, rebased stack (the flutter-tizen model). Canonical branch naming:
`avio/<upstream-ref>` (e.g. `avio/main-2026-06`). Every commit subject starts
with `[avio]` (`[avio][framework]` for packages/flutter changes); the two
cherry-pick/fix commits keep their original subjects.

Contract for what these patches may and may not do:
`avio/docs/engine-contract.md` in the Avio repo.

## Patch inventory

| # | Patch | Kind | Upstream replacement? |
|---|-------|------|----------------------|
| 1 | Add Vulkan Impeller backing store support for embedder API | permanent ABI extension | none |
| 2 | Add DMA-BUF external textures with GPU-side acquire fences | permanent ABI extension | open: flutter/flutter#117937 |
| 3 | Multi-rect damage tracking with DlRegion and per-buffer Vulkan support | permanent (upstreamable in principle) | open: flutter/flutter#109724 |
| 4 | Synchronous-resize plumbing, texture dirty awareness, integration fixes | permanent (configure_serial gating is live; blocking API removed by #13) | none |
| 5 | Per-display scheduling, empty-frame callback, EVE frame-damage metadata | permanent ABI extension | none |
| 6 | cherry-pick: dispose thread-local Vulkan caches in embedder compositor path | temporary — drop when upstream lands the EVE-path fix (original PR flutter/flutter#183268 never merged; #182265/#182402 cover other paths only) | watch |
| 7 | fix(embedder): make PublishDmabufTexture non-blocking | upstreamable bugfix | submit upstream |
| 8 | feat(embedder): expose render_complete_sync_fd via ExternalSemaphoreVK | permanent ABI extension (explicit sync) | none — no upstream fence/sync surface exists |
| 9 | Typed shell layer targets and per-window chrome presentation | permanent ABI extension | none |
| 10 | Vulkan Impeller embedder lifetime hardening and Linux build fixes | permanent | partially subsumed (SDF flags became stock `Settings::impeller_use_sdfs` + `impeller::Flags` in 2026-05; our plumbing was dropped at the 2026-06 rebase) |
| 11 | Timeline-semaphore Vulkan completion and ContextVK-owned transients pool | permanent | none |
| 12 | View-scoped frame scheduling via PlatformDispatcher.scheduleFrameForDisplayViews | permanent dart:ui extension | none |
| 13 | Remove synchronous immediate-frame resize path and stale RSS probes | permanent deletion (contract §8) | n/a |
| 14 | [framework] Wire engine per-display scoped frame callbacks into framework | permanent framework extension | none |
| 15 | [framework] Route Dart-originated scheduleFrame through per-display scoped engine API | permanent framework extension | none |
| 16 | [framework] Fix Phase 2 ordering bug — register dirty view BEFORE first scheduleFrame | permanent framework fix | none |
| 17 | [framework] Make unattributable dirty marks force the global frame path | permanent framework fix | none |
| 18 | [framework] Fall back to global frame path when a view's display is unregistered | permanent framework fix | none |
| 19 | [framework] Guard the remaining display lookup in dirty-view forwarding | permanent framework fix | none |
| 20 | [framework] Make MouseTracker device-update phase exception-safe | upstreamable bugfix | submit upstream |
| 21 | Make DlRegion total over empty rect inputs | upstreamable bugfix (latent upstream infinite loop) | submit upstream |
| 22 | [framework] Preserve scoped frame authority while ordering residual view work before input | permanent framework correctness fix | none while the shared build tree remains global |
| 23 | Explicit root-target compositor mode and semantic extension negotiation | permanent ABI extension | none |

### Patch 22: scoped-frame authority and input ordering

A framework frame carrying `activeFrameViewIds` may render only those view
IDs. If the shared widget build dirties another view, the framework schedules a
separate compositor-authorized frame for that residual work, preserving
view-scoped dispatch whenever the dirty set resolves to one display. Pointer
dispatch and post-frame mouse re-hit-testing for that exact view wait for its
frame, using a bounded per-view queue that is discarded with the view.

Never restore the superseded `frameRenderViews` widening path from
`7a007de9ffc`: synchronously rendering dirty siblings produces presents without
compositor grants and can overload the embedder pipeline. The focused
regression is
`packages/flutter/test/widgets/view_scoped_frame_scheduling_test.dart`;
`avio-verify-patches.sh` asserts both the replacement primitives and the
absence of the superseded path. The Avio-side normative contract is
`avio/docs/engine-contract.md` core invariant 10.

## Known baseline debt

- ~~Generic embedder compositor and platform-view tests required broad
  exclusions~~ FIXED by patch #23. Stock `present_layers_callback` /
  `present_view_callback` semantics remain the default generic mode. Avio's
  single-root behavior requires an explicit negotiated `RootRenderTarget`
  mode, and every root submission returns a typed terminal result—including
  unsupported platform-view, backing-store, and raster failures—rather than
  abandoning a callback latch.
- `embedder_unittests`: the **GL present-info damage family**
  (`EmbedderTest.PresentInfo*` and related populate-existing-damage render
  tests) hangs after rendering one frame — the OpenGL `present_with_info`
  damage contract diverges under the fork's damage rework. Avio is
  Vulkan-only and never exercises the GL present path, so this is
  unused-path debt; the damage behavior Avio does rely on is gated by
  integration telemetry (`flutter_full_damage_fallback_total`). Investigate
  only if a GL deployment ever becomes relevant.
- `flow_unittests`: `PerformanceOverlayLayerDefault.Gold` fails locally
  (golden-image fixture, environmental).
- `FML_LOG(IMPORTANT)` "high-water" probes remain in
  `lib/ui/window/platform_configuration.cc` and `shell/common/animator.cc`
  (engine-contract §10 hot-path-logging debt; remove in a follow-up patch).
- ~~`on_empty_frame_callback` never fires at runtime~~ FIXED (25c4418f63,
  2026-07-03): both silent abort paths in `Animator` (PipelineFull before
  BeginFrame, and a vsync whose requested views resolve to none) now notify
  `OnAnimatorEmptyFrameForDisplay` with the latched/requested view set, and
  the PipelineFull retry stays view-scoped. Avio's recovery watchdog is back
  to being a true last resort; sustained `Cleared timed-out in-flight
  Flutter frame request` warnings are a regression signal again.
- Per-view frame-request completion is now a **contract guarantee** of
  patch #5 (extended by 321a62c83b + the reuse-frame notify, 2026-07-08,
  squash into #5 at the next rebase): every view named in a view-scoped
  frame request completes exactly once — rendered (present), unrendered at
  frame end, or reported through `OnAnimatorEmptyFrameForDisplay`
  immediately when (a) the request targets an unregistered display, (b) a
  view not homed on that display (`LatchDisplayFrameRequest` used to filter
  those silently; the 2026-07-08 live RCA measured ~550 half-second watchdog
  timeouts per 5 minutes), or (c) the vsync resolves to the cached-tree
  reuse path (`BeginFrameForDisplay` early return), which consumes the
  latched set without running a UI frame. Preserve this guarantee when
  rebasing `Animator::RequestFrameForDisplayInternal` /
  `BeginFrameForDisplay`.
- Avio-side follow-up (rendering-plan step 4): scene assembly latches
  window elements on a newly-hosting output without their chrome binding;
  `synthesize_window_chrome_binding` in `presentation/latch.rs` papers over
  it from the chrome view's latest accepted entry. Attach bindings properly
  at assembly time and the synthesis becomes a cold fallback.

## Rebase runbook (quarterly, at upstream stable cutoffs)

1. `git fetch upstream main` (remote `upstream` = flutter/flutter; refspec
   `+refs/heads/main:refs/remotes/upstream/main` is configured).
2. Tag the current state: `git tag pre-upgrade-<date>`.
3. Pin the target SHA; create `avio/<new-ref>` from the current branch.
4. `git rebase --onto <target> <old-merge-base> avio/<new-ref>`.
   Conflict hot zones: `shell/platform/embedder/embedder_external_view*`
   (preserve upstream generic mode and port rendering fixes into the explicit
   Avio root-target mode), `impeller/renderer/backend/vulkan/**` (interface churn — adapt
   our DmabufTextureSourceVK / timeline files to new virtuals),
   `mock_vulkan.cc` (merge feature advertising into upstream's walker).
5. `git range-diff <old-base>..<old-branch> <target>..<new-branch>` — every
   patch must be accounted for.
6. `bash avio-verify-patches.sh` (in this directory) — all assertions must
   pass. This is the gate that guarantees fixes (e.g. the RSS
   DisposeThreadLocalCachedResources cherry-pick) survive.
7. `gclient sync -D` then **regenerate GN args** for every out config:
   `./flutter/tools/gn --unoptimized` / `--runtime-mode=profile` /
   `--runtime-mode=release` (stale `dart_version` stamps cause gen_snapshot
   `ApiError` failures).
8. Build per config: `ninja -C engine/src/out/<config> libflutter_engine.so
   libflutter_linux_gtk.so gen_snapshot flutter_patched_sdk
   flutter/build/dart:copy_dart_sdk embedder_unittests shell_unittests
   impeller_unittests flow_unittests`.
9. Run the four test suites. Only the GL present-info damage-family exclusion
   remains for `embedder_unittests`; generic compositor and platform-view tests
   must run.
10. Rebuild Avio (`cargo build --profile profile`) — bindgen recompiles
    against the fork header and fails loudly on ABI drift — then run the
    validation matrix (Avio `docs/engine-contract.md` §11).

Notes:
- This clone is shallow + blob:none. `bin/internal/content_aware_hash.sh`
  skips merge-base logic on shallow clones, so the flutter tool bootstrap
  would 404; Avio's build pins `FLUTTER_PREBUILT_ENGINE_VERSION` to the
  merge-base content hash automatically (build_support/flutter_sdk.rs).
- Watch list: PR flutter/flutter#183382 (Impeller Vulkan desktop backend —
  engine-managed VkSurfaceKHR, architecturally opposite to our
  embedder-managed model), `material_ui`/`cupertino_ui` first real releases
  (Material decoupling — placeholders as of 2026-06), windowing API
  stabilization (`WidgetsBinding.windowingOwner` — future Avio shell
  integration point).
