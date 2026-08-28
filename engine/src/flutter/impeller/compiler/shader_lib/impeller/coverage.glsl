// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COVERAGE_GLSL_
#define COVERAGE_GLSL_

// Converts a display-space source-over coverage mask for dark-on-light UI ink
// into the corresponding linear-light coverage response. The transfer is
// applied to transparency because source-over is expressed as
// `source + destination * (1 - alpha)`.
float IPExternalLinearBackdropCoverage(float alpha) {
  float encoded_transparency = clamp(1.0 - alpha, 0.0, 1.0);
  float linear_transparency = encoded_transparency <= 0.04045
                                  ? encoded_transparency / 12.92
                                  : pow((encoded_transparency + 0.055) / 1.055,
                                        2.4);
  return 1.0 - linear_transparency;
}

// Preserves the unpremultiplied source color while converting its final,
// already-combined opacity and edge/mask coverage.
vec4 IPApplyExternalLinearBackdropCoverage(vec4 premultiplied_color,
                                           float enabled) {
  if (enabled < 0.5 || premultiplied_color.a <= 0.0) {
    return premultiplied_color;
  }
  float alpha =
      IPExternalLinearBackdropCoverage(premultiplied_color.a);
  return vec4(premultiplied_color.rgb * (alpha / premultiplied_color.a),
              alpha);
}

#endif
