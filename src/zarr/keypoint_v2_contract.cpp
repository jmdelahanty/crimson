#include "zarr/keypoint_v2_contract.h"

#include "zarr/canonical_json.h"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

namespace crimson::zarr {
namespace {

using json = nlohmann::json;

struct BindingDeclaration {
  const char *path;
  const char *contract_id;
  int contract_version;
};

constexpr BindingDeclaration kRawBindings[] = {
    {"instance_key", "palette.array.keypoint.instance_key", 1},
    {"source_crop_row_ids", "palette.array.keypoint.source_crop_row_ids", 1},
    {"source_acquisition_frame_index",
     "palette.array.keypoint.source_acquisition_frame_index", 1},
    {"frame_indices", "palette.array.keypoint.frame_indices", 1},
    {"frame_row_offsets", "palette.array.frame_row_offsets", 1},
    {"source_crop_row_signature",
     "palette.array.keypoint.source_crop_row_signature", 1},
    {"keypoint_row_signature", "palette.array.keypoint.keypoint_row_signature",
     1},
    {"keypoints_roi", "palette.array.keypoints_roi", 2},
    {"keypoints_img", "palette.array.keypoints_img", 2},
    {"keypoint_confidences", "palette.array.keypoint.keypoint_confidences", 1},
    {"keypoint_valid", "palette.array.keypoint.keypoint_valid", 1},
    {"pose_confidence", "palette.array.keypoint.pose_confidence", 1},
    {"pose_bbox_xyxy_roi", "palette.array.keypoint.pose_bbox_xyxy_roi", 1},
    {"pose_bbox_xyxy_img", "palette.array.keypoint.pose_bbox_xyxy_img", 1},
    {"pose_success", "palette.array.keypoint.pose_success", 1},
};

constexpr BindingDeclaration kQualityBindings[] = {
    {"instance_key", "palette.array.keypoint.instance_key", 1},
    {"source_keypoint_row_ids",
     "palette.array.keypoint_quality.source_keypoint_row_ids", 1},
    {"source_keypoint_row_signature",
     "palette.array.keypoint_quality.source_keypoint_row_signature", 1},
    {"frame_indices", "palette.array.keypoint.frame_indices", 1},
    {"frame_row_offsets", "palette.array.frame_row_offsets", 1},
    {"keypoint_metric_values",
     "palette.array.keypoint_quality.keypoint_metric_values", 1},
    {"keypoint_metric_valid",
     "palette.array.keypoint_quality.keypoint_metric_valid", 1},
    {"pose_metric_values", "palette.array.keypoint_quality.pose_metric_values",
     1},
    {"pose_metric_valid", "palette.array.keypoint_quality.pose_metric_valid",
     1},
    {"keypoint_quality_flags",
     "palette.array.keypoint_quality.keypoint_quality_flags", 1},
    {"pose_quality_flags", "palette.array.keypoint_quality.pose_quality_flags",
     1},
    {"proposed_keypoint_valid",
     "palette.array.keypoint_quality.proposed_keypoint_valid", 1},
    {"proposed_pose_usable",
     "palette.array.keypoint_quality.proposed_pose_usable", 1},
};

constexpr BindingDeclaration kRefinedBindings[] = {
    {"instance_key", "palette.array.keypoint.instance_key", 1},
    {"source_crop_row_ids", "palette.array.keypoint.source_crop_row_ids", 1},
    {"source_acquisition_frame_index",
     "palette.array.keypoint.source_acquisition_frame_index", 1},
    {"frame_indices", "palette.array.keypoint.frame_indices", 1},
    {"frame_row_offsets", "palette.array.frame_row_offsets", 1},
    {"source_crop_row_signature",
     "palette.array.keypoint.source_crop_row_signature", 1},
    {"keypoint_row_signature", "palette.array.keypoint.keypoint_row_signature",
     1},
    {"keypoints_roi", "palette.array.keypoints_roi", 2},
    {"keypoints_img", "palette.array.keypoints_img", 2},
    {"keypoint_confidences", "palette.array.keypoint.keypoint_confidences", 1},
    {"keypoint_valid", "palette.array.keypoint.keypoint_valid", 1},
    {"pose_confidence", "palette.array.keypoint.pose_confidence", 1},
    {"pose_bbox_xyxy_roi", "palette.array.keypoint.pose_bbox_xyxy_roi", 1},
    {"pose_bbox_xyxy_img", "palette.array.keypoint.pose_bbox_xyxy_img", 1},
    {"source_success", "palette.array.refined_keypoint.source_success", 1},
    {"refined_success", "palette.array.refined_keypoint.refined_success", 1},
    {"keypoint_edit_flags",
     "palette.array.refined_keypoint.keypoint_edit_flags", 1},
    {"flip_corrected", "palette.array.refined_keypoint.flip_corrected", 1},
    {"confidence_valid", "palette.array.refined_keypoint.confidence_valid", 1},
    {"geometry_valid", "palette.array.refined_keypoint.geometry_valid", 1},
    {"usable_keypoints", "palette.array.refined_keypoint.usable_keypoints", 1},
    {"review_state_codes", "palette.array.refined_keypoint.review_state_codes",
     1},
    {"reason_codes", "palette.array.refined_keypoint.reason_codes", 1},
};

constexpr BindingDeclaration kBodyFrameBindings[] = {
    {"instance_key", "palette.array.keypoint.instance_key", 1},
    {"source_keypoint_row_ids",
     "palette.array.body_frame.source_keypoint_row_ids", 1},
    {"source_keypoint_row_signature",
     "palette.array.body_frame.source_keypoint_row_signature", 1},
    {"frame_indices", "palette.array.keypoint.frame_indices", 1},
    {"frame_row_offsets", "palette.array.frame_row_offsets", 1},
    {"origin_xy", "palette.array.body_frame.origin_xy", 1},
    {"forward_axis_xy", "palette.array.body_frame.forward_axis_xy", 1},
    {"left_axis_xy", "palette.array.body_frame.left_axis_xy", 1},
    {"axis_valid", "palette.array.body_frame.axis_valid", 1},
    {"heading_deg", "palette.array.body_frame.heading_deg", 1},
};

void assignError(std::string *destination, std::string message) {
  if (destination) {
    *destination = std::move(message);
  }
}

bool exactKeys(const json &value,
               std::initializer_list<std::string_view> expected) {
  if (!value.is_object() || value.size() != expected.size()) {
    return false;
  }
  return std::all_of(
      expected.begin(), expected.end(),
      [&](std::string_view key) { return value.contains(std::string(key)); });
}

template <size_t N>
json expectedBindings(const BindingDeclaration (&declarations)[N]) {
  json result = json::array();
  for (const auto &declaration : declarations) {
    result.push_back({{"path", declaration.path},
                      {"contract_id", declaration.contract_id},
                      {"contract_version", declaration.contract_version},
                      {"required", true}});
  }
  return result;
}

bool validPublication(const json &publication, const char *artifact_class,
                      bool requires_keypoint_authority,
                      std::string *metadata_digest, bool *selector_eligible) {
  const bool exact_fields =
      requires_keypoint_authority
          ? exactKeys(publication,
                      {"artifact_class", "completion_contract",
                       "completion_status", "stage_selector_eligible",
                       "keypoint_authority", "metadata_state",
                       "metadata_declarations_digest_scope",
                       "metadata_declarations_digest_algorithm",
                       "metadata_declarations_digest"})
          : exactKeys(publication,
                      {"artifact_class", "completion_contract",
                       "completion_status", "stage_selector_eligible",
                       "metadata_state", "metadata_declarations_digest_scope",
                       "metadata_declarations_digest_algorithm",
                       "metadata_declarations_digest"});
  if (!exact_fields ||
      publication.value("artifact_class", "") != artifact_class ||
      publication.value("completion_contract", "") !=
          "palette.zarr_run_completion.v1" ||
      publication.value("completion_status", "") != "complete" ||
      publication.value("metadata_state", "") !=
          "direct_and_consolidated_validated" ||
      publication.value("metadata_declarations_digest_scope", "") !=
          "exact_group_and_array_declarations_with_attributes_redacting_only_"
          "run_manifest" ||
      publication.value("metadata_declarations_digest_algorithm", "") !=
          "sha256_canonical_json_v1" ||
      !publication.contains("stage_selector_eligible") ||
      !publication.at("stage_selector_eligible").is_boolean() ||
      !IsLowerSha256(publication.value("metadata_declarations_digest", ""))) {
    return false;
  }
  if (requires_keypoint_authority &&
      (!publication.contains("keypoint_authority") ||
       !publication.at("keypoint_authority").is_boolean() ||
       publication.at("keypoint_authority").get<bool>())) {
    return false;
  }
  if (metadata_digest) {
    *metadata_digest =
        publication.at("metadata_declarations_digest").get<std::string>();
  }
  if (selector_eligible) {
    *selector_eligible = publication.at("stage_selector_eligible").get<bool>();
  }
  return true;
}

bool validateLogicalContent(const json &value, std::string *digest,
                            std::string *error) {
  if (!exactKeys(value, {"digest_algorithm", "digest", "document"}) ||
      value.value("digest_algorithm", "") != "sha256_canonical_json_v1" ||
      !value.at("document").is_object() ||
      !IsLowerSha256(value.value("digest", "")) ||
      CanonicalJsonSha256(value.at("document")) != value.at("digest")) {
    assignError(error, "Keypoint logical_content digest is invalid");
    return false;
  }
  if (digest) {
    *digest = value.at("digest").get<std::string>();
  }
  return true;
}

bool validateSource(const json &source, const char *coverage,
                    bool allow_refined, KeypointV2ManifestSummary *summary,
                    std::string *error) {
  const std::string stage = source.value("stage", "");
  const std::string schema_id = source.value("schema_id", "");
  const bool raw = stage == "keypoints" &&
                   schema_id == "palette.stage.keypoint_observations" &&
                   source.value("schema_version", 0) == 2;
  const bool refined = allow_refined && stage == "refined_keypoints" &&
                       schema_id == "palette.stage.refined_keypoints" &&
                       source.value("schema_version", 0) == 2;
  const std::string expected_path =
      std::string(refined ? "refined_keypoints_runs/" : "keypoints_runs/") +
      source.value("run_name", "");
  if (!exactKeys(source, {"stage", "run_name", "run_path", "schema_id",
                          "schema_version", "manifest_digest", "skeleton_id",
                          "skeleton_digest", "keypoint_row_signatures_digest",
                          "coverage"}) ||
      (!raw && !refined) || source.value("coverage", "") != coverage ||
      source.value("run_path", "") != expected_path ||
      !IsLowerSha256(source.value("manifest_digest", "")) ||
      !IsLowerSha256(source.value("skeleton_digest", "")) ||
      !IsLowerSha256(source.value("keypoint_row_signatures_digest", ""))) {
    assignError(error, "Keypoint source snapshot binding is invalid");
    return false;
  }
  if (summary) {
    summary->source_manifest_digest =
        source.at("manifest_digest").get<std::string>();
    summary->source_run_id = source.at("run_name").get<std::string>();
    summary->source_schema_id = schema_id;
    summary->source_row_signatures_digest =
        source.at("keypoint_row_signatures_digest").get<std::string>();
    summary->skeleton_id = source.at("skeleton_id").get<std::string>();
    summary->skeleton_digest = source.at("skeleton_digest").get<std::string>();
  }
  return true;
}

bool validateDigestEnvelope(const json &value, const char *schema_id,
                            std::string *error) {
  if (!exactKeys(value, {"digest_algorithm", "digest", "document"}) ||
      value.value("digest_algorithm", "") != "sha256_canonical_json_v1" ||
      !value.at("document").is_object() ||
      value.at("document").value("schema_id", "") != schema_id ||
      !IsLowerSha256(value.value("digest", "")) ||
      CanonicalJsonSha256(value.at("document")) != value.at("digest")) {
    assignError(error, std::string(schema_id) + " digest is invalid");
    return false;
  }
  return true;
}

bool validateEnvelope(const json &manifest, const std::string &requested_run,
                      const char *schema_id, const char *persisted_path,
                      const char *stage,
                      std::initializer_list<std::string_view> payload_keys,
                      const json **payload, std::string *payload_digest,
                      std::string *manifest_digest, std::string *error) {
  if (!exactKeys(manifest, {"schema_id", "schema_version",
                            "persisted_attribute", "persisted_path",
                            "digest_algorithm", "payload_digest", "payload"}) ||
      manifest.value("schema_id", "") != schema_id ||
      manifest.value("schema_version", 0) != 1 ||
      manifest.value("persisted_attribute", "") != "run_manifest" ||
      manifest.value("persisted_path", "") != persisted_path ||
      manifest.value("digest_algorithm", "") != "sha256_canonical_json_v1" ||
      !manifest.at("payload").is_object() ||
      !exactKeys(manifest.at("payload"), payload_keys) ||
      manifest.at("payload").value("run_id", "") != requested_run ||
      manifest.at("payload").value("stage", "") != stage) {
    assignError(error, "Keypoint run_manifest envelope is invalid");
    return false;
  }
  const std::string stored_payload_digest =
      manifest.value("payload_digest", "");
  if (!IsLowerSha256(stored_payload_digest) ||
      CanonicalJsonSha256(manifest.at("payload")) != stored_payload_digest) {
    assignError(error, "Keypoint run_manifest payload digest mismatch");
    return false;
  }
  if (payload) {
    *payload = &manifest.at("payload");
  }
  if (payload_digest) {
    *payload_digest = stored_payload_digest;
  }
  if (manifest_digest) {
    *manifest_digest = CanonicalJsonSha256(manifest);
  }
  return true;
}

bool dimensionsCommon(const json &dimensions, size_t *frames, size_t *rows,
                      std::string *error) {
  const size_t frame_count = dimensions.at("n_frames").get<size_t>();
  const size_t row_count = dimensions.at("n_instances").get<size_t>();
  if (frame_count == 0 ||
      dimensions.at("n_frame_boundaries").get<size_t>() != frame_count + 1) {
    assignError(error, "Keypoint manifest dimensions are invalid");
    return false;
  }
  if (frames) {
    *frames = frame_count;
  }
  if (rows) {
    *rows = row_count;
  }
  return true;
}

bool parseSkeleton(const json &binding, size_t expected_keypoints,
                   KeypointV2ManifestSummary *summary, std::string *error) {
  try {
    const auto &pose = binding.at("pose_schema");
    const auto &nodes = pose.at("nodes");
    const auto &edges = pose.at("edges");
    if (!nodes.is_array() || nodes.size() != expected_keypoints ||
        !edges.is_array()) {
      assignError(error, "Raw keypoint skeleton dimensions are invalid");
      return false;
    }
    std::vector<std::string> labels;
    labels.reserve(nodes.size());
    for (size_t index = 0; index < nodes.size(); ++index) {
      if (nodes[index].value("id", std::numeric_limits<size_t>::max()) !=
              index ||
          nodes[index].value("name", "").empty()) {
        assignError(error, "Raw keypoint skeleton nodes are invalid");
        return false;
      }
      labels.push_back(nodes[index].at("name").get<std::string>());
    }
    std::vector<std::array<size_t, 2>> parsed_edges;
    for (const auto &edge : edges) {
      if (!edge.is_array() || edge.size() != 2) {
        assignError(error, "Raw keypoint skeleton edge is invalid");
        return false;
      }
      const size_t first = edge[0].get<size_t>();
      const size_t second = edge[1].get<size_t>();
      if (first >= expected_keypoints || second >= expected_keypoints ||
          first == second) {
        assignError(error, "Raw keypoint skeleton edge is out of range");
        return false;
      }
      parsed_edges.push_back({first, second});
    }
    if (summary) {
      summary->skeleton_id = pose.value("skeleton_id", "");
      summary->keypoint_labels = std::move(labels);
      summary->skeleton_edges = std::move(parsed_edges);
    }
    return true;
  } catch (const json::exception &exception) {
    assignError(error, "Invalid raw keypoint skeleton: " +
                           std::string(exception.what()));
    return false;
  }
}

bool parseCanonicalCode(std::string_view text, uint64_t maximum,
                        uint64_t *value) {
  if (text.empty() || (text.size() > 1 && text.front() == '0')) {
    return false;
  }
  uint64_t parsed = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
    const uint64_t digit = static_cast<uint64_t>(character - '0');
    if (parsed > (maximum - digit) / 10) {
      return false;
    }
    parsed = parsed * 10 + digit;
  }
  if (value) {
    *value = parsed;
  }
  return true;
}

bool canonicalCodeLabel(std::string_view label) {
  if (label.empty() || label.front() < 'a' || label.front() > 'z') {
    return false;
  }
  return std::all_of(label.begin() + 1, label.end(), [](const char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_';
  });
}

bool validateCodeMap(
    const json &value, uint64_t maximum, std::string_view zero_label,
    std::initializer_list<std::pair<uint64_t, std::string_view>> allowed,
    std::string *error) {
  if (!value.is_object() || value.empty()) {
    assignError(error, "Refined keypoint code map is not a nonempty object");
    return false;
  }
  std::unordered_set<std::string> labels;
  for (auto item = value.begin(); item != value.end(); ++item) {
    uint64_t code = 0;
    if (!parseCanonicalCode(item.key(), maximum, &code) ||
        !item.value().is_string()) {
      assignError(error, "Refined keypoint code map entry is invalid");
      return false;
    }
    const std::string label = item.value().get<std::string>();
    if (!canonicalCodeLabel(label) || !labels.insert(label).second) {
      assignError(error,
                  "Refined keypoint code labels are invalid or duplicated");
      return false;
    }
    if (allowed.size() != 0) {
      const auto expected =
          std::find_if(allowed.begin(), allowed.end(), [&](const auto &entry) {
            return entry.first == code && entry.second == label;
          });
      if (expected == allowed.end()) {
        assignError(error, "Refined keypoint review-state code is unsupported");
        return false;
      }
    }
  }
  if (!value.contains("0") || !value.at("0").is_string() ||
      value.at("0").get<std::string>() != zero_label) {
    assignError(error, "Refined keypoint code-zero semantics are invalid");
    return false;
  }
  return true;
}

} // namespace

bool ValidateRefinedKeypointV2CodeRegistries(const json &registries,
                                             std::string *error) {
  try {
    if (!validateDigestEnvelope(
            registries, "palette.refined_keypoint.code_registries", error)) {
      return false;
    }
    const auto &document = registries.at("document");
    if (!exactKeys(document, {"schema_id", "schema_version", "review_state_map",
                              "reason_code_map", "zero_code_semantics"}) ||
        document.value("schema_version", 0) != 1 ||
        !validateCodeMap(document.at("review_state_map"), 255, "unreviewed",
                         {{0, "unreviewed"}, {1, "accepted"}, {2, "rejected"}},
                         error) ||
        !validateCodeMap(document.at("reason_code_map"), 65535, "none", {},
                         error)) {
      return false;
    }
    const auto &zero = document.at("zero_code_semantics");
    if (!exactKeys(zero, {"review_state", "reason"}) ||
        zero.value("review_state", "") != "unreviewed" ||
        zero.value("reason", "") != "none") {
      assignError(error, "Refined keypoint code-zero declaration is invalid");
      return false;
    }
    return true;
  } catch (const json::exception &exception) {
    assignError(error, "Invalid refined keypoint code registry: " +
                           std::string(exception.what()));
    return false;
  }
}

bool ValidateRawKeypointV2RunManifest(const json &manifest,
                                      const std::string &requested_run,
                                      KeypointV2ManifestSummary *summary,
                                      std::string *error) {
  try {
    KeypointV2ManifestSummary parsed;
    parsed.stage = KeypointV2Stage::RawObservations;
    const json *payload = nullptr;
    if (!validateEnvelope(
            manifest, requested_run, "palette.keypoint.run_manifest",
            "keypoints_runs/<run>/zarr.json.attributes.run_manifest",
            "keypoints",
            {"run_id", "stage", "publication", "logical_schema", "storage_plan",
             "source_crop_snapshot", "pose_model_schema_binding",
             "preprocessing", "logical_content", "coordinate_contract"},
            &payload, &parsed.payload_digest, &parsed.manifest_digest, error)) {
      return false;
    }
    parsed.run_id = requested_run;
    if (!validPublication(
            payload->at("publication"), "raw_keypoint_observations", true,
            &parsed.metadata_declarations_digest, &parsed.selector_eligible)) {
      assignError(error, "Raw keypoint publication state is invalid");
      return false;
    }
    const auto &logical = payload->at("logical_schema");
    if (logical.value("schema_id", "") !=
            "palette.stage.keypoint_observations" ||
        logical.value("schema_version", 0) != 2 ||
        logical.value("stage", "") != "keypoints" ||
        logical.value("layout", "") !=
            "sparse_observations_with_frame_row_offsets_v2" ||
        logical.value("base_path", "") != "keypoints_runs/<run>" ||
        logical.at("bindings") != expectedBindings(kRawBindings)) {
      assignError(error, "Raw keypoint logical schema is incompatible");
      return false;
    }
    const auto &dimensions = logical.at("dimensions");
    if (!dimensionsCommon(dimensions, &parsed.frame_count, &parsed.row_count,
                          error)) {
      return false;
    }
    parsed.keypoint_count = dimensions.at("n_keypoints").get<size_t>();
    parsed.source_width = dimensions.at("source_width").get<size_t>();
    parsed.source_height = dimensions.at("source_height").get<size_t>();
    if (parsed.keypoint_count == 0 || parsed.source_width == 0 ||
        parsed.source_height == 0 ||
        logical.at("invariants").value("instances_per_frame", "") !=
            "zero_one_or_many" ||
        logical.at("invariants").value("frame_lookup", "") !=
            "frame_row_offsets_csr" ||
        logical.at("invariants").value("heading_payload", "") !=
            "forbidden_use_bound_body_frame_run" ||
        logical.at("invariants").value("instance_key_semantics", "") !=
            "observation_identity_not_subject_identity") {
      assignError(error, "Raw keypoint dimensions or invariants are invalid");
      return false;
    }
    const std::vector<std::string> forbidden = {"confidence",
                                                "detection_indices",
                                                "detection_success",
                                                "frame_counts",
                                                "heading",
                                                "heading_delta_next_deg",
                                                "heading_delta_prev_deg",
                                                "heading_finite",
                                                "heading_temporal_outlier",
                                                "heading_usable",
                                                "keypoints_norm",
                                                "n_keypoints",
                                                "n_rois",
                                                "quality_labels",
                                                "triangle_angles",
                                                "triangle_angles_raw",
                                                "triangle_area"};
    if (logical.at("forbidden_v1_arrays") != forbidden ||
        !parseSkeleton(payload->at("pose_model_schema_binding"),
                       parsed.keypoint_count, &parsed, error) ||
        !validateLogicalContent(payload->at("logical_content"),
                                &parsed.logical_content_digest, error)) {
      return false;
    }
    const auto &document = payload->at("logical_content").at("document");
    parsed.skeleton_digest = document.value("skeleton_digest", "");
    parsed.row_signatures_digest =
        document.at("arrays").at("keypoint_row_signature").value("sha256", "");
    if (!IsLowerSha256(parsed.skeleton_digest) ||
        !IsLowerSha256(parsed.row_signatures_digest)) {
      assignError(error, "Raw keypoint logical identity digests are invalid");
      return false;
    }
    if (summary) {
      *summary = std::move(parsed);
    }
    return true;
  } catch (const json::exception &exception) {
    assignError(error, "Invalid raw keypoint v2 manifest: " +
                           std::string(exception.what()));
    return false;
  }
}

bool ValidateKeypointQualityV1RunManifest(const json &manifest,
                                          const std::string &requested_run,
                                          KeypointV2ManifestSummary *summary,
                                          std::string *error) {
  try {
    KeypointV2ManifestSummary parsed;
    parsed.stage = KeypointV2Stage::Quality;
    const json *payload = nullptr;
    if (!validateEnvelope(
            manifest, requested_run, "palette.keypoint_quality.run_manifest",
            "keypoint_quality_runs/<run>/zarr.json.attributes.run_manifest",
            "keypoint_quality",
            {"run_id", "stage", "publication", "logical_schema", "storage_plan",
             "source_keypoint_snapshot", "policy", "logical_content"},
            &payload, &parsed.payload_digest, &parsed.manifest_digest, error)) {
      return false;
    }
    parsed.run_id = requested_run;
    if (!validPublication(payload->at("publication"),
                          "observation_local_quality_diagnostics", false,
                          &parsed.metadata_declarations_digest,
                          &parsed.selector_eligible)) {
      assignError(error, "Keypoint quality publication state is invalid");
      return false;
    }
    const auto &logical = payload->at("logical_schema");
    if (logical.value("schema_id", "") != "palette.stage.keypoint_quality" ||
        logical.value("schema_version", 0) != 1 ||
        logical.value("stage", "") != "keypoint_quality" ||
        logical.value("layout", "") !=
            "immutable_source_bound_diagnostics_v1" ||
        logical.value("base_path", "") != "keypoint_quality_runs/<run>" ||
        logical.at("bindings") != expectedBindings(kQualityBindings)) {
      assignError(error, "Keypoint quality logical schema is incompatible");
      return false;
    }
    const auto &dimensions = logical.at("dimensions");
    if (!dimensionsCommon(dimensions, &parsed.frame_count, &parsed.row_count,
                          error)) {
      return false;
    }
    parsed.keypoint_count = dimensions.at("n_keypoints").get<size_t>();
    if (parsed.keypoint_count == 0 ||
        dimensions.at("n_keypoint_metrics").get<size_t>() == 0 ||
        dimensions.at("n_pose_metrics").get<size_t>() == 0 ||
        logical.at("invariants").value("coordinates", "") !=
            "forbidden_reference_bound_keypoint_snapshot" ||
        logical.at("invariants").value("heading", "") !=
            "forbidden_use_bound_body_frame_run" ||
        !validateSource(payload->at("source_keypoint_snapshot"),
                        "every_source_row_exactly_once_in_source_order", false,
                        &parsed, error) ||
        !validateLogicalContent(payload->at("logical_content"),
                                &parsed.logical_content_digest, error)) {
      return false;
    }
    if (summary) {
      *summary = std::move(parsed);
    }
    return true;
  } catch (const json::exception &exception) {
    assignError(error, "Invalid keypoint quality v1 manifest: " +
                           std::string(exception.what()));
    return false;
  }
}

bool ValidateRefinedKeypointV2RunManifest(const json &manifest,
                                          const std::string &requested_run,
                                          KeypointV2ManifestSummary *summary,
                                          std::string *error) {
  try {
    KeypointV2ManifestSummary parsed;
    parsed.stage = KeypointV2Stage::RefinedObservations;
    const json *payload = nullptr;
    if (!validateEnvelope(
            manifest, requested_run, "palette.refined_keypoint.run_manifest",
            "refined_keypoints_runs/<run>/zarr.json.attributes.run_manifest",
            "refined_keypoints",
            {"run_id", "stage", "publication", "logical_schema", "storage_plan",
             "source_bindings", "snapshot_identity", "code_registries",
             "logical_content"},
            &payload, &parsed.payload_digest, &parsed.manifest_digest, error)) {
      return false;
    }
    parsed.run_id = requested_run;
    if (!validPublication(payload->at("publication"),
                          "reviewed_keypoint_authority_candidate", false,
                          &parsed.metadata_declarations_digest,
                          &parsed.selector_eligible)) {
      assignError(error, "Refined keypoint publication state is invalid");
      return false;
    }
    const auto &logical = payload->at("logical_schema");
    if (logical.value("schema_id", "") != "palette.stage.refined_keypoints" ||
        logical.value("schema_version", 0) != 2 ||
        logical.value("stage", "") != "refined_keypoints" ||
        logical.value("layout", "") != "immutable_reviewed_snapshot_v2" ||
        logical.value("base_path", "") != "refined_keypoints_runs/<run>" ||
        logical.at("bindings") != expectedBindings(kRefinedBindings)) {
      assignError(error, "Refined keypoint logical schema is incompatible");
      return false;
    }
    const auto &dimensions = logical.at("dimensions");
    if (!dimensionsCommon(dimensions, &parsed.frame_count, &parsed.row_count,
                          error)) {
      return false;
    }
    parsed.keypoint_count = dimensions.at("n_keypoints").get<size_t>();
    parsed.source_width = dimensions.at("source_width").get<size_t>();
    parsed.source_height = dimensions.at("source_height").get<size_t>();
    if (parsed.keypoint_count == 0 || parsed.source_width == 0 ||
        parsed.source_height == 0 ||
        logical.at("invariants").value("instances_per_frame", "") !=
            "zero_one_or_many" ||
        logical.at("invariants").value("frame_lookup", "") !=
            "frame_row_offsets_csr" ||
        logical.at("invariants").value("heading_payload", "") !=
            "forbidden_use_bound_body_frame_run" ||
        logical.at("invariants").value("instance_key_semantics", "") !=
            "observation_identity_not_subject_identity") {
      assignError(error,
                  "Refined keypoint dimensions or invariants are invalid");
      return false;
    }
    const auto &sources = payload->at("source_bindings");
    if (!exactKeys(sources, {"schema_id", "schema_version",
                             "recording_identity", "raw_keypoint_snapshot",
                             "quality_snapshot", "crop_snapshot", "skeleton",
                             "coordinate_catalog_digest", "dimensions"}) ||
        sources.value("schema_id", "") !=
            "palette.refined_keypoint.source_bindings" ||
        sources.value("schema_version", 0) != 1 ||
        sources.at("dimensions") != dimensions) {
      assignError(error, "Refined keypoint source bindings are invalid");
      return false;
    }
    const auto &raw = sources.at("raw_keypoint_snapshot");
    const auto &quality = sources.at("quality_snapshot");
    const auto &crop = sources.at("crop_snapshot");
    if (raw.value("stage", "") != "keypoints" ||
        raw.value("schema_id", "") != "palette.stage.keypoint_observations" ||
        raw.value("schema_version", 0) != 2 ||
        quality.value("stage", "") != "keypoint_quality" ||
        quality.value("schema_id", "") != "palette.stage.keypoint_quality" ||
        quality.value("schema_version", 0) != 1 ||
        crop.value("stage", "") != "crop" ||
        !IsLowerSha256(raw.value("manifest_digest", "")) ||
        !IsLowerSha256(quality.value("manifest_digest", "")) ||
        !IsLowerSha256(crop.value("manifest_digest", "")) ||
        !IsLowerSha256(raw.value("keypoint_row_signatures_digest", "")) ||
        quality.value("source_row_signatures_digest", "") !=
            raw.value("keypoint_row_signatures_digest", "")) {
      assignError(error, "Refined keypoint source identity is invalid");
      return false;
    }
    parsed.source_manifest_digest =
        raw.at("manifest_digest").get<std::string>();
    parsed.quality_source_manifest_digest =
        quality.at("manifest_digest").get<std::string>();
    parsed.crop_source_manifest_digest =
        crop.at("manifest_digest").get<std::string>();
    parsed.source_run_id = raw.at("run_id").get<std::string>();
    parsed.source_schema_id = raw.at("schema_id").get<std::string>();
    parsed.source_row_signatures_digest =
        raw.at("keypoint_row_signatures_digest").get<std::string>();
    parsed.skeleton_id = sources.at("skeleton").value("skeleton_id", "");
    parsed.skeleton_digest =
        sources.at("skeleton").value("skeleton_digest", "");
    if (parsed.skeleton_id.empty() || !IsLowerSha256(parsed.skeleton_digest)) {
      assignError(error, "Refined keypoint skeleton identity is invalid");
      return false;
    }
    const auto &identity = payload->at("snapshot_identity");
    if (!exactKeys(identity,
                   {"schema_id", "schema_version", "recording_identity",
                    "lineage_id", "snapshot_id", "parent",
                    "ancestry_snapshot_ids", "instance_key_policy",
                    "retired_instance_keys"}) ||
        identity.value("schema_id", "") !=
            "palette.refined_keypoint.snapshot_identity" ||
        identity.value("schema_version", 0) != 1 ||
        !identity.at("parent").is_null() ||
        !identity.at("ancestry_snapshot_ids").empty() ||
        identity.value("instance_key_policy", "") !=
            "preserve_raw_observation_identity_v1" ||
        identity.at("retired_instance_keys").value("count", 1) != 0 ||
        !identity.at("retired_instance_keys").value("nonreuse", false)) {
      assignError(error, "Refined keypoint snapshot identity is invalid");
      return false;
    }
    if (!ValidateRefinedKeypointV2CodeRegistries(payload->at("code_registries"),
                                                 error)) {
      return false;
    }
    if (!validateLogicalContent(payload->at("logical_content"),
                                &parsed.logical_content_digest, error)) {
      return false;
    }
    const auto &document = payload->at("logical_content").at("document");
    const auto &source_digests = document.at("source_manifest_digests");
    parsed.row_signatures_digest =
        document.at("arrays").at("keypoint_row_signature").value("sha256", "");
    if (source_digests.value("raw_keypoints", "") !=
            parsed.source_manifest_digest ||
        source_digests.value("keypoint_quality", "") !=
            parsed.quality_source_manifest_digest ||
        source_digests.value("crop", "") !=
            parsed.crop_source_manifest_digest ||
        document.value("source_row_signatures_digest", "") !=
            parsed.source_row_signatures_digest ||
        !IsLowerSha256(parsed.row_signatures_digest)) {
      assignError(error, "Refined keypoint logical-content lineage is invalid");
      return false;
    }
    if (summary) {
      *summary = std::move(parsed);
    }
    return true;
  } catch (const json::exception &exception) {
    assignError(error, "Invalid refined keypoint v2 manifest: " +
                           std::string(exception.what()));
    return false;
  }
}

bool ValidateBodyFrameV1RunManifest(const json &manifest,
                                    const std::string &requested_run,
                                    KeypointV2ManifestSummary *summary,
                                    std::string *error) {
  try {
    KeypointV2ManifestSummary parsed;
    parsed.stage = KeypointV2Stage::BodyFrame;
    const json *payload = nullptr;
    if (!validateEnvelope(
            manifest, requested_run, "palette.body_frame.run_manifest",
            "analysis/body_frame_runs/<run>/zarr.json.attributes.run_manifest",
            "body_frame",
            {"run_id", "stage", "publication", "logical_schema", "storage_plan",
             "source_keypoint_snapshot", "heading_recipe", "logical_content"},
            &payload, &parsed.payload_digest, &parsed.manifest_digest, error)) {
      return false;
    }
    parsed.run_id = requested_run;
    if (!validPublication(payload->at("publication"),
                          "derived_keypoint_body_frame_cache", true,
                          &parsed.metadata_declarations_digest,
                          &parsed.selector_eligible)) {
      assignError(error, "Body-frame publication state is invalid");
      return false;
    }
    const auto &logical = payload->at("logical_schema");
    if (logical.value("schema_id", "") != "palette.analysis.body_frame" ||
        logical.value("schema_version", 0) != 1 ||
        logical.value("stage", "") != "body_frame" ||
        logical.value("layout", "") != "sparse_observation_body_frames_v1" ||
        logical.value("base_path", "") != "analysis/body_frame_runs/<run>" ||
        logical.at("bindings") != expectedBindings(kBodyFrameBindings)) {
      assignError(error, "Body-frame logical schema is incompatible");
      return false;
    }
    const auto &dimensions = logical.at("dimensions");
    if (!dimensionsCommon(dimensions, &parsed.frame_count, &parsed.row_count,
                          error)) {
      return false;
    }
    if (logical.at("invariants").value("coordinate_space", "") !=
            "source_camera_pixels" ||
        logical.at("invariants").value("heading_derivation", "") !=
            "atan2_negative_y_x_degrees" ||
        logical.at("invariants").value("instances_per_frame", "") == "one" ||
        !validateSource(payload->at("source_keypoint_snapshot"),
                        "complete_row_for_row_snapshot", true, &parsed,
                        error) ||
        !validateLogicalContent(payload->at("logical_content"),
                                &parsed.logical_content_digest, error)) {
      return false;
    }
    if (summary) {
      *summary = std::move(parsed);
    }
    return true;
  } catch (const json::exception &exception) {
    assignError(error, "Invalid body-frame v1 manifest: " +
                           std::string(exception.what()));
    return false;
  }
}

bool ValidateKeypointV2FrameIndex(const std::vector<int64_t> &offsets,
                                  const std::vector<int64_t> &frame_indices,
                                  size_t frame_count, size_t row_count,
                                  std::string *error) {
  if (!ValidateKeypointV2Offsets(offsets, frame_count, row_count, error) ||
      frame_indices.size() != row_count) {
    if (error && error->empty()) {
      *error = "Keypoint frame_indices extent is invalid";
    }
    return false;
  }
  for (size_t frame = 0; frame < frame_count; ++frame) {
    const int64_t first = offsets[frame];
    const int64_t last = offsets[frame + 1];
    for (int64_t row = first; row < last; ++row) {
      if (frame_indices[static_cast<size_t>(row)] !=
          static_cast<int64_t>(frame)) {
        assignError(error,
                    "Keypoint offsets do not group frame_indices exactly");
        return false;
      }
    }
  }
  return true;
}

bool ValidateKeypointV2Offsets(const std::vector<int64_t> &offsets,
                               size_t frame_count, size_t row_count,
                               std::string *error) {
  if (offsets.size() != frame_count + 1 || offsets.empty() ||
      offsets.front() != 0 ||
      offsets.back() != static_cast<int64_t>(row_count)) {
    assignError(error, "Keypoint frame-row offset extents are invalid");
    return false;
  }
  for (size_t frame = 0; frame < frame_count; ++frame) {
    const int64_t first = offsets[frame];
    const int64_t last = offsets[frame + 1];
    if (first < 0 || last < first || last > static_cast<int64_t>(row_count)) {
      assignError(error, "Keypoint frame-row offsets are not monotone");
      return false;
    }
  }
  return true;
}

bool ValidateKeypointV2InstanceKeys(const std::vector<uint64_t> &instance_keys,
                                    size_t row_count, std::string *error) {
  if (instance_keys.size() != row_count) {
    assignError(error, "Keypoint instance_key extent is invalid");
    return false;
  }
  std::unordered_set<uint64_t> unique;
  unique.reserve(instance_keys.size());
  for (const uint64_t key : instance_keys) {
    if (!unique.insert(key).second) {
      assignError(error, "Keypoint instance_key values are not unique");
      return false;
    }
  }
  return true;
}

} // namespace crimson::zarr
