#include "zarr/subject_mask_bundle_v2_contract.h"

#include "zarr/canonical_json.h"
#include "zarr/zarr_metadata_equivalence.h"

#include <array>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace crimson::zarr {
namespace {

using json = nlohmann::json;

struct ArrayDeclaration {
  std::string dtype;
  std::vector<size_t> shape;
};

using ArrayDeclarations = std::map<std::string, ArrayDeclaration>;

constexpr std::array<std::string_view, 5> kSharedIdentityPaths = {
    "source_crop_row_ids", "instance_key", "source_acquisition_frame_index",
    "frame_row_offsets", "source_crop_xywh"};

constexpr std::array<std::string_view, 6> kQualitySourcePaths = {
    "masks_roi",           "instance_key",
    "source_crop_row_ids", "source_acquisition_frame_index",
    "frame_row_offsets",   "available_channels"};

void AssignError(std::string *error, std::string value) {
  if (error) {
    *error = std::move(value);
  }
}

bool IsSha256(const json &value) {
  static const std::regex pattern("^[0-9a-f]{64}$");
  return value.is_string() &&
         std::regex_match(value.get_ref<const std::string &>(), pattern);
}

bool SafeRunId(const std::string &value) {
  return !value.empty() && value != "." && value != ".." &&
         value.find('/') == std::string::npos &&
         value.find_first_of(" \t\r\n") == std::string::npos;
}

bool ExactKeys(const json &value,
               std::initializer_list<std::string_view> expected) {
  if (!value.is_object() || value.size() != expected.size()) {
    return false;
  }
  for (const auto key : expected) {
    if (!value.contains(std::string(key))) {
      return false;
    }
  }
  return true;
}

std::optional<json> ReadJson(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  try {
    json value;
    input >> value;
    if (!value.is_object()) {
      return std::nullopt;
    }
    return value;
  } catch (const json::exception &) {
    return std::nullopt;
  }
}

const json *ConsolidatedEntry(const json &root, const std::string &path) {
  try {
    const auto &entries = root.at("consolidated_metadata").at("metadata");
    const auto found = entries.find(path);
    return found == entries.end() ? nullptr : &*found;
  } catch (const json::exception &) {
    return nullptr;
  }
}

bool NormalizeEmptyGroupConsolidation(json *value) {
  if (!value || !value->is_object()) {
    return false;
  }
  const auto found = value->find("consolidated_metadata");
  if (found == value->end()) {
    return true;
  }
  const bool empty =
      found->is_null() ||
      (found->is_object() && found->size() == 3 &&
       found->value("kind", "") == "inline" &&
       !found->value("must_understand", true) && found->contains("metadata") &&
       found->at("metadata").is_object() && found->at("metadata").empty());
  if (!empty) {
    return false;
  }
  value->erase(found);
  return true;
}

bool Dimensions(const json &value, size_t expected_channels,
                size_t *frame_count, size_t *row_count, size_t *height,
                size_t *width) {
  if (!ExactKeys(value,
                 {"n_frames", "n_frame_boundaries", "n_instances", "n_rois",
                  "n_channels", "H", "W", "roi_height", "roi_width"})) {
    return false;
  }
  try {
    const size_t frames = value.at("n_frames").get<size_t>();
    const size_t rows = value.at("n_rois").get<size_t>();
    const size_t channels = value.at("n_channels").get<size_t>();
    const size_t h = value.at("roi_height").get<size_t>();
    const size_t w = value.at("roi_width").get<size_t>();
    if (frames == 0 || rows == 0 || channels != expected_channels || h == 0 ||
        w == 0 || value.at("n_frame_boundaries").get<size_t>() != frames + 1 ||
        value.at("n_instances").get<size_t>() != rows ||
        value.at("H").get<size_t>() != h || value.at("W").get<size_t>() != w) {
      return false;
    }
    *frame_count = frames;
    *row_count = rows;
    *height = h;
    *width = w;
    return true;
  } catch (const json::exception &) {
    return false;
  }
}

bool Components(const json &value,
                const std::vector<std::string> &expected_labels) {
  return ExactKeys(value, {"schema_id", "schema_version", "labels",
                           "channel_axis", "ordering"}) &&
         value.value("schema_id", "") ==
             "palette.subject_mask.component_registry" &&
         value.value("schema_version", 0) == 1 &&
         value.value("channel_axis", 0) == 1 &&
         value.value("ordering", "") == "persisted_exact_order" &&
         value.contains("labels") &&
         value.at("labels").get<std::vector<std::string>>() == expected_labels;
}

ArrayDeclarations CoreDeclarations(size_t frames, size_t rows, size_t channels,
                                   size_t height, size_t width, bool refined) {
  ArrayDeclarations result = {
      {"available_channels", {"bool", {channels}}},
      {"frame_row_offsets", {"int64", {frames + 1}}},
      {"instance_key", {"uint64", {rows}}},
      {"metrics/area_px", {"float32", {rows, channels}}},
      {"metrics/bbox_valid", {"bool", {rows, channels}}},
      {"metrics/bbox_xyxy", {"float32", {rows, channels, 4}}},
      {"metrics/centroid_valid", {"bool", {rows, channels}}},
      {"metrics/centroid_xy", {"float32", {rows, channels, 2}}},
      {"metrics/mask_present", {"bool", {rows, channels}}},
      {"source_acquisition_frame_index", {"int64", {rows}}},
      {"source_crop_row_ids", {"int64", {rows}}},
      {"source_crop_xywh", {"float32", {rows, 4}}},
  };
  if (refined) {
    result.emplace("masks_roi",
                   ArrayDeclaration{"uint8", {rows, channels, height, width}});
  } else {
    result.emplace("mask_probs_roi",
                   ArrayDeclaration{"uint8", {rows, channels, height, width}});
    result.emplace("metrics/prob_max",
                   ArrayDeclaration{"float32", {rows, channels}});
  }
  return result;
}

ArrayDeclarations QualityDeclarations(size_t frames, size_t rows,
                                      size_t channels) {
  return {
      {"component_metric_valid", {"bool", {rows, channels, 8}}},
      {"component_metric_values", {"float32", {rows, channels, 8}}},
      {"component_quality_flags", {"uint16", {rows, channels}}},
      {"frame_row_offsets", {"int64", {frames + 1}}},
      {"instance_key", {"uint64", {rows}}},
      {"observation_metric_valid", {"bool", {rows, 7}}},
      {"observation_metric_values", {"float32", {rows, 7}}},
      {"observation_quality_flags", {"uint16", {rows}}},
      {"proposed_component_usable", {"bool", {rows, channels}}},
      {"proposed_observation_usable", {"bool", {rows}}},
      {"source_acquisition_frame_index", {"int64", {rows}}},
      {"source_mask_row_ids", {"int64", {rows}}},
  };
}

const json *ManifestArrays(const json &manifest) {
  try {
    return &manifest.at("payload")
                .at("logical_content")
                .at("document")
                .at("arrays");
  } catch (const json::exception &) {
    return nullptr;
  }
}

bool ValidateArrayInventory(const json &manifest,
                            const ArrayDeclarations &expected,
                            std::string *error) {
  const json *arrays = ManifestArrays(manifest);
  if (!arrays || !arrays->is_object() || arrays->size() != expected.size()) {
    AssignError(error, "Subject-mask member array inventory is not exact");
    return false;
  }
  for (const auto &[path, declaration] : expected) {
    const auto found = arrays->find(path);
    if (found == arrays->end() ||
        !ExactKeys(*found, {"shape", "dtype", "digest_algorithm", "sha256"}) ||
        found->value("dtype", "") != declaration.dtype ||
        found->value("digest_algorithm", "") !=
            "sha256_c_contiguous_bytes_v1" ||
        !found->contains("shape") ||
        found->at("shape").get<std::vector<size_t>>() != declaration.shape ||
        !IsSha256(found->at("sha256"))) {
      AssignError(error, "Invalid subject-mask member declaration: " + path);
      return false;
    }
  }
  return true;
}

bool ValidateManifestEnvelope(const json &manifest, std::string_view schema_id,
                              int schema_version, std::string *error) {
  const bool quality = schema_id == "palette.subject_mask_quality.run_manifest";
  const bool exact_keys =
      quality ? ExactKeys(manifest,
                          {"schema_id", "schema_version", "digest_algorithm",
                           "payload_digest", "payload", "persisted_attribute",
                           "persisted_path"})
              : ExactKeys(manifest,
                          {"schema_id", "schema_version", "digest_algorithm",
                           "payload_digest", "payload"});
  if (!exact_keys || manifest.value("schema_id", "") != schema_id ||
      manifest.value("schema_version", 0) != schema_version ||
      manifest.value("digest_algorithm", "") != "sha256_canonical_json_v1" ||
      !manifest.contains("payload_digest") ||
      !IsSha256(manifest.at("payload_digest")) ||
      CanonicalJsonSha256(manifest.at("payload")) !=
          manifest.value("payload_digest", "")) {
    AssignError(error, "Subject-mask member manifest envelope is invalid");
    return false;
  }
  if (quality && (manifest.value("persisted_attribute", "") != "run_manifest" ||
                  manifest.value("persisted_path", "") !=
                      "subject_mask_quality_runs/<run>/"
                      "zarr.json.attributes.run_manifest")) {
    AssignError(error,
                "Subject-mask quality manifest persisted path is invalid");
    return false;
  }
  return true;
}

bool ValidateLogicalContent(const json &manifest, std::string *error) {
  try {
    const auto &content = manifest.at("payload").at("logical_content");
    if (!ExactKeys(content, {"digest_algorithm", "digest", "document"}) ||
        content.value("digest_algorithm", "") != "sha256_canonical_json_v1" ||
        !IsSha256(content.at("digest")) ||
        CanonicalJsonSha256(content.at("document")) !=
            content.value("digest", "")) {
      AssignError(error, "Subject-mask member logical-content digest differs");
      return false;
    }
    return true;
  } catch (const json::exception &) {
    AssignError(error, "Subject-mask member logical content is malformed");
    return false;
  }
}

bool RedactMemberGroup(json *value, std::string_view manifest_attribute) {
  if (!NormalizeEmptyGroupConsolidation(value) ||
      value->value("node_type", "") != "group") {
    return false;
  }
  auto attributes = value->find("attributes");
  if (attributes == value->end() || !attributes->is_object()) {
    return false;
  }
  constexpr std::array<std::string_view, 8> kLifecycleAndTransport = {
      "status",
      "palette_run_completion_status",
      "palette_run_completed_at_utc",
      "atomic_publication_owner_uuid",
      "atomic_publication_tombstone",
      "cluster_output_staging",
      "publication_status",
      "subject_mask_bundle_selector_eligible"};
  attributes->erase(std::string(manifest_attribute));
  for (const auto name : kLifecycleAndTransport) {
    attributes->erase(std::string(name));
  }
  return true;
}

bool ValidateMemberMetadata(const std::filesystem::path &archive_root,
                            const json &root, const std::string &run_path,
                            const ArrayDeclarations &declarations,
                            const std::string &expected_digest,
                            bool quality_digest, std::string *error) {
  json normalized = json::object();
  std::set<std::string> expected_nodes = {run_path};
  const auto direct_group = ReadJson(archive_root / run_path / "zarr.json");
  const auto *consolidated_group = ConsolidatedEntry(root, run_path);
  if (!direct_group || !consolidated_group ||
      !internal::EquivalentDirectAndConsolidatedZarrNode(*direct_group,
                                                         *consolidated_group)) {
    AssignError(error,
                "Subject-mask member group metadata differs: " + run_path);
    return false;
  }
  json group = *direct_group;
  if (!RedactMemberGroup(&group, "run_manifest")) {
    AssignError(error,
                "Subject-mask member group metadata is invalid: " + run_path);
    return false;
  }
  normalized[""] = std::move(group);

  for (const auto &[relative, declaration] : declarations) {
    const std::string full = run_path + "/" + relative;
    expected_nodes.insert(full);
    const auto direct = ReadJson(archive_root / full / "zarr.json");
    const auto *consolidated = ConsolidatedEntry(root, full);
    if (!direct || !consolidated ||
        !internal::EquivalentDirectAndConsolidatedZarrNode(*direct,
                                                           *consolidated) ||
        direct->value("zarr_format", 0) != 3 ||
        direct->value("node_type", "") != "array" ||
        direct->value("data_type", "") != declaration.dtype ||
        !direct->contains("shape") ||
        direct->at("shape").get<std::vector<size_t>>() != declaration.shape) {
      AssignError(error, "Subject-mask array metadata is invalid: " + full);
      return false;
    }
    normalized[relative] = *direct;
  }

  const auto &all = root.at("consolidated_metadata").at("metadata");
  const std::string prefix = run_path + "/";
  for (auto item = all.begin(); item != all.end(); ++item) {
    if (item.key() != run_path && item.key().rfind(prefix, 0) != 0) {
      continue;
    }
    if (expected_nodes.find(item.key()) != expected_nodes.end()) {
      continue;
    }
    const auto relative = item.key().substr(prefix.size());
    if (relative == "metrics" && !quality_digest) {
      continue;
    }
    AssignError(error, "Unexpected subject-mask member node: " + item.key());
    return false;
  }

  const json digest_document =
      quality_digest
          ? json{{"scope",
                  "exact_group_and_array_declarations_redacting_manifest_"
                  "lifecycle_and_transport_publication_attrs"},
                 {"declarations", normalized}}
          : normalized;
  if (CanonicalJsonSha256(digest_document) != expected_digest) {
    AssignError(error,
                "Subject-mask member metadata digest differs: " + run_path);
    return false;
  }
  return true;
}

std::string ArrayHash(const json &manifest, std::string_view path) {
  const auto *arrays = ManifestArrays(manifest);
  if (!arrays) {
    return {};
  }
  const auto found = arrays->find(std::string(path));
  return found == arrays->end() ? std::string{} : found->value("sha256", "");
}

bool ValidateMemberReference(const json &reference, std::string_view role,
                             std::string_view family, const json &manifest,
                             std::string *run_id, std::string *error) {
  if (!ExactKeys(reference,
                 {"role", "family", "run_id", "run_path", "manifest_schema_id",
                  "manifest_schema_version", "manifest_payload_digest",
                  "manifest_document_digest", "logical_content_digest"}) ||
      reference.value("role", "") != role ||
      reference.value("family", "") != family ||
      !SafeRunId(reference.value("run_id", "")) ||
      reference.value("run_path", "") !=
          std::string(family) + "/" + reference.value("run_id", "") ||
      reference.value("manifest_schema_id", "") !=
          manifest.value("schema_id", "") ||
      reference.value("manifest_schema_version", 0) !=
          manifest.value("schema_version", 0) ||
      reference.value("manifest_payload_digest", "") !=
          manifest.value("payload_digest", "") ||
      reference.value("manifest_document_digest", "") !=
          CanonicalJsonSha256(manifest) ||
      reference.value("logical_content_digest", "") !=
          manifest.at("payload").at("logical_content").value("digest", "")) {
    AssignError(error, "Subject-mask bundle member reference differs for " +
                           std::string(role));
    return false;
  }
  *run_id = reference.at("run_id").get<std::string>();
  return true;
}

} // namespace

bool ValidateSubjectMaskBundleV2Archive(
    const std::filesystem::path &archive_root,
    const std::string &requested_bundle,
    const std::string &expected_payload_digest,
    SubjectMaskBundleV2Summary *summary, std::string *error) {
  try {
    if (!summary || !SafeRunId(requested_bundle) ||
        !std::regex_match(expected_payload_digest,
                          std::regex("^[0-9a-f]{64}$"))) {
      AssignError(error, "Explicit bundle id, digest, and output are required");
      return false;
    }
    const auto root = ReadJson(archive_root / "zarr.json");
    if (!root || root->value("zarr_format", 0) != 3 ||
        root->value("node_type", "") != "group" ||
        !root->contains("consolidated_metadata") ||
        root->at("consolidated_metadata").value("kind", "") != "inline" ||
        root->at("consolidated_metadata").value("must_understand", true)) {
      AssignError(error,
                  "Subject-mask archive lacks exact inline Zarr v3 metadata");
      return false;
    }

    const std::string bundle_path =
        "subject_mask_bundle_runs/" + requested_bundle;
    const auto bundle_group =
        ReadJson(archive_root / bundle_path / "zarr.json");
    const auto *consolidated_bundle = ConsolidatedEntry(*root, bundle_path);
    if (!bundle_group || !consolidated_bundle ||
        !internal::EquivalentDirectAndConsolidatedZarrNode(
            *bundle_group, *consolidated_bundle) ||
        !bundle_group->contains("attributes") ||
        !bundle_group->at("attributes").contains("run_manifest")) {
      AssignError(error,
                  "Subject-mask bundle direct/consolidated metadata differs");
      return false;
    }
    const json &manifest = bundle_group->at("attributes").at("run_manifest");
    if (!ValidateManifestEnvelope(
            manifest, "palette.subject_mask.bundle_manifest", 2, error) ||
        manifest.value("payload_digest", "") != expected_payload_digest) {
      AssignError(error, "Subject-mask bundle manifest digest differs");
      return false;
    }
    const auto &payload = manifest.at("payload");
    if (!ExactKeys(payload,
                   {"bundle_id", "recording_identity", "publication", "members",
                    "cross_binding", "import_receipt_digests"}) ||
        payload.value("bundle_id", "") != requested_bundle ||
        payload.value("recording_identity", "").empty()) {
      AssignError(error, "Subject-mask bundle identity is invalid");
      return false;
    }
    const auto &publication = payload.at("publication");
    if (!ExactKeys(publication, {"completion_contract", "completion_status",
                                 "stage_selector_eligible",
                                 "bundle_selector_eligible", "metadata_state",
                                 "metadata_digest_scope", "metadata_digest",
                                 "activation_policy", "activation_state"}) ||
        publication.value("completion_contract", "") !=
            "palette.zarr_run_completion.v1" ||
        publication.value("completion_status", "") != "complete" ||
        publication.value("stage_selector_eligible", true) ||
        publication.value("bundle_selector_eligible", true) ||
        publication.value("metadata_state", "") !=
            "direct_and_consolidated_validated" ||
        publication.value("metadata_digest_scope", "") !=
            "exact_bundle_group_declaration_redacting_manifest_lifecycle_and_"
            "bundle_eligibility" ||
        publication.value("activation_policy", "") !=
            "bundle_members_ready_then_single_root_authority_commit_v1" ||
        publication.value("activation_state", "") != "deferred" ||
        !IsSha256(publication.at("metadata_digest"))) {
      AssignError(error,
                  "Subject-mask bundle publication declaration is invalid");
      return false;
    }
    json normalized_bundle = *bundle_group;
    if (!NormalizeEmptyGroupConsolidation(&normalized_bundle)) {
      AssignError(error, "Subject-mask bundle group metadata is invalid");
      return false;
    }
    auto &bundle_attributes = normalized_bundle.at("attributes");
    for (const auto name :
         {"run_manifest", "status", "palette_run_completion_status",
          "palette_run_completed_at_utc",
          "subject_mask_bundle_selector_eligible"}) {
      bundle_attributes.erase(name);
    }
    if (CanonicalJsonSha256(normalized_bundle) !=
        publication.value("metadata_digest", "")) {
      AssignError(error, "Subject-mask bundle group metadata digest differs");
      return false;
    }

    const auto &members = payload.at("members");
    if (!ExactKeys(members, {"raw", "refined", "quality"})) {
      AssignError(error, "Subject-mask bundle members are not exact");
      return false;
    }
    auto read_member = [&](std::string_view role) -> std::optional<json> {
      const std::string path =
          members.at(std::string(role)).value("run_path", "");
      const auto group = ReadJson(archive_root / path / "zarr.json");
      if (!group || !group->contains("attributes") ||
          !group->at("attributes").contains("run_manifest")) {
        return std::nullopt;
      }
      return group->at("attributes").at("run_manifest");
    };
    const auto raw_manifest = read_member("raw");
    const auto refined_manifest = read_member("refined");
    const auto quality_manifest = read_member("quality");
    if (!raw_manifest || !refined_manifest || !quality_manifest ||
        !ValidateManifestEnvelope(*raw_manifest,
                                  "palette.subject_mask_core.run_manifest", 2,
                                  error) ||
        !ValidateManifestEnvelope(*refined_manifest,
                                  "palette.subject_mask_core.run_manifest", 2,
                                  error) ||
        !ValidateManifestEnvelope(*quality_manifest,
                                  "palette.subject_mask_quality.run_manifest",
                                  2, error) ||
        !ValidateLogicalContent(*raw_manifest, error) ||
        !ValidateLogicalContent(*refined_manifest, error) ||
        !ValidateLogicalContent(*quality_manifest, error)) {
      return false;
    }

    SubjectMaskBundleV2Summary parsed;
    parsed.bundle_id = requested_bundle;
    parsed.payload_digest = expected_payload_digest;
    parsed.recording_identity =
        payload.at("recording_identity").get<std::string>();
    if (!ValidateMemberReference(members.at("raw"), "raw", "subject_mask_runs",
                                 *raw_manifest, &parsed.raw_run, error) ||
        !ValidateMemberReference(
            members.at("refined"), "refined", "refined_subject_masks_runs",
            *refined_manifest, &parsed.refined_run, error) ||
        !ValidateMemberReference(members.at("quality"), "quality",
                                 "subject_mask_quality_runs", *quality_manifest,
                                 &parsed.quality_run, error)) {
      return false;
    }

    const auto &raw_payload = raw_manifest->at("payload");
    const auto &refined_payload = refined_manifest->at("payload");
    const auto &quality_payload = quality_manifest->at("payload");
    if (raw_payload.value("run_id", "") != parsed.raw_run ||
        raw_payload.value("stage_family", "") != "subject_mask_runs" ||
        raw_payload.value("kind", "") != "raw_probability_uint8" ||
        refined_payload.value("run_id", "") != parsed.refined_run ||
        refined_payload.value("stage_family", "") !=
            "refined_subject_masks_runs" ||
        refined_payload.value("kind", "") != "refined_dense_core" ||
        quality_payload.value("run_id", "") != parsed.quality_run ||
        quality_payload.value("stage", "") != "subject_mask_quality") {
      AssignError(error, "Subject-mask member identity is invalid");
      return false;
    }

    const auto &raw_schema = raw_payload.at("logical_schema");
    const auto &refined_schema = refined_payload.at("logical_schema");
    const auto &quality_schema = quality_payload.at("logical_schema");
    size_t raw_frames = 0, raw_rows = 0, raw_height = 0, raw_width = 0;
    if (raw_schema.value("schema_id", "") !=
            "palette.stage.subject_mask_probabilities_uint8" ||
        raw_schema.value("schema_version", 0) != 1 ||
        !Dimensions(raw_schema.at("dimensions"), 3, &raw_frames, &raw_rows,
                    &raw_height, &raw_width) ||
        !Components(raw_schema.at("components"),
                    {"subject_body", "eyes_union", "swim_bladder"})) {
      AssignError(error, "Raw subject-mask logical schema is invalid");
      return false;
    }
    size_t refined_frames = 0, refined_rows = 0, refined_height = 0,
           refined_width = 0;
    if (refined_schema.value("schema_id", "") !=
            "palette.stage.refined_subject_mask_dense_core" ||
        refined_schema.value("schema_version", 0) != 1 ||
        !Dimensions(refined_schema.at("dimensions"), 4, &refined_frames,
                    &refined_rows, &refined_height, &refined_width) ||
        !Components(
            refined_schema.at("components"),
            {"subject_body", "eye_left", "eye_right", "swim_bladder"}) ||
        raw_frames != refined_frames || raw_rows != refined_rows ||
        raw_height != refined_height || raw_width != refined_width) {
      AssignError(error, "Refined subject-mask logical schema is invalid");
      return false;
    }
    const auto raw_declarations =
        CoreDeclarations(raw_frames, raw_rows, 3, raw_height, raw_width, false);
    const auto refined_declarations = CoreDeclarations(
        refined_frames, refined_rows, 4, refined_height, refined_width, true);
    const auto quality_declarations =
        QualityDeclarations(refined_frames, refined_rows, 4);
    if (!ValidateArrayInventory(*raw_manifest, raw_declarations, error) ||
        !ValidateArrayInventory(*refined_manifest, refined_declarations,
                                error) ||
        !ValidateArrayInventory(*quality_manifest, quality_declarations,
                                error) ||
        quality_schema.value("schema_id", "") !=
            "palette.stage.subject_mask_quality" ||
        quality_schema.value("schema_version", 0) != 2 ||
        quality_schema.at("dimensions").value("n_frames", 0U) !=
            refined_frames ||
        quality_schema.at("dimensions").value("n_rois", 0U) != refined_rows ||
        quality_schema.at("dimensions").value("n_channels", 0U) != 4U ||
        quality_schema.at("dimensions").value("n_component_metrics", 0U) !=
            8U ||
        quality_schema.at("dimensions").value("n_observation_metrics", 0U) !=
            7U ||
        !Components(
            quality_schema.at("components"),
            {"subject_body", "eye_left", "eye_right", "swim_bladder"})) {
      AssignError(error, "Subject-mask member declarations are invalid");
      return false;
    }

    const auto &cross = payload.at("cross_binding");
    if (!ExactKeys(cross,
                   {"dimensions", "components", "component_registry_digest",
                    "raw_refined_identity_array_values_sha256",
                    "quality_source_array_values_sha256",
                    "quality_source_manifest_digest", "identity_policy",
                    "raw_dimensions", "raw_components",
                    "raw_component_registry_digest",
                    "component_registry_policy"}) ||
        cross.at("dimensions") != refined_schema.at("dimensions") ||
        cross.at("components") != refined_schema.at("components") ||
        cross.at("raw_dimensions") != raw_schema.at("dimensions") ||
        cross.at("raw_components") != raw_schema.at("components") ||
        cross.value("component_registry_digest", "") !=
            CanonicalJsonSha256(refined_schema.at("components")) ||
        cross.value("raw_component_registry_digest", "") !=
            CanonicalJsonSha256(raw_schema.at("components")) ||
        cross.value("identity_policy", "") !=
            "exact_logical_array_hash_equality_v1" ||
        cross.value("component_registry_policy", "") !=
            "raw_and_refined_bound_independently_v1") {
      AssignError(error, "Subject-mask bundle cross-binding is invalid");
      return false;
    }
    const auto &identity = cross.at("raw_refined_identity_array_values_sha256");
    if (identity.size() != kSharedIdentityPaths.size()) {
      AssignError(error, "Subject-mask shared identity hashes are not exact");
      return false;
    }
    for (const auto path : kSharedIdentityPaths) {
      const std::string raw_hash = ArrayHash(*raw_manifest, path);
      if (raw_hash.empty() || raw_hash != ArrayHash(*refined_manifest, path) ||
          identity.value(std::string(path), "") != raw_hash) {
        AssignError(error, "Subject-mask raw/refined identity differs at " +
                               std::string(path));
        return false;
      }
    }
    const auto &quality_source =
        quality_payload.at("source_refined_subject_mask_snapshot");
    const auto &quality_hashes = cross.at("quality_source_array_values_sha256");
    if (cross.value("quality_source_manifest_digest", "") !=
            CanonicalJsonSha256(*refined_manifest) ||
        quality_source.value("run_name", "") != parsed.refined_run ||
        quality_source.value("run_path", "") !=
            "refined_subject_masks_runs/" + parsed.refined_run ||
        quality_source.value("manifest_digest", "") !=
            CanonicalJsonSha256(*refined_manifest) ||
        quality_source.value("dense_array_values_sha256", "") !=
            ArrayHash(*refined_manifest, "masks_roi") ||
        quality_hashes.size() != kQualitySourcePaths.size()) {
      AssignError(error, "Subject-mask quality source binding is invalid");
      return false;
    }
    for (const auto path : kQualitySourcePaths) {
      const std::string refined_hash = ArrayHash(*refined_manifest, path);
      if (quality_hashes.value(std::string(path), "") != refined_hash ||
          quality_source.at("source_array_values_sha256")
                  .value(std::string(path), "") != refined_hash) {
        AssignError(error, "Subject-mask quality binding differs at " +
                               std::string(path));
        return false;
      }
    }

    const auto core_digest = [](const json &member) {
      return member.at("payload")
          .at("publication")
          .value("metadata_digest", "");
    };
    if (!ValidateMemberMetadata(
            archive_root, *root, members.at("raw").at("run_path"),
            raw_declarations, core_digest(*raw_manifest), false, error) ||
        !ValidateMemberMetadata(archive_root, *root,
                                members.at("refined").at("run_path"),
                                refined_declarations,
                                core_digest(*refined_manifest), false, error) ||
        !ValidateMemberMetadata(archive_root, *root,
                                members.at("quality").at("run_path"),
                                quality_declarations,
                                quality_payload.at("publication")
                                    .value("metadata_declarations_digest", ""),
                                true, error)) {
      return false;
    }

    const auto &receipts = payload.at("import_receipt_digests");
    if (!ExactKeys(receipts, {"raw", "refined", "quality"}) ||
        !IsSha256(receipts.at("raw")) || !IsSha256(receipts.at("refined")) ||
        !IsSha256(receipts.at("quality"))) {
      AssignError(error, "Subject-mask import receipt digests are invalid");
      return false;
    }

    parsed.frame_count = refined_frames;
    parsed.row_count = refined_rows;
    parsed.raw_channel_count = 3;
    parsed.refined_channel_count = 4;
    parsed.mask_height = refined_height;
    parsed.mask_width = refined_width;
    parsed.raw_component_labels = {"subject_body", "eyes_union",
                                   "swim_bladder"};
    parsed.refined_component_labels = {"subject_body", "eye_left", "eye_right",
                                       "swim_bladder"};
    parsed.validated_array_declarations = raw_declarations.size() +
                                          refined_declarations.size() +
                                          quality_declarations.size();
    parsed.selector_eligible = false;
    parsed.activation_deferred = true;
    parsed.quality_payload_opened = false;
    *summary = std::move(parsed);
    return true;
  } catch (const json::exception &) {
    AssignError(error, "Subject-mask bundle contains invalid JSON types");
    return false;
  }
}

} // namespace crimson::zarr
