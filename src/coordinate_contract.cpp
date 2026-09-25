#include "coordinate_contract.h"

#include <cmath>

namespace crimson::coordinates {
namespace {

bool isFinite(double value) { return std::isfinite(value); }

bool additionFits(int64_t origin, int64_t extent, int64_t limit) {
  return origin >= 0 && extent > 0 && limit > 0 && origin <= limit &&
         extent <= limit - origin;
}

} // namespace

bool CoordinateSpace::valid() const {
  if (axes != CoordinateAxes::TopLeftXRightYDown) {
    return false;
  }
  if (domain != CoordinateDomain::SourceCamera &&
      domain != CoordinateDomain::Roi) {
    return false;
  }
  if (numeric_kind != CoordinateNumericKind::Continuous &&
      numeric_kind != CoordinateNumericKind::IntegerIndex) {
    return false;
  }
  if (units == CoordinateUnits::Normalized) {
    return normalization == CoordinateNormalization::WidthHeight &&
           numeric_kind == CoordinateNumericKind::Continuous;
  }
  if (units == CoordinateUnits::Pixels) {
    return normalization == CoordinateNormalization::NotApplicable;
  }
  return false;
}

bool operator==(const CoordinateSpace &left, const CoordinateSpace &right) {
  return left.domain == right.domain && left.units == right.units &&
         left.axes == right.axes && left.normalization == right.normalization &&
         left.numeric_kind == right.numeric_kind;
}

bool operator!=(const CoordinateSpace &left, const CoordinateSpace &right) {
  return !(left == right);
}

std::string_view coordinateSpaceName(const CoordinateSpace &space) {
  if (!space.valid()) {
    return "invalid";
  }
  if (space.domain == CoordinateDomain::SourceCamera) {
    if (space.units == CoordinateUnits::Normalized) {
      return "source_camera_normalized";
    }
    return space.numeric_kind == CoordinateNumericKind::IntegerIndex
               ? "source_camera_integer_pixel_indices"
               : "source_camera_continuous_pixels";
  }
  if (space.units == CoordinateUnits::Normalized) {
    return "roi_normalized";
  }
  return space.numeric_kind == CoordinateNumericKind::IntegerIndex
             ? "roi_integer_pixel_indices"
             : "roi_continuous_pixels";
}

bool CoordinateContract::valid() const {
  if (!space.valid()) {
    return false;
  }
  switch (geometry) {
  case GeometryKind::IntegerExtractionWindow:
    return space.units == CoordinateUnits::Pixels &&
           space.numeric_kind == CoordinateNumericKind::IntegerIndex;
  case GeometryKind::Point:
  case GeometryKind::CenterSizeBox:
  case GeometryKind::HalfOpenXyxyBox:
    return space.numeric_kind == CoordinateNumericKind::Continuous;
  }
  return false;
}

bool PixelDimensions::valid() const { return width > 0 && height > 0; }

bool ContinuousPoint::finite() const { return isFinite(x) && isFinite(y); }

bool CenterSizeBox::valid() const {
  return isFinite(center_x) && isFinite(center_y) && isFinite(width) &&
         isFinite(height) && width > 0.0 && height > 0.0;
}

bool HalfOpenXyxyBox::valid() const {
  return isFinite(x_min) && isFinite(y_min) && isFinite(x_max) &&
         isFinite(y_max) && x_max > x_min && y_max > y_min;
}

double HalfOpenXyxyBox::width() const { return x_max - x_min; }

double HalfOpenXyxyBox::height() const { return y_max - y_min; }

bool IntegerExtractionWindow::valid() const {
  return x >= 0 && y >= 0 && width > 0 && height > 0;
}

bool IntegerExtractionWindow::validWithin(
    PixelDimensions source_dimensions) const {
  return valid() && source_dimensions.valid() &&
         additionFits(x, width, source_dimensions.width) &&
         additionFits(y, height, source_dimensions.height);
}

bool TransformAuthority::validForSourceCamera() const {
  return source_dimensions.valid();
}

bool TransformAuthority::validForRoiPlacement() const {
  return validForSourceCamera() && !crop_manifest_digest.empty() &&
         !crop_policy_digest.empty();
}

bool sameTransformAuthority(const TransformAuthority &left,
                            const TransformAuthority &right) {
  return left.source_dimensions.width == right.source_dimensions.width &&
         left.source_dimensions.height == right.source_dimensions.height &&
         left.crop_manifest_digest == right.crop_manifest_digest &&
         left.crop_policy_digest == right.crop_policy_digest;
}

bool RoiPlacement::valid() const {
  return authority.validForRoiPlacement() &&
         source_window.validWithin(authority.source_dimensions);
}

std::optional<ContinuousPoint>
normalizedPointToContinuousPixels(ContinuousPoint normalized,
                                  const TransformAuthority &authority) {
  if (!normalized.finite() || !authority.validForSourceCamera()) {
    return std::nullopt;
  }
  return ContinuousPoint{
      normalized.x * static_cast<double>(authority.source_dimensions.width),
      normalized.y * static_cast<double>(authority.source_dimensions.height),
  };
}

std::optional<ContinuousPoint>
continuousPixelPointToNormalized(ContinuousPoint pixels,
                                 const TransformAuthority &authority) {
  if (!pixels.finite() || !authority.validForSourceCamera()) {
    return std::nullopt;
  }
  return ContinuousPoint{
      pixels.x / static_cast<double>(authority.source_dimensions.width),
      pixels.y / static_cast<double>(authority.source_dimensions.height),
  };
}

std::optional<HalfOpenXyxyBox> normalizedCenterSizeBoxToContinuousPixelXyxy(
    CenterSizeBox normalized, const TransformAuthority &authority) {
  if (!normalized.valid() || !authority.validForSourceCamera()) {
    return std::nullopt;
  }
  const double source_width =
      static_cast<double>(authority.source_dimensions.width);
  const double source_height =
      static_cast<double>(authority.source_dimensions.height);
  const double center_x = normalized.center_x * source_width;
  const double center_y = normalized.center_y * source_height;
  const double width = normalized.width * source_width;
  const double height = normalized.height * source_height;
  HalfOpenXyxyBox result{
      center_x - width * 0.5,
      center_y - height * 0.5,
      center_x + width * 0.5,
      center_y + height * 0.5,
  };
  return result.valid() ? std::optional<HalfOpenXyxyBox>(result) : std::nullopt;
}

std::optional<CenterSizeBox>
continuousPixelXyxyToNormalizedCenterSize(HalfOpenXyxyBox pixels,
                                          const TransformAuthority &authority) {
  if (!pixels.valid() || !authority.validForSourceCamera()) {
    return std::nullopt;
  }
  const double source_width =
      static_cast<double>(authority.source_dimensions.width);
  const double source_height =
      static_cast<double>(authority.source_dimensions.height);
  CenterSizeBox result{
      (pixels.x_min + pixels.x_max) * 0.5 / source_width,
      (pixels.y_min + pixels.y_max) * 0.5 / source_height,
      pixels.width() / source_width,
      pixels.height() / source_height,
  };
  return result.valid() ? std::optional<CenterSizeBox>(result) : std::nullopt;
}

std::optional<ContinuousPoint>
roiPixelPointToSourceCamera(ContinuousPoint roi_pixels,
                            const RoiPlacement &placement) {
  if (!roi_pixels.finite() || !placement.valid()) {
    return std::nullopt;
  }
  return ContinuousPoint{
      static_cast<double>(placement.source_window.x) + roi_pixels.x,
      static_cast<double>(placement.source_window.y) + roi_pixels.y,
  };
}

std::optional<HalfOpenXyxyBox>
roiPixelXyxyBoxToSourceCamera(HalfOpenXyxyBox roi_pixels,
                              const RoiPlacement &placement) {
  if (!roi_pixels.valid() || !placement.valid()) {
    return std::nullopt;
  }
  HalfOpenXyxyBox result{
      static_cast<double>(placement.source_window.x) + roi_pixels.x_min,
      static_cast<double>(placement.source_window.y) + roi_pixels.y_min,
      static_cast<double>(placement.source_window.x) + roi_pixels.x_max,
      static_cast<double>(placement.source_window.y) + roi_pixels.y_max,
  };
  return result.valid() ? std::optional<HalfOpenXyxyBox>(result) : std::nullopt;
}

std::optional<ContinuousPoint>
roiNormalizedPointToSourceCamera(ContinuousPoint roi_normalized,
                                 const RoiPlacement &placement) {
  if (!roi_normalized.finite() || !placement.valid()) {
    return std::nullopt;
  }
  return ContinuousPoint{
      static_cast<double>(placement.source_window.x) +
          roi_normalized.x * static_cast<double>(placement.source_window.width),
      static_cast<double>(placement.source_window.y) +
          roi_normalized.y *
              static_cast<double>(placement.source_window.height),
  };
}

std::optional<ContinuousPoint>
sourceCameraPointToRoiPixels(ContinuousPoint source_pixels,
                             const RoiPlacement &placement) {
  if (!source_pixels.finite() || !placement.valid()) {
    return std::nullopt;
  }
  return ContinuousPoint{
      source_pixels.x - static_cast<double>(placement.source_window.x),
      source_pixels.y - static_cast<double>(placement.source_window.y),
  };
}

std::optional<ContinuousPoint>
sourceCameraPointToRoiNormalized(ContinuousPoint source_pixels,
                                 const RoiPlacement &placement) {
  const auto roi_pixels =
      sourceCameraPointToRoiPixels(source_pixels, placement);
  if (!roi_pixels) {
    return std::nullopt;
  }
  return ContinuousPoint{
      roi_pixels->x / static_cast<double>(placement.source_window.width),
      roi_pixels->y / static_cast<double>(placement.source_window.height),
  };
}

} // namespace crimson::coordinates
