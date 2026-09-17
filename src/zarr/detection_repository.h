#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace crimson::zarr {

enum class DetectionDataset : uint8_t {
  RawDetect = 0,
  RefinedFiltered = 1,
  RefinedInterpolated = 2,
  RefinedManual = 3,
  RefinedRoot = 4,
};

struct DetectionDatasetOption {
  DetectionDataset dataset = DetectionDataset::RawDetect;
  std::string label;
};

struct DetectionRepositoryDescriptor {
  std::string archive_path;
  std::string run_name;
  std::string interpolation_method;
  size_t total_frames = 0;
  size_t maximum_observations_per_frame = 0;
  double frames_per_second = 0.0;
  int source_width = 0;
  int source_height = 0;
  DetectionDataset active_dataset = DetectionDataset::RawDetect;
  bool available = false;
  bool has_scores = false;
  bool has_class_ids = false;
  bool coordinates_normalized = false;
  bool interpolation_available = false;
  bool clipped_collection = false;
  bool active_dataset_has_synthetic_observations = false;

  bool activeDatasetAllowsBboxEditing() const {
    return available && active_dataset != DetectionDataset::RawDetect &&
           !clipped_collection;
  }
};

struct DetectionObservation {
  size_t ordinal = 0;
  // Stable row identity within descriptor.archive_path + descriptor.run_name.
  // Legacy repositories leave this unset; it is not a global instance key.
  int64_t canonical_row_index = -1;
  std::array<float, 4> box_xyxy{};
  float score = 1.0f;
  int32_t class_id = 0;
  uint8_t source_kind = 0;
  std::string reason;
  bool score_valid = false;
  bool class_id_valid = false;
};

enum class DetectionFrameStatus : uint8_t {
  Ready,
  Unavailable,
  OutOfRange,
};

struct DetectionFrame {
  DetectionFrameStatus status = DetectionFrameStatus::Unavailable;
  size_t frame_id = 0;
  bool interpolated = false;
  std::vector<DetectionObservation> observations;

  bool ready() const { return status == DetectionFrameStatus::Ready; }
};

class DetectionRepository {
public:
  virtual ~DetectionRepository() = default;

  virtual DetectionRepositoryDescriptor descriptor() const = 0;
  virtual std::vector<DetectionDatasetOption> availableDatasets() const = 0;
  virtual bool selectDataset(DetectionDataset dataset) = 0;
  virtual bool isDatasetAvailable(DetectionDataset dataset) const = 0;
  virtual size_t observationCount(size_t frame_id) const = 0;
  virtual bool isFrameInterpolated(size_t frame_id) const = 0;
  virtual DetectionFrame resolveFrame(size_t frame_id,
                                      bool use_interpolated = false) const = 0;
};

} // namespace crimson::zarr
