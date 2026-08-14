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
  bool acceptScheduledFrames = true;
  final List<PointerEvent> dispatchedEvents = <PointerEvent>[];
  final List<int> hitTestViewIds = <int>[];

  @override
  Set<int>? get activeFrameViewIds => _testActiveViewIds;

  @override
  bool dispatchPlatformScheduleFrame() {
    scheduledFrames += 1;
    return acceptScheduledFrames;
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

  void notifyTextureFrameAvailable(int textureId) {
    handleTextureFrameAvailable(textureId);
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
    binding.rootPipelineOwner.adoptChild(owner);
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
    binding.rootPipelineOwner.dropChild(owner);
    owner.rootNode = null;
    owner.dispose();
  }
}

void main() {
  final binding = _ViewScopedRenderingBinding();

  setUp(() {
    binding.beforeDrawFrame = null;
    binding.scheduledFrames = 0;
    binding.acceptScheduledFrames = true;
    binding.dispatchedEvents.clear();
    binding.hitTestViewIds.clear();
  });

  test('a rejected engine request does not latch the framework scheduler', () {
    binding.acceptScheduledFrames = false;

    binding.scheduleFrame();

    expect(binding.scheduledFrames, 1);
    expect(binding.hasScheduledFrame, isFalse);

    // A later independent dirty edge is free to ask again; there is no timer
    // and no synthetic begin-frame needed to recover the scheduler.
    binding.scheduleFrame();
    expect(binding.scheduledFrames, 2);
  });

  test('scoped frame leaves inactive view coherent and its input live', () {
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
    binding.beforeDrawFrame = () {
      dirtyInactive.renderView.markNeedsPaint();
    };

    binding.scheduledFrames = 0;
    binding.runScopedFrame(<int>{active.flutterView.viewId});

    expect(dirtyInactive.owner.hasDirtyForFrame, isTrue);
    expect(active.flutterView.renderedScenes, hasLength(1));
    expect(dirtyInactive.flutterView.renderedScenes, isEmpty);
    expect(cleanInactive.flutterView.renderedScenes, isEmpty);
    expect(binding.dispatchedEvents, isEmpty);
    expect(binding.hitTestViewIds, isNot(contains(dirtyInactive.flutterView.viewId)));
    expect(binding.scheduledFrames, 1);

    // The inactive view remains on its last coherent build/layout state, so
    // input does not need a second queue while its repaint awaits admission.
    binding.handlePointerEvent(
      const PointerHoverEvent(
        viewId: 202,
        pointer: 1,
        kind: PointerDeviceKind.mouse,
        position: Offset(20, 20),
      ),
    );
    expect(binding.dispatchedEvents, hasLength(1));
    expect(binding.dispatchedEvents.single.position, const Offset(20, 20));
    expect(binding.hitTestViewIds, contains(dirtyInactive.flutterView.viewId));

    binding.runScopedFrame(<int>{dirtyInactive.flutterView.viewId});

    expect(dirtyInactive.owner.hasDirtyForFrame, isFalse);
    expect(dirtyInactive.flutterView.renderedScenes, hasLength(1));
  });

  test('a texture frame schedules and renders only its owning view', () {
    final textured = _RenderTree(binding, 350);
    final cleanSibling = _RenderTree(binding, 351);
    addTearDown(textured.dispose);
    addTearDown(cleanSibling.dispose);

    textured.renderView.child = TextureBox(textureId: 9001);
    binding.runScopedFrame(<int>{textured.flutterView.viewId});
    textured.flutterView.renderedScenes.clear();
    cleanSibling.flutterView.renderedScenes.clear();
    binding.scheduledFrames = 0;

    binding.notifyTextureFrameAvailable(9001);

    expect(textured.owner.hasDirtyForFrame, isTrue);
    expect(cleanSibling.owner.hasDirtyForFrame, isFalse);
    expect(binding.scheduledFrames, 1);

    binding.runScopedFrame(<int>{textured.flutterView.viewId});

    expect(textured.flutterView.renderedScenes, hasLength(1));
    expect(cleanSibling.flutterView.renderedScenes, isEmpty);
  });

  test('scoped render demand never rewrites structural pointer events', () {
    final tree = _RenderTree(binding, 404);
    addTearDown(tree.dispose);

    tree.renderView.markNeedsPaint();
    binding.handlePointerEvent(
      const PointerDownEvent(viewId: 404, pointer: 4, kind: PointerDeviceKind.mouse),
    );
    binding.handlePointerEvent(
      const PointerUpEvent(viewId: 404, pointer: 4, kind: PointerDeviceKind.mouse),
    );

    expect(binding.dispatchedEvents, hasLength(2));
    expect(binding.dispatchedEvents.first, isA<PointerDownEvent>());
    expect(binding.dispatchedEvents.last, isA<PointerUpEvent>());
  });
}
