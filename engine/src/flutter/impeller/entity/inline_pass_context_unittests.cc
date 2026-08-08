// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gtest/gtest.h"
#include "impeller/core/formats.h"
#include "impeller/entity/inline_pass_context.h"

namespace impeller {
namespace testing {

// Every target a save layer or subpass paints into comes from
// `RenderTargetCache`, declaring `kDontCare` because nothing chose a load
// action for it. It is recycled, so it arrives holding the previous tenant's
// pixels. Honoring the declaration ghosts them through wherever this pass does
// not paint.
TEST(InlinePassContextTest, FirstPassClearsARecycledTarget) {
  EXPECT_EQ(ColorLoadActionForPass(/*pass_count=*/0, /*is_msaa=*/false,
                                   LoadAction::kDontCare,
                                   /*honor_declared_load_action=*/false),
            LoadAction::kClear);
  EXPECT_EQ(ColorLoadActionForPass(/*pass_count=*/0, /*is_msaa=*/true,
                                   LoadAction::kDontCare,
                                   /*honor_declared_load_action=*/false),
            LoadAction::kClear);
}

// A target that did not opt in cannot vouch for itself by declaring `kLoad`
// either: the opt-in is the caller's statement, not the target's.
TEST(InlinePassContextTest, FirstPassClearsEvenWhenTheTargetDeclaresLoad) {
  EXPECT_EQ(ColorLoadActionForPass(/*pass_count=*/0, /*is_msaa=*/false,
                                   LoadAction::kLoad,
                                   /*honor_declared_load_action=*/false),
            LoadAction::kClear);
}

// The root pass over an embedder-supplied target does opt in, which is what
// lets partial repaint keep the pixels outside the damage region.
TEST(InlinePassContextTest, FirstPassKeepsTheDeclaredActionWhenHonored) {
  EXPECT_EQ(ColorLoadActionForPass(/*pass_count=*/0, /*is_msaa=*/false,
                                   LoadAction::kLoad,
                                   /*honor_declared_load_action=*/true),
            LoadAction::kLoad);
  EXPECT_EQ(ColorLoadActionForPass(/*pass_count=*/0, /*is_msaa=*/false,
                                   LoadAction::kClear,
                                   /*honor_declared_load_action=*/true),
            LoadAction::kClear);
  EXPECT_EQ(ColorLoadActionForPass(/*pass_count=*/0, /*is_msaa=*/false,
                                   LoadAction::kDontCare,
                                   /*honor_declared_load_action=*/true),
            LoadAction::kDontCare);
}

// Later passes continue a target this context already painted. A single-sample
// attachment loads it; a fresh MSAA attachment has nothing to load and must be
// cleared before it resolves over the same resolve texture.
TEST(InlinePassContextTest, LaterPassesContinueTheTargetTheyAlreadyPainted) {
  for (bool honored : {false, true}) {
    EXPECT_EQ(ColorLoadActionForPass(/*pass_count=*/1, /*is_msaa=*/false,
                                     LoadAction::kDontCare, honored),
              LoadAction::kLoad);
    EXPECT_EQ(ColorLoadActionForPass(/*pass_count=*/1, /*is_msaa=*/true,
                                     LoadAction::kDontCare, honored),
              LoadAction::kClear);
  }
}

}  // namespace testing
}  // namespace impeller
