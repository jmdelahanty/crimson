#pragma once

#include "frame_types.h"

#include <limits>
#include <vector>

struct BufferedFrameCandidate {
    int slot_index = -1;
    DecodedFrameMetadata metadata;
};

enum class FrameSelectionFallback {
    ExactOnly,
    LatestAtOrBefore,
    LatestAtOrBeforeThenNearest,
    Nearest,
};

enum class FrameSelectionReason {
    None,
    Preferred,
    Exact,
    BufferedBefore,
    Nearest,
    RetainedPrevious,
};

struct FrameSelectionRequest {
    int target_frame = -1;
    int preferred_slot = -1;
    int minimum_frame_exclusive = std::numeric_limits<int>::min();
    int retained_slot = -1;
    int retained_frame = -1;
    FrameSelectionFallback fallback =
        FrameSelectionFallback::LatestAtOrBeforeThenNearest;
};

struct FrameSelectionResult {
    int slot_index = -1;
    int frame_number = -1;
    FrameSelectionReason reason = FrameSelectionReason::None;

    bool selectedBufferedFrame() const {
        return reason != FrameSelectionReason::None &&
               reason != FrameSelectionReason::RetainedPrevious;
    }
};

FrameSelectionResult
selectBufferedFrame(const std::vector<BufferedFrameCandidate>& candidates,
                    const FrameSelectionRequest& request);
