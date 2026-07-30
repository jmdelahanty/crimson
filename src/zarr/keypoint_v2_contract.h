#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace crimson::zarr {

enum class KeypointV2Stage {
  RawObservations,
  RefinedObservations,
  Quality,
  BodyFrame,
};

struct KeypointV2ManifestSummary {
  KeypointV2Stage stage = KeypointV2Stage::RawObservations;
  std::string run_id;
  std::string manifest_digest;
  std::string payload_digest;
  std::string metadata_declarations_digest;
  std::string logical_content_digest;
  std::string source_manifest_digest;
  std::string quality_source_manifest_digest;
  std::string crop_source_manifest_digest;
  std::string source_run_id;
  std::string source_schema_id;
  std::string source_row_signatures_digest;
  std::string row_signatures_digest;
  std::string skeleton_id;
  std::string skeleton_digest;
  size_t frame_count = 0;
  size_t row_count = 0;
  size_t keypoint_count = 0;
  size_t source_width = 0;
  size_t source_height = 0;
  bool selector_eligible = false;
  std::vector<std::string> keypoint_labels;
  std::vector<std::array<size_t, 2>> skeleton_edges;
};

bool ValidateRawKeypointV2RunManifest(const nlohmann::json &manifest,
                                      const std::string &requested_run,
                                      KeypointV2ManifestSummary *summary,
                                      std::string *error = nullptr);

bool ValidateRefinedKeypointV2RunManifest(const nlohmann::json &manifest,
                                          const std::string &requested_run,
                                          KeypointV2ManifestSummary *summary,
                                          std::string *error = nullptr);

bool ValidateKeypointQualityV1RunManifest(const nlohmann::json &manifest,
                                          const std::string &requested_run,
                                          KeypointV2ManifestSummary *summary,
                                          std::string *error = nullptr);

bool ValidateBodyFrameV1RunManifest(const nlohmann::json &manifest,
                                    const std::string &requested_run,
                                    KeypointV2ManifestSummary *summary,
                                    std::string *error = nullptr);

bool ValidateKeypointV2FrameIndex(const std::vector<int64_t> &offsets,
                                  const std::vector<int64_t> &frame_indices,
                                  size_t frame_count, size_t row_count,
                                  std::string *error = nullptr);

bool ValidateKeypointV2Offsets(const std::vector<int64_t> &offsets,
                               size_t frame_count, size_t row_count,
                               std::string *error = nullptr);

bool ValidateKeypointV2InstanceKeys(const std::vector<uint64_t> &instance_keys,
                                    size_t row_count,
                                    std::string *error = nullptr);

} // namespace crimson::zarr
