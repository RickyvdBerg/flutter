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

}  // namespace
}  // namespace testing
}  // namespace flutter
