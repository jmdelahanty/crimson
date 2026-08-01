#include "zarr/subject_mask_v1_contract.h"

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

constexpr std::string_view kOriginalMetadataDigestScope =
    "exact_run_group_and_array_declarations_redacting_only_run_manifest";
constexpr std::string_view kMaintainedMetadataDigestScope =
    "exact_run_group_and_array_declarations_redacting_manifest_lifecycle_"
    "and_transport_publication_attrs";

struct BindingDeclaration {
  const char *path;
  const char *contract_id;
};

constexpr BindingDeclaration kBindings[] = {
    {"source_crop_row_ids", "palette.array.subject_mask.source_crop_row_ids"},
    {"instance_key", "palette.array.detection.instance_key"},
    {"source_acquisition_frame_index",
     "palette.array.detection.source_acquisition_frame_index"},
    {"frame_row_offsets", "palette.array.frame_row_offsets"},
    {"source_crop_xywh", "palette.array.crop.source_crop_xywh"},
    {"masks_roi", "palette.array.subject_masks_roi_dense"},
    {"available_channels", "palette.array.subject_mask.available_channels"},
    {"metrics/mask_present", "palette.array.subject_mask.mask_present"},
    {"metrics/area_px", "palette.array.subject_mask.area_px"},
    {"metrics/centroid_xy", "palette.array.subject_mask.centroid_xy"},
    {"metrics/centroid_valid", "palette.array.subject_mask.centroid_valid"},
    {"metrics/bbox_xyxy", "palette.array.subject_mask.bbox_xyxy"},
    {"metrics/bbox_valid", "palette.array.subject_mask.bbox_valid"},
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
  return std::all_of(expected.begin(), expected.end(), [&](const auto key) {
    return value.contains(std::string(key));
  });
}

bool positiveSize(const json &value, const char *key, size_t *output) {
  const auto found = value.find(key);
  if (found == value.end() || !found->is_number_integer()) {
    return false;
  }
  const int64_t parsed = found->get<int64_t>();
  if (parsed <= 0 ||
      static_cast<uint64_t>(parsed) > std::numeric_limits<size_t>::max()) {
    return false;
  }
  *output = static_cast<size_t>(parsed);
  return true;
}

json expectedBindings() {
  json result = json::array();
  for (const auto &binding : kBindings) {
    result.push_back({{"path", binding.path},
                      {"contract_id", binding.contract_id},
                      {"contract_version", 1},
                      {"required", true}});
  }
  return result;
}

bool validateDimensions(const json &dimensions,
                        SubjectMaskV1ManifestSummary *summary) {
  size_t frame_boundaries = 0;
  size_t rois = 0;
  if (!exactKeys(dimensions,
                 {"n_frames", "n_frame_boundaries", "n_instances", "n_rois",
                  "n_channels", "H", "W", "roi_height", "roi_width"}) ||
      !positiveSize(dimensions, "n_frames", &summary->frame_count) ||
      !positiveSize(dimensions, "n_frame_boundaries", &frame_boundaries) ||
      !positiveSize(dimensions, "n_instances", &summary->row_count) ||
      !positiveSize(dimensions, "n_rois", &rois) ||
      !positiveSize(dimensions, "n_channels", &summary->channel_count) ||
      !positiveSize(dimensions, "H", &summary->mask_height) ||
      !positiveSize(dimensions, "W", &summary->mask_width) ||
      dimensions.value("roi_height", size_t{0}) != summary->mask_height ||
      dimensions.value("roi_width", size_t{0}) != summary->mask_width ||
      frame_boundaries != summary->frame_count + 1 ||
      rois != summary->row_count) {
    return false;
  }
  return true;
}

bool validateComponents(const json &components,
                        SubjectMaskV1ManifestSummary *summary) {
  if (!exactKeys(components, {"schema_id", "schema_version", "labels",
                              "channel_axis", "ordering"}) ||
      components.value("schema_id", "") !=
          "palette.subject_mask.component_registry" ||
      components.value("schema_version", 0) != 1 ||
      components.value("channel_axis", -1) != 1 ||
      components.value("ordering", "") != "persisted_exact_order" ||
      !components.at("labels").is_array() ||
      components.at("labels").size() != summary->channel_count) {
    return false;
  }
  std::unordered_set<std::string> unique;
  for (const auto &label : components.at("labels")) {
    if (!label.is_string() || label.get_ref<const std::string &>().empty() ||
        !unique.insert(label.get<std::string>()).second) {
      return false;
    }
    summary->component_labels.push_back(label.get<std::string>());
  }
  return true;
}

bool validateLogicalSchema(const json &schema,
                           SubjectMaskV1ManifestSummary *summary) {
  if (!exactKeys(schema,
                 {"schema_id", "schema_version", "layout", "dimensions",
                  "components", "authority", "bindings", "invariants"}) ||
      schema.value("schema_id", "") !=
          "palette.stage.refined_subject_mask_dense_core" ||
      schema.value("schema_version", 0) != 1 ||
      schema.value("layout", "") !=
          "recording_observations_with_frame_row_offsets_v1" ||
      schema.value("authority", "") != "dense_binary_masks_roi" ||
      schema.at("bindings") != expectedBindings() ||
      !validateDimensions(schema.at("dimensions"), summary) ||
      !validateComponents(schema.at("components"), summary)) {
    return false;
  }
  const auto &invariants = schema.at("invariants");
  return exactKeys(invariants,
                   {"instances_per_frame", "frame_index", "row_order",
                    "crop_contract", "derived_surfaces", "legacy_aliases"}) &&
         invariants.value("instances_per_frame", "") == "zero_one_or_many" &&
         invariants.value("frame_index", "") ==
             "retained_int64_f_plus_one_offsets" &&
         invariants.value("row_order", "") ==
             "nondecreasing_source_acquisition_frame_index" &&
         invariants.value("crop_contract", "") ==
             "palette.stage.crop_geometry_v1_float32_placement" &&
         invariants.value("derived_surfaces", "") ==
             "must_exactly_match_dense_authority" &&
         invariants.value("legacy_aliases", "") == "forbidden";
}

bool validateLogicalContent(const json &content,
                            const SubjectMaskV1ManifestSummary &summary) {
  if (!exactKeys(content, {"digest_algorithm", "digest", "document"}) ||
      content.value("digest_algorithm", "") != "sha256_canonical_json_v1" ||
      !IsLowerSha256(content.value("digest", "")) ||
      CanonicalJsonSha256(content.at("document")) !=
          content.value("digest", "")) {
    return false;
  }
  const auto &document = content.at("document");
  if (!exactKeys(document, {"schema_id", "schema_version", "kind", "dimensions",
                            "components", "arrays"}) ||
      document.value("schema_id", "") !=
          "palette.subject_mask_core.logical_content" ||
      document.value("schema_version", 0) != 1 ||
      document.value("kind", "") != "refined_dense_core" ||
      !document.at("arrays").is_object() ||
      document.at("arrays").size() != kSubjectMaskV1ArrayDeclarations.size()) {
    return false;
  }
  const auto &dimensions = document.at("dimensions");
  const auto &components = document.at("components");
  if (dimensions.value("n_frames", size_t{0}) != summary.frame_count ||
      dimensions.value("n_frame_boundaries", size_t{0}) !=
          summary.frame_count + 1 ||
      dimensions.value("n_instances", size_t{0}) != summary.row_count ||
      dimensions.value("n_rois", size_t{0}) != summary.row_count ||
      dimensions.value("n_channels", size_t{0}) != summary.channel_count ||
      dimensions.value("H", size_t{0}) != summary.mask_height ||
      dimensions.value("W", size_t{0}) != summary.mask_width ||
      dimensions.value("roi_height", size_t{0}) != summary.mask_height ||
      dimensions.value("roi_width", size_t{0}) != summary.mask_width ||
      components.value("schema_id", "") !=
          "palette.subject_mask.component_registry" ||
      components.value("schema_version", 0) != 1 ||
      components.value("channel_axis", -1) != 1 ||
      components.value("ordering", "") != "persisted_exact_order" ||
      !components.contains("labels") ||
      components.at("labels") != summary.component_labels) {
    return false;
  }
  for (const auto &declaration : kSubjectMaskV1ArrayDeclarations) {
    const auto found = document.at("arrays").find(declaration.path);
    if (found == document.at("arrays").end() ||
        !exactKeys(*found, {"shape", "dtype", "digest_algorithm", "sha256"}) ||
        found->value("dtype", "") != declaration.dtype ||
        found->value("digest_algorithm", "") !=
            "sha256_c_contiguous_bytes_v1" ||
        !IsLowerSha256(found->value("sha256", "")) ||
        found->at("shape").get<std::vector<size_t>>() !=
            ExpectedSubjectMaskV1Shape(declaration, summary)) {
      return false;
    }
  }
  return true;
}

} // namespace

std::vector<size_t>
ExpectedSubjectMaskV1Shape(const SubjectMaskV1ArrayDeclaration &declaration,
                           const SubjectMaskV1ManifestSummary &summary) {
  const auto extent = [&](SubjectMaskV1Extent value) -> size_t {
    switch (value) {
    case SubjectMaskV1Extent::Rows:
      return summary.row_count;
    case SubjectMaskV1Extent::FrameBoundaries:
      return summary.frame_count + 1;
    case SubjectMaskV1Extent::Channels:
      return summary.channel_count;
    case SubjectMaskV1Extent::Two:
      return 2;
    case SubjectMaskV1Extent::Four:
      return 4;
    case SubjectMaskV1Extent::Height:
      return summary.mask_height;
    case SubjectMaskV1Extent::Width:
      return summary.mask_width;
    }
    return 0;
  };
  std::vector<size_t> result;
  result.reserve(declaration.rank);
  for (size_t index = 0; index < declaration.rank; ++index) {
    result.push_back(extent(declaration.extents[index]));
  }
  return result;
}

bool ValidateSubjectMaskV1FrameIndex(const std::vector<int64_t> &offsets,
                                     const std::vector<int64_t> &frames,
                                     size_t frame_count, size_t row_count,
                                     std::string *error) {
  if (offsets.size() != frame_count + 1 || frames.size() != row_count ||
      offsets.empty() || offsets.front() != 0 ||
      offsets.back() != static_cast<int64_t>(row_count)) {
    assignError(error, "Subject-mask v1 frame index shape is invalid");
    return false;
  }
  for (size_t frame = 0; frame < frame_count; ++frame) {
    const int64_t first = offsets[frame];
    const int64_t last = offsets[frame + 1];
    if (first < 0 || last < first || last > static_cast<int64_t>(row_count)) {
      assignError(error, "Subject-mask v1 frame offsets are malformed");
      return false;
    }
    for (int64_t row = first; row < last; ++row) {
      if (frames[static_cast<size_t>(row)] != static_cast<int64_t>(frame)) {
        assignError(error,
                    "Subject-mask v1 offsets disagree with frame indices");
        return false;
      }
    }
  }
  return true;
}

bool ValidateSubjectMaskV1InstanceKeys(const std::vector<uint64_t> &keys,
                                       size_t row_count, std::string *error) {
  if (keys.size() != row_count) {
    assignError(error, "Subject-mask v1 instance-key shape is invalid");
    return false;
  }
  std::unordered_set<uint64_t> unique;
  unique.reserve(keys.size());
  for (const uint64_t key : keys) {
    if (!unique.insert(key).second) {
      assignError(error, "Subject-mask v1 instance keys are not unique");
      return false;
    }
  }
  return true;
}

bool ValidateSubjectMaskV1Manifest(const json &manifest,
                                   std::string_view requested_run,
                                   SubjectMaskV1ManifestSummary *summary,
                                   std::string *error) {
  try {
    if (!summary ||
        !exactKeys(manifest, {"schema_id", "schema_version", "digest_algorithm",
                              "payload_digest", "payload"}) ||
        manifest.value("schema_id", "") !=
            "palette.subject_mask_core.run_manifest" ||
        manifest.value("schema_version", 0) != 2 ||
        manifest.value("digest_algorithm", "") != "sha256_canonical_json_v1" ||
        !IsLowerSha256(manifest.value("payload_digest", "")) ||
        CanonicalJsonSha256(manifest.at("payload")) !=
            manifest.value("payload_digest", "")) {
      assignError(error, "Subject-mask v1 run_manifest envelope is invalid");
      return false;
    }

    SubjectMaskV1ManifestSummary parsed;
    parsed.payload_digest = manifest.at("payload_digest").get<std::string>();
    parsed.manifest_digest = CanonicalJsonSha256(manifest);
    const auto &payload = manifest.at("payload");
    if (!exactKeys(payload, {"run_id", "stage_family", "kind", "publication",
                             "logical_schema", "storage_plan", "source",
                             "write_receipt", "logical_content"}) ||
        payload.value("run_id", "") != requested_run ||
        payload.value("stage_family", "") != "refined_subject_masks_runs" ||
        payload.value("kind", "") != "refined_dense_core") {
      assignError(error, "Subject-mask v1 manifest identity is invalid");
      return false;
    }
    parsed.run_id = payload.at("run_id").get<std::string>();

    const auto &publication = payload.at("publication");
    const std::string metadata_digest_scope =
        publication.value("metadata_digest_scope", "");
    if (!exactKeys(publication, {"completion_contract", "completion_status",
                                 "stage_selector_eligible", "metadata_state",
                                 "metadata_digest_scope", "metadata_digest"}) ||
        publication.value("completion_contract", "") !=
            "palette.zarr_run_completion.v1" ||
        publication.value("completion_status", "") != "complete" ||
        !publication.at("stage_selector_eligible").is_boolean() ||
        publication.value("metadata_state", "") !=
            "direct_and_consolidated_validated" ||
        (metadata_digest_scope != kOriginalMetadataDigestScope &&
         metadata_digest_scope != kMaintainedMetadataDigestScope) ||
        !IsLowerSha256(publication.value("metadata_digest", ""))) {
      assignError(error, "Subject-mask v1 publication declaration is invalid");
      return false;
    }
    parsed.selector_eligible =
        publication.at("stage_selector_eligible").get<bool>();
    parsed.metadata_digest =
        publication.at("metadata_digest").get<std::string>();
    parsed.metadata_digest_scope = metadata_digest_scope;

    if (!validateLogicalSchema(payload.at("logical_schema"), &parsed) ||
        !validateLogicalContent(payload.at("logical_content"), parsed) ||
        !payload.at("storage_plan").is_object() ||
        !payload.at("source").is_object() ||
        !payload.at("write_receipt").is_object()) {
      assignError(error, "Subject-mask v1 logical contract is invalid");
      return false;
    }
    *summary = std::move(parsed);
    return true;
  } catch (const std::exception &) {
    assignError(error, "Subject-mask v1 run_manifest contains invalid types");
    return false;
  }
}

} // namespace crimson::zarr
