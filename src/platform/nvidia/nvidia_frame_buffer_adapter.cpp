#include "platform/nvidia/nvidia_frame_buffer_adapter.h"

#include "frame_slot.h"

namespace crimson::platform::nvidia {

std::vector<playback::PlaybackPresentationCandidate>
snapshotFrameBuffer(const PictureBuffer *slots, size_t slot_count) {
  std::vector<playback::PlaybackPresentationCandidate> candidates;
  if (slots == nullptr) {
    return candidates;
  }
  candidates.reserve(slot_count);
  for (size_t slot_index = 0; slot_index < slot_count; ++slot_index) {
    if (const auto metadata = frameSlotSnapshotReadable(slots[slot_index])) {
      candidates.push_back(
          {metadata->frame_number, static_cast<int>(slot_index)});
    }
  }
  return candidates;
}

} // namespace crimson::platform::nvidia
