#include "recording_clip_index.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
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

bool ValidateChecks(const json &root, size_t clip_count,
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
        !ValidateChecks(*root, clips_found->size(), error_message)) {
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
