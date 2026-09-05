// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

part of 'ui_test.dart';

@pragma('vm:external-name', 'ValidateAvioPreviewAncestors')
external void _validateAncestors(SceneBuilder builder);

@pragma('vm:external-name', 'ValidateAvioPreviewScene')
external void _validateScene(Scene scene, double expectedY);

void runAvioPreviewSceneBuilder() {
  for (final bool replaceChildren in <bool>[false, true]) {
    final SceneBuilder first = SceneBuilder();
    first.pushOffset(10.25, 20.5);
    final ClipRectEngineLayer retained = first.pushClipRect(const Rect.fromLTWH(0, 0, 100, 100));
    first.pushAvioWindowPreview(
      surfaceId: 41,
      rect: const Rect.fromLTWH(2, 3, 40, 30),
      cornerRadius: 3,
      replaceChildren: replaceChildren,
    );
    _validateAncestors(first);
    first.pop();
    first.pop();
    first.pop();
    final Scene firstScene = first.build();
    _validateScene(firstScene, 23.5);
    firstScene.dispose();

    // Real framework retention inserts an already-built child into a new
    // parent-first ancestor stack. Its metadata must follow the new offset.
    final SceneBuilder second = SceneBuilder();
    second.pushOffset(10.25, 40.5);
    second.addRetained(retained);
    _validateAncestors(second);
    second.pop();
    final Scene secondScene = second.build();
    _validateScene(secondScene, 43.5);
    secondScene.dispose();
    retained.dispose();
  }
  _finish();
}
