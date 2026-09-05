// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef FLUTTER_FLOW_AVIO_WINDOW_PREVIEW_H_
#define FLUTTER_FLOW_AVIO_WINDOW_PREVIEW_H_
#include <cstddef>
#include <cstdint>
#include "flutter/display_list/geometry/dl_geometry_types.h"
namespace flutter {
constexpr size_t kMaxAvioWindowPreviewsPerFrame = 8;
// Exact retained client identity and destinations from one scene. Rect is the
// complete destination; clip may expose only part during entrance/scrolling.
struct AvioWindowPreview {
  uint64_t surface_id;
  DlRect rect;
  DlRect clip;
  float corner_radius;
  float opacity;
};
}  // namespace flutter
#endif
