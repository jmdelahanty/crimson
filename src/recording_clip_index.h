#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace crimson::media {

struct RecordingClipDescriptor {
  size_t index = 0;
  std::string clip_id;
  std::string recording_id;
  std::string camera_serial;
  std::filesystem::path stored_video_path;
  std::filesystem::path resolved_video_path;
  int64_t parent_start_frame = 0;
  int64_t parent_stop_frame = 0;
  int64_t local_start_frame = 0;
  int64_t local_stop_frame = 0;
  int64_t frame_count = 0;
};

struct RecordingClipFrameMapping {
  const RecordingClipDescriptor *clip = nullptr;
  int64_t parent_frame = -1;
  int64_t clip_local_frame = -1;

  explicit operator bool() const { return clip != nullptr; }
};

// Strict compatibility adapter for Palette's currently unversioned
// recording_clip_index.json. Platform decoders consume only the validated
// descriptors and frame mappings exposed here.
class RecordingClipIndex {
public:
  static std::optional<RecordingClipIndex>
  Open(const std::filesystem::path &index_path,
       std::string *error_message = nullptr);

  const std::filesystem::path &indexPath() const { return index_path_; }
  const std::filesystem::path &recordingRoot() const { return recording_root_; }
  const std::string &recordingId() const { return recording_id_; }
  const std::string &cameraSerial() const { return camera_serial_; }
  int64_t totalFrameCount() const { return total_frame_count_; }
  double framesPerSecond() const { return frames_per_second_; }
  const std::vector<RecordingClipDescriptor> &clips() const { return clips_; }

  const RecordingClipDescriptor *clip(size_t index) const;
  std::optional<RecordingClipFrameMapping>
  resolveParentFrame(int64_t parent_frame) const;

private:
  std::filesystem::path index_path_;
  std::filesystem::path recording_root_;
  std::string recording_id_;
  std::string camera_serial_;
  int64_t total_frame_count_ = 0;
  double frames_per_second_ = 0.0;
  std::vector<RecordingClipDescriptor> clips_;
};

} // namespace crimson::media
