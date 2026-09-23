#include "frame_selection.h"

#include <cstdint>

namespace {

bool isEligible(const BufferedFrameCandidate& candidate,
                const FrameSelectionRequest& request) {
    return candidate.slot_index >= 0 && candidate.metadata.frame_number >= 0 &&
           candidate.metadata.frame_number > request.minimum_frame_exclusive;
}

FrameSelectionResult retainedOrEmpty(const FrameSelectionRequest& request) {
    if (request.retained_frame >= 0) {
        return {request.retained_slot, request.retained_frame,
                FrameSelectionReason::RetainedPrevious};
    }
    return {};
}

}  // namespace

FrameSelectionResult
selectBufferedFrame(const std::vector<BufferedFrameCandidate>& candidates,
                    const FrameSelectionRequest& request) {
    const BufferedFrameCandidate* preferred = nullptr;
    const BufferedFrameCandidate* exact = nullptr;
    const BufferedFrameCandidate* best_before = nullptr;
    const BufferedFrameCandidate* nearest = nullptr;
    int64_t nearest_distance = std::numeric_limits<int64_t>::max();

    for (const BufferedFrameCandidate& candidate : candidates) {
        if (!isEligible(candidate, request)) {
            continue;
        }
        const int frame = candidate.metadata.frame_number;
        if (candidate.slot_index == request.preferred_slot) {
            preferred = &candidate;
        }
        if (frame == request.target_frame && exact == nullptr) {
            exact = &candidate;
        }
        if (frame <= request.target_frame &&
            (best_before == nullptr ||
             frame > best_before->metadata.frame_number)) {
            best_before = &candidate;
        }

        const int64_t distance =
            frame >= request.target_frame
                ? static_cast<int64_t>(frame) - request.target_frame
                : static_cast<int64_t>(request.target_frame) - frame;
        if (nearest == nullptr || distance < nearest_distance ||
            (distance == nearest_distance &&
             frame > nearest->metadata.frame_number)) {
            nearest = &candidate;
            nearest_distance = distance;
        }
    }

    if (preferred != nullptr &&
        (request.target_frame < 0 ||
         preferred->metadata.frame_number == request.target_frame)) {
        return {preferred->slot_index, preferred->metadata.frame_number,
                request.target_frame < 0 ? FrameSelectionReason::Preferred
                                         : FrameSelectionReason::Exact};
    }
    if (exact != nullptr) {
        return {exact->slot_index, exact->metadata.frame_number,
                FrameSelectionReason::Exact};
    }

    switch (request.fallback) {
    case FrameSelectionFallback::ExactOnly:
        break;
    case FrameSelectionFallback::LatestAtOrBefore:
        if (best_before != nullptr) {
            return {best_before->slot_index, best_before->metadata.frame_number,
                    FrameSelectionReason::BufferedBefore};
        }
        break;
    case FrameSelectionFallback::LatestAtOrBeforeThenNearest:
        if (best_before != nullptr) {
            return {best_before->slot_index, best_before->metadata.frame_number,
                    FrameSelectionReason::BufferedBefore};
        }
        if (nearest != nullptr) {
            return {nearest->slot_index, nearest->metadata.frame_number,
                    FrameSelectionReason::Nearest};
        }
        break;
    case FrameSelectionFallback::Nearest:
        if (nearest != nullptr) {
            return {nearest->slot_index, nearest->metadata.frame_number,
                    FrameSelectionReason::Nearest};
        }
        break;
    }

    return retainedOrEmpty(request);
}
