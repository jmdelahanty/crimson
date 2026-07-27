#include "chaser_distance_polar.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace crimson::polar {
namespace {

std::string_view trimAsciiWhitespace(std::string_view value) {
  while (!value.empty() &&
         (value.front() == ' ' || value.front() == '\t' ||
          value.front() == '\n' || value.front() == '\r')) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' ||
          value.back() == '\n' || value.back() == '\r')) {
    value.remove_suffix(1);
  }
  return value;
}

bool finite(double value) {
  return std::isfinite(value);
}

bool unitColorChannel(double value) {
  return finite(value) && value >= 0.0 && value <= 1.0;
}

ChaserDistancePolarFrameSample sampleFromDescriptor(
    const ChaserDistancePolarDescriptor& descriptor,
    int64_t requested_camera_frame) {
  ChaserDistancePolarFrameSample sample;
  sample.availability = descriptor.availability;
  sample.requested_camera_frame = requested_camera_frame;
  sample.provenance = descriptor.provenance;
  sample.distance_unit = descriptor.distance_unit;
  sample.coordinate_frame = descriptor.coordinate_frame;
  sample.angle_convention = descriptor.angle_convention;
  sample.normalized_coordinate_frame =
      descriptor.normalized_coordinate_frame;
  sample.normalized_angle_convention =
      descriptor.normalized_angle_convention;
  sample.radial_scale = descriptor.radial_scale;
  sample.error = descriptor.error;
  return sample;
}

}  // namespace

bool ChaserDistancePolarDatasetProvenance::complete() const {
  return source_group == kChaserDistancePolarSourceGroup &&
         !run_name.empty() && !component_name.empty() &&
         run_selection != ChaserDistancePolarSelectionProvenance::Unspecified &&
         component_selection !=
             ChaserDistancePolarSelectionProvenance::Unspecified;
}

bool ChaserDistancePolarRgba::valid() const {
  return unitColorChannel(red) && unitColorChannel(green) &&
         unitColorChannel(blue) && unitColorChannel(alpha);
}

bool ChaserDistancePolarRadialScale::valid() const {
  return finite(data_max_distance_mm) && data_max_distance_mm > 0.0 &&
         finite(display_max_distance_mm) &&
         display_max_distance_mm >= data_max_distance_mm;
}

double ChaserDistancePolarRadialScale::normalizedRadius(
    double distance_mm) const {
  if (!valid() || !finite(distance_mm)) {
    return 0.0;
  }
  return std::clamp(distance_mm, 0.0, display_max_distance_mm) /
         display_max_distance_mm;
}

bool ChaserDistancePolarPoint::scientificallyUsable() const {
  return valid && chaser_index >= 0 && finite(distance_mm) &&
         distance_mm >= 0.0 && finite(bearing_degrees);
}

std::string_view chaserDistancePolarAvailabilityName(
    ChaserDistancePolarAvailability availability) {
  switch (availability) {
    case ChaserDistancePolarAvailability::DatasetUnavailable:
      return "dataset_unavailable";
    case ChaserDistancePolarAvailability::UnsupportedMetadata:
      return "unsupported_metadata";
    case ChaserDistancePolarAvailability::ExactFrameMissing:
      return "exact_frame_missing";
    case ChaserDistancePolarAvailability::ValidFrameEmpty:
      return "valid_frame_empty";
    case ChaserDistancePolarAvailability::Ready:
      return "ready";
    case ChaserDistancePolarAvailability::ReadFailed:
      return "read_failed";
  }
  return "read_failed";
}

std::string_view chaserDistancePolarSelectionProvenanceName(
    ChaserDistancePolarSelectionProvenance provenance) {
  switch (provenance) {
    case ChaserDistancePolarSelectionProvenance::Unspecified:
      return "unspecified";
    case ChaserDistancePolarSelectionProvenance::Requested:
      return "requested";
    case ChaserDistancePolarSelectionProvenance::LatestComplete:
      return "latest_complete";
    case ChaserDistancePolarSelectionProvenance::LatestCompleted:
      return "latest_completed";
    case ChaserDistancePolarSelectionProvenance::LatestSuccess:
      return "latest_success";
    case ChaserDistancePolarSelectionProvenance::Latest:
      return "latest";
    case ChaserDistancePolarSelectionProvenance::
        LexicographicCompatibilityFallback:
      return "lexicographic_compatibility_fallback";
  }
  return "unspecified";
}

std::string_view chaserDistancePolarColorProvenanceName(
    ChaserDistancePolarColorProvenance provenance) {
  switch (provenance) {
    case ChaserDistancePolarColorProvenance::Unspecified:
      return "unspecified";
    case ChaserDistancePolarColorProvenance::StimulusProtocol:
      return "stimulus_protocol";
    case ChaserDistancePolarColorProvenance::ComponentSummary:
      return "component_summary";
    case ChaserDistancePolarColorProvenance::FixedFallbackPalette:
      return "fixed_fallback_palette";
  }
  return "unspecified";
}

std::string_view chaserDistancePolarDistanceUnitName(
    ChaserDistancePolarDistanceUnit unit) {
  switch (unit) {
    case ChaserDistancePolarDistanceUnit::Unknown:
      return "unknown";
    case ChaserDistancePolarDistanceUnit::Millimeters:
      return "mm";
  }
  return "unknown";
}

ChaserDistancePolarConventionResolution resolveChaserDistancePolarConventions(
    std::string_view coordinate_frame,
    std::string_view angle_convention) {
  ChaserDistancePolarConventionResolution result;
  coordinate_frame = trimAsciiWhitespace(coordinate_frame);
  angle_convention = trimAsciiWhitespace(angle_convention);

  if (coordinate_frame.empty()) {
    result.error = "chaser-distance coordinate_frame is missing";
    return result;
  }
  if (coordinate_frame != kArenaRelativeCanvasPixelFrame) {
    result.status =
        ChaserDistancePolarConventionStatus::UnsupportedCoordinateFrame;
    result.error = "unsupported chaser-distance coordinate_frame: " +
                   std::string(coordinate_frame);
    return result;
  }
  result.coordinate_frame =
      ChaserDistancePolarCoordinateFrame::ArenaRelativeCanvasPixels;

  if (angle_convention.empty()) {
    result.status =
        ChaserDistancePolarConventionStatus::MissingAngleConvention;
    result.error = "chaser-distance angle_convention is missing";
    return result;
  }
  if (angle_convention != kPositiveAnatomicalLeftAngleConvention) {
    result.status =
        ChaserDistancePolarConventionStatus::UnsupportedAngleConvention;
    result.error = "unsupported chaser-distance angle_convention: " +
                   std::string(angle_convention);
    return result;
  }
  result.angle_convention = ChaserDistancePolarAngleConvention::
      EgocentricDegreesZeroFrontPositiveAnatomicalLeft;
  result.status = ChaserDistancePolarConventionStatus::Supported;
  return result;
}

std::optional<double> normalizeChaserDistancePolarBearingDegrees(
    double bearing_degrees,
    ChaserDistancePolarAngleConvention convention) {
  if (!finite(bearing_degrees) ||
      convention != ChaserDistancePolarAngleConvention::
                        EgocentricDegreesZeroFrontPositiveAnatomicalLeft) {
    return std::nullopt;
  }
  double normalized = std::fmod(bearing_degrees + 180.0, 360.0);
  if (normalized < 0.0) {
    normalized += 360.0;
  }
  normalized -= 180.0;
  if (normalized == 0.0) {
    normalized = 0.0;
  }
  return normalized;
}

ChaserDistancePolarRgba chaserDistancePolarFallbackColor(
    int32_t chaser_index) {
  static constexpr std::array<ChaserDistancePolarRgba, 8> kPalette = {{
      {1.00, 0.12, 0.10, 1.0},
      {0.10, 0.32, 1.00, 1.0},
      {0.10, 0.78, 0.32, 1.0},
      {1.00, 0.68, 0.10, 1.0},
      {0.70, 0.24, 0.95, 1.0},
      {0.05, 0.75, 0.85, 1.0},
      {0.95, 0.25, 0.58, 1.0},
      {0.82, 0.82, 0.18, 1.0},
  }};
  const size_t index = static_cast<size_t>(std::max<int32_t>(0, chaser_index)) %
                       kPalette.size();
  return kPalette[index];
}

ChaserDistancePolarColor resolveChaserDistancePolarColor(
    int32_t chaser_index,
    std::optional<ChaserDistancePolarRgba> stimulus_protocol_color,
    std::optional<ChaserDistancePolarRgba> component_summary_color) {
  if (stimulus_protocol_color && stimulus_protocol_color->valid()) {
    return {*stimulus_protocol_color,
            ChaserDistancePolarColorProvenance::StimulusProtocol};
  }
  if (component_summary_color && component_summary_color->valid()) {
    return {*component_summary_color,
            ChaserDistancePolarColorProvenance::ComponentSummary};
  }
  return {chaserDistancePolarFallbackColor(chaser_index),
          ChaserDistancePolarColorProvenance::FixedFallbackPalette};
}

ChaserDistancePolarRadialScale resolveChaserDistancePolarRadialScale(
    double dataset_global_max_distance_mm) {
  ChaserDistancePolarRadialScale scale;
  if (finite(dataset_global_max_distance_mm) &&
      dataset_global_max_distance_mm > 0.0) {
    scale.provenance = ChaserDistancePolarRadialScaleProvenance::
        DatasetGlobalValidMaximum;
    scale.data_max_distance_mm = dataset_global_max_distance_mm;
  }
  scale.display_max_distance_mm =
      scale.data_max_distance_mm *
      (1.0 + kChaserDistancePolarDisplayHeadroom);
  return scale;
}

ChaserDistancePolarDescriptor normalizeChaserDistancePolarDescriptor(
    ChaserDistancePolarDescriptor descriptor) {
  descriptor.radial_scale = resolveChaserDistancePolarRadialScale(
      descriptor.dataset_global_max_distance_mm);
  if (descriptor.availability ==
          ChaserDistancePolarAvailability::DatasetUnavailable ||
      descriptor.availability ==
          ChaserDistancePolarAvailability::UnsupportedMetadata ||
      descriptor.availability == ChaserDistancePolarAvailability::ReadFailed) {
    return descriptor;
  }
  if (descriptor.availability != ChaserDistancePolarAvailability::Ready) {
    descriptor.availability = ChaserDistancePolarAvailability::ReadFailed;
    descriptor.error = "invalid dataset-level polar availability";
    return descriptor;
  }
  if (!descriptor.provenance.complete()) {
    descriptor.availability =
        ChaserDistancePolarAvailability::UnsupportedMetadata;
    descriptor.error =
        "polar dataset identity or selection provenance is incomplete";
    return descriptor;
  }
  if (descriptor.row_count == 0 || descriptor.chaser_count == 0) {
    descriptor.availability =
        ChaserDistancePolarAvailability::UnsupportedMetadata;
    descriptor.error = "polar dataset dimensions must be nonzero";
    return descriptor;
  }
  if (descriptor.distance_unit !=
      ChaserDistancePolarDistanceUnit::Millimeters) {
    descriptor.availability =
        ChaserDistancePolarAvailability::UnsupportedMetadata;
    descriptor.error = "polar distance unit must be millimeters";
    return descriptor;
  }

  const auto conventions = resolveChaserDistancePolarConventions(
      descriptor.coordinate_frame, descriptor.angle_convention);
  descriptor.normalized_coordinate_frame = conventions.coordinate_frame;
  descriptor.normalized_angle_convention = conventions.angle_convention;
  if (!conventions.supported()) {
    descriptor.availability =
        ChaserDistancePolarAvailability::UnsupportedMetadata;
    descriptor.error = conventions.error;
  } else {
    descriptor.error.clear();
  }
  return descriptor;
}

ChaserDistancePolarFrameSample makeChaserDistancePolarFrameSample(
    const ChaserDistancePolarDescriptor& source_descriptor,
    int64_t requested_camera_frame,
    std::optional<int64_t> source_camera_frame,
    std::vector<ChaserDistancePolarPoint> source_points,
    std::string read_error) {
  const auto descriptor =
      normalizeChaserDistancePolarDescriptor(source_descriptor);
  auto sample = sampleFromDescriptor(descriptor, requested_camera_frame);
  sample.source_point_count = source_points.size();

  if (!descriptor.ready()) {
    return sample;
  }
  if (!read_error.empty()) {
    sample.availability = ChaserDistancePolarAvailability::ReadFailed;
    sample.error = std::move(read_error);
    return sample;
  }
  if (!source_camera_frame.has_value() || requested_camera_frame < 0) {
    if (!source_points.empty()) {
      sample.availability = ChaserDistancePolarAvailability::ReadFailed;
      sample.error =
          "polar repository returned points without an exact camera frame";
      return sample;
    }
    sample.availability =
        ChaserDistancePolarAvailability::ExactFrameMissing;
    sample.error.clear();
    return sample;
  }
  sample.source_camera_frame = source_camera_frame;
  if (*source_camera_frame != requested_camera_frame) {
    sample.availability = ChaserDistancePolarAvailability::ReadFailed;
    sample.error = "polar repository returned a non-exact camera frame";
    return sample;
  }

  sample.points.reserve(source_points.size());
  for (auto& point : source_points) {
    if (!point.scientificallyUsable()) {
      ++sample.discarded_point_count;
      continue;
    }
    const auto normalized_bearing =
        normalizeChaserDistancePolarBearingDegrees(
            point.bearing_degrees,
            descriptor.normalized_angle_convention);
    if (!normalized_bearing.has_value()) {
      ++sample.discarded_point_count;
      continue;
    }
    point.bearing_degrees = *normalized_bearing;
    if (!point.color.rgba.valid() ||
        point.color.provenance ==
            ChaserDistancePolarColorProvenance::Unspecified) {
      point.color =
          resolveChaserDistancePolarColor(point.chaser_index,
                                          std::nullopt,
                                          std::nullopt);
    }
    point.valid = true;
    sample.points.push_back(std::move(point));
  }
  sample.error.clear();
  sample.availability = sample.points.empty()
                            ? ChaserDistancePolarAvailability::ValidFrameEmpty
                            : ChaserDistancePolarAvailability::Ready;
  return sample;
}

}  // namespace crimson::polar
