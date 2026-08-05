// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/display_list/geometry/dl_region.h"
#include "flutter/flow/compositor_context.h"
#include "flutter/flow/layers/layer_tree.h"
#include "flutter/flow/testing/diff_context_test.h"

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

TEST_F(DiffContextTest, FrameDamageSeparatesLogicalAndRasterDamage) {
  const DlISize frame_size(800, 600);
  const auto unchanged_middle = CreateDisplayList(
      DlRect::MakeLTRB(300, 220, 500, 380), DlColor(0x8050A0F0));

  auto previous_root = CreateContainerLayer({
      CreateDisplayListLayer(CreateDisplayList(
          DlRect::MakeLTRB(20, 220, 120, 380), DlColor(0xFF1EB45A))),
      CreateDisplayListLayer(unchanged_middle),
      CreateDisplayListLayer(CreateDisplayList(
          DlRect::MakeLTRB(680, 220, 780, 380), DlColor(0xFFAA46D2))),
  });
  LayerTree previous(previous_root, frame_size);
  FrameDamage initial_damage;
  initial_damage.ComputeDamage(previous, /*has_raster_cache=*/false,
                               /*impeller_enabled=*/true);

  auto current_root = CreateContainerLayer({
      CreateDisplayListLayer(CreateDisplayList(
          DlRect::MakeLTRB(20, 220, 120, 380), DlColor(0xFFDC7823))),
      CreateDisplayListLayer(unchanged_middle),
      CreateDisplayListLayer(CreateDisplayList(
          DlRect::MakeLTRB(680, 220, 780, 380), DlColor(0xFF2896DC))),
  });
  LayerTree current(current_root, frame_size);
  FrameDamage damage;
  damage.SetPreviousLayerTree(&previous);
  damage.ComputeDamage(current, /*has_raster_cache=*/false,
                       /*impeller_enabled=*/true);

  const auto frame_damage = damage.GetFrameDamage();
  const auto raster_damage = damage.GetBufferDamage();
  ASSERT_TRUE(frame_damage.has_value());
  ASSERT_TRUE(raster_damage.has_value());
  EXPECT_GT(frame_damage->getRects(/*deband=*/true).size(), 1u);
  EXPECT_EQ(raster_damage->getRects(/*deband=*/true).size(), 1u);
  EXPECT_EQ(raster_damage->bounds(), frame_damage->bounds());
}

}  // namespace testing
}  // namespace flutter
