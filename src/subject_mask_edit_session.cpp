#include "subject_mask_edit_session.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace {

std::vector<uint8_t> binarize(const std::vector<uint8_t>& mask) {
    std::vector<uint8_t> out(mask.size(), 0);
    std::transform(mask.begin(), mask.end(), out.begin(),
                   [](uint8_t value) -> uint8_t {
                       return value != 0 ? 1 : 0;
                   });
    return out;
}

bool validateTargetAndMask(const SubjectMaskEditTarget& target,
                           const std::vector<uint8_t>& mask,
                           std::string* error_message) {
    if (target.zarr_path.empty()) {
        if (error_message != nullptr) {
            *error_message = "No Zarr archive path available.";
        }
        return false;
    }
    if (target.refined_run.empty()) {
        if (error_message != nullptr) {
            *error_message = "No refined subject-mask run available.";
        }
        return false;
    }
    if (target.roi_index < 0) {
        if (error_message != nullptr) {
            *error_message = "No valid ROI row selected.";
        }
        return false;
    }
    if (target.component_name.empty()) {
        if (error_message != nullptr) {
            *error_message = "No subject-mask component selected.";
        }
        return false;
    }
    if (target.rows == 0 || target.cols == 0) {
        if (error_message != nullptr) {
            *error_message = "Selected subject-mask component has empty dimensions.";
        }
        return false;
    }
    const size_t expected_size = target.rows * target.cols;
    if (mask.size() != expected_size) {
        if (error_message != nullptr) {
            std::ostringstream oss;
            oss << "Selected subject-mask payload size mismatch: expected "
                << expected_size << " bytes, got " << mask.size() << ".";
            *error_message = oss.str();
        }
        return false;
    }
    return true;
}

}  // namespace

bool SubjectMaskEditSession::start(
    const SubjectMaskEditTarget& target,
    const std::vector<uint8_t>& original_mask,
    std::string* error_message) {
    if (!validateTargetAndMask(target, original_mask, error_message)) {
        return false;
    }

    target_ = target;
    original_mask_ = binarize(original_mask);
    preview_mask_ = original_mask_;
    dirty_ = false;
    active_ = true;
    return true;
}

bool SubjectMaskEditSession::startFromLoadedRow(
    const ZarrDetectionLoader& loader,
    size_t roi_index,
    const std::string& component_name,
    std::string* error_message) {
    ZarrDetectionLoader::RefinedSubjectMaskComponentRow row;
    if (!loader.readRefinedSubjectMaskComponentRow(
            roi_index, component_name, row, error_message)) {
        return false;
    }

    SubjectMaskEditTarget target;
    target.zarr_path = loader.getArchivePath();
    target.refined_run = row.run_name;
    target.roi_index = static_cast<int32_t>(row.roi_index);
    target.component_name = row.component_name;
    target.channel_index = row.channel_index;
    target.rows = row.rows;
    target.cols = row.cols;
    return start(target, row.mask, error_message);
}

void SubjectMaskEditSession::clear() {
    active_ = false;
    dirty_ = false;
    target_ = SubjectMaskEditTarget{};
    original_mask_.clear();
    preview_mask_.clear();
}

bool SubjectMaskEditSession::resetPreview() {
    if (!active_) {
        return false;
    }
    preview_mask_ = original_mask_;
    dirty_ = false;
    return true;
}

bool SubjectMaskEditSession::replacePreviewMask(
    const std::vector<uint8_t>& mask,
    std::string* error_message) {
    if (!active_) {
        if (error_message != nullptr) {
            *error_message = "No active subject-mask edit session.";
        }
        return false;
    }
    if (!validateTargetAndMask(target_, mask, error_message)) {
        return false;
    }
    preview_mask_ = binarize(mask);
    dirty_ = preview_mask_ != original_mask_;
    return true;
}

bool SubjectMaskEditSession::setPixel(size_t row, size_t col, uint8_t value) {
    if (!active_ || row >= target_.rows || col >= target_.cols) {
        return false;
    }
    const size_t index = row * target_.cols + col;
    preview_mask_[index] = value != 0 ? 1 : 0;
    dirty_ = preview_mask_ != original_mask_;
    return true;
}

SubjectMaskWritebackRequest SubjectMaskEditSession::buildWritebackRequest(
    std::string reason) const {
    SubjectMaskWritebackRequest request;
    if (!active_) {
        return request;
    }
    request.zarr_path = target_.zarr_path;
    request.refined_run = target_.refined_run;
    request.component_name = target_.component_name;
    request.roi_index = target_.roi_index;
    request.rows = target_.rows;
    request.cols = target_.cols;
    request.binary_mask = preview_mask_;
    request.reason = std::move(reason);
    request.validate = true;
    return request;
}
