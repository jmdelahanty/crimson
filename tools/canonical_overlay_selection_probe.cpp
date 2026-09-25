#include "zarr/archive_context.h"
#include "zarr/canonical_overlay_selection.h"
#include "zarr/tensorstore_bound_keypoint_overlay_repository.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

nlohmann::json productJson(
    const crimson::zarr::CanonicalOverlayProductBinding &product) {
  return {{"valid", product.valid},
          {"error", product.error},
          {"group", product.group},
          {"run_id", product.run_id},
          {"schema_id", product.schema_id},
          {"schema_version", product.schema_version},
          {"identity_digest", product.identity_digest},
          {"manifest_payload_digest", product.manifest_payload_digest},
          {"bound_source_run_id", product.bound_source_run_id},
          {"bound_source_payload_digest",
           product.bound_source_payload_digest},
          {"selector_eligible", product.selector_eligible},
          {"bound_selector_exception", product.bound_selector_exception}};
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2 || argc > 34) {
    std::cerr << "Usage: " << argv[0]
              << " <analysis.zarr> [acquisition_frame ...] (max 32)\n";
    return 2;
  }
  std::string error;
  const auto archive = crimson::zarr::ArchiveContext::Open(argv[1], &error);
  if (!archive) {
    std::cerr << "Archive open failed: " << error << '\n';
    return 1;
  }
  const auto selection =
      crimson::zarr::SelectCanonicalOverlaySources(archive, {}, &error);
  if (!selection) {
    std::cerr << "Selection failed: " << error << '\n';
    return 1;
  }
  nlohmann::json output = {
      {"archive_identity", selection->archive_identity},
      {"recording_id", selection->recording_id},
      {"camera_id", selection->camera_id},
      {"first_acquisition_frame", selection->first_acquisition_frame},
      {"frame_count", selection->frame_count},
      {"observation_count", selection->observation_count},
      {"source_width", selection->source_width},
      {"source_height", selection->source_height},
      {"coordinate_surface_id", selection->coordinate_surface_id},
      {"coordinate_descriptor_profile",
       selection->coordinate_descriptor_profile},
      {"instance_key_digest", selection->instance_key_digest},
      {"acquisition_frame_digest", selection->acquisition_frame_digest},
      {"frame_row_offsets_digest", selection->frame_row_offsets_digest},
      {"eye", productJson(selection->eye)},
      {"keypoints", productJson(selection->keypoints)},
      {"mask", productJson(selection->mask)},
      {"shape", productJson(selection->shape)},
  };
  bool passed = selection->eye.valid && selection->keypoints.valid &&
                selection->mask.valid && selection->shape.valid;

  std::vector<int64_t> frames;
  for (int index = 2; index < argc; ++index) {
    try {
      size_t consumed = 0;
      const int64_t frame = std::stoll(argv[index], &consumed, 10);
      if (consumed != std::string(argv[index]).size() || frame < 0 ||
          static_cast<uint64_t>(frame) >= selection->frame_count) {
        throw std::invalid_argument("out of range");
      }
      frames.push_back(frame);
    } catch (const std::exception &) {
      std::cerr << "Invalid acquisition frame: " << argv[index] << '\n';
      return 2;
    }
  }
  if (frames.empty()) {
    frames = {0, static_cast<int64_t>(selection->frame_count / 2),
              static_cast<int64_t>(selection->frame_count - 1)};
  }
  if (selection->keypoints.valid) {
    crimson::zarr::BoundKeypointOverlayOpenRequest request;
    request.archive = archive;
    request.selection = *selection;
    request.max_rows_per_frame = 64;
    request.max_frame_payload_bytes = 1ULL * 1024ULL * 1024ULL;
    crimson::zarr::BoundKeypointOverlayOpenMetrics open_metrics;
    auto repository = crimson::zarr::OpenBoundKeypointOverlayRepository(
        request, &error, &open_metrics);
    if (!repository) {
      output["keypoint_probe_error"] = error;
      passed = false;
    } else {
      output["keypoint_open"] = {
          {"metadata_reads", open_metrics.metadata_reads},
          {"exact_handle_opens", open_metrics.exact_handle_opens},
          {"offset_read_calls", open_metrics.offset_read_calls},
          {"retained_offset_bytes", open_metrics.retained_offset_bytes},
          {"maximum_rows_in_frame", open_metrics.maximum_rows_in_frame}};
      for (const int64_t frame : frames) {
        const auto resolved = repository->resolveCameraFrame(
            frame, static_cast<int>(selection->source_width),
            static_cast<int>(selection->source_height));
        nlohmann::json sample = {
            {"frame", frame},
            {"status", static_cast<int>(resolved.status)},
            {"error", resolved.error},
            {"observation_count", resolved.detections.size()},
        };
        if (resolved.status != crimson::zarr::KeypointOverlayStatus::Mapped &&
            resolved.status != crimson::zarr::KeypointOverlayStatus::Missing) {
          passed = false;
        }
        sample["observations"] = nlohmann::json::array();
        for (const auto &observation : resolved.detections) {
          nlohmann::json row = {
              {"instance_key", observation.instance_key},
              {"instance_key_valid", observation.instance_key_valid},
              {"acquisition_frame", observation.acquisition_frame},
              {"source_crop_row_id", observation.source_crop_row_id},
              {"source_success", observation.source_success},
              {"pose_confidence", observation.pose_confidence},
              {"keypoint_valid", observation.keypoint_valid},
          };
          row["keypoints_img"] = nlohmann::json::array();
          for (const auto &point : observation.keypoints) {
            row["keypoints_img"].push_back({point.x, point.y});
          }
          sample["observations"].push_back(std::move(row));
        }
        output["keypoint_samples"].push_back(std::move(sample));
      }
      const auto access = repository->accessMetrics();
      output["keypoint_access"] = {
          {"frame_requests", access.frame_requests},
          {"rows_resolved", access.rows_resolved},
          {"payload_read_batches", access.payload_read_batches},
          {"payload_read_calls", access.payload_read_calls},
          {"decoded_payload_bytes", access.decoded_payload_bytes},
          {"read_failures", access.read_failures}};
    }
  }
  output["success"] = passed;
  std::cout << output.dump(2) << '\n';
  return passed ? 0 : 1;
}
