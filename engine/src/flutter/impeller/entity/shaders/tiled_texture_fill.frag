// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

precision mediump float;

#include <impeller/texture.glsl>
#include <impeller/coverage.glsl>
#include <impeller/types.glsl>

layout(constant_id = 0) const float supports_decal = 1.0;

uniform f16sampler2D texture_sampler;

uniform FragInfo {
  float x_tile_mode;
  float y_tile_mode;
  float alpha;
  float external_linear_backdrop;
}
frag_info;

in highp vec2 v_texture_coords;

out f16vec4 frag_color;

void main() {
  if (supports_decal == 1.0) {
    frag_color = texture(texture_sampler,   // sampler
                         v_texture_coords,  // texture coordinates
                         float16_t(kDefaultMipBias)) *
                 float16_t(frag_info.alpha);
  } else {
    frag_color = IPHalfSampleWithTileMode(
                     texture_sampler,                   // sampler
                     v_texture_coords,                  // texture coordinates
                     float16_t(frag_info.x_tile_mode),  // x tile mode
                     float16_t(frag_info.y_tile_mode)   // y tile mode
                     ) *
                 float16_t(frag_info.alpha);
  }
  frag_color = f16vec4(IPApplyExternalLinearBackdropCoverage(
      vec4(frag_color), frag_info.external_linear_backdrop));
}
