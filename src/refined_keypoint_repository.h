#ifndef REFINED_KEYPOINT_REPOSITORY_H
#define REFINED_KEYPOINT_REPOSITORY_H

#include "zarr/review_write_repository.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include "zarr_loader.h"

struct RefinedKeypointReviewStatusWriteOptions {
    std::string state = "approved";
    std::string method = "manual";
    std::string intended_use = "training";
    std::string reviewer;
    std::string notes;
    bool update_latest = true;
};

// Central seam for refined-keypoint editing. Selection, review-status writes,
// and row-level manual-write operations land here.
class RefinedKeypointRepository : public crimson::zarr::ReviewWriteRepository {
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
    crimson::zarr::ReviewWriteResult write(
        const crimson::zarr::ReviewWriteOperation& operation) override;

private:
    const ZarrDetectionLoader& loader_;
};

namespace crimson::zarr {

std::unique_ptr<ReviewWriteRepository> OpenLegacyReviewWriteRepository(
    const std::string& archive_path,
    std::string& error);

}  // namespace crimson::zarr

#endif
