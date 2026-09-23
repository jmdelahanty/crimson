#include "zarr/canonical_overlay_selection.h"

#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':'    \
                << __LINE__ << '\n';                                           \
      return false;                                                            \
    }                                                                          \
  } while (false)

constexpr const char *kEye = "eye_selected";
constexpr const char *kKeypoint = "keypoint_eye_bound_not_parent_latest";
constexpr const char *kMask = "mask_eye_bound_parent_latest_is_empty";
constexpr const char *kShape = "shape_eye_bound";
const std::string kDigest(64, 'a');
const std::string kReceipt(64, 'b');

crimson::zarr::CanonicalOverlaySelectionDocuments baseDocuments() {
  crimson::zarr::CanonicalOverlaySelectionDocuments documents;
  documents.archive_identity = "/fixture/analysis.zarr";
  documents.root_attributes = {
      {"recording_id", "recording_fixture"},
      {"camera_id", "93"},
      {"recording_frame_index_row_count", 4},
      {"recording_frame_id_min", 1},
      {"recording_frame_id_max", 4},
      {"source_video_metadata",
       {{"schema_id", "palette.source_video_collection_metadata.v1"},
        {"camera_id", "93"},
        {"total_frames", 4},
        {"width", 100},
        {"height", 80}}}};
  documents.eye_group_attributes = {
      {"latest", kEye}, {"latest_complete", kEye}};
  documents.eye_attributes = {
      {"schema_id", "analysis.eye_angle_runs"},
      {"schema_version", 7},
      {"palette_run_name", kEye},
      {"palette_run_completion_contract", "palette.zarr_run_completion.v1"},
      {"palette_run_completion_status", "complete"},
      {"stage_selector_eligible", true},
      {"lineage_hash", kDigest},
      {"source_fingerprint", kDigest},
      {"staged_input_integrity_receipt_sha256", kReceipt},
      {"source_base_keypoints_run", kKeypoint},
      {"source_refined_subject_masks_run", kMask},
      {"source_subject_shape_run", kShape},
      {"source_instance_key_path",
       std::string("keypoints_runs/") + kKeypoint + "/instance_key"},
      {"source_acquisition_frame_index_path",
       std::string("keypoints_runs/") + kKeypoint +
           "/source_acquisition_frame_index"},
      {"source_detection_success_path",
       std::string("keypoints_runs/") + kKeypoint + "/pose_success"},
      {"eye_angle_array_schema",
       {{"dimensions", {{"n_frames", 4}, {"n_roi_rows", 3}}}}}};
  documents.keypoint_attributes = nlohmann::json::object();
  documents.mask_attributes = nlohmann::json::object();
  documents.shape_group_attributes = {
      {"latest", kShape}, {"latest_complete", kShape}};
  documents.shape_attributes = nlohmann::json::object();
  return documents;
}

bool TestEyeAnchorsExactParentsAndIsolatesProductErrors() {
  auto documents = baseDocuments();
  std::string error;
  const auto selection =
      crimson::zarr::ValidateCanonicalOverlaySelectionDocuments(documents, {},
                                                                 &error);
  CHECK(selection.has_value());
  CHECK(error.empty());
  CHECK(selection->eye.valid);
  CHECK(selection->eye.run_id == kEye);
  CHECK(selection->keypoints.run_id == kKeypoint);
  CHECK(selection->mask.run_id == kMask);
  CHECK(selection->shape.run_id == kShape);
  CHECK(!selection->keypoints.valid);
  CHECK(!selection->mask.valid);
  CHECK(!selection->shape.valid);
  CHECK(!selection->keypoints.error.empty());
  CHECK(!selection->mask.error.empty());
  CHECK(!selection->shape.error.empty());
  CHECK(selection->frame_count == 4);
  CHECK(selection->first_acquisition_frame == 0);
  CHECK(selection->source_width == 100);
  CHECK(selection->source_height == 80);
  CHECK(selection->observation_count == 3);
  return true;
}

bool TestSelectorDisagreementFailsClosed() {
  auto documents = baseDocuments();
  crimson::zarr::CanonicalOverlaySelectionRequest request;
  request.eye_run = "independent_eye_latest";
  std::string error;
  CHECK(!crimson::zarr::ValidateCanonicalOverlaySelectionDocuments(
      documents, request, &error));
  CHECK(error.find("selector") != std::string::npos);

  documents = baseDocuments();
  documents.eye_group_attributes["latest_complete"] = "other_eye";
  error.clear();
  CHECK(!crimson::zarr::ValidateCanonicalOverlaySelectionDocuments(
      documents, {}, &error));
  CHECK(error.find("selector") != std::string::npos);
  return true;
}

bool TestRecordingAndFrameExpectationsFailClosed() {
  const auto documents = baseDocuments();
  crimson::zarr::CanonicalOverlaySelectionRequest request;
  request.expected_recording_id = "wrong_recording";
  std::string error;
  CHECK(!crimson::zarr::ValidateCanonicalOverlaySelectionDocuments(
      documents, request, &error));
  CHECK(error.find("expectations") != std::string::npos);

  request = {};
  request.expected_source_width = 101;
  error.clear();
  CHECK(!crimson::zarr::ValidateCanonicalOverlaySelectionDocuments(
      documents, request, &error));
  CHECK(error.find("expectations") != std::string::npos);
  return true;
}

bool TestEyeBindingAndEligibilityFailClosed() {
  auto documents = baseDocuments();
  documents.eye_attributes["source_instance_key_path"] =
      "keypoints_runs/unbound_latest/instance_key";
  std::string error;
  CHECK(!crimson::zarr::ValidateCanonicalOverlaySelectionDocuments(
      documents, {}, &error));
  CHECK(error.find("upstream bindings") != std::string::npos);

  documents = baseDocuments();
  documents.eye_attributes["stage_selector_eligible"] = false;
  error.clear();
  CHECK(!crimson::zarr::ValidateCanonicalOverlaySelectionDocuments(
      documents, {}, &error));
  CHECK(error.find("schema-7 authority") != std::string::npos);
  return true;
}

} // namespace

int main() {
  if (!TestEyeAnchorsExactParentsAndIsolatesProductErrors() ||
      !TestSelectorDisagreementFailsClosed() ||
      !TestRecordingAndFrameExpectationsFailClosed() ||
      !TestEyeBindingAndEligibilityFailClosed()) {
    return 1;
  }
  std::cout << "canonical_overlay_selection_tests: PASS\n";
  return 0;
}
