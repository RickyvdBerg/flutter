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

## Known baseline debt

- `embedder_unittests`: the **layer-present contract test class** — superset
  of the platform-view tests below. Any upstream test that arms the harness's
  layer-array present callback (`SetNextPresentCallback(FlutterLayer**)`) or
  exercises the compositor scene-decomposition contract
  (`EmbedderTest.Compositor*`) hangs: the fork presents per render target via
  `present_render_target_callback`, never as upstream layer arrays, so the
  armed latch never counts down and the 300 s harness timeout kills the run.
  ~40 of 159 embedder tests fall in this class. The fork's own ABI is covered
  by the fork-added tests (`EmbedderDmabufMailboxTest.*`,
  `EmbedderTest.VulkanImpellerCompositor*`,
  `EmbedderTest.CanRenderImplicitViewUsingPresentRenderTargetCallback`) plus
  Avio's integration validation. The current exclusion filter is maintained
  in this repo's CI notes; regenerate per rebase if upstream adds tests.
- Subset detail, the **embedded-platform-view test class**
  (`*PlatformView*`, `EmbedderTest.CustomCompositorMustWorkWithCustomTaskRunner`,
  `EmbedderTest.CompositorMustBeAbleToRenderToOpenGLFramebuffer`, and any
  test whose Dart fixture calls `SceneBuilder.addPlatformView`) hangs or
  aborts **by design**, not by defect. Mechanism: the fixture builds a scene
  with a platform view (`fixtures/main.dart`), the test arms a latch counting
  the compositor present callback (e.g. `embedder_gl_unittests.cc`), but the
  explicit render-target path sees the platform-view entry in
  `composition_order_` and exits early
  (`embedder_external_view_embedder.cc`: "Explicit render-target presentation
  does not support embedded platform views") after `frame->Submit()` —
  `present_render_target_callback_` never fires, the latch never counts down,
  and the 300 s test harness kills the run (SIGTERM/SIGABRT). Upstream's
  tests expect Flutter to decompose scenes into backing-store/platform-view
  layer sandwiches; Avio's contract (engine contract §1) makes the compositor
  the sole composer of client content, so the fork only presents root render
  targets. Pre-dates the 2026-06 curation (verified on the pre-cleanup tree).
  Exclude the class with `--gtest_filter='-*PlatformView*:...'` when running
  the suite.
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
   (keep our present-per-render-target path; port upstream rendering fixes
   into it), `impeller/renderer/backend/vulkan/**` (interface churn — adapt
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
9. Run the four test suites (exclusions per "Known baseline debt").
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
