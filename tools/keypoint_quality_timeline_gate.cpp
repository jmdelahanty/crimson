#include "zarr/archive_context.h"
#include "zarr/tensorstore_keypoint_v2_repository.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::shared_ptr<crimson::zarr::ArchiveContext>
openArchive(const std::string &path) {
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(path, &error);
  require(archive != nullptr, "Archive open failed: " + error);
  return archive;
}

} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 15,
            "Usage: keypoint_quality_timeline_gate RAW_STORE RAW_RUN "
            "RAW_DIGEST QUALITY_STORE QUALITY_RUN QUALITY_DIGEST "
            "REFINED_STORE REFINED_RUN REFINED_DIGEST BODY_STORE BODY_RUN "
            "BODY_DIGEST FIRST_FRAME LAST_FRAME");
    crimson::zarr::KeypointV2RepositoryOpenRequest request;
    request.raw_archive = openArchive(argv[1]);
    request.raw_run = argv[2];
    request.expected_raw_manifest_digest = argv[3];
    request.quality_archive = openArchive(argv[4]);
    request.quality_run = argv[5];
    request.expected_quality_manifest_digest = argv[6];
    request.refined_archive = openArchive(argv[7]);
    request.refined_run = argv[8];
    request.expected_refined_manifest_digest = argv[9];
    request.body_frame_archive = openArchive(argv[10]);
    request.body_frame_run = argv[11];
    request.expected_body_frame_manifest_digest = argv[12];
    request.allow_selector_ineligible = true;
    const int64_t first = std::stoll(argv[13]);
    const int64_t last = std::stoll(argv[14]);
    std::string error;
    crimson::zarr::KeypointV2RepositoryOpenMetrics open_metrics;
    auto keypoints =
        crimson::zarr::OpenKeypointV2Repository(request, &error, &open_metrics);
    require(keypoints != nullptr, "Keypoint repository open failed: " + error);
    require(keypoints->v2Descriptor().quality_offset_read_calls == 0 &&
                keypoints->accessMetrics().quality_payload_read_calls == 0,
            "Quality payload was not lazy before timeline activation");
    bool current_frame_confidence_ready = false;
    for (int64_t frame = first; frame <= last; ++frame) {
      const auto resolved = keypoints->resolveCameraFrame(frame, 4512, 4512);
      if (resolved.status != crimson::zarr::KeypointOverlayStatus::Mapped) {
        continue;
      }
      for (const auto &observation : resolved.detections) {
        require(observation.keypoint_confidences.size() ==
                        keypoints->v2Descriptor().selected.keypoint_count &&
                    observation.keypoint_valid.size() ==
                        keypoints->v2Descriptor().selected.keypoint_count &&
                    std::isfinite(observation.pose_confidence),
                "Current-frame confidence payload is incomplete");
      }
      current_frame_confidence_ready = true;
      break;
    }
    require(current_frame_confidence_ready,
            "No current-frame confidence observation was found");
    const auto timeline_started = Clock::now();
    auto timeline = keypoints->createQualityTimelineRepository(&error);
    const double timeline_open_ms = std::chrono::duration<double, std::milli>(
                                        Clock::now() - timeline_started)
                                        .count();
    require(timeline != nullptr, "Timeline open failed: " + error);
    require(timeline->descriptor().offset_read_calls == 1 &&
                keypoints->v2Descriptor().quality_offset_read_calls == 1,
            "Quality offsets were not retained exactly once");
    const auto read_started = Clock::now();
    const auto window = timeline->resolveWindow(first, last);
    const double read_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - read_started)
            .count();
    require(window.ready(), "Timeline window failed: " + window.error);
    require(window.frames.size() == static_cast<size_t>(last - first + 1),
            "Timeline frame count changed");
    require(keypoints->accessMetrics().quality_payload_read_calls == 0,
            "Timeline activation contaminated ordinary overlay metrics");
    const auto metrics = timeline->metrics();
    std::cout << "keypoint_quality_timeline_gate: PASS"
              << " frames=" << window.frames.size()
              << " rows=" << window.rows_read
              << " decoded_bytes=" << window.decoded_bytes
              << " retained_offset_bytes="
              << timeline->descriptor().retained_offset_bytes
              << " offset_reads=" << timeline->descriptor().offset_read_calls
              << " timeline_open_ms=" << timeline_open_ms
              << " window_read_ms=" << read_ms
              << " field_reads=" << metrics.peak_concurrent_field_reads << '\n';
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "keypoint_quality_timeline_gate: FAIL " << exception.what()
              << '\n';
    return 1;
  }
}
