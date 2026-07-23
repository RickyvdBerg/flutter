// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:ui';

import 'package:flutter/gestures.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';

class _ViewScopedRenderingBinding extends RenderingFlutterBinding {
  Set<int>? _testActiveViewIds;
  VoidCallback? beforeDrawFrame;
  int scheduledFrames = 0;
  final List<PointerEvent> dispatchedEvents = <PointerEvent>[];
  final List<int> hitTestViewIds = <int>[];

  @override
  Set<int>? get activeFrameViewIds => _testActiveViewIds;

  @override
  void dispatchPlatformScheduleFrame() {
    scheduledFrames += 1;
  }

  @override
  void dispatchEvent(PointerEvent event, HitTestResult? hitTestResult) {
    dispatchedEvents.add(event);
    super.dispatchEvent(event, hitTestResult);
  }

  @override
  void hitTestInView(HitTestResult result, Offset position, int viewId) {
    hitTestViewIds.add(viewId);
    super.hitTestInView(result, position, viewId);
  }

  @override
  void drawFrame() {
    beforeDrawFrame?.call();
    beforeDrawFrame = null;
    super.drawFrame();
  }

  void awaitTestFrame(int viewId) {
    markViewsAwaitingScopedFrame(<int>[viewId]);
  }

  void runScopedFrame(Set<int> viewIds) {
    assert(_testActiveViewIds == null);
    _testActiveViewIds = viewIds;
    try {
      handleBeginFrame(Duration.zero);
      handleDrawFrame();
    } finally {
      _testActiveViewIds = null;
    }
  }
}

class _FakeFlutterView extends Fake implements FlutterView {
  _FakeFlutterView(this.viewId);

  @override
  final int viewId;

  @override
  double devicePixelRatio = 1;

  @override
  Size physicalSize = const Size(800, 600);

  @override
  ViewConstraints get physicalConstraints => ViewConstraints.tight(physicalSize);

  @override
  ViewPadding padding = FakeViewPadding.zero;

  final List<Scene> renderedScenes = <Scene>[];

  @override
  void render(Scene scene, {Size? size}) {
    renderedScenes.add(scene);
  }
}

class _RenderTree {
  _RenderTree(this.binding, int viewId) {
    flutterView = _FakeFlutterView(viewId);
    renderView = RenderView(view: flutterView);
    owner.rootNode = renderView;
    binding.addRenderView(renderView);
    renderView.prepareInitialFrame();
    owner.flushLayout();
    owner.flushCompositingBits();
    owner.flushPaint();
    renderView.compositeFrame();
    flutterView.renderedScenes.clear();
  }

  final _ViewScopedRenderingBinding binding;
  late final _FakeFlutterView flutterView;
  late final RenderView renderView;
  final PipelineOwner owner = PipelineOwner();

  void dispose() {
    binding.removeRenderView(renderView);
    owner.rootNode = null;
    owner.dispose();
  }
}

void main() {
  final binding = _ViewScopedRenderingBinding();

  setUp(() {
    binding.beforeDrawFrame = null;
    binding.scheduledFrames = 0;
    binding.dispatchedEvents.clear();
    binding.hitTestViewIds.clear();
  });

  test('scoped frame defers dirty non-active view and its input', () {
    final active = _RenderTree(binding, 101);
    final dirtyInactive = _RenderTree(binding, 202);
    final cleanInactive = _RenderTree(binding, 303);
    addTearDown(active.dispose);
    addTearDown(dirtyInactive.dispose);
    addTearDown(cleanInactive.dispose);

    // Establish a mouse device whose last position belongs to the soon-dirty
    // inactive view. The active view's post-frame MouseTracker pass must not
    // re-hit-test it.
    binding.handlePointerEvent(
      const PointerHoverEvent(
        viewId: 202,
        pointer: 1,
        kind: PointerDeviceKind.mouse,
        position: Offset(10, 10),
      ),
    );
    binding.dispatchedEvents.clear();
    binding.hitTestViewIds.clear();
    late final RenderConstrainedBox newlyAttachedBox;
    binding.beforeDrawFrame = () {
      newlyAttachedBox = RenderConstrainedBox(
        additionalConstraints: const BoxConstraints.tightFor(width: 200, height: 100),
      );
      dirtyInactive.renderView.child = newlyAttachedBox;
    };

    binding.scheduledFrames = 0;
    binding.runScopedFrame(<int>{active.flutterView.viewId});

    expect(newlyAttachedBox.hasSize, isFalse);
    expect(dirtyInactive.owner.hasDirtyForFrame, isTrue);
    expect(active.flutterView.renderedScenes, hasLength(1));
    expect(dirtyInactive.flutterView.renderedScenes, isEmpty);
    expect(cleanInactive.flutterView.renderedScenes, isEmpty);
    expect(binding.dispatchedEvents, isEmpty);
    expect(binding.hitTestViewIds, isNot(contains(dirtyInactive.flutterView.viewId)));
    expect(binding.scheduledFrames, 1);

    // Direct input for the dirty view is held behind its requested frame.
    binding.handlePointerEvent(
      const PointerHoverEvent(
        viewId: 202,
        pointer: 1,
        kind: PointerDeviceKind.mouse,
        position: Offset(20, 20),
      ),
    );
    expect(binding.dispatchedEvents, isEmpty);

    binding.runScopedFrame(<int>{dirtyInactive.flutterView.viewId});

    expect(newlyAttachedBox.hasSize, isTrue);
    expect(dirtyInactive.owner.hasDirtyForFrame, isFalse);
    expect(dirtyInactive.flutterView.renderedScenes, hasLength(1));
    expect(binding.dispatchedEvents, hasLength(1));
    expect(binding.dispatchedEvents.single.position, const Offset(20, 20));
    expect(binding.hitTestViewIds, contains(dirtyInactive.flutterView.viewId));
  });

  test('deferred hover events coalesce without losing accumulated delta', () {
    final tree = _RenderTree(binding, 404);
    addTearDown(tree.dispose);

    binding.awaitTestFrame(404);
    binding.handlePointerEvent(
      const PointerHoverEvent(
        viewId: 404,
        pointer: 2,
        kind: PointerDeviceKind.mouse,
        position: Offset(10, 10),
        delta: Offset(10, 10),
      ),
    );
    binding.handlePointerEvent(
      const PointerHoverEvent(
        viewId: 404,
        pointer: 2,
        kind: PointerDeviceKind.mouse,
        position: Offset(14, 16),
        delta: Offset(4, 6),
      ),
    );

    expect(binding.dispatchedEvents, isEmpty);
    binding.runScopedFrame(<int>{404});

    expect(binding.dispatchedEvents, hasLength(1));
    final replayed = binding.dispatchedEvents.single as PointerHoverEvent;
    expect(replayed.position, const Offset(14, 16));
    expect(replayed.delta, const Offset(14, 16));
  });

  test('removing a view retires its scoped wait and deferred input', () {
    final tree = _RenderTree(binding, 505);
    var disposed = false;
    addTearDown(() {
      if (!disposed) {
        tree.dispose();
      }
    });
    binding.awaitTestFrame(505);
    binding.handlePointerEvent(
      const PointerHoverEvent(
        viewId: 505,
        pointer: 3,
        kind: PointerDeviceKind.mouse,
        position: Offset(10, 10),
      ),
    );
    expect(binding.dispatchedEvents, isEmpty);

    tree.dispose();
    disposed = true;
    binding.handlePointerEvent(
      const PointerHoverEvent(
        viewId: 505,
        pointer: 3,
        kind: PointerDeviceKind.mouse,
        position: Offset(20, 20),
      ),
    );

    expect(binding.dispatchedEvents, hasLength(1));
    expect(binding.dispatchedEvents.single.position, const Offset(20, 20));
  });

  test('deferred pointer backlog is bounded and cancels an overflowing sequence', () {
    final tree = _RenderTree(binding, 606);
    addTearDown(tree.dispose);

    binding.handlePointerEvent(
      const PointerDownEvent(viewId: 606, pointer: 4, kind: PointerDeviceKind.mouse),
    );
    binding.dispatchedEvents.clear();
    binding.awaitTestFrame(606);

    for (var index = 0; index < 513; index += 1) {
      binding.handlePointerEvent(
        PointerMoveEvent(
          viewId: 606,
          pointer: 4,
          kind: PointerDeviceKind.mouse,
          position: Offset(index.toDouble(), 0),
          delta: const Offset(1, 0),
        ),
      );
    }
    expect(binding.dispatchedEvents, isEmpty);

    binding.runScopedFrame(<int>{606});

    expect(binding.dispatchedEvents, hasLength(1));
    expect(binding.dispatchedEvents.single, isA<PointerCancelEvent>());
  });
}
