#include "review_frame_index.h"

#include "ui_path_config.h"

#include <algorithm>
#include <limits>

namespace {

bool frameHasNonCleanDetections(ZarrDetectionLoader& zarr_loader, int frame_id) {
    if (frame_id < 0) {
        return false;
    }
    if (zarr_loader.getDetectionsForFrame(static_cast<size_t>(frame_id)) <= 0) {
        return false;
    }

    auto detections =
        zarr_loader.getRawDetections(static_cast<size_t>(frame_id), false);
    const size_t detection_count = detections.boxes.size();
    for (size_t det_idx = 0; det_idx < detection_count; ++det_idx) {
        bool is_interp_source = false;
        if (det_idx < detections.detection_source.size()) {
            is_interp_source = detections.detection_source[det_idx] != 0;
        }

        if (det_idx < detections.detection_reason.size()) {
            std::string reason = ToLowerCopy(detections.detection_reason[det_idx]);
            if (!reason.empty() && reason != "clean") {
                return true;
            }
            if (!reason.empty()) {
                if (is_interp_source) {
                    return true;
                }
                continue;
            }
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
                            ZarrDetectionLoader& zarr_loader,
                            const ReviewFrameFilters& filters,
                            ReviewFrameCache& cache) {
    const bool has_filters = filters.include_interpolated ||
                             filters.include_non_clean || filters.include_empty;
    if (!zarr_loaded || !zarr_loader.hasDetectionData() || !has_filters) {
        cache.valid = true;
        cache.archive_path = zarr_loader.getArchivePath();
        cache.dataset = zarr_loader.getActiveDetectionDataset();
        cache.total_frames = zarr_loader.getTotalFrames();
        cache.filters = filters;
        cache.frames.clear();
        return;
    }

    const auto current_dataset = zarr_loader.getActiveDetectionDataset();
    const size_t total_frames = zarr_loader.getTotalFrames();
    const std::string current_archive_path = zarr_loader.getArchivePath();

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
        current_dataset != ZarrDetectionLoader::DetectionDataset::RawDetect;

    for (size_t frame_idx = 0; frame_idx < total_frames; ++frame_idx) {
        bool matches = false;
        const int32_t det_count = zarr_loader.getDetectionsForFrame(frame_idx);

        if (filters.include_empty && det_count <= 0) {
            matches = true;
        }
        if (!matches && filters.include_interpolated &&
            zarr_loader.isFrameInterpolated(frame_idx)) {
            matches = true;
        }
        if (!matches && filters.include_non_clean && non_clean_possible &&
            det_count > 0 &&
            frameHasNonCleanDetections(zarr_loader, static_cast<int>(frame_idx))) {
            matches = true;
        }

        if (matches) {
            cache.frames.push_back(static_cast<int>(frame_idx));
        }
    }
}

ReviewFrameJumpResult computeReviewFrameJump(bool zarr_loaded,
                                             ZarrDetectionLoader& zarr_loader,
                                             const ReviewFrameFilters& filters,
                                             ReviewFrameCache& cache,
                                             int current_frame_num,
                                             bool forward) {
    ensureReviewFrameIndex(zarr_loaded, zarr_loader, filters, cache);

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
