#pragma once

#include "review_frame_state.h"

#include <optional>
#include <string>

struct ReviewFrameJumpResult {
    std::optional<int> target_frame;
    std::string status;
};

void invalidateReviewFrameCache(ReviewFrameCache& cache);

void ensureReviewFrameIndex(bool zarr_loaded,
                            crimson::zarr::DetectionRepository& repository,
                            const ReviewFrameFilters& filters,
                            ReviewFrameCache& cache);

ReviewFrameJumpResult computeReviewFrameJump(bool zarr_loaded,
                                             crimson::zarr::DetectionRepository& repository,
                                             const ReviewFrameFilters& filters,
                                             ReviewFrameCache& cache,
                                             int current_frame_num,
                                             bool forward);
