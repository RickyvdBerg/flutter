// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef IMPELLER_SUPPORTS_RENDERING
#include "flutter/shell/platform/embedder/embedder_render_target_impeller.h"

#include <vector>

#include "flutter/impeller/display_list/aiks_context.h"
#include "flutter/impeller/renderer/render_target.h"
#include "flutter/shell/platform/embedder/embedder_external_view_embedder.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {
namespace {

TEST(EmbedderRenderTargetImpellerTest, DefersCreationAndReleasesUnusedLease) {
  int creations = 0;
  std::vector<int> release_order;
  auto aiks = std::make_shared<impeller::AiksContext>(nullptr, nullptr);
  {
    EmbedderRenderTargetImpeller target(
        {}, aiks, DlISize(800, 600),
        [&]() -> std::unique_ptr<impeller::RenderTarget> {
          creations++;
          return nullptr;
        },
        [&] { release_order.push_back(2); },
        [&] { release_order.push_back(1); });
    EXPECT_EQ(target.GetRenderTargetSize(), DlISize(800, 600));
    EXPECT_EQ(target.GetAiksContext(), aiks);
    EXPECT_TRUE(target.RasterReplacesWholeTarget());
    EXPECT_NE(target.GetBackingStore(), nullptr);
    EXPECT_EQ(creations, 0);
  }
  EXPECT_EQ(creations, 0);
  EXPECT_EQ(release_order, (std::vector<int>{1, 2}));
}

TEST(EmbedderRenderTargetImpellerTest, MaterializesOnlyOnce) {
  int creations = 0;
  auto aiks = std::make_shared<impeller::AiksContext>(nullptr, nullptr);
  EmbedderRenderTargetImpeller target(
      {}, aiks, DlISize(800, 600),
      [&] {
        creations++;
        return std::make_unique<impeller::RenderTarget>();
      },
      [] {}, [] {});
  auto* first = target.GetImpellerRenderTarget();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, target.GetImpellerRenderTarget());
  EXPECT_EQ(creations, 1);
}

// Exercise the real selected-target metadata and terminal paths. No GPU is
// needed: a skipped target must never invoke its factory, and a failed factory
// must terminalize as raster failure without falling into Slimpeller's fatal
// backend-switch path.
void CheckDeferredTerminal(bool unchanged) {
  int creations = 0;
  int releases = 0;
  int presents = 0;
  auto aiks = std::make_shared<impeller::AiksContext>(nullptr, nullptr);
  FlutterRegion empty_damage{.struct_size = sizeof(FlutterRegion)};
  FlutterBackingStoreContentState contents{
      .struct_size = sizeof(FlutterBackingStoreContentState),
      .target_identifier = 9u,
      .content_epoch = 5u,
      .preserved_contents = true,
      .existing_damage = &empty_damage,
  };
  FlutterBackingStore store{
      .struct_size = sizeof(FlutterBackingStore),
      .type = kFlutterBackingStoreTypeVulkan,
      .content_state = &contents,
  };
  EmbedderExternalViewEmbedder embedder(
      kFlutterCompositorModeRootRenderTarget, true, true, nullptr,
      [&](GrDirectContext*, const std::shared_ptr<impeller::AiksContext>&,
          const FlutterBackingStoreConfig&, FlutterFrameOpportunityId,
          FlutterEngineDisplayId) {
        return EmbedderExternalViewEmbedder::RenderTargetAcquisition{
            .status =
                ExternalViewEmbedder::RootRenderTargetAcquisition::kGranted,
            .target = std::make_unique<EmbedderRenderTargetImpeller>(
                store, aiks, DlISize(800, 600),
                [&]() -> std::unique_ptr<impeller::RenderTarget> {
                  creations++;
                  return nullptr;
                },
                [&] { releases++; }, [] {}),
        };
      },
      nullptr,
      [&](FlutterViewId view, FlutterFrameOpportunityId opportunity,
          FlutterEngineDisplayId display,
          FlutterPresentRenderTargetStatus status,
          const FlutterBackingStore* backing,
          const FlutterBackingStorePresentInfo*,
          const std::vector<FlutterAvioCompositorMaterial>&, bool,
          const std::vector<FlutterAvioWindowPreview>&, bool) {
        presents++;
        EXPECT_EQ(view, 29);
        EXPECT_EQ(opportunity, 73u);
        EXPECT_EQ(display, 11u);
        EXPECT_NE(backing, nullptr);
        EXPECT_EQ(status, unchanged
                              ? kFlutterPresentRenderTargetStatusNoVisualChange
                              : kFlutterPresentRenderTargetStatusRasterFailed);
        return true;
      });
  ExternalViewEmbedder& boundary = embedder;
  boundary.BeginFrame(nullptr, nullptr);
  boundary.SetFrameOpportunity(FrameOpportunityContext{
      .id = 73u,
      .display_id = 11,
      .target_ids = {29},
  });
  boundary.PrepareFlutterView(DlISize(800, 600), 1.0);
  auto info = boundary.AcquireRootRenderTarget(29, nullptr, aiks);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->target_identifier, 9u);
  EXPECT_EQ(info->content_epoch, 5u);
  EXPECT_TRUE(info->preserved_contents);
  ASSERT_TRUE(info->existing_damage.has_value());
  EXPECT_TRUE(info->existing_damage->isEmpty());
  EXPECT_TRUE(info->raster_replaces_whole_target);
  EXPECT_EQ(creations, 0);
  if (!unchanged) {
    boundary.GetRootCanvas()->DrawRect(DlRect::MakeXYWH(0, 0, 50, 50),
                                       DlPaint(DlColor::kRed()));
  }
  auto frame = std::make_unique<SurfaceFrame>(
      nullptr, *info, [](SurfaceFrame&, DlCanvas*) { return true; },
      [](SurfaceFrame&) { return true; }, DlISize(800, 600));
  SurfaceFrame::SubmitInfo submit;
  submit.buffer_damage =
      unchanged ? DlRegion() : DlRegion(DlIRect::MakeWH(800, 600));
  frame->set_submit_info(submit);
  boundary.SubmitFlutterView(29, nullptr, aiks, std::move(frame));
  EXPECT_EQ(creations, unchanged ? 0 : 1);
  EXPECT_EQ(presents, 1);
  EXPECT_EQ(releases, 1);
}

TEST(EmbedderRenderTargetImpellerTest,
     UnchangedTargetKeepsHistoryWithoutAllocation) {
  CheckDeferredTerminal(true);
}

TEST(EmbedderRenderTargetImpellerTest,
     AllocationFailureHasOneRasterFailureOutcome) {
  CheckDeferredTerminal(false);
}

}  // namespace
}  // namespace testing
}  // namespace flutter
#endif  // IMPELLER_SUPPORTS_RENDERING
