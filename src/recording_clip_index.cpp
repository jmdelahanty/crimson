#include "recording_clip_index.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace crimson::media {
namespace {

using json = nlohmann::json;

void AssignError(std::string *destination, std::string value) {
  if (destination) {
    *destination = std::move(value);
  }
}

bool ContainsParentTraversal(const std::filesystem::path &path) {
  return std::any_of(path.begin(), path.end(),
                     [](const auto &part) { return part == ".."; });
}

bool IsSafeRelativePath(const std::filesystem::path &path) {
  return !path.empty() && !path.is_absolute() && !path.has_root_name() &&
         !ContainsParentTraversal(path);
}

bool ReadRequiredString(const json &object, std::string_view key,
                        std::string *value) {
  const auto found = object.find(std::string(key));
  if (found == object.end() || !found->is_string() ||
      found->get_ref<const std::string &>().empty()) {
    return false;
  }
  *value = found->get<std::string>();
  return true;
}

bool ReadRequiredInt64(const json &object, std::string_view key,
                       int64_t *value) {
  const auto found = object.find(std::string(key));
  if (found == object.end() || !found->is_number_integer()) {
    return false;
  }
  try {
    if (found->is_number_unsigned()) {
      const uint64_t unsigned_value = found->get<uint64_t>();
      if (unsigned_value >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return false;
      }
      *value = static_cast<int64_t>(unsigned_value);
      return true;
    }
    *value = found->get<int64_t>();
    return true;
  } catch (const json::exception &) {
    return false;
  }
}

bool ReadRequiredBool(const json &object, std::string_view key, bool *value) {
  const auto found = object.find(std::string(key));
  if (found == object.end() || !found->is_boolean()) {
    return false;
  }
  *value = found->get<bool>();
  return true;
}

bool ReadRequiredDouble(const json &object, std::string_view key,
                        double *value) {
  const auto found = object.find(std::string(key));
  if (found == object.end() || !found->is_number()) {
    return false;
  }
  try {
    *value = found->get<double>();
    return std::isfinite(*value);
  } catch (const json::exception &) {
    return false;
  }
}

bool CheckedSubtract(int64_t left, int64_t right, int64_t *value) {
  if ((right > 0 && left < std::numeric_limits<int64_t>::min() + right) ||
      (right < 0 && left > std::numeric_limits<int64_t>::max() + right)) {
    return false;
  }
  *value = left - right;
  return true;
}

bool CheckedAdd(int64_t left, int64_t right, int64_t *value) {
  if ((right > 0 && left > std::numeric_limits<int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<int64_t>::min() - right)) {
    return false;
  }
  *value = left + right;
  return true;
}

bool EqualFps(double left, double right) {
  const double scale = std::max({1.0, std::abs(left), std::abs(right)});
  return std::abs(left - right) <= 1e-9 * scale;
}

bool IsRegularFile(const std::filesystem::path &path) {
  std::error_code status_error;
  return std::filesystem::is_regular_file(path, status_error);
}

std::optional<json> ReadJson(const std::filesystem::path &path,
                             std::string *error_message) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    AssignError(error_message,
                "Recording clip index is not readable: " + path.string());
    return std::nullopt;
  }
  try {
    json value;
    input >> value;
    if (!value.is_object()) {
      AssignError(error_message, "Recording clip index root is not an object");
      return std::nullopt;
    }
    return value;
  } catch (const json::exception &error) {
    AssignError(error_message, "Recording clip index is invalid JSON: " +
                                   std::string(error.what()));
    return std::nullopt;
  }
}

std::optional<json> ReadJsonArtifact(const std::filesystem::path &path,
                                     std::string_view label,
                                     std::string *error_message) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    AssignError(error_message,
                std::string(label) + " is not readable: " + path.string());
    return std::nullopt;
  }
  try {
    json value;
    input >> value;
    if (!value.is_object()) {
      AssignError(error_message, std::string(label) + " is not an object");
      return std::nullopt;
    }
    return value;
  } catch (const json::exception &error) {
    AssignError(error_message, std::string(label) + " is invalid JSON: " +
                                   error.what());
    return std::nullopt;
  }
}

bool ValidateMaterializedChecks(const json &root, size_t clip_count,
                                std::string *error_message) {
  const auto found = root.find("checks");
  if (found == root.end() || !found->is_array() || found->empty()) {
    AssignError(error_message,
                "Recording clip index has no completed validation checks");
    return false;
  }
  size_t keyframe_checks = 0;
  size_t continuity_checks = 0;
  for (const auto &check : *found) {
    if (!check.is_object() || check.value("status", "") != "ok") {
      AssignError(error_message,
                  "Recording clip index contains a failed validation check");
      return false;
    }
    const std::string code = check.value("code", "");
    keyframe_checks += code == "clip_start_is_keyframe" ? 1 : 0;
    continuity_checks += code == "clip_recording_frame_id_continuity" ? 1 : 0;
  }
  if (keyframe_checks != clip_count || continuity_checks != clip_count) {
    AssignError(error_message,
                "Recording clip index validation coverage is incomplete");
    return false;
  }
  return true;
}

struct RollingCheckExpectation {
  std::string clip_id;
  std::string camera_serial;
  int64_t first_id = 0;
  int64_t last_id = 0;
  int64_t frame_count = 0;
};

bool ValidateRollingChecks(
    const json &manifest,
    const std::vector<RollingCheckExpectation> &expectations,
    std::string *error_message) {
  const auto found = manifest.find("checks");
  if (found == manifest.end() || !found->is_array()) {
    AssignError(error_message,
                "Rolling frame-index validation checks are missing");
    return false;
  }
  std::map<std::string, std::set<std::string>> coverage;
  std::map<std::string, const RollingCheckExpectation *> by_clip;
  for (const auto &expected : expectations) {
    by_clip.emplace(expected.clip_id, &expected);
  }
  bool nonempty_check = false;
  for (const auto &check : *found) {
    if (!check.is_object() || check.value("status", "") != "ok") {
      AssignError(error_message,
                  "Rolling frame-index manifest contains a failed check");
      return false;
    }
    const std::string code = check.value("code", "");
    if (code == "recording_frame_index_nonempty") {
      int64_t declared_rows = 0;
      int64_t expected_rows = 0;
      for (const auto &expected : expectations) {
        if (!CheckedAdd(expected_rows, expected.frame_count, &expected_rows)) {
          AssignError(error_message,
                      "Rolling frame-index check row count overflows");
          return false;
        }
      }
      nonempty_check =
          ReadRequiredInt64(check, "row_count", &declared_rows) &&
          declared_rows == expected_rows;
      if (!nonempty_check) {
        AssignError(error_message,
                    "Rolling frame-index nonempty check payload is invalid");
        return false;
      }
      continue;
    }
    const auto id = check.find("clip_id");
    if (id != check.end() && id->is_string()) {
      const auto expected = by_clip.find(id->get<std::string>());
      if (expected == by_clip.end()) {
        AssignError(error_message,
                    "Rolling frame-index check names an unknown clip");
        return false;
      }
      const auto camera = check.find("camera_serial");
      if (camera != check.end() &&
          (!camera->is_string() ||
           camera->get<std::string>() != expected->second->camera_serial)) {
        AssignError(error_message,
                    "Rolling frame-index check camera identity is invalid");
        return false;
      }
      int64_t expected_value = 0;
      int64_t observed_value = 0;
      int64_t gaps = -1;
      bool payload_ok = true;
      if (code == "metadata_rows_match_clip_index_frame_count") {
        payload_ok = ReadRequiredInt64(check, "expected", &expected_value) &&
                     ReadRequiredInt64(check, "observed", &observed_value) &&
                     expected_value == expected->second->frame_count &&
                     observed_value == expected->second->frame_count;
      } else if (code == "first_recording_frame_id_matches_clip_index") {
        payload_ok = ReadRequiredInt64(check, "expected", &expected_value) &&
                     ReadRequiredInt64(check, "observed", &observed_value) &&
                     expected_value == expected->second->first_id &&
                     observed_value == expected->second->first_id;
      } else if (code == "last_recording_frame_id_matches_clip_index") {
        payload_ok = ReadRequiredInt64(check, "expected", &expected_value) &&
                     ReadRequiredInt64(check, "observed", &observed_value) &&
                     expected_value == expected->second->last_id &&
                     observed_value == expected->second->last_id;
      } else if (code == "clip_recording_frame_id_continuity") {
        payload_ok = ReadRequiredInt64(check, "recording_frame_id_gaps", &gaps) &&
                     gaps == 0;
      } else if (code == "inter_clip_recording_frame_id_continuity") {
        const auto ordinal = static_cast<size_t>(
            std::distance(expectations.data(), expected->second));
        int64_t previous_last = 0;
        int64_t current_first = 0;
        payload_ok = ordinal > 0 &&
                     camera != check.end() &&
                     ReadRequiredInt64(check, "previous_last_recording_frame_id",
                                       &previous_last) &&
                     ReadRequiredInt64(check, "current_first_recording_frame_id",
                                       &current_first) &&
                     previous_last == expectations[ordinal - 1].last_id &&
                     current_first == expected->second->first_id;
      }
      if (!payload_ok) {
        AssignError(error_message,
                    "Rolling frame-index check payload is stale or invalid");
        return false;
      }
      coverage[id->get<std::string>()].insert(code);
    }
  }
  const std::set<std::string> required = {
      "metadata_rows_match_clip_index_frame_count",
      "first_recording_frame_id_matches_clip_index",
      "last_recording_frame_id_matches_clip_index",
      "clip_recording_frame_id_continuity"};
  if (!nonempty_check) {
    AssignError(error_message,
                "Rolling frame-index nonempty check is missing");
    return false;
  }
  for (size_t ordinal = 0; ordinal < expectations.size(); ++ordinal) {
    const auto entry = coverage.find(expectations[ordinal].clip_id);
    if (entry == coverage.end() ||
        !std::includes(entry->second.begin(), entry->second.end(),
                       required.begin(), required.end()) ||
        (ordinal > 0 &&
         entry->second.find("inter_clip_recording_frame_id_continuity") ==
             entry->second.end())) {
      AssignError(error_message,
                  "Rolling frame-index check coverage is incomplete for " +
                      expectations[ordinal].clip_id);
      return false;
    }
  }
  return true;
}

} // namespace

std::optional<RecordingClipIndex>
RecordingClipIndex::Open(const std::filesystem::path &index_path,
                         std::string *error_message) {
  if (error_message) {
    error_message->clear();
  }
  const auto root = ReadJson(index_path, error_message);
  if (!root) {
    return std::nullopt;
  }

  try {
    RecordingClipIndex result;
    result.index_path_ =
        std::filesystem::absolute(index_path).lexically_normal();
    result.recording_root_ = result.index_path_.parent_path();

    if (root->value("mode", "") == "rolling_clips") {
      constexpr std::string_view kIndexSchema =
          "palette.orange_external_ipc_recording_clip_index.v1";
      constexpr std::string_view kClipSchema =
          "palette.orange_external_ipc_rolling_clip.v1";
      constexpr std::string_view kManifestSchema =
          "palette.recording_frame_index_manifest.v1";
      constexpr std::string_view kFrameIndexSchema =
          "palette.recording_frame_index.v1";
      int64_t schema_version = 0;
      int64_t declared_clip_count = 0;
      int64_t declared_row_count = 0;
      std::string session_id;
      const auto cameras = root->find("cameras");
      const auto rows = root->find("rows");
      if (root->value("schema_id", "") != std::string(kIndexSchema) ||
          !ReadRequiredInt64(*root, "schema_version", &schema_version) ||
          schema_version != 1 ||
          root->value("source_layout", "") != "rolling_clips" ||
          root->value("recording_backend_mode", "") != "external_ipc" ||
          root->value("producer", "") != "orange_gui_external_ipc" ||
          root->value("recording_folder", "") != "." ||
          root->value("row_granularity", "") != "clip_camera" ||
          !ReadRequiredString(*root, "recording_id", &result.recording_id_) ||
          !ReadRequiredString(*root, "session_id", &session_id) ||
          cameras == root->end() || !cameras->is_array() ||
          cameras->size() != 1 || !cameras->at(0).is_string() ||
          cameras->at(0).get_ref<const std::string &>().empty() ||
          rows == root->end() || !rows->is_array() || rows->empty() ||
          !ReadRequiredInt64(*root, "clip_count", &declared_clip_count) ||
          !ReadRequiredInt64(*root, "row_count", &declared_row_count) ||
          declared_clip_count <= 0 ||
          declared_clip_count != declared_row_count ||
          static_cast<size_t>(declared_row_count) != rows->size()) {
        AssignError(error_message,
                    "Rolling recording clip index schema or inventory is invalid");
        return std::nullopt;
      }
      result.camera_serial_ = cameras->at(0).get<std::string>();

      int64_t range_clip_count = 0;
      int64_t range_first_id = 0;
      int64_t range_last_id = 0;
      int64_t range_frame_count = 0;
      int64_t range_gaps = -1;
      const auto ranges = root->find("camera_ranges");
      if (ranges == root->end() || !ranges->is_object()) {
        AssignError(error_message, "Rolling camera range is missing");
        return std::nullopt;
      }
      // Consolidated per-camera recordings may retain source-session ranges
      // for sibling cameras. Only the singleton selected camera is authority.
      const auto selected_range = ranges->find(result.camera_serial_);
      if (selected_range == ranges->end() || !selected_range->is_object() ||
          !ReadRequiredInt64(*selected_range, "clip_count",
                             &range_clip_count) ||
          !ReadRequiredInt64(*selected_range, "first_recording_frame_id",
                             &range_first_id) ||
          !ReadRequiredInt64(*selected_range, "last_recording_frame_id",
                             &range_last_id) ||
          !ReadRequiredInt64(*selected_range, "total_frame_count",
                             &range_frame_count) ||
          !ReadRequiredInt64(*selected_range, "recording_frame_id_gaps",
                             &range_gaps) ||
          range_clip_count != declared_clip_count || range_frame_count <= 0 ||
          range_gaps != 0 || range_last_id < range_first_id) {
        AssignError(error_message, "Rolling selected-camera range is invalid");
        return std::nullopt;
      }

      const auto manifest_path =
          result.recording_root_ / "recording_frame_index_manifest.json";
      // The bounded manifest publishes offsets and validation evidence; the
      // multi-million-row derived Parquet/CSV index is not needed for playback.
      const auto manifest = ReadJsonArtifact(
          manifest_path, "Rolling frame-index manifest", error_message);
      if (!manifest) {
        return std::nullopt;
      }
      int64_t failure_count = -1;
      int64_t manifest_row_count = 0;
      int64_t manifest_min_id = 0;
      int64_t manifest_max_id = 0;
      bool dry_run = true;
      std::string declared_recording_folder;
      const auto inputs = manifest->find("inputs");
      const auto manifest_cameras = manifest->find("camera_serials");
      if (manifest->value("schema_version", "") !=
              std::string(kManifestSchema) ||
          manifest->value("frame_index_schema_version", "") !=
              std::string(kFrameIndexSchema) ||
          manifest->value("status", "") != "ok" ||
          manifest->value("source_layout", "") != "rolling_clips" ||
          manifest->value("source_authority", "") !=
              "recording_clip_index + per_clip_metadata_csv" ||
          manifest->value("recording_id", "") != result.recording_id_ ||
          manifest->value("session_id", "") != session_id ||
          !ReadRequiredInt64(*manifest, "failure_count", &failure_count) ||
          failure_count != 0 ||
          !ReadRequiredBool(*manifest, "dry_run", &dry_run) || dry_run ||
          !ReadRequiredInt64(*manifest, "row_count", &manifest_row_count) ||
          !ReadRequiredInt64(*manifest, "recording_frame_id_min",
                             &manifest_min_id) ||
          !ReadRequiredInt64(*manifest, "recording_frame_id_max",
                             &manifest_max_id) ||
          !ReadRequiredString(*manifest, "recording_folder",
                              &declared_recording_folder) ||
          manifest_row_count != range_frame_count ||
          manifest_min_id != range_first_id ||
          manifest_max_id != range_last_id || inputs == manifest->end() ||
          !inputs->is_array() || inputs->size() != rows->size() ||
          manifest_cameras == manifest->end() ||
          !manifest_cameras->is_array() || manifest_cameras->size() != 1 ||
          !manifest_cameras->at(0).is_string() ||
          manifest_cameras->at(0).get<std::string>() != result.camera_serial_) {
        AssignError(error_message,
                    "Rolling frame-index manifest identity or publication state is invalid");
        return std::nullopt;
      }
      const std::filesystem::path old_recording_root =
          std::filesystem::path(declared_recording_folder).lexically_normal();
      if (!old_recording_root.is_absolute()) {
        AssignError(error_message,
                    "Rolling frame-index manifest recording folder is invalid");
        return std::nullopt;
      }

      std::map<std::string, const json *> inputs_by_clip;
      for (const auto &input : *inputs) {
        std::string clip_id;
        std::string camera_serial;
        if (!input.is_object() ||
            !ReadRequiredString(input, "clip_id", &clip_id) ||
            !ReadRequiredString(input, "camera_serial", &camera_serial) ||
            camera_serial != result.camera_serial_ ||
            !inputs_by_clip.emplace(clip_id, &input).second) {
          AssignError(error_message,
                      "Rolling frame-index input identity is invalid");
          return std::nullopt;
        }
      }

      std::set<std::string> clip_ids;
      std::vector<RollingCheckExpectation> check_expectations;
      int64_t expected_parent_start = 0;
      int64_t expected_recording_first = range_first_id;
      result.clips_.reserve(rows->size());
      for (size_t ordinal = 0; ordinal < rows->size(); ++ordinal) {
        const auto &entry = rows->at(ordinal);
        RecordingClipDescriptor clip;
        int64_t declared_index = -1;
        int64_t first_id = 0;
        int64_t last_id = 0;
        int64_t gaps = -1;
        int64_t packet_count = 0;
        bool drain_completed = false;
        bool final_clip = false;
        std::string status;
        std::string stored_video;
        std::string stored_metadata;
        std::string stored_keyframe;
        std::string stored_manifest;
        if (!entry.is_object() ||
            !ReadRequiredInt64(entry, "clip_index", &declared_index) ||
            declared_index != static_cast<int64_t>(ordinal) ||
            !ReadRequiredString(entry, "clip_id", &clip.clip_id) ||
            !ReadRequiredString(entry, "recording_id", &clip.recording_id) ||
            !ReadRequiredString(entry, "camera_serial", &clip.camera_serial) ||
            !ReadRequiredString(entry, "status", &status) ||
            !ReadRequiredBool(entry, "drain_completed", &drain_completed) ||
            !ReadRequiredBool(entry, "final_clip", &final_clip) ||
            !ReadRequiredInt64(entry, "first_recording_frame_id", &first_id) ||
            !ReadRequiredInt64(entry, "last_recording_frame_id", &last_id) ||
            !ReadRequiredInt64(entry, "frame_count", &clip.frame_count) ||
            !ReadRequiredInt64(entry, "recording_frame_id_gaps", &gaps) ||
            !ReadRequiredInt64(entry, "packet_count", &packet_count) ||
            !ReadRequiredString(entry, "video_path", &stored_video) ||
            !ReadRequiredString(entry, "metadata_path", &stored_metadata) ||
            !ReadRequiredString(entry, "keyframe_path", &stored_keyframe) ||
            !ReadRequiredString(entry, "clip_manifest_path", &stored_manifest)) {
          AssignError(error_message,
                      "Rolling clip declaration is incomplete at ordinal " +
                          std::to_string(ordinal));
          return std::nullopt;
        }
        clip.index = ordinal;
        clip.stored_video_path = stored_video;
        const std::filesystem::path metadata_path = stored_metadata;
        const std::filesystem::path keyframe_path = stored_keyframe;
        const std::filesystem::path clip_manifest_path = stored_manifest;
        int64_t id_delta = 0;
        int64_t computed_count = 0;
        if (status != "completed" || !drain_completed ||
            final_clip != (ordinal + 1 == rows->size()) ||
            clip.recording_id != result.recording_id_ ||
            clip.camera_serial != result.camera_serial_ || gaps != 0 ||
            entry.value("session_id", "") != session_id ||
            entry.value("producer", "") != "orange_gui_external_ipc" ||
            entry.value("recording_backend_mode", "") != "external_ipc" ||
            entry.value("source_layout", "") != "rolling_clips" ||
            packet_count < 0 || clip.frame_count <= 0 ||
            last_id < first_id || !CheckedSubtract(last_id, first_id, &id_delta) ||
            !CheckedAdd(id_delta, 1, &computed_count) ||
            computed_count != clip.frame_count ||
            first_id != expected_recording_first ||
            !clip_ids.insert(clip.clip_id).second ||
            !IsSafeRelativePath(clip.stored_video_path) ||
            !IsSafeRelativePath(metadata_path) ||
            !IsSafeRelativePath(keyframe_path) ||
            !IsSafeRelativePath(clip_manifest_path)) {
          AssignError(error_message,
                      "Rolling clip violates completion, continuity, identity, or path policy at ordinal " +
                          std::to_string(ordinal));
          return std::nullopt;
        }

        const auto input_found = inputs_by_clip.find(clip.clip_id);
        int64_t input_first = 0;
        int64_t input_last = 0;
        int64_t input_rows = 0;
        int64_t input_gaps = -1;
        int64_t parent_offset = 0;
        std::string input_metadata;
        if (input_found == inputs_by_clip.end() ||
            !ReadRequiredInt64(*input_found->second,
                               "first_recording_frame_id", &input_first) ||
            !ReadRequiredInt64(*input_found->second,
                               "last_recording_frame_id", &input_last) ||
            !ReadRequiredInt64(*input_found->second, "rows", &input_rows) ||
            !ReadRequiredInt64(*input_found->second,
                               "recording_frame_id_gaps", &input_gaps) ||
            !ReadRequiredInt64(*input_found->second,
                               "parent_frame_index_offset", &parent_offset) ||
            !ReadRequiredString(*input_found->second, "metadata_path",
                                &input_metadata) ||
            input_first != first_id || input_last != last_id ||
            input_rows != clip.frame_count || input_gaps != 0) {
          AssignError(error_message,
                      "Rolling frame-index input disagrees with clip " +
                          clip.clip_id);
          return std::nullopt;
        }
        std::filesystem::path input_metadata_path(input_metadata);
        if (input_metadata_path.is_absolute()) {
          input_metadata_path = input_metadata_path.lexically_relative(
              old_recording_root);
        }
        if (!IsSafeRelativePath(input_metadata_path) ||
            input_metadata_path.lexically_normal() !=
                metadata_path.lexically_normal()) {
          AssignError(error_message,
                      "Rolling frame-index metadata path disagrees with clip " +
                          clip.clip_id);
          return std::nullopt;
        }

        if (!CheckedSubtract(first_id, parent_offset,
                             &clip.parent_start_frame) ||
            !CheckedAdd(clip.parent_start_frame, clip.frame_count,
                        &clip.parent_stop_frame) ||
            clip.parent_start_frame != expected_parent_start ||
            clip.parent_stop_frame <= clip.parent_start_frame) {
          AssignError(error_message,
                      "Rolling frame-domain mapping is invalid for clip " +
                          clip.clip_id);
          return std::nullopt;
        }
        clip.local_start_frame = 0;
        clip.local_stop_frame = clip.frame_count - 1;
        clip.resolved_video_path =
            (result.recording_root_ / clip.stored_video_path).lexically_normal();
        const auto resolved_metadata =
            (result.recording_root_ / metadata_path).lexically_normal();
        const auto resolved_keyframe =
            (result.recording_root_ / keyframe_path).lexically_normal();
        const auto resolved_manifest =
            (result.recording_root_ / clip_manifest_path).lexically_normal();
        if (!IsRegularFile(clip.resolved_video_path) ||
            !IsRegularFile(resolved_metadata) ||
            !IsRegularFile(resolved_keyframe) ||
            !IsRegularFile(resolved_manifest)) {
          AssignError(error_message,
                      "Rolling clip artifact is not readable for clip " +
                          clip.clip_id);
          return std::nullopt;
        }

        const auto keyframes = ReadJsonArtifact(
            resolved_keyframe, "Rolling keyframe sidecar", error_message);
        int64_t keyframe_total = 0;
        double keyframe_fps = 0.0;
        std::string keyframe_codec;
        const auto keyframe_values =
            keyframes ? keyframes->find("keyframe_frames") : json::const_iterator{};
        if (!keyframes ||
            !ReadRequiredString(*keyframes, "codec", &keyframe_codec) ||
            !ReadRequiredInt64(*keyframes, "total_frames", &keyframe_total) ||
            !ReadRequiredDouble(*keyframes, "fps", &keyframe_fps) ||
            keyframe_fps <= 0.0 || keyframe_total != clip.frame_count ||
            keyframe_values == keyframes->end() || !keyframe_values->is_array() ||
            keyframe_values->empty()) {
          if (error_message && error_message->empty()) {
            AssignError(error_message,
                        "Rolling keyframe evidence is invalid for clip " +
                            clip.clip_id);
          }
          return std::nullopt;
        }
        int64_t previous_keyframe = -1;
        for (const auto &value : *keyframe_values) {
          int64_t keyframe = -1;
          if (!value.is_number_integer()) {
            AssignError(error_message,
                        "Rolling keyframe evidence contains a non-integer");
            return std::nullopt;
          }
          try {
            keyframe = value.get<int64_t>();
          } catch (const json::exception &) {
            AssignError(error_message,
                        "Rolling keyframe evidence contains an out-of-range value");
            return std::nullopt;
          }
          if (keyframe <= previous_keyframe || keyframe < 0 ||
              keyframe >= clip.frame_count) {
            AssignError(error_message,
                        "Rolling keyframe evidence is unordered or out of range");
            return std::nullopt;
          }
          previous_keyframe = keyframe;
        }
        if (keyframe_values->front().get<int64_t>() != 0) {
          AssignError(error_message,
                      "Rolling clip does not start at a keyframe: " +
                          clip.clip_id);
          return std::nullopt;
        }
        if (ordinal == 0) {
          result.frames_per_second_ = keyframe_fps;
        } else if (!EqualFps(result.frames_per_second_, keyframe_fps)) {
          AssignError(error_message,
                      "Rolling clip frame rates are inconsistent");
          return std::nullopt;
        }

        const auto clip_manifest = ReadJsonArtifact(
            resolved_manifest, "Rolling clip manifest", error_message);
        int64_t manifest_version = 0;
        int64_t manifest_index = -1;
        bool manifest_final = false;
        const auto artifacts = clip_manifest
                                   ? clip_manifest->find("camera_artifacts")
                                   : json::const_iterator{};
        const auto outputs = clip_manifest
                                 ? clip_manifest->find("recording_outputs")
                                 : json::const_iterator{};
        if (!clip_manifest ||
            clip_manifest->value("schema_id", "") !=
                std::string(kClipSchema) ||
            !ReadRequiredInt64(*clip_manifest, "schema_version",
                               &manifest_version) ||
            manifest_version != 1 ||
            clip_manifest->value("status", "") != "completed" ||
            clip_manifest->value("recording_id", "") != result.recording_id_ ||
            clip_manifest->value("session_id", "") != session_id ||
            clip_manifest->value("clip_id", "") != clip.clip_id ||
            !ReadRequiredInt64(*clip_manifest, "clip_index", &manifest_index) ||
            manifest_index != declared_index ||
            !ReadRequiredBool(*clip_manifest, "final_clip", &manifest_final) ||
            manifest_final != final_clip || artifacts == clip_manifest->end() ||
            !artifacts->is_array() || artifacts->size() != 1 ||
            outputs == clip_manifest->end() || !outputs->is_object()) {
          if (error_message && error_message->empty()) {
            AssignError(error_message,
                        "Rolling clip manifest identity is invalid for " +
                            clip.clip_id);
          }
          return std::nullopt;
        }
        const auto &artifact = artifacts->at(0);
        const auto camera_output = outputs->find(result.camera_serial_);
        const auto full = camera_output != outputs->end() && camera_output->is_object()
                              ? camera_output->find("full")
                              : json::const_iterator{};
        auto matching_stream = [&](const json &stream, bool require_role,
                                   bool require_camera) {
          int64_t stream_first = 0;
          int64_t stream_last = 0;
          int64_t stream_count = 0;
          int64_t stream_gaps = -1;
          double stream_fps = 0.0;
          std::string stream_camera;
          std::string stream_video;
          std::string stream_metadata;
          std::string stream_keyframe;
          std::string stream_codec;
          const bool has_video = require_role
                                     ? ReadRequiredString(stream, "video",
                                                          &stream_video)
                                     : ReadRequiredString(stream, "video_path",
                                                          &stream_video);
          const bool has_metadata =
              require_role
                  ? ReadRequiredString(stream, "metadata", &stream_metadata)
                  : ReadRequiredString(stream, "metadata_path",
                                       &stream_metadata);
          const bool has_keyframe =
              require_role
                  ? ReadRequiredString(stream, "keyframes", &stream_keyframe)
                  : ReadRequiredString(stream, "keyframe_path",
                                       &stream_keyframe);
          const auto stream_camera_value = stream.find("camera_serial");
          const bool camera_matches =
              stream_camera_value == stream.end() ||
              (stream_camera_value->is_string() &&
               stream_camera_value->get<std::string>() ==
                   result.camera_serial_);
          return stream.is_object() &&
                 (!require_camera ||
                  (ReadRequiredString(stream, "camera_serial", &stream_camera) &&
                   stream_camera == result.camera_serial_)) &&
                 camera_matches &&
                 (!require_role ||
                  (stream.value("role", "") == "ingest_authoritative" &&
                   stream.value("output_kind", "") == "full")) &&
                 ReadRequiredInt64(stream, "first_recording_frame_id",
                                   &stream_first) &&
                 ReadRequiredInt64(stream, "last_recording_frame_id",
                                   &stream_last) &&
                 ReadRequiredInt64(stream, "frame_count", &stream_count) &&
                 ReadRequiredInt64(stream, "recording_frame_id_gaps",
                                   &stream_gaps) &&
                 stream_first == first_id && stream_last == last_id &&
                 stream_count == clip.frame_count && stream_gaps == 0 &&
                 has_video && stream_video == stored_video && has_metadata &&
                 stream_metadata == stored_metadata && has_keyframe &&
                 stream_keyframe == stored_keyframe &&
                 (!require_role ||
                  (ReadRequiredString(stream, "codec", &stream_codec) &&
                   stream_codec == keyframe_codec &&
                   ReadRequiredDouble(stream, "frame_rate", &stream_fps) &&
                   EqualFps(stream_fps, keyframe_fps)));
        };
        if (!matching_stream(artifact, false, true) ||
            camera_output == outputs->end() || !camera_output->is_object() ||
            full == camera_output->end() ||
            !matching_stream(*full, true, false)) {
          AssignError(error_message,
                      "Rolling clip manifest stream disagrees with clip " +
                          clip.clip_id);
          return std::nullopt;
        }

        expected_parent_start = clip.parent_stop_frame;
        if (ordinal + 1 < rows->size() &&
            !CheckedAdd(last_id, 1, &expected_recording_first)) {
          AssignError(error_message,
                      "Rolling acquisition frame range overflows");
          return std::nullopt;
        }
        check_expectations.push_back(RollingCheckExpectation{
            clip.clip_id, clip.camera_serial, first_id, last_id,
            clip.frame_count});
        result.clips_.push_back(std::move(clip));
      }
      if (expected_parent_start != range_frame_count ||
          result.clips_.front().parent_start_frame != 0 ||
          result.clips_.back().parent_stop_frame != manifest_row_count ||
          rows->back().value("last_recording_frame_id", int64_t{-1}) !=
              range_last_id ||
          !ValidateRollingChecks(*manifest, check_expectations,
                                 error_message)) {
        if (error_message && error_message->empty()) {
          AssignError(error_message,
                      "Rolling clip coverage differs from selected-camera range");
        }
        return std::nullopt;
      }
      result.total_frame_count_ = range_frame_count;
      return result;
    }

    if (root->value("status", "") != "ok" ||
        root->value("mode", "") != "materialized_stream_copy" ||
        !ReadRequiredString(*root, "recording_id", &result.recording_id_) ||
        !ReadRequiredString(*root, "camera_serial", &result.camera_serial_)) {
      AssignError(
          error_message,
          "Recording clip index identity or publication state is invalid");
      return std::nullopt;
    }
    const auto clips_found = root->find("clips");
    int64_t declared_clip_count = 0;
    if (clips_found == root->end() || !clips_found->is_array() ||
        clips_found->empty() ||
        !ReadRequiredInt64(*root, "clip_count", &declared_clip_count) ||
        declared_clip_count <= 0 ||
        static_cast<size_t>(declared_clip_count) != clips_found->size() ||
        !ValidateMaterializedChecks(*root, clips_found->size(), error_message)) {
      if (error_message && error_message->empty()) {
        AssignError(error_message,
                    "Recording clip index has an invalid clip inventory");
      }
      return std::nullopt;
    }

    const auto source_found = root->find("source");
    if (source_found == root->end() || !source_found->is_object() ||
        !ReadRequiredInt64(*source_found, "total_frames",
                           &result.total_frame_count_) ||
        result.total_frame_count_ <= 0) {
      AssignError(error_message,
                  "Recording clip index source frame count is invalid");
      return std::nullopt;
    }
    const auto fps_found = source_found->find("fps");
    if (fps_found == source_found->end() || !fps_found->is_number()) {
      AssignError(error_message, "Recording clip index source FPS is missing");
      return std::nullopt;
    }
    result.frames_per_second_ = fps_found->get<double>();
    if (!std::isfinite(result.frames_per_second_) ||
        result.frames_per_second_ <= 0.0) {
      AssignError(error_message, "Recording clip index source FPS is invalid");
      return std::nullopt;
    }

    std::set<std::string> clip_ids;
    int64_t expected_parent_start = 0;
    result.clips_.reserve(clips_found->size());
    for (size_t ordinal = 0; ordinal < clips_found->size(); ++ordinal) {
      const auto &entry = clips_found->at(ordinal);
      RecordingClipDescriptor clip;
      int64_t declared_index = -1;
      bool start_is_keyframe = false;
      bool final_clip = false;
      std::string stored_video;
      std::string status;
      if (!entry.is_object() ||
          !ReadRequiredInt64(entry, "clip_index", &declared_index) ||
          declared_index != static_cast<int64_t>(ordinal) ||
          !ReadRequiredString(entry, "clip_id", &clip.clip_id) ||
          !ReadRequiredString(entry, "recording_id", &clip.recording_id) ||
          !ReadRequiredString(entry, "camera_serial", &clip.camera_serial) ||
          !ReadRequiredString(entry, "video_path", &stored_video) ||
          !ReadRequiredString(entry, "status", &status) ||
          !ReadRequiredBool(entry, "start_is_keyframe", &start_is_keyframe) ||
          !ReadRequiredBool(entry, "final_clip", &final_clip) ||
          !ReadRequiredInt64(entry, "actual_start_frame",
                             &clip.parent_start_frame) ||
          !ReadRequiredInt64(entry, "end_frame_exclusive",
                             &clip.parent_stop_frame) ||
          !ReadRequiredInt64(entry, "first_clip_local_frame_index",
                             &clip.local_start_frame) ||
          !ReadRequiredInt64(entry, "last_clip_local_frame_index",
                             &clip.local_stop_frame) ||
          !ReadRequiredInt64(entry, "frame_count", &clip.frame_count)) {
        AssignError(error_message,
                    "Recording clip declaration is incomplete at ordinal " +
                        std::to_string(ordinal));
        return std::nullopt;
      }
      clip.index = ordinal;
      clip.stored_video_path = stored_video;
      const bool expected_final = ordinal + 1 == clips_found->size();
      if (status != "materialized" || !start_is_keyframe ||
          final_clip != expected_final ||
          clip.recording_id != result.recording_id_ ||
          clip.camera_serial != result.camera_serial_ ||
          !clip_ids.insert(clip.clip_id).second ||
          clip.parent_start_frame != expected_parent_start ||
          clip.parent_stop_frame <= clip.parent_start_frame ||
          clip.frame_count !=
              clip.parent_stop_frame - clip.parent_start_frame ||
          clip.local_start_frame != 0 ||
          clip.local_stop_frame != clip.frame_count - 1 ||
          clip.stored_video_path.empty() ||
          clip.stored_video_path.is_absolute() ||
          ContainsParentTraversal(clip.stored_video_path)) {
        AssignError(
            error_message,
            "Recording clip declaration violates continuity or identity "
            "at ordinal " +
                std::to_string(ordinal));
        return std::nullopt;
      }
      clip.resolved_video_path =
          (result.recording_root_ / clip.stored_video_path).lexically_normal();
      std::error_code status_error;
      if (!std::filesystem::is_regular_file(clip.resolved_video_path,
                                            status_error)) {
        AssignError(error_message, "Recording clip video is not readable: " +
                                       clip.resolved_video_path.string());
        return std::nullopt;
      }
      expected_parent_start = clip.parent_stop_frame;
      result.clips_.push_back(std::move(clip));
    }
    if (expected_parent_start != result.total_frame_count_) {
      AssignError(error_message,
                  "Recording clip coverage differs from source total frames");
      return std::nullopt;
    }
    return result;
  } catch (const json::exception &) {
    AssignError(error_message,
                "Recording clip index contains an invalid JSON value type");
    return std::nullopt;
  } catch (const std::filesystem::filesystem_error &error) {
    AssignError(error_message,
                "Recording clip index path validation failed: " +
                    std::string(error.what()));
    return std::nullopt;
  }
}

const RecordingClipDescriptor *RecordingClipIndex::clip(size_t index) const {
  return index < clips_.size() ? &clips_[index] : nullptr;
}

std::optional<RecordingClipFrameMapping>
RecordingClipIndex::resolveParentFrame(int64_t parent_frame) const {
  if (parent_frame < 0 || parent_frame >= total_frame_count_) {
    return std::nullopt;
  }
  const auto found =
      std::upper_bound(clips_.begin(), clips_.end(), parent_frame,
                       [](int64_t frame, const RecordingClipDescriptor &clip) {
                         return frame < clip.parent_start_frame;
                       });
  if (found == clips_.begin()) {
    return std::nullopt;
  }
  const auto &clip = *std::prev(found);
  if (parent_frame >= clip.parent_stop_frame) {
    return std::nullopt;
  }
  return RecordingClipFrameMapping{&clip, parent_frame,
                                   clip.local_start_frame + parent_frame -
                                       clip.parent_start_frame};
}

} // namespace crimson::media
