// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/common/frame_opportunity.h"

#include <tuple>
#include <vector>

#include "flutter/testing/testing.h"

namespace flutter {
namespace testing {

using Outcome =
    std::tuple<FrameOpportunityId, int64_t, int64_t, FrameOpportunityOutcome>;

TEST(FrameOpportunityRegistryTest, AdmissionIsExactAndNonEmpty) {
  FrameOpportunityRegistry registry(nullptr);

  EXPECT_FALSE(registry.Open(0, 1, {11}));
  EXPECT_FALSE(registry.Open(1, 1, {}));
  EXPECT_FALSE(registry.Open(1, 1, {11, 11}));
  EXPECT_TRUE(registry.Open(1, 1, {11, 12}));
  EXPECT_FALSE(registry.Open(1, 2, {21}));
}

TEST(FrameOpportunityRegistryTest, EachTargetTerminatesExactlyOnce) {
  std::vector<Outcome> outcomes;
  FrameOpportunityRegistry registry(
      [&outcomes](FrameOpportunityId opportunity_id, int64_t display_id,
                  int64_t target_id, FrameOpportunityOutcome outcome) {
        outcomes.emplace_back(opportunity_id, display_id, target_id, outcome);
      });

  ASSERT_TRUE(registry.Open(7, 3, {31, 32}));
  EXPECT_TRUE(
      registry.Complete(7, 3, 31, FrameOpportunityOutcome::kNoVisualChange));
  EXPECT_FALSE(
      registry.Complete(7, 3, 31, FrameOpportunityOutcome::kRasterFailed));
  EXPECT_TRUE(
      registry.Complete(7, 3, 32, FrameOpportunityOutcome::kRasterFailed));
  EXPECT_FALSE(
      registry.Complete(7, 3, 32, FrameOpportunityOutcome::kRasterFailed));
  EXPECT_TRUE(registry.IsEmpty());
  EXPECT_EQ(outcomes.size(), 2u);
}

TEST(FrameOpportunityRegistryTest, CancellationClaimsOnlyPendingTargets) {
  std::vector<Outcome> outcomes;
  FrameOpportunityRegistry registry(
      [&outcomes](FrameOpportunityId opportunity_id, int64_t display_id,
                  int64_t target_id, FrameOpportunityOutcome outcome) {
        outcomes.emplace_back(opportunity_id, display_id, target_id, outcome);
      });

  ASSERT_TRUE(registry.Open(8, 4, {43, 41, 42}));
  ASSERT_TRUE(
      registry.Complete(8, 4, 42, FrameOpportunityOutcome::kNoVisualChange));
  EXPECT_TRUE(registry.Cancel(8, 4, FrameOpportunityOutcome::kEpochStale));
  EXPECT_FALSE(registry.Cancel(8, 4, FrameOpportunityOutcome::kEpochStale));
  ASSERT_EQ(outcomes.size(), 3u);
  EXPECT_EQ(std::get<2>(outcomes[0]), 42);
  EXPECT_EQ(std::get<2>(outcomes[1]), 41);
  EXPECT_EQ(std::get<2>(outcomes[2]), 43);
}

TEST(FrameOpportunityRegistryTest, ProducedClaimWinsCancellationRace) {
  std::vector<Outcome> outcomes;
  FrameOpportunityRegistry registry(
      [&outcomes](FrameOpportunityId opportunity_id, int64_t display_id,
                  int64_t target_id, FrameOpportunityOutcome outcome) {
        outcomes.emplace_back(opportunity_id, display_id, target_id, outcome);
      });

  ASSERT_TRUE(registry.Open(9, 5, {51, 52}));
  EXPECT_TRUE(registry.ClaimTarget(9, 5, 51));
  EXPECT_TRUE(registry.Cancel(9, 5, FrameOpportunityOutcome::kTargetRemoved));
  ASSERT_EQ(outcomes.size(), 1u);
  EXPECT_EQ(std::get<2>(outcomes[0]), 52);
  EXPECT_FALSE(
      registry.Complete(9, 5, 51, FrameOpportunityOutcome::kRasterFailed));
}

TEST(FrameOpportunityRegistryTest, OneDisplayMayPipelineOpportunities) {
  FrameOpportunityRegistry registry(nullptr);

  EXPECT_TRUE(registry.Open(10, 6, {61}));
  EXPECT_TRUE(registry.Open(11, 6, {61}));
  EXPECT_TRUE(registry.Abandon(10, 6));
  EXPECT_TRUE(registry.Abandon(11, 6));
  EXPECT_TRUE(registry.IsEmpty());
}

}  // namespace testing
}  // namespace flutter
