#include "read_only_overlay_scene.h"
#include "zarr/canonical_json.h"
#include "zarr/keypoint_overlay_scene_adapter.h"
#include "zarr/keypoint_v2_contract.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':'    \
                << __LINE__ << '\n';                                           \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool TestMultiObservationFrameIndex() {
  const std::vector<int64_t> offsets = {0, 2, 2, 3, 6};
  const std::vector<int64_t> frames = {0, 0, 2, 3, 3, 3};
  std::string error;
  CHECK(crimson::zarr::ValidateKeypointV2Offsets(offsets, 4, 6, &error));
  CHECK(crimson::zarr::ValidateKeypointV2FrameIndex(offsets, frames, 4, 6,
                                                    &error));
  CHECK(offsets[1] - offsets[0] == 2);
  CHECK(offsets[2] - offsets[1] == 0);
  CHECK(offsets[3] - offsets[2] == 1);
  CHECK(offsets[4] - offsets[3] == 3);

  auto malformed = offsets;
  malformed[2] = 1;
  error.clear();
  CHECK(!crimson::zarr::ValidateKeypointV2Offsets(malformed, 4, 6, &error));
  CHECK(!error.empty());
  malformed = offsets;
  malformed.back() = 5;
  CHECK(!crimson::zarr::ValidateKeypointV2Offsets(malformed, 4, 6));
  auto wrong_frames = frames;
  wrong_frames[2] = 1;
  CHECK(!crimson::zarr::ValidateKeypointV2FrameIndex(offsets, wrong_frames, 4,
                                                     6));
  CHECK(crimson::zarr::ValidateKeypointV2Offsets({0, 0, 0, 0}, 3, 0));
  CHECK(crimson::zarr::ValidateKeypointV2FrameIndex({0, 0, 0, 0}, {}, 3, 0));
  return true;
}

bool TestStableObservationKeys() {
  const std::vector<uint64_t> keys = {17, 23, 29, 31, 37, 41};
  CHECK(crimson::zarr::ValidateKeypointV2InstanceKeys(keys, keys.size()));
  auto duplicate = keys;
  duplicate.back() = duplicate.front();
  std::string error;
  CHECK(!crimson::zarr::ValidateKeypointV2InstanceKeys(
      duplicate, duplicate.size(), &error));
  CHECK(!error.empty());
  CHECK(!crimson::zarr::ValidateKeypointV2InstanceKeys(keys, keys.size() - 1));
  return true;
}

nlohmann::json CodeRegistries(nlohmann::json review_states,
                              nlohmann::json reasons) {
  nlohmann::json document = {
      {"schema_id", "palette.refined_keypoint.code_registries"},
      {"schema_version", 1},
      {"review_state_map", std::move(review_states)},
      {"reason_code_map", std::move(reasons)},
      {"zero_code_semantics",
       {{"review_state", "unreviewed"}, {"reason", "none"}}},
  };
  return {{"digest_algorithm", "sha256_canonical_json_v1"},
          {"digest", crimson::zarr::CanonicalJsonSha256(document)},
          {"document", std::move(document)}};
}

bool TestRefinedCodeRegistries() {
  std::string error;
  const auto unreviewed =
      CodeRegistries({{"0", "unreviewed"}}, {{"0", "none"}});
  CHECK(crimson::zarr::ValidateRefinedKeypointV2CodeRegistries(unreviewed,
                                                               &error));

  const auto reviewed = CodeRegistries(
      {{"0", "unreviewed"}, {"1", "accepted"}, {"2", "rejected"}},
      {{"0", "none"}, {"1", "manual_correction"}, {"2", "manual_rejection"}});
  CHECK(
      crimson::zarr::ValidateRefinedKeypointV2CodeRegistries(reviewed, &error));

  auto invalid = unreviewed;
  invalid["document"]["review_state_map"] = {{"1", "accepted"}};
  invalid["digest"] = crimson::zarr::CanonicalJsonSha256(invalid["document"]);
  CHECK(
      !crimson::zarr::ValidateRefinedKeypointV2CodeRegistries(invalid, &error));

  invalid = unreviewed;
  invalid["document"]["reason_code_map"]["01"] = "bad_code";
  invalid["digest"] = crimson::zarr::CanonicalJsonSha256(invalid["document"]);
  CHECK(
      !crimson::zarr::ValidateRefinedKeypointV2CodeRegistries(invalid, &error));

  invalid = unreviewed;
  invalid["document"]["review_state_map"]["3"] = "accepted";
  invalid["digest"] = crimson::zarr::CanonicalJsonSha256(invalid["document"]);
  CHECK(
      !crimson::zarr::ValidateRefinedKeypointV2CodeRegistries(invalid, &error));

  invalid = reviewed;
  invalid["document"]["reason_code_map"]["3"] = "manual_correction";
  invalid["digest"] = crimson::zarr::CanonicalJsonSha256(invalid["document"]);
  CHECK(
      !crimson::zarr::ValidateRefinedKeypointV2CodeRegistries(invalid, &error));

  invalid = reviewed;
  invalid["digest"] = std::string(64, '0');
  CHECK(
      !crimson::zarr::ValidateRefinedKeypointV2CodeRegistries(invalid, &error));
  return true;
}

nlohmann::json RefinedSkeletonSemantics() {
  return {
      {"schema_id", "palette.keypoint.skeleton_semantics"},
      {"schema_version", 1},
      {"skeleton_id", "pose_skel_traditional_v2"},
      {"kpt_shape", {5, 2}},
      {"keypoint_labels",
       {"swim_bladder", "eye_left", "eye_right", "snout_tip", "tail_tip"}},
      {"nodes",
       {{{"id", 0}, {"name", "swim_bladder"}},
        {{"id", 1}, {"name", "eye_left"}},
        {{"id", 2}, {"name", "eye_right"}},
        {{"id", 3}, {"name", "snout_tip"}},
        {{"id", 4}, {"name", "tail_tip"}}}},
      {"edges", {{0, 1}, {0, 2}, {1, 2}, {1, 3}, {2, 3}, {0, 4}}},
      {"heading_computation",
       {{"dependent_keypoints", {"swim_bladder", "eye_left", "eye_right"}},
        {"direction_from", {{"label", "swim_bladder"}, {"op", "keypoint"}}},
        {"direction_to",
         {{"labels", {"eye_left", "eye_right"}}, {"op", "midpoint"}}},
        {"enabled", true},
        {"origin", {{"labels", {"eye_left", "eye_right"}}, {"op", "midpoint"}}},
        {"version", 1}}},
      {"heading_computation_source",
       "authoritative_ordered_labels_controlled_policy_v1"},
  };
}

bool TestRefinedSkeletonSemantics() {
  const auto valid = RefinedSkeletonSemantics();
  const std::string digest = crimson::zarr::CanonicalJsonSha256(valid);
  std::vector<std::string> labels;
  std::vector<std::array<size_t, 2>> edges;
  std::string error;
  CHECK(crimson::zarr::ValidateRefinedKeypointV2SkeletonSemantics(
      valid, 5, "pose_skel_traditional_v2", digest, &labels, &edges, &error));
  CHECK(labels ==
        std::vector<std::string>({"swim_bladder", "eye_left", "eye_right",
                                  "snout_tip", "tail_tip"}));
  const std::vector<std::array<size_t, 2>> expected_edges = {
      {0, 1}, {0, 2}, {1, 2}, {1, 3}, {2, 3}, {0, 4}};
  CHECK(edges == expected_edges);

  auto invalid = valid;
  invalid["keypoint_labels"][4] = "snout_tip";
  invalid["nodes"][4]["name"] = "snout_tip";
  CHECK(!crimson::zarr::ValidateRefinedKeypointV2SkeletonSemantics(
      invalid, 5, "pose_skel_traditional_v2",
      crimson::zarr::CanonicalJsonSha256(invalid), nullptr, nullptr, &error));

  invalid = valid;
  invalid["edges"].push_back({0, 1});
  CHECK(!crimson::zarr::ValidateRefinedKeypointV2SkeletonSemantics(
      invalid, 5, "pose_skel_traditional_v2",
      crimson::zarr::CanonicalJsonSha256(invalid), nullptr, nullptr, &error));

  invalid = valid;
  invalid["heading_computation_source"] = "";
  CHECK(!crimson::zarr::ValidateRefinedKeypointV2SkeletonSemantics(
      invalid, 5, "pose_skel_traditional_v2",
      crimson::zarr::CanonicalJsonSha256(invalid), nullptr, nullptr, &error));

  CHECK(!crimson::zarr::ValidateRefinedKeypointV2SkeletonSemantics(
      valid, 5, "pose_skel_traditional_v2", std::string(64, '0'), nullptr,
      nullptr, &error));
  return true;
}

bool TestPresentationPreservesRefinedState() {
  crimson::zarr::KeypointOverlayDescriptor descriptor;
  descriptor.source_group = "refined_keypoints_runs";
  descriptor.run_name = "refined_v2";
  descriptor.coordinate_space = crimson::zarr::KeypointCoordinateSpace::Image;
  descriptor.refined = true;
  descriptor.keypoint_labels = {"center", "left", "right"};
  descriptor.skeleton_edges = {{0, 1}, {0, 2}};
  descriptor.camera_frame_count = 4;
  descriptor.row_count = 6;

  crimson::zarr::KeypointOverlayResolution resolution;
  resolution.status = crimson::zarr::KeypointOverlayStatus::Mapped;
  resolution.camera_frame = 3;
  crimson::zarr::KeypointOverlayDetection detection;
  detection.instance_key = 0xfedcba9876543210ULL;
  detection.detection_index = 5;
  detection.keypoints = {{10.0, 20.0}, {12.0, 18.0}, {14.0, 18.0}};
  detection.heading_origin = crimson::zarr::KeypointOverlayPoint{13.0, 18.0};
  detection.heading_degrees = 35.0;
  detection.heading_valid = true;
  detection.heading_from_body_frame = true;
  detection.refined_keypoints = true;
  detection.source_success = false;
  detection.refined_success = true;
  detection.confidence_valid = false;
  detection.geometry_valid = true;
  detection.keypoint_usable = true;
  detection.review_state_code = 1;
  detection.reason_code = 3;
  detection.keypoint_edit_flags = {1, 1, 1};
  resolution.detections.push_back(detection);

  const auto input = crimson::zarr::makeKeypointOverlaySceneInput(
      descriptor, resolution, 0, 3, 0, 4512, 4512);
  CHECK(input.detections.size() == 1);
  const auto &adapted = input.detections.front();
  CHECK(adapted.instance_key == detection.instance_key);
  CHECK(adapted.source_success == detection.source_success);
  CHECK(adapted.refined_success == detection.refined_success);
  CHECK(adapted.review_state_code == detection.review_state_code);
  CHECK(adapted.reason_code == detection.reason_code);
  CHECK(adapted.keypoint_edit_flags == detection.keypoint_edit_flags);
  CHECK(adapted.heading_from_body_frame);

  const auto scene = crimson::overlay::buildReadOnlyOverlayScene(input);
  CHECK(scene.ready());
  CHECK(scene.count(crimson::overlay::PrimitiveType::Arrow) == 1);
  CHECK(scene.count(crimson::overlay::PrimitiveType::Marker) == 3);
  CHECK(std::all_of(scene.primitives.begin(), scene.primitives.end(),
                    [&](const auto &primitive) {
                      return primitive.instance_key == detection.instance_key;
                    }));
  return true;
}

} // namespace

int main() {
  if (!TestMultiObservationFrameIndex() || !TestStableObservationKeys() ||
      !TestRefinedCodeRegistries() || !TestRefinedSkeletonSemantics() ||
      !TestPresentationPreservesRefinedState()) {
    return 1;
  }
  std::cout << "keypoint_v2_contract_tests: PASS\n";
  return 0;
}
