#include "gui/quality_timeline_session.h"

#include "zarr/archive_context.h"
#include "zarr/tensorstore_keypoint_v2_repository.h"

#include <chrono>
#include <future>
#include <iostream>
#include <utility>

namespace crimson::gui {
namespace {

using namespace std::chrono_literals;

struct DetectionOpenResult {
  std::unique_ptr<timeline::DetectionQualityTimelineRepository> repository;
  zarr::DetectionQualityTimelineOpenMetrics metrics;
  std::string error;
};

struct KeypointOpenResult {
  std::unique_ptr<timeline::KeypointQualityTimelineRepository> repository;
  zarr::KeypointV2RepositoryOpenMetrics metrics;
  std::string error;
};

std::shared_ptr<zarr::ArchiveContext>
openArtifactArchive(const QualityTimelineArtifactSelection &selection,
                    const char *label, std::string *error) {
  std::string open_error;
  auto archive =
      zarr::ArchiveContext::Open(selection.archive_path, &open_error);
  if (!archive && error != nullptr) {
    *error = std::string("Could not open ") + label + " archive: " + open_error;
  }
  return archive;
}

KeypointOpenResult
openKeypointTimeline(const QualityTimelineSessionRequest &selection) {
  KeypointOpenResult result;
  zarr::KeypointV2RepositoryOpenRequest request;
  request.raw_archive = openArtifactArchive(selection.raw_keypoints,
                                            "raw-keypoint", &result.error);
  if (!request.raw_archive) {
    return result;
  }
  request.quality_archive = openArtifactArchive(
      selection.keypoint_quality, "keypoint-quality", &result.error);
  if (!request.quality_archive) {
    return result;
  }
  request.body_frame_archive =
      openArtifactArchive(selection.body_frame, "body-frame", &result.error);
  if (!request.body_frame_archive) {
    return result;
  }
  if (!selection.refined_keypoints.empty()) {
    request.refined_archive = openArtifactArchive(
        selection.refined_keypoints, "refined-keypoint", &result.error);
    if (!request.refined_archive) {
      return result;
    }
  }

  request.raw_run = selection.raw_keypoints.run_name;
  request.quality_run = selection.keypoint_quality.run_name;
  request.refined_run = selection.refined_keypoints.run_name;
  request.body_frame_run = selection.body_frame.run_name;
  request.expected_raw_manifest_digest =
      selection.raw_keypoints.manifest_digest;
  request.expected_quality_manifest_digest =
      selection.keypoint_quality.manifest_digest;
  request.expected_refined_manifest_digest =
      selection.refined_keypoints.manifest_digest;
  request.expected_body_frame_manifest_digest =
      selection.body_frame.manifest_digest;
  request.allow_selector_ineligible =
      selection.allow_selector_ineligible_keypoints;
  request.deep_validate_identity = selection.deep_validate_keypoint_identity;

  auto keypoints =
      zarr::OpenKeypointV2Repository(request, &result.error, &result.metrics);
  if (!keypoints) {
    return result;
  }
  result.repository = keypoints->createQualityTimelineRepository(&result.error);
  return result;
}

} // namespace

bool QualityTimelineArtifactSelection::empty() const {
  return archive_path.empty() && run_name.empty() && manifest_digest.empty();
}

bool QualityTimelineArtifactSelection::complete() const {
  return !archive_path.empty() && !run_name.empty() && !manifest_digest.empty();
}

bool QualityTimelineArtifactSelection::operator==(
    const QualityTimelineArtifactSelection &other) const {
  return archive_path == other.archive_path && run_name == other.run_name &&
         manifest_digest == other.manifest_digest;
}

bool QualityTimelineSessionRequest::operator==(
    const QualityTimelineSessionRequest &other) const {
  return detection_archive_path == other.detection_archive_path &&
         detection_surface == other.detection_surface &&
         detection_run_name == other.detection_run_name &&
         allow_selector_ineligible_refined_run ==
             other.allow_selector_ineligible_refined_run &&
         raw_keypoints == other.raw_keypoints &&
         keypoint_quality == other.keypoint_quality &&
         refined_keypoints == other.refined_keypoints &&
         body_frame == other.body_frame &&
         allow_selector_ineligible_keypoints ==
             other.allow_selector_ineligible_keypoints &&
         deep_validate_keypoint_identity ==
             other.deep_validate_keypoint_identity;
}

struct QualityTimelineSession::Impl {
  explicit Impl(std::shared_ptr<data::DataAccessScheduler> shared_scheduler)
      : scheduler(std::move(shared_scheduler)), detection_buffer(scheduler),
        keypoint_buffer(scheduler) {}

  bool detectionConfigured() const {
    return !request.detection_archive_path.empty() &&
           !request.detection_run_name.empty();
  }

  bool keypointConfigured() const {
    return request.raw_keypoints.complete() &&
           request.keypoint_quality.complete() &&
           request.body_frame.complete() &&
           (request.refined_keypoints.empty() ||
            request.refined_keypoints.complete());
  }

  void pollDetection(bool requested) {
    if (detection_state != QualityTimelineLoadState::Opening ||
        !detection_future.valid() ||
        detection_future.wait_for(0ms) != std::future_status::ready) {
      return;
    }
    auto opened = detection_future.get();
    detection_error = std::move(opened.error);
    if (!requested || !detectionConfigured()) {
      detection_state = QualityTimelineLoadState::Closed;
      return;
    }
    if (opened.repository &&
        detection_buffer.open(std::move(opened.repository), 8192, 4096, 3,
                              &detection_error)) {
      detection_descriptor = detection_buffer.descriptor();
      detection_state = QualityTimelineLoadState::Ready;
      std::cout << "[QualityTimeline] detection state=ready run="
                << detection_descriptor.run_name
                << " frames=" << detection_descriptor.frame_count
                << " offset_reads=" << opened.metrics.offset_read_calls
                << " retained_offset_bytes="
                << opened.metrics.retained_offset_bytes
                << " open_ms=" << opened.metrics.total_ms << std::endl;
      return;
    }
    if (detection_error.empty()) {
      detection_error = "Could not open the detection timeline buffer";
    }
    detection_state = QualityTimelineLoadState::Failed;
  }

  void pollKeypoint(bool requested) {
    if (keypoint_state != QualityTimelineLoadState::Opening ||
        !keypoint_future.valid() ||
        keypoint_future.wait_for(0ms) != std::future_status::ready) {
      return;
    }
    auto opened = keypoint_future.get();
    keypoint_error = std::move(opened.error);
    if (!requested || !keypointConfigured()) {
      keypoint_state = QualityTimelineLoadState::Closed;
      return;
    }
    if (opened.repository &&
        keypoint_buffer.open(std::move(opened.repository), 4096, 2048, 3,
                             &keypoint_error)) {
      keypoint_descriptor = keypoint_buffer.descriptor();
      keypoint_state = QualityTimelineLoadState::Ready;
      std::cout << "[QualityTimeline] keypoints state=ready run="
                << keypoint_descriptor.run_name
                << " quality_run=" << keypoint_descriptor.quality_run_name
                << " frames=" << keypoint_descriptor.frame_count
                << " offset_reads=" << keypoint_descriptor.offset_read_calls
                << " retained_offset_bytes="
                << keypoint_descriptor.retained_offset_bytes
                << " open_ms=" << opened.metrics.total_ms << std::endl;
      return;
    }
    if (keypoint_error.empty()) {
      keypoint_error = "Could not open the keypoint timeline buffer";
    }
    keypoint_state = QualityTimelineLoadState::Failed;
  }

  void updateDetection(int64_t current_frame, bool discontinuity,
                       bool requested,
                       const DetectionQualityTimelineControls &controls) {
    pollDetection(requested);
    detection_window.reset();
    detection_overview.reset();
    if (!requested) {
      if (detection_state == QualityTimelineLoadState::Ready ||
          detection_state == QualityTimelineLoadState::Failed) {
        detection_buffer.close();
        detection_error.clear();
        detection_state = QualityTimelineLoadState::Closed;
      }
      return;
    }
    if (detection_state == QualityTimelineLoadState::Closed &&
        detectionConfigured()) {
      zarr::DetectionQualityTimelineOpenRequest open_request;
      open_request.surface_kind = request.detection_surface;
      open_request.run_name = request.detection_run_name;
      open_request.allow_selector_ineligible_refined_run =
          request.allow_selector_ineligible_refined_run;
      const std::string archive_path = request.detection_archive_path;
      detection_state = QualityTimelineLoadState::Opening;
      detection_future = std::async(
          std::launch::async,
          [archive_path, open_request = std::move(open_request)]() mutable {
            DetectionOpenResult result;
            auto archive =
                zarr::ArchiveContext::Open(archive_path, &result.error);
            if (archive) {
              result.repository = zarr::OpenDetectionQualityTimelineRepository(
                  archive, open_request, &result.error, &result.metrics);
            }
            return result;
          });
      return;
    }
    if (detection_state != QualityTimelineLoadState::Ready ||
        current_frame < 0 ||
        current_frame >=
            static_cast<int64_t>(detection_descriptor.frame_count)) {
      return;
    }
    const bool accepted =
        controls.full_recording
            ? detection_buffer.requestOverview(1200, 8 * 1024 * 1024,
                                               &detection_error)
            : detection_buffer.requestFrame(current_frame, discontinuity,
                                            &detection_error);
    if (!accepted) {
      detection_state = QualityTimelineLoadState::Failed;
      return;
    }
    if (controls.full_recording) {
      detection_overview = detection_buffer.overview();
    } else {
      detection_window = detection_buffer.window(current_frame);
    }
  }

  void updateKeypoint(int64_t current_frame, bool discontinuity, bool requested,
                      const KeypointQualityTimelineControls &controls) {
    pollKeypoint(requested);
    keypoint_window.reset();
    keypoint_overview.reset();
    if (!requested) {
      if (keypoint_state == QualityTimelineLoadState::Ready ||
          keypoint_state == QualityTimelineLoadState::Failed) {
        keypoint_buffer.close();
        keypoint_error.clear();
        keypoint_state = QualityTimelineLoadState::Closed;
      }
      return;
    }
    if (keypoint_state == QualityTimelineLoadState::Closed &&
        keypointConfigured()) {
      const auto selection = request;
      keypoint_state = QualityTimelineLoadState::Opening;
      keypoint_future = std::async(std::launch::async, [selection]() {
        return openKeypointTimeline(selection);
      });
      return;
    }
    if (keypoint_state != QualityTimelineLoadState::Ready ||
        current_frame < 0 ||
        current_frame >=
            static_cast<int64_t>(keypoint_descriptor.frame_count)) {
      return;
    }
    const bool accepted =
        controls.full_recording
            ? keypoint_buffer.requestOverview(1200, 8 * 1024 * 1024,
                                              &keypoint_error)
            : keypoint_buffer.requestFrame(current_frame, discontinuity,
                                           &keypoint_error);
    if (!accepted) {
      keypoint_state = QualityTimelineLoadState::Failed;
      return;
    }
    if (controls.full_recording) {
      keypoint_overview = keypoint_buffer.overview();
    } else {
      keypoint_window = keypoint_buffer.window(current_frame);
    }
  }

  void close() {
    detection_buffer.close();
    keypoint_buffer.close();
    if (detection_future.valid()) {
      detection_future.wait();
      (void)detection_future.get();
    }
    if (keypoint_future.valid()) {
      keypoint_future.wait();
      (void)keypoint_future.get();
    }
    detection_state = QualityTimelineLoadState::Closed;
    keypoint_state = QualityTimelineLoadState::Closed;
    detection_descriptor = {};
    keypoint_descriptor = {};
    detection_window.reset();
    detection_overview.reset();
    keypoint_window.reset();
    keypoint_overview.reset();
    detection_error.clear();
    keypoint_error.clear();
  }

  std::shared_ptr<data::DataAccessScheduler> scheduler;
  QualityTimelineSessionRequest request;
  DetectionQualityTimelineBuffer detection_buffer;
  KeypointQualityTimelineBuffer keypoint_buffer;
  QualityTimelineLoadState detection_state = QualityTimelineLoadState::Closed;
  QualityTimelineLoadState keypoint_state = QualityTimelineLoadState::Closed;
  std::future<DetectionOpenResult> detection_future;
  std::future<KeypointOpenResult> keypoint_future;
  timeline::DetectionQualityTimelineDescriptor detection_descriptor;
  timeline::KeypointQualityTimelineDescriptor keypoint_descriptor;
  std::shared_ptr<const timeline::DetectionQualityTimelineWindow>
      detection_window;
  std::shared_ptr<const timeline::DetectionQualityTimelineOverview>
      detection_overview;
  std::shared_ptr<const timeline::KeypointQualityTimelineWindow>
      keypoint_window;
  std::shared_ptr<const timeline::KeypointQualityTimelineOverview>
      keypoint_overview;
  std::string detection_error;
  std::string keypoint_error;
};

QualityTimelineSession::QualityTimelineSession(
    std::shared_ptr<data::DataAccessScheduler> scheduler)
    : impl_(std::make_unique<Impl>(std::move(scheduler))) {}

QualityTimelineSession::~QualityTimelineSession() { impl_->close(); }

void QualityTimelineSession::configure(QualityTimelineSessionRequest request) {
  if (request == impl_->request) {
    return;
  }
  impl_->close();
  impl_->request = std::move(request);
}

void QualityTimelineSession::update(
    int64_t current_frame, bool discontinuity, bool detection_requested,
    const DetectionQualityTimelineControls &detection_controls,
    bool keypoint_requested,
    const KeypointQualityTimelineControls &keypoint_controls) {
  impl_->updateDetection(current_frame, discontinuity, detection_requested,
                         detection_controls);
  impl_->updateKeypoint(current_frame, discontinuity, keypoint_requested,
                        keypoint_controls);
}

void QualityTimelineSession::close() { impl_->close(); }

bool QualityTimelineSession::detectionConfigured() const {
  return impl_->detectionConfigured();
}

QualityTimelineLoadState QualityTimelineSession::detectionState() const {
  return impl_->detection_state;
}

const std::string &QualityTimelineSession::detectionError() const {
  return impl_->detection_error;
}

const timeline::DetectionQualityTimelineDescriptor *
QualityTimelineSession::detectionDescriptor() const {
  return impl_->detection_state == QualityTimelineLoadState::Ready
             ? &impl_->detection_descriptor
             : nullptr;
}

std::shared_ptr<const timeline::DetectionQualityTimelineWindow>
QualityTimelineSession::detectionWindow() const {
  return impl_->detection_window;
}

std::shared_ptr<const timeline::DetectionQualityTimelineOverview>
QualityTimelineSession::detectionOverview() const {
  return impl_->detection_overview;
}

bool QualityTimelineSession::keypointConfigured() const {
  return impl_->keypointConfigured();
}

QualityTimelineLoadState QualityTimelineSession::keypointState() const {
  return impl_->keypoint_state;
}

const std::string &QualityTimelineSession::keypointError() const {
  return impl_->keypoint_error;
}

const timeline::KeypointQualityTimelineDescriptor *
QualityTimelineSession::keypointDescriptor() const {
  return impl_->keypoint_state == QualityTimelineLoadState::Ready
             ? &impl_->keypoint_descriptor
             : nullptr;
}

std::shared_ptr<const timeline::KeypointQualityTimelineWindow>
QualityTimelineSession::keypointWindow() const {
  return impl_->keypoint_window;
}

std::shared_ptr<const timeline::KeypointQualityTimelineOverview>
QualityTimelineSession::keypointOverview() const {
  return impl_->keypoint_overview;
}

} // namespace crimson::gui
