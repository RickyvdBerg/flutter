// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_COMMON_FRAME_OPPORTUNITY_H_
#define FLUTTER_COMMON_FRAME_OPPORTUNITY_H_

#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace flutter {

using FrameOpportunityId = uint64_t;

struct FrameOpportunityContext {
  FrameOpportunityId id;
  int64_t display_id;
  // Complete target set admitted by the embedder for this opportunity. The
  // animator may narrow it to requested active views, but every admitted
  // target must still reach one terminal outcome.
  std::set<int64_t> target_ids;
};

// Terminal engine-side outcomes for one target admitted under one compositor
// frame opportunity. These describe only Flutter/host production; they never
// imply publication, KMS submission, presentation, authority, or release.
enum class FrameOpportunityOutcome : uint32_t {
  kNoVisualChange,
  kBackpressured,
  kTargetRemoved,
  kCancelledByEpoch,
  kRasterFailed,
  kHostRejected,
};

// Per-engine conservation ledger for compositor-authorized work. The host
// names the exact targets when it returns a scheduled baton. Every later
// engine exit must claim one of those targets before it can reach the host;
// cancellation claims all targets which have not already terminalized.
//
// The ledger owns identity and terminality only. It does not schedule frames,
// infer presentation, retain GPU resources, or make authority decisions.
class FrameOpportunityRegistry {
 public:
  using OutcomeCallback = std::function<void(FrameOpportunityId opportunity_id,
                                             int64_t display_id,
                                             int64_t target_id,
                                             FrameOpportunityOutcome outcome)>;

  explicit FrameOpportunityRegistry(OutcomeCallback outcome_callback);

  bool Open(FrameOpportunityId opportunity_id,
            int64_t display_id,
            const std::vector<int64_t>& target_ids);

  // Removes an opportunity opened speculatively before the waiter accepted
  // the matching baton. No target was accepted, so no outcome is emitted.
  bool Abandon(FrameOpportunityId opportunity_id, int64_t display_id);

  // Claims one target without emitting a secondary outcome. Root-target
  // delivery uses this immediately before invoking the host callback, which
  // is itself the terminal Produced/error record.
  bool ClaimTarget(FrameOpportunityId opportunity_id,
                   int64_t display_id,
                   int64_t target_id);

  // Claims one target and emits its non-render terminal outcome.
  bool Complete(FrameOpportunityId opportunity_id,
                int64_t display_id,
                int64_t target_id,
                FrameOpportunityOutcome outcome);

  // Claims every target which has not already reached a terminal edge and
  // emits one cancellation outcome for each. Work which already won the
  // terminal race is intentionally absent.
  bool Cancel(FrameOpportunityId opportunity_id,
              int64_t display_id,
              FrameOpportunityOutcome outcome);

  bool IsEmpty() const;

 private:
  struct Record {
    int64_t display_id;
    std::set<int64_t> pending_target_ids;
  };

  bool ClaimTargetLocked(FrameOpportunityId opportunity_id,
                         int64_t display_id,
                         int64_t target_id);

  const OutcomeCallback outcome_callback_;
  mutable std::mutex mutex_;
  std::unordered_map<FrameOpportunityId, Record> records_;
};

}  // namespace flutter

#endif  // FLUTTER_COMMON_FRAME_OPPORTUNITY_H_
