#include "zarr/canonical_overlay_selection.h"

#include "zarr/archive_context.h"
#include "zarr/archive_context_internal.h"
#include "zarr/canonical_json.h"
#include "zarr/keypoint_v2_contract.h"
#include "zarr/subject_mask_v1_contract.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <utility>

namespace crimson::zarr {
namespace {

using json = nlohmann::json;

constexpr std::array<std::string_view, 5> kExpectedKeypointLabels = {
    "swim_bladder", "eye_left", "eye_right", "snout_tip", "tail_tip"};
constexpr std::array<std::string_view, 4> kExpectedMaskLabels = {
    "subject_body", "eye_left", "eye_right", "swim_bladder"};

void assignError(std::string *destination, std::string message) {
  if (destination) {
    *destination = std::move(message);
  }
}

bool validRunName(std::string_view value) {
  return !value.empty() && value != "." && value != ".." &&
         value.find('/') == std::string_view::npos &&
         value.find('\\') == std::string_view::npos;
}

std::string stringValue(const json &value, std::string_view key) {
  const auto found = value.find(std::string(key));
  return found != value.end() && found->is_string()
             ? found->get<std::string>()
             : std::string{};
}

bool boolValue(const json &value, std::string_view key, bool fallback = false) {
  const auto found = value.find(std::string(key));
  return found != value.end() && found->is_boolean() ? found->get<bool>()
                                                     : fallback;
}

template <typename T>
bool exactStrings(const json &value, const T &expected) {
  if (!value.is_array() || value.size() != expected.size()) {
    return false;
  }
  for (size_t index = 0; index < expected.size(); ++index) {
    if (!value[index].is_string() ||
        value[index].get<std::string>() != expected[index]) {
      return false;
    }
  }
  return true;
}

const json *objectAt(const json &value, std::string_view key) {
  const auto found = value.find(std::string(key));
  return found != value.end() && found->is_object() ? &*found : nullptr;
}

bool getSize(const json &value, std::string_view key, size_t *output) {
  const auto found = value.find(std::string(key));
  if (found == value.end() || !found->is_number_integer()) {
    return false;
  }
  uint64_t parsed = 0;
  if (found->is_number_unsigned()) {
    parsed = found->get<uint64_t>();
  } else {
    const int64_t signed_value = found->get<int64_t>();
    if (signed_value < 0) {
      return false;
    }
    parsed = static_cast<uint64_t>(signed_value);
  }
  if (parsed > std::numeric_limits<size_t>::max()) {
    return false;
  }
  *output = static_cast<size_t>(parsed);
  return true;
}

bool digestEnvelopeValid(const json &envelope,
                         std::string_view expected_schema) {
  const auto *document = objectAt(envelope, "document");
  const std::string digest = stringValue(envelope, "digest");
  return document && stringValue(*document, "schema_id") == expected_schema &&
         stringValue(envelope, "digest_algorithm") ==
             "sha256_canonical_json_v1" &&
         IsLowerSha256(digest) && CanonicalJsonSha256(*document) == digest;
}

std::string arrayDigest(const json &manifest, std::string_view path,
                        const std::vector<size_t> &expected_shape,
                        std::string_view expected_dtype) {
  try {
    const auto &entry = manifest.at("payload")
                            .at("logical_content")
                            .at("document")
                            .at("arrays")
                            .at(std::string(path));
    if (entry.at("shape").get<std::vector<size_t>>() != expected_shape ||
        entry.value("dtype", "") != expected_dtype ||
        entry.value("digest_algorithm", "") !=
            "sha256_c_contiguous_bytes_v1" ||
        !IsLowerSha256(entry.value("sha256", ""))) {
      return {};
    }
    return entry.at("sha256").get<std::string>();
  } catch (const json::exception &) {
    return {};
  }
}

bool rawImageCoordinateContractValid(const json &manifest) {
  try {
    const auto &contract = manifest.at("payload").at("coordinate_contract");
    if (!digestEnvelopeValid(contract, "palette.array_coordinate_catalog")) {
      return false;
    }
    const auto &document = contract.at("document");
    bool binding_found = false;
    for (const auto &binding : document.at("bindings")) {
      if (binding.value("array_contract_id", "") ==
          "palette.array.keypoints_img") {
        binding_found =
            binding.value("array_contract_version", 0) == 2 &&
            binding.value("surface_id", "") ==
                "source_camera_point_xy_v1" &&
            binding.value("semantic_role", "") ==
                "exact_derived_numeric_surface";
      }
    }
    bool surface_found = false;
    for (const auto &surface : document.at("surfaces")) {
      if (surface.value("surface_id", "") ==
          "source_camera_point_xy_v1") {
        surface_found =
            surface.value("domain_id", "") == "source_camera_image_px" &&
            surface.value("geometry_type", "") == "point_xy" &&
            surface.value("pixel_convention", "") == "continuous" &&
            surface.value("source_camera_mapping", "") ==
                "direct_source_camera_continuous_pixels" &&
            surface.value("descriptor_profile_id", "") ==
                "source_camera_image_px.top_left_y_down.v1" &&
            surface.value("descriptor_overlay_status", "") == "direct" &&
            surface.at("components") == json::array({"x", "y"}) &&
            surface.at("component_units") == json::array({"px", "px"});
      }
    }
    return binding_found && surface_found;
  } catch (const json::exception &) {
    return false;
  }
}

void invalidate(CanonicalOverlayProductBinding *binding, std::string message) {
  binding->valid = false;
  binding->error = std::move(message);
}

bool validateRoot(const CanonicalOverlaySelectionDocuments &documents,
                  const CanonicalOverlaySelectionRequest &request,
                  CanonicalOverlaySelection *selection,
                  std::string *error) {
  const json &root = documents.root_attributes;
  const auto *video = objectAt(root, "source_video_metadata");
  size_t frame_count = 0;
  size_t width = 0;
  size_t height = 0;
  const std::string recording_id = stringValue(root, "recording_id");
  const std::string camera_id = stringValue(root, "camera_id");
  if (!video || recording_id.empty() || camera_id.empty() ||
      stringValue(*video, "schema_id") !=
          "palette.source_video_collection_metadata.v1" ||
      stringValue(*video, "camera_id") != camera_id ||
      !getSize(*video, "total_frames", &frame_count) || frame_count == 0 ||
      !getSize(*video, "width", &width) || width == 0 ||
      !getSize(*video, "height", &height) || height == 0 ||
      root.value("recording_frame_index_row_count", uint64_t{0}) !=
          frame_count ||
      root.value("recording_frame_id_min", int64_t{-1}) != 1 ||
      root.value("recording_frame_id_max", uint64_t{0}) != frame_count) {
    assignError(error, "Canonical overlay recording/frame authority is invalid");
    return false;
  }
  if ((!request.expected_recording_id.empty() &&
       request.expected_recording_id != recording_id) ||
      (request.expected_frame_count != 0 &&
       request.expected_frame_count != frame_count) ||
      (request.expected_source_width != 0 &&
       request.expected_source_width != width) ||
      (request.expected_source_height != 0 &&
       request.expected_source_height != height)) {
    assignError(error,
                "Canonical overlay recording/frame expectations disagree");
    return false;
  }
  selection->archive_identity = documents.archive_identity;
  selection->recording_id = recording_id;
  selection->camera_id = camera_id;
  selection->first_acquisition_frame = 0;
  selection->frame_count = frame_count;
  selection->source_width = width;
  selection->source_height = height;
  selection->coordinate_surface_id = "source_camera_point_xy_v1";
  selection->coordinate_descriptor_profile =
      "source_camera_image_px.top_left_y_down.v1";
  return true;
}

bool validateEye(const CanonicalOverlaySelectionDocuments &documents,
                 const CanonicalOverlaySelectionRequest &request,
                 CanonicalOverlaySelection *selection, std::string *error) {
  const std::string latest =
      stringValue(documents.eye_group_attributes, "latest");
  const std::string latest_complete =
      stringValue(documents.eye_group_attributes, "latest_complete");
  if (!validRunName(latest) || latest != latest_complete ||
      (!request.eye_run.empty() && request.eye_run != latest)) {
    assignError(error,
                "Eye-angle selector is invalid or disagrees with the request");
    return false;
  }
  const json &eye = documents.eye_attributes;
  const auto *array_schema = objectAt(eye, "eye_angle_array_schema");
  const auto *dimensions = array_schema ? objectAt(*array_schema, "dimensions")
                                        : nullptr;
  size_t frames = 0;
  size_t rows = 0;
  const std::string lineage = stringValue(eye, "lineage_hash");
  if (stringValue(eye, "schema_id") != "analysis.eye_angle_runs" ||
      eye.value("schema_version", 0) != 7 ||
      stringValue(eye, "palette_run_name") != latest ||
      stringValue(eye, "palette_run_completion_contract") !=
          "palette.zarr_run_completion.v1" ||
      stringValue(eye, "palette_run_completion_status") != "complete" ||
      !boolValue(eye, "stage_selector_eligible") ||
      !IsLowerSha256(lineage) ||
      stringValue(eye, "source_fingerprint") != lineage || !dimensions ||
      !getSize(*dimensions, "n_frames", &frames) ||
      !getSize(*dimensions, "n_roi_rows", &rows) ||
      frames != selection->frame_count || rows == 0) {
    assignError(error, "Selected eye-angle schema-7 authority is invalid");
    return false;
  }
  const std::string keypoint = stringValue(eye, "source_base_keypoints_run");
  const std::string mask =
      stringValue(eye, "source_refined_subject_masks_run");
  const std::string shape = stringValue(eye, "source_subject_shape_run");
  const std::string keypoint_base = "keypoints_runs/" + keypoint;
  if (!validRunName(keypoint) || !validRunName(mask) || !validRunName(shape) ||
      stringValue(eye, "source_instance_key_path") !=
          keypoint_base + "/instance_key" ||
      stringValue(eye, "source_acquisition_frame_index_path") !=
          keypoint_base + "/source_acquisition_frame_index" ||
      stringValue(eye, "source_detection_success_path") !=
          keypoint_base + "/pose_success") {
    assignError(error, "Selected eye-angle upstream bindings are invalid");
    return false;
  }
  selection->observation_count = rows;
  selection->eye.valid = true;
  selection->eye.group = "analysis/eye_angle_runs";
  selection->eye.run_id = latest;
  selection->eye.schema_id = "analysis.eye_angle_runs";
  selection->eye.schema_version = 7;
  selection->eye.identity_digest = lineage;
  selection->eye.manifest_payload_digest =
      stringValue(eye, "staged_input_integrity_receipt_sha256");
  selection->eye.selector_eligible = true;
  selection->keypoints.group = "keypoints_runs";
  selection->keypoints.run_id = keypoint;
  selection->mask.group = "refined_subject_masks_runs";
  selection->mask.run_id = mask;
  selection->shape.group = "analysis/subject_shape_runs";
  selection->shape.run_id = shape;
  return true;
}

void validateKeypoints(const json &attributes,
                       CanonicalOverlaySelection *selection) {
  auto &binding = selection->keypoints;
  binding.schema_id = "palette.stage.keypoint_observations";
  binding.schema_version = 2;
  const auto manifest = attributes.find("run_manifest");
  KeypointV2ManifestSummary summary;
  std::string error;
  if (manifest == attributes.end() || !manifest->is_object() ||
      !ValidateRawKeypointV2RunManifest(*manifest, binding.run_id, &summary,
                                        &error)) {
    invalidate(&binding, error.empty() ? "Bound raw-v2 keypoint manifest is missing"
                                       : std::move(error));
    return;
  }
  if (summary.frame_count != selection->frame_count ||
      summary.row_count != selection->observation_count ||
      summary.source_width != selection->source_width ||
      summary.source_height != selection->source_height ||
      summary.keypoint_count != kExpectedKeypointLabels.size() ||
      !std::equal(summary.keypoint_labels.begin(),
                  summary.keypoint_labels.end(),
                  kExpectedKeypointLabels.begin(), kExpectedKeypointLabels.end()) ||
      !rawImageCoordinateContractValid(*manifest)) {
    invalidate(&binding,
               "Bound raw-v2 keypoint dimensions, labels, or image coordinate "
               "authority disagree");
    return;
  }
  const std::string keys =
      arrayDigest(*manifest, "instance_key", {summary.row_count}, "uint64");
  const std::string frames = arrayDigest(*manifest,
                                         "source_acquisition_frame_index",
                                         {summary.row_count}, "int64");
  const std::string offsets =
      arrayDigest(*manifest, "frame_row_offsets",
                  {summary.frame_count + 1}, "int64");
  if (keys.empty() || frames.empty() || offsets.empty()) {
    invalidate(&binding, "Bound raw-v2 keypoint identity digests are invalid");
    return;
  }
  binding.valid = true;
  binding.error.clear();
  binding.identity_digest = summary.manifest_digest;
  binding.manifest_payload_digest = summary.payload_digest;
  binding.bound_source_run_id = selection->eye.run_id;
  binding.selector_eligible = summary.selector_eligible;
  binding.bound_selector_exception = !summary.selector_eligible;
  selection->keypoint_labels = summary.keypoint_labels;
  selection->instance_key_digest = keys;
  selection->acquisition_frame_digest = frames;
  selection->frame_row_offsets_digest = offsets;
}

void validateMask(const json &attributes,
                  CanonicalOverlaySelection *selection) {
  auto &binding = selection->mask;
  binding.schema_id = "palette.stage.refined_subject_mask_dense_core";
  binding.schema_version = 1;
  const auto manifest = attributes.find("run_manifest");
  SubjectMaskV1ManifestSummary summary;
  std::string error;
  if (manifest == attributes.end() || !manifest->is_object() ||
      !ValidateSubjectMaskV1Manifest(*manifest, binding.run_id, &summary,
                                     &error)) {
    invalidate(&binding, error.empty() ? "Bound strict-v1 mask manifest is missing"
                                       : std::move(error));
    return;
  }
  if (summary.manifest_schema_version != 5 ||
      summary.frame_count != selection->frame_count ||
      summary.row_count != selection->observation_count ||
      !exactStrings(json(summary.component_labels), kExpectedMaskLabels) ||
      stringValue(attributes, "palette_run_completion_status") != "complete" ||
      stringValue(attributes, "assignment_keypoint_group") !=
          "keypoints_runs" ||
      stringValue(attributes, "assignment_keypoints_run") !=
          selection->keypoints.run_id) {
    invalidate(&binding,
               "Bound strict-v1 mask schema, dimensions, or keypoint lineage "
               "disagree");
    return;
  }
  const std::string keys =
      arrayDigest(*manifest, "instance_key", {summary.row_count}, "uint64");
  const std::string frames = arrayDigest(*manifest,
                                         "source_acquisition_frame_index",
                                         {summary.row_count}, "int64");
  const std::string offsets =
      arrayDigest(*manifest, "frame_row_offsets",
                  {summary.frame_count + 1}, "int64");
  if (keys.empty() || frames.empty() || offsets.empty() ||
      (!selection->instance_key_digest.empty() &&
       selection->instance_key_digest != keys) ||
      (!selection->acquisition_frame_digest.empty() &&
       selection->acquisition_frame_digest != frames) ||
      (!selection->frame_row_offsets_digest.empty() &&
       selection->frame_row_offsets_digest != offsets)) {
    invalidate(&binding, "Bound mask/keypoint observation identity disagrees");
    return;
  }
  if (selection->instance_key_digest.empty()) {
    selection->instance_key_digest = keys;
    selection->acquisition_frame_digest = frames;
    selection->frame_row_offsets_digest = offsets;
  }
  binding.valid = true;
  binding.error.clear();
  binding.identity_digest = summary.manifest_digest;
  binding.manifest_payload_digest = summary.payload_digest;
  binding.bound_source_run_id = selection->keypoints.run_id;
  binding.selector_eligible = summary.selector_eligible;
  binding.bound_selector_exception = !summary.selector_eligible;
}

void validateShape(const json &group_attributes, const json &attributes,
                   CanonicalOverlaySelection *selection) {
  auto &binding = selection->shape;
  binding.schema_id = "analysis.subject_shape_runs";
  binding.schema_version = 5;
  const std::string latest = stringValue(group_attributes, "latest");
  const std::string latest_complete =
      stringValue(group_attributes, "latest_complete");
  const std::string publication =
      stringValue(attributes, "publication_manifest_sha256");
  const std::string shape_publication =
      stringValue(attributes, "subject_shape_publication_manifest_sha256");
  const std::string source_binding_digest =
      stringValue(attributes, "subject_shape_source_binding_sha256");
  const auto *source = objectAt(attributes, "subject_shape_source_binding");
  const auto *publication_document =
      objectAt(attributes, "subject_shape_publication_manifest");
  if (latest != binding.run_id || latest_complete != binding.run_id ||
      stringValue(attributes, "schema_id") != "analysis.subject_shape_runs" ||
      attributes.value("schema_version", 0) != 5 ||
      stringValue(attributes, "palette_run_name") != binding.run_id ||
      stringValue(attributes, "palette_run_completion_status") != "complete" ||
      !boolValue(attributes, "stage_selector_eligible") ||
      stringValue(attributes, "source_refined_subject_masks_run") !=
          selection->mask.run_id ||
      stringValue(attributes, "row_axis") !=
          "recording_subject_mask_bundle_rows" ||
      stringValue(attributes, "coordinate_contract") != "canonical_v2" ||
      !IsLowerSha256(publication) || publication != shape_publication ||
      !publication_document ||
      CanonicalJsonSha256(*publication_document) != publication || !source ||
      !IsLowerSha256(source_binding_digest) ||
      CanonicalJsonSha256(*source) != source_binding_digest) {
    invalidate(&binding, "Bound shape-v5 publication or selector is invalid");
    return;
  }
  if (!selection->mask.valid) {
    invalidate(&binding, "Bound shape mask is unavailable: " +
                             selection->mask.error);
    return;
  }
  try {
    const auto &frame_axis = source->at("frame_axis");
    const auto &extent = source->at("source_camera_extent");
    const auto &authorities = source->at("authorities");
    const auto &arrays = source->at("row_arrays");
    const std::string mask_path =
        "refined_subject_masks_runs/" + selection->mask.run_id;
    auto shapeArrayDigest = [&](const char *name) {
      const auto &entry = arrays.at(name);
      return IsLowerSha256(entry.value("sha256", ""))
                 ? entry.at("sha256").get<std::string>()
                 : std::string{};
    };
    if (source->value("schema_id", "") !=
            "palette.subject_shape.recording_mask_bundle_source" ||
        source->value("schema_version", 0) != 1 ||
        source->value("recording_identity", "") != selection->recording_id ||
        source->value("camera_identity", "") != selection->camera_id ||
        frame_axis.value("domain", "") !=
            "zero_based_acquisition_camera_frame" ||
        frame_axis.value("source_total_frames", size_t{0}) !=
            selection->frame_count ||
        frame_axis.value("frame_row_offsets_length", size_t{0}) !=
            selection->frame_count + 1 ||
        extent.value("width_px", size_t{0}) != selection->source_width ||
        extent.value("height_px", size_t{0}) != selection->source_height ||
        source->value("row_count", size_t{0}) !=
            selection->observation_count ||
        authorities.value("refined_run_path", "") != mask_path ||
        authorities.value("refined_manifest_payload_digest", "") !=
            selection->mask.manifest_payload_digest ||
        shapeArrayDigest("instance_key") != selection->instance_key_digest ||
        shapeArrayDigest("source_acquisition_frame_index") !=
            selection->acquisition_frame_digest ||
        shapeArrayDigest("frame_row_offsets") !=
            selection->frame_row_offsets_digest) {
      invalidate(&binding, "Bound shape/mask frame or observation lineage disagrees");
      return;
    }
  } catch (const json::exception &) {
    invalidate(&binding, "Bound shape source binding is malformed");
    return;
  }
  binding.valid = true;
  binding.error.clear();
  binding.identity_digest = publication;
  binding.manifest_payload_digest = source_binding_digest;
  binding.bound_source_run_id = selection->mask.run_id;
  binding.bound_source_payload_digest =
      selection->mask.manifest_payload_digest;
  binding.selector_eligible = true;
  binding.bound_selector_exception = false;
}

} // namespace

std::optional<CanonicalOverlaySelection>
ValidateCanonicalOverlaySelectionDocuments(
    const CanonicalOverlaySelectionDocuments &documents,
    const CanonicalOverlaySelectionRequest &request,
    std::string *error_message) {
  CanonicalOverlaySelection selection;
  if (!validateRoot(documents, request, &selection, error_message) ||
      !validateEye(documents, request, &selection, error_message)) {
    return std::nullopt;
  }
  validateKeypoints(documents.keypoint_attributes, &selection);
  validateMask(documents.mask_attributes, &selection);
  validateShape(documents.shape_group_attributes, documents.shape_attributes,
                &selection);
  if (error_message) {
    error_message->clear();
  }
  return selection;
}

std::optional<CanonicalOverlaySelection> SelectCanonicalOverlaySources(
    const std::shared_ptr<ArchiveContext> &archive,
    const CanonicalOverlaySelectionRequest &request,
    std::string *error_message) {
  if (!archive || !archive->impl_) {
    assignError(error_message, "Archive context is unavailable");
    return std::nullopt;
  }
  const auto &impl = *archive->impl_;
  const auto root = internal::ReadArchiveAttributes(impl, "");
  const auto eye_group =
      internal::ReadArchiveAttributes(impl, "analysis/eye_angle_runs");
  if (!root || !eye_group) {
    assignError(error_message,
                "Canonical overlay root or eye selector metadata is unavailable");
    return std::nullopt;
  }
  const std::string eye_run = stringValue(
      *eye_group, request.eye_run.empty() ? "latest" : "latest_complete");
  if (!validRunName(eye_run)) {
    assignError(error_message, "No selected eye-angle run is available");
    return std::nullopt;
  }
  const auto eye = internal::ReadArchiveAttributes(
      impl, "analysis/eye_angle_runs/" + eye_run);
  if (!eye) {
    assignError(error_message, "Selected eye-angle metadata is unavailable");
    return std::nullopt;
  }
  const std::string keypoint = stringValue(*eye, "source_base_keypoints_run");
  const std::string mask =
      stringValue(*eye, "source_refined_subject_masks_run");
  const std::string shape = stringValue(*eye, "source_subject_shape_run");
  if (!validRunName(keypoint) || !validRunName(mask) || !validRunName(shape)) {
    assignError(error_message,
                "Selected eye-angle run has malformed upstream bindings");
    return std::nullopt;
  }
  CanonicalOverlaySelectionDocuments documents;
  documents.archive_identity = archive->rootPath().lexically_normal().string();
  documents.root_attributes = *root;
  documents.eye_group_attributes = *eye_group;
  documents.eye_attributes = *eye;
  documents.keypoint_attributes =
      internal::ReadArchiveAttributes(impl, "keypoints_runs/" + keypoint)
          .value_or(json::object());
  documents.mask_attributes = internal::ReadArchiveAttributes(
                                  impl, "refined_subject_masks_runs/" + mask)
                                  .value_or(json::object());
  documents.shape_group_attributes =
      internal::ReadArchiveAttributes(impl, "analysis/subject_shape_runs")
          .value_or(json::object());
  documents.shape_attributes = internal::ReadArchiveAttributes(
                                   impl, "analysis/subject_shape_runs/" + shape)
                                   .value_or(json::object());
  return ValidateCanonicalOverlaySelectionDocuments(documents, request,
                                                     error_message);
}

} // namespace crimson::zarr
