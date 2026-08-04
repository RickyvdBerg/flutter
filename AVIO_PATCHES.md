# Avio Engine Patch Stack

This fork of flutter/flutter carries Avio's engine + framework patches as a
curated, rebased stack (the flutter-tizen model). Canonical branch naming:
`avio/<upstream-ref>` (e.g. `avio/main-2026-08`); the published fork uses
`main`. Every commit subject starts with `[avio]` (`[avio][framework]` for
packages/flutter changes), except commits which deliberately retain an
upstreamable `fix`/`feat` subject or preserve an original cherry-pick subject.

Contract for what these patches may and may not do:
`avio/docs/engine-contract.md` in the Avio repo.

## Current upstream baseline

The 2026-08 refresh was performed at the Flutter 3.44.8 stable cutoff:

| Identity | Pin |
|----------|-----|
| Official stable release | Flutter 3.44.8, `058e0af2c2b57e369d905a03ac9748b0ebf543c6` (2026-07-23) |
| Avio rebase target | `upstream/main` at `5a2a94a5a971471ad940709c75463b0798df7e5c` (2026-08-03) |
| Previous upstream base | `b79192e735bb13bfcb20f982689e9792d5c485cf` |
| Pre-refresh rollback tag | `avio-pre-flutter-3.44.8-2026-08-03` |

This fork follows upstream `main` at stable cutoffs; it does not merge the
release branch into main. Flutter stable is a release branch with selected
cherry-picks, not a newer linear ancestor: the previous Avio main base was
already 915 main commits ahead of the stable branch point while stable carried
78 branch-only commits. Rebasing onto the stable release commit would therefore
discard newer mainline Engine work. The stable-only delta must instead be
audited for fixes not already represented on main. For this refresh, the
relevant Impeller fixes for AHB swapchain teardown (`145475453cbe`), GLES
texture cleanup (`d742b87b7836`), and text-shadow masks (`308ba65eaeadd`) were
already ancestors of the selected main target under their original commits.

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
| 22 | [framework] Preserve scoped render authority and schedule residual view work | permanent framework correctness fix; input-queue portion superseded by #31 | none while Avio carries view-scoped frame admission |
| 23 | Explicit root-target compositor mode and semantic extension negotiation | permanent ABI extension | none |
| 24 | Engine-local vsync leases and exact per-target frame-opportunity terminality | permanent ABI/lifecycle extension | none |
| 25 | Soften UberSDF antialiasing with high precision and thin-stroke coverage | visual correctness fork | partial overlap with #188821/#189224; retain until equivalent thin-stroke goldens pass upstream behavior |
| 26 | Share the raster pipeline reservation across displays | permanent while display-scoped scheduling drains through one raster pipeline | none |
| 27 | Transfer external Vulkan image queue-family ownership | permanent explicit-sync/ownership extension | none |
| 28 | Keep normalized degrees below a full circle | upstreamable Impeller correctness fix | submit upstream |
| 29 | [framework] Maintain typed O(1) element-to-view ownership | permanent framework correctness/performance fix | none while Avio carries view-scoped frame admission |
| 30 | [framework] Route texture frames to their exact render consumers | upstream-aligned framework/engine fix adapted for compositor pacing | open: flutter/flutter#179874 |
| 31 | [framework] Give each View an independently admitted BuildScope | permanent framework correctness fix; deletes #22's deferred-input compensation | none while Avio carries view-scoped frame admission |
| 32 | Acquire the exact embedder target before damage and raster only its required pixels | permanent ABI/rendering extension | none |

Patch #5 also owns the later exact empty-frame and global-request corrections:
global requests may not be consumed by a display-scoped frame; sibling-render,
PipelineFull, unhomed-view, unresolved-view, and cached-tree-reuse exits must
terminalize the affected scoped request exactly once. The intermediate
revert/reapply commits preserve review history but do not define separate
runtime contracts.

### Patch 11: Vulkan completion and upstream buffer recycling

Avio replaces per-submit fences with one persistent timeline semaphore. A
queue-local binary semaphore orders the render batch before the timeline marker
batch, so CPU retirement cannot race render execution. The completion callback
is the sole successful retirement edge for tracked render objects, imported and
exported semaphores, and the upstream `GpuSubmissionTracker` submission ID.
Preserving that tracker is mandatory: current upstream uses its monotonic GPU
completion watermark to decide when HostBuffer storage can be reused. A failed
queue submission retires its never-executed ID immediately; failure to register
the timeline callback waits for the exact submitted value and otherwise leaves
the ID pending rather than asserting unsafe GPU completion.

### Patch 22: scoped render authority

A framework frame carrying `activeFrameViewIds` may render only those view
IDs. Residual render work schedules a separate compositor-authorized frame,
preserving view-scoped dispatch whenever the dirty set resolves to one display.
Patch #31 supersedes this patch's original deferred-pointer compensation by
removing the shared widget-build condition that required it.

Never restore the superseded `frameRenderViews` widening path from
`7a007de9ffc`: synchronously rendering dirty siblings produces presents without
compositor grants and can overload the embedder pipeline. Never restore the
superseded deferred-pointer queue either: its finite overflow path rewrote real
down/up sequences because it compensated below the shared-build violation. The
focused regressions are `packages/flutter/test/widgets/view_test.dart` and
`packages/flutter/test/widgets/view_scoped_frame_scheduling_test.dart`;
`avio-verify-patches.sh` asserts both the replacement primitives and the
absence of the superseded path. The Avio-side normative contract is
`avio/docs/engine-contract.md` core invariant 10.

### Patch 24: exact cadence, work, and cancellation

Each engine instance owns at most one delivered vsync baton per display; the
old process-global 512-token registry and silent eviction are gone. Returning
a baton names a non-empty target set and opens an engine-local
`FrameOpportunityId`. Every target must claim that record exactly once through
the root-target callback or a typed non-render outcome. A produced-target claim
and cancellation are one mutex-serialized race, so late raster work from a
retired epoch cannot escape to the host.

Pending batons and already-returned future opportunities have distinct exact
cancellation entry points. Both acknowledge only after the UI-side Animator
state has settled. Pipeline rejection releases its reservation immediately;
typed `Backpressured` completion and fresh demand are separate edges. The
semantic feature request requires per-display vsync, root-target mode, explicit
render completion, and the terminal-outcome callback as one indivisible
contract.

The admitted target set travels with the opportunity through the timing
recorder into the Animator. At the UI boundary it is partitioned exactly once:
only requested, active, admitted targets may render; admitted active targets
without work terminalize as `NoVisualChange`; admitted targets removed before
the callback terminalize as `TargetRemoved`; requested targets absent from the
grant cannot render. This reconciliation is what closes the conservation
ledger for clean and concurrently removed views without a watchdog.

### Patch 25: two-pixel UberSDF antialiasing

The wider antialiasing ramp expands geometry coverage by
`kAntialiasPixels`, and full-coverage detection accounts for the half-pixel
already supplied by an integer pixel boundary. Tests derive their expected
footprints from the same named constant, so upstream geometry changes cannot
silently restore one-pixel assumptions or disable the background-color
optimization wholesale.

### Patch 27: external Vulkan image ownership

External render targets carry their foreign queue-family ownership into the
Impeller render pass. The acquire and release barriers cover the complete
`TextureDescriptor`, including its array-layer count; passing only the texture
type is both incompatible with current upstream and would lose the descriptor's
explicit `array_layer_count` for `kTexture2DArray`.

### Patch 29: O(1) element view ownership

Every mounted `Element` carries a typed `BuildViewIdentity`. Real `View`
boundaries seal one `SingleBuildView` token for themselves and descendants;
roots and cross-view widget ownership carry `AllBuildViews`. Mount inherits the
parent token and GlobalKey reparenting updates the affected subtree until the
next View boundary, so `BuildOwner.scheduleBuildFor` never walks ancestors or
guesses a render root on the hot dirty-mark path. A stale view token widens to
the explicit all-views scheduler path. Focused tests pin initial attachment,
cross-view reparenting, and unowned-root behavior.

### Patch 30: exact texture invalidation

Native texture-frame notifications reach Dart with the texture ID that changed.
`RendererBinding` owns an ID-keyed callback map, so lookup is O(1) in the
number of registered texture IDs while a shared texture explicitly fans out to
all of its consumers. `TextureBox` registration follows attach, detach, and ID
retargeting; frozen consumers remain clean.

The stock platform texture API still schedules its compatibility frame after
the exact invalidation. Avio's DMA-BUF publication path deliberately does not:
it only marks the raster texture and notifies Dart, allowing the dirty owner to
request work while the compositor-issued frame opportunity remains the sole
cadence authority. Never add `Engine::ScheduleFrame` to
`EmbedderEngine::PublishDmabufTexture`; that would widen one producer update
back into a global frame.

### Patch 31: View-owned build scopes

Each real `View` owns and registers one native framework `BuildScope` with its
`BuildOwner`. `WidgetsBinding` resolves the engine-admitted view IDs through
that O(1) registry and builds only those scopes before rendering the same set.
A global frame builds the non-view root followed by every registered view in
stable ID order. Dirty entries settle as their scope returns, so a later scope
dirtying an already-built sibling remains demand for another opportunity and
requests that opportunity at the end of the current widget frame.
Retiring a view also retires its scheduler custody at the same lifecycle edge,
so a removed scope cannot leave a stale ID widening later frame requests.

This keeps an inactive view on its last coherent widget, render, and hit-test
tree. Pointer events therefore stay on Flutter's ordinary ordered dispatch
path; there is no Avio backlog, overflow policy, synthetic cancel, or wait for
KMS presentation. A parent-driven update to a nested `View` marks that view's
own scope dirty instead of synchronously crossing the scope boundary. The
frame-driving Flutter test binding calls the same protected build operation as
production so conformance cannot drift behind a cloned global-build step. A
binding-level regression test admits one of two dirty views and pins that the
unprocessed scope requests the next frame before its demand can be forgotten.

### Patch 32: selected-target partial raster

Root-target embedders select the exact backing target before Flutter finalizes
damage. `FlutterBackingStoreContentState` names that target and content epoch,
and supplies either exact catch-up damage for preserved pixels or unknown
history for a mandatory full repaint. The engine keeps logical frame damage
separate from selected-buffer damage in `FlutterBackingStorePresentInfo`.

Preserved Vulkan targets render directly with a single-sample `Load` first
pass, so Impeller's existing external-image queue-family barriers and exact
render-complete semaphore remain the only GPU ownership path. There is no
scratch image, copy pass, or second fence source. A full-repaint fallback
restores `Clear`, unknown or malformed target history fails safely to full, and
an empty exact buffer update terminates as `NoVisualChange` without raster.

Every target selected by the embedder is returned exactly once even when target
construction fails. Pixel tests rotate three real targets, exercise sparse
catch-up, transparent blur removal, and compare both partial and heuristic-full
fallbacks byte-for-byte against a forced full repaint.

### Patch 33: bounded Impeller pipeline-cache I/O

Impeller validates an already-open pipeline-cache file as a regular file and
checks its profile-owned byte ceiling before mapping it. The compatibility
header's declared payload must fit both that ceiling and the mapped file
remainder. Truncated, oversized, sparse, non-regular, or length-mismatched
cache entries therefore fall back to an empty Vulkan cache without exposing an
unbounded mapping or span.

Persistence applies the same payload ceiling before allocation and after the
driver fills the cache blob. The existing worker serialization and atomic
replacement remain the sole write path. Tests pin every malformed read shape
and an over-budget driver result; cache policy/configuration is supplied by the
separate resource-lifecycle extension rather than global process state.

### Patch 34: hard transient budgets and idle-only trim

The Vulkan context's cached MSAA and depth/stencil attachments obey exact
entry and byte limits. Admission reclaims only entries with no external
wrapper owner and no texture reference from submitted GPU work. If every
candidate is leased, the acquisition fails instead of dropping a live entry
from accounting and silently exceeding the configured limit. Footprint
arithmetic is checked before allocation, and explicit profiles bypass the
legacy process-environment override.

Memory-pressure cleanup reaches Impeller in Slimpeller builds on the raster
thread. The backend-neutral `TrimIdleResourceCaches` seam reports exact
before/after usage, while Vulkan removes only the same provably idle entries;
active render targets remain untouched. Per-frame thread-local descriptor and
command-pool disposal is unchanged. Focused tests pin lease conservation,
entry and byte rejection, overflow rejection, idle-only trimming, and GPU
tracked-texture preservation.

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
- `flow_unittests`: the three performance-overlay golden variants
  (`PerformanceOverlayLayerDefault.Gold`,
  `PerformanceOverlayLayer90fps.Gold`, and
  `PerformanceOverlayLayer120fps.Gold`) abort locally when the test-font
  fixture is unavailable. The remaining 317 tests pass; do not broaden this
  fixture exception to other flow tests.
- ~~Frame-path high-water probes and the signal-driven raster watchdog~~
  REMOVED by patch #24. Exact opportunity identities and terminal outcomes are
  the causal evidence now; production starts no polling thread, owns no
  process-global signal handler, and emits no per-frame `IMPORTANT` stream.
- ~~`on_empty_frame_callback` never fires at runtime~~ FIXED (25c4418f63,
  2026-07-03): both silent abort paths in `Animator` (PipelineFull before
  BeginFrame, and a vsync whose requested views resolve to none) now notify
  `OnAnimatorEmptyFrameForDisplay` with the latched/requested view set, and
  the PipelineFull retry stays view-scoped. Patch #24 supersedes the old
  recovery-watchdog inference entirely: backpressure terminalizes the exact
  target and re-arms typed demand as a separate edge.
- Per-view frame-request completion is now a **contract guarantee** of
  patch #5 (extended by 321a62c83b + the reuse-frame notify, 2026-07-08,
  squash into #5 at the next rebase): every view named in a view-scoped
  frame request completes exactly once — produced through its root-target
  callback or terminalized by a typed exact-opportunity outcome
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
3. Record the stable release SHA, audit its branch-only delta, pin the selected
   `upstream/main` SHA, and create or rename the local maintenance branch to
   `avio/<new-ref>`. Do not merge the stable release branch into main.
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
   `ApiError` failures). Existing output directories can also retain an old
   copied Dart SDK and `flutter_tester` because checkout timestamps do not
   invalidate those outputs. Explicitly build `flutter/build/dart:dart_sdk`
   and `flutter_tester`, then verify both report the Dart version in
   `flutter/third_party/dart/tools/VERSION` before running framework tests.
8. Build per config: `ninja -C engine/src/out/<config> libflutter_engine.so
   libflutter_linux_gtk.so gen_snapshot flutter_patched_sdk
   flutter/build/dart:dart_sdk flutter_tester embedder_unittests
   shell_unittests impeller_unittests flow_unittests`.
9. Run the test suites with explicit, narrow platform boundaries:
   - `shell_unittests` and `embedder_proctable_unittests` run unfiltered.
   - `embedder_unittests` excludes only
     `EmbedderTest.PresentInfo*:EmbedderTest.PopulateExistingDamage*`; generic
     compositor and platform-view tests must run.
   - On Linux, run `impeller_unittests` with the generated SwiftShader ICD,
     validation-layer path, and `VK_LAYER_KHRONOS_validation`, excluding only
     the interactive `Play`, `Compute`, and `FrameBufferObject` playground
     families. Upstream's interactive Impeller runner is macOS-only; the
     non-playground Linux result is the supported automation boundary.
   - Run `flow_unittests` in full and account for only the three named missing
     font-fixture goldens above.
   - Run
     `packages/flutter/test/widgets/view_scoped_frame_scheduling_test.dart`
     with both `--local-engine` and `--local-engine-host` naming the rebuilt
     host output.
10. Rebuild Avio (`cargo build --profile profile`) — bindgen recompiles
    against the fork header and fails loudly on ABI drift — then run the
    validation matrix (Avio `docs/engine-contract.md` §11).

Notes:
- This clone is shallow + blob:none. `bin/internal/content_aware_hash.sh`
  skips merge-base logic on shallow clones, so the flutter tool bootstrap
  would 404; Avio's build pins `FLUTTER_PREBUILT_ENGINE_VERSION` to the
  merge-base content hash automatically
  (`build_support/shell_builder/src/flutter_sdk.rs`). The same value for a
  manual framework test comes from
  `cargo run -p avio_shell_builder -- tool-engine-version <avio-root>`.
- Watch list: PR flutter/flutter#183382 (Impeller Vulkan desktop backend —
  engine-managed VkSurfaceKHR, architecturally opposite to our
  embedder-managed model), `material_ui`/`cupertino_ui` first real releases
  (Material decoupling — placeholders as of 2026-06), windowing API
  stabilization (`WidgetsBinding.windowingOwner` — future Avio shell
  integration point).
