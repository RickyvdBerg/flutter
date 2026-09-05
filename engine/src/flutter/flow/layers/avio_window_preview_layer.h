// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef FLUTTER_FLOW_LAYERS_AVIO_WINDOW_PREVIEW_LAYER_H_
#define FLUTTER_FLOW_LAYERS_AVIO_WINDOW_PREVIEW_LAYER_H_
#include "flutter/flow/avio_window_preview.h"
#include "flutter/flow/layers/container_layer.h"
namespace flutter {
// A retained projection. For inline previews, preroll admits a bounded set
// and Paint replaces the placeholder with its matching transparent opening.
class AvioWindowPreviewLayer final : public ContainerLayer {
 public:
  AvioWindowPreviewLayer(uint64_t surface_id,
                         DlRect rect,
                         float corner_radius,
                         bool replace_children = false);
  void Diff(DiffContext* context, const Layer* old_layer) override;
  void Preroll(PrerollContext* context) override;
  void Paint(PaintContext& context) const override;
  const AvioWindowPreviewLayer* as_avio_window_preview_layer() const override {
    return this;
  }

 private:
  uint64_t surface_id_;
  DlRect rect_;
  float corner_radius_;
  bool replace_children_;
  bool admitted_ = false;
};
}  // namespace flutter
#endif
