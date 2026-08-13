#include "zarr/subject_mask_sampled_contour_v1_contract.h"

#include "zarr/canonical_json.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace crimson::zarr {
namespace {

using json = nlohmann::json;

constexpr std::array<std::string_view, 4> kComponents = {
    "subject_body", "eye_left", "eye_right", "swim_bladder"};
constexpr std::array<size_t, 4> kSampleCounts = {128, 64, 64, 32};
constexpr std::array<std::string_view, 5> kIdentityArrays = {
    "instance_key", "source_crop_row_ids", "source_acquisition_frame_index",
    "frame_row_offsets", "source_crop_xywh"};

void AssignError(std::string *error, std::string value) {
  if (error) {
    *error = std::move(value);
  }
}

bool ExactKeys(const json &value,
               std::initializer_list<std::string_view> expected) {
  if (!value.is_object() || value.size() != expected.size()) {
    return false;
  }
  return std::all_of(expected.begin(), expected.end(), [&](const auto key) {
    return value.contains(std::string(key));
  });
}

bool Sha256Value(const json &value) {
  return value.is_string() && IsLowerSha256(value.get<std::string>());
}

json ExpectedComponents() {
  return {{"schema_id", "palette.subject_mask.component_registry"},
          {"schema_version", 1},
          {"labels", kComponents},
          {"channel_axis", 1},
          {"ordering", "persisted_exact_order"}};
}

json ExpectedDimensions(const SubjectMaskV1ManifestSummary &source) {
  return {{"n_frames", source.frame_count},
          {"n_frame_boundaries", source.frame_count + 1},
          {"n_instances", source.row_count},
          {"n_rois", source.row_count},
          {"n_channels", source.channel_count},
          {"H", source.mask_height},
          {"W", source.mask_width},
          {"roi_height", source.mask_height},
          {"roi_width", source.mask_width}};
}

json ExpectedContourProfile() {
  return {{"schema_id", "palette.refined_subject_mask.contour_cache_profile"},
          {"schema_version", 1},
          {"profile_id", "palette_subject_mask_sampled_contours_default"},
          {"profile_version", 1},
          {"authority", "derived_from_dense_masks_roi"},
          {"default_cache",
           {{"kind", "sampled_contours"},
            {"required_for_profile", true},
            {"component_sample_counts",
             {{"subject_body", 128},
              {"eye_left", 64},
              {"eye_right", 64},
              {"swim_bladder", 32}}},
            {"source_contour_method", "largest_external_contour"},
            {"source_contour_method_version", 2},
            {"boundary_policy", "external_only"},
            {"coordinate_space", "roi_pixels"},
            {"point_order", "xy"},
            {"sampling_method", "closed_arc_length_uniform"},
            {"sampling_method_version", 2},
            {"point_canonicalization",
             "clockwise_in_roi_y_down_topmost_then_leftmost_v1"},
            {"winding", "clockwise_in_roi_y_down"},
            {"start_point", "topmost_then_leftmost_vertex"},
            {"duplicate_closing_point", false},
            {"invalid_row_encoding", "all_nan_points_valid_false"}}},
          {"full_contours",
           {{"kind", "contours"},
            {"required_for_profile", false},
            {"role", "optional_cold_inspection_or_export_cache"}}},
          {"freshness", "receipt_bound_full_dense_equivalence"}};
}

std::vector<SubjectMaskSampledContourV1ArrayDeclaration>
ExpectedArrays(size_t rows) {
  std::vector<SubjectMaskSampledContourV1ArrayDeclaration> arrays;
  arrays.reserve(kComponents.size() * 3);
  for (size_t index = 0; index < kComponents.size(); ++index) {
    const std::string component(kComponents[index]);
    const size_t count = kSampleCounts[index];
    const std::string prefix = "components/" + component + "/sampled_contours/";
    arrays.push_back({prefix + "points_xy",
                      "float32",
                      {rows, count, 2},
                      component,
                      "points_xy",
                      count});
    arrays.push_back(
        {prefix + "valid", "bool", {rows}, component, "valid", count});
    arrays.push_back({prefix + "source_point_count",
                      "int32",
                      {rows},
                      component,
                      "source_point_count",
                      count});
  }
  return arrays;
}

const json *SourceArrays(const json &source_manifest) {
  try {
    return &source_manifest.at("payload")
                .at("logical_content")
                .at("document")
                .at("arrays");
  } catch (const json::exception &) {
    return nullptr;
  }
}

bool ValidateSourceBinding(const json &binding, const json &source_manifest,
                           const SubjectMaskV1ManifestSummary &source,
                           int cache_manifest_version,
                           bool composable_dense_identity, std::string *error) {
  const json *arrays = SourceArrays(source_manifest);
  const bool exact_binding =
      composable_dense_identity
          ? ExactKeys(
                binding,
                {"stage", "run_name", "run_path", "manifest_schema_id",
                 "manifest_schema_version", "manifest_payload_digest",
                 "manifest_document_digest", "component_registry_digest",
                 "row_identity_array_values_sha256", "authority",
                 "dense_identity_kind", "dense_array_logical_identity_digest",
                 "dense_array_logical_identity", "worker_assembly_digest"})
          : ExactKeys(binding,
                      {"stage", "run_name", "run_path", "manifest_schema_id",
                       "manifest_schema_version", "manifest_payload_digest",
                       "manifest_document_digest", "dense_array_values_sha256",
                       "component_registry_digest",
                       "row_identity_array_values_sha256", "authority"});
  if (!arrays || !exact_binding) {
    AssignError(error, "Sampled-contour source binding is malformed");
    return false;
  }
  json identity = json::object();
  for (const auto path : kIdentityArrays) {
    const auto found = arrays->find(std::string(path));
    if (found == arrays->end() || !found->is_object() ||
        !Sha256Value(found->at("sha256"))) {
      AssignError(error, "Dense source identity digest is unavailable");
      return false;
    }
    identity[std::string(path)] = found->at("sha256");
  }
  const auto masks = arrays->find("masks_roi");
  if (masks == arrays->end() || !masks->is_object()) {
    AssignError(error, "Dense source mask digest is unavailable");
    return false;
  }
  const auto &components =
      source_manifest.at("payload").at("logical_schema").at("components");
  json expected = {
      {"stage", "refined_subject_masks"},
      {"run_name", source.run_id},
      {"run_path", "refined_subject_masks_runs/" + source.run_id},
      {"manifest_schema_id", "palette.subject_mask_core.run_manifest"},
      {"manifest_schema_version", source.manifest_schema_version},
      {"manifest_payload_digest", source.payload_digest},
      {"manifest_document_digest", CanonicalJsonSha256(source_manifest)},
      {"component_registry_digest", CanonicalJsonSha256(components)},
      {"row_identity_array_values_sha256", identity},
      {"authority", "dense_masks_roi"}};
  if (composable_dense_identity) {
    expected["dense_identity_kind"] = "composable_logical_units_v1";
    expected["dense_array_logical_identity_digest"] =
        CanonicalJsonSha256(*masks);
    expected["dense_array_logical_identity"] = *masks;
    expected["worker_assembly_digest"] = binding.at("worker_assembly_digest");
    if (!Sha256Value(binding.at("worker_assembly_digest"))) {
      AssignError(error, "Sampled-contour worker assembly digest is invalid");
      return false;
    }
  } else {
    const auto &declared_dense_sha = binding.at("dense_array_values_sha256");
    if (!Sha256Value(declared_dense_sha) ||
        (cache_manifest_version == 1 &&
         (!masks->contains("sha256") || !Sha256Value(masks->at("sha256")) ||
          masks->at("sha256") != declared_dense_sha))) {
      AssignError(error, "Dense source mask digest is invalid");
      return false;
    }
    expected["dense_array_values_sha256"] = declared_dense_sha;
  }
  if (binding != expected) {
    AssignError(error,
                "Sampled-contour cache is bound to a different dense source");
    return false;
  }
  return true;
}

bool ValidateLogicalContent(
    const json &content,
    const std::vector<SubjectMaskSampledContourV1ArrayDeclaration> &expected,
    std::string *digest, std::string *error) {
  if (!ExactKeys(content, {"digest_algorithm", "document", "digest"}) ||
      content.value("digest_algorithm", "") != "sha256_canonical_json_v1" ||
      !Sha256Value(content.at("digest")) ||
      CanonicalJsonSha256(content.at("document")) !=
          content.value("digest", "")) {
    AssignError(error, "Sampled-contour logical-content digest is invalid");
    return false;
  }
  const auto &document = content.at("document");
  if (!ExactKeys(document, {"schema_id", "schema_version", "arrays"}) ||
      document.value("schema_id", "") !=
          "palette.subject_mask.sampled_contour_logical_content" ||
      document.value("schema_version", 0) != 1 ||
      !document.at("arrays").is_object() ||
      document.at("arrays").size() != expected.size()) {
    AssignError(error, "Sampled-contour array inventory is not exact");
    return false;
  }
  for (const auto &declaration : expected) {
    const auto found = document.at("arrays").find(declaration.path);
    if (found == document.at("arrays").end() ||
        !ExactKeys(*found, {"shape", "dtype", "digest_algorithm", "sha256"}) ||
        found->value("dtype", "") != declaration.dtype ||
        found->value("digest_algorithm", "") !=
            "sha256_c_contiguous_logical_values_v1" ||
        found->at("shape").get<std::vector<size_t>>() != declaration.shape ||
        !Sha256Value(found->at("sha256"))) {
      AssignError(error, "Invalid sampled-contour array declaration: " +
                             declaration.path);
      return false;
    }
  }
  *digest = content.at("digest").get<std::string>();
  return true;
}

bool ValidateStoragePlan(
    const json &storage, const json &dimensions, const json &components,
    const json &profile,
    const std::vector<SubjectMaskSampledContourV1ArrayDeclaration> &arrays,
    std::string *error) {
  if (!storage.is_object() ||
      storage.value("schema_id", "") !=
          "palette.stage_storage.subject_mask_sampled_contours" ||
      storage.value("schema_version", 0) != 1 ||
      storage.value("stage_kind", "") != "sampled_contour_display_cache" ||
      storage.value("dimensions", json{}) != dimensions ||
      storage.value("components", json{}) != components ||
      storage.value("contour_profile", json{}) != profile ||
      !storage.contains("storage_profile") ||
      storage.at("storage_profile").value("profile_id", "") !=
          "subject_mask_presentation_candidate_v1" ||
      storage.at("storage_profile").value("target_chunk_bytes", 0) != 131072 ||
      storage.at("storage_profile").value("target_shard_bytes", 0) != 8388608 ||
      !storage.contains("arrays") || !storage.at("arrays").is_array() ||
      storage.at("arrays").size() != arrays.size()) {
    AssignError(error, "Sampled-contour storage plan is invalid");
    return false;
  }
  std::map<std::string, const SubjectMaskSampledContourV1ArrayDeclaration *>
      expected;
  for (const auto &array : arrays) {
    expected[array.path] = &array;
  }
  std::set<std::string> observed;
  for (const auto &entry : storage.at("arrays")) {
    const std::string path = entry.value("path", "");
    const auto found = expected.find(path);
    if (found == expected.end() || !observed.insert(path).second ||
        entry.value("component", "") != found->second->component ||
        entry.value("field", "") != found->second->field ||
        entry.value("sample_count", size_t{0}) != found->second->sample_count ||
        !entry.contains("plan") ||
        entry.at("plan").value("logical_dtype", "") != found->second->dtype ||
        entry.at("plan").value("logical_shape", json{}) !=
            found->second->shape ||
        entry.at("plan").value("profile_id", "") !=
            "subject_mask_presentation_candidate_v1" ||
        entry.at("plan").value("codec_profile_id", "") != "zstd_fast_v1" ||
        entry.at("plan").value("chunk_nbytes", 0) != 131072 ||
        entry.at("plan").value("write_mode", "") != "immutable") {
      AssignError(error, "Sampled-contour physical array plan differs");
      return false;
    }
  }
  return true;
}

bool ValidateReceipts(
    const json &extension, const json &source_binding, const json &logical,
    const std::vector<SubjectMaskSampledContourV1ArrayDeclaration> &arrays,
    bool composable_dense_identity, std::string *error) {
  if (!ExactKeys(extension,
                 {"schema_id", "schema_version", "authority", "freshness_rule",
                  "receipts", "receipts_digest"}) ||
      extension.value("schema_id", "") !=
          "palette.stage.refined_subject_mask.published_cache_extension" ||
      extension.value("schema_version", 0) != 1 ||
      extension.value("authority", "") != "non_authoritative_derived_caches" ||
      !extension.at("receipts").is_array() ||
      extension.at("receipts").size() != kComponents.size() ||
      !Sha256Value(extension.at("receipts_digest")) ||
      CanonicalJsonSha256(extension.at("receipts")) !=
          extension.value("receipts_digest", "")) {
    AssignError(error, "Sampled-contour cache receipts are invalid");
    return false;
  }
  const auto &logical_arrays = logical.at("document").at("arrays");
  std::set<std::string> observed;
  for (const auto &receipt : extension.at("receipts")) {
    if (!ExactKeys(receipt, {"schema_id", "schema_version", "digest_algorithm",
                             "payload", "payload_digest"}) ||
        receipt.value("schema_id", "") !=
            "palette.refined_subject_mask.derived_cache_receipt" ||
        receipt.value("schema_version", 0) !=
            (composable_dense_identity ? 2 : 1) ||
        receipt.value("digest_algorithm", "") != "sha256_canonical_json_v1" ||
        CanonicalJsonSha256(receipt.at("payload")) !=
            receipt.value("payload_digest", "")) {
      AssignError(error, "Sampled-contour component receipt is invalid");
      return false;
    }
    const auto &payload = receipt.at("payload");
    const std::string path = payload.value("cache_path", "");
    if (payload.value("cache_kind", "") != "sampled_contours" ||
        !observed.insert(path).second || payload.value("stale", true) ||
        payload.value("authoritative_pixels", true) ||
        payload.at("validation").value("mode", "") !=
            (composable_dense_identity
                 ? "receipt_bound_complete_dense_equivalence_v2"
                 : "full_dense_equivalence") ||
        payload.at("validation").value("status", "") != "passed" ||
        payload.at("generator") !=
            json{{"id", "palette_subject_mask_sampled_contours"},
                 {"version", 1}}) {
      AssignError(error, "Sampled-contour component receipt is not usable");
      return false;
    }
    const auto component_pos = path.find("components/");
    const auto suffix_pos = path.rfind("/sampled_contours");
    if (component_pos != 0 || suffix_pos == std::string::npos) {
      AssignError(error, "Sampled-contour receipt path is invalid");
      return false;
    }
    const json component_content = {
        {"points_xy", logical_arrays.at(path + "/points_xy")},
        {"valid", logical_arrays.at(path + "/valid")},
        {"source_point_count",
         logical_arrays.at(path + "/source_point_count")}};
    const json &receipt_source = payload.at("source");
    const bool source_matches =
        receipt_source.value("dense_core_manifest_digest", "") ==
            source_binding.value("manifest_document_digest", "") &&
        receipt_source.value("component_registry_digest", "") ==
            source_binding.value("component_registry_digest", "") &&
        payload.value("logical_content_digest", "") ==
            CanonicalJsonSha256(component_content) &&
        (composable_dense_identity
             ? (receipt_source.value("dense_identity_kind", "") ==
                    "composable_logical_units_v1" &&
                receipt_source.value("dense_array_logical_identity_digest",
                                     "") ==
                    source_binding.value("dense_array_logical_identity_digest",
                                         ""))
             : receipt_source.value("dense_array_values_sha256", "") ==
                   source_binding.value("dense_array_values_sha256", ""));
    if (!source_matches) {
      AssignError(error, "Sampled-contour receipt source binding differs");
      return false;
    }
  }
  return observed.size() == kComponents.size();
}

} // namespace

bool ValidateSubjectMaskSampledContourV1Manifest(
    const json &manifest, std::string_view requested_run,
    const json &source_manifest, const SubjectMaskV1ManifestSummary &source,
    SubjectMaskSampledContourV1Summary *summary, std::string *error) {
  try {
    if (!summary ||
        !ExactKeys(manifest, {"schema_id", "schema_version", "digest_algorithm",
                              "payload_digest", "payload"}) ||
        manifest.value("schema_id", "") !=
            "palette.subject_mask.derived_cache_run_manifest" ||
        (manifest.value("schema_version", 0) != 1 &&
         manifest.value("schema_version", 0) != 2 &&
         manifest.value("schema_version", 0) != 3) ||
        manifest.value("digest_algorithm", "") != "sha256_canonical_json_v1" ||
        !Sha256Value(manifest.at("payload_digest")) ||
        CanonicalJsonSha256(manifest.at("payload")) !=
            manifest.value("payload_digest", "")) {
      AssignError(error, "Sampled-contour run_manifest envelope is invalid");
      return false;
    }
    if (source.component_labels !=
            std::vector<std::string>(kComponents.begin(), kComponents.end()) ||
        source.channel_count != kComponents.size()) {
      AssignError(error,
                  "Selected dense mask component registry is unsupported");
      return false;
    }

    const auto &payload = manifest.at("payload");
    const bool composable_dense_identity =
        manifest.value("schema_version", 0) == 3;
    if (!ExactKeys(payload,
                   {"run_id", "stage_family", "kind", "publication",
                    "dimensions", "components", "contour_profile",
                    "source_refined_subject_mask_snapshot", "storage_plan",
                    "logical_content", "cache_extension", "write_receipt"}) ||
        payload.value("run_id", "") != requested_run ||
        payload.value("stage_family", "") != "subject_mask_cache_runs" ||
        payload.value("kind", "") != "sampled_contour_display_cache") {
      AssignError(error, "Sampled-contour manifest identity is invalid");
      return false;
    }
    const auto &publication = payload.at("publication");
    if (!ExactKeys(publication, {"completion_contract", "completion_status",
                                 "stage_selector_eligible", "metadata_state",
                                 "metadata_digest_scope", "metadata_digest"}) ||
        publication.value("completion_contract", "") !=
            "palette.zarr_run_completion.v1" ||
        publication.value("completion_status", "") != "complete" ||
        publication.value("stage_selector_eligible", true) ||
        publication.value("metadata_state", "") !=
            "direct_and_consolidated_validated" ||
        publication.value("metadata_digest_scope", "") !=
            "exact_run_subtree_declarations_redacting_manifest_and_lifecycle_"
            "v1" ||
        !Sha256Value(publication.at("metadata_digest"))) {
      AssignError(error, "Sampled-contour publication state is invalid");
      return false;
    }
    const json dimensions = ExpectedDimensions(source);
    const json components = ExpectedComponents();
    const json profile = ExpectedContourProfile();
    if (payload.at("dimensions") != dimensions ||
        payload.at("components") != components ||
        payload.at("contour_profile") != profile) {
      AssignError(error, "Sampled-contour dimensions or profile differ");
      return false;
    }
    if (!ValidateSourceBinding(
            payload.at("source_refined_subject_mask_snapshot"), source_manifest,
            source, manifest.value("schema_version", 0),
            composable_dense_identity, error)) {
      return false;
    }
    const auto arrays = ExpectedArrays(source.row_count);
    std::string logical_digest;
    if (!ValidateLogicalContent(payload.at("logical_content"), arrays,
                                &logical_digest, error) ||
        !ValidateStoragePlan(payload.at("storage_plan"), dimensions, components,
                             profile, arrays, error) ||
        !ValidateReceipts(payload.at("cache_extension"),
                          payload.at("source_refined_subject_mask_snapshot"),
                          payload.at("logical_content"), arrays,
                          composable_dense_identity, error)) {
      return false;
    }
    const auto &write_receipt = payload.at("write_receipt");
    if (!write_receipt.is_object() ||
        write_receipt.value("full_dense_equivalence", false) != true ||
        write_receipt.value("publication", "") !=
            "one_process_owns_every_complete_output_shard" ||
        !write_receipt.contains("physical_write_counts") ||
        !write_receipt.contains("bounded_reopen_samples") ||
        write_receipt.at("physical_write_counts").size() != arrays.size() ||
        write_receipt.at("bounded_reopen_samples").size() != arrays.size()) {
      AssignError(error, "Sampled-contour write receipt is invalid");
      return false;
    }

    SubjectMaskSampledContourV1Summary parsed;
    parsed.run_id = std::string(requested_run);
    parsed.payload_digest = manifest.at("payload_digest").get<std::string>();
    parsed.manifest_digest = CanonicalJsonSha256(manifest);
    parsed.metadata_digest =
        publication.at("metadata_digest").get<std::string>();
    parsed.logical_content_digest = std::move(logical_digest);
    parsed.source_run_id = source.run_id;
    parsed.source_manifest_payload_digest = source.payload_digest;
    parsed.source_manifest_digest = CanonicalJsonSha256(source_manifest);
    parsed.frame_count = source.frame_count;
    parsed.row_count = source.row_count;
    parsed.component_labels = source.component_labels;
    parsed.component_sample_counts.assign(kSampleCounts.begin(),
                                          kSampleCounts.end());
    parsed.arrays = arrays;
    *summary = std::move(parsed);
    return true;
  } catch (const std::exception &) {
    AssignError(error, "Sampled-contour run_manifest contains invalid types");
    return false;
  }
}

} // namespace crimson::zarr
