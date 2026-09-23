#pragma once

#include "frame_types.h"
#include "playback_presentation_lifecycle.h"

#include <cstddef>
#include <vector>

namespace crimson::platform::nvidia {

std::vector<playback::PlaybackPresentationCandidate>
snapshotFrameBuffer(const PictureBuffer *slots, size_t slot_count);

} // namespace crimson::platform::nvidia
