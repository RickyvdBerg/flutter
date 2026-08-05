// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_LAYERS_AVIO_COMPOSITOR_MATERIAL_LAYER_H_
#define FLUTTER_FLOW_LAYERS_AVIO_COMPOSITOR_MATERIAL_LAYER_H_

#include "flutter/flow/avio_compositor_material.h"
#include "flutter/flow/layers/container_layer.h"

namespace flutter {

/// A retained, non-painting scene node describing material owned by the
/// external compositor. It participates in layer diffing so metadata-only
/// changes produce exact frame damage.
class AvioCompositorMaterialLayer final : public ContainerLayer {
 public:
  explicit AvioCompositorMaterialLayer(AvioCompositorMaterial material);

  void Diff(DiffContext* context, const Layer* old_layer) override;
  void Preroll(PrerollContext* context) override;
  void Paint(PaintContext& context) const override;

  const AvioCompositorMaterialLayer* as_avio_compositor_material_layer()
      const override {
    return this;
  }

  const AvioCompositorMaterial& material() const { return material_; }

 private:
  AvioCompositorMaterial material_;

  FML_DISALLOW_COPY_AND_ASSIGN(AvioCompositorMaterialLayer);
};

}  // namespace flutter

#endif  // FLUTTER_FLOW_LAYERS_AVIO_COMPOSITOR_MATERIAL_LAYER_H_
