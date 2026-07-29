#include "coordinate_contract.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": "   \
                << #condition << '\n';                                         \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool near(double actual, double expected, double tolerance = 1e-9) {
  return std::abs(actual - expected) <= tolerance;
}

crimson::coordinates::TransformAuthority sourceAuthority() {
  crimson::coordinates::TransformAuthority authority;
  authority.source_dimensions = {4512, 4512};
  return authority;
}

crimson::coordinates::RoiPlacement roiPlacement() {
  crimson::coordinates::RoiPlacement placement;
  placement.source_window = {100, 200, 512, 256};
  placement.authority = sourceAuthority();
  placement.authority.crop_manifest_digest = "sha256:crop-manifest";
  placement.authority.crop_policy_digest = "sha256:crop-policy";
  return placement;
}

bool testVocabulary() {
  using namespace crimson::coordinates;
  CHECK(kSourceCameraContinuousPixels.valid());
  CHECK(kRoiContinuousPixels.valid());
  CHECK(kSourceCameraNormalized.valid());
  CHECK(kRoiNormalized.valid());
  CHECK(kSourceCameraIntegerPixelIndices.valid());
  CHECK(coordinateSpaceName(kSourceCameraContinuousPixels) ==
        "source_camera_continuous_pixels");
  CHECK(coordinateSpaceName(kRoiContinuousPixels) == "roi_continuous_pixels");
  CHECK(coordinateSpaceName(kSourceCameraNormalized) ==
        "source_camera_normalized");
  CHECK(coordinateSpaceName(kRoiNormalized) == "roi_normalized");
  CHECK(coordinateSpaceName(kSourceCameraIntegerPixelIndices) ==
        "source_camera_integer_pixel_indices");

  CHECK(kSourceCameraNormalizedPoint.valid());
  CHECK(kSourceCameraNormalizedCenterSizeBox.valid());
  CHECK(kSourceCameraContinuousPixelPoint.valid());
  CHECK(kSourceCameraContinuousPixelXyxyBox.valid());
  CHECK(kRoiNormalizedPoint.valid());
  CHECK(kRoiContinuousPixelPoint.valid());
  CHECK(kSourceCameraIntegerExtractionWindow.valid());

  CoordinateSpace wrong_normalization = kSourceCameraNormalized;
  wrong_normalization.normalization = CoordinateNormalization::NotApplicable;
  CHECK(!wrong_normalization.valid());
  CHECK(coordinateSpaceName(wrong_normalization) == "invalid");

  CoordinateContract continuous_window{kSourceCameraContinuousPixels,
                                       GeometryKind::IntegerExtractionWindow};
  CHECK(!continuous_window.valid());
  CoordinateContract integer_point{kSourceCameraIntegerPixelIndices,
                                   GeometryKind::Point};
  CHECK(!integer_point.valid());
  CoordinateSpace unknown_domain = kSourceCameraContinuousPixels;
  unknown_domain.domain = static_cast<CoordinateDomain>(255);
  CHECK(!unknown_domain.valid());
  CoordinateContract unknown_geometry{kSourceCameraContinuousPixels,
                                      static_cast<GeometryKind>(255)};
  CHECK(!unknown_geometry.valid());
  return true;
}

bool testSourceCameraTransforms() {
  using namespace crimson::coordinates;
  const auto authority = sourceAuthority();
  CHECK(authority.validForSourceCamera());
  CHECK(!authority.validForRoiPlacement());

  const auto point = normalizedPointToContinuousPixels({0.5, 0.25}, authority);
  CHECK(point.has_value());
  CHECK(near(point->x, 2256.0));
  CHECK(near(point->y, 1128.0));
  const auto normalized = continuousPixelPointToNormalized(*point, authority);
  CHECK(normalized.has_value());
  CHECK(near(normalized->x, 0.5));
  CHECK(near(normalized->y, 0.25));

  const auto box = normalizedCenterSizeBoxToContinuousPixelXyxy(
      {0.5, 0.25, 0.2, 0.1}, authority);
  CHECK(box.has_value());
  CHECK(near(box->x_min, 1804.8));
  CHECK(near(box->y_min, 902.4));
  CHECK(near(box->x_max, 2707.2));
  CHECK(near(box->y_max, 1353.6));
  CHECK(near(box->width(), 902.4));
  CHECK(near(box->height(), 451.2));
  const auto round_trip =
      continuousPixelXyxyToNormalizedCenterSize(*box, authority);
  CHECK(round_trip.has_value());
  CHECK(near(round_trip->center_x, 0.5));
  CHECK(near(round_trip->center_y, 0.25));
  CHECK(near(round_trip->width, 0.2));
  CHECK(near(round_trip->height, 0.1));

  const auto full_frame = normalizedCenterSizeBoxToContinuousPixelXyxy(
      {0.5, 0.5, 1.0, 1.0}, authority);
  CHECK(full_frame.has_value());
  CHECK(near(full_frame->x_min, 0.0));
  CHECK(near(full_frame->y_min, 0.0));
  CHECK(near(full_frame->x_max, 4512.0));
  CHECK(near(full_frame->y_max, 4512.0));

  TransformAuthority invalid;
  CHECK(!normalizedPointToContinuousPixels({0.5, 0.5}, invalid));
  CHECK(!normalizedCenterSizeBoxToContinuousPixelXyxy({0.5, 0.5, -0.1, 0.1},
                                                      authority));
  CHECK(!normalizedPointToContinuousPixels(
      {std::numeric_limits<double>::quiet_NaN(), 0.5}, authority));
  return true;
}

bool testRoiTransformsAndAuthority() {
  using namespace crimson::coordinates;
  const auto placement = roiPlacement();
  CHECK(placement.valid());
  CHECK(placement.source_window.validWithin(
      placement.authority.source_dimensions));

  const auto source_from_pixels =
      roiPixelPointToSourceCamera({256.0, 128.0}, placement);
  CHECK(source_from_pixels.has_value());
  CHECK(near(source_from_pixels->x, 356.0));
  CHECK(near(source_from_pixels->y, 328.0));

  const auto source_from_normalized =
      roiNormalizedPointToSourceCamera({0.5, 0.5}, placement);
  CHECK(source_from_normalized.has_value());
  CHECK(near(source_from_normalized->x, 356.0));
  CHECK(near(source_from_normalized->y, 328.0));

  const auto roi_pixels =
      sourceCameraPointToRoiPixels(*source_from_normalized, placement);
  CHECK(roi_pixels.has_value());
  CHECK(near(roi_pixels->x, 256.0));
  CHECK(near(roi_pixels->y, 128.0));
  const auto roi_normalized =
      sourceCameraPointToRoiNormalized(*source_from_normalized, placement);
  CHECK(roi_normalized.has_value());
  CHECK(near(roi_normalized->x, 0.5));
  CHECK(near(roi_normalized->y, 0.5));

  const auto far_edge = roiNormalizedPointToSourceCamera({1.0, 1.0}, placement);
  CHECK(far_edge.has_value());
  CHECK(near(far_edge->x, 612.0));
  CHECK(near(far_edge->y, 456.0));

  auto missing_digest = placement;
  missing_digest.authority.crop_policy_digest.clear();
  CHECK(!missing_digest.valid());
  CHECK(!roiPixelPointToSourceCamera({0.0, 0.0}, missing_digest));

  auto outside = placement;
  outside.source_window.x = 4300;
  CHECK(!outside.valid());

  auto different = placement.authority;
  CHECK(sameTransformAuthority(placement.authority, different));
  different.crop_policy_digest = "sha256:other-policy";
  CHECK(!sameTransformAuthority(placement.authority, different));
  return true;
}

} // namespace

int main() {
  if (!testVocabulary() || !testSourceCameraTransforms() ||
      !testRoiTransformsAndAuthority()) {
    return 1;
  }
  std::cout << "coordinate_contract_tests: PASS\n";
  return 0;
}
