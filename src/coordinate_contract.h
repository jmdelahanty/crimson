#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace crimson::coordinates {

enum class CoordinateDomain : uint8_t {
  SourceCamera,
  Roi,
};

enum class CoordinateUnits : uint8_t {
  Normalized,
  Pixels,
};

enum class CoordinateAxes : uint8_t {
  TopLeftXRightYDown,
};

enum class CoordinateNormalization : uint8_t {
  NotApplicable,
  WidthHeight,
};

enum class CoordinateNumericKind : uint8_t {
  Continuous,
  IntegerIndex,
};

enum class GeometryKind : uint8_t {
  Point,
  CenterSizeBox,
  HalfOpenXyxyBox,
  IntegerExtractionWindow,
};

struct CoordinateSpace {
  CoordinateDomain domain = CoordinateDomain::SourceCamera;
  CoordinateUnits units = CoordinateUnits::Pixels;
  CoordinateAxes axes = CoordinateAxes::TopLeftXRightYDown;
  CoordinateNormalization normalization =
      CoordinateNormalization::NotApplicable;
  CoordinateNumericKind numeric_kind = CoordinateNumericKind::Continuous;

  bool valid() const;
};

bool operator==(const CoordinateSpace &left, const CoordinateSpace &right);
bool operator!=(const CoordinateSpace &left, const CoordinateSpace &right);
std::string_view coordinateSpaceName(const CoordinateSpace &space);

inline constexpr CoordinateSpace kSourceCameraContinuousPixels{
    CoordinateDomain::SourceCamera,     CoordinateUnits::Pixels,
    CoordinateAxes::TopLeftXRightYDown, CoordinateNormalization::NotApplicable,
    CoordinateNumericKind::Continuous,
};

inline constexpr CoordinateSpace kRoiContinuousPixels{
    CoordinateDomain::Roi,
    CoordinateUnits::Pixels,
    CoordinateAxes::TopLeftXRightYDown,
    CoordinateNormalization::NotApplicable,
    CoordinateNumericKind::Continuous,
};

inline constexpr CoordinateSpace kSourceCameraNormalized{
    CoordinateDomain::SourceCamera,     CoordinateUnits::Normalized,
    CoordinateAxes::TopLeftXRightYDown, CoordinateNormalization::WidthHeight,
    CoordinateNumericKind::Continuous,
};

inline constexpr CoordinateSpace kRoiNormalized{
    CoordinateDomain::Roi,
    CoordinateUnits::Normalized,
    CoordinateAxes::TopLeftXRightYDown,
    CoordinateNormalization::WidthHeight,
    CoordinateNumericKind::Continuous,
};

inline constexpr CoordinateSpace kSourceCameraIntegerPixelIndices{
    CoordinateDomain::SourceCamera,      CoordinateUnits::Pixels,
    CoordinateAxes::TopLeftXRightYDown,  CoordinateNormalization::NotApplicable,
    CoordinateNumericKind::IntegerIndex,
};

struct CoordinateContract {
  CoordinateSpace space;
  GeometryKind geometry = GeometryKind::Point;

  bool valid() const;
};

inline constexpr CoordinateContract kSourceCameraNormalizedPoint{
    kSourceCameraNormalized, GeometryKind::Point};
inline constexpr CoordinateContract kSourceCameraNormalizedCenterSizeBox{
    kSourceCameraNormalized, GeometryKind::CenterSizeBox};
inline constexpr CoordinateContract kSourceCameraContinuousPixelPoint{
    kSourceCameraContinuousPixels, GeometryKind::Point};
inline constexpr CoordinateContract kSourceCameraContinuousPixelXyxyBox{
    kSourceCameraContinuousPixels, GeometryKind::HalfOpenXyxyBox};
inline constexpr CoordinateContract kRoiNormalizedPoint{kRoiNormalized,
                                                        GeometryKind::Point};
inline constexpr CoordinateContract kRoiContinuousPixelPoint{
    kRoiContinuousPixels, GeometryKind::Point};
inline constexpr CoordinateContract kSourceCameraIntegerExtractionWindow{
    kSourceCameraIntegerPixelIndices, GeometryKind::IntegerExtractionWindow};

struct PixelDimensions {
  int64_t width = 0;
  int64_t height = 0;

  bool valid() const;
};

struct ContinuousPoint {
  double x = 0.0;
  double y = 0.0;

  bool finite() const;
};

struct CenterSizeBox {
  double center_x = 0.0;
  double center_y = 0.0;
  double width = 0.0;
  double height = 0.0;

  bool valid() const;
};

struct HalfOpenXyxyBox {
  double x_min = 0.0;
  double y_min = 0.0;
  double x_max = 0.0;
  double y_max = 0.0;

  bool valid() const;
  double width() const;
  double height() const;
};

struct IntegerExtractionWindow {
  int64_t x = 0;
  int64_t y = 0;
  int64_t width = 0;
  int64_t height = 0;

  bool valid() const;
  bool validWithin(PixelDimensions source_dimensions) const;
};

// Digests are opaque, exact identifiers. Producers own their algorithm and
// canonical encoding; consumers must compare the complete strings.
struct TransformAuthority {
  PixelDimensions source_dimensions;
  std::string crop_manifest_digest;
  std::string crop_policy_digest;

  bool validForSourceCamera() const;
  bool validForRoiPlacement() const;
};

bool sameTransformAuthority(const TransformAuthority &left,
                            const TransformAuthority &right);

struct RoiPlacement {
  IntegerExtractionWindow source_window;
  TransformAuthority authority;

  bool valid() const;
};

std::optional<ContinuousPoint>
normalizedPointToContinuousPixels(ContinuousPoint normalized,
                                  const TransformAuthority &authority);
std::optional<ContinuousPoint>
continuousPixelPointToNormalized(ContinuousPoint pixels,
                                 const TransformAuthority &authority);

std::optional<HalfOpenXyxyBox> normalizedCenterSizeBoxToContinuousPixelXyxy(
    CenterSizeBox normalized, const TransformAuthority &authority);
std::optional<CenterSizeBox>
continuousPixelXyxyToNormalizedCenterSize(HalfOpenXyxyBox pixels,
                                          const TransformAuthority &authority);

std::optional<ContinuousPoint>
roiPixelPointToSourceCamera(ContinuousPoint roi_pixels,
                            const RoiPlacement &placement);
std::optional<ContinuousPoint>
roiNormalizedPointToSourceCamera(ContinuousPoint roi_normalized,
                                 const RoiPlacement &placement);
std::optional<ContinuousPoint>
sourceCameraPointToRoiPixels(ContinuousPoint source_pixels,
                             const RoiPlacement &placement);
std::optional<ContinuousPoint>
sourceCameraPointToRoiNormalized(ContinuousPoint source_pixels,
                                 const RoiPlacement &placement);

} // namespace crimson::coordinates
