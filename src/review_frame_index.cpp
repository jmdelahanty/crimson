#include "review_frame_index.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace {

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool frameHasNonCleanDetections(
    crimson::zarr::DetectionRepository& repository,
    int frame_id) {
    if (frame_id < 0) {
        return false;
    }
    if (repository.observationCount(static_cast<size_t>(frame_id)) == 0) {
        return false;
    }

    const auto frame =
        repository.resolveFrame(static_cast<size_t>(frame_id), false);
    if (!frame.ready()) {
        return false;
    }
    for (const auto& observation : frame.observations) {
        const bool is_interp_source = observation.source_kind != 0;
        if (!observation.reason.empty()) {
            const std::string reason = lowerAscii(observation.reason);
            if (reason != "clean") {
                return true;
            }
            if (is_interp_source) {
                return true;
            }
            continue;
        }
        if (is_interp_source) {
            return true;
        }
    }
    return false;
}

}  // namespace

void invalidateReviewFrameCache(ReviewFrameCache& cache) {
    cache.valid = false;
    cache.frames.clear();
}

void ensureReviewFrameIndex(bool zarr_loaded,
                            crimson::zarr::DetectionRepository& repository,
                            const ReviewFrameFilters& filters,
                            ReviewFrameCache& cache) {
    const auto descriptor = repository.descriptor();
    const bool has_filters = filters.include_interpolated ||
                             filters.include_non_clean || filters.include_empty;
    if (!zarr_loaded || !descriptor.available || !has_filters) {
        cache.valid = true;
        cache.archive_path = descriptor.archive_path;
        cache.dataset = descriptor.active_dataset;
        cache.total_frames = descriptor.total_frames;
        cache.filters = filters;
        cache.frames.clear();
        return;
    }

    const auto current_dataset = descriptor.active_dataset;
    const size_t total_frames = descriptor.total_frames;
    const std::string& current_archive_path = descriptor.archive_path;

    if (cache.valid && cache.archive_path == current_archive_path &&
        cache.dataset == current_dataset && cache.total_frames == total_frames &&
        cache.filters.include_interpolated == filters.include_interpolated &&
        cache.filters.include_non_clean == filters.include_non_clean &&
        cache.filters.include_empty == filters.include_empty) {
        return;
    }

    cache.valid = true;
    cache.archive_path = current_archive_path;
    cache.dataset = current_dataset;
    cache.total_frames = total_frames;
    cache.filters = filters;
    cache.frames.clear();
    const bool non_clean_possible =
        current_dataset != crimson::zarr::DetectionDataset::RawDetect;

    for (size_t frame_idx = 0; frame_idx < total_frames; ++frame_idx) {
        bool matches = false;
        const size_t det_count = repository.observationCount(frame_idx);

        if (filters.include_empty && det_count == 0) {
            matches = true;
        }
        if (!matches && filters.include_interpolated &&
            repository.isFrameInterpolated(frame_idx)) {
            matches = true;
        }
        if (!matches && filters.include_non_clean && non_clean_possible &&
            det_count > 0 && frameHasNonCleanDetections(
                                 repository, static_cast<int>(frame_idx))) {
            matches = true;
        }

        if (matches) {
            cache.frames.push_back(static_cast<int>(frame_idx));
        }
    }
}

ReviewFrameJumpResult computeReviewFrameJump(bool zarr_loaded,
                                             crimson::zarr::DetectionRepository& repository,
                                             const ReviewFrameFilters& filters,
                                             ReviewFrameCache& cache,
                                             int current_frame_num,
                                             bool forward) {
    ensureReviewFrameIndex(zarr_loaded, repository, filters, cache);

    ReviewFrameJumpResult result;
    if (cache.frames.empty()) {
        result.status = "No frames match the selected review filters.";
        return result;
    }

    const auto& review_frames = cache.frames;
    int target_frame = current_frame_num;
    if (forward) {
        auto it = std::upper_bound(review_frames.begin(), review_frames.end(),
                                   current_frame_num);
        if (it == review_frames.end()) {
            it = review_frames.begin();
        }
        target_frame = *it;
    } else {
        auto it = std::lower_bound(review_frames.begin(), review_frames.end(),
                                   current_frame_num);
        if (it == review_frames.begin()) {
            target_frame = review_frames.back();
        } else {
            --it;
            target_frame = *it;
        }
    }

    result.target_frame = target_frame;
    return result;
}
