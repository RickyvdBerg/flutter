// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:ui';

import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';

int _nextViewId = 1;

extension on PipelineOwner {
  // ignore: invalid_use_of_protected_member
  bool get needsPaint => nodesNeedingPaint.isNotEmpty;
}

void main() {
  final _TextureTestBinding binding = _TextureTestBinding();

  test('a texture notification dirties only matching consumers', () {
    final _TextureScene matching = _TextureScene(binding, textureId: 42);
    final _TextureScene unrelated = _TextureScene(binding, textureId: 99);
    addTearDown(matching.dispose);
    addTearDown(unrelated.dispose);

    binding.notifyTextureFrameAvailable(42);

    expect(matching.owner.needsPaint, isTrue);
    expect(unrelated.owner.needsPaint, isFalse);
  });

  test('changing textureId atomically retargets notifications', () {
    final _TextureScene scene = _TextureScene(binding, textureId: 1);
    addTearDown(scene.dispose);

    scene.textureBox.textureId = 2;
    scene.flush();
    binding.notifyTextureFrameAvailable(1);
    expect(scene.owner.needsPaint, isFalse);

    binding.notifyTextureFrameAvailable(2);
    expect(scene.owner.needsPaint, isTrue);
  });

  test('one texture notification fans out to every matching consumer', () {
    final _TextureScene first = _TextureScene(binding, textureId: 7);
    final _TextureScene second = _TextureScene(binding, textureId: 7);
    addTearDown(first.dispose);
    addTearDown(second.dispose);

    binding.notifyTextureFrameAvailable(7);

    expect(first.owner.needsPaint, isTrue);
    expect(second.owner.needsPaint, isTrue);
  });

  test('detached texture consumers are unregistered', () {
    final _TextureScene scene = _TextureScene(binding, textureId: 11);
    addTearDown(scene.dispose);

    scene.renderView.child = null;
    scene.flush();
    binding.notifyTextureFrameAvailable(11);

    expect(scene.owner.needsPaint, isFalse);
  });

  test('a frozen texture ignores content notifications', () {
    final _TextureScene scene = _TextureScene(binding, textureId: 12);
    addTearDown(scene.dispose);

    scene.textureBox.freeze = true;
    scene.flush();
    binding.notifyTextureFrameAvailable(12);

    expect(scene.owner.needsPaint, isFalse);
  });
}

class _TextureTestBinding extends RenderingFlutterBinding {
  void notifyTextureFrameAvailable(int textureId) {
    handleTextureFrameAvailable(textureId);
  }
}

class _TextureScene {
  _TextureScene(this.binding, {required int textureId}) {
    flutterView = _FakeFlutterView(_nextViewId++);
    renderView = RenderView(view: flutterView);
    owner = PipelineOwner()..rootNode = renderView;
    binding.rootPipelineOwner.adoptChild(owner);
    binding.addRenderView(renderView);
    renderView.prepareInitialFrame();

    textureBox = TextureBox(textureId: textureId);
    renderView.child = RenderConstrainedBox(
      additionalConstraints: const BoxConstraints.tightFor(width: 100, height: 100),
      child: textureBox,
    );
    flush();
  }

  final _TextureTestBinding binding;
  late final _FakeFlutterView flutterView;
  late final RenderView renderView;
  late final PipelineOwner owner;
  late final TextureBox textureBox;

  void flush() {
    owner.flushLayout();
    owner.flushCompositingBits();
    owner.flushPaint();
    if (owner.hasDirtyForFrame) {
      renderView.compositeFrame();
    }
  }

  void dispose() {
    renderView.child = null;
    binding.removeRenderView(renderView);
    binding.rootPipelineOwner.dropChild(owner);
    owner.rootNode = null;
    owner.dispose();
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

  @override
  void render(Scene scene, {Size? size}) {}
}
