// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_DAMAGE_COALESCE_H_
#define FLUTTER_FLOW_DAMAGE_COALESCE_H_

#include <vector>
#include "flutter/display_list/geometry/dl_region.h"

namespace flutter {

struct CoalesceConfig {
  size_t max_rects = 8;
  float gap_ratio = 0.15f;
  int32_t min_edge = 64;
};

std::vector<DlIRect> CoalesceDamageRects(
    std::vector<DlIRect> rects,
    const CoalesceConfig& config = {});

}  // namespace flutter

#endif  // FLUTTER_FLOW_DAMAGE_COALESCE_H_
