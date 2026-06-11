// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/damage_coalesce.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace flutter {

namespace {

int64_t RectArea(const DlIRect& r) {
  return static_cast<int64_t>(r.GetWidth()) * r.GetHeight();
}

// Returns the area of the gap between two rects — i.e. the area of their
// union minus the area of each rect individually.
int64_t GapArea(const DlIRect& a, const DlIRect& b) {
  DlIRect merged = a.Union(b);
  return RectArea(merged) - RectArea(a) - RectArea(b);
}

}  // namespace

std::vector<DlIRect> CoalesceDamageRects(std::vector<DlIRect> rects,
                                          const CoalesceConfig& config) {
  if (rects.size() <= 1) {
    return rects;
  }

  // Fast-path: avoid O(n^3) coalescing for pathologically large rect counts.
  if (rects.size() > 128) {
    DlIRect bounds = rects[0];
    for (size_t i = 1; i < rects.size(); i++) {
      bounds = bounds.Union(rects[i]);
    }
    return {bounds};
  }

  // Merge any rect with area < min_edge * min_edge into its nearest neighbor.
  int64_t min_area =
      static_cast<int64_t>(config.min_edge) * config.min_edge;
  for (size_t i = 0; i < rects.size();) {
    if (RectArea(rects[i]) >= min_area) {
      ++i;
      continue;
    }
    if (rects.size() == 1) {
      break;
    }
    // Find nearest neighbor by smallest gap area.
    size_t best = (i == 0) ? 1 : 0;
    int64_t best_gap = GapArea(rects[i], rects[best]);
    for (size_t j = 0; j < rects.size(); ++j) {
      if (j == i) {
        continue;
      }
      int64_t g = GapArea(rects[i], rects[j]);
      if (g < best_gap) {
        best_gap = g;
        best = j;
      }
    }
    rects[best] = rects[best].Union(rects[i]);
    rects.erase(rects.begin() + static_cast<ptrdiff_t>(i));
    if (best > i) {
      --best;
    }
    // Re-check the merged rect (it may now also be small enough or best
    // may have changed), but don't loop forever.
    i = best;
    ++i;
  }

  // Iteratively merge the closest pair until count and gap constraints
  // are satisfied.
  for (;;) {
    if (rects.size() <= 1) {
      break;
    }

    // Compute union area for gap_ratio threshold.
    DlIRect union_rect = rects[0];
    for (size_t i = 1; i < rects.size(); ++i) {
      union_rect = union_rect.Union(rects[i]);
    }
    int64_t union_area = RectArea(union_rect);

    // Find pair with smallest gap area.
    size_t best_i = 0;
    size_t best_j = 1;
    int64_t best_gap = GapArea(rects[0], rects[1]);
    for (size_t i = 0; i < rects.size(); ++i) {
      for (size_t j = i + 1; j < rects.size(); ++j) {
        int64_t g = GapArea(rects[i], rects[j]);
        if (g < best_gap) {
          best_gap = g;
          best_i = i;
          best_j = j;
        }
      }
    }

    bool over_count = rects.size() > config.max_rects;
    bool under_gap_threshold =
        union_area > 0 &&
        static_cast<float>(best_gap) / union_area < config.gap_ratio;

    if (!over_count && !under_gap_threshold) {
      break;
    }

    // Merge the closest pair.
    rects[best_i] = rects[best_i].Union(rects[best_j]);
    rects.erase(rects.begin() + static_cast<ptrdiff_t>(best_j));
  }

  return rects;
}

}  // namespace flutter
