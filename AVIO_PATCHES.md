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
| 26 | Share the raster pipeline reservation across displays | permanent while display-scoped scheduling drains through one raster pipeline | none |
| 27 | Transfer external Vulkan image queue-family ownership | permanent explicit-sync/ownership extension | none |
| 28 | Keep normalized degrees below a full circle | upstreamable Impeller correctness fix | submit upstream |
| 29 | [framework] Maintain typed O(1) element-to-view ownership | permanent framework correctness/performance fix | none while Avio carries view-scoped frame admission |
| 30 | [framework] Route texture frames to their exact render consumers | upstream-aligned framework/engine fix adapted for compositor pacing | open: flutter/flutter#179874 |
| 31 | [framework] Give each View an independently admitted BuildScope | permanent framework correctness fix; deletes #22's deferred-input compensation | none while Avio carries view-scoped frame admission |
| 32 | Acquire the exact embedder target before damage and keep its root pass multisampled | permanent ABI/rendering extension | none |
| 33 | Bound pipeline-cache files before mapping or allocation | permanent defensive I/O contract | upstreamable in principle |
| 34 | Enforce hard transient budgets and trim only idle resources | permanent resource-lifecycle contract | none |
| 35 | Negotiate exact resource profiles and principal-scoped pipeline-cache access | permanent ABI/lifecycle extension | none |
| 36 | Suppress raster demand with exact per-view visibility | permanent ABI/lifecycle extension | none |
| 37 | Carry retained compositor-material nodes with the exact Flutter frame | permanent ABI/scene extension | none |
| 38 | Keep a refused render target's frame demand instead of stranding the view | permanent lifecycle correctness fix | none |
| 39 | Replace ambiguous target refusal with exact typed acquisition outcomes | permanent lifecycle correctness fix | none |
| 40 | Bind a newly added view to its initial display before first-frame scheduling | permanent per-display lifecycle fix | none |
| 41 | Report exact view-scoped frame-request acceptance to the framework scheduler | permanent framework/engine lifecycle fix | none |
| 42 | Give the GTK framebuffer real multisampling | upstreamable bugfix — attach to flutter/flutter#191171 | open: flutter/flutter#191171, flutter/flutter#191234 |
| 44 | Carry typed analytic clips with retained compositor materials | permanent ABI/scene extension | none |
| 45 | Author semantic foreground coverage for an external linear-light backdrop | permanent composition-contract extension | none — Flutter otherwise cannot know that its transparent target receives a backdrop later |

Patch #5 also owns the later exact empty-frame and global-request corrections:
global requests may not be consumed by a display-scoped frame; sibling-render,
PipelineFull, unhomed-view, unresolved-view, and cached-tree-reuse exits must
terminalize the affected scoped request exactly once. The intermediate
revert/reapply commits preserve review history but do not define separate
runtime contracts.

### Patch 5: per-display scheduling is an embedder opt-in

Display-scoped frame driving belongs only to embedders that can consume a
baton naming a display. Two signals grant it, and only these two: a
`vsync_for_display_callback` supplied at engine start, which the waiter owns
and reports through `VsyncWaiter::SupportsPerDisplayVsync`, or the first
`FlutterEngineSetViewDisplay` homing a view on a display. Display registration
is deliberately not a signal. `FlutterEngineNotifyDisplayUpdate` is stock
upstream API describing topology, and every GTK application calls it through
`fl_display_monitor`; letting it flip the frame-driving mode moved those
embedders onto a clock no display drives, so each `scheduleFrame` was dropped
by the per-display loop and the app painted once and froze.

The unscoped `RequestFrame` path additionally falls through to the global vsync
path whenever the display states cannot speak for every view — views parked in
`default_state_`, or displays registered while no view is homed. Suppression
stays exact: the fall-through keys on view ownership, not on whether a display
accepted the request, so a display whose views are all non-renderable keeps its
deliberate refusal instead of having a global frame resurrect it. Together the
opt-in and the fall-through make silent frame starvation unrepresentable rather
than merely unlikely. The regressions are
`DisplayRegistrationAloneDoesNotEnterPerDisplayMode`,
`RegisteredDisplaysWithoutHomedViewsStillScheduleFrames`, and
`UnhomedViewsKeepTheGlobalFrameClockInPerDisplayMode`.

Never make the GTK embedder call `FlutterEngineSetViewDisplay` to work around
this. That opts it into per-display frame opportunities
(`RequestFrameForDisplayInternal`) whose baton, target, and completion
obligations `fl_view`/`fl_engine` cannot satisfy.

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
that O(1) registry. Every frame may settle the non-rendering root scope so it
can create or retire structural `View` boundaries; mounting a boundary leaves
its visual descendants dirty in the new view-owned scope. A global frame then
builds every registered view in stable ID order, while a scoped frame builds
only its engine-admitted views. This makes the first exact frame a valid
bootstrap without letting it build or render an unadmitted sibling. Dirty
entries settle as their scope returns, so a later scope dirtying an
already-built sibling remains demand for another opportunity and requests that
opportunity at the end of the current widget frame.
Retiring a view also retires its scheduler custody at the same lifecycle edge,
so a removed scope cannot leave a stale ID widening later frame requests.

This keeps an inactive view on its last coherent widget, render, and hit-test
tree. Pointer events therefore stay on Flutter's ordinary ordered dispatch
path; there is no Avio backlog, overflow policy, synthetic cancel, or wait for
KMS presentation. A parent-driven update to a nested `View` marks that view's
own scope dirty instead of synchronously crossing the scope boundary. The
frame-driving Flutter test binding calls the same protected build operation as
production so conformance cannot drift behind a cloned global-build step. A
binding-level regressions begin from an unbuilt root, admit one of two views,
and pin both that only the admitted child boots and that the unprocessed scope
requests the next frame before its demand can be forgotten.

### Patch 32: selected-target partial raster

Root-target embedders select the exact backing target before Flutter finalizes
damage. `FlutterBackingStoreContentState` names that target and content epoch,
and supplies either exact catch-up damage for preserved pixels or unknown
history for a mandatory full repaint. The engine keeps logical frame damage
separate from selected-buffer damage in `FlutterBackingStorePresentInfo`.

The root pass is always multisampled. Impeller antialiases geometry by
rastering it multisampled and resolving, with no analytic coverage path for a
clip edge or an arbitrary path, so a single-sample root pass renders every such
edge in the frame hard. Multisampling therefore outranks partial repaint, and a
multisampled pass cannot partially repaint: it rasters into its own attachment
and resolves that attachment over every pixel of the target, so nothing the
target preserved survives it. Such a frame narrows nothing and reports no
buffer damage — there is no rectangle it honored — while logical frame damage
stays exact and sparse, which is what accumulates catch-up damage across the
embedder's other buffers.

Restricting the resolve to the damage is not available: Impeller's Vulkan
backend begins every render pass over the whole target
(`render_pass_vk.cc` `pass_info.renderArea`), has no render-area plumbing on
`RenderTarget` or `RenderPass`, and uses render pass objects rather than
`VK_KHR_dynamic_rendering`. The Metal backend's alternative — resolve into a
scratch texture and blit only the damage — would move the write to the
embedder's image out of the render pass, which is where this patch's
external-image queue-family acquire and its render-complete semaphore both
live; neither has a blit-pass equivalent. See "Deferred" below.

If the transients budget cannot seat a multisample reservation, the frame
degrades to a single-sample pass rather than failing: a refused reservation
used to return `nullptr` and lose the whole frame, which live sessions hit
during window-chrome reconfiguration bursts. Only that fallback path renders
into the embedder's image directly, and only it takes the preserved `Load`
first pass. Sample-count transitions are logged once per change, throttled to
one line a second, because a root pass that silently loses its antialiasing
reports nothing else.

Impeller's existing external-image queue-family barriers and exact
render-complete semaphore remain the only GPU ownership path. There is no
scratch image, copy pass, or second fence source. Unknown or malformed target
history fails safely to full, and an empty exact buffer update terminates as
`NoVisualChange` without raster.
An empty current display list is not itself a no-change proof: when its exact
buffer damage can remove a previously submitted root, the engine performs a
clear-only render, replaces retained coverage, and publishes that damage. An
initial empty root has no prior scene to remove and terminalizes as
`NoVisualChange` without publishing an unknown target. After a root has been
submitted, only exact empty selected-buffer damage may terminalize a selected
target as `NoVisualChange`.
Because `Load` reads the retained color attachment, both the foreign-queue
acquire barrier and the render pass's incoming dependency declare color
attachment read access alongside write access. Clear targets remain
write-only.

Deferred: partial repaint under multisampling, by resolving into a pooled
scratch texture and blitting only the damage rect to the embedder's image, as
`surface_mtl.mm` does for Metal drawables. It needs the external-image
queue-family acquire and release (patch #27) and the render-complete semaphore
(patch #8) to work from a blit pass as well as a render pass, plus a scratch
texture per concurrently live target. Until then a frame that would have
partially repainted pays a full layer-tree paint and a full-frame resolve.

Logical frame damage remains sparse. The renderer's one rectangular canvas and
Impeller dispatch are lowered once to an explicit raster/replacement region;
that same region is the layer-tree clip, transparent clear, retained-coverage
replacement, and reported buffer damage. This prevents unchanged translucent
content between disjoint logical changes from blending over preserved pixels.
The old sparse-rect coalescer was removed because every consumer immediately
reduced its result to the same bounding rectangle, so it neither reduced raster
work nor described the pixels actually replaced.

Every target selected by the embedder is returned exactly once even when target
construction fails. Pixel tests rotate three real targets, exercise sparse
catch-up, transparent blur removal, complete scene removal, disjoint damage
around unchanged translucent content, initial empty-root suppression, and
compare both partial and heuristic-full fallbacks byte-for-byte against a
forced full repaint.

Only the root pass over the embedder's own target may keep the load action its
target declares. `InlinePassContext` clears the first pass over every other
target, because every other target comes from `RenderTargetCache`, is recycled
across frames, and declares `kDontCare` to mean "nobody chose", not "these
pixels are disposable". The opt-in is an explicit constructor argument on
`InlinePassContext` and `LazyRenderingConfig`, passed true at exactly the two
sites in `Canvas` that wrap the caller's render target, so a save layer cannot
inherit the preserved-root exemption and paint over another layer's leftovers.

A frame whose diff records a readback cannot be a partial repaint however small
its damage is: Impeller rasters such a frame into an offscreen and copies that
offscreen, whole, over the target, replacing every preserved pixel outside the
damage. `Damage::has_readback` carries that from the diff, and the frame falls
back to a full repaint next to the existing partial-repaint cost heuristic --
before the cull rect narrows Preroll, because a tree already culled to the
damage cannot be promoted back to a full repaint without erasing the rest of
the scene.

`EmbedderExternalView::Render` returns a typed outcome rather than a bool. When
every requested damage rectangle falls outside the target no pass runs, and
that is reported as `NoVisualChange` rather than as a present: the target still
holds its previous contents, and recording the frame as its history would
compute the next frame's damage against a frame that was never drawn.

Published paint coverage is the recorded draw-op region unioned with the
frame's compositor material rects. A material emits no draw ops -- the
compositor paints it -- so the recording's rtree omits it while the layer's
`Diff` puts its bounds in damage; without the union, coverage claims every
glass surface was never painted, which is exactly the region a later partial
frame would then decline to preserve.

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

### Patch 35: negotiated resource profiles and cache capabilities

Avio's Vulkan Impeller embedder profile supplies one complete resource policy
before any platform view or GPU resource exists. The versioned extension binds
hard transient entry/byte limits to an explicit pipeline-cache mode: disabled,
read-only, or read-write. A cache-capable profile carries an already-open
directory descriptor that the engine duplicates during initialization, so the
embedder selects the principal namespace and the engine never interprets a
cache path. Missing, unnegotiated, truncated, non-directory, or internally
inconsistent profiles fail initialization rather than reverting to process
environment policy.

Pipeline-cache persistence now obeys the negotiated access mode and byte cap.
The compatibility header separately versions its serialized schema and
Impeller pipeline ABI in addition to the Vulkan device, driver, pointer ABI,
and driver UUID. Incompatible or corrupt data still falls back to an empty
Vulkan cache. Stock embedders that do not negotiate the feature retain their
existing ContextVK defaults.

### Patch 36: exact per-view render visibility

Avio supplies each hosted view's render relevance explicitly as visible,
obscured, or suspended. This is neither Dart application lifecycle nor input
focus: every view remains registered, while only visible views may create new
raster demand. A visibility change never rewrites an opportunity that the
embedder already admitted. At the UI boundary that target instead terminates
exactly once as no-visual-change; subsequent demand stays suppressed until an
explicit visible update schedules one fresh view-scoped frame.

The feature requires the exact frame-opportunity contract. When the last
renderable view becomes hidden, the raster thread trims only already-idle
Impeller resources. Dart timers and application policy remain controlled by
Avio's separate typed shell lifecycle channel, so the engine never infers
authority, lock state, or suspension from missing vsync.

### Patch 37: exact retained compositor material

`SceneBuilder.pushAvioCompositorMaterial` adds a bounded, non-painting retained
node whose transformed rectangle, rectangular clip, opacity, recipe, and stable
identity are collected from the complete scene. The immutable set rides
`SurfaceFrame::SubmitInfo` into `FlutterPresentViewInfo` or
`FlutterPresentRenderTargetInfo`, so an embedder cannot pair generation N's
pixels with a later material registry. Metadata changes participate in layer
diff damage; partial-raster culling cannot erase unchanged material nodes.

The feature is negotiated as
`kFlutterAvioExtensionFeatureAtomicCompositorMaterials`, capped by
`FLUTTER_AVIO_MAX_COMPOSITOR_MATERIALS`, and rejects overflow or scene shapes
the external rectangle vocabulary cannot express. Root-target rejection is a
typed pre-raster terminal that does not quarantine a healthy GPU target. Linux
GTK exposes the same exact-frame sidecar immediately before drawing the paired
frame. This patch does not precompile shaders or allocate material render
targets: non-material trees keep the stock preroll path and all GPU resources
remain demand-driven in Avio.

### Patch 38: refused-target demand retention (superseded)

This patch proved that scarcity must not consume demand, but its null target
collapsed scarcity, withdrawal, removal, and stale authority. Patch 39 removes
that ambiguity and the blanket rearm mechanism.

### Patch 39: exact typed render-target acquisition

Root-target mode uses `FlutterRenderTargetAcquisitionCallback`, never the stock
boolean backing-store create callback. Every request carries its exact
`FlutterFrameOpportunityId`, display, and target configuration and returns one
of `Granted`, `Backpressured`, `Withdrawn`, `Removed`, `EpochStale`, or
`HostRejected`.
Unavailable acquisition never reaches the presentation callback: the
opportunity ledger terminalizes the target directly with the same typed cause.

Only `Backpressured` preserves the retained pixel baseline and asks the
Animator for later demand. Withdrawal, removal, stale authority, and invalid
host results are terminal without rearm. A freshly built tree that acquired no
lease never becomes the damage baseline. The extension ABI is version 2 and
negotiates `kFlutterAvioExtensionFeatureTypedRenderTargetAcquisition`; root
mode rejects the old untyped callback.

### Patch 40: atomic initial view/display ownership

When per-display vsync is active, `FlutterEngineAddView` registers the new
view's `FlutterWindowMetricsEvent.display_id` in the Animator before publishing
the view to Dart. This ordering matters because Dart's synchronous
`onMetricsChanged` callback may call `scheduleFrame()` before `_addView`
returns. That request must already resolve to the view's exact display instead
of collapsing into an outstanding targetless startup request.

Initial registration itself never schedules. Dart publication remains the
RuntimeController's single operation, and its stock post-success
`ScheduleFrame` requests the first frame after publication. Failed publication
rolls back only the newly registered mapping; a duplicate add cannot move or
remove the existing view. `FlutterEngineSetViewDisplay` remains the later
reassignment API and continues to schedule the destination display.

Generic embedders that have not opted into per-display vsync retain the stock
global `ScheduleFrame` behavior. The regression is
`EmbedderTest.AddViewPublishesInitialDisplayBeforeMetricsScheduleFrame`; it
schedules synchronously from `onMetricsChanged` and requires display 19, not
the default display or a silently coalesced request.

### Patch 41: exact scoped frame-request acceptance

`SchedulerBinding._hasScheduledFrame` may represent only a request the engine
actually retained. The scoped scheduling API therefore returns one synchronous
acceptance bit from `PlatformConfiguration` through `RuntimeController`,
`Engine`, and `Animator` to the framework. Unknown displays and requests whose
exact target set contains no homed renderable view return false, even when an
unrelated request is already pending on that display. An already-retained
request for the caller's own target returns true.

The framework latches `_hasScheduledFrame` only on true. Refusal leaves the
dirty view eligible for a later independent scheduling edge; making an
obscured view visible still creates the one current scoped request at the
engine owner. No timer, synthetic begin-frame, global fallback, or empty frame
is invented to repair a rejected request. Generic scheduling remains accepted
because the global engine path always retains its request.

`TestPlatformDispatcher` forwards the same acceptance result. Letting its
forward-compatibility `noSuchMethod` absorb this typed method returned `null`
as `bool`, so ordinary multi-view widget tests failed while scheduling their
first frame instead of exercising the contract.

`ShellTest.ScopedFrameRequestReportsWhetherItWasRetained` pins the engine
contract. The framework regression `a rejected engine request does not latch
the framework scheduler` proves a subsequent dirty edge can ask again, and
`TestPlatformDispatcher forwards scoped frame request acceptance` pins the
test binding to the same result-bearing API.

### Patch 42: real multisampling for GTK backing stores

`MakeRenderTargetFromBackingStoreImpeller` declares the GTK backing store as a
4x multisample target whenever the driver exposes
`GL_EXT_multisampled_render_to_texture`, but `fl_framebuffer_new` built a plain
single-sample framebuffer. Impeller binds a wrapped FBO exactly as handed to it
(`render_pass_gles.cc` `is_wrapped_fbo`) and skips its own resolve for wrapped
targets, so the declaration bought nothing: every Flutter app on Linux
rasterized at one sample and every path, clip and circle edge came out
aliased. The declaration is now truthful.

Two paths, chosen by capability. With
`GL_EXT_multisampled_render_to_texture` the colour texture is attached through
`glFramebufferTexture2DMultisampleEXT` and the depth-stencil renderbuffer
allocated with `glRenderbufferStorageMultisampleEXT`; the driver resolves into
the texture on read, so no consumer changes. Without it, colour and
depth-stencil become multisample renderbuffers and the texture moves to a
second framebuffer that `fl_framebuffer_resolve()` blits into; the compositor
calls that before either of its two consumers (`glBlitFramebuffer` for the
first layer, the texture for the rest). In the fallback Impeller declares the
wrapped FBO single-sample, which is inert: the FBO is bound as-is, no
attachment is created or sized from the descriptor, and the declared sample
count reaches only `GetTextureTypeFromDescriptor`, whose result the wrapped
path never consults. GLES pipelines carry no sample count.

The explicit resolve constrains the colour format, and that constraint has one
owner. Resolving a multisample framebuffer is only defined between identical
read and draw formats (OpenGL ES 3.0 §4.3.2) — a mismatch is `INVALID_OPERATION`
and the blit is silently dropped — and `GL_RGBA8` is the only colour format
portable across the multisample renderbuffer storage entry points. So an
explicit resolve forces the texture to RGBA8 as well, overriding the caller's
preferred format. `fl_framebuffer_get_sized_format()` is the single answer for
what the framebuffer actually holds; `create_opengl_backing_store` reads it
back for `FlutterOpenGLFramebuffer.target` instead of deriving the format a
second time, so caller and framebuffer cannot disagree about byte order. Byte
order costs nothing here: every consumer is a GPU one. The compositor's
presentation framebuffer, which `glReadPixels` does read, is single-sample and
keeps its own format.

Sample counts are negotiated, never assumed: `GL_MAX_SAMPLES` clamps the
request, a driver with no multisample capability keeps the original
single-sample framebuffer, a framebuffer the driver refuses to complete is torn
down and rebuilt single-sample, and the resolve itself is performed once at
construction so a driver that rejects it degrades then rather than dropping
frames silently forever.

`FlFramebufferTest` covers each outcome:
`ImplicitMultisampleNeedsNoResolve`, `ImplicitMultisampleKeepsTheRequestedFormat`,
`ExplicitMultisampleResolvesWithABlit`, `ExplicitMultisampleResolveFormatsMatch`,
`SamplesClampedToDriverMaximum`, `IncompleteMultisampleFallsBackToSingleSample`,
and `RejectedResolveFallsBackToSingleSample`.

### Patch 44: typed analytic material clips

Retained compositor materials carry a closed clip kind and four validated
parameters through Dart, the layer tree, the embedder ABI, and Linux frame
publication. `roundedRectangle` preserves the original material vocabulary;
`bottomEdgePull` describes the Dock's deforming analytic boundary without
flattening it into a rounded envelope or an untyped payload. Unsupported or
malformed clip descriptors reject the exact frame at the engine boundary.

### Patch 45: external linear-backdrop foreground coverage

Stock Linux Impeller's glyph correction and UberSDF luma correction both
assume Flutter owns the destination against which the coverage is painted.
Avio's transparent shell target deliberately violates that assumption for
direct-wallpaper foreground: edge or mask coverage survives into the root
target, and Smithay supplies the wallpaper during a later linear-light blend.
The same mismatch affects paragraph glyphs, analytic chrome, and transparent
image marks, so a glyph-only exponent cannot be the owning contract.

`Paint.avioCoverageMode` makes the missing destination contract explicit.
`platformDefault` preserves upstream behavior. `externalLinearBackdrop` is
carried as display-list paint state into the existing glyph-atlas, UberSDF,
direct-texture, and tiled-texture shaders. Each shader first combines authored
opacity with edge or mask coverage, then converts that final alpha exactly as
`1 - srgbToLinear(1 - alpha)` while preserving unpremultiplied source color.
This restores the display-space dark-on-light coverage response after
Smithay's linear-light blend. It changes no color, geometry, shaping, font
selection, or atlas sample and adds no draw, surface, texture, or compositor
pass. Stock glyph and UberSDF correction remains authoritative in
`platformDefault`; the external mode does not apply either approximation a
second time.

The mode belongs only on semantic foreground roles known to remain translucent
until external composition. It must not become a platform-wide default or a
Smithay heuristic over generic alpha: both would also alter text already
flattened into opaque Flutter pixels and translucent materials whose linear
coverage is intentional.

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
  tests) hangs after rendering one frame. The mechanism is exact and is not a
  flake: patch #3 made a zero-damage frame skip submission entirely
  (`Rasterizer::DrawToSurfaceUnsafe` returns `kNoVisualChange`), while these
  four upstream tests render identical content twice and then block forever on
  an untimed latch waiting for the second frame's *present* to arrive carrying
  empty damage. They hang identically in isolation; only
  `PresentInfoReceivesFullScreenDamageWhenPopulateExistingDamageIsNotProvided`
  passes, because without `populate_existing_damage` there is no buffer damage
  and the skip cannot engage. The 300 s per-test timeout then aborts the whole
  binary, which is why the runbook excludes the family rather than tolerating
  failures. Avio is Vulkan-only and never exercises the GL present path, so
  this is unused-path debt; the damage behavior Avio does rely on is gated by
  integration telemetry (`flutter_full_damage_fallback_total`). Fixing it means
  rewriting the four tests against the fork's contract (no present for a
  zero-damage frame), not changing the engine. Investigate only if a GL
  deployment ever becomes relevant.
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
