#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crimson::polar {

inline constexpr std::string_view kChaserDistancePolarSourceGroup =
    "analysis/chaser_distance_runs";
inline constexpr std::string_view kArenaRelativeCanvasPixelFrame =
    "arena_relative_canvas_px";
inline constexpr std::string_view kPositiveAnatomicalLeftAngleConvention =
    "arena_xy_y_down_positions_converted_to_math_y_up_angles; "
    "heading_degrees_ccw_from_+x; "
    "egocentric_bearing_deg=wrap(object_bearing-heading); "
    "0=in_front; positive=anatomical_left";
inline constexpr double kChaserDistancePolarDisplayHeadroom = 0.05;

enum class ChaserDistancePolarAvailability : uint8_t {
  DatasetUnavailable,
  UnsupportedMetadata,
  ExactFrameMissing,
  ValidFrameEmpty,
  Ready,
  ReadFailed,
};

enum class ChaserDistancePolarCoordinateFrame : uint8_t {
  Unsupported,
  ArenaRelativeCanvasPixels,
};

enum class ChaserDistancePolarAngleConvention : uint8_t {
  Unsupported,
  EgocentricDegreesZeroFrontPositiveAnatomicalLeft,
};

enum class ChaserDistancePolarDistanceUnit : uint8_t {
  Unknown,
  Millimeters,
};

enum class ChaserDistancePolarSelectionProvenance : uint8_t {
  Unspecified,
  Requested,
  LatestComplete,
  LatestCompleted,
  LatestSuccess,
  Latest,
  LexicographicCompatibilityFallback,
};

enum class ChaserDistancePolarColorProvenance : uint8_t {
  Unspecified,
  StimulusProtocol,
  ComponentSummary,
  FixedFallbackPalette,
};

enum class ChaserDistancePolarRadialScaleProvenance : uint8_t {
  DatasetGlobalValidMaximum,
  UnitDistanceFallback,
};

enum class ChaserDistancePolarConventionStatus : uint8_t {
  Supported,
  MissingCoordinateFrame,
  UnsupportedCoordinateFrame,
  MissingAngleConvention,
  UnsupportedAngleConvention,
};

struct ChaserDistancePolarDatasetProvenance {
  std::string source_group{std::string(kChaserDistancePolarSourceGroup)};
  std::string run_name;
  std::string component_name;
  ChaserDistancePolarSelectionProvenance run_selection =
      ChaserDistancePolarSelectionProvenance::Unspecified;
  ChaserDistancePolarSelectionProvenance component_selection =
      ChaserDistancePolarSelectionProvenance::Unspecified;

  bool complete() const;
};

struct ChaserDistancePolarConventionResolution {
  ChaserDistancePolarConventionStatus status =
      ChaserDistancePolarConventionStatus::MissingCoordinateFrame;
  ChaserDistancePolarCoordinateFrame coordinate_frame =
      ChaserDistancePolarCoordinateFrame::Unsupported;
  ChaserDistancePolarAngleConvention angle_convention =
      ChaserDistancePolarAngleConvention::Unsupported;
  std::string error;

  bool supported() const {
    return status == ChaserDistancePolarConventionStatus::Supported;
  }
};

struct ChaserDistancePolarRgba {
  double red = 1.0;
  double green = 0.0;
  double blue = 0.0;
  double alpha = 1.0;

  bool valid() const;
};

struct ChaserDistancePolarColor {
  ChaserDistancePolarRgba rgba;
  ChaserDistancePolarColorProvenance provenance =
      ChaserDistancePolarColorProvenance::Unspecified;
};

struct ChaserDistancePolarRadialScale {
  ChaserDistancePolarRadialScaleProvenance provenance =
      ChaserDistancePolarRadialScaleProvenance::UnitDistanceFallback;
  double data_max_distance_mm = 1.0;
  double display_max_distance_mm =
      1.0 + kChaserDistancePolarDisplayHeadroom;

  bool valid() const;
  double normalizedRadius(double distance_mm) const;
};

struct ChaserDistancePolarDescriptor {
  ChaserDistancePolarAvailability availability =
      ChaserDistancePolarAvailability::DatasetUnavailable;
  ChaserDistancePolarDatasetProvenance provenance;
  size_t row_count = 0;
  size_t chaser_count = 0;
  ChaserDistancePolarDistanceUnit distance_unit =
      ChaserDistancePolarDistanceUnit::Millimeters;
  std::string coordinate_frame;
  std::string angle_convention;
  ChaserDistancePolarCoordinateFrame normalized_coordinate_frame =
      ChaserDistancePolarCoordinateFrame::Unsupported;
  ChaserDistancePolarAngleConvention normalized_angle_convention =
      ChaserDistancePolarAngleConvention::Unsupported;
  double dataset_global_max_distance_mm = 0.0;
  ChaserDistancePolarRadialScale radial_scale;
  std::string error;

  bool ready() const {
    return availability == ChaserDistancePolarAvailability::Ready;
  }
};

struct ChaserDistancePolarPoint {
  int32_t chaser_index = -1;
  double distance_mm = 0.0;
  double bearing_degrees = 0.0;
  bool valid = false;
  ChaserDistancePolarColor color;

  bool scientificallyUsable() const;
};

struct ChaserDistancePolarFrameSample {
  ChaserDistancePolarAvailability availability =
      ChaserDistancePolarAvailability::DatasetUnavailable;
  int64_t requested_camera_frame = -1;
  std::optional<int64_t> source_camera_frame;
  ChaserDistancePolarDatasetProvenance provenance;
  ChaserDistancePolarDistanceUnit distance_unit =
      ChaserDistancePolarDistanceUnit::Millimeters;
  std::string coordinate_frame;
  std::string angle_convention;
  ChaserDistancePolarCoordinateFrame normalized_coordinate_frame =
      ChaserDistancePolarCoordinateFrame::Unsupported;
  ChaserDistancePolarAngleConvention normalized_angle_convention =
      ChaserDistancePolarAngleConvention::Unsupported;
  ChaserDistancePolarRadialScale radial_scale;
  size_t source_point_count = 0;
  size_t discarded_point_count = 0;
  std::vector<ChaserDistancePolarPoint> points;
  std::string error;

  bool exactFrame() const {
    return source_camera_frame.has_value() &&
           *source_camera_frame == requested_camera_frame;
  }
  bool ready() const {
    return availability == ChaserDistancePolarAvailability::Ready;
  }
};

std::string_view chaserDistancePolarAvailabilityName(
    ChaserDistancePolarAvailability availability);
std::string_view chaserDistancePolarSelectionProvenanceName(
    ChaserDistancePolarSelectionProvenance provenance);
std::string_view chaserDistancePolarColorProvenanceName(
    ChaserDistancePolarColorProvenance provenance);
std::string_view chaserDistancePolarDistanceUnitName(
    ChaserDistancePolarDistanceUnit unit);

ChaserDistancePolarConventionResolution resolveChaserDistancePolarConventions(
    std::string_view coordinate_frame,
    std::string_view angle_convention);

std::optional<double> normalizeChaserDistancePolarBearingDegrees(
    double bearing_degrees,
    ChaserDistancePolarAngleConvention convention);

ChaserDistancePolarRgba chaserDistancePolarFallbackColor(
    int32_t chaser_index);

ChaserDistancePolarColor resolveChaserDistancePolarColor(
    int32_t chaser_index,
    std::optional<ChaserDistancePolarRgba> stimulus_protocol_color,
    std::optional<ChaserDistancePolarRgba> component_summary_color);

ChaserDistancePolarRadialScale resolveChaserDistancePolarRadialScale(
    double dataset_global_max_distance_mm);

ChaserDistancePolarDescriptor normalizeChaserDistancePolarDescriptor(
    ChaserDistancePolarDescriptor descriptor);

ChaserDistancePolarFrameSample makeChaserDistancePolarFrameSample(
    const ChaserDistancePolarDescriptor& descriptor,
    int64_t requested_camera_frame,
    std::optional<int64_t> source_camera_frame,
    std::vector<ChaserDistancePolarPoint> source_points,
    std::string read_error = {});

class ChaserDistancePolarRepository {
 public:
  virtual ~ChaserDistancePolarRepository() = default;

  virtual const ChaserDistancePolarDescriptor& descriptor() const = 0;
  virtual ChaserDistancePolarFrameSample resolveCameraFrame(
      int64_t camera_frame) const = 0;
};

}  // namespace crimson::polar
