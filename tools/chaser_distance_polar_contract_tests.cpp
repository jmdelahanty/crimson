#include "chaser_distance_polar.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace crimson::polar;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__         \
                << ": " #condition << '\n';                                 \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool near(double actual, double expected, double tolerance = 1e-9) {
  return std::abs(actual - expected) <= tolerance;
}

bool sameColor(const ChaserDistancePolarRgba& actual,
               const ChaserDistancePolarRgba& expected) {
  return near(actual.red, expected.red) &&
         near(actual.green, expected.green) &&
         near(actual.blue, expected.blue) &&
         near(actual.alpha, expected.alpha);
}

ChaserDistancePolarDescriptor readyDescriptor() {
  ChaserDistancePolarDescriptor descriptor;
  descriptor.availability = ChaserDistancePolarAvailability::Ready;
  descriptor.provenance.source_group =
      std::string(kChaserDistancePolarSourceGroup);
  descriptor.provenance.run_name =
      "goodcopbadcop_chaser_distance_v1_20260617";
  descriptor.provenance.component_name =
      "track_offline_goodcopbadcop_tk_hyst4_low2_latch_s005_id_0_smoothed";
  descriptor.provenance.run_selection =
      ChaserDistancePolarSelectionProvenance::LatestComplete;
  descriptor.provenance.component_selection =
      ChaserDistancePolarSelectionProvenance::LatestCompleted;
  descriptor.row_count = 140035;
  descriptor.chaser_count = 2;
  descriptor.distance_unit = ChaserDistancePolarDistanceUnit::Millimeters;
  descriptor.coordinate_frame = std::string(kArenaRelativeCanvasPixelFrame);
  descriptor.angle_convention =
      std::string(kPositiveAnatomicalLeftAngleConvention);
  descriptor.dataset_global_max_distance_mm = 76.72962188720703;
  return normalizeChaserDistancePolarDescriptor(std::move(descriptor));
}

ChaserDistancePolarPoint point(int32_t chaser_index,
                               double distance_mm,
                               double bearing_degrees,
                               bool valid = true) {
  ChaserDistancePolarPoint value;
  value.chaser_index = chaser_index;
  value.distance_mm = distance_mm;
  value.bearing_degrees = bearing_degrees;
  value.valid = valid;
  value.color = resolveChaserDistancePolarColor(
      chaser_index, std::nullopt, std::nullopt);
  return value;
}

class FixtureRepository final : public ChaserDistancePolarRepository {
 public:
  explicit FixtureRepository(ChaserDistancePolarDescriptor descriptor)
      : descriptor_(std::move(descriptor)) {}

  const ChaserDistancePolarDescriptor& descriptor() const override {
    return descriptor_;
  }

  ChaserDistancePolarFrameSample resolveCameraFrame(
      int64_t camera_frame) const override {
    if (camera_frame != 1024) {
      return makeChaserDistancePolarFrameSample(
          descriptor_, camera_frame, std::nullopt, {});
    }
    return makeChaserDistancePolarFrameSample(
        descriptor_, camera_frame, camera_frame,
        {point(0, 42.59596252441406, 99.85935974121094),
         point(1, 52.74302673339844, 43.18930435180664)});
  }

 private:
  ChaserDistancePolarDescriptor descriptor_;
};

bool testNamesAndProvenance() {
  CHECK(chaserDistancePolarAvailabilityName(
            ChaserDistancePolarAvailability::DatasetUnavailable) ==
        "dataset_unavailable");
  CHECK(chaserDistancePolarAvailabilityName(
            ChaserDistancePolarAvailability::UnsupportedMetadata) ==
        "unsupported_metadata");
  CHECK(chaserDistancePolarAvailabilityName(
            ChaserDistancePolarAvailability::ExactFrameMissing) ==
        "exact_frame_missing");
  CHECK(chaserDistancePolarAvailabilityName(
            ChaserDistancePolarAvailability::ValidFrameEmpty) ==
        "valid_frame_empty");
  CHECK(chaserDistancePolarAvailabilityName(
            ChaserDistancePolarAvailability::Ready) == "ready");
  CHECK(chaserDistancePolarAvailabilityName(
            ChaserDistancePolarAvailability::ReadFailed) == "read_failed");
  CHECK(chaserDistancePolarSelectionProvenanceName(
            ChaserDistancePolarSelectionProvenance::LatestComplete) ==
        "latest_complete");
  CHECK(chaserDistancePolarSelectionProvenanceName(
            ChaserDistancePolarSelectionProvenance::
                LexicographicCompatibilityFallback) ==
        "lexicographic_compatibility_fallback");
  CHECK(chaserDistancePolarColorProvenanceName(
            ChaserDistancePolarColorProvenance::StimulusProtocol) ==
        "stimulus_protocol");
  CHECK(chaserDistancePolarDistanceUnitName(
            ChaserDistancePolarDistanceUnit::Millimeters) == "mm");

  auto descriptor = readyDescriptor();
  CHECK(descriptor.ready());
  CHECK(descriptor.provenance.complete());
  CHECK(descriptor.provenance.run_name ==
        "goodcopbadcop_chaser_distance_v1_20260617");
  CHECK(descriptor.provenance.component_name ==
        "track_offline_goodcopbadcop_tk_hyst4_low2_latch_s005_id_0_smoothed");
  CHECK(descriptor.row_count == 140035);
  CHECK(descriptor.chaser_count == 2);
  CHECK(descriptor.coordinate_frame == kArenaRelativeCanvasPixelFrame);
  CHECK(descriptor.angle_convention ==
        kPositiveAnatomicalLeftAngleConvention);
  CHECK(descriptor.normalized_coordinate_frame ==
        ChaserDistancePolarCoordinateFrame::ArenaRelativeCanvasPixels);
  CHECK(descriptor.normalized_angle_convention ==
        ChaserDistancePolarAngleConvention::
            EgocentricDegreesZeroFrontPositiveAnatomicalLeft);
  CHECK(descriptor.radial_scale.provenance ==
        ChaserDistancePolarRadialScaleProvenance::
            DatasetGlobalValidMaximum);
  CHECK(near(descriptor.radial_scale.data_max_distance_mm,
             76.72962188720703));
  CHECK(near(descriptor.radial_scale.display_max_distance_mm,
             80.56610298156738));
  return true;
}

bool testConventionValidationAndBearingNormalization() {
  const auto supported = resolveChaserDistancePolarConventions(
      "  arena_relative_canvas_px\n",
      std::string(kPositiveAnatomicalLeftAngleConvention) + "\t");
  CHECK(supported.supported());
  CHECK(supported.coordinate_frame ==
        ChaserDistancePolarCoordinateFrame::ArenaRelativeCanvasPixels);
  CHECK(supported.angle_convention ==
        ChaserDistancePolarAngleConvention::
            EgocentricDegreesZeroFrontPositiveAnatomicalLeft);

  auto missing_coordinate = resolveChaserDistancePolarConventions(
      "", kPositiveAnatomicalLeftAngleConvention);
  CHECK(missing_coordinate.status ==
        ChaserDistancePolarConventionStatus::MissingCoordinateFrame);
  CHECK(!missing_coordinate.error.empty());

  auto bad_coordinate = resolveChaserDistancePolarConventions(
      "full_frame_camera_px", kPositiveAnatomicalLeftAngleConvention);
  CHECK(bad_coordinate.status ==
        ChaserDistancePolarConventionStatus::UnsupportedCoordinateFrame);

  auto missing_angle = resolveChaserDistancePolarConventions(
      kArenaRelativeCanvasPixelFrame, "");
  CHECK(missing_angle.status ==
        ChaserDistancePolarConventionStatus::MissingAngleConvention);

  auto bad_angle = resolveChaserDistancePolarConventions(
      kArenaRelativeCanvasPixelFrame,
      "0=in_front; positive=anatomical_right");
  CHECK(bad_angle.status ==
        ChaserDistancePolarConventionStatus::UnsupportedAngleConvention);

  const auto convention = ChaserDistancePolarAngleConvention::
      EgocentricDegreesZeroFrontPositiveAnatomicalLeft;
  CHECK(near(*normalizeChaserDistancePolarBearingDegrees(0.0, convention),
             0.0));
  CHECK(near(*normalizeChaserDistancePolarBearingDegrees(90.0, convention),
             90.0));
  CHECK(near(*normalizeChaserDistancePolarBearingDegrees(-90.0, convention),
             -90.0));
  CHECK(near(*normalizeChaserDistancePolarBearingDegrees(180.0, convention),
             -180.0));
  CHECK(near(*normalizeChaserDistancePolarBearingDegrees(540.0, convention),
             -180.0));
  CHECK(near(*normalizeChaserDistancePolarBearingDegrees(-540.0, convention),
             -180.0));
  CHECK(!normalizeChaserDistancePolarBearingDegrees(
             std::numeric_limits<double>::quiet_NaN(), convention)
             .has_value());
  CHECK(!normalizeChaserDistancePolarBearingDegrees(
             0.0, ChaserDistancePolarAngleConvention::Unsupported)
             .has_value());
  return true;
}

bool testColorPrecedenceAndValidation() {
  const ChaserDistancePolarRgba protocol{0.25, 0.50, 0.75, 0.40};
  const ChaserDistancePolarRgba component{0.0, 1.0, 0.0, 1.0};
  auto resolved =
      resolveChaserDistancePolarColor(0, protocol, component);
  CHECK(resolved.provenance ==
        ChaserDistancePolarColorProvenance::StimulusProtocol);
  CHECK(sameColor(resolved.rgba, protocol));

  resolved = resolveChaserDistancePolarColor(1, std::nullopt, component);
  CHECK(resolved.provenance ==
        ChaserDistancePolarColorProvenance::ComponentSummary);
  CHECK(sameColor(resolved.rgba, component));

  ChaserDistancePolarRgba invalid = protocol;
  invalid.red = std::numeric_limits<double>::quiet_NaN();
  resolved = resolveChaserDistancePolarColor(2, invalid, component);
  CHECK(resolved.provenance ==
        ChaserDistancePolarColorProvenance::ComponentSummary);
  resolved = resolveChaserDistancePolarColor(2, invalid, std::nullopt);
  CHECK(resolved.provenance ==
        ChaserDistancePolarColorProvenance::FixedFallbackPalette);
  CHECK(sameColor(resolved.rgba, {0.10, 0.78, 0.32, 1.0}));
  CHECK(sameColor(chaserDistancePolarFallbackColor(8),
                  chaserDistancePolarFallbackColor(0)));
  CHECK(sameColor(chaserDistancePolarFallbackColor(-1),
                  chaserDistancePolarFallbackColor(0)));
  return true;
}

bool testRadialScalePolicy() {
  auto scale = resolveChaserDistancePolarRadialScale(100.0);
  CHECK(scale.valid());
  CHECK(scale.provenance ==
        ChaserDistancePolarRadialScaleProvenance::
            DatasetGlobalValidMaximum);
  CHECK(near(scale.data_max_distance_mm, 100.0));
  CHECK(near(scale.display_max_distance_mm, 105.0));
  CHECK(near(scale.normalizedRadius(-5.0), 0.0));
  CHECK(near(scale.normalizedRadius(52.5), 0.5));
  CHECK(near(scale.normalizedRadius(105.0), 1.0));
  CHECK(near(scale.normalizedRadius(200.0), 1.0));
  CHECK(near(scale.normalizedRadius(
                 std::numeric_limits<double>::quiet_NaN()),
             0.0));

  for (double invalid :
       {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity()}) {
    scale = resolveChaserDistancePolarRadialScale(invalid);
    CHECK(scale.valid());
    CHECK(scale.provenance ==
          ChaserDistancePolarRadialScaleProvenance::UnitDistanceFallback);
    CHECK(near(scale.data_max_distance_mm, 1.0));
    CHECK(near(scale.display_max_distance_mm, 1.05));
  }
  return true;
}

bool testDescriptorRejection() {
  auto descriptor = readyDescriptor();
  descriptor.coordinate_frame = "camera_px";
  descriptor = normalizeChaserDistancePolarDescriptor(std::move(descriptor));
  CHECK(descriptor.availability ==
        ChaserDistancePolarAvailability::UnsupportedMetadata);
  CHECK(!descriptor.error.empty());

  descriptor = readyDescriptor();
  descriptor.angle_convention = "positive=anatomical_right";
  descriptor = normalizeChaserDistancePolarDescriptor(std::move(descriptor));
  CHECK(descriptor.availability ==
        ChaserDistancePolarAvailability::UnsupportedMetadata);

  descriptor = readyDescriptor();
  descriptor.provenance.component_name.clear();
  descriptor = normalizeChaserDistancePolarDescriptor(std::move(descriptor));
  CHECK(descriptor.availability ==
        ChaserDistancePolarAvailability::UnsupportedMetadata);

  descriptor = readyDescriptor();
  descriptor.provenance.run_selection =
      ChaserDistancePolarSelectionProvenance::Unspecified;
  descriptor = normalizeChaserDistancePolarDescriptor(std::move(descriptor));
  CHECK(descriptor.availability ==
        ChaserDistancePolarAvailability::UnsupportedMetadata);

  descriptor = readyDescriptor();
  descriptor.provenance.source_group = "analysis/not_the_polar_group";
  descriptor = normalizeChaserDistancePolarDescriptor(std::move(descriptor));
  CHECK(descriptor.availability ==
        ChaserDistancePolarAvailability::UnsupportedMetadata);

  descriptor = readyDescriptor();
  descriptor.row_count = 0;
  descriptor = normalizeChaserDistancePolarDescriptor(std::move(descriptor));
  CHECK(descriptor.availability ==
        ChaserDistancePolarAvailability::UnsupportedMetadata);

  descriptor = readyDescriptor();
  descriptor.distance_unit = ChaserDistancePolarDistanceUnit::Unknown;
  descriptor = normalizeChaserDistancePolarDescriptor(std::move(descriptor));
  CHECK(descriptor.availability ==
        ChaserDistancePolarAvailability::UnsupportedMetadata);

  descriptor = readyDescriptor();
  descriptor.availability =
      ChaserDistancePolarAvailability::ValidFrameEmpty;
  descriptor = normalizeChaserDistancePolarDescriptor(std::move(descriptor));
  CHECK(descriptor.availability ==
        ChaserDistancePolarAvailability::ReadFailed);
  return true;
}

bool testEveryAvailabilityStateAndExactFrameIdentity() {
  ChaserDistancePolarDescriptor unavailable;
  auto sample = makeChaserDistancePolarFrameSample(
      unavailable, 10, std::nullopt, {});
  CHECK(sample.availability ==
        ChaserDistancePolarAvailability::DatasetUnavailable);

  auto unsupported = readyDescriptor();
  unsupported.coordinate_frame = "unsupported";
  sample = makeChaserDistancePolarFrameSample(
      unsupported, 10, 10, {point(0, 1.0, 0.0)});
  CHECK(sample.availability ==
        ChaserDistancePolarAvailability::UnsupportedMetadata);
  CHECK(!sample.error.empty());

  auto descriptor = readyDescriptor();
  sample = makeChaserDistancePolarFrameSample(
      descriptor, 10, std::nullopt, {});
  CHECK(sample.availability ==
        ChaserDistancePolarAvailability::ExactFrameMissing);
  CHECK(!sample.source_camera_frame.has_value());
  CHECK(!sample.exactFrame());

  sample = makeChaserDistancePolarFrameSample(
      descriptor, 10, std::nullopt, {point(0, 42.5, 99.0)});
  CHECK(sample.availability == ChaserDistancePolarAvailability::ReadFailed);
  CHECK(!sample.error.empty());

  sample = makeChaserDistancePolarFrameSample(
      descriptor, 10, 10, {});
  CHECK(sample.availability ==
        ChaserDistancePolarAvailability::ValidFrameEmpty);
  CHECK(sample.exactFrame());

  sample = makeChaserDistancePolarFrameSample(
      descriptor, 10, 10, {point(0, 42.5, 99.0)});
  CHECK(sample.availability == ChaserDistancePolarAvailability::Ready);
  CHECK(sample.ready());
  CHECK(sample.exactFrame());
  CHECK(sample.requested_camera_frame == 10);
  CHECK(sample.source_camera_frame == 10);
  CHECK(sample.provenance.run_name == descriptor.provenance.run_name);
  CHECK(sample.distance_unit ==
        ChaserDistancePolarDistanceUnit::Millimeters);
  CHECK(sample.coordinate_frame == kArenaRelativeCanvasPixelFrame);
  CHECK(sample.angle_convention ==
        kPositiveAnatomicalLeftAngleConvention);

  sample = makeChaserDistancePolarFrameSample(
      descriptor, 10, 10, {point(0, 1.0, 0.0)}, "read failed fixture");
  CHECK(sample.availability == ChaserDistancePolarAvailability::ReadFailed);
  CHECK(sample.error == "read failed fixture");

  sample = makeChaserDistancePolarFrameSample(
      descriptor, 10, 11, {point(0, 1.0, 0.0)});
  CHECK(sample.availability == ChaserDistancePolarAvailability::ReadFailed);
  CHECK(!sample.exactFrame());
  CHECK(!sample.error.empty());

  sample = makeChaserDistancePolarFrameSample(
      descriptor, -1, std::nullopt, {});
  CHECK(sample.availability ==
        ChaserDistancePolarAvailability::ExactFrameMissing);
  return true;
}

bool testPointFilteringNormalizationAndColorFallback() {
  auto descriptor = readyDescriptor();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  auto missing_color = point(7, 10.0, 450.0);
  missing_color.color.provenance =
      ChaserDistancePolarColorProvenance::Unspecified;
  missing_color.color.rgba = {nan, 0.0, 0.0, 1.0};
  std::vector<ChaserDistancePolarPoint> source_points = {
      point(0, 42.5959625, 99.8593597),
      point(1, 52.7430267, 43.1893044, false),
      point(-1, 10.0, 0.0),
      point(2, -1.0, 0.0),
      point(3, nan, 0.0),
      point(4, 10.0, nan),
      missing_color,
  };
  auto sample = makeChaserDistancePolarFrameSample(
      descriptor, 1024, 1024, std::move(source_points));
  CHECK(sample.ready());
  CHECK(sample.source_point_count == 7);
  CHECK(sample.discarded_point_count == 5);
  CHECK(sample.points.size() == 2);
  CHECK(sample.points[0].valid);
  CHECK(sample.points[0].chaser_index == 0);
  CHECK(near(sample.points[0].distance_mm, 42.5959625));
  CHECK(near(sample.points[0].bearing_degrees, 99.8593597));
  CHECK(sample.points[1].chaser_index == 7);
  CHECK(near(sample.points[1].bearing_degrees, 90.0));
  CHECK(sample.points[1].color.provenance ==
        ChaserDistancePolarColorProvenance::FixedFallbackPalette);

  sample = makeChaserDistancePolarFrameSample(
      descriptor, 40, 40,
      {point(0, 10.0, 0.0, false), point(1, nan, 0.0)});
  CHECK(sample.availability ==
        ChaserDistancePolarAvailability::ValidFrameEmpty);
  CHECK(sample.source_point_count == 2);
  CHECK(sample.discarded_point_count == 2);
  CHECK(sample.points.empty());
  return true;
}

bool testRepositoryInterface() {
  std::unique_ptr<ChaserDistancePolarRepository> repository =
      std::make_unique<FixtureRepository>(readyDescriptor());
  CHECK(repository->descriptor().ready());

  const auto ready = repository->resolveCameraFrame(1024);
  CHECK(ready.ready());
  CHECK(ready.exactFrame());
  CHECK(ready.points.size() == 2);
  CHECK(ready.points[0].chaser_index == 0);
  CHECK(ready.points[1].chaser_index == 1);

  const auto missing = repository->resolveCameraFrame(56);
  CHECK(missing.availability ==
        ChaserDistancePolarAvailability::ExactFrameMissing);
  CHECK(!missing.exactFrame());
  return true;
}

}  // namespace

int main() {
  if (!testNamesAndProvenance() ||
      !testConventionValidationAndBearingNormalization() ||
      !testColorPrecedenceAndValidation() ||
      !testRadialScalePolicy() || !testDescriptorRejection() ||
      !testEveryAvailabilityStateAndExactFrameIdentity() ||
      !testPointFilteringNormalizationAndColorFallback() ||
      !testRepositoryInterface()) {
    return 1;
  }
  std::cout << "chaser_distance_polar_contract_tests: PASS\n";
  return 0;
}
