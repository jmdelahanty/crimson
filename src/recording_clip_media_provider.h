#pragma once

#include "recording_clip_index.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace crimson::media {

struct RecordingClipMediaBinding {
  size_t clip_index = 0;
  std::string clip_id;
  std::string camera_serial;
  std::filesystem::path video_path;
  int64_t parent_frame = -1;
  int64_t clip_local_frame = -1;
  int64_t first_parent_frame = -1;
  int64_t last_parent_frame = -1;
  std::shared_ptr<const std::vector<int64_t>> parent_frame_by_clip_local;
};

// Backend-neutral presentation of a validated recording clip index. Platform
// adapters retain ownership of demuxers, decoder threads, and GPU resources.
class RecordingClipMediaProvider {
public:
  static std::optional<RecordingClipMediaProvider>
  Open(const std::filesystem::path &index_path,
       std::string *error_message = nullptr);

  const RecordingClipIndex &index() const { return index_; }
  std::optional<RecordingClipMediaBinding>
  resolveParentFrame(int64_t parent_frame) const;

private:
  RecordingClipIndex index_;
  std::vector<std::shared_ptr<const std::vector<int64_t>>>
      parent_frames_by_clip_;
};

} // namespace crimson::media
