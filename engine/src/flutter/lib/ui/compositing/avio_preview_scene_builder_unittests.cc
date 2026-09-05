// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/compositing/scene_builder.h"

#include "flutter/display_list/dl_builder.h"
#include "flutter/flow/compositor_context.h"
#include "flutter/flow/layers/layer_tree.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/lib/ui/compositing/scene.h"
#include "flutter/shell/common/shell_test.h"
#include "flutter/testing/testing.h"

// CREATE_NATIVE_ENTRY is leaky by design.
// NOLINTBEGIN(clang-analyzer-core.StackAddressEscape)
namespace flutter::testing {

TEST_F(ShellTest, AvioPreviewSceneBuilderPreservesNestedAndRetainedMetadata) {
  auto finished = std::make_shared<fml::AutoResetWaitableEvent>();
  size_t validated_scenes = 0;
  auto validate_ancestors = [](Dart_NativeArguments args) {
    intptr_t peer = 0;
    ASSERT_FALSE(Dart_IsError(
        Dart_GetNativeInstanceField(Dart_GetNativeArgument(args, 0),
                                    tonic::DartWrappable::kPeerIndex, &peer)));
    auto* builder = reinterpret_cast<SceneBuilder*>(peer);
    ASSERT_NE(builder, nullptr);
    for (const auto& ancestor : builder->layer_stack()) {
      EXPECT_TRUE(ancestor->subtree_has_avio_window_preview());
    }
  };
  auto validate_scene = [&validated_scenes](Dart_NativeArguments args) {
    intptr_t peer = 0;
    ASSERT_FALSE(Dart_IsError(
        Dart_GetNativeInstanceField(Dart_GetNativeArgument(args, 0),
                                    tonic::DartWrappable::kPeerIndex, &peer)));
    double expected_y = 0;
    ASSERT_FALSE(Dart_IsError(
        Dart_DoubleValue(Dart_GetNativeArgument(args, 1), &expected_y)));
    auto* scene = reinterpret_cast<Scene*>(peer);
    ASSERT_NE(scene, nullptr);
    auto tree = scene->takeLayerTree(200, 200);
    ASSERT_NE(tree, nullptr);
    DisplayListBuilder canvas(DisplayListBuilder::kMaxCullRect);
    CompositorContext compositor;
    auto frame = compositor.AcquireFrame(nullptr, &canvas, nullptr, DlMatrix(),
                                         false, true, nullptr, nullptr);
    ASSERT_NE(frame, nullptr);
    // Do not inject metadata collectors. LayerTree must discover them from
    // the real SceneBuilder root, even when raster damage misses the preview.
    tree->Preroll(*frame, true, DlRect::MakeXYWH(150, 150, 10, 10));
    auto previews = tree->TakeAvioWindowPreviews();
    EXPECT_EQ(previews.size(), 1u);
    if (previews.size() == 1u) {
      EXPECT_EQ(previews[0].surface_id, 41u);
      EXPECT_EQ(previews[0].rect, DlRect::MakeXYWH(12.25f, expected_y, 40, 30));
      EXPECT_EQ(previews[0].clip, previews[0].rect);
      EXPECT_FLOAT_EQ(previews[0].corner_radius, 3);
      EXPECT_FLOAT_EQ(previews[0].opacity, 1);
    }
    validated_scenes++;
  };
  auto finish = [finished](Dart_NativeArguments) { finished->Signal(); };
  AddNativeCallback("ValidateAvioPreviewAncestors",
                    CREATE_NATIVE_ENTRY(validate_ancestors));
  AddNativeCallback("ValidateAvioPreviewScene",
                    CREATE_NATIVE_ENTRY(validate_scene));
  AddNativeCallback("Finish", CREATE_NATIVE_ENTRY(finish));

  auto settings = CreateSettingsForFixture();
  TaskRunners runners("preview-test", GetCurrentTaskRunner(), CreateNewThread(),
                      CreateNewThread(), CreateNewThread());
  auto shell = CreateShell(settings, runners);
  ASSERT_TRUE(shell->IsSetup());
  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("validateAvioPreviewSceneBuilder");
  shell->RunEngine(std::move(configuration), [](auto result) {
    EXPECT_EQ(result, Engine::RunStatus::Success);
  });
  finished->Wait();
  EXPECT_EQ(validated_scenes, 4u);
  DestroyShell(std::move(shell), runners);
}

}  // namespace flutter::testing
// NOLINTEND(clang-analyzer-core.StackAddressEscape)
