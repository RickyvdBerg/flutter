// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_external_view_embedder.h"

#include "gtest/gtest.h"

namespace flutter {
namespace testing {
namespace {

TEST(EmbedderExternalViewEmbedderTest,
     MaterialCoordinatesApplyRootSurfaceTransformExactlyOnce) {
  AvioCompositorMaterial material = {
      .id = 1u,
      .rect = DlRect::MakeXYWH(4.0f, 6.0f, 10.0f, 12.0f),
      .recipe = AvioCompositorMaterialRecipe::kTiered,
      .tier = 2u,
      .corner_scale = 1.5f,
      .corner_radius = 18.0f,
      .corner_exponent = 2.0f,
      .corner_mask = 0x0fu,
  };

  const auto converted = ConvertAvioCompositorMaterialsToEmbedderCoordinates(
      {material}, DlMatrix::MakeScale({2.0f, 2.0f, 1.0f}), 2.0);

  ASSERT_EQ(converted.size(), 1u);
  EXPECT_EQ(converted[0].rect.left, 4.0);
  EXPECT_EQ(converted[0].rect.top, 6.0);
  EXPECT_EQ(converted[0].rect.right, 14.0);
  EXPECT_EQ(converted[0].rect.bottom, 18.0);
  EXPECT_EQ(converted[0].corner_scale, 1.5f);
}

TEST(EmbedderExternalViewEmbedderTest,
     MaterialCoordinatesScaleBottomEdgeLengthsButNotProgress) {
  AvioCompositorMaterial material = {
      .id = 2u,
      .rect = DlRect::MakeXYWH(4.0f, 6.0f, 400.0f, 80.0f),
      .recipe = AvioCompositorMaterialRecipe::kTiered,
      .tier = 2u,
      .clip_kind = AvioCompositorMaterialClipKind::kBottomEdgePull,
      .clip_parameter_0 = 240.0f,
      .clip_parameter_1 = 0.8f,
      .clip_parameter_2 = 62.0f,
      .clip_parameter_3 = 8.0f,
  };

  const auto converted = ConvertAvioCompositorMaterialsToEmbedderCoordinates(
      {material}, DlMatrix::MakeScale({3.0f, 3.0f, 1.0f}), 2.0);

  ASSERT_EQ(converted.size(), 1u);
  EXPECT_EQ(converted[0].clip_kind,
            kFlutterAvioCompositorMaterialClipBottomEdgePull);
  EXPECT_EQ(converted[0].clip_parameter_0, 360.0);
  EXPECT_NEAR(converted[0].clip_parameter_1, 0.8, 0.000001);
  EXPECT_EQ(converted[0].clip_parameter_2, 93.0);
  EXPECT_EQ(converted[0].clip_parameter_3, 12.0);
}

TEST(EmbedderExternalViewEmbedderTest,
     PreviewCoordinatesKeepCropAndApplyDprExactlyOnce) {
  const AvioWindowPreview preview{41, DlRect::MakeXYWH(8.25f, -10.5f, 100, 60),
                                  DlRect::MakeXYWH(8.25f, 0, 100, 49.5f), 8.f,
                                  0.5f};
  const auto converted = ConvertAvioWindowPreviewsToEmbedderCoordinates(
      {preview}, DlMatrix::MakeScale({2.f, 2.f, 1.f}), 2.0);
  ASSERT_EQ(converted.size(), 1u);
  EXPECT_EQ(converted[0].rect.left, 8.25);
  EXPECT_EQ(converted[0].rect.top, -10.5);
  EXPECT_EQ(converted[0].rect.bottom, 49.5);
  EXPECT_EQ(converted[0].clip.top, 0.0);
  EXPECT_EQ(converted[0].corner_radius, 8.0);
  EXPECT_EQ(converted[0].opacity, 0.5);
}

AvioCompositorMaterial MakeMaterial(uint64_t id, const DlRect& rect) {
  return AvioCompositorMaterial{
      .id = id,
      .rect = rect,
      .recipe = AvioCompositorMaterialRecipe::kTiered,
      .tier = 1u,
      .corner_scale = 1.0f,
      .corner_radius = 8.0f,
      .corner_exponent = 2.0f,
      .corner_mask = 0x0fu,
  };
}

// Whether `region` covers every pixel of `rect`.
bool RegionContains(const DlRegion& region, const DlIRect& rect) {
  const DlRegion overlap = DlRegion::MakeIntersection(region, DlRegion(rect));
  return overlap.getRects(/*deband=*/true) == std::vector<DlIRect>{rect};
}

TEST(EmbedderExternalViewEmbedderTest,
     PaintCoverageIncludesMaterialThatDrewNothing) {
  EmbedderExternalView view(DlISize(800, 600), DlMatrix());
  // A compositor material layer paints no draw ops of its own; the compositor
  // is what puts pixels in that rectangle.
  ASSERT_FALSE(view.HasEngineRenderedContents());

  const DlIRect material_rect = DlIRect::MakeLTRB(200, 100, 400, 300);
  const DlRegion coverage = PaintCoverageForFrame(
      view, {MakeMaterial(1u, DlRect::Make(material_rect))});

  EXPECT_TRUE(RegionContains(coverage, material_rect));
  EXPECT_EQ(coverage.bounds(), material_rect);
}

TEST(EmbedderExternalViewEmbedderTest,
     PaintCoverageUnionsMaterialsWithRecordedDrawOps) {
  EmbedderExternalView view(DlISize(800, 600), DlMatrix());
  const DlIRect drawn_rect = DlIRect::MakeLTRB(10, 10, 60, 60);
  view.GetCanvas()->DrawRect(DlRect::Make(drawn_rect),
                             DlPaint(DlColor::kRed()));
  ASSERT_TRUE(view.HasEngineRenderedContents());

  const DlIRect material_rect = DlIRect::MakeLTRB(200, 100, 400, 300);
  const DlRegion recorded_only = PaintCoverageForFrame(view, {});
  EXPECT_TRUE(RegionContains(recorded_only, drawn_rect));
  EXPECT_FALSE(recorded_only.intersects(material_rect));

  const DlRegion coverage = PaintCoverageForFrame(
      view, {MakeMaterial(1u, DlRect::Make(material_rect))});
  EXPECT_TRUE(RegionContains(coverage, drawn_rect));
  EXPECT_TRUE(RegionContains(coverage, material_rect));
}

TEST(EmbedderExternalViewEmbedderTest,
     RootTargetAcquisitionCarriesExactOpportunityIdentity) {
  FlutterFrameOpportunityId observed_opportunity = 0u;
  FlutterEngineDisplayId observed_display = 0u;
  FlutterViewId observed_view = -1;
  EmbedderExternalViewEmbedder embedder(
      kFlutterCompositorModeRootRenderTarget,
      /*selected_target_damage=*/true,
      /*avoid_backing_store_cache=*/true,
      /*create_render_target_callback=*/nullptr,
      [&](GrDirectContext*, const std::shared_ptr<impeller::AiksContext>&,
          const FlutterBackingStoreConfig& config,
          FlutterFrameOpportunityId opportunity_id,
          FlutterEngineDisplayId display_id) {
        observed_opportunity = opportunity_id;
        observed_display = display_id;
        observed_view = config.view_id;
        return EmbedderExternalViewEmbedder::RenderTargetAcquisition{
            .status =
                ExternalViewEmbedder::RootRenderTargetAcquisition::kWithdrawn,
            .target = nullptr,
        };
      },
      /*present_callback=*/nullptr,
      [](FlutterViewId, FlutterFrameOpportunityId, FlutterEngineDisplayId,
         FlutterPresentRenderTargetStatus, const FlutterBackingStore*,
         const FlutterBackingStorePresentInfo*,
         const std::vector<FlutterAvioCompositorMaterial>&, bool,
         const std::vector<FlutterAvioWindowPreview>&, bool) { return true; });
  ExternalViewEmbedder& boundary = embedder;
  boundary.BeginFrame(nullptr, nullptr);
  boundary.SetFrameOpportunity(FrameOpportunityContext{
      .id = 73u,
      .display_id = 11,
      .target_ids = {29},
  });
  boundary.PrepareFlutterView(DlISize(800, 600), 1.0);
  const auto target_info =
      boundary.AcquireRootRenderTarget(29, nullptr, nullptr);

  ASSERT_TRUE(target_info.has_value());
  EXPECT_EQ(observed_opportunity, 73u);
  EXPECT_EQ(observed_display, 11u);
  EXPECT_EQ(observed_view, 29);
  EXPECT_EQ(boundary.GetRootRenderTargetAcquisition(29),
            ExternalViewEmbedder::RootRenderTargetAcquisition::kWithdrawn);
}

}  // namespace
}  // namespace testing
}  // namespace flutter
