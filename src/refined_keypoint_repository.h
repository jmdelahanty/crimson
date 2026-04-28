#ifndef REFINED_KEYPOINT_REPOSITORY_H
#define REFINED_KEYPOINT_REPOSITORY_H

#include <array>
#include <cstddef>
#include <string>
#include <vector>
#include "zarr_loader.h"

struct RefinedKeypointSelection {
    bool valid = false;
    bool editable = false;
    size_t frame_id = 0;
    size_t detection_index = 0;
    int32_t roi_index = -1;
    ZarrDetectionLoader::KeypointRoiMetadata roi_metadata;
    std::string run_name;
    std::string message;
};

struct RefinedKeypointReviewStatusWriteOptions {
    std::string state = "approved";
    std::string method = "manual";
    std::string intended_use = "training";
    std::string reviewer;
    std::string notes;
    bool update_latest = true;
};

struct RefinedKeypointEditResult {
    bool changed = false;
    bool summary_updated = false;
    int stale_eye_mask_runs = 0;
};

// Central seam for refined-keypoint editing. Selection, review-status writes,
// and row-level manual-write operations land here.
class RefinedKeypointRepository {
public:
    explicit RefinedKeypointRepository(const ZarrDetectionLoader& loader)
        : loader_(loader) {}

    bool canEditActiveRun(std::string* reason = nullptr) const;
    RefinedKeypointSelection resolveFrameDetectionSelection(
        size_t frame_id,
        size_t detection_index,
        bool use_interpolated = false) const;
    bool writeReviewStatus(
        const RefinedKeypointReviewStatusWriteOptions& options,
        std::string& error_message,
        std::string* resolved_run_name = nullptr) const;
    bool writeManualCorrection(
        const RefinedKeypointSelection& selection,
        const std::vector<std::array<double, 2>>& keypoints_roi,
        std::string& error_message,
        RefinedKeypointEditResult* edit_result = nullptr) const;
    bool markFishPresentNoKeypoints(
        const RefinedKeypointSelection& selection,
        std::string& error_message,
        RefinedKeypointEditResult* edit_result = nullptr) const;
    bool markDetectionIssue(
        const RefinedKeypointSelection& selection,
        std::string& error_message,
        RefinedKeypointEditResult* edit_result = nullptr) const;

private:
    const ZarrDetectionLoader& loader_;
};

#endif
