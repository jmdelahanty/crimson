#include "roi_inset_presentation.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition    \
                << '\n';                                                       \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool testDefaults() {
  const crimson::crop::RoiInsetPresentationState state;
  CHECK(state.visible);
  CHECK(state.width_px == crimson::crop::kDefaultRoiInsetWidthPx);
  CHECK(state.show_label);
  CHECK(state.overlay_policy ==
        crimson::crop::RoiInsetOverlayPolicy::MatchCamera);
  CHECK(state.orientation ==
        crimson::crop::RoiInsetOrientation::Acquisition);
  return true;
}

bool testCapabilityResolution() {
  crimson::crop::RoiInsetPresentationState requested;
  requested.width_px = std::numeric_limits<float>::quiet_NaN();
  requested.orientation =
      crimson::crop::RoiInsetOrientation::HeadingNormalized;

  const auto acquisition_only = crimson::crop::resolveRoiInsetPresentation(
      requested, crimson::crop::RoiInsetPresentationCapabilities{});
  CHECK(acquisition_only.width_px ==
        crimson::crop::kDefaultRoiInsetWidthPx);
  CHECK(acquisition_only.overlay_policy ==
        crimson::crop::RoiInsetOverlayPolicy::None);
  CHECK(acquisition_only.orientation ==
        crimson::crop::RoiInsetOrientation::Acquisition);

  crimson::crop::RoiInsetPresentationCapabilities selected_only;
  selected_only.selected_component_overlay = true;
  const auto selected =
      crimson::crop::resolveRoiInsetPresentation(requested, selected_only);
  CHECK(selected.overlay_policy ==
        crimson::crop::RoiInsetOverlayPolicy::SelectedComponent);

  crimson::crop::RoiInsetPresentationCapabilities complete;
  complete.selected_component_overlay = true;
  complete.match_camera_overlays = true;
  complete.heading_normalization = true;
  const auto supported =
      crimson::crop::resolveRoiInsetPresentation(requested, complete);
  CHECK(supported.overlay_policy ==
        crimson::crop::RoiInsetOverlayPolicy::MatchCamera);
  CHECK(supported.orientation ==
        crimson::crop::RoiInsetOrientation::HeadingNormalized);
  return true;
}

bool testWidthBounds() {
  crimson::crop::RoiInsetPresentationState requested;
  requested.width_px = 1.0f;
  auto resolved = crimson::crop::resolveRoiInsetPresentation(
      requested, crimson::crop::RoiInsetPresentationCapabilities{});
  CHECK(resolved.width_px == crimson::crop::kMinimumRoiInsetWidthPx);

  requested.width_px = 1000.0f;
  resolved = crimson::crop::resolveRoiInsetPresentation(
      requested, crimson::crop::RoiInsetPresentationCapabilities{});
  CHECK(resolved.width_px == crimson::crop::kMaximumRoiInsetWidthPx);
  return true;
}

}  // namespace

int main() {
  if (!testDefaults() || !testCapabilityResolution() || !testWidthBounds()) {
    return EXIT_FAILURE;
  }
  std::cout << "roi_inset_presentation_tests: PASS\n";
  return EXIT_SUCCESS;
}
