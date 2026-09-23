#include "recording_clip_media_provider.h"

#include <utility>

namespace crimson::media {

std::optional<RecordingClipMediaProvider>
RecordingClipMediaProvider::Open(const std::filesystem::path &index_path,
                                 std::string *error_message) {
  auto index = RecordingClipIndex::Open(index_path, error_message);
  if (!index) {
    return std::nullopt;
  }

  RecordingClipMediaProvider provider;
  provider.index_ = std::move(*index);
  provider.parent_frames_by_clip_.reserve(provider.index_.clips().size());
  for (const auto &clip : provider.index_.clips()) {
    auto frames = std::make_shared<std::vector<int64_t>>();
    frames->reserve(static_cast<size_t>(clip.frame_count));
    for (int64_t local_frame = 0; local_frame < clip.frame_count;
         ++local_frame) {
      frames->push_back(clip.parent_start_frame + local_frame);
    }
    provider.parent_frames_by_clip_.push_back(std::move(frames));
  }
  return provider;
}

std::optional<RecordingClipMediaBinding>
RecordingClipMediaProvider::resolveParentFrame(int64_t parent_frame) const {
  const auto mapping = index_.resolveParentFrame(parent_frame);
  if (!mapping || mapping->clip->index >= parent_frames_by_clip_.size()) {
    return std::nullopt;
  }
  const auto &clip = *mapping->clip;
  return RecordingClipMediaBinding{
      clip.index,
      clip.clip_id,
      clip.camera_serial,
      clip.resolved_video_path,
      mapping->parent_frame,
      mapping->clip_local_frame,
      clip.parent_start_frame,
      clip.parent_stop_frame - 1,
      parent_frames_by_clip_[clip.index],
  };
}

} // namespace crimson::media
