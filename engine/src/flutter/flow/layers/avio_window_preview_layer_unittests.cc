// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "flutter/flow/compositor_context.h"
#include "flutter/flow/layers/avio_window_preview_layer.h"
#include "flutter/flow/layers/clip_rect_layer.h"
#include "flutter/flow/layers/layer_tree.h"
#include "flutter/flow/layers/opacity_layer.h"
#include "flutter/flow/layers/transform_layer.h"
#include "flutter/flow/testing/diff_context_test.h"
#include "flutter/flow/testing/layer_test.h"
#include "flutter/flow/testing/mock_layer.h"
#include "gtest/gtest.h"
namespace flutter::testing {
namespace {
class AvioWindowPreviewLayerTest : public LayerTest {
 public:
  std::vector<AvioWindowPreview> previews;
  bool invalid = false;
  void SetUp() override {
    LayerTest::SetUp();
    preroll_context()->avio_window_previews = &previews;
    preroll_context()->avio_window_previews_invalid = &invalid;
  }
};
TEST_F(AvioWindowPreviewLayerTest,
       FractionalTransformClipOpacityKeepFullDestination) {
  auto layer = std::make_shared<AvioWindowPreviewLayer>(
      41, DlRect::MakeXYWH(0, -10, 100, 60), 8);
  auto clip = std::make_shared<ClipRectLayer>(DlRect::MakeXYWH(0, 0, 100, 100),
                                              Clip::kHardEdge);
  clip->Add(layer);
  auto opacity = std::make_shared<OpacityLayer>(128, DlPoint());
  opacity->Add(clip);
  auto transform = std::make_shared<TransformLayer>(
      DlMatrix::MakeTranslation({10.25f, 20.5f}));
  transform->Add(opacity);
  transform->Preroll(preroll_context());
  ASSERT_EQ(previews.size(), 1u);
  EXPECT_EQ(previews[0].rect, DlRect::MakeXYWH(10.25f, 10.5f, 100, 60));
  EXPECT_EQ(previews[0].clip, DlRect::MakeXYWH(10.25f, 20.5f, 100, 50));
  EXPECT_FLOAT_EQ(previews[0].opacity, 128.f / 255.f);
  EXPECT_FALSE(invalid);
}
TEST_F(AvioWindowPreviewLayerTest,
       RetainedNodeFollowsNewAncestorScaleAndPosition) {
  auto layer = std::make_shared<AvioWindowPreviewLayer>(
      41, DlRect::MakeXYWH(2, 3, 100, 60), 8);
  for (int i = 1; i <= 12; i++) {
    previews.clear();
    auto transform = std::make_shared<TransformLayer>(
        DlMatrix::MakeTranslation({i * 1.25f, i * 1.5f}) *
        DlMatrix::MakeScale({0.5f, 0.5f, 1.f}));
    transform->Add(layer);
    transform->Preroll(preroll_context());
    ASSERT_EQ(previews.size(), 1u);
    EXPECT_EQ(previews[0].rect,
              DlRect::MakeXYWH(i * 1.25f + 1.f, i * 1.5f + 1.5f, 50, 30));
    EXPECT_FLOAT_EQ(previews[0].corner_radius, 4);
    EXPECT_FALSE(invalid);
  }
}
TEST_F(AvioWindowPreviewLayerTest, RasterDamageDoesNotEraseRetainedPreview) {
  preroll_context()->state_stack.set_preroll_delegate_with_scene_cull(
      DlRect::MakeXYWH(100, 100, 20, 20), DlRect::MakeXYWH(0, 0, 200, 200),
      DlMatrix());
  auto layer = std::make_shared<AvioWindowPreviewLayer>(
      41, DlRect::MakeXYWH(10, 10, 20, 20), 8);
  layer->Preroll(preroll_context());
  ASSERT_EQ(previews.size(), 1u);
  EXPECT_EQ(previews[0].clip, DlRect::MakeXYWH(10, 10, 20, 20));
  EXPECT_FALSE(invalid);
}
TEST_F(AvioWindowPreviewLayerTest,
       InlineOverflowKeepsPlaceholderWhileExplicitOverflowRejects) {
  auto root = std::make_shared<ContainerLayer>();
  for (int i = 1; i <= 9; i++)
    root->Add(std::make_shared<AvioWindowPreviewLayer>(
        i, DlRect::MakeXYWH(i * 10, 0, 8, 8), 2, true));
  root->Preroll(preroll_context());
  EXPECT_EQ(previews.size(), 8u);
  EXPECT_FALSE(invalid);
  auto explicit_layer = std::make_shared<AvioWindowPreviewLayer>(
      10, DlRect::MakeXYWH(100, 0, 8, 8), 2);
  explicit_layer->Preroll(preroll_context());
  EXPECT_EQ(previews.size(), 8u);
  EXPECT_TRUE(invalid);
}
TEST_F(AvioWindowPreviewLayerTest, FullyClippedCandidatesDoNotConsumeSlots) {
  preroll_context()->state_stack.set_preroll_delegate(
      DlRect::MakeXYWH(0, 0, 100, 100), DlMatrix());
  for (int i = 1; i <= 10; i++) {
    auto layer = std::make_shared<AvioWindowPreviewLayer>(
        i, DlRect::MakeXYWH(200, 0, 10, 10), 2, true);
    layer->Preroll(preroll_context());
  }
  auto visible = std::make_shared<AvioWindowPreviewLayer>(
      11, DlRect::MakeXYWH(0, 0, 10, 10), 2, true);
  visible->Preroll(preroll_context());
  ASSERT_EQ(previews.size(), 1u);
  EXPECT_EQ(previews[0].surface_id, 11u);
  EXPECT_FALSE(invalid);
}
TEST_F(AvioWindowPreviewLayerTest, DuplicateInlineKeepsPlaceholder) {
  auto root = std::make_shared<ContainerLayer>();
  for (int i = 0; i < 2; i++)
    root->Add(std::make_shared<AvioWindowPreviewLayer>(
        41, DlRect::MakeXYWH(i * 20, 0, 10, 10), 2, true));
  root->Preroll(preroll_context());
  EXPECT_EQ(previews.size(), 1u);
  EXPECT_FALSE(invalid);
}
TEST_F(AvioWindowPreviewLayerTest,
       AdmittedInlinePaintsHoleInsteadOfPlaceholder) {
  auto rect = DlRect::MakeXYWH(0, 0, 20, 20);
  auto layer = std::make_shared<AvioWindowPreviewLayer>(41, rect, 3, true);
  auto placeholder = std::make_shared<MockLayer>(DlPath::MakeRect(rect),
                                                 DlPaint(DlColor::kCyan()));
  layer->Add(placeholder);
  layer->Preroll(preroll_context());
  layer->Paint(display_list_paint_context());
  DisplayListBuilder expected;
  expected.DrawRoundRect(DlRoundRect::MakeRectXY(rect, 3, 3),
                         DlPaint().setBlendMode(DlBlendMode::kClear));
  EXPECT_TRUE(DisplayListsEQ_Verbose(display_list(), expected.Build()));
}
TEST_F(AvioWindowPreviewLayerTest, RejectedInlinePaintsItsPlaceholder) {
  auto rect = DlRect::MakeXYWH(0, 0, 20, 20);
  previews.push_back({41, rect, rect, 3, 1});
  auto layer = std::make_shared<AvioWindowPreviewLayer>(41, rect, 3, true);
  layer->Add(std::make_shared<MockLayer>(DlPath::MakeRect(rect),
                                         DlPaint(DlColor::kCyan())));
  layer->Preroll(preroll_context());
  layer->Paint(display_list_paint_context());
  DisplayListBuilder expected;
  expected.DrawPath(DlPath::MakeRect(rect), DlPaint(DlColor::kCyan()));
  EXPECT_TRUE(DisplayListsEQ_Verbose(display_list(), expected.Build()));
}
TEST(AvioWindowPreviewTest, InvalidExplicitDescriptorStopsBeforePaint) {
  DisplayListBuilder builder(DisplayListBuilder::kMaxCullRect);
  CompositorContext compositor;
  auto frame = compositor.AcquireFrame(nullptr, &builder, nullptr, DlMatrix(),
                                       false, true, nullptr, nullptr);
  auto root = std::make_shared<AvioWindowPreviewLayer>(
      0, DlRect::MakeXYWH(0, 0, 20, 20), 3);
  root->Add(std::make_shared<MockLayer>(
      DlPath::MakeRect(DlRect::MakeXYWH(0, 0, 20, 20)),
      DlPaint(DlColor::kCyan())));
  LayerTree tree(root, DlISize(64, 64));
  EXPECT_EQ(frame->Raster(tree, true, nullptr, true),
            RasterStatus::kInvalidWindowPreviews);
  DisplayListBuilder empty;
  EXPECT_TRUE(DisplayListsEQ_Verbose(builder.Build(), empty.Build()));
}
class AvioWindowPreviewDiffTest : public DiffContextTest {};
TEST_F(AvioWindowPreviewDiffTest,
       RetainedInlineCandidatesDamageOnlyTheirFootprint) {
  auto candidate = std::make_shared<AvioWindowPreviewLayer>(
      41, DlRect::MakeXYWH(10, 20, 30, 40), 3, true);
  auto retained = std::make_shared<ContainerLayer>();
  retained->Add(candidate);
  MockLayerTree first;
  first.root()->Add(retained);
  DiffLayerTree(first, MockLayerTree());
  MockLayerTree second;
  second.root()->Add(retained);
  auto damage = DiffLayerTree(second, first);
  // A preceding candidate can disappear, changing admission even though this
  // retained subtree is identical. Repaint its placeholder/opening together.
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(10, 20, 40, 60));
}
}  // namespace
}  // namespace flutter::testing
