// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_AVIO_COMPOSITOR_MATERIAL_H_
#define FLUTTER_FLOW_AVIO_COMPOSITOR_MATERIAL_H_

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "flutter/display_list/geometry/dl_geometry_types.h"

namespace flutter {

enum class AvioCompositorMaterialRecipe : uint32_t {
  kExplicit = 0,
  kTiered = 1,
};

/// Immutable compositor-material metadata collected from one retained scene.
///
/// The rect starts in layer-local coordinates. Layer-tree preroll copies the
/// descriptor and maps the copy into physical view coordinates, preserving the
/// original value for retained-layer diffing.
struct AvioCompositorMaterial {
  uint64_t id = 0;
  DlRect rect;
  AvioCompositorMaterialRecipe recipe = AvioCompositorMaterialRecipe::kExplicit;
  uint32_t tier = 0;
  bool uses_default_corner = false;
  /// Uniform retained-tree geometry scale after preroll. Consumers apply it
  /// to the declared or tier-default corner shape, then normalize it with the
  /// same device-pixel ratio used for `rect`.
  DlScalar corner_scale = 1.0f;
  DlScalar corner_radius = 0.0f;
  DlScalar corner_exponent = 2.0f;
  uint32_t corner_mask = 0;
  DlScalar blur_radius = 0.0f;
  DlScalar tint_red = 0.0f;
  DlScalar tint_green = 0.0f;
  DlScalar tint_blue = 0.0f;
  DlScalar tint_alpha = 0.0f;
  DlScalar saturation = 1.0f;
  DlScalar luminosity = 1.0f;
  DlScalar noise_opacity = 0.0f;
  int32_t order = 0;
  DlScalar strength = 1.0f;

  bool operator==(const AvioCompositorMaterial&) const = default;
};

/// Visible material is deliberately bounded independently of painted layers.
/// Overflow rejects the exact frame instead of truncating its scene identity.
constexpr size_t kMaxAvioCompositorMaterialsPerFrame = 64u;
constexpr DlScalar kMaxAvioCompositorMaterialCornerExponent = 12.0f;

/// Whether a retained descriptor can be represented exactly by Avio's v1
/// external material vocabulary. Dart assertions are developer guidance; this
/// check is the release-mode engine boundary and therefore rejects malformed
/// native input instead of normalizing it into a different scene.
inline bool IsValidAvioCompositorMaterial(
    const AvioCompositorMaterial& material) {
  const auto is_finite_non_negative = [](DlScalar value) {
    return std::isfinite(value) && value >= 0.0f;
  };
  const auto is_finite_unit = [](DlScalar value) {
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
  };

  if (material.id == 0u || !material.rect.IsFinite() ||
      material.rect.IsEmpty() || !std::isfinite(material.corner_scale) ||
      material.corner_scale <= 0.0f ||
      !is_finite_non_negative(material.corner_radius) ||
      !std::isfinite(material.corner_exponent) ||
      material.corner_exponent < 2.0f ||
      material.corner_exponent > kMaxAvioCompositorMaterialCornerExponent ||
      (material.corner_mask & ~0x0fu) != 0u ||
      !is_finite_non_negative(material.blur_radius) ||
      !is_finite_unit(material.tint_red) ||
      !is_finite_unit(material.tint_green) ||
      !is_finite_unit(material.tint_blue) ||
      !is_finite_unit(material.tint_alpha) ||
      !is_finite_non_negative(material.saturation) ||
      !is_finite_non_negative(material.luminosity) ||
      !is_finite_unit(material.noise_opacity) ||
      !is_finite_unit(material.strength)) {
    return false;
  }

  switch (material.recipe) {
    case AvioCompositorMaterialRecipe::kExplicit:
      return !material.uses_default_corner;
    case AvioCompositorMaterialRecipe::kTiered:
      return material.tier <= 3u;
  }
  return false;
}

}  // namespace flutter

#endif  // FLUTTER_FLOW_AVIO_COMPOSITOR_MATERIAL_H_
