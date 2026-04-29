#pragma once

#include "subject_mask_writeback_client.h"
#include "zarr_loader.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SubjectMaskEditTarget {
    std::string zarr_path;
    std::string refined_run;
    int32_t roi_index = -1;
    std::string component_name;
    size_t channel_index = 0;
    size_t rows = 0;
    size_t cols = 0;
};

class SubjectMaskEditSession {
public:
    bool start(const SubjectMaskEditTarget& target,
               const std::vector<uint8_t>& original_mask,
               std::string* error_message = nullptr);
    bool startFromLoadedRow(const ZarrDetectionLoader& loader,
                            size_t roi_index,
                            const std::string& component_name,
                            std::string* error_message = nullptr);
    void clear();
    bool resetPreview();
    bool replacePreviewMask(const std::vector<uint8_t>& mask,
                            std::string* error_message = nullptr);
    bool setPixel(size_t row, size_t col, uint8_t value);

    bool active() const { return active_; }
    bool dirty() const { return dirty_; }
    const SubjectMaskEditTarget& target() const { return target_; }
    const std::vector<uint8_t>& originalMask() const { return original_mask_; }
    const std::vector<uint8_t>& previewMask() const { return preview_mask_; }

    SubjectMaskWritebackRequest buildWritebackRequest(
        std::string reason = "crimson_refined_subject_mask_edit") const;

private:
    bool active_ = false;
    bool dirty_ = false;
    SubjectMaskEditTarget target_;
    std::vector<uint8_t> original_mask_;
    std::vector<uint8_t> preview_mask_;
};
