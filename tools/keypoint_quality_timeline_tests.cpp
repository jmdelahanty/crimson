#include "keypoint_quality_timeline.h"
#include "keypoint_quality_timeline_buffer.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <thread>

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!(expression)) {                                                       \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,  \
                   #expression);                                               \
      std::exit(1);                                                            \
    }                                                                          \
  } while (false)

namespace {

crimson::timeline::KeypointQualityTimelineDescriptor makeDescriptor() {
  crimson::timeline::KeypointQualityTimelineDescriptor value;
  value.refined = true;
  value.source_group = "refined_keypoints_runs";
  value.run_name = "refined";
  value.quality_run_name = "quality";
  value.frame_count = 4;
  value.row_count = 6;
  value.keypoint_count = 2;
  value.offset_read_calls = 1;
  value.retained_offset_bytes = 5 * sizeof(int64_t);
  value.keypoint_labels = {"nose", "tail"};
  value.keypoint_metrics = {{"confidence_margin", "probability", false}};
  value.pose_metrics = {{"valid_landmark_fraction", "fraction", false}};
  value.keypoint_flags = {{1, "low_confidence"}, {2, "source_invalid"}};
  value.pose_flags = {{1, "source_pose_failed"},
                      {2, "insufficient_valid_landmarks"}};
  value.review_states = {{0, "unreviewed"}, {1, "accepted"}};
  value.reason_codes = {{0, "none"}, {2, "recovered"}};
  return value;
}

crimson::timeline::KeypointQualityColumnData columns() {
  crimson::timeline::KeypointQualityColumnData value;
  value.frame_row_offsets = {0, 2, 2, 3, 6};
  value.selected_instance_keys = {10, 11, 12, 13, 14, 15};
  value.quality_instance_keys = value.selected_instance_keys;
  value.keypoint_confidences = {0.2f, 0.8f, 0.6f, 0.4f, 0.9f, 0.7f,
                                0.3f, 0.5f, 0.8f, 0.9f, 0.1f, 0.2f};
  value.keypoint_valid.assign(12, 1);
  value.pose_confidences = {0.5f, 0.7f, 0.8f, 0.9f, 0.4f, 0.6f};
  value.source_success = {1, 1, 1, 1, 0, 1};
  value.refined_success = {1, 0, 1, 1, 1, 1};
  value.keypoint_edit_flags.assign(12, 0);
  value.keypoint_edit_flags[1] = 1;
  value.flip_corrected = {0, 1, 0, 0, 0, 1};
  value.usable_keypoints = {1, 0, 1, 1, 1, 1};
  value.review_state_codes = {0, 1, 0, 0, 1, 0};
  value.reason_codes = {0, 2, 0, 0, 2, 0};
  value.keypoint_metric_values.assign(12, 0.25f);
  value.keypoint_metric_valid.assign(12, 1);
  value.pose_metric_values.assign(6, 1.0f);
  value.pose_metric_valid.assign(6, 1);
  value.keypoint_quality_flags.assign(12, 0);
  value.keypoint_quality_flags[0] = 1;
  value.pose_quality_flags = {0, 2, 0, 0, 1, 0};
  value.proposed_keypoint_valid.assign(12, 1);
  value.proposed_keypoint_valid[0] = 0;
  value.proposed_pose_usable = {1, 0, 1, 1, 0, 1};
  return value;
}

void testMultiRowAndEmptyFrames() {
  std::string error;
  auto repository = crimson::timeline::MakeKeypointQualityTimelineRepository(
      makeDescriptor(), columns(), &error);
  CHECK(repository != nullptr);
  CHECK(error.empty());
  const auto window = repository->resolveWindow(0, 3);
  CHECK(window.ready());
  CHECK(window.frames.size() == 4);
  CHECK(window.frames[0].observation_count == 2);
  CHECK(window.frames[1].observation_count == 0);
  CHECK(window.frames[3].observation_count == 3);
  CHECK(std::abs(window.frames[0].pose_confidence_median - 0.6) < 1e-6);
  CHECK(std::abs(window.frames[0].keypoint_confidence_medians[0] - 0.4) < 1e-6);
  CHECK(window.frames[0].edited_keypoint_count == 1);
  CHECK(window.frames[0].keypoint_flag_counts[0] == 1);
  CHECK(window.frames[0].pose_flag_counts[1] == 1);
  CHECK(window.frames[0].review_state_counts[1] == 1);
  CHECK(window.frames[0].reason_code_counts[1] == 1);
  CHECK(window.rows_read == 6);
  CHECK(repository->metrics().range_reads == 1);
}

void testFullRecordingConfidenceOverview() {
  std::string error;
  auto repository = crimson::timeline::MakeKeypointQualityTimelineRepository(
      makeDescriptor(), columns(), &error);
  CHECK(repository != nullptr);
  const auto overview = repository->resolveOverview(4, 1024, {});
  CHECK(overview.ready());
  CHECK(overview.frame_count == 4);
  CHECK(overview.rows_read == 6);
  CHECK(overview.decoded_bytes == 6 * 3 * (sizeof(float) + sizeof(uint8_t)));
  CHECK(overview.pose_confidence.camera_frames.size() == 4);
  CHECK(overview.pose_confidence.camera_frames[0] == 0);
  CHECK(overview.pose_confidence.camera_frames[1] == 0);
  CHECK(std::abs(overview.pose_confidence.values[0] - 0.5) < 1e-6);
  CHECK(std::abs(overview.pose_confidence.values[1] - 0.7) < 1e-6);
  CHECK(overview.keypoint_confidence.size() == 2);
  CHECK(overview.keypoint_confidence[0].values.size() == 4);
  CHECK(std::abs(overview.keypoint_confidence[0].values[0] - 0.2) < 1e-6);
  CHECK(std::abs(overview.keypoint_confidence[0].values[1] - 0.6) < 1e-6);
  const auto metrics = repository->metrics();
  CHECK(metrics.overview_reads == 1);
  CHECK(metrics.overview_rows_read == 6);
  size_t cancellation_checks = 0;
  const auto cancelled = repository->resolveOverview(
      4, 1024, [&] { return cancellation_checks++ >= 2; });
  CHECK(!cancelled.ready());
  CHECK(cancelled.error == "Keypoint-quality overview was cancelled");
}

void testMaskedMissingConfidences() {
  std::string error;
  auto masked = columns();
  masked.source_success[0] = 0;
  masked.pose_confidences[0] = std::numeric_limits<float>::quiet_NaN();
  masked.keypoint_valid[0] = 0;
  masked.keypoint_confidences[0] = std::numeric_limits<float>::quiet_NaN();
  auto repository = crimson::timeline::MakeKeypointQualityTimelineRepository(
      makeDescriptor(), std::move(masked), &error);
  CHECK(repository != nullptr);
  const auto window = repository->resolveWindow(0, 0);
  CHECK(window.ready());
  CHECK(std::abs(window.frames[0].pose_confidence_median - 0.7) < 1e-6);
  CHECK(std::abs(window.frames[0].keypoint_confidence_medians[0] - 0.6) < 1e-6);
  CHECK(repository->resolveOverview(4, 1024, {}).ready());

  auto invalid = columns();
  invalid.pose_confidences[0] = std::numeric_limits<float>::quiet_NaN();
  repository = crimson::timeline::MakeKeypointQualityTimelineRepository(
      makeDescriptor(), std::move(invalid), &error);
  CHECK(repository->resolveWindow(0, 0).status ==
        crimson::timeline::KeypointQualityTimelineStatus::ReadFailed);
  CHECK(!repository->resolveOverview(4, 1024, {}).ready());

  invalid = columns();
  invalid.source_success[0] = 0;
  invalid.pose_confidences[0] = std::numeric_limits<float>::infinity();
  repository = crimson::timeline::MakeKeypointQualityTimelineRepository(
      makeDescriptor(), std::move(invalid), &error);
  CHECK(repository->resolveWindow(0, 0).status ==
        crimson::timeline::KeypointQualityTimelineStatus::ReadFailed);
  CHECK(!repository->resolveOverview(4, 1024, {}).ready());
}

void testContractFailures() {
  std::string error;
  auto broken = columns();
  broken.quality_instance_keys[2] = 99;
  auto repository = crimson::timeline::MakeKeypointQualityTimelineRepository(
      makeDescriptor(), std::move(broken), &error);
  CHECK(repository != nullptr);
  CHECK(repository->resolveWindow(0, 3).status ==
        crimson::timeline::KeypointQualityTimelineStatus::ReadFailed);

  broken = columns();
  broken.frame_row_offsets[2] = 1;
  repository = crimson::timeline::MakeKeypointQualityTimelineRepository(
      makeDescriptor(), std::move(broken), &error);
  CHECK(repository == nullptr);

  broken = columns();
  broken.keypoint_quality_flags[0] = 4;
  repository = crimson::timeline::MakeKeypointQualityTimelineRepository(
      makeDescriptor(), std::move(broken), &error);
  CHECK(repository->resolveWindow(0, 0).status ==
        crimson::timeline::KeypointQualityTimelineStatus::ReadFailed);

  broken = columns();
  broken.pose_confidences[0] = 2.0f;
  repository = crimson::timeline::MakeKeypointQualityTimelineRepository(
      makeDescriptor(), std::move(broken), &error);
  CHECK(repository->resolveWindow(0, 0).status ==
        crimson::timeline::KeypointQualityTimelineStatus::ReadFailed);

  broken = columns();
  broken.review_state_codes[0] = 99;
  repository = crimson::timeline::MakeKeypointQualityTimelineRepository(
      makeDescriptor(), std::move(broken), &error);
  CHECK(repository->resolveWindow(0, 0).status ==
        crimson::timeline::KeypointQualityTimelineStatus::ReadFailed);

  broken = columns();
  broken.pose_metric_valid[0] = 0;
  repository = crimson::timeline::MakeKeypointQualityTimelineRepository(
      makeDescriptor(), std::move(broken), &error);
  CHECK(repository->resolveWindow(0, 0).status ==
        crimson::timeline::KeypointQualityTimelineStatus::ReadFailed);
}

void testPageBuffer() {
  auto scheduler = std::make_shared<crimson::data::DataAccessScheduler>(16, 2);
  KeypointQualityTimelineBuffer buffer(scheduler, "test");
  std::string error;
  CHECK(buffer.open(crimson::timeline::MakeKeypointQualityTimelineRepository(
                        makeDescriptor(), columns(), &error),
                    3, 2, 2, &error));
  CHECK(buffer.requestFrame(0, false, &error));
  for (int attempt = 0; attempt < 100 && !buffer.window(0); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(buffer.window(0) != nullptr);
  CHECK(buffer.window(0)->ready());
  CHECK(buffer.requestFrame(0, false, &error));
  CHECK(buffer.metrics().cache_hits == 1);
  CHECK(buffer.requestOverview(4, 1024, &error));
  for (int attempt = 0; attempt < 100 && !buffer.overview(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(buffer.overview() != nullptr);
  CHECK(buffer.overview()->ready());
  CHECK(buffer.requestOverview(4, 1024, &error));
  CHECK(buffer.metrics().overview_cache_hits == 1);
  CHECK(buffer.requestFrame(3, true, &error));
  buffer.close();
}

class BlockingRepository final
    : public crimson::timeline::KeypointQualityTimelineRepository {
public:
  BlockingRepository() : descriptor_(makeDescriptor()) {
    descriptor_.frame_count = 12;
  }

  const crimson::timeline::KeypointQualityTimelineDescriptor &
  descriptor() const override {
    return descriptor_;
  }

  crimson::timeline::KeypointQualityTimelineWindow
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
    crimson::timeline::KeypointQualityTimelineWindow result;
    result.status = crimson::timeline::KeypointQualityTimelineStatus::Ready;
    result.first_camera_frame = first_camera_frame;
    result.last_camera_frame = last_camera_frame;
    for (int64_t frame = first_camera_frame; frame <= last_camera_frame;
         ++frame) {
      crimson::timeline::KeypointQualityFrame value;
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
  crimson::timeline::KeypointQualityTimelineDescriptor descriptor_;
  mutable std::mutex mutex_;
  mutable std::condition_variable condition_;
  mutable size_t calls_ = 0;
  mutable bool first_started_ = false;
  mutable bool release_first_ = false;
};

void testBufferDiscardsSupersededPage() {
  auto scheduler = std::make_shared<crimson::data::DataAccessScheduler>(16, 1);
  KeypointQualityTimelineBuffer buffer(scheduler, "test");
  auto repository = std::make_unique<BlockingRepository>();
  auto *blocking = repository.get();
  std::string error;
  CHECK(buffer.open(std::move(repository), 3, 2, 2, &error));
  CHECK(buffer.requestFrame(0, false, &error));
  CHECK(blocking->waitForFirstRequest());
  CHECK(buffer.requestFrame(6, true, &error));
  blocking->releaseFirstRequest();
  scheduler->waitUntilIdle();
  CHECK(buffer.window(0) == nullptr);
  CHECK(buffer.window(6) != nullptr);
  CHECK(buffer.window(6)->ready());
  CHECK(buffer.metrics().discarded_results == 1);
  buffer.close();
}

} // namespace

int main() {
  testMultiRowAndEmptyFrames();
  testFullRecordingConfidenceOverview();
  testMaskedMissingConfidences();
  testContractFailures();
  testPageBuffer();
  testBufferDiscardsSupersededPage();
  std::puts("keypoint_quality_timeline_tests: PASS");
  return 0;
}
