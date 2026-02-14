// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/testing/diff_context_test.h"
#include "flutter/display_list/geometry/dl_region.h"
#include "flutter/flow/damage_coalesce.h"

namespace flutter {
namespace testing {

TEST_F(DiffContextTest, ClipAlignment) {
  MockLayerTree t1;
  t1.root()->Add(CreateDisplayListLayer(
      CreateDisplayList(DlRect::MakeLTRB(30, 30, 50, 50))));
  auto damage = DiffLayerTree(t1, MockLayerTree(), DlIRect(), 0, 0);
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(30, 30, 50, 50));
  EXPECT_EQ(damage.buffer_damage.bounds(), DlIRect::MakeLTRB(30, 30, 50, 50));

  damage = DiffLayerTree(t1, MockLayerTree(), DlIRect(), 1, 1);
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(30, 30, 50, 50));
  EXPECT_EQ(damage.buffer_damage.bounds(), DlIRect::MakeLTRB(30, 30, 50, 50));

  damage = DiffLayerTree(t1, MockLayerTree(), DlIRect(), 8, 1);
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(24, 30, 56, 50));
  EXPECT_EQ(damage.buffer_damage.bounds(), DlIRect::MakeLTRB(24, 30, 56, 50));

  damage = DiffLayerTree(t1, MockLayerTree(), DlIRect(), 1, 8);
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(30, 24, 50, 56));
  EXPECT_EQ(damage.buffer_damage.bounds(), DlIRect::MakeLTRB(30, 24, 50, 56));

  damage = DiffLayerTree(t1, MockLayerTree(), DlIRect(), 16, 16);
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(16, 16, 64, 64));
  EXPECT_EQ(damage.buffer_damage.bounds(), DlIRect::MakeLTRB(16, 16, 64, 64));
}

TEST_F(DiffContextTest, DisjointDamage) {
  DlISize frame_size = DlISize(90, 90);
  auto in_bounds_dl = CreateDisplayList(DlRect::MakeLTRB(30, 30, 50, 50));
  auto out_bounds_dl = CreateDisplayList(DlRect::MakeLTRB(100, 100, 120, 120));

  // We need both DisplayLists to be non-empty.
  ASSERT_FALSE(in_bounds_dl->GetBounds().IsEmpty());
  ASSERT_FALSE(out_bounds_dl->GetBounds().IsEmpty());

  // We need the in_bounds DisplayList to be inside the frame size.
  // We need the out_bounds DisplayList to be completely outside the frame.
  ASSERT_TRUE(DlRect::MakeSize(frame_size).Contains(in_bounds_dl->GetBounds()));
  ASSERT_FALSE(DlRect::MakeSize(frame_size)
                   .IntersectsWithRect(out_bounds_dl->GetBounds()));

  MockLayerTree t1(frame_size);
  t1.root()->Add(CreateDisplayListLayer(in_bounds_dl));

  MockLayerTree t2(frame_size);
  // Include previous
  t2.root()->Add(CreateDisplayListLayer(in_bounds_dl));
  // Add a new layer that is out of frame bounds
  t2.root()->Add(CreateDisplayListLayer(out_bounds_dl));

  // Cannot use DiffLayerTree because it implicitly adds a clip layer
  // around the tree, but we want the out of bounds dl to not be pruned
  // to test the intersection code inside layer::Diff/ComputeDamage
  // damage = DiffLayerTree(t2, t1, DlIRect(), 0, 0);

  DiffContext dc(frame_size, t2.paint_region_map(), t1.paint_region_map(), true,
                 false);
  t2.root()->Diff(&dc, t1.root());
  auto damage = dc.ComputeDamage(DlRegion(), 0, 0);
  EXPECT_TRUE(damage.frame_damage.isEmpty());
  EXPECT_TRUE(damage.buffer_damage.isEmpty());
}

TEST_F(DiffContextTest, DisjointRectsProduceMultipleRegionRects) {
  // Two disjoint display lists at opposite corners should produce
  // a DlRegion with more than one rect (not a single bounding box).
  auto dl1 = CreateDisplayList(DlRect::MakeLTRB(0, 0, 50, 50));
  auto dl2 = CreateDisplayList(DlRect::MakeLTRB(200, 200, 250, 250));

  MockLayerTree t1;
  t1.root()->Add(CreateDisplayListLayer(dl1));
  t1.root()->Add(CreateDisplayListLayer(dl2));

  auto damage = DiffLayerTree(t1, MockLayerTree());
  // Bounding box covers both rects.
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(0, 0, 250, 250));
  // The region should contain multiple disjoint rects, not a single box.
  auto rects = damage.frame_damage.getRects(/*deband=*/true);
  EXPECT_GT(rects.size(), 1u);
}

TEST_F(DiffContextTest, IdenticalTreesProduceEmptyDamage) {
  auto dl1 = CreateDisplayList(DlRect::MakeLTRB(10, 10, 60, 60));

  MockLayerTree t1;
  t1.root()->Add(CreateDisplayListLayer(dl1));

  MockLayerTree t2;
  t2.root()->Add(CreateDisplayListLayer(dl1));

  DiffLayerTree(t1, MockLayerTree());
  auto damage = DiffLayerTree(t2, t1);
  EXPECT_TRUE(damage.frame_damage.isEmpty());
  EXPECT_TRUE(damage.buffer_damage.isEmpty());
}

// ---- CoalesceDamageRects tests ----

TEST(CoalesceDamageRectsTest, EmptyInput) {
  auto result = CoalesceDamageRects({});
  EXPECT_TRUE(result.empty());
}

TEST(CoalesceDamageRectsTest, SingleRect) {
  std::vector<DlIRect> rects = {DlIRect::MakeLTRB(10, 10, 50, 50)};
  auto result = CoalesceDamageRects(rects);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0], DlIRect::MakeLTRB(10, 10, 50, 50));
}

TEST(CoalesceDamageRectsTest, MaxRectsEnforced) {
  // Create 20 well-separated rects.
  std::vector<DlIRect> rects;
  for (int i = 0; i < 20; i++) {
    int x = i * 200;
    rects.push_back(DlIRect::MakeLTRB(x, 0, x + 100, 100));
  }
  CoalesceConfig config;
  config.max_rects = 4;
  auto result = CoalesceDamageRects(std::move(rects), config);
  EXPECT_LE(result.size(), 4u);
  EXPECT_GE(result.size(), 1u);
}

TEST(CoalesceDamageRectsTest, SmallRectsAreMerged) {
  // A tiny rect (below min_edge threshold) should be merged into its neighbor.
  std::vector<DlIRect> rects = {
      DlIRect::MakeLTRB(0, 0, 100, 100),
      DlIRect::MakeLTRB(110, 110, 115, 115),  // 5x5, below 64x64 threshold
  };
  CoalesceConfig config;
  config.min_edge = 64;
  auto result = CoalesceDamageRects(std::move(rects), config);
  EXPECT_EQ(result.size(), 1u);
}

TEST(CoalesceDamageRectsTest, WellSeparatedRectsPreserved) {
  // Two well-separated, large rects should stay separate.
  std::vector<DlIRect> rects = {
      DlIRect::MakeLTRB(0, 0, 100, 100),
      DlIRect::MakeLTRB(5000, 5000, 5100, 5100),
  };
  CoalesceConfig config;
  config.max_rects = 8;
  config.gap_ratio = 0.01f;  // very strict gap ratio
  auto result = CoalesceDamageRects(std::move(rects), config);
  EXPECT_EQ(result.size(), 2u);
}

}  // namespace testing
}  // namespace flutter
