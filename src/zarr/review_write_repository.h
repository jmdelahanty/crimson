#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace crimson::zarr {

struct KeypointRoiMetadata {
  bool valid = false;
  bool has_crop_metadata = false;
  int32_t roi_index = -1;
  float offset_x = std::numeric_limits<float>::quiet_NaN();
  float offset_y = std::numeric_limits<float>::quiet_NaN();
  float roi_width = 0.0f;
  float roi_height = 0.0f;
};

struct RefinedKeypointSelection {
  bool valid = false;
  bool editable = false;
  size_t frame_id = 0;
  size_t detection_index = 0;
  int32_t roi_index = -1;
  KeypointRoiMetadata roi_metadata;
  std::string run_name;
  std::string message;
};

struct RefinedKeypointCacheUpdate {
  bool valid = false;
  size_t frame_id = 0;
  size_t detection_index = 0;
  int32_t roi_index = -1;
  std::vector<std::array<float, 2>> keypoints_img;
  float heading_deg = std::numeric_limits<float>::quiet_NaN();
  std::array<float, 2> heading_origin_img = {
      std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<float>::quiet_NaN()};
  bool heading_valid = false;
  int32_t quality_label = -1;
  std::string reason;
  uint8_t flip_corrected = 0;
  uint8_t usable = 0;
  uint8_t confidence_valid = 0;
  uint8_t geometry_valid = 0;
  uint8_t refined_success = 0;
};

struct RefinedKeypointEditResult {
  bool changed = false;
  bool summary_updated = false;
  int stale_eye_mask_runs = 0;
  RefinedKeypointCacheUpdate cache_update;
};

enum class ReviewWriteOperationKind : uint8_t {
  ManualKeypointCorrection,
  FishPresentNoKeypoints,
  DetectionIssue,
};

struct ReviewWriteOperation {
  ReviewWriteOperationKind kind =
      ReviewWriteOperationKind::ManualKeypointCorrection;
  RefinedKeypointSelection selection;
  std::vector<std::array<double, 2>> keypoints_roi;
};

struct ReviewWriteResult {
  bool ok = false;
  RefinedKeypointEditResult edit_result;
  std::string error;
};

class ReviewWriteRepository {
public:
  virtual ~ReviewWriteRepository() = default;
  virtual ReviewWriteResult write(const ReviewWriteOperation &operation) = 0;
};

using ReviewWriteRepositoryFactory =
    std::function<std::unique_ptr<ReviewWriteRepository>(
        const std::string &archive_path, std::string &error)>;

ReviewWriteResult
ExecuteReviewWrite(const ReviewWriteRepositoryFactory &repository_factory,
                   const std::string &archive_path,
                   const ReviewWriteOperation &operation);

} // namespace crimson::zarr

// Preserve the established UI-facing names while their definitions live in a
// backend-neutral repository contract.
using RefinedKeypointSelection = crimson::zarr::RefinedKeypointSelection;
using RefinedKeypointCacheUpdate = crimson::zarr::RefinedKeypointCacheUpdate;
using RefinedKeypointEditResult = crimson::zarr::RefinedKeypointEditResult;
