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
         const std::vector<FlutterAvioCompositorMaterial>&,
         bool) { return true; });
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
