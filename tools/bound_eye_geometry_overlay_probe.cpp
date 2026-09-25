#include "zarr/archive_context.h"
#include "zarr/canonical_overlay_selection.h"
#include "zarr/eye_geometry_overlay_scene_adapter.h"
#include "zarr/tensorstore_bound_eye_geometry_overlay_repository.h"
#include "zarr/shared_mask_frame_index.h"

#include <chrono>
#include <cstdlib>
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
  std::string shared_index_error;
  request.shared_mask_frame_index = zarr::OpenSharedMaskFrameIndex(
      archive, *selection, &shared_index_error);
  zarr::EyeGeometryFieldMask fields = zarr::EyeGeometryFields::All;
  std::string field_plan = "full";
  if (const char *value = std::getenv("CRIMSON_EYE_PROBE_FIELDS")) {
    field_plan = value;
    using namespace zarr::EyeGeometryFields;
    if (field_plan == "axes") fields = LeftGeometry | RightGeometry;
    else if (field_plan == "gaze") fields = LeftGaze | RightGaze;
    else if (field_plan == "signed") fields = LeftSigned | RightSigned;
    else if (field_plan == "labels")
      fields = LeftAngle | RightAngle | LeftSigned | RightSigned | Vergence;
    else if (field_plan != "full") {
      try { fields = static_cast<zarr::EyeGeometryFieldMask>(
          std::stoul(field_plan, nullptr, 0)); }
      catch (...) { std::cerr << "Invalid CRIMSON_EYE_PROBE_FIELDS\n"; return 2; }
    }
  }
  fields = zarr::NormalizeEyeGeometryFields(fields);
  zarr::BoundEyeGeometryOverlayOpenMetrics opened;
  auto reader = zarr::OpenBoundEyeGeometryOverlayRepository(request, &error, &opened);
  if (!reader) { std::cerr << error << '\n'; return 1; }
  Json report = {{"run", reader->descriptor().run_name},
      {"shape_run", reader->descriptor().source_subject_shape_run},
      {"open_ms", std::chrono::duration<double, std::milli>(Clock::now()-start).count()},
      {"retained_index_bytes", opened.retained_offset_bytes},
      {"shared_index_reused", opened.reused_shared_mask_index},
      {"shared_index_error_if_unavailable", shared_index_error},
      {"offset_read_calls", opened.offset_read_calls},
      {"field_plan", field_plan}, {"requested_loaded_fields", fields},
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
    const auto before = reader->accessMetrics();
    const auto result = reader->resolveCameraFrameFields(
        frame, selection->source_width, selection->source_height, fields);
    const auto after = reader->accessMetrics();
    size_t primitives = 0, labels = 0;
    if (fields == zarr::EyeGeometryFields::All) {
      const auto scene = overlay::buildReadOnlyOverlayScene(
          zarr::makeEyeGeometryOverlaySceneInput(reader->descriptor(), result,
              0, frame, 0, selection->source_width, selection->source_height));
      primitives = scene.primitives.size();
      labels = scene.text_annotations.size();
    }
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
        {"loaded_fields", result.loaded_fields},
        {"payload_read_calls_delta", after.payload_read_calls -
                                     before.payload_read_calls},
        {"logical_payload_bytes_delta", after.logical_payload_bytes_read -
                                         before.logical_payload_bytes_read},
        {"elapsed_ms", std::chrono::duration<double, std::milli>(Clock::now()-began).count()},
        {"primitives", primitives}, {"labels", labels},
        {"scene_skipped_for_partial_fields", fields != zarr::EyeGeometryFields::All}});
  }
  const auto m = reader->accessMetrics();
  report["payload_read_calls"] = m.payload_read_calls;
  report["logical_requested_bytes_not_nfs_bytes"] = m.logical_payload_bytes_read;
  report["retained_decoded_cache_bytes"] = m.retained_decoded_cache_bytes;
  report["frame_cache_hits"] = m.cache_hits;
  report["peak_inflight_payload_reads"] = m.peak_inflight_payload_reads;
  report["per_array_tensorstore_future_elapsed_dispatch_to_consumption"] = Json::array();
  for (const auto &a : m.per_array)
    report["per_array_tensorstore_future_elapsed_dispatch_to_consumption"].push_back({
        {"array", a.array}, {"calls", a.calls},
        {"logical_requested_bytes_not_nfs_bytes", a.logical_bytes},
        {"failures", a.failures}, {"elapsed_count", a.future_elapsed_count},
        {"elapsed_ms_sum", a.future_elapsed_ns_sum / 1e6},
        {"elapsed_ms_max", a.future_elapsed_ns_max / 1e6}});
  report["passed"] = passed;
  std::cout << report.dump(2) << '\n';
  return passed ? 0 : 1;
}
