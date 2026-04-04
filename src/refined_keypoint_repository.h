#ifndef REFINED_KEYPOINT_REPOSITORY_H
#define REFINED_KEYPOINT_REPOSITORY_H

#include <cstddef>
#include <string>
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

// Central seam for refined-keypoint editing. Selection/lookup lands here first,
// and the row-level manual-write methods should be added here next.
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

private:
    const ZarrDetectionLoader& loader_;
};

#endif
