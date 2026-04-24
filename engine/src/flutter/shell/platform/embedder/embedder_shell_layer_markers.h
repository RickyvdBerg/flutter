// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_SHELL_LAYER_MARKERS_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_SHELL_LAYER_MARKERS_H_

#include <cstdint>
#include <optional>

#include "flutter/shell/platform/embedder/embedder.h"

namespace flutter {

// Private Avio shell-layer boundary ABI carried through PlatformViewLayer IDs.
// The embedder consumes these markers before they are exposed as platform views.
namespace shell_layer_marker {

constexpr FlutterPlatformViewIdentifier kUnderlay = -901001;
constexpr FlutterPlatformViewIdentifier kOverlay = -901002;
constexpr FlutterPlatformViewIdentifier kPerWindowChromeExplicitBase =
    -9000000000000000000LL;
constexpr uint64_t kPerWindowChromeExplicitMaxVisualIdentifier =
    899999999999999999ULL;

inline std::optional<uint64_t> DecodePerWindowChromeVisualIdentifier(
    FlutterPlatformViewIdentifier identifier) {
  if (identifier <= kPerWindowChromeExplicitBase) {
    return std::nullopt;
  }
  constexpr FlutterPlatformViewIdentifier kPerWindowChromeExplicitMaxViewId =
      kPerWindowChromeExplicitBase +
      static_cast<FlutterPlatformViewIdentifier>(
          kPerWindowChromeExplicitMaxVisualIdentifier);
  if (identifier > kPerWindowChromeExplicitMaxViewId) {
    return std::nullopt;
  }
  return static_cast<uint64_t>(identifier - kPerWindowChromeExplicitBase);
}

inline bool IsBoundary(FlutterPlatformViewIdentifier identifier) {
  return identifier == kUnderlay || identifier == kOverlay ||
         DecodePerWindowChromeVisualIdentifier(identifier).has_value();
}

inline FlutterBackingStoreRequestType BackingStoreRequestTypeFor(
    FlutterPlatformViewIdentifier identifier) {
  return DecodePerWindowChromeVisualIdentifier(identifier).has_value()
             ? kFlutterBackingStoreRequestTypePerWindowChrome
             : kFlutterBackingStoreRequestTypeView;
}

inline FlutterShellLayerRole RoleFor(FlutterPlatformViewIdentifier identifier) {
  if (identifier == kUnderlay) {
    return kFlutterShellLayerRoleUnderlay;
  }
  if (identifier == kOverlay) {
    return kFlutterShellLayerRoleOverlay;
  }
  if (DecodePerWindowChromeVisualIdentifier(identifier).has_value()) {
    return kFlutterShellLayerRolePerWindowChrome;
  }
  return kFlutterShellLayerRoleBackground;
}

}  // namespace shell_layer_marker

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_SHELL_LAYER_MARKERS_H_
