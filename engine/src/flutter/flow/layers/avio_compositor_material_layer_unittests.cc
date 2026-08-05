// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/layers/avio_compositor_material_layer.h"

#include <limits>

#include "flutter/flow/compositor_context.h"
#include "flutter/flow/layers/clip_rect_layer.h"
#include "flutter/flow/layers/clip_rrect_layer.h"
#include "flutter/flow/layers/layer_tree.h"
#include "flutter/flow/layers/opacity_layer.h"
#include "flutter/flow/layers/transform_layer.h"
#include "flutter/flow/testing/diff_context_test.h"
#include "flutter/flow/testing/layer_test.h"
#include "flutter/flow/testing/mock_layer.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {
namespace {

AvioCompositorMaterial MakeMaterial(uint64_t id, DlRect rect) {
  return AvioCompositorMaterial{
      .id = id,
      .rect = rect,
      .recipe = AvioCompositorMaterialRecipe::kTiered,
      .tier = 2,
      .corner_radius = 18.0f,
      .corner_exponent = 2.0f,
      .corner_mask = 0x0fu,
  };
}

class AvioCompositorMaterialLayerTest : public LayerTest {
 public:
  std::vector<AvioCompositorMaterial>& CollectIntoFrame() {
    preroll_context()->avio_compositor_materials = &materials_;
    preroll_context()->avio_compositor_materials_invalid = &invalid_;
    return materials_;
  }

  bool invalid() const { return invalid_; }

 private:
  std::vector<AvioCompositorMaterial> materials_;
  bool invalid_ = false;
};

TEST_F(AvioCompositorMaterialLayerTest,
       CollectionFollowsTransformClipAndOpacity) {
  auto& materials = CollectIntoFrame();
  auto material = std::make_shared<AvioCompositorMaterialLayer>(
      MakeMaterial(7u, DlRect::MakeXYWH(0.0f, 0.0f, 20.0f, 20.0f)));
  auto clip = std::make_shared<ClipRectLayer>(
      DlRect::MakeXYWH(5.0f, 4.0f, 10.0f, 8.0f), Clip::kHardEdge);
  clip->Add(material);
  auto opacity = std::make_shared<OpacityLayer>(128u, DlPoint());
  opacity->Add(clip);
  auto transform = std::make_shared<TransformLayer>(
      DlMatrix::MakeTranslation({30.0f, 40.0f}));
  transform->Add(opacity);

  transform->Preroll(preroll_context());

  ASSERT_EQ(materials.size(), 1u);
  EXPECT_EQ(materials[0].id, 7u);
  EXPECT_EQ(materials[0].rect, DlRect::MakeXYWH(35.0f, 44.0f, 10.0f, 8.0f));
  EXPECT_EQ(materials[0].corner_mask, 0u);
  EXPECT_EQ(materials[0].corner_scale, 1.0f);
  EXPECT_NEAR(materials[0].strength, 128.0f / 255.0f, 0.001f);
  EXPECT_FALSE(invalid());
}

TEST_F(AvioCompositorMaterialLayerTest,
       UniformScaleTransformsBoundsAndCornerShape) {
  auto& materials = CollectIntoFrame();
  auto material = std::make_shared<AvioCompositorMaterialLayer>(
      MakeMaterial(9u, DlRect::MakeXYWH(2.0f, 3.0f, 20.0f, 10.0f)));
  auto transform =
      std::make_shared<TransformLayer>(DlMatrix::MakeScale({2.0f, 2.0f, 1.0f}));
  transform->Add(material);

  transform->Preroll(preroll_context());

  ASSERT_EQ(materials.size(), 1u);
  EXPECT_EQ(materials[0].rect, DlRect::MakeXYWH(4.0f, 6.0f, 40.0f, 20.0f));
  EXPECT_EQ(materials[0].corner_scale, 2.0f);
  EXPECT_FALSE(invalid());
}

TEST_F(AvioCompositorMaterialLayerTest, NonUniformScaleRejectsTheFrameShape) {
  auto& materials = CollectIntoFrame();
  auto material = std::make_shared<AvioCompositorMaterialLayer>(
      MakeMaterial(9u, DlRect::MakeXYWH(2.0f, 3.0f, 20.0f, 10.0f)));
  auto transform =
      std::make_shared<TransformLayer>(DlMatrix::MakeScale({2.0f, 1.0f, 1.0f}));
  transform->Add(material);

  transform->Preroll(preroll_context());

  EXPECT_TRUE(materials.empty());
  EXPECT_TRUE(invalid());
}

TEST_F(AvioCompositorMaterialLayerTest, ReflectionRejectsTheFrameShape) {
  auto& materials = CollectIntoFrame();
  auto material = std::make_shared<AvioCompositorMaterialLayer>(
      MakeMaterial(9u, DlRect::MakeXYWH(2.0f, 3.0f, 20.0f, 10.0f)));
  auto transform = std::make_shared<TransformLayer>(
      DlMatrix::MakeScale({-1.0f, 1.0f, 1.0f}));
  transform->Add(material);

  transform->Preroll(preroll_context());

  EXPECT_TRUE(materials.empty());
  EXPECT_TRUE(invalid());
}

TEST(AvioCompositorMaterialTest, MalformedDescriptorsFailClosed) {
  auto material = MakeMaterial(1u, DlRect::MakeXYWH(0.0f, 0.0f, 20.0f, 20.0f));
  EXPECT_TRUE(IsValidAvioCompositorMaterial(material));

  material.id = 0u;
  EXPECT_FALSE(IsValidAvioCompositorMaterial(material));
  material.id = 1u;

  material.tier = 4u;
  EXPECT_FALSE(IsValidAvioCompositorMaterial(material));
  material.tier = 2u;

  material.corner_mask = 0x10u;
  EXPECT_FALSE(IsValidAvioCompositorMaterial(material));
  material.corner_mask = 0x0fu;

  material.strength = std::numeric_limits<DlScalar>::quiet_NaN();
  EXPECT_FALSE(IsValidAvioCompositorMaterial(material));
  material.strength = 1.0f;

  material.corner_exponent = 13.0f;
  EXPECT_FALSE(IsValidAvioCompositorMaterial(material));
  material.corner_exponent = 2.0f;

  material.recipe = static_cast<AvioCompositorMaterialRecipe>(99u);
  EXPECT_FALSE(IsValidAvioCompositorMaterial(material));
}

TEST(AvioCompositorMaterialTest, SelectedTargetValidationStopsBeforePainting) {
  DisplayListBuilder builder(DisplayListBuilder::kMaxCullRect);
  CompositorContext compositor_context;
  auto frame = compositor_context.AcquireFrame(
      nullptr, &builder, nullptr, DlMatrix(), false, true, nullptr, nullptr);

  auto invalid_material =
      MakeMaterial(0u, DlRect::MakeXYWH(0.0f, 0.0f, 20.0f, 20.0f));
  auto root = std::make_shared<AvioCompositorMaterialLayer>(invalid_material);
  root->Add(std::make_shared<MockLayer>(
      DlPath::MakeRect(DlRect::MakeXYWH(0.0f, 0.0f, 20.0f, 20.0f)),
      DlPaint(DlColor::kCyan())));
  LayerTree layer_tree(root, DlISize(64, 64));

  EXPECT_EQ(frame->Raster(layer_tree, true, nullptr, true),
            RasterStatus::kInvalidCompositorMaterials);

  DisplayListBuilder empty_builder;
  EXPECT_TRUE(DisplayListsEQ_Verbose(builder.Build(), empty_builder.Build()));
}

TEST_F(AvioCompositorMaterialLayerTest, CulledNodesAreNotPublished) {
  auto& materials = CollectIntoFrame();
  preroll_context()->state_stack.set_preroll_delegate(
      DlRect::MakeXYWH(100.0f, 100.0f, 20.0f, 20.0f), DlMatrix());
  auto material = std::make_shared<AvioCompositorMaterialLayer>(
      MakeMaterial(1u, DlRect::MakeXYWH(0.0f, 0.0f, 20.0f, 20.0f)));

  material->Preroll(preroll_context());

  EXPECT_TRUE(materials.empty());
  EXPECT_FALSE(invalid());
}

TEST_F(AvioCompositorMaterialLayerTest,
       PartialRasterCullDoesNotEraseRetainedMaterial) {
  auto& materials = CollectIntoFrame();
  preroll_context()->state_stack.set_preroll_delegate_with_scene_cull(
      DlRect::MakeXYWH(100.0f, 100.0f, 20.0f, 20.0f),
      DlRect::MakeXYWH(0.0f, 0.0f, 200.0f, 200.0f), DlMatrix());
  auto material = std::make_shared<AvioCompositorMaterialLayer>(
      MakeMaterial(3u, DlRect::MakeXYWH(10.0f, 10.0f, 20.0f, 20.0f)));

  material->Preroll(preroll_context());

  ASSERT_EQ(materials.size(), 1u);
  EXPECT_EQ(materials[0].id, 3u);
  EXPECT_FALSE(invalid());
}

TEST_F(AvioCompositorMaterialLayerTest,
       NonRectilinearAncestorClipRejectsFrameShape) {
  auto& materials = CollectIntoFrame();
  preroll_context()->state_stack.set_preroll_delegate_with_scene_cull(
      DlRect::MakeXYWH(0.0f, 0.0f, 200.0f, 200.0f),
      DlRect::MakeXYWH(0.0f, 0.0f, 200.0f, 200.0f), DlMatrix());
  auto material = std::make_shared<AvioCompositorMaterialLayer>(
      MakeMaterial(4u, DlRect::MakeXYWH(10.0f, 10.0f, 20.0f, 20.0f)));
  auto clip = std::make_shared<ClipRRectLayer>(
      DlRoundRect::MakeRectXY(DlRect::MakeXYWH(0.0f, 0.0f, 40.0f, 40.0f), 8.0f,
                              8.0f),
      Clip::kAntiAlias);
  clip->Add(material);

  clip->Preroll(preroll_context());

  EXPECT_TRUE(materials.empty());
  EXPECT_TRUE(invalid());
}

TEST_F(AvioCompositorMaterialLayerTest, OverflowRejectsRatherThanTruncates) {
  auto& materials = CollectIntoFrame();
  auto root = std::make_shared<ContainerLayer>();
  for (size_t index = 0; index < kMaxAvioCompositorMaterialsPerFrame + 1u;
       index++) {
    root->Add(std::make_shared<AvioCompositorMaterialLayer>(MakeMaterial(
        index + 1u,
        DlRect::MakeXYWH(static_cast<float>(index), 0.0f, 1.0f, 1.0f))));
  }

  root->Preroll(preroll_context());

  EXPECT_EQ(materials.size(), kMaxAvioCompositorMaterialsPerFrame);
  EXPECT_TRUE(invalid());
}

TEST_F(AvioCompositorMaterialLayerTest,
       DuplicateIdentityRejectsBeforePublishingASecondNode) {
  auto& materials = CollectIntoFrame();
  auto root = std::make_shared<ContainerLayer>();
  root->Add(std::make_shared<AvioCompositorMaterialLayer>(
      MakeMaterial(11u, DlRect::MakeXYWH(0.0f, 0.0f, 20.0f, 20.0f))));
  root->Add(std::make_shared<AvioCompositorMaterialLayer>(
      MakeMaterial(11u, DlRect::MakeXYWH(30.0f, 0.0f, 20.0f, 20.0f))));

  root->Preroll(preroll_context());

  ASSERT_EQ(materials.size(), 1u);
  EXPECT_EQ(materials[0].id, 11u);
  EXPECT_TRUE(invalid());
}

class AvioCompositorMaterialDiffTest : public DiffContextTest {};

TEST_F(AvioCompositorMaterialDiffTest, MetadataChangesDamageExactFootprint) {
  MockLayerTree first;
  auto first_layer = std::make_shared<AvioCompositorMaterialLayer>(
      MakeMaterial(1u, DlRect::MakeXYWH(10.0f, 20.0f, 30.0f, 40.0f)));
  first.root()->Add(first_layer);
  auto damage = DiffLayerTree(first, MockLayerTree());
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(10, 20, 40, 60));

  MockLayerTree unchanged;
  auto unchanged_layer = std::make_shared<AvioCompositorMaterialLayer>(
      MakeMaterial(1u, DlRect::MakeXYWH(10.0f, 20.0f, 30.0f, 40.0f)));
  unchanged_layer->AssignOldLayer(first_layer.get());
  unchanged.root()->Add(unchanged_layer);
  damage = DiffLayerTree(unchanged, first);
  EXPECT_TRUE(damage.frame_damage.isEmpty());

  auto changed_material =
      MakeMaterial(1u, DlRect::MakeXYWH(10.0f, 20.0f, 30.0f, 40.0f));
  changed_material.strength = 0.5f;
  MockLayerTree changed;
  auto changed_layer =
      std::make_shared<AvioCompositorMaterialLayer>(changed_material);
  changed_layer->AssignOldLayer(unchanged_layer.get());
  changed.root()->Add(changed_layer);
  damage = DiffLayerTree(changed, unchanged);
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(10, 20, 40, 60));
}

}  // namespace
}  // namespace testing
}  // namespace flutter
