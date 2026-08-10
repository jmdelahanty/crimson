#include "zarr/detection_repository.h"

#include <iostream>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

class FakeDetectionRepository final
    : public crimson::zarr::DetectionRepository {
public:
  FakeDetectionRepository() {
    descriptor_.archive_path = "fixture.zarr";
    descriptor_.run_name = "refined_fixture";
    descriptor_.total_frames = 4;
    descriptor_.maximum_observations_per_frame = 3;
    descriptor_.frames_per_second = 30.0;
    descriptor_.available = true;
    descriptor_.has_scores = true;
    descriptor_.has_class_ids = true;
    descriptor_.coordinates_normalized = true;
    descriptor_.active_dataset = crimson::zarr::DetectionDataset::RefinedRoot;

    frames_.resize(4);
    for (size_t frame = 0; frame < frames_.size(); ++frame) {
      frames_[frame].status = crimson::zarr::DetectionFrameStatus::Ready;
      frames_[frame].frame_id = frame;
    }
    frames_[0].observations = {
        observation(0, {0.1f, 0.1f, 0.4f, 0.4f}, 2),
        observation(1, {0.2f, 0.2f, 0.5f, 0.5f}, 2),
    };
    frames_[2].observations = {
        observation(0, {0.3f, 0.3f, 0.6f, 0.6f}, 1),
    };
    frames_[3].observations = {
        observation(0, {0.0f, 0.0f, 0.2f, 0.2f}, 0),
        observation(1, {0.3f, 0.3f, 0.5f, 0.5f}, 0),
        observation(2, {0.6f, 0.6f, 0.8f, 0.8f}, 0),
    };
  }

  crimson::zarr::DetectionRepositoryDescriptor descriptor() const override {
    return descriptor_;
  }

  std::vector<crimson::zarr::DetectionDatasetOption>
  availableDatasets() const override {
    return {{crimson::zarr::DetectionDataset::RawDetect, "Raw"},
            {crimson::zarr::DetectionDataset::RefinedRoot, "Refined"}};
  }

  bool selectDataset(crimson::zarr::DetectionDataset dataset) override {
    if (!isDatasetAvailable(dataset)) {
      return false;
    }
    descriptor_.active_dataset = dataset;
    return true;
  }

  bool
  isDatasetAvailable(crimson::zarr::DetectionDataset dataset) const override {
    return dataset == crimson::zarr::DetectionDataset::RawDetect ||
           dataset == crimson::zarr::DetectionDataset::RefinedRoot;
  }

  size_t observationCount(size_t frame_id) const override {
    return frame_id < frames_.size() ? frames_[frame_id].observations.size()
                                     : 0;
  }

  bool isFrameInterpolated(size_t frame_id) const override {
    return frame_id == 2;
  }

  crimson::zarr::DetectionFrame resolveFrame(size_t frame_id,
                                             bool) const override {
    if (frame_id < frames_.size()) {
      return frames_[frame_id];
    }
    crimson::zarr::DetectionFrame result;
    result.status = crimson::zarr::DetectionFrameStatus::OutOfRange;
    result.frame_id = frame_id;
    return result;
  }

private:
  static crimson::zarr::DetectionObservation
  observation(size_t ordinal, std::array<float, 4> box, int32_t class_id) {
    crimson::zarr::DetectionObservation value;
    value.ordinal = ordinal;
    value.box_xyxy = box;
    value.score = 0.9f;
    value.score_valid = true;
    value.class_id = class_id;
    value.class_id_valid = true;
    return value;
  }

  crimson::zarr::DetectionRepositoryDescriptor descriptor_;
  std::vector<crimson::zarr::DetectionFrame> frames_;
};

bool testCompleteFrameRangesAndCapabilities() {
  using namespace crimson::zarr;
  FakeDetectionRepository repository;
  const auto descriptor = repository.descriptor();
  CHECK(descriptor.available);
  CHECK(descriptor.total_frames == 4);
  CHECK(descriptor.maximum_observations_per_frame == 3);
  CHECK(descriptor.activeDatasetAllowsBboxEditing());

  const auto frame_zero = repository.resolveFrame(0, false);
  CHECK(frame_zero.ready());
  CHECK(frame_zero.observations.size() == 2);
  CHECK(frame_zero.observations[0].class_id == 2);
  CHECK(frame_zero.observations[1].class_id == 2);
  CHECK(frame_zero.observations[0].box_xyxy[2] >
        frame_zero.observations[1].box_xyxy[0]);

  const auto empty = repository.resolveFrame(1, false);
  CHECK(empty.ready());
  CHECK(empty.observations.empty());
  CHECK(repository.resolveFrame(3, false).observations.size() == 3);
  CHECK(repository.resolveFrame(4, false).status ==
        DetectionFrameStatus::OutOfRange);
  return true;
}

bool testDatasetSelectionPolicy() {
  using namespace crimson::zarr;
  FakeDetectionRepository repository;
  CHECK(repository.availableDatasets().size() == 2);
  CHECK(repository.selectDataset(DetectionDataset::RawDetect));
  CHECK(!repository.descriptor().activeDatasetAllowsBboxEditing());
  CHECK(!repository.selectDataset(DetectionDataset::RefinedManual));
  CHECK(repository.descriptor().active_dataset == DetectionDataset::RawDetect);
  return true;
}

} // namespace

int main() {
  if (!testCompleteFrameRangesAndCapabilities() ||
      !testDatasetSelectionPolicy()) {
    return 1;
  }
  std::cout << "detection_repository_tests: PASS\n";
  return 0;
}
