#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace crimson::crop {

constexpr float kDefaultRoiInsetWidthPx = 240.0f;
constexpr float kMinimumRoiInsetWidthPx = 120.0f;
constexpr float kMaximumRoiInsetWidthPx = 420.0f;

enum class RoiInsetOverlayPolicy : uint8_t {
  None,
  SelectedComponent,
  MatchCamera,
};

enum class RoiInsetOrientation : uint8_t {
  Acquisition,
  HeadingNormalized,
};

// Presentation intent only. Coordinate transforms remain the responsibility of
// an adapter until the acquisition-to-presentation contract is finalized.
struct RoiInsetPresentationState {
  bool visible = true;
  float width_px = kDefaultRoiInsetWidthPx;
  bool show_label = true;
  RoiInsetOverlayPolicy overlay_policy = RoiInsetOverlayPolicy::MatchCamera;
  RoiInsetOrientation orientation = RoiInsetOrientation::Acquisition;
};

struct RoiInsetPresentationCapabilities {
  bool selected_component_overlay = false;
  bool match_camera_overlays = false;
  bool heading_normalization = false;
};

inline bool supportsRoiInsetOverlayPolicy(
    const RoiInsetPresentationCapabilities& capabilities,
    RoiInsetOverlayPolicy policy) {
  switch (policy) {
    case RoiInsetOverlayPolicy::None:
      return true;
    case RoiInsetOverlayPolicy::SelectedComponent:
      return capabilities.selected_component_overlay;
    case RoiInsetOverlayPolicy::MatchCamera:
      return capabilities.match_camera_overlays;
  }
  return false;
}

inline bool supportsRoiInsetOrientation(
    const RoiInsetPresentationCapabilities& capabilities,
    RoiInsetOrientation orientation) {
  switch (orientation) {
    case RoiInsetOrientation::Acquisition:
      return true;
    case RoiInsetOrientation::HeadingNormalized:
      return capabilities.heading_normalization;
  }
  return false;
}

inline RoiInsetPresentationState resolveRoiInsetPresentation(
    RoiInsetPresentationState requested,
    const RoiInsetPresentationCapabilities& capabilities) {
  if (!std::isfinite(requested.width_px)) {
    requested.width_px = kDefaultRoiInsetWidthPx;
  }
  requested.width_px =
      std::clamp(requested.width_px, kMinimumRoiInsetWidthPx,
                 kMaximumRoiInsetWidthPx);

  if (!supportsRoiInsetOverlayPolicy(capabilities,
                                     requested.overlay_policy)) {
    requested.overlay_policy =
        capabilities.selected_component_overlay
            ? RoiInsetOverlayPolicy::SelectedComponent
            : RoiInsetOverlayPolicy::None;
  }
  if (!supportsRoiInsetOrientation(capabilities, requested.orientation)) {
    requested.orientation = RoiInsetOrientation::Acquisition;
  }
  return requested;
}

}  // namespace crimson::crop
