// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/common/frame_opportunity.h"

#include <utility>

namespace flutter {

FrameOpportunityRegistry::FrameOpportunityRegistry(
    OutcomeCallback outcome_callback)
    : outcome_callback_(std::move(outcome_callback)) {}

bool FrameOpportunityRegistry::Open(FrameOpportunityId opportunity_id,
                                    int64_t display_id,
                                    const std::vector<int64_t>& target_ids) {
  if (opportunity_id == 0 || target_ids.empty()) {
    return false;
  }

  std::set<int64_t> targets(target_ids.begin(), target_ids.end());
  if (targets.size() != target_ids.size()) {
    return false;
  }

  std::scoped_lock lock(mutex_);
  if (records_.find(opportunity_id) != records_.end()) {
    return false;
  }

  records_.emplace(opportunity_id,
                   Record{.display_id = display_id,
                          .pending_target_ids = std::move(targets)});
  return true;
}

bool FrameOpportunityRegistry::Abandon(FrameOpportunityId opportunity_id,
                                       int64_t display_id) {
  std::scoped_lock lock(mutex_);
  const auto record = records_.find(opportunity_id);
  if (record == records_.end() || record->second.display_id != display_id) {
    return false;
  }
  records_.erase(record);
  return true;
}

bool FrameOpportunityRegistry::ClaimTargetLocked(
    FrameOpportunityId opportunity_id,
    int64_t display_id,
    int64_t target_id) {
  const auto record = records_.find(opportunity_id);
  if (record == records_.end() || record->second.display_id != display_id ||
      record->second.pending_target_ids.erase(target_id) != 1) {
    return false;
  }
  if (record->second.pending_target_ids.empty()) {
    records_.erase(record);
  }
  return true;
}

bool FrameOpportunityRegistry::ClaimTarget(FrameOpportunityId opportunity_id,
                                           int64_t display_id,
                                           int64_t target_id) {
  std::scoped_lock lock(mutex_);
  return ClaimTargetLocked(opportunity_id, display_id, target_id);
}

bool FrameOpportunityRegistry::Complete(FrameOpportunityId opportunity_id,
                                        int64_t display_id,
                                        int64_t target_id,
                                        FrameOpportunityOutcome outcome) {
  {
    std::scoped_lock lock(mutex_);
    if (!ClaimTargetLocked(opportunity_id, display_id, target_id)) {
      return false;
    }
  }
  if (outcome_callback_) {
    outcome_callback_(opportunity_id, display_id, target_id, outcome);
  }
  return true;
}

bool FrameOpportunityRegistry::Cancel(FrameOpportunityId opportunity_id,
                                      int64_t display_id,
                                      FrameOpportunityOutcome outcome) {
  if (outcome != FrameOpportunityOutcome::kTargetRemoved &&
      outcome != FrameOpportunityOutcome::kEpochStale) {
    return false;
  }

  std::set<int64_t> targets;
  {
    std::scoped_lock lock(mutex_);
    const auto record = records_.find(opportunity_id);
    if (record == records_.end() || record->second.display_id != display_id) {
      return false;
    }
    targets = std::move(record->second.pending_target_ids);
    records_.erase(record);
  }

  if (outcome_callback_) {
    for (int64_t target_id : targets) {
      outcome_callback_(opportunity_id, display_id, target_id, outcome);
    }
  }
  return true;
}

bool FrameOpportunityRegistry::IsEmpty() const {
  std::scoped_lock lock(mutex_);
  return records_.empty();
}

}  // namespace flutter
