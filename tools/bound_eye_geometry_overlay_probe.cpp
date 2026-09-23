#include "zarr/archive_context.h"
#include "zarr/canonical_overlay_selection.h"
#include "zarr/eye_geometry_overlay_scene_adapter.h"
#include "zarr/tensorstore_bound_eye_geometry_overlay_repository.h"

#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>

int main(int argc, char** argv) {
  using namespace crimson;
  using Clock = std::chrono::steady_clock;
  using Json = nlohmann::json;
  if (argc < 3 || argc > 19) {
    std::cerr << "Usage: bound_eye_geometry_overlay_probe ARCHIVE FRAME [FRAME ...] (max 17)\n";
    return 2;
  }
  std::string error;
  auto archive = zarr::ArchiveContext::Open(argv[1], &error);
  auto selection = archive ? zarr::SelectCanonicalOverlaySources(archive, {}, &error)
                           : std::optional<zarr::CanonicalOverlaySelection>{};
  if (!selection) { std::cerr << error << '\n'; return 1; }
  const auto start = Clock::now();
  zarr::BoundEyeGeometryOverlayOpenRequest request;
  request.archive = archive;
  request.selection = *selection;
  zarr::BoundEyeGeometryOverlayOpenMetrics opened;
  auto reader = zarr::OpenBoundEyeGeometryOverlayRepository(request, &error, &opened);
  if (!reader) { std::cerr << error << '\n'; return 1; }
  Json report = {{"run", reader->descriptor().run_name},
      {"shape_run", reader->descriptor().source_subject_shape_run},
      {"open_ms", std::chrono::duration<double, std::milli>(Clock::now()-start).count()},
      {"retained_index_bytes", opened.retained_offset_bytes},
      {"samples", Json::array()}};
  bool passed = true;
  for (int i = 2; i < argc; ++i) {
    int64_t frame;
    try {
      size_t consumed = 0;
      frame = std::stoll(argv[i], &consumed);
      if (consumed != std::string(argv[i]).size() || frame < 0 ||
          static_cast<uint64_t>(frame) >= selection->frame_count) return 2;
    } catch (...) { return 2; }
    const auto began = Clock::now();
    const auto result = reader->resolveCameraFrame(frame, selection->source_width,
                                                   selection->source_height);
    const auto scene = overlay::buildReadOnlyOverlayScene(zarr::makeEyeGeometryOverlaySceneInput(
        reader->descriptor(), result, 0, frame, 0,
        selection->source_width, selection->source_height));
    const bool ok = result.status == zarr::EyeGeometryOverlayStatus::Mapped ||
                    result.status == zarr::EyeGeometryOverlayStatus::Missing;
    passed &= ok;
    Json rows = Json::array();
    for (const auto& row : result.detections) {
      rows.push_back({{"instance_key", row.instance_key}, {"frame", row.camera_frame},
          {"roi", {row.roi_x, row.roi_y, row.roi_width, row.roi_height}},
          {"valid", row.frame_valid}, {"body_valid", row.body_frame_valid},
          {"left_valid", row.eyes[0].valid}, {"right_valid", row.eyes[1].valid},
          {"left_angle_valid", row.eyes[0].eye_frame_angle_valid},
          {"right_angle_valid", row.eyes[1].eye_frame_angle_valid},
          {"left_angle", row.eyes[0].eye_frame_angle_degrees},
          {"right_angle", row.eyes[1].eye_frame_angle_degrees},
          {"vergence_valid", row.vergence_valid}, {"vergence", row.vergence_degrees}});
    }
    report["samples"].push_back({{"frame", frame}, {"passed", ok},
        {"error", result.error}, {"rows", rows},
        {"elapsed_ms", std::chrono::duration<double, std::milli>(Clock::now()-began).count()},
        {"primitives", scene.primitives.size()}, {"labels", scene.text_annotations.size()}});
  }
  const auto m = reader->accessMetrics();
  report["payload_read_calls"] = m.payload_read_calls;
  report["logical_requested_bytes_not_nfs_bytes"] = m.logical_payload_bytes_read;
  report["retained_decoded_cache_bytes"] = m.retained_decoded_cache_bytes;
  report["frame_cache_hits"] = m.cache_hits;
  report["passed"] = passed;
  std::cout << report.dump(2) << '\n';
  return passed ? 0 : 1;
}
