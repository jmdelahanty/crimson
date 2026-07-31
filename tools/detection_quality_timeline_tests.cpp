#include "detection_quality_timeline.h"
#include "detection_quality_timeline_buffer.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

namespace {

int failures = 0;

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!(expression)) {                                                       \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " << #expression  \
                << '\n';                                                       \
      ++failures;                                                              \
    }                                                                          \
  } while (false)

crimson::timeline::DetectionQualityTimelineDescriptor makeDescriptor() {
  crimson::timeline::DetectionQualityTimelineDescriptor result;
  result.surface_kind = crimson::zarr::DetectionSurfaceKind::RefinedSnapshotV1;
  result.source_group = "refined_detect_runs";
  result.run_name = "test_refined";
  result.run_manifest_digest = std::string(64, 'a');
  result.frame_count = 4;
  result.source_row_count = 6;
  result.instance_row_count = 4;
  result.offset_read_calls = 2;
  result.source_audit = true;
  result.source_reason_codes = {
      {0, "none"}, {1, "filtered_blip"}, {2, "filtered_jump"}};
  return result;
}

crimson::timeline::DetectionQualityColumnData columns() {
  crimson::timeline::DetectionQualityColumnData result;
  result.source_frame_row_offsets = {0, 2, 2, 3, 6};
  result.source_scores = {0.9f, 0.2f, 0.8f, 0.7f, 0.6f, 0.5f};
  result.source_decision_codes = {0, 1, 0, 0, 2, 1};
  result.source_reason_codes = {0, 1, 0, 0, 0, 2};
  result.instance_frame_row_offsets = {0, 1, 1, 2, 4};
  result.instance_source_kind_codes = {1, 1, 1, 3};
  return result;
}

void testMultiObservationAndEmptyFrames() {
  std::string error;
  auto repository = crimson::timeline::MakeDetectionQualityTimelineRepository(
      makeDescriptor(), columns(), &error);
  CHECK(repository != nullptr);
  CHECK(error.empty());
  const auto window = repository->resolveWindow(0, 3);
  CHECK(window.ready());
  CHECK(window.frames.size() == 4);
  CHECK(window.frames[0].source_count == 2);
  CHECK(window.frames[0].accepted_count == 1);
  CHECK(window.frames[0].filtered_count == 1);
  CHECK(std::abs(window.frames[0].score_min - 0.2) < 1e-5);
  CHECK(std::abs(window.frames[0].score_median - 0.55) < 1e-5);
  CHECK(std::abs(window.frames[0].score_max - 0.9) < 1e-5);
  CHECK(window.frames[0].reason_counts[1] == 1);
  CHECK(window.frames[1].source_count == 0);
  CHECK(std::isnan(window.frames[1].score_median));
  CHECK(window.frames[2].accepted_count == 1);
  CHECK(window.frames[3].source_count == 3);
  CHECK(window.frames[3].accepted_count == 1);
  CHECK(window.frames[3].filtered_count == 1);
  CHECK(window.frames[3].duplicate_count == 1);
  CHECK(window.frames[3].manual_count == 1);
  CHECK(window.frames[3].reason_counts[2] == 1);
}

void testRangeAndSemanticFailures() {
  auto data = columns();
  std::string error;
  auto repository = crimson::timeline::MakeDetectionQualityTimelineRepository(
      makeDescriptor(), data, &error);
  CHECK(repository != nullptr);
  CHECK(repository->resolveWindow(-1, 1).status ==
        crimson::timeline::DetectionQualityTimelineStatus::OutOfRange);

  data.source_reason_codes[1] = 99;
  repository = crimson::timeline::MakeDetectionQualityTimelineRepository(
      makeDescriptor(), std::move(data), &error);
  CHECK(repository != nullptr);
  const auto invalid = repository->resolveWindow(0, 1);
  CHECK(invalid.status ==
        crimson::timeline::DetectionQualityTimelineStatus::ReadFailed);
  CHECK(!invalid.error.empty());
}

void testMalformedOffsetsFailOpen() {
  auto data = columns();
  data.source_frame_row_offsets = {0, 2, 1, 3, 6};
  std::string error;
  auto repository = crimson::timeline::MakeDetectionQualityTimelineRepository(
      makeDescriptor(), std::move(data), &error);
  CHECK(repository == nullptr);
  CHECK(!error.empty());
}

class BlockingRepository final
    : public crimson::timeline::DetectionQualityTimelineRepository {
public:
  BlockingRepository() : descriptor_(makeDescriptor()) {
    descriptor_.frame_count = 12;
  }

  const crimson::timeline::DetectionQualityTimelineDescriptor &
  descriptor() const override {
    return descriptor_;
  }

  crimson::timeline::DetectionQualityTimelineWindow
  resolveWindow(int64_t first_camera_frame,
                int64_t last_camera_frame) const override {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (calls_++ == 0) {
        first_started_ = true;
        condition_.notify_all();
        condition_.wait(lock, [&] { return release_first_; });
      }
    }
    crimson::timeline::DetectionQualityTimelineWindow result;
    result.status = crimson::timeline::DetectionQualityTimelineStatus::Ready;
    result.first_camera_frame = first_camera_frame;
    result.last_camera_frame = last_camera_frame;
    for (int64_t frame = first_camera_frame; frame <= last_camera_frame;
         ++frame) {
      crimson::timeline::DetectionQualityFrame value;
      value.camera_frame = frame;
      result.frames.push_back(std::move(value));
    }
    return result;
  }

  bool waitForFirstRequest() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(2),
                               [&] { return first_started_; });
  }

  void releaseFirstRequest() {
    std::lock_guard<std::mutex> lock(mutex_);
    release_first_ = true;
    condition_.notify_all();
  }

private:
  crimson::timeline::DetectionQualityTimelineDescriptor descriptor_;
  mutable std::mutex mutex_;
  mutable std::condition_variable condition_;
  mutable size_t calls_ = 0;
  mutable bool first_started_ = false;
  mutable bool release_first_ = false;
};

void testBufferDiscardsSupersededPage() {
  auto scheduler = std::make_shared<crimson::data::DataAccessScheduler>(16, 1);
  DetectionQualityTimelineBuffer buffer(scheduler, "test-archive");
  auto repository = std::make_unique<BlockingRepository>();
  auto *blocking = repository.get();
  std::string error;
  if (!buffer.open(std::move(repository), 3, 2, 2, &error)) {
    CHECK(false);
    return;
  }
  if (!buffer.requestFrame(0, false, &error) ||
      !blocking->waitForFirstRequest()) {
    CHECK(false);
    return;
  }
  CHECK(buffer.requestFrame(6, true, &error));
  blocking->releaseFirstRequest();
  scheduler->waitUntilIdle();
  CHECK(buffer.window(0) == nullptr);
  const auto current = buffer.window(6);
  CHECK(current != nullptr);
  CHECK(current->ready());
  CHECK(current->first_camera_frame <= 6);
  CHECK(current->last_camera_frame >= 6);
  CHECK(buffer.metrics().discarded_results == 1);
  CHECK(buffer.requestFrame(6, false, &error));
  CHECK(buffer.metrics().cache_hits == 1);
}

} // namespace

int main() {
  testMultiObservationAndEmptyFrames();
  testRangeAndSemanticFailures();
  testMalformedOffsetsFailOpen();
  testBufferDiscardsSupersededPage();
  if (failures != 0) {
    std::cerr << failures << " detection-quality checks failed\n";
    return 1;
  }
  std::cout << "Detection-quality timeline tests passed\n";
  return 0;
}
