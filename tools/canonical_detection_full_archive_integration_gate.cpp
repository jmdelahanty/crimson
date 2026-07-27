#include "canonical_detection_buffer.h"
#include "platform/macos/apple_analysis_repository_loader.h"
#include "read_only_overlay_scene.h"
#include "session_readiness.h"
#include "zarr/affiliated_video_repository.h"
#include "zarr/canonical_detection_overlay_scene_adapter.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr int64_t kFrameStop = 2048;
constexpr int kSourceWidth = 4512;
constexpr int kSourceHeight = 4512;
constexpr size_t kPageFrames = 70;
constexpr size_t kCachePages = 4;

struct LoadedProducts {
  std::shared_ptr<crimson::zarr::ArchiveContext> archive;
  std::unique_ptr<crimson::zarr::CanonicalDetectionRepository> detections;
  std::unique_ptr<crimson::zarr::KeypointOverlayRepository> keypoints;
  std::unique_ptr<crimson::zarr::SubjectMaskOverlayRepository> masks;
  std::unique_ptr<crimson::zarr::SubjectShapeOverlayRepository> shape;
  std::unique_ptr<crimson::zarr::EyeGeometryOverlayRepository> eye_geometry;
  std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository> motion;
  std::unique_ptr<crimson::timeline::EyeAngleTimelineRepository> eye_angles;
  std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
      tail_kinematics;
  std::unique_ptr<crimson::zarr::AnalysisCropGeometryRepository> crop_geometry;
  crimson::zarr::CanonicalDetectionRepositoryOpenMetrics detection_open;
};

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

double elapsedMilliseconds(Clock::time_point started) {
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}

void hashBytes(uint64_t *hash, const void *data, size_t size) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t index = 0; index < size; ++index) {
    *hash ^= bytes[index];
    *hash *= 1099511628211ULL;
  }
}

void hashFrame(uint64_t *hash,
               const crimson::zarr::CanonicalDetectionFrame &frame) {
  hashBytes(hash, &frame.camera_frame, sizeof(frame.camera_frame));
  const uint64_t count = frame.detections.size();
  hashBytes(hash, &count, sizeof(count));
  for (const auto &detection : frame.detections) {
    hashBytes(hash, &detection.row_index, sizeof(detection.row_index));
    hashBytes(hash, detection.normalized_cxcywh.data(),
              detection.normalized_cxcywh.size() * sizeof(float));
    hashBytes(hash, &detection.score, sizeof(detection.score));
    hashBytes(hash, &detection.class_id, sizeof(detection.class_id));
  }
}

std::string hexDigest(uint64_t value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string output(16, '0');
  for (size_t index = 0; index < output.size(); ++index) {
    output[output.size() - index - 1] = digits[value & 0xfU];
    value >>= 4U;
  }
  return output;
}

void adopt(AppleAnalysisRepositoryBundle event, LoadedProducts *products,
           json *timings) {
  require(products != nullptr && timings != nullptr,
          "Internal product adoption target is missing");
  if (event.archive) {
    products->archive = event.archive;
  }
  for (const auto &timing : event.timings) {
    (*timings)[timing.product] = {
        {"available", timing.available},
        {"elapsed_ms", timing.elapsed_ms},
        {"error", timing.error},
    };
  }
  if (event.canonical_detection) {
    products->detections = std::move(event.canonical_detection);
    products->detection_open = event.canonical_detection_open_metrics;
  }
  if (event.keypoints) {
    products->keypoints = std::move(event.keypoints);
  }
  if (event.subject_masks) {
    products->masks = std::move(event.subject_masks);
  }
  if (event.subject_shape) {
    products->shape = std::move(event.subject_shape);
  }
  if (event.eye_geometry) {
    products->eye_geometry = std::move(event.eye_geometry);
  }
  if (event.motion) {
    products->motion = std::move(event.motion);
  }
  if (event.eye_angles) {
    products->eye_angles = std::move(event.eye_angles);
  }
  if (event.tail_kinematics) {
    products->tail_kinematics = std::move(event.tail_kinematics);
  }
  if (event.crop_geometry) {
    products->crop_geometry = std::move(event.crop_geometry);
  }
}

LoadedProducts loadRequiredProducts(
    const std::filesystem::path &archive_path, const std::string &run_name,
    const std::shared_ptr<crimson::data::DataAccessScheduler> &scheduler,
    json *evidence) {
  AppleAnalysisRepositoryLoadRequest request;
  request.archive_path = archive_path.string();
  request.detection_run = run_name;
  request.camera_frame_count = kFrameStop;
  request.swim_bout_timeline_enabled = false;
  request.stimulus_context_timeline_enabled = false;
  request.scheduler = scheduler;

  AppleAnalysisRepositoryLoader loader;
  std::string error;
  const auto started = Clock::now();
  require(loader.start(std::move(request), &error),
          "Could not start repository loader: " + error);

  LoadedProducts products;
  json timings = json::object();
  const auto deadline = Clock::now() + std::chrono::seconds(120);
  while (loader.loading() && Clock::now() < deadline) {
    while (auto ready = loader.takeReady()) {
      adopt(std::move(*ready), &products, &timings);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  while (auto ready = loader.takeReady()) {
    adopt(std::move(*ready), &products, &timings);
  }
  require(!loader.loading(), "Repository loader exceeded 120 seconds");
  const auto progress = loader.progress();

  using Requirement = crimson::session::ProductAvailabilityRequirement;
  const std::vector<crimson::session::SessionReadinessProductRule> rules = {
      {"archive", Requirement::Required},
      {"canonical_detection", Requirement::Required},
      {"keypoints", Requirement::Required},
      {"subject_masks", Requirement::Required},
      {"subject_shape", Requirement::Required},
      {"eye_geometry", Requirement::Required},
      {"motion", Requirement::Required},
      {"eye_angles", Requirement::Required},
      {"tail_kinematics", Requirement::Required},
      {"crop_geometry", Requirement::Required},
  };
  const auto readiness =
      crimson::session::evaluateSessionReadiness(progress, rules);
  (*evidence)["loading"] = {
      {"elapsed_ms", elapsedMilliseconds(started)},
      {"state", crimson::loading::loadingStateName(progress.state)},
      {"completed_products", progress.completed_products},
      {"total_products", progress.total_products},
      {"readiness",
       crimson::session::sessionReadinessStateName(readiness.state)},
      {"readiness_reason", readiness.reason},
      {"products", std::move(timings)},
  };
  loader.close();

  require(readiness.ready(),
          "Required-product readiness failed: " + readiness.reason);
  require(products.archive != nullptr, "Archive product is unavailable");
  require(products.detections != nullptr,
          "Canonical detection product is unavailable");
  require(products.keypoints != nullptr, "Keypoint product is unavailable");
  require(products.masks != nullptr, "Subject-mask product is unavailable");
  require(products.shape != nullptr, "Subject-shape product is unavailable");
  require(products.eye_geometry != nullptr,
          "Eye-geometry product is unavailable");
  require(products.motion != nullptr, "Motion product is unavailable");
  require(products.eye_angles != nullptr, "Eye-angle product is unavailable");
  require(products.tail_kinematics != nullptr,
          "Tail-kinematics product is unavailable");
  require(products.crop_geometry != nullptr,
          "Crop-geometry product is unavailable");
  return products;
}

void validateFirstPresentations(LoadedProducts *products, json *evidence) {
  const auto started = Clock::now();
  auto keypoints = std::async(std::launch::async, [&] {
    return products->keypoints->resolveCameraFrame(0, kSourceWidth,
                                                   kSourceHeight);
  });
  auto masks = std::async(std::launch::async, [&] {
    return products->masks->resolveCameraFrame(0, kSourceWidth, kSourceHeight);
  });
  auto shape = std::async(std::launch::async, [&] {
    return products->shape->resolveCameraFrame(0, kSourceWidth, kSourceHeight);
  });
  auto eyes = std::async(std::launch::async, [&] {
    return products->eye_geometry->resolveCameraFrame(0, kSourceWidth,
                                                      kSourceHeight);
  });
  auto crop = std::async(std::launch::async, [&] {
    return products->crop_geometry->resolveCameraFrame(0, kSourceWidth,
                                                       kSourceHeight);
  });

  const auto keypoint_result = keypoints.get();
  const auto mask_result = masks.get();
  const auto shape_result = shape.get();
  const auto eye_result = eyes.get();
  const auto crop_result = crop.get();
  require(keypoint_result.status ==
              crimson::zarr::KeypointOverlayStatus::Mapped,
          "Frame-zero keypoints did not map: " + keypoint_result.error);
  require(mask_result.status == crimson::zarr::SubjectMaskOverlayStatus::Mapped,
          "Frame-zero subject masks did not map: " + mask_result.error);
  require(shape_result.status ==
              crimson::zarr::SubjectShapeOverlayStatus::Mapped,
          "Frame-zero subject shape did not map: " + shape_result.error);
  require(eye_result.status == crimson::zarr::EyeGeometryOverlayStatus::Mapped,
          "Frame-zero eye geometry did not map: " + eye_result.error);
  require(crop_result.status ==
              crimson::zarr::AnalysisCropGeometryStatus::Mapped,
          "Frame-zero crop geometry did not map");

  auto resolveSeries = [](auto *repository) {
    const auto &descriptor = repository->descriptor();
    crimson::timeline::AnalysisSeriesTimelineRequest request;
    request.source_key = descriptor.default_source;
    request.first_frame = 0;
    request.last_frame = kFrameStop - 1;
    request.anchor_frame = 0;
    request.fallback_frames_per_second = 30.0;
    return repository->resolveWindow(request);
  };
  auto motion = std::async(std::launch::async, [&] {
    return resolveSeries(products->motion.get());
  });
  auto tail = std::async(std::launch::async, [&] {
    return resolveSeries(products->tail_kinematics.get());
  });
  auto eye_angles = std::async(std::launch::async, [&] {
    const auto &descriptor = products->eye_angles->descriptor();
    crimson::timeline::EyeAngleTimelineRequest request;
    request.representation_key = descriptor.default_representation;
    request.first_frame = 0;
    request.last_frame = kFrameStop - 1;
    request.anchor_frame = 0;
    request.fallback_frames_per_second = 30.0;
    return products->eye_angles->resolveWindow(request);
  });
  const auto motion_result = motion.get();
  const auto tail_result = tail.get();
  const auto eye_angle_result = eye_angles.get();
  require(motion_result.ready(), "Motion timeline first window is unavailable");
  require(tail_result.ready(), "Tail-kinematics first window is unavailable");
  require(eye_angle_result.ready(), "Eye-angle first window is unavailable");

  (*evidence)["first_presentations"] = {
      {"elapsed_ms", elapsedMilliseconds(started)},
      {"camera_frame", 0},
      {"keypoint_detections", keypoint_result.detections.size()},
      {"mask_detections", mask_result.detections.size()},
      {"shape_detections", shape_result.detections.size()},
      {"eye_detections", eye_result.detections.size()},
      {"crop_rows", crop_result.frame_row_count},
      {"motion_points", motion_result.published_point_count},
      {"tail_points", tail_result.published_point_count},
      {"eye_angle_points", eye_angle_result.published_point_count},
      {"concurrent_streams", 5},
      {"concurrent_timelines", 3},
  };
}

void validateCanonicalTraversal(
    const std::filesystem::path &archive_path, const std::string &run_name,
    LoadedProducts *products,
    const std::shared_ptr<crimson::data::DataAccessScheduler> &scheduler,
    json *evidence) {
  const auto descriptor = products->detections->descriptor();
  require(descriptor.run_name == run_name,
          "Canonical detection selected the wrong run");
  require(descriptor.camera_frame_count == kFrameStop,
          "Canonical detection frame count is not 2048");
  require(descriptor.row_count == kFrameStop,
          "Canonical detection row count is not 2048");
  const auto &open = products->detection_open;
  require(open.root_metadata_reads == 1,
          "Canonical adapter did not read root metadata exactly once");
  require(open.consolidated_array_declarations == 9,
          "Canonical adapter did not validate nine declarations");
  require(open.exact_handle_opens == 4,
          "Canonical adapter did not perform four exact opens");
  require(open.fallback_metadata_reads == 0 && open.fallback_dtype_opens == 0,
          "Canonical adapter used a forbidden fallback probe");
  require(open.offset_read_calls == 1,
          "Canonical adapter did not read offsets exactly once");

  CanonicalDetectionBuffer buffer(scheduler, archive_path.string());
  std::string error;
  require(buffer.open(std::move(products->detections), kPageFrames, kCachePages,
                      &error),
          "Could not open canonical buffer: " + error);

  uint64_t digest = 1469598103934665603ULL;
  uint64_t detection_count = 0;
  size_t overlay_boxes = 0;
  const auto traversal_started = Clock::now();
  for (int64_t page_start = 0; page_start < kFrameStop;
       page_start += static_cast<int64_t>(kPageFrames)) {
    require(buffer.requestFrame(page_start, false, &error),
            "Traversal request failed: " + error);
    require(buffer.waitForFrame(page_start, std::chrono::seconds(30)),
            "Traversal page timed out at frame " + std::to_string(page_start));
    const int64_t page_stop =
        std::min(kFrameStop, page_start + static_cast<int64_t>(kPageFrames));
    for (int64_t frame_number = page_start; frame_number < page_stop;
         ++frame_number) {
      const auto frame = buffer.frame(frame_number);
      require(frame != nullptr && frame->camera_frame == frame_number,
              "Traversal cache missed frame " + std::to_string(frame_number));
      hashFrame(&digest, *frame);
      detection_count += frame->detections.size();
      const auto scene = crimson::overlay::buildReadOnlyOverlayScene(
          crimson::zarr::makeCanonicalDetectionOverlaySceneInput(
              descriptor, *frame, 0, frame_number, 0, kSourceWidth,
              kSourceHeight));
      require(scene.ready(), "Canonical overlay scene was not ready");
      overlay_boxes +=
          scene.count(crimson::overlay::CameraOverlayLayer::BoundingBoxes);
    }
  }
  require(detection_count == kFrameStop,
          "Canonical traversal did not resolve 2048 detections");
  require(overlay_boxes == kFrameStop,
          "Canonical traversal did not produce 2048 boxes");

  const auto before_seek = scheduler->metrics();
  const std::vector<int64_t> seeks = {140,  700, 1260, 350,
                                      1960, 980, 1750, 2030};
  const auto seek_started = Clock::now();
  for (const int64_t frame : seeks) {
    require(buffer.requestFrame(frame, true, &error),
            "Seek request failed: " + error);
  }
  require(buffer.waitForFrame(seeks.back(), std::chrono::seconds(30)),
          "Final seek did not resolve");
  scheduler->waitUntilIdle();
  require(buffer.frame(seeks.back()) != nullptr,
          "Final seek generation was not published");
  for (size_t index = 0; index + 1 < seeks.size(); ++index) {
    require(buffer.frame(seeks[index]) == nullptr,
            "A stale seek generation remained publishable");
  }
  const auto after_seek = scheduler->metrics();
  const auto buffer_metrics = buffer.metrics();
  const auto repository_metrics = buffer.repositoryMetrics();
  require(buffer_metrics.failed_pages == 0,
          "Canonical buffer reported a failed page");
  require(repository_metrics.failed_reads == 0,
          "Canonical repository reported a failed read");
  require(after_seek.queue.cancelled_requests >
              before_seek.queue.cancelled_requests,
          "Seek burst did not cancel any stale work");

  (*evidence)["canonical"] = {
      {"run", descriptor.run_name},
      {"frames", descriptor.camera_frame_count},
      {"rows", descriptor.row_count},
      {"logical_digest_fnv1a64", hexDigest(digest)},
      {"detections", detection_count},
      {"overlay_boxes", overlay_boxes},
      {"traversal_ms", elapsedMilliseconds(traversal_started)},
      {"seek_settle_ms", elapsedMilliseconds(seek_started)},
      {"open",
       {{"root_metadata_reads", open.root_metadata_reads},
        {"consolidated_declarations", open.consolidated_array_declarations},
        {"exact_handle_opens", open.exact_handle_opens},
        {"fallback_metadata_reads", open.fallback_metadata_reads},
        {"fallback_dtype_opens", open.fallback_dtype_opens},
        {"offset_reads", open.offset_read_calls},
        {"retained_offset_bytes", open.retained_offset_bytes},
        {"elapsed_ms", open.total_ms},
        {"offset_ms", open.offset_read_ms}}},
      {"buffer",
       {{"page_frames", kPageFrames},
        {"cache_pages", kCachePages},
        {"requests", buffer_metrics.requests},
        {"cache_hits", buffer_metrics.cache_hits},
        {"resolved_pages", buffer_metrics.resolved_pages},
        {"discarded_pages", buffer_metrics.discarded_pages},
        {"evicted_pages", buffer_metrics.evicted_pages},
        {"peak_cached_pages", buffer_metrics.peak_cached_pages},
        {"peak_cached_bytes", buffer_metrics.peak_cached_bytes}}},
      {"repository",
       {{"range_reads", repository_metrics.range_reads},
        {"resolved_frames", repository_metrics.resolved_frames},
        {"resolved_rows", repository_metrics.resolved_rows},
        {"field_reads", repository_metrics.ui_field_reads},
        {"peak_concurrent_fields",
         repository_metrics.peak_concurrent_ui_field_reads}}},
      {"cancellation",
       {{"seek_count", seeks.size()},
        {"cancelled_requests", after_seek.queue.cancelled_requests -
                                   before_seek.queue.cancelled_requests},
        {"discarded_completions", after_seek.queue.discarded_completions -
                                      before_seek.queue.discarded_completions},
        {"stale_publications", 0}}},
  };
  buffer.close();
}

void writeEvidence(const std::filesystem::path &path, const json &evidence) {
  std::ofstream output(path);
  require(output.good(), "Could not open evidence output: " + path.string());
  output << evidence.dump(2) << '\n';
  require(output.good(), "Could not write evidence output: " + path.string());
}

void validateAffiliatedVideo(const LoadedProducts &products,
                             const std::filesystem::path &explicit_video_path,
                             json *evidence) {
  require(std::filesystem::is_regular_file(explicit_video_path),
          "Explicit affiliated video is unavailable: " +
              explicit_video_path.string());
  std::string discovery_error;
  const auto discovered = crimson::zarr::DiscoverAffiliatedVideo(
      products.archive, &discovery_error);
  if (discovered) {
    require(std::filesystem::equivalent(discovered->resolved_path,
                                        explicit_video_path),
            "Discovered and explicit affiliated videos do not match");
  }
  (*evidence)["affiliated_video"] = {
      {"explicit_path", explicit_video_path.string()},
      {"explicit_exists", true},
      {"discovery_succeeded", discovered.has_value()},
      {"discovery_error", discovery_error},
  };
  if (discovered) {
    (*evidence)["affiliated_video"]["source"] =
        crimson::zarr::AffiliatedVideoSourceName(discovered->source);
    (*evidence)["affiliated_video"]["stored_path"] =
        discovered->stored_path.string();
    (*evidence)["affiliated_video"]["resolved_path"] =
        discovered->resolved_path.string();
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 6) {
    std::cerr << "Usage: " << argv[0]
              << " ARCHIVE.zarr DETECTION_RUN VIDEO LABEL OUTPUT.json\n";
    return 2;
  }
  const std::filesystem::path archive_path = argv[1];
  const std::string run_name = argv[2];
  const std::filesystem::path video_path = argv[3];
  const std::string label = argv[4];
  const std::filesystem::path output_path = argv[5];
  json evidence = {
      {"schema_id", "crimson.canonical_detection_full_archive_integration"},
      {"schema_version", 1},
      {"classification", "integration_only"},
      {"not_valid_for",
       {"full_duration_startup", "full_duration_cache_pressure",
        "full_duration_object_count", "promotion"}},
      {"label", label},
      {"archive", archive_path.string()},
      {"requested_run", run_name},
      {"frame_range", {0, kFrameStop}},
      {"pass", false},
  };

  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(64, 4, 1, 1);
  try {
    require(std::filesystem::exists(archive_path / "zarr.json"),
            "Archive root is unavailable");
    auto products =
        loadRequiredProducts(archive_path, run_name, scheduler, &evidence);
    evidence["archive_context"] = {
        {"cache_pool_bytes", products.archive->cachePoolBytes()},
    };
    validateAffiliatedVideo(products, video_path, &evidence);
    validateFirstPresentations(&products, &evidence);
    validateCanonicalTraversal(archive_path, run_name, &products, scheduler,
                               &evidence);
    scheduler->waitUntilIdle();
    const auto scheduler_metrics = scheduler->metrics();
    evidence["scheduler"] = {
        {"workers", scheduler_metrics.worker_count},
        {"peak_active", scheduler_metrics.queue.peak_active_requests},
        {"submissions", scheduler_metrics.queue.submissions},
        {"completed", scheduler_metrics.queue.completed_requests},
        {"failed", scheduler_metrics.queue.failed_completions},
        {"work_exceptions", scheduler_metrics.work_exceptions},
    };
    require(scheduler_metrics.queue.failed_completions == 0,
            "Shared scheduler reported failed work");
    require(scheduler_metrics.work_exceptions == 0,
            "Shared scheduler reported a work exception");
    evidence["pass"] = true;
    writeEvidence(output_path, evidence);
    scheduler->shutdown();
    std::cout << "canonical_detection_full_archive_integration_gate: PASS "
              << label << '\n';
    return 0;
  } catch (const std::exception &exception) {
    evidence["error"] = exception.what();
    try {
      writeEvidence(output_path, evidence);
    } catch (...) {
    }
    scheduler->shutdown();
    std::cerr << "canonical_detection_full_archive_integration_gate: FAIL "
              << label << ": " << exception.what() << '\n';
    return 1;
  }
}
