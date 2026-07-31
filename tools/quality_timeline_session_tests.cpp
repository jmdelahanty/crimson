#include "gui/quality_timeline_session.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
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

crimson::timeline::DetectionQualityTimelineDescriptor detectionDescriptor() {
  crimson::timeline::DetectionQualityTimelineDescriptor value;
  value.surface_kind = crimson::zarr::DetectionSurfaceKind::RefinedSnapshotV1;
  value.source_group = "refined_detect_runs";
  value.run_name = "detect_session_test";
  value.frame_count = 4;
  value.source_row_count = 4;
  value.instance_row_count = 4;
  value.offset_read_calls = 1;
  value.retained_offset_bytes = 5 * sizeof(int64_t);
  value.source_audit = true;
  value.source_reason_codes = {{0, "none"}};
  return value;
}

crimson::timeline::DetectionQualityColumnData detectionColumns() {
  crimson::timeline::DetectionQualityColumnData value;
  value.source_frame_row_offsets = {0, 1, 1, 2, 4};
  value.source_scores = {0.9f, 0.8f, 0.7f, 0.6f};
  value.source_decision_codes = {0, 0, 0, 0};
  value.source_reason_codes = {0, 0, 0, 0};
  value.instance_frame_row_offsets = value.source_frame_row_offsets;
  value.instance_source_kind_codes = {1, 1, 1, 1};
  return value;
}

crimson::timeline::KeypointQualityTimelineDescriptor keypointDescriptor() {
  crimson::timeline::KeypointQualityTimelineDescriptor value;
  value.source_group = "keypoints_runs";
  value.run_name = "keypoint_session_test";
  value.quality_run_name = "quality_session_test";
  value.frame_count = 4;
  value.row_count = 4;
  value.keypoint_count = 1;
  value.offset_read_calls = 1;
  value.retained_offset_bytes = 5 * sizeof(int64_t);
  value.keypoint_labels = {"nose"};
  value.keypoint_metrics = {{"margin", "probability", false}};
  value.pose_metrics = {{"valid_fraction", "fraction", false}};
  value.keypoint_flags = {{1, "low_confidence"}};
  value.pose_flags = {{1, "source_failed"}};
  value.review_states = {{0, "unreviewed"}};
  value.reason_codes = {{0, "none"}};
  return value;
}

crimson::timeline::KeypointQualityColumnData keypointColumns() {
  crimson::timeline::KeypointQualityColumnData value;
  value.frame_row_offsets = {0, 1, 1, 2, 4};
  value.selected_instance_keys = {10, 11, 12, 13};
  value.quality_instance_keys = value.selected_instance_keys;
  value.keypoint_confidences = {0.9f, 0.8f, 0.7f, 0.6f};
  value.keypoint_valid.assign(4, 1);
  value.pose_confidences = value.keypoint_confidences;
  value.source_success.assign(4, 1);
  value.refined_success.assign(4, 1);
  value.keypoint_edit_flags.assign(4, 0);
  value.flip_corrected.assign(4, 0);
  value.usable_keypoints.assign(4, 1);
  value.review_state_codes.assign(4, 0);
  value.reason_codes.assign(4, 0);
  value.keypoint_metric_values.assign(4, 0.1f);
  value.keypoint_metric_valid.assign(4, 1);
  value.pose_metric_values.assign(4, 1.0f);
  value.pose_metric_valid.assign(4, 1);
  value.keypoint_quality_flags.assign(4, 0);
  value.pose_quality_flags.assign(4, 0);
  value.proposed_keypoint_valid.assign(4, 1);
  value.proposed_pose_usable.assign(4, 1);
  return value;
}

crimson::gui::QualityTimelineSessionRequest sessionRequest() {
  crimson::gui::QualityTimelineSessionRequest value;
  value.detection_archive_path = "factory://detection";
  value.detection_run_name = "detect_session_test";
  const auto artifact = [](const char *name) {
    return crimson::gui::QualityTimelineArtifactSelection{
        std::string("factory://") + name, name, std::string(64, 'a')};
  };
  value.raw_keypoints = artifact("raw");
  value.keypoint_quality = artifact("quality");
  value.body_frame = artifact("body");
  return value;
}

void testFactoryLifecycleAndDurableMetrics() {
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(32, 2, 1, 1);
  crimson::gui::QualityTimelineSession session(scheduler);
  size_t detection_opens = 0;
  size_t keypoint_opens = 0;
  crimson::gui::QualityTimelineRepositoryFactories factories;
  factories.detection = [&detection_opens](std::string *error) {
    ++detection_opens;
    return crimson::timeline::MakeDetectionQualityTimelineRepository(
        detectionDescriptor(), detectionColumns(), error);
  };
  factories.keypoints = [&keypoint_opens](std::string *error) {
    ++keypoint_opens;
    return crimson::timeline::MakeKeypointQualityTimelineRepository(
        keypointDescriptor(), keypointColumns(), error);
  };
  session.configure(sessionRequest(), std::move(factories));
  CHECK(session.detectionConfigured());
  CHECK(session.keypointConfigured());

  crimson::gui::DetectionQualityTimelineControls detection_controls;
  crimson::gui::KeypointQualityTimelineControls keypoint_controls;
  bool ready = false;
  for (int attempt = 0; attempt < 500; ++attempt) {
    session.update(2, false, true, detection_controls, true, keypoint_controls);
    const auto detection_window = session.detectionWindow();
    const auto keypoint_window = session.keypointWindow();
    if (session.detectionState() ==
            crimson::gui::QualityTimelineLoadState::Ready &&
        session.keypointState() ==
            crimson::gui::QualityTimelineLoadState::Ready &&
        detection_window && detection_window->ready() && keypoint_window &&
        keypoint_window->ready()) {
      ready = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!ready) {
    const auto metrics = session.metrics();
    std::fprintf(
        stderr,
        "not ready detection_state=%d keypoint_state=%d detection_error=%s "
        "keypoint_error=%s detection_resolved=%llu detection_failed=%llu "
        "keypoint_resolved=%llu keypoint_failed=%llu detection_last=%s "
        "keypoint_last=%s\n",
        static_cast<int>(session.detectionState()),
        static_cast<int>(session.keypointState()),
        session.detectionError().c_str(), session.keypointError().c_str(),
        static_cast<unsigned long long>(
            metrics.detection_buffer.resolved_windows),
        static_cast<unsigned long long>(
            metrics.detection_buffer.failed_windows),
        static_cast<unsigned long long>(
            metrics.keypoint_buffer.resolved_windows),
        static_cast<unsigned long long>(metrics.keypoint_buffer.failed_windows),
        metrics.detection_buffer.last_error.c_str(),
        metrics.keypoint_buffer.last_error.c_str());
  }
  CHECK(ready);
  CHECK(detection_opens == 1);
  CHECK(keypoint_opens == 1);
  CHECK(session.detectionDescriptor() != nullptr);
  CHECK(session.keypointDescriptor() != nullptr);

  session.update(2, false, false, detection_controls, false, keypoint_controls);
  CHECK(session.detectionState() ==
        crimson::gui::QualityTimelineLoadState::Closed);
  CHECK(session.keypointState() ==
        crimson::gui::QualityTimelineLoadState::Closed);
  const auto metrics = session.metrics();
  CHECK(metrics.detection_descriptor.run_name == "detect_session_test");
  CHECK(metrics.keypoint_descriptor.run_name == "keypoint_session_test");
  CHECK(metrics.detection_open.total_ms >= 0.0);
  CHECK(metrics.keypoint_open.total_ms >= 0.0);
  CHECK(metrics.detection_open.offset_read_calls == 1);
  CHECK(metrics.keypoint_open.offset_read_calls == 1);
  CHECK(metrics.detection_open.retained_offset_bytes == 5 * sizeof(int64_t));
  CHECK(metrics.keypoint_open.retained_offset_bytes == 5 * sizeof(int64_t));
  CHECK(metrics.detection_repository.range_reads == 1);
  CHECK(metrics.keypoint_repository.range_reads == 1);
  CHECK(metrics.detection_buffer.resolved_windows == 1);
  CHECK(metrics.keypoint_buffer.resolved_windows == 1);
  scheduler->shutdown();
}

void testFactoriesRemainLazy() {
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(8, 1, 1, 0);
  crimson::gui::QualityTimelineSession session(scheduler);
  size_t opens = 0;
  crimson::gui::QualityTimelineRepositoryFactories factories;
  factories.detection = [&opens](std::string *) {
    ++opens;
    return std::unique_ptr<
        crimson::timeline::DetectionQualityTimelineRepository>{};
  };
  session.configure(sessionRequest(), std::move(factories));
  session.update(0, false, false, {}, false, {});
  CHECK(opens == 0);
  scheduler->shutdown();
}

} // namespace

int main() {
  testFactoryLifecycleAndDurableMetrics();
  testFactoriesRemainLazy();
  std::puts("quality_timeline_session_tests: PASS");
  return 0;
}
